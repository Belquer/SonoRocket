// SonoRocket talkback. See Talkback.h.

#include "Talkback.h"
#include <map>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <speex/speex_resampler.h>

namespace SonoRocket {

namespace {
    // Incoming voice per listener, at 16 kHz. Start once a little is buffered,
    // and never let it grow: late voice is dropped, not delayed.
    constexpr int kStartSamples = 640;     // 40 ms
    constexpr int kMaxSamples   = 3200;    // 200 ms → trimmed back to kTrimTo
    constexpr int kTrimTo       = 800;     // 50 ms
    constexpr double kPlayAheadSeconds = 0.02;

    inline int16_t toS16 (float v) noexcept
    {
        v *= 32767.0f;
        return (int16_t) juce::jlimit (-32768.0f, 32767.0f, v);
    }

    struct Source { std::vector<int16_t> q; bool playing = false; juce::uint32 lastMs = 0; };
    std::map<uint16_t, Source>& sources() { static std::map<uint16_t, Source> s; return s; }   // worker thread only
}

Talkback::Talkback (RocketStreamer& s) : juce::Thread ("SonoRocket talkback"), streamer (s)
{
    startThread (juce::Thread::Priority::high);
}

Talkback::~Talkback()
{
    signalThreadShouldExit();
    notify();
    stopThread (3000);
    const juce::ScopedLock sl (dspLock);
    destroyDsp();
}

void Talkback::destroyDsp()
{
    prepared = false;
    if (echo) { speex_echo_state_destroy (echo); echo = nullptr; }
    if (pre)  { speex_preprocess_state_destroy (pre); pre = nullptr; }
    if (down) { speex_resampler_destroy (down); down = nullptr; }
    if (up)   { speex_resampler_destroy (up); up = nullptr; }
}

void Talkback::prepare (double sr)
{
    const juce::ScopedLock sl (dspLock);
    destroyDsp();
    deviceRate = sr > 0 ? sr : 48000.0;

    fifoFrames = (int) deviceRate;   // 1 s each way: far more than the worker ever lags
    capData.allocate ((size_t) fifoFrames * kCapCh, true);
    capFifo = std::make_unique<juce::AbstractFifo> (fifoFrames);
    playData.allocate ((size_t) fifoFrames, true);
    playFifo = std::make_unique<juce::AbstractFifo> (fifoFrames);

    echo = speex_echo_state_init_mc (kFrame, kRate * kTailMs / 1000, 1, 3);   // 1 mic, 3 references
    int rate = kRate;
    speex_echo_ctl (echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    pre = speex_preprocess_state_init (kFrame, kRate);
    speex_preprocess_ctl (pre, SPEEX_PREPROCESS_SET_ECHO_STATE, echo);
    int on = 1, off = 0, suppress = -45, suppressActive = -20, noise = -25;
    speex_preprocess_ctl (pre, SPEEX_PREPROCESS_SET_DENOISE, &on);
    speex_preprocess_ctl (pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise);
    speex_preprocess_ctl (pre, SPEEX_PREPROCESS_SET_AGC, &off);
    speex_preprocess_ctl (pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &suppress);
    speex_preprocess_ctl (pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &suppressActive);

    int err = 0;
    down = speex_resampler_init (kCapCh, (spx_uint32_t) deviceRate, kRate, 5, &err);
    up   = speex_resampler_init (1, kRate, (spx_uint32_t) deviceRate, 5, &err);

    pend16.clear(); jitter.clear(); sources().clear();
    fade = 0.0f;
    playStarved = true;
    prepared = (echo && pre && down && up);
}

void Talkback::process (const float* mic, const float* refL, const float* refR,
                        float* outVoice, int n) noexcept
{
    if (n <= 0) return;
    if (! prepared.load()) { if (outVoice) juce::FloatVectorOperations::clear (outVoice, n); return; }

    // 1) the listeners' voice, from the worker
    float tmp[2048];
    float* voice = outVoice != nullptr ? outVoice : (n <= 2048 ? tmp : nullptr);
    if (voice != nullptr) {
        juce::FloatVectorOperations::clear (voice, n);
        const int take = juce::jmin (n, playFifo->getNumReady());
        int s1, n1, s2, n2;
        playFifo->prepareToRead (take, s1, n1, s2, n2);
        if (n1 > 0) std::memcpy (voice, playData.get() + s1, sizeof (float) * (size_t) n1);
        if (n2 > 0) std::memcpy (voice + n1, playData.get() + s2, sizeof (float) * (size_t) n2);
        playFifo->finishedRead (n1 + n2);
        if (take < n) playStarved = true;
        juce::FloatVectorOperations::multiply (voice, playbackGain.load(), n);
        auto r = juce::FloatVectorOperations::findMinAndMax (voice, n);
        const float pk = juce::jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
        float prev = voicePeak.load();
        while (pk > prev && ! voicePeak.compare_exchange_weak (prev, pk)) {}
    }

    // 2) mic + references, sample-aligned, to the worker
    if (mic == nullptr || refL == nullptr) return;
    if (refR == nullptr) refR = refL;
    if (capFifo->getFreeSpace() < n) return;   // worker stuck: skip, never block
    const float g = micGain.load();
    int s1, n1, s2, n2;
    capFifo->prepareToWrite (n, s1, n1, s2, n2);
    int i = 0;
    auto put = [&] (int start, int count) {
        float* d = capData.get() + (size_t) start * kCapCh;
        for (int k = 0; k < count; ++k, ++i) {
            *d++ = mic[i] * g;
            *d++ = refL[i];
            *d++ = refR[i];
            *d++ = voice != nullptr ? voice[i] : 0.0f;
        }
    };
    put (s1, n1);
    put (s2, n2);
    capFifo->finishedWrite (n1 + n2);
}

void Talkback::run()
{
    while (! threadShouldExit()) {
        wait (4);
        const juce::ScopedLock sl (dspLock);
        if (! prepared.load()) continue;
        processCaptured();
        feedPlayback();
    }
}

void Talkback::processCaptured()
{
    const int ready = capFifo->getNumReady();
    if (ready <= 0) return;

    capChunk.resize ((size_t) ready * kCapCh);
    int s1, n1, s2, n2;
    capFifo->prepareToRead (ready, s1, n1, s2, n2);
    std::memcpy (capChunk.data(), capData.get() + (size_t) s1 * kCapCh, sizeof (float) * (size_t) n1 * kCapCh);
    if (n2 > 0) std::memcpy (capChunk.data() + (size_t) n1 * kCapCh, capData.get() + (size_t) s2 * kCapCh, sizeof (float) * (size_t) n2 * kCapCh);
    capFifo->finishedRead (n1 + n2);

    spx_uint32_t inLen = (spx_uint32_t) ready;
    spx_uint32_t outLen = (spx_uint32_t) (ready * (double) kRate / deviceRate) + 32;
    down16.resize ((size_t) outLen * kCapCh);
    speex_resampler_process_interleaved_float (down, capChunk.data(), &inLen, down16.data(), &outLen);
    pend16.insert (pend16.end(), down16.begin(), down16.begin() + (std::ptrdiff_t) (outLen * kCapCh));

    int16_t rec[kFrame], play[kFrame * 3], out[kFrame];
    size_t pos = 0;
    const size_t frameFloats = (size_t) kFrame * kCapCh;
    while (pend16.size() - pos >= frameFloats) {
        const float* f = pend16.data() + pos;
        for (int k = 0; k < kFrame; ++k) {
            rec[k]          = toS16 (f[k * kCapCh + 0]);
            play[k * 3 + 0] = toS16 (f[k * kCapCh + 1]);
            play[k * 3 + 1] = toS16 (f[k * kCapCh + 2]);
            play[k * 3 + 2] = toS16 (f[k * kCapCh + 3]);
        }
        pos += frameFloats;

        // The canceller runs all the time so it stays converged, gate open or not.
        speex_echo_cancellation (echo, rec, play, out);
        speex_preprocess_run (pre, out);

        float pk = 0.0f;
        for (int k = 0; k < kFrame; ++k) pk = juce::jmax (pk, std::abs (out[k] / 32768.0f));
        float prev = micPeak.load();
        while (pk > prev && ! micPeak.compare_exchange_weak (prev, pk)) {}

        // gate with a 10 ms ramp
        const bool open = micOpen.load();
        sending = open;
        if (! open && fade <= 0.0f) continue;
        const float target = open ? 1.0f : 0.0f, step = 1.0f / kFrame;
        for (int k = 0; k < kFrame; ++k) {
            fade = target > fade ? juce::jmin (target, fade + step) : juce::jmax (target, fade - step);
            out[k] = (int16_t) (out[k] * fade);
        }
        streamer.pushVoice (out, kFrame);
    }
    pend16.erase (pend16.begin(), pend16.begin() + (std::ptrdiff_t) pos);
}

void Talkback::feedPlayback()
{
    auto& srcs = sources();
    const auto now = juce::Time::getMillisecondCounter();

    std::vector<int16_t> pcm; uint16_t id = 0;
    while (streamer.popIncomingVoice (pcm, id)) {
        auto& s = srcs[id];
        s.q.insert (s.q.end(), pcm.begin(), pcm.end());
        s.lastMs = now;
        if ((int) s.q.size() > kMaxSamples)   // fell behind: catch up to real time
            s.q.erase (s.q.begin(), s.q.end() - kTrimTo);
        if (! s.playing && (int) s.q.size() >= kStartSamples) s.playing = true;
    }
    for (auto it = srcs.begin(); it != srcs.end();)   // listeners that went quiet
        it = (now - it->second.lastMs > 5000 && it->second.q.empty()) ? srcs.erase (it) : std::next (it);

    const int ahead = (int) (deviceRate * kPlayAheadSeconds);
    float mix[kFrame];
    while (playFifo->getNumReady() < ahead) {
        bool any = false;
        std::fill (mix, mix + kFrame, 0.0f);
        for (auto& [sid, s] : srcs) {
            if (! s.playing) continue;
            if ((int) s.q.size() < kFrame) { s.playing = false; continue; }   // underrun: rebuffer
            for (int k = 0; k < kFrame; ++k) mix[k] += s.q[(size_t) k] / 32768.0f;
            s.q.erase (s.q.begin(), s.q.begin() + kFrame);
            any = true;
        }
        if (! any) break;

        spx_uint32_t inLen = kFrame;
        spx_uint32_t outLen = (spx_uint32_t) (kFrame * deviceRate / kRate) + 32;
        voiceUp.resize (outLen);
        speex_resampler_process_float (up, 0, mix, &inLen, voiceUp.data(), &outLen);

        const int w = juce::jmin ((int) outLen, playFifo->getFreeSpace());
        int s1, n1, s2, n2;
        playFifo->prepareToWrite (w, s1, n1, s2, n2);
        if (n1 > 0) std::memcpy (playData.get() + s1, voiceUp.data(), sizeof (float) * (size_t) n1);
        if (n2 > 0) std::memcpy (playData.get() + s2, voiceUp.data() + n1, sizeof (float) * (size_t) n2);
        playFifo->finishedWrite (n1 + n2);
        playStarved = false;
    }
}

} // namespace SonoRocket

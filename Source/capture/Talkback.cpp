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

// ---------------------------------------------------------------- Lane

void Talkback::Lane::prepare (int channels, double sr)
{
    release();
    ch = channels; rate = sr > 0 ? sr : 48000.0;
    const int frames = (int) rate;   // 1 s: far more than the worker ever lags
    data.allocate ((size_t) frames * (size_t) ch, true);
    fifo = std::make_unique<juce::AbstractFifo> (frames);
    int err = 0;
    rs = speex_resampler_init ((spx_uint32_t) ch, (spx_uint32_t) rate, kRate, 5, &err);
    pending.clear();
    ready = (rs != nullptr);
}

void Talkback::Lane::release()
{
    ready = false;
    if (rs) { speex_resampler_destroy (rs); rs = nullptr; }
    pending.clear();
}

void Talkback::Lane::push (const float* const* chans, int n, float gain) noexcept
{
    if (! ready.load() || n <= 0 || fifo->getFreeSpace() < n) return;   // worker stuck: skip, never block
    int s1, n1, s2, n2;
    fifo->prepareToWrite (n, s1, n1, s2, n2);
    int i = 0;
    auto put = [&] (int start, int count) {
        float* d = data.get() + (size_t) start * (size_t) ch;
        for (int k = 0; k < count; ++k, ++i)
            for (int c = 0; c < ch; ++c) *d++ = chans[c][i] * gain;
    };
    put (s1, n1);
    put (s2, n2);
    fifo->finishedWrite (n1 + n2);
}

void Talkback::Lane::pull()
{
    if (! ready.load()) return;
    const int n = fifo->getNumReady();
    if (n <= 0) return;
    chunk.resize ((size_t) n * (size_t) ch);
    int s1, n1, s2, n2;
    fifo->prepareToRead (n, s1, n1, s2, n2);
    std::memcpy (chunk.data(), data.get() + (size_t) s1 * (size_t) ch, sizeof (float) * (size_t) n1 * (size_t) ch);
    if (n2 > 0) std::memcpy (chunk.data() + (size_t) n1 * (size_t) ch, data.get() + (size_t) s2 * (size_t) ch, sizeof (float) * (size_t) n2 * (size_t) ch);
    fifo->finishedRead (n1 + n2);
    spx_uint32_t inLen = (spx_uint32_t) n;
    spx_uint32_t outLen = (spx_uint32_t) (n * (double) kRate / rate) + 32;
    out.resize ((size_t) outLen * (size_t) ch);
    speex_resampler_process_interleaved_float (rs, chunk.data(), &inLen, out.data(), &outLen);
    pending.insert (pending.end(), out.begin(), out.begin() + (std::ptrdiff_t) (outLen * (spx_uint32_t) ch));
}

void Talkback::Lane::drop16 (int frames)
{
    const size_t k = juce::jmin (pending.size(), (size_t) frames * (size_t) ch);
    pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) k);
}

// ---------------------------------------------------------------- Talkback

Talkback::Talkback (RocketStreamer& s) : juce::Thread ("SonoRocket talkback"), streamer (s)
{
    echo = speex_echo_state_init_mc (kFrame, kRate * kTailMs / 1000, 1, 2);   // 1 mic, master L/R as references
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
    startThread (juce::Thread::Priority::high);
}

Talkback::~Talkback()
{
    signalThreadShouldExit();
    notify();
    stopThread (3000);
    const juce::ScopedLock sl (dspLock);
    ref.release(); mic.release();
    outReady = false;
    if (up)   speex_resampler_destroy (up);
    if (pre)  speex_preprocess_state_destroy (pre);
    if (echo) speex_echo_state_destroy (echo);
}

void Talkback::prepareReference (double sr)
{
    const juce::ScopedLock sl (dspLock);
    ref.prepare (2, sr);
    mic.pending.clear();
    speex_echo_state_reset (echo);
}

void Talkback::prepareMic (double sr)
{
    const juce::ScopedLock sl (dspLock);
    mic.prepare (1, sr);
    ref.pending.clear();
    speex_echo_state_reset (echo);
    fade = 0.0f; duck = 1.0f;
}

void Talkback::stopMic()
{
    const juce::ScopedLock sl (dspLock);
    mic.release();
    sending = false;
}

void Talkback::prepareOutput (double sr)
{
    const juce::ScopedLock sl (dspLock);
    outReady = false;
    if (up) { speex_resampler_destroy (up); up = nullptr; }
    outRate = sr > 0 ? sr : 48000.0;
    const int frames = (int) outRate;
    playData.allocate ((size_t) frames, true);
    playFifo = std::make_unique<juce::AbstractFifo> (frames);
    int err = 0;
    up = speex_resampler_init (1, kRate, (spx_uint32_t) outRate, 5, &err);
    sources().clear();
    outReady = (up != nullptr);
}

void Talkback::captureReference (const float* refL, const float* refR, int n) noexcept
{
    if (refL == nullptr) return;
    const float* chans[2] = { refL, refR != nullptr ? refR : refL };
    ref.push (chans, n, 1.0f);
}

void Talkback::captureMic (const float* m, int n) noexcept
{
    if (m == nullptr) return;
    const float* chans[1] = { m };
    mic.push (chans, n, micGain.load());
}

void Talkback::render (float* out, int n) noexcept
{
    if (out == nullptr || n <= 0) return;
    juce::FloatVectorOperations::clear (out, n);
    if (! outReady.load()) return;
    const int take = juce::jmin (n, playFifo->getNumReady());
    int s1, n1, s2, n2;
    playFifo->prepareToRead (take, s1, n1, s2, n2);
    if (n1 > 0) std::memcpy (out, playData.get() + s1, sizeof (float) * (size_t) n1);
    if (n2 > 0) std::memcpy (out + n1, playData.get() + s2, sizeof (float) * (size_t) n2);
    playFifo->finishedRead (n1 + n2);
    juce::FloatVectorOperations::multiply (out, playbackGain.load(), n);
    auto r = juce::FloatVectorOperations::findMinAndMax (out, n);
    const float pk = juce::jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
    if (pk > 0.003f) lastVoiceMs = juce::Time::getMillisecondCounter();
    float prev = voicePeak.load();
    while (pk > prev && ! voicePeak.compare_exchange_weak (prev, pk)) {}
}

void Talkback::run()
{
    while (! threadShouldExit()) {
        wait (4);
        const juce::ScopedLock sl (dspLock);
        processCaptured();
        if (outReady.load()) feedPlayback();
        else { std::vector<int16_t> pcm; uint16_t id; while (streamer.popIncomingVoice (pcm, id)) {} }   // nowhere to play it
    }
}

void Talkback::processCaptured()
{
    ref.pull();
    mic.pull();
    if (! mic.ready.load()) { ref.pending.clear(); sending = false; return; }

    // Keep the reference and the mic within 100 ms of each other. Same device: they
    // never drift. Two devices: start-up offset and clock drift get trimmed here.
    const bool haveRef = ref.ready.load();
    if (haveRef) {
        const int d = ref.frames16() - mic.frames16();
        if (d > 1600) ref.drop16 (d);
        else if (d < -1600) mic.drop16 (-d);
    } else ref.pending.clear();

    int16_t rec[kFrame], play[kFrame * 2], out[kFrame];
    while (mic.frames16() >= kFrame && (! haveRef || ref.frames16() >= kFrame)) {
        const float* m = mic.pending.data();
        const float* f = haveRef ? ref.pending.data() : nullptr;
        for (int k = 0; k < kFrame; ++k) {
            rec[k]          = toS16 (m[k]);
            play[k * 2 + 0] = f ? toS16 (f[k * 2 + 0]) : (int16_t) 0;
            play[k * 2 + 1] = f ? toS16 (f[k * 2 + 1]) : (int16_t) 0;
        }
        mic.drop16 (kFrame);
        if (haveRef) ref.drop16 (kFrame);

        // The canceller runs all the time so it stays converged, gate open or not.
        speex_echo_cancellation (echo, rec, play, out);
        speex_preprocess_run (pre, out);

        float pk = 0.0f;
        for (int k = 0; k < kFrame; ++k) pk = juce::jmax (pk, std::abs (out[k] / 32768.0f));
        float prev = micPeak.load();
        while (pk > prev && ! micPeak.compare_exchange_weak (prev, pk)) {}

        // gate with a 10 ms ramp; a hands-free mic (not push-to-talk) is ducked
        // while the listener's voice plays, so he never hears himself come back
        const bool open = micOpen.load();
        sending = open;
        if (! open && fade <= 0.0f) continue;
        const bool listenerTalking = juce::Time::getMillisecondCounter() - lastVoiceMs.load() < 250;
        const float duckTarget = (listenerTalking && ! ptt.load()) ? 0.1f : 1.0f;
        const float target = open ? 1.0f : 0.0f, step = 1.0f / kFrame;
        for (int k = 0; k < kFrame; ++k) {
            fade = target > fade ? juce::jmin (target, fade + step) : juce::jmax (target, fade - step);
            duck += (duckTarget - duck) * 0.01f;
            out[k] = (int16_t) (out[k] * fade * duck);
        }
        streamer.pushVoice (out, kFrame);
    }
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

    const int ahead = (int) (outRate * kPlayAheadSeconds);
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
        spx_uint32_t outLen = (spx_uint32_t) (kFrame * outRate / kRate) + 32;
        voiceUp.resize (outLen);
        speex_resampler_process_float (up, 0, mix, &inLen, voiceUp.data(), &outLen);

        const int w = juce::jmin ((int) outLen, playFifo->getFreeSpace());
        int s1, n1, s2, n2;
        playFifo->prepareToWrite (w, s1, n1, s2, n2);
        if (n1 > 0) std::memcpy (playData.get() + s1, voiceUp.data(), sizeof (float) * (size_t) n1);
        if (n2 > 0) std::memcpy (playData.get() + s2, voiceUp.data() + n1, sizeof (float) * (size_t) n2);
        playFifo->finishedWrite (n1 + n2);
    }
}

} // namespace SonoRocket

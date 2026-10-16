// SonoRocket talkback for the capture app.
//
// A microphone on its own input of the audio interface, separate from the
// master pair. Its voice goes to the listeners at low latency (16 kHz, 10 ms
// frames, never behind the master's 2 s buffer), and the listeners' voice
// comes back out of an output pair of the same interface.
//
// Echo: the master is captured on this very interface, sample-aligned with
// the mic, so it is a perfect reference for the echo canceller (speexdsp):
// what leaks from the studio speakers into the mic is subtracted. The voice
// we play out is a reference too, so the listener never hears himself.
//
// Threads: the audio callback only copies into lock-free FIFOs. Resampling,
// echo cancelling and networking run on the talkback thread, so the master
// capture (bit-exact) can never glitch because of talkback.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <vector>
#include "../RocketStreamer.h"

struct SpeexEchoState_;
struct SpeexPreprocessState_;
struct SpeexResamplerState_;

namespace SonoRocket {

class Talkback : private juce::Thread
{
public:
    explicit Talkback (RocketStreamer& s);
    ~Talkback() override;

    // Not real-time (device about to start). Safe while the audio callback is stopped.
    void prepare (double deviceSampleRate);

    // Audio thread. mic may be null (no mic input chosen): then only playback runs.
    // refL/refR: the master as captured. outVoice: receives the listeners' voice
    // (numSamples), already at the playback volume.
    void process (const float* mic, const float* refL, const float* refR,
                  float* outVoice, int numSamples) noexcept;

    // Any thread
    void setMicOpen (bool open) noexcept       { micOpen = open; }      // gate: send our voice?
    void setPlaybackGain (float g) noexcept    { playbackGain = g; }
    void setMicGain (float g) noexcept         { micGain = g; }
    float takeMicPeak() noexcept               { return micPeak.exchange (0.0f); }   // after echo cancel
    float takeVoicePeak() noexcept             { return voicePeak.exchange (0.0f); } // listener talking
    bool isSending() const noexcept            { return sending.load(); }

    static constexpr int kRate = RocketStreamer::kVoiceRate;
    static constexpr int kFrame = 160;               // 10 ms at 16 kHz
    static constexpr int kTailMs = 250;              // echo tail the canceller models

private:
    void run() override;
    void processCaptured();
    void feedPlayback();
    void destroyDsp();

    RocketStreamer& streamer;

    // audio → worker: interleaved [mic, refL, refR, voiceOut] at the device rate
    static constexpr int kCapCh = 4;
    juce::HeapBlock<float> capData;
    std::unique_ptr<juce::AbstractFifo> capFifo;
    // worker → audio: mono voice at the device rate
    juce::HeapBlock<float> playData;
    std::unique_ptr<juce::AbstractFifo> playFifo;
    int fifoFrames = 0;

    std::atomic<bool> micOpen { false }, sending { false }, prepared { false };
    std::atomic<float> playbackGain { 1.0f }, micGain { 1.0f };
    std::atomic<float> micPeak { 0.0f }, voicePeak { 0.0f };
    std::atomic<bool> playStarved { true };

    juce::CriticalSection dspLock;   // worker vs prepare()
    double deviceRate = 48000.0;
    SpeexEchoState_* echo = nullptr;
    SpeexPreprocessState_* pre = nullptr;
    SpeexResamplerState_* down = nullptr;   // device → 16 kHz, 4 channels
    SpeexResamplerState_* up = nullptr;     // 16 kHz → device, mono

    // worker scratch
    std::vector<float> capChunk, down16, voiceUp;
    std::vector<float> pend16;              // 16 kHz interleaved 4 ch, waiting for a full frame
    std::vector<int16_t> jitter;            // incoming voice at 16 kHz
    bool jitterPlaying = false;
    float fade = 0.0f;                      // send gate ramp, avoids clicks at open/close

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Talkback)
};

} // namespace SonoRocket

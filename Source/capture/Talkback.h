// SonoRocket talkback for the capture app.
//
// A microphone separate from the master: another input of the capture device,
// or any other input device (Windows often splits an interface into one device
// per input pair, or it can be a USB mic). Its voice goes to the listeners at
// low latency (16 kHz, 10 ms frames, never behind the master's 2 s buffer), and
// the listeners' voice comes back out of an output device of its own.
//
// Echo: the master as captured is the echo canceller's reference (speexdsp):
// what leaks from the studio speakers into the mic is subtracted. With the mic
// on the same device as the master the two are sample-aligned; from another
// device the canceller absorbs the (fixed) offset, and drift is trimmed. While
// the listener's voice plays, a hands-free mic is ducked so he never hears
// himself come back.
//
// Devices are kept apart on purpose: a talkback setting can never stop the
// master capture. Audio callbacks only copy into lock-free FIFOs; resampling,
// echo cancelling and networking run on the talkback thread.

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

    // Not real-time: call when the corresponding device is about to start.
    void prepareReference (double sampleRate);   // the capture device (master)
    void prepareMic (double sampleRate);         // the mic's device (may be the same)
    void prepareOutput (double sampleRate);      // the talkback output device
    void stopMic();                              // no mic: stop sending

    // Audio threads
    void captureReference (const float* refL, const float* refR, int numSamples) noexcept;
    void captureMic (const float* mic, int numSamples) noexcept;
    void render (float* out, int numSamples) noexcept;

    // Any thread
    void setMicOpen (bool open, bool pushToTalk) noexcept { micOpen = open; ptt = pushToTalk; }
    void setPlaybackGain (float g) noexcept    { playbackGain = g; }
    void setMicGain (float g) noexcept         { micGain = g; }
    float takeMicPeak() noexcept               { return micPeak.exchange (0.0f); }   // after echo cancel
    float takeVoicePeak() noexcept             { return voicePeak.exchange (0.0f); } // listener talking
    bool isSending() const noexcept            { return sending.load(); }
    void playTestTone() noexcept               { testTone = kRate / 2; }   // a short chime on the output, to find the right device

    static constexpr int kRate = RocketStreamer::kVoiceRate;
    static constexpr int kFrame = 160;               // 10 ms at 16 kHz
    static constexpr int kTailMs = 250;              // echo tail the canceller models

    // One audio stream from an audio thread to the worker, resampled to 16 kHz.
    struct Lane
    {
        ~Lane() { release(); }
        void prepare (int channels, double sampleRate);
        void release();
        void push (const float* const* chans, int n, float gain) noexcept;   // audio thread
        void pull();                                                          // worker: FIFO → pending (16 kHz)
        int frames16() const { return ch > 0 ? (int) (pending.size() / (size_t) ch) : 0; }
        void drop16 (int frames);

        int ch = 0;
        double rate = 48000.0;
        std::atomic<bool> ready { false };
        juce::HeapBlock<float> data;
        std::unique_ptr<juce::AbstractFifo> fifo;
        SpeexResamplerState_* rs = nullptr;
        std::vector<float> chunk, out, pending;   // pending: interleaved at 16 kHz
    };

private:
    void run() override;
    void processCaptured();
    void feedPlayback();

    RocketStreamer& streamer;
    Lane ref, mic;   // ref: master L/R; mic: mono

    // worker → output: mono voice at the output rate
    juce::HeapBlock<float> playData;
    std::unique_ptr<juce::AbstractFifo> playFifo;
    SpeexResamplerState_* up = nullptr;
    double outRate = 48000.0;

    std::atomic<bool> micOpen { false }, ptt { false }, sending { false }, outReady { false };
    std::atomic<juce::uint32> lastVoiceMs { 0 };
    std::atomic<int> testTone { 0 };
    double tonePhase = 0;
    std::atomic<float> playbackGain { 1.0f }, micGain { 1.0f };
    std::atomic<float> micPeak { 0.0f }, voicePeak { 0.0f };

    juce::CriticalSection dspLock;   // worker vs prepare*()
    SpeexEchoState_* echo = nullptr;        // studio speakers (master L/R)
    SpeexEchoState_* echoVoice = nullptr;   // the listener's voice we play
    SpeexPreprocessState_* pre = nullptr;

    std::vector<float> voiceUp;
    std::vector<float> voiceRef;   // listener voice as played (16 kHz): third echo reference
    float fade = 0.0f;   // send gate ramp, avoids clicks at open/close
    float duck = 1.0f;   // hands-free mic ducked while the listener talks

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Talkback)
};

} // namespace SonoRocket

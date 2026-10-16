// SonoRocket: streams the plugin input (the DAW master) to a SonoRocket relay,
// sample-exact, so it can be heard in any browser. See sonorocket/PROTOCOL.md.
//
// Real-time safety: pushAudio() runs on the audio thread and only writes into a
// lock-free single-producer/single-consumer FIFO. All networking happens on a
// background thread. If the network stalls, the FIFO absorbs up to
// kFifoSeconds; past that, new audio is dropped and counted (never blocking
// the host).

#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <deque>
#include <functional>
#include <vector>

namespace SonoRocket {

uint32_t crc32 (const void* data, size_t numBytes, uint32_t crc = 0) noexcept;

class RocketStreamer : private juce::Thread
{
public:
    enum class State { Off, Connecting, Streaming, Error };

    struct Config {
        juce::String host { "127.0.0.1" };
        int port { 9100 };
        juce::String room { "auto" };      // "auto": the relay picks the room and reports the link
        juce::String key;
        bool enabled { true };               // zero-config: streams as soon as a relay is reachable
        juce::String listenUrl;   // public base URL of the relay page, for the share link (UI only)
    };

    RocketStreamer();
    ~RocketStreamer() override;

    // Message thread
    void setConfig (const Config& c);
    Config getConfig() const;

    // Call from prepareToPlay (not real-time)
    void prepare (double sampleRate, int numChannels);

    // Audio thread. channels[c][i], numChannels 1 or 2.
    void pushAudio (const float* const* channels, int numChannels, int numSamples) noexcept;

    // Status, any thread
    State getState() const noexcept           { return state.load(); }
    juce::String getStatusText() const;
    int64_t getFramesSent() const noexcept    { return framesSent.load(); }
    int64_t getFramesDropped() const noexcept { return framesDropped.load(); }
    double getFifoSeconds() const noexcept;
    juce::String getListenerLink() const;    // link reported by the relay (empty until known)

    // ---- Talkback (PROTOCOL.md, "Talkback"): 16 kHz mono int16, ~10 ms frames.
    // Voice rides the same connection but is always sent ahead of master blocks.
    // Enabling it changes the handshake, so it reconnects.
    void setTalkbackEnabled (bool on);
    bool isTalkbackEnabled() const noexcept   { return talkEnabled.load(); }
    // Any non-audio thread. Drops the oldest frame rather than ever delaying.
    void pushVoice (const int16_t* pcm, int numSamples);
    // Any non-audio thread. Voice from a listener; false when nothing is waiting.
    bool popIncomingVoice (std::vector<int16_t>& pcm, uint16_t& listenerId);
    static constexpr int kVoiceRate = 16000;

    // Talk-only: connect as the relay's "talk" sender (voice both ways, no audio blocks),
    // while the master comes from another sender, e.g. the plugin in a DAW on this computer.
    // The relay then sends us a copy of that master (for the echo canceller's reference).
    void setTalkOnly (bool on);
    bool isTalkOnly() const noexcept { return talkOnly.load(); }
    std::function<void (double sampleRate, int channels)> onMasterFormat;            // network thread
    std::function<void (const float* interleaved, int frames, int channels)> onMasterBlock;   // network thread

    static constexpr int kBlockFrames  = 1024;
    static constexpr double kFifoSeconds = 20.0;

    // exposed for tests: builds one length-prefixed RKT1 audio frame
    static void buildAudioFrame (juce::MemoryBlock& out, uint32_t seq, const float* interleaved, int frames, int channels);

private:
    void run() override;
    bool connectAndHandshake();
    bool sendAll (const void* data, int size);
    bool sendFormatChange();
    void disconnect (const juce::String& why, State newState);
    void pollRelayMessages();
    void handleRelayLine (const juce::String& line);
    void handleRelayBinary (const uint8_t* p, size_t n);
    bool sendPendingVoice();
    void setLink (const juce::String& l);

    mutable juce::CriticalSection configLock;
    Config config;
    std::atomic<int> configGeneration { 0 };
    int connectedGeneration = -1;

    // FIFO of interleaved float samples. Allocated once, never resized, so the
    // audio thread and the network thread never need a lock.
    static constexpr int kFifoCapacitySamples = (int) (kFifoSeconds * 96000.0 * 2.0);
    juce::HeapBlock<float> fifoData;
    std::unique_ptr<juce::AbstractFifo> fifo;
    std::atomic<int> channels { 2 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> formatGeneration { 0 };
    int sentFormatGeneration = -1;

    // Only the network thread creates or destroys `socket`. Other threads may only
    // interrupt it (shutdown) while holding socketLock; they never close or delete it.
    std::unique_ptr<juce::StreamingSocket> socket;
    juce::CriticalSection socketLock;
    void interruptSocket();
    std::atomic<State> state { State::Off };
    juce::String lastError, relayLink;
    std::vector<uint8_t> rx;   // bytes from the relay: text lines, or 0x00 + u32 len + payload

    std::atomic<bool> talkEnabled { false }, talkOnly { false };
    int masterCopyChannels = 2;
    juce::CriticalSection voiceLock;
    std::deque<juce::MemoryBlock> voiceOut;                                    // framed, ready to send
    std::deque<std::pair<uint16_t, std::vector<int16_t>>> voiceIn;
    uint32_t voiceSeq = 0;
    static constexpr size_t kVoiceQueue = 16;   // ~160 ms
    mutable juce::CriticalSection errorLock;

    uint32_t seq = 0;
    std::atomic<int64_t> framesSent { 0 }, framesDropped { 0 };
    juce::uint32 lastSendMs = 0;
    int backoffMs = 500;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RocketStreamer)
};

} // namespace SonoRocket

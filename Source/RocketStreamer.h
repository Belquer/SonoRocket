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
    juce::String relayLineBuf;
    mutable juce::CriticalSection errorLock;

    uint32_t seq = 0;
    std::atomic<int64_t> framesSent { 0 }, framesDropped { 0 };
    juce::uint32 lastSendMs = 0;
    int backoffMs = 500;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RocketStreamer)
};

} // namespace SonoRocket

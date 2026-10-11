// SonoRocket streamer. See RocketStreamer.h and sonorocket/PROTOCOL.md.

#include "RocketStreamer.h"

#if JUCE_WINDOWS
 #include <winsock2.h>
#else
 #include <sys/types.h>
 #include <sys/socket.h>
#endif

#if ! JUCE_LITTLE_ENDIAN
 #error "SonoRocket wire format is little-endian float32; add byte swapping for this target"
#endif

namespace SonoRocket {

static const uint32_t* crcTable()
{
    static uint32_t table[256] = {};
    static bool init = [] {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            table[n] = c;
        }
        return true;
    }();
    juce::ignoreUnused (init);
    return table;
}

uint32_t crc32 (const void* data, size_t numBytes, uint32_t crc) noexcept
{
    const auto* t = crcTable();
    auto* p = static_cast<const uint8_t*> (data);
    crc = ~crc;
    for (size_t i = 0; i < numBytes; ++i) crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void putU32 (uint8_t* p, uint32_t v) noexcept
{
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}

void RocketStreamer::buildAudioFrame (juce::MemoryBlock& out, uint32_t seq, const float* interleaved, int frames, int chans)
{
    const size_t pcmBytes = (size_t) frames * (size_t) chans * sizeof (float);
    const size_t payload = 13 + pcmBytes;
    out.setSize (4 + payload, false);
    auto* p = static_cast<uint8_t*> (out.getData());
    putU32 (p, (uint32_t) payload);
    p[4] = 1;
    putU32 (p + 5, seq);
    putU32 (p + 9, (uint32_t) frames);
    putU32 (p + 13, crc32 (interleaved, pcmBytes));
    std::memcpy (p + 17, interleaved, pcmBytes);   // raw IEEE-754 bits, untouched
}

RocketStreamer::RocketStreamer()
    : juce::Thread ("SonoRocket streamer")
{
    fifoData.allocate ((size_t) kFifoCapacitySamples, true);
    fifo = std::make_unique<juce::AbstractFifo> (kFifoCapacitySamples);
    startThread (juce::Thread::Priority::high);
}

RocketStreamer::~RocketStreamer()
{
    // Never close the socket from here: the network thread may be inside a call on it.
    // Shut it down (unblocks any read/write), then let the thread clean up and exit.
    signalThreadShouldExit();
    notify();
    interruptSocket();
    stopThread (6000);
}

void RocketStreamer::interruptSocket()
{
    const juce::ScopedLock sl (socketLock);
    if (socket != nullptr && socket->getRawSocketHandle() >= 0) {
       #if JUCE_WINDOWS
        ::shutdown ((SOCKET) socket->getRawSocketHandle(), SD_BOTH);
       #else
        ::shutdown (socket->getRawSocketHandle(), SHUT_RDWR);
       #endif
    }
}

void RocketStreamer::setConfig (const Config& c)
{
    {
        const juce::ScopedLock sl (configLock);
        const bool same = c.host == config.host && c.port == config.port && c.room == config.room
                       && c.key == config.key && c.enabled == config.enabled;
        config = c;
        if (same) return;   // UI-only change (or a session reload): don't drop the connection
    }
    ++configGeneration;
    notify();
}

RocketStreamer::Config RocketStreamer::getConfig() const
{
    const juce::ScopedLock sl (configLock);
    return config;
}

void RocketStreamer::prepare (double sr, int numChannels)
{
    numChannels = juce::jlimit (1, 2, numChannels);
    if (sr != sampleRate.load() || numChannels != channels.load()) {
        sampleRate = sr;
        channels = numChannels;
        ++formatGeneration;   // network thread flushes stale samples and announces the new format
        notify();
    }
}

void RocketStreamer::pushAudio (const float* const* chans, int numChannels, int numSamples) noexcept
{
    if (numSamples <= 0 || numChannels <= 0 || state.load() != State::Streaming)
        return;

    const int ch = channels.load();
    const int total = numSamples * ch;
    if (fifo->getFreeSpace() < total) {
        framesDropped += numSamples;
        return;
    }

    int start1, size1, start2, size2;
    fifo->prepareToWrite (total, start1, size1, start2, size2);

    int idx = 0;  // interleaved sample index into this block
    auto writeRegion = [&] (int start, int size) {
        float* dst = fifoData.get() + start;
        for (int k = 0; k < size; ++k, ++idx) {
            const int frame = idx / ch;
            const int c = idx % ch;
            const float* src = chans[juce::jmin (c, numChannels - 1)];
            dst[k] = src[frame];
        }
    };
    writeRegion (start1, size1);
    writeRegion (start2, size2);
    fifo->finishedWrite (size1 + size2);
}

double RocketStreamer::getFifoSeconds() const noexcept
{
    const double sr = sampleRate.load();
    return sr > 0 ? fifo->getNumReady() / (double) channels.load() / sr : 0.0;
}

juce::String RocketStreamer::getStatusText() const
{
    switch (state.load()) {
        case State::Off:        return "off";
        case State::Connecting: return "connecting...";
        case State::Streaming:  return "live";
        case State::Error: {
            const juce::ScopedLock sl (errorLock);
            return "error: " + lastError;
        }
    }
    return {};
}

juce::String RocketStreamer::getListenerLink() const
{
    const juce::ScopedLock sl (errorLock);
    return relayLink;
}

void RocketStreamer::setLink (const juce::String& l)
{
    const juce::ScopedLock sl (errorLock);
    relayLink = l;
}

void RocketStreamer::pollRelayMessages()
{
    // The relay may push "LINK <url>\n" when its public tunnel comes up or changes.
    while (socket && socket->waitUntilReady (true, 0) == 1) {
        char buf[512];
        const int n = socket->read (buf, (int) sizeof (buf), false);
        if (n <= 0) { disconnect ("connection lost", State::Error); return; }
        relayLineBuf += juce::String::fromUTF8 (buf, n);
        int nl;
        while ((nl = relayLineBuf.indexOfChar ('\n')) >= 0) {
            const auto line = relayLineBuf.substring (0, nl).trim();
            relayLineBuf = relayLineBuf.substring (nl + 1);
            if (line.startsWith ("LINK ")) setLink (line.substring (5).trim());
        }
        if (relayLineBuf.length() > 8192) relayLineBuf.clear();
    }
}

void RocketStreamer::disconnect (const juce::String& why, State newState)
{
    {
        const juce::ScopedLock sl (socketLock);
        if (socket) { socket->close(); socket.reset(); }
    }
    {
        const juce::ScopedLock sl (errorLock);
        lastError = why;
    }
    state = newState;
}

bool RocketStreamer::sendAll (const void* data, int size)
{
    auto* p = static_cast<const char*> (data);
    while (size > 0 && ! threadShouldExit()) {
       #if JUCE_WINDOWS
        const int n = socket->write (p, size);
       #else
        // Never let a dropped relay raise SIGPIPE: inside a DAW that would kill the host.
        #ifdef MSG_NOSIGNAL
         const int flags = MSG_NOSIGNAL;
        #else
         const int flags = 0;   // Apple: SO_NOSIGPIPE is set on connect
        #endif
        const int n = (int) ::send (socket->getRawSocketHandle(), p, (size_t) size, flags);
       #endif
        if (n <= 0) return false;
        p += n; size -= n;
    }
    lastSendMs = juce::Time::getMillisecondCounter();
    return size == 0;
}

bool RocketStreamer::connectAndHandshake()
{
    const Config c = getConfig();
    connectedGeneration = configGeneration.load();
    state = State::Connecting;


    {
        const juce::ScopedLock sl (socketLock);
        socket = std::make_unique<juce::StreamingSocket>();
    }
    if (threadShouldExit()) { disconnect ({}, State::Off); return false; }
    if (! socket->connect (c.host, c.port, 4000)) {
        disconnect ("can't reach " + c.host + ":" + juce::String (c.port), State::Error);
        return false;
    }
   #if JUCE_MAC || JUCE_IOS
    {
        int one = 1;
        setsockopt (socket->getRawSocketHandle(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
    }
   #endif

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("v", 1);
    obj->setProperty ("room", c.room.isEmpty() ? juce::String ("auto") : c.room);
    obj->setProperty ("key", c.key);
    obj->setProperty ("sr", (int) sampleRate.load());
    obj->setProperty ("ch", channels.load());
    obj->setProperty ("fmt", "f32");
    obj->setProperty ("app", "SonoRocket");
    const juce::String hs = juce::JSON::toString (juce::var (obj), true) + "\n";
    sentFormatGeneration = formatGeneration.load();

    if (! sendAll (hs.toRawUTF8(), (int) hs.getNumBytesAsUTF8())) {
        disconnect ("handshake send failed", State::Error);
        return false;
    }

    juce::String reply;
    while (! threadShouldExit()) {
        if (socket->waitUntilReady (true, 4000) != 1) { disconnect ("relay did not answer", State::Error); return false; }
        char ch = 0;
        if (socket->read (&ch, 1, false) != 1) { disconnect ("relay closed", State::Error); return false; }
        if (ch == '\n') break;
        reply += ch;
        if (reply.length() > 4096) break;
    }
    if (! reply.startsWith ("OK")) {
        disconnect (reply.fromFirstOccurrenceOf ("ERR ", false, false).trim().isNotEmpty()
                        ? reply.fromFirstOccurrenceOf ("ERR ", false, false).trim()
                        : juce::String ("rejected by relay"), State::Error);
        return false;
    }

    // "OK {json}" carries the room the relay chose and the public listener link
    {
        const auto js = juce::JSON::parse (reply.fromFirstOccurrenceOf ("OK", false, false).trim());
        setLink (js.getProperty ("link", juce::var()).toString());
    }
    relayLineBuf.clear();

    // Start clean: nothing queued from before this session.
    fifo->finishedRead (fifo->getNumReady());
    seq = 0;
    backoffMs = 500;
    state = State::Streaming;
    return true;
}

bool RocketStreamer::sendFormatChange()
{
    sentFormatGeneration = formatGeneration.load();
    const juce::String js = "{\"sr\":" + juce::String ((int) sampleRate.load())
                          + ",\"ch\":" + juce::String (channels.load()) + ",\"fmt\":\"f32\"}";
    juce::MemoryBlock mb;
    const int payload = 1 + (int) js.getNumBytesAsUTF8();
    mb.setSize ((size_t) (4 + payload));
    auto* p = static_cast<uint8_t*> (mb.getData());
    putU32 (p, (uint32_t) payload);
    p[4] = 2;
    std::memcpy (p + 5, js.toRawUTF8(), (size_t) payload - 1);
    return sendAll (mb.getData(), (int) mb.getSize());
}

void RocketStreamer::run()
{
    std::vector<float> block;
    juce::MemoryBlock frame;

    while (! threadShouldExit())
    {
        const Config c = getConfig();

        if (! c.enabled) {
            if (state.load() != State::Off) disconnect ({}, State::Off);
            fifo->finishedRead (fifo->getNumReady());
            wait (100);
            continue;
        }

        if (socket && connectedGeneration != configGeneration.load())
            disconnect ({}, State::Connecting);      // settings changed: reconnect

        if (! socket) {
            if (! connectAndHandshake()) {
                wait (backoffMs);
                backoffMs = juce::jmin (backoffMs * 2, 3000);
            }
            continue;
        }

        if (sentFormatGeneration != formatGeneration.load()) {
            fifo->finishedRead (fifo->getNumReady());  // drop samples in the old format
            if (! sendFormatChange()) { disconnect ("connection lost", State::Error); continue; }
        }

        const int ch = channels.load();
        const int blockSamples = kBlockFrames * ch;
        block.resize ((size_t) blockSamples);

        bool ok = true;
        while (ok && fifo->getNumReady() >= blockSamples && ! threadShouldExit()) {
            int s1, n1, s2, n2;
            fifo->prepareToRead (blockSamples, s1, n1, s2, n2);
            std::memcpy (block.data(), fifoData.get() + s1, sizeof (float) * (size_t) n1);
            if (n2 > 0) std::memcpy (block.data() + n1, fifoData.get() + s2, sizeof (float) * (size_t) n2);
            fifo->finishedRead (n1 + n2);

            buildAudioFrame (frame, seq++, block.data(), kBlockFrames, ch);
            ok = sendAll (frame.getData(), (int) frame.getSize());
            if (ok) framesSent += kBlockFrames;
        }
        if (! ok) { disconnect ("connection lost", State::Error); continue; }

        pollRelayMessages();

        if (juce::Time::getMillisecondCounter() - lastSendMs > 2000) {
            const uint8_t ka[5] = { 1, 0, 0, 0, 3 };
            if (! sendAll (ka, 5)) { disconnect ("connection lost", State::Error); continue; }
        }

        wait (2);
    }
    disconnect ({}, State::Off);
}

} // namespace SonoRocket

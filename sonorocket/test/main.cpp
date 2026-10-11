// Drives RocketStreamer exactly like a DAW would: variable block sizes from an
// "audio thread", real-time pacing. Writes the reference samples to a .f32 file
// so the listener can compare bit for bit.
#include "RocketStreamer.h"
#include <cmath>
#include <cstdio>
#include <thread>
#include <chrono>

int main (int argc, char** argv)
{

    const juce::String host = argc > 1 ? argv[1] : "127.0.0.1";
    const juce::String room = argc > 2 ? argv[2] : "cpp";
    const juce::String key  = argc > 3 ? argv[3] : "";
    const double seconds    = argc > 4 ? atof (argv[4]) : 5.0;
    const char* refPath     = argc > 5 ? argv[5] : "ref.f32";
    const double sr = 48000.0;

    SonoRocket::RocketStreamer s;
    s.prepare (sr, 2);
    SonoRocket::RocketStreamer::Config c; c.host = host; c.port = 9100; c.room = room; c.key = key; c.enabled = true;
    s.setConfig (c);
    for (int i = 0; i < 300 && s.getState() != SonoRocket::RocketStreamer::State::Streaming; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    if (s.getState() != SonoRocket::RocketStreamer::State::Streaming) { printf ("not streaming: %s\n", s.getStatusText().toRawUTF8()); return 2; }

    FILE* ref = fopen (refPath, "wb");
    juce::Random rng (1234);
    const int sizes[] = { 32, 64, 128, 256, 512, 1000, 1024, 2048, 77 };   // hosts vary block size
    std::vector<float> L (4096), R (4096), inter (8192);
    uint32_t x = 0x5eed;
    auto rnd = [&] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x / 4294967296.0; };
    int64_t produced = 0, target = (int64_t) (seconds * sr);
    auto t0 = std::chrono::steady_clock::now();
    int bi = 0;
    while (produced < target) {
        int n = sizes[bi++ % 9];
        n = (int) std::min<int64_t> (n, target - produced);
        for (int i = 0; i < n; ++i) {
            const int64_t k = (produced + i) % 1000;
            auto gen = [&] (int ch) -> float {
                if (k < 5) return 0.0f;
                if (k < 10) return (float) ((rnd() - 0.5) * 1e-40);
                if (k < 20) return (float) ((rnd() - 0.5) * 1e-9);
                if (k < 30) return (float) ((rnd() < 0.5 ? -1 : 1) * (1 + rnd() * 0.5));
                return (float) (std::sin ((produced + i) * 0.031 * (ch + 1)) * 0.5 + (rnd() - 0.5) * 0.4);
            };
            L[(size_t) i] = gen (0); R[(size_t) i] = gen (1);
            inter[(size_t) (2 * i)] = L[(size_t) i]; inter[(size_t) (2 * i + 1)] = R[(size_t) i];
        }
        const float* chans[2] = { L.data(), R.data() };
        s.pushAudio (chans, 2, n);
        fwrite (inter.data(), sizeof (float), (size_t) (2 * n), ref);
        produced += n;
        std::this_thread::sleep_until (t0 + std::chrono::microseconds ((int64_t) (produced / sr * 1e6)));
    }
    fclose (ref);
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    printf ("{\"produced\":%lld,\"sent\":%lld,\"dropped\":%lld,\"state\":\"%s\"}\n",
            (long long) produced, (long long) s.getFramesSent(), (long long) s.getFramesDropped(), s.getStatusText().toRawUTF8());
    return 0;
}

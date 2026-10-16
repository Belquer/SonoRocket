// Talkback test, against a real relay (see run.sh). Drives Talkback like the
// capture app's audio callback, in real time, 48 kHz, 480-sample blocks:
//   0–8 s   mic = room echo of the master only, mic forced open  → echo must be cancelled
//   8–11 s  mic = echo + "voice"                                  → voice must pass
//   11–14 s listener sends a tone (talk-listen.js)                → must come out of outVoice
// Prints phase marks (epoch ms) and what came out, for check.py.
#include "capture/Talkback.h"
#include <cmath>
#include <cstdio>
#include <thread>
#include <chrono>

using namespace SonoRocket;
static long long nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now().time_since_epoch()).count(); }

int main (int argc, char** argv)
{
    const juce::String room = argc > 1 ? argv[1] : "talk-test";
    const double sr = 48000.0; const int bs = 480;
    // argv[3] == "plugin": the master comes from a separate sender (a DAW plugin), and the
    // talkback runs talk-only, getting the master back from the relay as its echo reference
    const bool pluginMode = argc > 3 && juce::String (argv[3]) == "plugin";
    RocketStreamer s, plugin;
    RocketStreamer::Config c; c.room = room; c.enabled = true;
    { RocketStreamer::Config off; off.enabled = false; plugin.setConfig (off); }   // streams only in plugin mode
    s.prepare (sr, 2);
    s.setTalkbackEnabled (true);
    s.setTalkOnly (pluginMode);
    if (pluginMode) { plugin.prepare (sr, 2); plugin.setConfig (c); }
    s.setConfig (c);
    for (int i = 0; i < 300 && s.getState() != RocketStreamer::State::Streaming; ++i) std::this_thread::sleep_for (std::chrono::milliseconds (10));
    if (s.getState() != RocketStreamer::State::Streaming) { printf ("not streaming\n"); return 2; }

    // argv[2]: mic offset in ms (simulates the mic on another device, e.g. 30)
    const int micDelay = (int) (sr * (argc > 2 ? atof (argv[2]) : 0.0) / 1000.0);
    Talkback tb (s);
    std::vector<float> cl, cr;
    if (pluginMode) {
        s.onMasterFormat = [&] (double r, int) { tb.prepareReference (r); };
        s.onMasterBlock = [&] (const float* x, int frames, int ch) {
            cl.resize ((size_t) frames); cr.resize ((size_t) frames);
            for (int i = 0; i < frames; ++i) { cl[(size_t) i] = x[i * ch]; cr[(size_t) i] = x[i * ch + ch - 1]; }
            tb.captureReference (cl.data(), cr.data(), frames);
        };
        tb.setReferenceLatencyMs (100);
        for (int i = 0; i < 300 && plugin.getState() != RocketStreamer::State::Streaming; ++i) std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    tb.prepareReference (sr);
    tb.prepareMic (sr);
    tb.prepareOutput (sr);
    tb.setMicOpen (true, true);
    std::vector<float> micLine ((size_t) micDelay + 1); size_t mlp = 0;
    // from 11 s the studio speaker also plays the listener's voice back into the mic (60 ms later)
    const int vDelay = (int) (0.060 * sr); std::vector<float> outLine ((size_t) vDelay + 1, 0.0f); size_t olp = 0;
    double backE = 0; long long backN = 0;

    juce::Random rng (7);
    // "music": filtered noise, different L/R, with a slow level wobble
    float lpL = 0, lpR = 0;
    // room: echo = sum of delayed, attenuated taps of L and R (speakers → mic)
    const int histN = 8192; std::vector<float> hL (histN), hR (histN); int hp = 0;
    struct Tap { int d; float gl, gr; };
    const Tap taps[] = { {240, 0.35f, 0.20f}, {310, 0.15f, 0.30f}, {900, 0.08f, 0.06f}, {1900, 0.05f, 0.05f}, {3300, 0.03f, 0.02f} };

    std::vector<float> L (bs), R (bs), mic (bs), out (bs);
    double ph = 0, t = 0;
    double echoE = 0, echoN = 0, voiceE = 0, voiceN = 0, outE[3] = {0,0,0}; long long outN[3] = {0,0,0};
    long long t0 = nowMs();
    printf ("MARK start %lld\n", t0);
    bool m8 = false, m11 = false;
    auto next = std::chrono::steady_clock::now();
    for (int b = 0; t < 14.0; ++b) {
        for (int i = 0; i < bs; ++i, t += 1.0 / sr) {
            const float wob = 0.6f + 0.4f * (float) std::sin (2 * M_PI * 0.3 * t);
            lpL += 0.15f * ((rng.nextFloat() * 2 - 1) - lpL);
            lpR += 0.15f * ((rng.nextFloat() * 2 - 1) - lpR);
            L[i] = 0.9f * wob * lpL; R[i] = 0.9f * wob * lpR;
            hL[(size_t) hp] = L[i]; hR[(size_t) hp] = R[i];
            float e = 0;
            for (auto& tp : taps) { const int k = (hp - tp.d + histN) % histN; e += tp.gl * hL[(size_t) k] + tp.gr * hR[(size_t) k]; }
            hp = (hp + 1) % histN;
            float v = 0;
            if (t >= 8.0 && t < 11.0) {   // "voice": 180 Hz buzz with formant-ish harmonics, syllable envelope
                ph += 2 * M_PI * 180 / sr;
                const float env = (float) std::pow (std::sin (M_PI * std::fmod (t * 4, 1.0)), 2);
                v = 0.25f * env * (float) (std::sin (ph) + 0.5 * std::sin (3 * ph) + 0.3 * std::sin (5 * ph));
            }
            float voiceEcho = 0;
            if (t >= 11.0) { voiceEcho = 0.5f * outLine[olp]; backE += voiceEcho * voiceEcho; ++backN; }
            if (t >= 11.0 && getenv ("NOMUSIC")) e = 0;
            micLine[mlp] = e + v + voiceEcho; mlp = (mlp + 1) % micLine.size();
            mic[i] = micLine[mlp];   // oldest = delayed by micDelay samples
            if (t >= 3 && t < 8) { echoE += e * e; ++echoN; }
            if (t >= 8 && t < 11) { voiceE += v * v; ++voiceN; }
        }
        if (! m8 && t >= 8) { printf ("MARK voice %lld\n", nowMs()); m8 = true; }
        if (! m11 && t >= 11) { printf ("MARK listen %lld\n", nowMs()); fflush (stdout); m11 = true; }
        const float* m[2] = { L.data(), R.data() };
        if (pluginMode) plugin.pushAudio (m, 2, bs);   // the DAW plugin streams the master
        else { s.pushAudio (m, 2, bs); tb.captureReference (L.data(), R.data(), bs); }   // same connection, as in the app
        tb.captureMic (mic.data(), bs);
        tb.render (out.data(), bs);
        for (int i = 0; i < bs; ++i) { outLine[olp] = out[i]; olp = (olp + 1) % outLine.size(); }
        const int phase = t < 8 ? 0 : t < 11 ? 1 : 2;
        for (int i = 0; i < bs; ++i) { outE[phase] += out[i] * out[i]; ++outN[phase]; }
        if (phase == 2) {
            static bool first = true;
            float pk = 0; for (int i = 0; i < bs; ++i) pk = std::max (pk, std::abs (out[i]));
            if (first && pk > 0.05f) { printf ("MARK heard %lld\n", nowMs()); first = false; }
        }
        next += std::chrono::microseconds ((long long) (1e6 * bs / sr));
        std::this_thread::sleep_until (next);
    }
    auto db = [] (double e, double n) { return n > 0 && e > 0 ? 10 * std::log10 (e / n) : -200.0; };
    printf ("RESULT echo_in_dB %.1f\n", db (echoE, echoN));
    printf ("RESULT voice_in_dB %.1f\n", db (voiceE, voiceN));
    printf ("RESULT out_phase2_dB %.1f\n", db (outE[2], (double) outN[2]));
    printf ("RESULT dropped %lld\n", (long long) s.getFramesDropped());
    printf ("RESULT voice_echo_in_dB %.1f\n", db (backE, (double) backN));
    return 0;
}

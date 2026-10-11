# SonoRocket

Stream the master of any DAW, sample-exact, to any browser: iPhone, Android, Mac, Windows, Linux.
Made for remote mix reviews, where the producer plays the session and the listener hears exactly what leaves the master bus, with no codec, no resampling, no dither and no conferencing-app processing.

SonoRocket is a fork of [SonoBus](https://github.com/sonosaurus/sonobus) (GPLv3) by Jesse Chappell. Everything SonoBus does (peer-to-peer network audio between DAWs) still works; SonoRocket adds a browser path and retunes the defaults for fidelity over latency.

```
 DAW master ──► SonoRocket plugin ──TCP──► relay ──WebSocket──► any browser
 (VST3/AU/AAX/LV2,                         (Node)                (AudioWorklet,
  or standalone + loopback)                                       2 s+ buffer)
```

## What "exact" means here

| Stage | What happens to the samples |
|---|---|
| Plugin | Taps the input buffer before any SonoBus processing; copies raw float32 bits into a lock-free FIFO |
| Network | TCP, so nothing is lost or reordered; a 20 s FIFO rides out network stalls |
| Relay | Forwards payloads byte-for-byte; never decodes |
| Browser | CRC32 checks every block; the worklet plays samples as-is (no interpolation, no gain). Underruns rebuffer with silence; nothing is stretched or concealed |

The page shows a green **"integridade: bit a bit ✓"** while every block matches.
The one stage outside our control is the listener's device. If the phone's hardware runs at a different rate than the stream, the browser resamples on output, and the page says so ("browser reamostra").

Verified in this repo (`sonorocket/test`): the real VST3, loaded in a headless host and fed overs above 0 dBFS, denormals and values below the 24-bit floor in varying block sizes, arrives at the listener with **0 sample mismatches**.

## Quick start (no server: relay on the producer's Mac)

1. Install the plugin; put **SonoRocket** as the last insert on the master.
2. On the same machine: `brew install cloudflared node`, then `sonorocket/relay/start-with-tunnel.sh mix-luciano`.
3. In the plugin, click **Rocket**: Relay `127.0.0.1`, Port `9100`, Room `mix-luciano`, Listen URL = the `trycloudflare.com` address printed by the script. Click **Start streaming**.
4. Send the link (**Copy listener link**). The listener opens it and taps **Ouvir**.

Talk over Meet/WhatsApp as usual with the conferencing app's audio muted on your music output, or keep calling on a separate device. The browser stream runs ~2 s behind, so refer to timecode or bar numbers.

## Relay on a server (always-on link)

```
cd sonorocket/relay && npm install
RELAY_KEY=some-long-secret SENDER_PORT=9100 HTTP_PORT=8080 node server.js
```

Put HTTPS in front of port 8080 (Caddy, nginx or a Cloudflare tunnel). Browsers need HTTPS for AudioWorklet everywhere except `localhost`. Open TCP 9100 for senders and put the same key in the plugin.

## Plugin defaults (vs. SonoBus)

* Browser stream: float32, the host's native sample rate, stereo.
* Peer (SonoBus) path: PCM 32-bit float, send channels match the input, fixed 2 s jitter buffer (max 8 s), auto-resize off, dynamic resampling off, 8 s retransmit window.
* Plugin instance: receive muted, so nothing from the network can leak into a bounce. Standalone: send muted, so it acts as a listener.

## Building

Same as SonoBus (see `README-SonoBus.md`); the plugin JUCE is `deps/juce`.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release            # VST3, AU (mac), LV2 (linux), Standalone
cmake -S . -B build -DAAX_SDK_PATH=/path/to/aax-sdk ...             # + AAX
```

**Pro Tools.** It loads AAX only, and release Pro Tools only loads PACE-signed AAX. That requires an Avid developer account and PACE's signing tools (free for developers, but approval takes time). Until then, host `SonoRocket.vst3` inside Blue Cat PatchWork (AAX) on the master. Check the AAX SDK's license terms against GPLv3 before distributing AAX binaries.

**Any app, not just DAWs.** Run the SonoRocket standalone with a loopback device (BlackHole on macOS, VB-Cable on Windows, PipeWire on Linux) as its input.

## Protocol

`sonorocket/PROTOCOL.md`. Test tools: `sonorocket/tools/send-test.js` (stream a test signal or a WAV) and `listen-verify.js` (headless bit-for-bit check).

## License

GPLv3 with the SonoBus app-store exception (`LICENSE`, `LICENSE_EXCEPTION`). Source must stay available.

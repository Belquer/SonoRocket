# SonoRocket wire protocol (RKT1)

Goal: the samples that leave the DAW master are the samples that reach the
browser. No codec, no resampling, no dither. Lossless by construction (TCP),
verified by construction (CRC32 per block).

## Sender → relay (TCP, default port 9100)

1. Sender sends one line of JSON terminated by `\n`:

   ```json
   {"v":1,"room":"mix-luciano","key":"<RELAY_KEY>","sr":48000,"ch":2,"fmt":"f32"}
   ```

   `room` may be `"auto"`: the relay then uses its own room (stable for the run).

2. Relay answers `OK {"room":"…","link":"…"}\n` or `ERR <reason>\n` (and closes).
   Later, whenever the public link changes (tunnel up, restarted), the relay
   sends `LINK <url>\n` on the same connection. Senders that don't care can
   ignore incoming bytes.

3. Sender then streams length-prefixed frames, all little-endian:

   | bytes | field                                           |
   |-------|-------------------------------------------------|
   | 4     | `len` = size of the payload that follows        |
   | len   | payload                                         |

   Payload type 1, audio block:

   | bytes        | field                                    |
   |--------------|------------------------------------------|
   | 1            | type = 1                                 |
   | 4            | seq (u32, +1 per block, wraps)           |
   | 4            | nframes (u32)                            |
   | 4            | crc32 (IEEE) of the PCM bytes below      |
   | nframes*ch*4 | PCM, float32 LE, interleaved             |

   Payload type 2, format change: `type` byte followed by UTF-8 JSON
   `{"sr":..,"ch":..,"fmt":"f32"}`. Sent when the host sample rate changes.

   Payload type 3, keepalive: just the type byte. Sent every 2 s of silence
   or transport stop, so the relay knows the sender is alive.

## Relay → browser (WebSocket, `/ws?room=<room>`)

* Text message: JSON `{"type":"format","sr":..,"ch":..,"fmt":"f32"}` on join
  and whenever the format changes; `{"type":"sender","online":true|false}`.
* Binary message: an audio payload exactly as received (type 1), untouched.
  The relay never decodes or re-encodes audio.

On join the relay sends the last few seconds of blocks it holds so the
listener's buffer fills immediately.

## Why float32 and not 24-bit

Pro Tools (and every modern DAW) mixes in floating point. Sending float32
means even overs above 0 dBFS and anything below the 24-bit floor arrive
intact. Cost: 48 kHz stereo is 3.1 Mbit/s, 96 kHz is 6.1 Mbit/s.

## Talkback (voice both ways, low latency)

Separate from the master: 16 kHz mono int16 LE, ~10 ms frames (160 samples, max 960).
Voice never enters the backlog, never waits behind master blocks (both relay and
sender send it first) and queues hold at most ~160 ms: late voice is dropped, not delayed.

* Handshake: the sender adds `"talk":1`. Listeners are told in
  `{"type":"sender","online":true,"talk":true}`.
* Sender → relay, payload type 4: `[4][seq u32][n u16][n × int16]`. Relayed to every
  listener as a binary WebSocket message, unchanged.
* Listener → relay, binary WebSocket message type 5: `[5][seq u32][n u16][n × int16]`.
* Relay → sender: binary frames are marked with a leading `0x00` byte (text lines never
  contain one): `0x00, len u32, payload`, payload `[5][listener id u16][seq u32][n u16][n × int16]`.

Gate (both sides): with the DAW stopped (master below −60 dBFS for 0.7 s) the mic is open;
while it plays, the mic only opens while T (or the talk button, or a MIDI pedal on the
capture app) is held. The capture app cancels the studio speakers' echo from the
talkback mic using the master it captures as the reference (speexdsp, 250 ms tail).

## Video (between pages)

Pages encode their camera with WebCodecs (H.264 Annex B, else VP8; 640×360, 15 fps,
~700 kbit/s) and send it over the same WebSocket; the relay passes it to the other pages
of the room (never to the sender) and never decodes it. Priority: voice, then video,
then the master (which has its 2 s buffer).

* Page → relay, binary: `[6][0 u16][flags u8, 1 = keyframe][ts ms u32][chunk]`; the relay
  stamps the sender's id into bytes 1–2 before forwarding.
* Text, page ↔ page via the relay (stamped with `from`): `{"type":"vconf","codec":..,"w":..,"h":..}`,
  `{"type":"vkey"}` (please send config + a keyframe), `{"type":"vbye"}`.
* A page whose video queue overflows loses it, skips delta frames until the next keyframe,
  and the relay asks the senders for one.
* `?studio=1` is the studio's page (opened by the capture app's "Vídeo" button on
  `http://127.0.0.1:8080`): camera only, no master audio.

## Talk-only sender (master from a plugin on the same computer)

A second sender role in the same room: handshake `{"v":1,"room":..,"role":"talk","talk":1,...}`.
It sends only voice (type 4) and keepalives, and receives, as `0x00, len u32, payload` frames,
the listeners' voice (type 5, preferred over the master sender's) plus a copy of the master
sender's format (type 2) and audio blocks (type 1), oldest dropped first, to use as its echo
canceller's reference. The master sender (e.g. the DAW plugin) is unaffected; "newest wins"
applies within each role. The capture app holds its mic back 100 ms in this mode, since that
reference arrives after the speakers have played it.

## Registration (studio side)

Studios register with a Lemon Squeezy license key; listeners never do. Built into the relay
(our own code, not the GPL plugin): `go build -ldflags "-X main.lsProductID=… -X main.lsStoreID=…
-X main.buyURL=…"` (CI reads repository variables `LS_PRODUCT_ID`, `LS_STORE_ID`, `BUY_URL`).
Without a product id, registration is off. Keys must belong to our store/product (and match the
purchase e-mail); validated daily, 30 days offline grace. `/status` carries `licensing`,
`licensed`, `buyUrl`; `/conta`, `/license`, `/license/activate`, `/license/deactivate` answer only
requests from this computer that did not come through the tunnel (which also arrives from
127.0.0.1, so proxy headers decide), and so does `/quit`.

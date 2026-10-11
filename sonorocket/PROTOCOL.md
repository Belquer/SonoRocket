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

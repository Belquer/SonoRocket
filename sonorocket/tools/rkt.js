// Shared helpers for the SonoRocket test tools (same framing as the plugin).
'use strict';

const CRC_TABLE = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
function crc32(u8) { let c = 0xFFFFFFFF; for (let i = 0; i < u8.length; i++) c = CRC_TABLE[(c ^ u8[i]) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }

function audioFrame(seq, pcm /* Float32Array interleaved */, ch) {
  const bytes = Buffer.from(pcm.buffer, pcm.byteOffset, pcm.byteLength);
  const payload = Buffer.alloc(13 + bytes.length);
  payload[0] = 1;
  payload.writeUInt32LE(seq >>> 0, 1);
  payload.writeUInt32LE(pcm.length / ch, 5);
  payload.writeUInt32LE(crc32(bytes), 9);
  bytes.copy(payload, 13);
  const frame = Buffer.alloc(4 + payload.length);
  frame.writeUInt32LE(payload.length, 0);
  payload.copy(frame, 4);
  return frame;
}

// Deterministic "nasty" test material: full-scale noise with overs, denormals,
// tiny values below the 24-bit floor, and exact zeros. Any codec, dither,
// resampler or clipper anywhere in the chain changes at least one of these.
function* testSignal(ch, seed = 0x5eed) {
  let s = seed >>> 0, i = 0;
  const rnd = () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
  for (;;) {
    for (let c = 0; c < ch; c++) {
      const k = i % 1000;
      let v;
      if (k < 5) v = 0;                                  // exact silence
      else if (k < 10) v = (rnd() - 0.5) * 1e-40;        // denormals
      else if (k < 20) v = (rnd() - 0.5) * 1e-9;         // below 24-bit LSB
      else if (k < 30) v = (rnd() < 0.5 ? -1 : 1) * (1 + rnd() * 0.5); // overs up to +3.5 dBFS
      else v = Math.sin(i * 0.031 * (c + 1)) * 0.5 + (rnd() - 0.5) * 0.4;
      yield Math.fround(v);
    }
    i++;
  }
}

module.exports = { crc32, audioFrame, testSignal };

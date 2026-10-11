#!/usr/bin/env node
// Streams a deterministic test signal (or a float/PCM WAV) to a SonoRocket relay,
// in real time, exactly like the plugin does.
// usage: node send-test.js --host 127.0.0.1 --port 9100 --room test --key K [--sr 48000] [--seconds 10] [--wav file.wav]
'use strict';
const net = require('net');
const fs = require('fs');
const { audioFrame, testSignal } = require('./rkt');

const arg = (k, d) => { const i = process.argv.indexOf('--' + k); return i > 0 ? process.argv[i + 1] : d; };
const host = arg('host', '127.0.0.1'), port = +arg('port', 9100);
const room = arg('room', 'test'), key = arg('key', '');
let sr = +arg('sr', 48000), ch = 2;
const seconds = +arg('seconds', 10);
const BLOCK = 1024;

let source; // () => Float32Array of BLOCK*ch or null at end
const wav = arg('wav');
if (wav) {
  const b = fs.readFileSync(wav);
  let p = 12, fmt = null, data = null;
  while (p < b.length) { const id = b.toString('ascii', p, p + 4), sz = b.readUInt32LE(p + 4); if (id === 'fmt ') fmt = b.subarray(p + 8, p + 8 + sz); if (id === 'data') data = b.subarray(p + 8, p + 8 + sz); p += 8 + sz + (sz & 1); }
  const tag = fmt.readUInt16LE(0), bits = fmt.readUInt16LE(14);
  ch = fmt.readUInt16LE(2); sr = fmt.readUInt32LE(4);
  const n = data.length / (bits / 8);
  const all = new Float32Array(n);
  for (let i = 0; i < n; i++) all[i] = tag === 3 ? data.readFloatLE(i * 4) : bits === 24 ? data.readIntLE(i * 3, 3) / 8388608 : data.readInt16LE(i * 2) / 32768;
  let off = 0;
  source = () => { if (off >= all.length) return null; const s = all.slice(off, off + BLOCK * ch); off += BLOCK * ch; return s; };
} else {
  const g = testSignal(ch);
  let sent = 0;
  source = () => { if (sent >= seconds * sr) return null; const s = new Float32Array(BLOCK * ch); for (let i = 0; i < s.length; i++) s[i] = g.next().value; sent += BLOCK; return s; };
}

const sock = net.connect(port, host, () => sock.write(JSON.stringify({ v: 1, room, key, sr, ch, fmt: 'f32' }) + '\n'));
sock.setNoDelay(true);
let ready = false, seq = 0;
sock.on('data', (d) => {
  if (ready) return;
  const line = d.toString().trim();
  if (!line.startsWith('OK')) { console.error('relay said:', line); process.exit(1); }
  ready = true;
  console.log(`streaming ${sr} Hz ${ch}ch to ${host}:${port} room=${room}`);
  const t0 = Date.now(); let frames = 0;
  const tick = () => {
    // send whatever is due by now (real-time pacing, like an audio callback)
    while (frames <= (Date.now() - t0) / 1000 * sr) {
      const s = source();
      if (!s) { console.log(`done, ${seq} blocks`); sock.end(); return; }
      sock.write(audioFrame(seq++, s, ch));
      frames += s.length / ch;
    }
    setTimeout(tick, 5);
  };
  tick();
});
sock.on('error', (e) => { console.error(e.message); process.exit(1); });

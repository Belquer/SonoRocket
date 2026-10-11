#!/usr/bin/env node
// Headless listener: joins a room like a browser does and checks every sample
// against the deterministic test signal, bit for bit.
// usage: node listen-verify.js --url ws://127.0.0.1:8080 --room test --frames 480000
'use strict';
const WebSocket = require('ws');
const { crc32, testSignal } = require('./rkt');

const arg = (k, d) => { const i = process.argv.indexOf('--' + k); return i > 0 ? process.argv[i + 1] : d; };
const url = arg('url', 'ws://127.0.0.1:8080'), room = arg('room', 'test');
const expectFrames = +arg('frames', 0);
const refPath = arg('ref'); // compare against a raw float32 file instead of the JS generator
let ref = null, refOff = 0;

let ch = 2, gen = null, frames = 0, blocks = 0, crcBad = 0, mismatches = 0, gaps = 0, lastSeq = null;
const exp = new Float32Array(1), expU = new Uint32Array(exp.buffer);

const ws = new WebSocket(`${url}/ws?room=${room}`);
ws.on('message', (data, isBinary) => {
  if (!isBinary) {
    const m = JSON.parse(data.toString());
    if (m.type === 'format') { ch = m.ch; gen = testSignal(ch); console.log('format', m); }
    return;
  }
  const b = Buffer.from(data);
  if (b[0] !== 1) return;
  const seq = b.readUInt32LE(1), n = b.readUInt32LE(5), crc = b.readUInt32LE(9);
  const pcm = b.subarray(13);
  if (crc32(pcm) !== crc) crcBad++;
  if (lastSeq !== null && seq !== lastSeq + 1) gaps++;
  if (lastSeq === null && seq !== 0) { console.error(`joined mid-stream at seq ${seq}; start the listener first`); process.exit(2); }
  lastSeq = seq;
  if (refPath) {
    if (!ref) ref = require('fs').readFileSync(refPath);   // read late: the sender writes it as it goes
    const want = ref.subarray(refOff, refOff + pcm.length);
    if (want.length < pcm.length) ref = require('fs').readFileSync(refPath);
    for (let i = 0; i < pcm.length; i += 4) if (pcm.readUInt32LE(i) !== ref.readUInt32LE(refOff + i)) mismatches++;
    refOff += pcm.length;
  } else {
    for (let i = 0; i < n * ch; i++) {
      exp[0] = gen.next().value;
      if (pcm.readUInt32LE(i * 4) !== expU[0]) mismatches++;   // compare raw bits, not values
    }
  }
  frames += n; blocks++;
  if (expectFrames && frames >= expectFrames) finish();
});
ws.on('close', finish);
function finish() {
  const ok = crcBad === 0 && mismatches === 0 && gaps === 0 && frames > 0;
  console.log(JSON.stringify({ frames, blocks, crcBad, gaps, sampleMismatches: mismatches, bitExact: ok }));
  process.exit(ok ? 0 : 1);
}

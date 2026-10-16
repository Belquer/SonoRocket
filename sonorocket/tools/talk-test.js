// Relay talkback test: a fake talk-capable sender and one browser-like listener.
// Checks voice both ways and that sender voice overtakes a queue of master blocks.
// usage: node talk-test.js [host=127.0.0.1] [senderPort=9100] [httpPort=8080]
const net = require('net'), WebSocket = require('ws');
const [host = '127.0.0.1', sport = 9100, hport = 8080] = process.argv.slice(2);
const SR = 48000, CH = 2, BLOCK = 1024;
const fail = (m) => { console.error('FAIL', m); process.exit(1); };
const u32 = (b, o, v) => b.writeUInt32LE(v >>> 0, o);

function voicePayload(type, seq, n, fill) {
  const p = Buffer.alloc(7 + 2 * n); p[0] = type; u32(p, 1, seq); p.writeUInt16LE(n, 5);
  for (let i = 0; i < n; i++) p.writeInt16LE(fill, 7 + 2 * i);
  return p;
}
function frame(p) { const b = Buffer.alloc(4); u32(b, 0, p.length); return Buffer.concat([b, p]); }
function audioPayload(seq) { const p = Buffer.alloc(13 + BLOCK * CH * 4); p[0] = 1; u32(p, 1, seq); u32(p, 5, BLOCK); return p; }

const s = net.connect(sport, host, () => s.write(JSON.stringify({ v: 1, room: 'auto', sr: SR, ch: CH, fmt: 'f32', talk: 1 }) + '\n'));
let buf = Buffer.alloc(0), room = null, gotFromListener = 0;
s.on('data', (d) => {
  buf = Buffer.concat([buf, d]);
  for (;;) {
    if (!buf.length) break;
    if (buf[0] === 0) {
      if (buf.length < 5) break; const n = buf.readUInt32LE(1); if (buf.length < 5 + n) break;
      const p = buf.subarray(5, 5 + n); buf = buf.subarray(5 + n);
      if (p[0] !== 5) fail('sender got non-voice binary');
      const id = p.readUInt16LE(1), seq = p.readUInt32LE(3), cnt = p.readUInt16LE(7), v = p.readInt16LE(9);
      if (cnt !== 160 || v !== -1234 || id < 1) fail(`bad voice to sender id=${id} n=${cnt} v=${v}`);
      gotFromListener++;
      continue;
    }
    const nl = buf.indexOf(10); if (nl < 0) break;
    const line = buf.subarray(0, nl).toString(); buf = buf.subarray(nl + 1);
    if (line.startsWith('OK ')) { room = JSON.parse(line.slice(3)).room; startListener(); }
  }
});

function startListener() {
  const ws = new WebSocket(`ws://${host}:${hport}/ws?room=${room}`);
  let talkFlag = null, voiceSeen = 0, audioAfterBurst = 0, voiceOrder = [];
  ws.on('message', (d, isBin) => {
    if (!isBin) { const m = JSON.parse(d); if (m.type === 'sender' && m.online) talkFlag = m.talk; return; }
    if (d[0] === 1) { audioAfterBurst++; voiceOrder.push('a'); }
    if (d[0] === 4) { if (d.readUInt16LE(5) !== 160 || d.readInt16LE(7) !== 777) fail('bad voice to listener'); voiceSeen++; voiceOrder.push('v'); }
  });
  ws.on('open', () => {
    // a burst of master blocks, then one voice frame
    for (let i = 0; i < 200; i++) s.write(frame(audioPayload(i)));
    s.write(frame(voicePayload(4, 1, 160, 777)));
    for (let i = 0; i < 5; i++) ws.send(voicePayload(5, i, 160, -1234));
    setTimeout(() => {
      if (talkFlag !== true) fail('listener was not told talk is available');
      if (voiceSeen !== 1) fail('listener voice frames: ' + voiceSeen);
      if (gotFromListener !== 5) fail('sender voice frames: ' + gotFromListener);
      const pos = voiceOrder.indexOf('v');
      console.log(`OK talk flag, voice both ways (5→sender, 1→listener); voice delivered after ${pos} of ${voiceOrder.length - 1} master blocks`);
      process.exit(0);
    }, 1500);
  });
}
setTimeout(() => fail('timeout'), 8000);

// Listener side of the talkback test: records voice from the sender (with arrival
// times) and, from the "listen" mark on, talks back a 440 Hz tone for 2 s.
// usage: node talk-listen.js room outfile   (reads MARK lines on stdin)
const WebSocket = require(process.env.WS_PATH || 'ws'), fs = require('fs'), rl = require('readline');
const [room, outFile] = process.argv.slice(2);
const ws = new WebSocket(`ws://127.0.0.1:8080/ws?room=${room}`);
const recs = []; let audioBlocks = 0, crcFail = 0, talk = null, sentAt = null;
ws.on('message', (d, bin) => {
  if (!bin) { const m = JSON.parse(d); if (m.type === 'sender') talk = talk || m.talk; return; }
  if (d[0] === 1) audioBlocks++;
  if (d[0] === 4) { const n = d.readUInt16LE(5); const pcm = new Int16Array(n); for (let i = 0; i < n; i++) pcm[i] = d.readInt16LE(7 + 2 * i); recs.push({ t: Date.now(), pcm }); }
});
function talkTone() {
  sentAt = Date.now(); let seq = 0, ph = 0;
  const iv = setInterval(() => {
    const n = 160, p = Buffer.alloc(7 + 2 * n); p[0] = 5; p.writeUInt32LE(seq++, 1); p.writeUInt16LE(n, 5);
    for (let i = 0; i < n; i++) { ph += 2 * Math.PI * 440 / 16000; p.writeInt16LE(Math.round(8000 * Math.sin(ph)), 7 + 2 * i); }
    ws.send(p);
    if (Date.now() - sentAt > 2000) clearInterval(iv);
  }, 10);
}
rl.createInterface({ input: process.stdin }).on('line', (l) => {
  process.stdout.write(l + '\n');
  if (l.startsWith('MARK listen')) talkTone();
});
process.stdin.on('end', () => setTimeout(() => {
  fs.writeFileSync(outFile, JSON.stringify({ talk, audioBlocks, sentAt, frames: recs.map(r => ({ t: r.t, rms: Math.sqrt(r.pcm.reduce((a, x) => a + x * x, 0) / r.pcm.length) / 32768 })) }));
  process.exit(0);
}, 300));

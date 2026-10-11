// SonoRocket relay: TCP in from the DAW plugin, WebSocket out to any browser.
// Audio payloads are forwarded byte-for-byte. See ../PROTOCOL.md.
'use strict';

const net = require('net');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

const SENDER_PORT = +process.env.SENDER_PORT || 9100;
const HTTP_PORT = +process.env.HTTP_PORT || 8080;
const RELAY_KEY = process.env.RELAY_KEY || '';           // empty = open (local use)
const BACKLOG_SEC = +process.env.BACKLOG_SEC || 4;        // prefill for late joiners
const MAX_LISTENER_QUEUE = 64 * 1024 * 1024;             // bytes; beyond this the listener is too slow
const PUBLIC = path.join(__dirname, 'public');

const rooms = new Map(); // name -> { format, sender, listeners:Set, backlog:[], backlogFrames }

function room(name) {
  let r = rooms.get(name);
  if (!r) {
    r = { format: null, sender: null, listeners: new Set(), backlog: [], backlogFrames: 0 };
    rooms.set(name, r);
  }
  return r;
}

function broadcastText(r, obj) {
  const s = JSON.stringify(obj);
  for (const ws of r.listeners) if (ws.readyState === 1) ws.send(s);
}

function log(...a) { console.log(new Date().toISOString(), ...a); }

// ---------- sender side (TCP) ----------
const tcp = net.createServer((sock) => {
  sock.setNoDelay(true);
  let buf = Buffer.alloc(0);
  let r = null, roomName = null;
  const peer = `${sock.remoteAddress}:${sock.remotePort}`;

  function setFormat(fmt) {
    if (fmt.fmt !== 'f32' || !(fmt.sr > 0) || !(fmt.ch > 0 && fmt.ch <= 2)) throw new Error('bad format');
    r.format = { sr: fmt.sr | 0, ch: fmt.ch | 0, fmt: 'f32' };
    r.backlog = []; r.backlogFrames = 0;           // old blocks no longer match
    broadcastText(r, { type: 'format', ...r.format });
    log(`[${roomName}] format ${r.format.sr} Hz, ${r.format.ch} ch`);
  }

  function onPayload(p) {
    const type = p[0];
    if (type === 1) {
      if (!r.format || p.length < 13) return;
      const nframes = p.readUInt32LE(5);
      if (p.length !== 13 + nframes * r.format.ch * 4) { log(`[${roomName}] size mismatch, dropping block`); return; }
      r.backlog.push(p); r.backlogFrames += nframes;
      while (r.backlogFrames - r.backlog[0].readUInt32LE(5) >= BACKLOG_SEC * r.format.sr) {
        r.backlogFrames -= r.backlog.shift().readUInt32LE(5);
      }
      for (const ws of r.listeners) {
        if (ws.readyState !== 1) continue;
        if (ws.bufferedAmount > MAX_LISTENER_QUEUE) { ws.close(4000, 'too slow'); continue; }
        ws.send(p, { binary: true });
      }
    } else if (type === 2) {
      setFormat(JSON.parse(p.subarray(1).toString('utf8')));
    } // type 3 keepalive: nothing to do
  }

  sock.on('data', (chunk) => {
    buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;
    try {
      if (!r) {
        const nl = buf.indexOf(10);
        if (nl < 0) { if (buf.length > 4096) throw new Error('no handshake'); return; }
        const hs = JSON.parse(buf.subarray(0, nl).toString('utf8'));
        buf = buf.subarray(nl + 1);
        if (RELAY_KEY && hs.key !== RELAY_KEY) throw new Error('bad key');
        if (!/^[A-Za-z0-9_-]{3,64}$/.test(hs.room || '')) throw new Error('bad room');
        roomName = hs.room; r = room(roomName);
        if (r.sender) { r.sender.destroy(); }      // newest sender wins (reconnects)
        r.sender = sock;
        setFormat(hs);
        broadcastText(r, { type: 'sender', online: true });
        sock.write('OK\n');
        log(`[${roomName}] sender connected from ${peer}`);
      }
      while (buf.length >= 4) {
        const len = buf.readUInt32LE(0);
        if (len === 0 || len > 4 * 1024 * 1024) throw new Error('bad frame length');
        if (buf.length < 4 + len) break;
        onPayload(buf.subarray(4, 4 + len));
        buf = buf.subarray(4 + len);
      }
      if (buf.length) buf = Buffer.from(buf);        // detach from big chunk
    } catch (e) {
      log(`sender ${peer}: ${e.message}`);
      try { sock.end(`ERR ${e.message}\n`); } catch {}
      sock.destroy();
    }
  });

  sock.on('close', () => {
    if (r && r.sender === sock) {
      r.sender = null;
      broadcastText(r, { type: 'sender', online: false });
      log(`[${roomName}] sender disconnected`);
    }
  });
  sock.on('error', () => {});
});

// ---------- listener side (HTTP + WebSocket) ----------
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.json': 'application/json' };

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  let p = url.pathname === '/' ? '/index.html' : url.pathname;
  if (p === '/health') { res.end('ok'); return; }
  const file = path.join(PUBLIC, path.normalize(p).replace(/^(\.\.[/\\])+/, ''));
  if (!file.startsWith(PUBLIC)) { res.writeHead(403).end(); return; }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404).end('not found'); return; }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
    res.end(data);
  });
});

const wss = new WebSocketServer({ server, path: '/ws', perMessageDeflate: false });
wss.on('connection', (ws, req) => {
  const name = new URL(req.url, 'http://x').searchParams.get('room') || '';
  if (!/^[A-Za-z0-9_-]{3,64}$/.test(name)) { ws.close(4001, 'bad room'); return; }
  const r = room(name);
  r.listeners.add(ws);
  log(`[${name}] listener joined (${r.listeners.size})`);
  ws.send(JSON.stringify({ type: 'sender', online: !!r.sender }));
  if (r.format) {
    ws.send(JSON.stringify({ type: 'format', ...r.format }));
    for (const p of r.backlog) ws.send(p, { binary: true });
  }
  ws.on('close', () => { r.listeners.delete(ws); log(`[${name}] listener left (${r.listeners.size})`); });
  ws.on('error', () => {});
});

tcp.listen(SENDER_PORT, () => log(`senders: tcp :${SENDER_PORT}${RELAY_KEY ? ' (key required)' : ' (no key)'}`));
server.listen(HTTP_PORT, () => log(`listeners: http://localhost:${HTTP_PORT}/?room=<room>`));

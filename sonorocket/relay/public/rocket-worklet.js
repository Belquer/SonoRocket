// SonoRocket playback worklet.
// Samples come in exactly as sent and go out exactly as received:
// no resampling, no interpolation. Underruns produce silence and a rebuffer,
// never a "concealed" or stretched sample.
// Listener volume: at 0 dB (gain exactly 1) the multiply is skipped, so the
// output stays bit-exact. Any other setting is a deliberate, smoothed gain.

class RocketPlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { channels, capacitySeconds } = options.processorOptions;
    this.ch = channels;
    this.cap = Math.ceil(capacitySeconds * sampleRate);
    this.buf = [];
    for (let c = 0; c < this.ch; c++) this.buf.push(new Float32Array(this.cap));
    this.w = 0; this.r = 0; this.fill = 0;
    this.target = Math.round(2 * sampleRate);
    this.playing = false;
    this.paused = false;
    this.underruns = 0; this.overflows = 0;
    this.peak = new Float32Array(this.ch);
    this.overs = 0;
    this.lastReport = 0;
    this.gain = 1; this.gainTarget = 1;
    this.gainCoef = 1 - Math.exp(-1 / (0.02 * sampleRate)); // ~20 ms glide, no zipper
    this.port.onmessage = (e) => this.onMsg(e.data);
  }

  onMsg(m) {
    if (m.type === 'data') {
      const pcm = m.pcm, ch = this.ch, n = pcm.length / ch;
      if (this.fill + n > this.cap) {
        // Should never happen with sane settings; drop oldest, counted and shown.
        const drop = this.fill + n - this.cap;
        this.r = (this.r + drop) % this.cap; this.fill -= drop; this.overflows++;
      }
      let w = this.w;
      for (let i = 0; i < n; i++) {
        for (let c = 0; c < ch; c++) this.buf[c][w] = pcm[i * ch + c];
        if (++w === this.cap) w = 0;
      }
      this.w = w; this.fill += n;
    } else if (m.type === 'target') {
      this.target = Math.max(128, Math.round(m.seconds * sampleRate));
      this.playing = false; // rebuffer to the new depth
    } else if (m.type === 'flush') {
      this.w = this.r = this.fill = 0; this.playing = false;
    } else if (m.type === 'gain') {
      this.gainTarget = m.value;
    } else if (m.type === 'pause') {
      this.paused = !!m.value;
    }
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    if (!this.playing && !this.paused && this.fill >= this.target) this.playing = true;

    if (this.playing && !this.paused && this.fill >= n) {
      let r = this.r, g = this.gain;
      const gt = this.gainTarget, k = this.gainCoef;
      for (let i = 0; i < n; i++) {
        if (g !== gt) { g += (gt - g) * k; if (Math.abs(gt - g) < 1e-6) g = gt; }
        for (let o = 0; o < out.length; o++) {
          const s = this.buf[Math.min(o, this.ch - 1)][r];
          out[o][i] = g === 1 ? s : s * g;
          const a = s < 0 ? -s : s;
          const c = Math.min(o, this.ch - 1);
          if (a > this.peak[c]) this.peak[c] = a;
          if (a > 1) this.overs++;
        }
        if (++r === this.cap) r = 0;
      }
      this.r = r; this.fill -= n; this.gain = g;
    } else {
      for (let o = 0; o < out.length; o++) out[o].fill(0);
      if (this.playing && !this.paused) { this.playing = false; this.underruns++; }
    }

    if (currentTime - this.lastReport > 0.1) {
      this.lastReport = currentTime;
      this.port.postMessage({
        fill: this.fill, target: this.target, playing: this.playing,
        underruns: this.underruns, overflows: this.overflows,
        peak: Array.from(this.peak), overs: this.overs,
      });
      this.peak.fill(0);
    }
    return true;
  }
}

registerProcessor('rocket-player', RocketPlayer);

// ---------------------------------------------------------------- talkback
// Voice is a separate, low-latency path: 16 kHz mono int16 in ~10 ms frames.
// It never touches the master buffer above. Late voice is dropped, not delayed.
const VOICE_RATE = 16000;

// The sender's talkback voice → speakers. Adaptive jitter buffer: starts at
// 80 ms, grows 40 ms on every underrun (up to 300 ms), creeps back 20 ms after
// 15 s without one; more than 200 ms over target is dropped, not delayed.
// Fades in after a rebuffer and out on an underrun, so gaps never click.
class RocketVoice extends AudioWorkletProcessor {
  constructor() {
    super();
    this.q = new Float32Array(VOICE_RATE);   // 1 s ring
    this.r = 0; this.n = 0; this.pos = 0; this.playing = false;
    this.target = 1280; this.lastUnder = 0; this.fadeIn = 0;
    this.gain = 1; this.peak = 0; this.last = 0;
    this.step = VOICE_RATE / sampleRate;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.type === 'voice') this.push(m.pcm);
      else if (m.type === 'gain') this.gain = m.value;
    };
  }
  push(pcm) {
    const cap = this.q.length;
    for (let i = 0; i < pcm.length; i++) {
      if (this.n === cap) { this.r = (this.r + 1) % cap; this.n--; }
      this.q[(this.r + this.n) % cap] = pcm[i] / 32768; this.n++;
    }
    if (this.lastUnder && currentTime - this.lastUnder > 15 && this.target > 960) { this.target -= 320; this.lastUnder = currentTime; }
    if (this.n > this.target + 3200) { const drop = this.n - this.target; this.r = (this.r + drop) % cap; this.n -= drop; this.pos = 0; }
    if (!this.playing && this.n >= this.target) { this.playing = true; this.fadeIn = 0; }
  }
  process(_in, outputs) {
    const out = outputs[0], len = out[0].length, cap = this.q.length;
    let pk = 0;
    if (this.playing) {
      for (let i = 0; i < len; i++) {
        if (this.n < 2) {   // underrun: fade the tail we just played, wait longer next time
          this.playing = false; this.target = Math.min(4800, this.target + 640); this.lastUnder = currentTime;
          for (let k = Math.max(0, i - 64), j = 0; k < i; k++, j++) for (let o = 0; o < out.length; o++) out[o][k] *= 1 - j / 64;
          break;
        }
        const a = this.q[this.r], b = this.q[(this.r + 1) % cap];
        const fi = this.fadeIn < 240 ? this.fadeIn++ / 240 : 1;
        const s = (a + (b - a) * this.pos) * this.gain * fi;
        for (let o = 0; o < out.length; o++) out[o][i] = s;
        const abs = s < 0 ? -s : s; if (abs > pk) pk = abs;
        this.pos += this.step;
        while (this.pos >= 1) { this.pos -= 1; this.r = (this.r + 1) % cap; this.n--; }
      }
    }
    if (pk > this.peak) this.peak = pk;
    if (currentTime - this.last > 0.1) { this.last = currentTime; this.port.postMessage({ peak: this.peak }); this.peak = 0; }
    return true;
  }
}
registerProcessor('rocket-voice', RocketVoice);

// Microphone → 16 kHz int16 frames of 160 (10 ms). Anti-alias low-pass, then
// fractional decimation. Frames are only emitted while the gate is open, with
// a 10 ms ramp so opening and closing never click.
class RocketMic extends AudioWorkletProcessor {
  constructor() {
    super();
    this.step = sampleRate / VOICE_RATE;
    this.pos = 0; this.prev = 0;
    // two cascaded biquad low-passes at ~6.5 kHz (Butterworth sections)
    const fc = Math.min(6500, sampleRate * 0.45), w = 2 * Math.PI * fc / sampleRate, cw = Math.cos(w), sw = Math.sin(w);
    this.bq = [0.5412, 1.3066].map((qInv) => {
      const al = sw * qInv / 2, a0 = 1 + al;
      return { b0: (1 - cw) / 2 / a0, b1: (1 - cw) / a0, b2: (1 - cw) / 2 / a0, a1: -2 * cw / a0, a2: (1 - al) / a0, x1: 0, x2: 0, y1: 0, y2: 0 };
    });
    this.frame = new Int16Array(160); this.fi = 0;
    this.open = false; this.g = 0; this.active = false; this.peak = 0; this.last = 0;
    this.port.onmessage = (e) => { if (e.data.type === 'open') this.open = !!e.data.value; };
  }
  lp(x) {
    for (const f of this.bq) {
      const y = f.b0 * x + f.b1 * f.x1 + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2;
      f.x2 = f.x1; f.x1 = x; f.y2 = f.y1; f.y1 = y; x = y;
    }
    return x;
  }
  process(inputs) {
    const inp = inputs[0] && inputs[0][0];
    if (!inp) return true;
    for (let i = 0; i < inp.length; i++) {
      const x = this.lp(inp[i]);
      const a = x < 0 ? -x : x; if (a > this.peak) this.peak = a;
      this.pos += 1;
      while (this.pos >= this.step) {   // emit a 16 kHz sample between prev and x
        this.pos -= this.step;
        const frac = 1 - this.pos;   // where the 16 kHz instant falls between prev (0) and x (1)
        const s = this.prev + (x - this.prev) * frac;
        const target = this.open ? 1 : 0;
        this.g += Math.max(-1 / 160, Math.min(1 / 160, target - this.g));
        if (this.g > 0) this.active = true;
        this.frame[this.fi++] = Math.max(-32768, Math.min(32767, Math.round(s * this.g * 32767)));
        if (this.fi === 160) {
          this.fi = 0;
          if (this.active) this.port.postMessage({ pcm: this.frame.slice() });
          this.active = this.g > 0;
        }
      }
      this.prev = x;
    }
    if (currentTime - this.last > 0.1) { this.last = currentTime; this.port.postMessage({ peak: this.peak }); this.peak = 0; }
    return true;
  }
}
registerProcessor('rocket-mic', RocketMic);

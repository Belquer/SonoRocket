// SonoRocket playback worklet.
// Samples come in exactly as sent and go out exactly as received:
// no resampling, no interpolation, no gain. Underruns produce silence and
// a rebuffer, never a "concealed" or stretched sample.

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
    } else if (m.type === 'pause') {
      this.paused = !!m.value;
    }
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    if (!this.playing && !this.paused && this.fill >= this.target) this.playing = true;

    if (this.playing && !this.paused && this.fill >= n) {
      let r = this.r;
      for (let i = 0; i < n; i++) {
        for (let o = 0; o < out.length; o++) {
          const s = this.buf[Math.min(o, this.ch - 1)][r];
          out[o][i] = s;
          const a = s < 0 ? -s : s;
          const c = Math.min(o, this.ch - 1);
          if (a > this.peak[c]) this.peak[c] = a;
          if (a > 1) this.overs++;
        }
        if (++r === this.cap) r = 0;
      }
      this.r = r; this.fill -= n;
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

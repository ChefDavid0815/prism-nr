// Makes a reduced-resolution copy of a prepared reference sequence for the NR bench: the network's
// working resolution study. Proxy: area average in linear light, re-encoded; motion: centre sample
// scaled with the resolution; depth: centre sample.
//   node tools/ghost/scale.cjs <prepared dir> <scale, e.g. 0.75>   -> <dir>/ref<pct>/
const fs = require('fs');
const path = require('path');
const [dir, sa] = process.argv.slice(2);
const s = Number(sa);
const crop = JSON.parse(fs.readFileSync(path.join(dir, 'crop.json'), 'utf8'));
const W = crop.cw, H = crop.ch, N = crop.frames, w = Math.round(W * s), h = Math.round(H * s);
const out = path.join(dir, `ref${Math.round(s * 100)}`);
fs.mkdirSync(out, { recursive: true });
const rd = (f) => new Float32Array(fs.readFileSync(f).buffer.slice(0));
const dec = (c) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
const enc = (v) => (v <= 0.0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - 0.055);
for (let t = 0; t < N; t++) {
  const c = rd(path.join(dir, 'ref', `c_${t}.f32`)), m = rd(path.join(dir, 'ref', `m_${t}.f32`)), d = rd(path.join(dir, 'ref', `d_${t}.f32`));
  const co = new Float32Array(w * h * 4), mo = new Float32Array(w * h * 2), dout = new Float32Array(w * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    // Footprint of this low-res pixel in full-res pixels.
    const x0 = x / s, x1 = (x + 1) / s, y0 = y / s, y1 = (y + 1) / s;
    const acc = [0, 0, 0]; let wsum = 0;
    for (let yy = Math.floor(y0); yy < Math.ceil(y1); yy++) for (let xx = Math.floor(x0); xx < Math.ceil(x1); xx++) {
      const wx = Math.min(xx + 1, x1) - Math.max(xx, x0), wy = Math.min(yy + 1, y1) - Math.max(yy, y0), wt = wx * wy;
      if (wt <= 0 || xx >= W || yy >= H) continue;
      const i = (yy * W + xx) * 4;
      for (let k = 0; k < 3; k++) acc[k] += dec(c[i + k]) * wt;
      wsum += wt;
    }
    const o = (y * w + x) * 4;
    for (let k = 0; k < 3; k++) co[o + k] = enc(acc[k] / wsum);
    co[o + 3] = 1;
    const cx = Math.min(W - 1, Math.floor((x + 0.5) / s)), cy = Math.min(H - 1, Math.floor((y + 0.5) / s)), ci = cy * W + cx;
    mo[(y * w + x) * 2] = m[ci * 2] * s; mo[(y * w + x) * 2 + 1] = m[ci * 2 + 1] * s;
    dout[y * w + x] = d[ci];
  }
  fs.writeFileSync(path.join(out, `c_${t}.f32`), Buffer.from(co.buffer));
  fs.writeFileSync(path.join(out, `m_${t}.f32`), Buffer.from(mo.buffer));
  fs.writeFileSync(path.join(out, `d_${t}.f32`), Buffer.from(dout.buffer));
}
fs.writeFileSync(path.join(out, 'meta.txt'), `${w} ${h} ${N} ${crop.inverted}\n`);
console.log(`wrote ${N} frames at ${w}x${h} to ${out}`);

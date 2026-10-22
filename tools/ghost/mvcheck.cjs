// Checks a prepared sequence's motion vectors: last frame's brightness fetched along the vectors
// should match this frame far better than the same pixel unmoved, and better than the vectors with
// either axis flipped. Prints the mean |log2 brightness| difference on moving pixels.
//   node tools/ghost/mvcheck.cjs <prepared dir>
const fs = require('fs');
const path = require('path');
const dir = process.argv[2];
const crop = JSON.parse(fs.readFileSync(path.join(dir, 'crop.json'), 'utf8'));
const W = crop.cw, H = crop.ch, N = crop.frames;
const rd = (f) => new Float32Array(fs.readFileSync(f).buffer.slice(0));
const dec = (c) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
const lum = (t) => {
  const c = rd(path.join(dir, 'ref', `c_${t}.f32`)), Y = new Float32Array(W * H);
  for (let i = 0; i < W * H; i++) Y[i] = Math.log2(Math.max(1e-4, 0.2126 * dec(c[i * 4]) + 0.7152 * dec(c[i * 4 + 1]) + 0.0722 * dec(c[i * 4 + 2])));
  return Y;
};
const sample = (Y, x, y) => {
  x = Math.min(W - 1.001, Math.max(0, x)); y = Math.min(H - 1.001, Math.max(0, y));
  const bx = Math.floor(x), by = Math.floor(y), fx = x - bx, fy = y - by, i = by * W + bx;
  return Y[i] * (1 - fx) * (1 - fy) + Y[i + 1] * fx * (1 - fy) + Y[i + W] * (1 - fx) * fy + Y[i + W + 1] * fx * fy;
};
const variants = { unmoved: [0, 0], vectors: [1, 1], 'flip x': [-1, 1], 'flip y': [1, -1], negated: [-1, -1], 'x2': [2, 2], 'x0.5': [0.5, 0.5], 'neg x0.5': [-0.5, -0.5], 'x0.25': [0.25, 0.25] };
const acc = Object.fromEntries(Object.keys(variants).map((k) => [k, [0, 0]]));
let prev = lum(0), meanMv = 0, nMv = 0;
for (let t = 1; t < Math.min(N, 24); t++) {
  const Y = lum(t), mv = rd(path.join(dir, 'ref', `m_${t}.f32`));
  for (let y = 24; y < H - 24; y += 2) for (let x = 24; x < W - 24; x += 2) {
    const i = y * W + x, mx = mv[i * 2], my = mv[i * 2 + 1];
    if (Math.hypot(mx, my) <= 1.5) continue;
    meanMv += Math.hypot(mx, my); nMv++;
    for (const [k, [sx, sy]] of Object.entries(variants)) { acc[k][0] += Math.abs(Y[i] - sample(prev, x + mx * sx, y + my * sy)); acc[k][1]++; }
  }
  prev = Y;
}
console.log(`moving samples ${nMv}, mean |mv| ${(meanMv / nMv).toFixed(2)} px`);
for (const [k, [s, n]] of Object.entries(acc)) console.log(k.padEnd(10), (s / n).toFixed(4));

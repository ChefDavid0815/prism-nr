// Per-pixel probe of a prepared sequence at one frame: motion, depth and the reference edit along a
// vertical line, to see what a stubborn error band is made of.
//   node tools/ghost/probe.cjs <prepared dir> <frame> <x> <y0> <y1> [step]
const fs = require('fs');
const path = require('path');
const [dir, ts, xs, y0s, y1s, st] = process.argv.slice(2);
const crop = JSON.parse(fs.readFileSync(path.join(dir, 'crop.json'), 'utf8'));
const W = crop.cw, t = +ts, x = +xs, step = +(st || 4);
const rd = (f) => new Float32Array(fs.readFileSync(f).buffer.slice(0));
const dec = (c) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
const c = rd(path.join(dir, 'ref', `c_${t}.f32`)), o = rd(path.join(dir, 'ref', `o_${t}.f32`)), mv = rd(path.join(dir, 'ref', `m_${t}.f32`)), d = rd(path.join(dir, 'ref', `d_${t}.f32`));
const cp = rd(path.join(dir, 'ref', `c_${t - 1}.f32`)), op = rd(path.join(dir, 'ref', `o_${t - 1}.f32`));
const Y = (a, i) => Math.log2(Math.max(1e-4, 0.2126 * dec(a[i * 4]) + 0.7152 * dec(a[i * 4 + 1]) + 0.0722 * dec(a[i * 4 + 2])));
const E = (cc, oo, i) => Math.log2(Math.max(dec(Math.max(0, oo[i * 4 + 1])), 1 / 512) / Math.max(dec(cc[i * 4 + 1]), 1 / 512));
console.log('   y   depth      mv(x,y)      Y      refEdit(g)  refEdit at mv-source in t-1  Y at source');
for (let y = +y0s; y <= +y1s; y += step) {
  const i = y * W + x, sx = Math.round(x + mv[i * 2]), sy = Math.round(y + mv[i * 2 + 1]), si = Math.min(crop.ch - 1, Math.max(0, sy)) * W + Math.min(W - 1, Math.max(0, sx));
  console.log(`${String(y).padStart(4)}  ${d[i].toExponential(3)}  (${mv[i * 2].toFixed(1).padStart(6)},${mv[i * 2 + 1].toFixed(1).padStart(6)})  ${Y(c, i).toFixed(2).padStart(6)}  ${E(c, o, i).toFixed(3).padStart(8)}     ${E(cp, op, si).toFixed(3).padStart(8)} (src ${sx},${sy} d ${d[si].toExponential(2)})  ${Y(cp, si).toFixed(2)}`);
}

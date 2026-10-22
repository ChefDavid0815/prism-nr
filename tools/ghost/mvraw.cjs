// Raw motion-vector statistics of a Prism Core recording (RG16F at render resolution): the
// distribution of each channel over a few regions, before any scale is applied.
//   node tools/ghost/mvraw.cjs <record dir> [frame]
const fs = require('fs');
const path = require('path');
const [dir, fa] = process.argv.slice(2);
const t = Number(fa || 10);
const m = fs.readFileSync(path.join(dir, 'meta.txt'), 'utf8').trim().split(/\r?\n/)[t].split(/\s+/);
const rw = +m[7], rh = +m[8], pitch = +m[9], sx = +m[15], sy = +m[16];
const raw = fs.readFileSync(path.join(dir, `motion_${t}.raw`));
const half = (h) => { const s = h & 0x8000 ? -1 : 1, e = (h >> 10) & 0x1f, f = h & 0x3ff; return e === 0 ? s * f * 2 ** -24 : e === 31 ? NaN : s * (1 + f / 1024) * 2 ** (e - 15); };
const at = (x, y) => [half(raw.readUInt16LE(y * pitch + x * 4)), half(raw.readUInt16LE(y * pitch + x * 4 + 2))];
console.log(`frame ${t}: render ${rw}x${rh}, mvscale ${sx} ${sy}`);
const regions = { 'player car (centre bottom)': [0.42, 0.62, 0.58, 0.82], 'left buildings': [0.02, 0.25, 0.15, 0.45], 'road ahead': [0.45, 0.48, 0.55, 0.55], 'sky/top': [0.4, 0.05, 0.6, 0.12] };
for (const [name, [x0, y0, x1, y1]] of Object.entries(regions)) {
  const xs = [], ys = [];
  for (let y = Math.floor(y0 * rh); y < y1 * rh; y += 3) for (let x = Math.floor(x0 * rw); x < x1 * rw; x += 3) { const [a, b] = at(x, y); xs.push(a); ys.push(b); }
  const med = (v) => v.slice().sort((p, q) => p - q)[v.length >> 1];
  const mx = med(xs), my = med(ys);
  console.log(`${name.padEnd(28)} raw median (${mx.toExponential(3)}, ${my.toExponential(3)})  x scale -> (${(mx * sx).toFixed(2)}, ${(my * sy).toFixed(2)}) render px`);
}

// Ground truth for motion vectors: for patches of a prepared sequence, the displacement into the
// previous frame that matches best (brute force), next to what the motion vectors claim there.
//   node tools/ghost/blockmatch.cjs <prepared dir> [frame] [range]
const fs = require('fs');
const path = require('path');
const [dir, fa, ra] = process.argv.slice(2);
const crop = JSON.parse(fs.readFileSync(path.join(dir, 'crop.json'), 'utf8'));
const W = crop.cw, H = crop.ch, t = Number(fa || 12), R = Number(ra || 160);
const rd = (f) => new Float32Array(fs.readFileSync(f).buffer.slice(0));
const lum = (k) => { const c = rd(path.join(dir, 'ref', `c_${k}.f32`)), Y = new Float32Array(W * H); for (let i = 0; i < W * H; i++) Y[i] = 0.2126 * c[i * 4] + 0.7152 * c[i * 4 + 1] + 0.0722 * c[i * 4 + 2]; return Y; };
const cur = lum(t), prev = lum(t - 1), mv = rd(path.join(dir, 'ref', `m_${t}.f32`)), d = rd(path.join(dir, 'ref', `d_${t}.f32`));
const B = 12;
console.log(' patch(x,y)   depth      mv claims (dx,dy)   best match (dx,dy)  err@mv  err@best err@0');
for (let py = 80; py < H - 80; py += 110) for (let px = 80; px < W - 80; px += 140) {
  const sad = (dx, dy) => { let s = 0; for (let y = -B; y <= B; y += 2) for (let x = -B; x <= B; x += 2) { const xx = px + x + dx, yy = py + y + dy; if (xx < 0 || yy < 0 || xx >= W || yy >= H) return Infinity; s += Math.abs(cur[(py + y) * W + px + x] - prev[yy * W + xx]); } return s; };
  let best = [0, 0], bs = Infinity;
  for (let dy = -R; dy <= R; dy += 2) for (let dx = -R; dx <= R; dx += 2) { const s = sad(dx, dy); if (s < bs) { bs = s; best = [dx, dy]; } }
  const i = py * W + px, m = [Math.round(mv[i * 2]), Math.round(mv[i * 2 + 1])];
  console.log(`(${String(px).padStart(4)},${String(py).padStart(4)}) ${d[i].toExponential(2)}  (${String(m[0]).padStart(5)},${String(m[1]).padStart(5)})       (${String(best[0]).padStart(5)},${String(best[1]).padStart(5)})     ${sad(m[0] - (m[0] % 2), m[1] - (m[1] % 2)).toFixed(1).padStart(6)} ${bs.toFixed(1).padStart(7)} ${sad(0, 0).toFixed(1).padStart(6)}`);
}

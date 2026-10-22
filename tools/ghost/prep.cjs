// Turns a Prism Core recording (prism-record/: the upscaled frame, motion vectors and depth of
// consecutive frames) into NR bench sequences for the ghosting study:
//   ref/        the network on every frame with that frame's motion (the reference answer)
//   c4approx/   every 4th frame, motion = the launch frame's vectors x 4 (constant velocity, 1.1)
//   c4exact/    every 4th frame, motion followed back through the 4 frames' own vectors
// The proxy is made exactly as Prism Core makes it (tile log-luma exposure eased by 0.05,
// hue-preserving shoulder on the max channel, sRGB). A crop keeps the data small.
//   node tools/ghost/prep.cjs <record dir> <out dir> [x y w h]
const fs = require('fs');
const path = require('path');

const [recDir, outDir, ...cropArg] = process.argv.slice(2);
const meta = fs.readFileSync(path.join(recDir, 'meta.txt'), 'utf8').trim().split(/\r?\n/).map((l) => {
  const t = l.split(/\s+/);
  return { i: +t[0], W: +t[2], H: +t[3], cp: +t[4], fmt: +t[5], rw: +t[7], rh: +t[8], mp: +t[9], dp: +t[13], sx: +t[15], sy: +t[16], reset: +t[18], flags: +t[20] };
});
const { W, H, rw, rh } = meta[0];
const [cx, cy, cw, ch] = cropArg.length === 4 ? cropArg.map(Number) : [0, 0, W, H];
const N = Math.min(meta.length, Number(process.env.PREP_FRAMES || meta.length));
const inverted = (meta[0].flags >> 3) & 1;

function half(h) {
  const s = h & 0x8000 ? -1 : 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
  if (e === 0) return s * m * 2 ** -24;
  if (e === 31) return m ? NaN : s * Infinity;
  return s * (1 + m / 1024) * 2 ** (e - 15);
}
const smallF = (bits, mb) => { const e = bits >> mb, m = bits & ((1 << mb) - 1); return e === 0 ? m * 2 ** (-14 - mb) : e === 31 ? 0 : (1 + m / (1 << mb)) * 2 ** (e - 15); };
function loadColor(i) {
  const m = meta[i], raw = fs.readFileSync(path.join(recDir, `color_${i}.raw`)), px = new Float32Array(W * H * 3);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const v = raw.readUInt32LE(y * m.cp + x * 4), o = (y * W + x) * 3;
    px[o] = smallF(v & 0x7ff, 6); px[o + 1] = smallF((v >> 11) & 0x7ff, 6); px[o + 2] = smallF(v >>> 22, 5);
  }
  return px;
}
// Motion at output resolution, in output pixels, previous position minus current (Core's Carry).
function loadMotion(i) {
  const m = meta[i], raw = fs.readFileSync(path.join(recDir, `motion_${i}.raw`)), mv = new Float32Array(W * H * 2);
  const kx = m.sx * (W / rw), ky = m.sy * (H / rh);
  for (let y = 0; y < H; y++) {
    const ry = Math.min(rh - 1, Math.floor((y + 0.5) * rh / H));
    for (let x = 0; x < W; x++) {
      const rx = Math.min(rw - 1, Math.floor((x + 0.5) * rw / W)), b = ry * m.mp + rx * 4;
      mv[(y * W + x) * 2] = half(raw.readUInt16LE(b)) * kx;
      mv[(y * W + x) * 2 + 1] = half(raw.readUInt16LE(b + 2)) * ky;
    }
  }
  return mv;
}
function loadDepth(i) {
  const m = meta[i], raw = fs.readFileSync(path.join(recDir, `depth_${i}.raw`)), d = new Float32Array(W * H);
  for (let y = 0; y < H; y++) {
    const ry = Math.min(rh - 1, Math.floor((y + 0.5) * rh / H));
    for (let x = 0; x < W; x++) d[y * W + x] = raw.readFloatLE(ry * m.dp + Math.min(rw - 1, Math.floor((x + 0.5) * rw / W)) * 4);
  }
  return d;
}

// Core's exposure: mean over 16x16 tiles of mean log2 luma, target log2(0.18) - mean, eased by 0.05.
let expPrev = 0;
function proxyOf(c) {
  const tx = Math.ceil(W / 16), ty = Math.ceil(H / 16);
  let sum = 0;
  for (let j = 0; j < ty; j++) for (let i = 0; i < tx; i++) {
    let s = 0;
    for (let y = j * 16; y < j * 16 + 16; y++) for (let x = i * 16; x < i * 16 + 16; x++) {
      if (x < W && y < H) { const o = (y * W + x) * 3; s += Math.log2(Math.max(0.2126 * c[o] + 0.7152 * c[o + 1] + 0.0722 * c[o + 2], 1e-5)); }
    }
    sum += s / 256;
  }
  const target = Math.log2(0.18) - sum / (tx * ty);
  expPrev = expPrev === 0 ? target : expPrev + (target - expPrev) * 0.05;
  const k = 2 ** expPrev, out = new Float32Array(cw * ch * 4);
  const enc = (v) => (v <= 0.0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - 0.055);
  for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) {
    const o = ((y + cy) * W + x + cx) * 3, r = Math.max(c[o], 0) * k, g = Math.max(c[o + 1], 0) * k, b = Math.max(c[o + 2], 0) * k;
    const m = Math.max(r, g, b), q = (y * cw + x) * 4;
    out[q] = enc(r / (1 + m)); out[q + 1] = enc(g / (1 + m)); out[q + 2] = enc(b / (1 + m)); out[q + 3] = 1;
  }
  return out;
}
const crop2 = (mv) => { const o = new Float32Array(cw * ch * 2); for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) { const s = ((y + cy) * W + x + cx) * 2, d = (y * cw + x) * 2; o[d] = mv[s]; o[d + 1] = mv[s + 1]; } return o; };
const crop1 = (a) => { const o = new Float32Array(cw * ch); for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) o[y * cw + x] = a[(y + cy) * W + x + cx]; return o; };
// Bilinear sample of a full-frame motion field at an output position (pixel-centre space minus 0.5).
function sampleMv(mv, px, py) {
  const x0 = Math.max(0, Math.min(W - 1, Math.floor(px))), y0 = Math.max(0, Math.min(H - 1, Math.floor(py)));
  return [mv[(y0 * W + x0) * 2], mv[(y0 * W + x0) * 2 + 1]];  // nearest: motion fields are piecewise
}

const CADS = (process.env.CADS || '4').split(',').map(Number);
const sets = { ref: [] };
for (const c of CADS) { sets['c' + c + 'approx'] = []; sets['c' + c + 'exact'] = []; }
for (const s of Object.keys(sets)) fs.mkdirSync(path.join(outDir, s), { recursive: true });
const put = (s, k, name, arr) => fs.writeFileSync(path.join(outDir, s, `${name}_${k}.f32`), Buffer.from(arr.buffer));
const recent = [];  // the last motion fields, newest first
const MAXC = Math.max(...CADS);
const count = Object.fromEntries(Object.keys(sets).map((k) => [k, 0]));
for (let t = 0; t < N; t++) {
  const c = loadColor(t), mv = loadMotion(t), d = loadDepth(t);
  recent.unshift(mv);
  if (recent.length > MAXC) recent.pop();
  const px = proxyOf(c), mvc = crop2(mv), dc = crop1(d);
  put('ref', count.ref++, 'c', px); put('ref', count.ref - 1, 'm', mvc); put('ref', count.ref - 1, 'd', dc);
  for (const CAD of CADS) {
  if (t % CAD === 0) {
    const span = Math.min(CAD, t) || 1;
    const approx = new Float32Array(cw * ch * 2), exact = new Float32Array(cw * ch * 2);
    for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) {
      const i = (y * cw + x) * 2;
      approx[i] = mvc[i] * span; approx[i + 1] = mvc[i + 1] * span;
      // Follow back frame by frame: position on t-1 from t's vectors, on t-2 from t-1's, ...
      let pxx = x + cx, pyy = y + cy;
      for (let k = 0; k < Math.min(span, recent.length); k++) { const [dx, dy] = sampleMv(recent[k], pxx, pyy); pxx += dx; pyy += dy; }
      exact[i] = pxx - (x + cx); exact[i + 1] = pyy - (y + cy);
    }
    for (const [s, m] of [['c' + CAD + 'approx', approx], ['c' + CAD + 'exact', exact]]) { put(s, count[s], 'c', px); put(s, count[s], 'm', m); put(s, count[s], 'd', dc); count[s]++; }
  }
  }
  if (t % 10 === 0) console.log(`frame ${t}/${N}`);
}
for (const s of Object.keys(count)) fs.writeFileSync(path.join(outDir, s, 'meta.txt'), `${cw} ${ch} ${count[s]} ${inverted}\n`);
fs.writeFileSync(path.join(outDir, 'crop.json'), JSON.stringify({ W, H, cx, cy, cw, ch, frames: N, inverted }));
console.log('done', count);

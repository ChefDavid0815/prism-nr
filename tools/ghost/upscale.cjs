// The network at a reduced working resolution, its edit brought back to native resolution by
// several upsamplers, scored against the network at native resolution (every frame, no carry).
//   node tools/ghost/upscale.cjs <prepared dir> [pct=75]
// Needs <dir>/ref (native, NR bench outputs) and <dir>/ref<pct> (scale.cjs + NR bench).
// Edits are log2(network / proxy) per channel, clamped to +-3 stops as in Core.
const fs = require('fs');
const path = require('path');
const [dir, pa] = process.argv.slice(2);
const pct = Number(pa || 75), s = pct / 100;
const crop = JSON.parse(fs.readFileSync(path.join(dir, 'crop.json'), 'utf8'));
const W = crop.cw, H = crop.ch, N = crop.frames, INV = crop.inverted, TOL = 0.05;
const w = Math.round(W * s), h = Math.round(H * s);
const rd = (f) => new Float32Array(fs.readFileSync(f).buffer.slice(0));
const dec = (c) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
const lum = (r, g, b) => Math.log2(Math.max(1e-4, 0.2126 * r + 0.7152 * g + 0.0722 * b));
function edits(sub, ww, hh, t) {
  const c = rd(path.join(dir, sub, `c_${t}.f32`)), o = rd(path.join(dir, sub, `o_${t}.f32`)), d = rd(path.join(dir, sub, `d_${t}.f32`));
  const E = new Float32Array(ww * hh * 3), Y = new Float32Array(ww * hh);
  for (let i = 0; i < ww * hh; i++) {
    const p = [dec(c[i * 4]), dec(c[i * 4 + 1]), dec(c[i * 4 + 2])];
    for (let k = 0; k < 3; k++) E[i * 3 + k] = Math.max(-3, Math.min(3, Math.log2(Math.max(dec(Math.max(0, o[i * 4 + k])), 1 / 512) / Math.max(p[k], 1 / 512))));
    Y[i] = lum(p[0], p[1], p[2]);
  }
  return { E, Y, d, mv: sub === 'ref' ? rd(path.join(dir, sub, `m_${t}.f32`)) : null };
}

// Upsamplers: low-res edit -> native. Taps over a 4x4 low-res window around the native pixel's
// position; spatial weight from the bilinear/gaussian distance, optional range weights.
function upsample(lo, hi, { sigmaS = 0, sigmaY = 0, depth = false }) {
  const out = new Float32Array(W * H * 3);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const fx = (x + 0.5) * s - 0.5, fy = (y + 0.5) * s - 0.5, i = y * W + x;
    let ws = 0, a0 = 0, a1 = 0, a2 = 0;
    const bx = Math.floor(fx), by = Math.floor(fy);
    for (let j = (sigmaS ? -1 : 0); j <= (sigmaS ? 2 : 1); j++) for (let k = (sigmaS ? -1 : 0); k <= (sigmaS ? 2 : 1); k++) {
      const tx = Math.min(w - 1, Math.max(0, bx + k)), ty = Math.min(h - 1, Math.max(0, by + j)), q = ty * w + tx;
      const dx = fx - (bx + k), dy = fy - (by + j);
      let wt = sigmaS ? Math.exp(-(dx * dx + dy * dy) / (2 * sigmaS * sigmaS)) : (1 - Math.abs(dx)) * (1 - Math.abs(dy));
      if (sigmaY) { const r = (hi.Y[i] - lo.Y[q]) / sigmaY; wt *= Math.exp(-0.5 * r * r); }
      if (depth) { const dd = hi.d[i], r = (dd - lo.d[q]) / (TOL * Math.max(Math.abs(dd), 1e-4)); wt *= Math.exp(-0.5 * r * r); }
      ws += wt; a0 += lo.E[q * 3] * wt; a1 += lo.E[q * 3 + 1] * wt; a2 += lo.E[q * 3 + 2] * wt;
    }
    if (ws < 1e-6) { // nothing similar nearby: plain bilinear
      upsampleFallback(lo, out, x, y, fx, fy, i);
      continue;
    }
    out[i * 3] = a0 / ws; out[i * 3 + 1] = a1 / ws; out[i * 3 + 2] = a2 / ws;
  }
  return out;
}
function upsampleFallback(lo, out, x, y, fx, fy, i) {
  const bx = Math.floor(fx), by = Math.floor(fy);
  let ws = 0; const a = [0, 0, 0];
  for (let j = 0; j <= 1; j++) for (let k = 0; k <= 1; k++) {
    const tx = Math.min(w - 1, Math.max(0, bx + k)), ty = Math.min(h - 1, Math.max(0, by + j)), q = ty * w + tx;
    const wt = (1 - Math.abs(fx - (bx + k))) * (1 - Math.abs(fy - (by + j)));
    ws += wt; for (let c = 0; c < 3; c++) a[c] += lo.E[q * 3 + c] * wt;
  }
  for (let c = 0; c < 3; c++) out[i * 3 + c] = a[c] / ws;
  return out;
}

// GAIN=<k>: the reduced-resolution edit is scaled by k before scoring (intensity compensation).
const GAIN = Number(process.env.GAIN || 1);
const methods = {
  'bilinear': { },
  'JBU luma 0.3': { sigmaS: 0.8, sigmaY: 0.3 },
  'JBU luma 0.15': { sigmaS: 0.8, sigmaY: 0.15 },
  'JBU luma 0.3 + depth': { sigmaS: 0.8, sigmaY: 0.3, depth: true },
  'JBU luma 0.15 + depth': { sigmaS: 0.8, sigmaY: 0.15, depth: true },
};
const M = 24;
const acc = {};
const add = (k, a, b) => { acc[k] = acc[k] || { all: [0, 0], edge: [0, 0], flicker: [0, 0] }; return acc[k]; };
let prevHi = null; const prevUp = {};
for (let t = 0; t < N; t++) {
  const hi = edits('ref', W, H, t), lo = edits(`ref${pct}`, w, h, t);
  const ups = { 'no NR (edit 0)': new Float32Array(W * H * 3) };
  for (const [k, opt] of Object.entries(methods)) { ups[k] = upsample(lo, hi, opt); if (GAIN !== 1) for (let q = 0; q < ups[k].length; q++) ups[k][q] = Math.max(-3, Math.min(3, ups[k][q] * GAIN)); }
  for (const [k, e] of Object.entries(ups)) {
    const a = add(k);
    for (let y = M; y < H - M; y += 1) for (let x = M; x < W - M; x += 1) {
      const i = y * W + x;
      let e2 = 0;
      for (let c = 0; c < 3; c++) e2 += (e[i * 3 + c] - hi.E[i * 3 + c]) ** 2;
      e2 /= 3; a.all[0] += e2; a.all[1]++;
      const d = hi.d[i];
      let edge = false;
      for (let j = -4; j <= 4 && !edge; j += 2) for (let kx = -4; kx <= 4; kx += 2) if (Math.abs(hi.d[(y + j) * W + x + kx] - d) > TOL * Math.max(Math.abs(d), 1e-4)) { edge = true; break; }
      if (edge) { a.edge[0] += e2; a.edge[1]++; }
      if (prevHi && prevUp[k]) {
        const px = Math.round(x + hi.mv[i * 2]), py = Math.round(y + hi.mv[i * 2 + 1]);
        if (px < 0 || py < 0 || px >= W || py >= H) continue;
        const jj = py * W + px;
        if (Math.abs(prevHi.d[jj] - d) > TOL * Math.max(Math.abs(d), 1e-4)) continue;
        let f2 = 0;
        for (let c = 0; c < 3; c++) f2 += ((e[i * 3 + c] - prevUp[k][jj * 3 + c]) - (hi.E[i * 3 + c] - prevHi.E[jj * 3 + c])) ** 2;
        a.flicker[0] += f2 / 3; a.flicker[1]++;
      }
    }
    prevUp[k] = e;
  }
  prevHi = hi;
  // SAVE=<frame>: the frame as shown at native, 75% bilinear and 75% JBU, side by side (PNG).
  if (Number(process.env.SAVE) === t) {
    const zlib = require('zlib');
    const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let q = 0; q < 8; q++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
    const crc = (b) => { let c = -1; for (const v of b) c = crcT[(c ^ v) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
    const chunk = (ty, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(ty), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
    const enc = (v) => { v = Math.max(0, Math.min(1, v)); return Math.round(255 * (v <= 0.0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - 0.055)); };
    const cs = rd(path.join(dir, 'ref', `c_${t}.f32`));
    const panels = [null, hi.E, ups['bilinear'], ups['JBU luma 0.15']], PW = W * panels.length;
    const raw = Buffer.alloc((PW * 3 + 1) * H);
    for (let y = 0; y < H; y++) {
      raw[y * (PW * 3 + 1)] = 0;
      panels.forEach((E, p) => { for (let x = 0; x < W; x++) { const i = y * W + x; for (let c = 0; c < 3; c++) raw[y * (PW * 3 + 1) + 1 + ((p * W + x) * 3) + c] = enc(dec(cs[i * 4 + c]) * (E ? 2 ** E[i * 3 + c] : 1)); } });
    }
    const ih = Buffer.alloc(13); ih.writeUInt32BE(PW, 0); ih.writeUInt32BE(H, 4); ih[8] = 8; ih[9] = 2;
    const file = path.join(dir, `up${pct}_f${t}.png`);
    fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw, { level: 4 })), chunk('IEND', Buffer.alloc(0))]));
    console.log('saved', file, '(proxy | native | 75% bilinear | 75% JBU)');
  }
}
const f = (v) => (v[1] ? Math.sqrt(v[0] / v[1]).toFixed(4) : '-');
console.log(`${dir}: network at ${pct}% (${w}x${h}) vs native ${W}x${H}, ${N} frames, every frame`);
for (const [k, a] of Object.entries(acc)) console.log(`${k.padEnd(24)} rmse ${f(a.all)}  edge ${f(a.edge)}  flicker ${f(a.flicker)}`);

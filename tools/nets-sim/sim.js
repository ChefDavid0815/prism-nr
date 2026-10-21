/*
 * NETS reference simulation.
 *
 * Ground truth is a real DLSS 5 edit: the RenoDX run against the original run of the same CONTROL
 * cutscene frame, e = log2(neural / original) per channel. A synthetic camera pans over it and a
 * foreground layer (the face) crosses the frame with its own depth and motion, so every frame has
 * exact motion vectors, depth and a known full-resolution answer.
 *
 * Assumption, stated in the report: the network run at a reduced resolution answers like the
 * full-resolution answer box-filtered to that resolution. Real NR may synthesise different detail
 * at low resolution, so these numbers bound what the reconstruction can recover; they do not
 * measure the network.
 */
const fs = require('node:fs');
const path = require('node:path');
const { ipcRenderer } = require('electron');

const log = (m) => ipcRenderer.send('log', String(m));
// Recorded runs (run-*.mp4 and align/offsets.json); set PRISM_FOOTAGE to their folder.
const DIR = (process.env.PRISM_FOOTAGE || path.join(__dirname, '..', '..', 'footage')).replace(/\\/g, '/') + '/';
const url = (p) => 'file:///' + p.replace(/\\/g, '/');

// Full-resolution frame of the simulation and the larger source the camera moves over.
const W = 960, H = 540, SW = 1280, SH = 720;
const FRAMES = 72;
const SEGMENTS = [
  { name: 'still', from: 0, to: 24, v: [0, 0] },
  { name: 'slow pan', from: 24, to: 48, v: [0.4, 0.15] },
  { name: 'fast pan', from: 48, to: 72, v: [3.0, 1.2] },
];
const EPS = 1 / 512;
const BORDER = 8;

// ------------------------------------------------------------------ sources
async function grab(run, T) {
  const offsets = JSON.parse(fs.readFileSync(DIR + 'align/offsets.json', 'utf8'));
  const v = document.createElement('video');
  v.muted = true; v.src = url(DIR + `run-${run}.mp4`);
  await new Promise((r, j) => { v.onloadeddata = r; v.onerror = () => j(new Error('video ' + run)); });
  await new Promise((r) => { v.onseeked = r; v.currentTime = Math.max(0, T - (offsets[run] ?? 0)); });
  const c = document.createElement('canvas'); c.width = SW; c.height = SH;
  const x = c.getContext('2d', { willReadFrequently: true });
  x.imageSmoothingQuality = 'high';
  x.drawImage(v, 0, 80, 2560, 1440, 0, 0, SW, SH);
  const d = x.getImageData(0, 0, SW, SH).data;
  const out = new Float32Array(SW * SH * 3);
  for (let i = 0, k = 0; i < d.length; i += 4, k += 3) {
    out[k] = srgbDecode(d[i] / 255); out[k + 1] = srgbDecode(d[i + 1] / 255); out[k + 2] = srgbDecode(d[i + 2] / 255);
  }
  return out;
}

const srgbDecode = (c) => (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));
const srgbEncode = (c) => (c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055);
const lum = (a, k) => 0.2126 * a[k] + 0.7152 * a[k + 1] + 0.0722 * a[k + 2];

// Separate runs of a deterministic cutscene still land a few pixels apart: find the integer
// shift of the neural frame that best matches the original's luminance gradients.
function align(o, n) {
  const g = (a) => { const l = new Float32Array(SW * SH); for (let i = 0; i < SW * SH; i++) l[i] = Math.log2(Math.max(lum(a, i * 3), 1e-4)); return l; };
  const lo = g(o), ln = g(n);
  let best = [0, 0], bestErr = Infinity;
  for (let dy = -6; dy <= 6; dy++) for (let dx = -6; dx <= 6; dx++) {
    let err = 0;
    for (let y = 40; y < SH - 40; y += 2) for (let x = 40; x < SW - 40; x += 2) {
      const i = y * SW + x, j = (y + dy) * SW + (x + dx);
      const go = lo[i + 1] - lo[i] + lo[i + SW] - lo[i];
      const gn = ln[j + 1] - ln[j] + ln[j + SW] - ln[j];
      err += Math.abs(go - gn);
    }
    if (err < bestErr) { bestErr = err; best = [dx, dy]; }
  }
  const shifted = new Float32Array(n.length);
  for (let y = 0; y < SH; y++) for (let x = 0; x < SW; x++) {
    const sx = Math.min(SW - 1, Math.max(0, x + best[0])), sy = Math.min(SH - 1, Math.max(0, y + best[1]));
    const i = (y * SW + x) * 3, j = (sy * SW + sx) * 3;
    shifted[i] = n[j]; shifted[i + 1] = n[j + 1]; shifted[i + 2] = n[j + 2];
  }
  return { shifted, shift: best };
}

// Bilinear sample at continuous coordinates (pixel centres at i + 0.5).
function sample(a, w, h, x, y, out, o = 0) {
  x -= 0.5; y -= 0.5;
  const x0 = Math.floor(x), y0 = Math.floor(y), fx = x - x0, fy = y - y0;
  const cx0 = Math.min(w - 1, Math.max(0, x0)), cx1 = Math.min(w - 1, Math.max(0, x0 + 1));
  const cy0 = Math.min(h - 1, Math.max(0, y0)), cy1 = Math.min(h - 1, Math.max(0, y0 + 1));
  const i00 = (cy0 * w + cx0) * 3, i10 = (cy0 * w + cx1) * 3, i01 = (cy1 * w + cx0) * 3, i11 = (cy1 * w + cx1) * 3;
  for (let c = 0; c < 3; c++) {
    out[o + c] = (a[i00 + c] * (1 - fx) + a[i10 + c] * fx) * (1 - fy) + (a[i01 + c] * (1 - fx) + a[i11 + c] * fx) * fy;
  }
}
function sample1(a, w, h, x, y) {
  x -= 0.5; y -= 0.5;
  const x0 = Math.floor(x), y0 = Math.floor(y), fx = x - x0, fy = y - y0;
  const cx0 = Math.min(w - 1, Math.max(0, x0)), cx1 = Math.min(w - 1, Math.max(0, x0 + 1));
  const cy0 = Math.min(h - 1, Math.max(0, y0)), cy1 = Math.min(h - 1, Math.max(0, y0 + 1));
  return (a[cy0 * w + cx0] * (1 - fx) + a[cy0 * w + cx1] * fx) * (1 - fy) + (a[cy1 * w + cx0] * (1 - fx) + a[cy1 * w + cx1] * fx) * fy;
}

// Catmull-Rom history read, clamped to the 2x2 bilinear footprint so it cannot ring. Repeated
// bilinear reads blur the history a little every frame under sub-pixel motion; this is the
// standard TAA answer to that.
function cr(t) {
  const t2 = t * t, t3 = t2 * t;
  return [-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1, -1.5 * t3 + 2 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2];
}
function sampleCatmull(a, w, h, x, y, out) {
  x -= 0.5; y -= 0.5;
  const x0 = Math.floor(x), y0 = Math.floor(y), wx = cr(x - x0), wy = cr(y - y0);
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  out[0] = out[1] = out[2] = 0;
  for (let j = 0; j < 4; j++) {
    const yy = Math.min(h - 1, Math.max(0, y0 - 1 + j));
    for (let i = 0; i < 4; i++) {
      const xx = Math.min(w - 1, Math.max(0, x0 - 1 + i)), k = (yy * w + xx) * 3, wt = wx[i] * wy[j];
      for (let c = 0; c < 3; c++) {
        out[c] += a[k + c] * wt;
        if ((i === 1 || i === 2) && (j === 1 || j === 2)) { lo[c] = Math.min(lo[c], a[k + c]); hi[c] = Math.max(hi[c], a[k + c]); }
      }
    }
  }
  for (let c = 0; c < 3; c++) out[c] = Math.min(hi[c], Math.max(lo[c], out[c]));
}

// Separable box blur of a 3-channel full-resolution field.
function boxBlur3(a, r) {
  const tmp = new Float32Array(a.length), out = new Float32Array(a.length);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let s0 = 0, s1 = 0, s2 = 0, n = 0;
    for (let k = -r; k <= r; k++) { const xx = x + k; if (xx < 0 || xx >= W) continue; const i = (y * W + xx) * 3; s0 += a[i]; s1 += a[i + 1]; s2 += a[i + 2]; n++; }
    const o = (y * W + x) * 3; tmp[o] = s0 / n; tmp[o + 1] = s1 / n; tmp[o + 2] = s2 / n;
  }
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let s0 = 0, s1 = 0, s2 = 0, n = 0;
    for (let k = -r; k <= r; k++) { const yy = y + k; if (yy < 0 || yy >= H) continue; const i = (yy * W + x) * 3; s0 += tmp[i]; s1 += tmp[i + 1]; s2 += tmp[i + 2]; n++; }
    const o = (y * W + x) * 3; out[o] = s0 / n; out[o + 1] = s1 / n; out[o + 2] = s2 / n;
  }
  return out;
}

// ------------------------------------------------------------------ synthetic sequence
const SPRITE = { x: 520, y: 100, w: 280, h: 240 };  // face region in source coordinates
function cameraPath() {
  const c = [[100, 60]];
  for (let t = 1; t < FRAMES; t++) {
    const seg = SEGMENTS.find((s) => t >= s.from && t < s.to);
    c.push([c[t - 1][0] + seg.v[0], c[t - 1][1] + seg.v[1]]);
  }
  return c;
}
const spritePos = (t) => [80 + 2.2 * t, 260 - 0.6 * t];
const inSprite = (u, v) => { const dx = (u - SPRITE.w / 2) / (SPRITE.w / 2), dy = (v - SPRITE.h / 2) / (SPRITE.h / 2); return dx * dx + dy * dy < 1; };

// `back`: how many frames the motion vectors reach back (a tile evaluated every other frame
// gets the motion since its own last evaluation).
function makeFrame(t, cam, srcO, srcN, back = 1) {
  const O = new Float32Array(W * H * 3), N = new Float32Array(W * H * 3), D = new Float32Array(W * H), MV = new Float32Array(W * H * 2);
  const c = cam[t], cp = cam[Math.max(0, t - back)], q = spritePos(t), qp = spritePos(Math.max(0, t - back));
  const tmp = new Float32Array(3);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x, P = [x + 0.5, y + 0.5];
    const u = P[0] - q[0], v = P[1] - q[1];
    if (u >= 0 && v >= 0 && u < SPRITE.w && v < SPRITE.h && inSprite(u, v)) {
      sample(srcO, SW, SH, SPRITE.x + u, SPRITE.y + v, O, i * 3);
      sample(srcN, SW, SH, SPRITE.x + u, SPRITE.y + v, N, i * 3);
      D[i] = 0.3; MV[i * 2] = qp[0] - q[0]; MV[i * 2 + 1] = qp[1] - q[1];
    } else {
      sample(srcO, SW, SH, P[0] + c[0], P[1] + c[1], O, i * 3);
      sample(srcN, SW, SH, P[0] + c[0], P[1] + c[1], N, i * 3);
      D[i] = 0.7; MV[i * 2] = c[0] - cp[0]; MV[i * 2 + 1] = c[1] - cp[1];
    }
  }
  void tmp;
  const E = new Float32Array(W * H * 3), G = new Float32Array(W * H);
  for (let i = 0; i < W * H; i++) {
    for (let ch = 0; ch < 3; ch++) E[i * 3 + ch] = Math.log2(Math.max(N[i * 3 + ch], EPS) / Math.max(O[i * 3 + ch], EPS));
    G[i] = Math.log2(Math.max(lum(O, i * 3), 1e-4));
  }
  return { O, N, D, MV, E, G };
}

// ------------------------------------------------------------------ the network at working resolution
// Box filter at the working resolution with the sampling window shifted by phase (working pixels).
function downsample(f, s, phase) {
  const w = Math.round(W * s), h = Math.round(H * s), k = 4;
  const e = new Float32Array(w * h * 3), g = new Float32Array(w * h);
  const o = new Float32Array(3), n = new Float32Array(3), so = new Float32Array(3), sn = new Float32Array(3);
  for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
    const cx = (i + 0.5 + phase[0]) / s, cy = (j + 0.5 + phase[1]) / s;
    so.fill(0); sn.fill(0);
    for (let b = 0; b < k; b++) for (let a = 0; a < k; a++) {
      const x = cx + ((a + 0.5) / k - 0.5) / s, y = cy + ((b + 0.5) / k - 0.5) / s;
      sample(f.O, W, H, x, y, o); sample(f.N, W, H, x, y, n);
      for (let c = 0; c < 3; c++) { so[c] += o[c]; sn[c] += n[c]; }
    }
    const idx = j * w + i;
    for (let c = 0; c < 3; c++) e[idx * 3 + c] = Math.log2(Math.max(sn[c] / (k * k), EPS) / Math.max(so[c] / (k * k), EPS));
    g[idx] = Math.log2(Math.max(0.2126 * so[0] / (k * k) + 0.7152 * so[1] / (k * k) + 0.0722 * so[2] / (k * k), 1e-4));
  }
  return { e, g, w, h, s, phase };
}

// RenoDX's edge-aware transport (look_upsample.cs): 4x4 joint-bilateral, spatial sigma 0.5 NR
// pixels, range sigma 0.5 stops on log2 luminance.
function jbu(low, f) {
  const out = new Float32Array(W * H * 3);
  const { e, g, w, h, s, phase } = low;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x;
    const ux = (x + 0.5) * s - 0.5 - phase[0], uy = (y + 0.5) * s - 0.5 - phase[1];
    const bx = Math.floor(ux), by = Math.floor(uy);
    let jr = 0, jg = 0, jb = 0, jw = 0, sr = 0, sg = 0, sb = 0, sw = 0;
    for (let dy = -1; dy <= 2; dy++) for (let dx = -1; dx <= 2; dx++) {
      const tx = Math.min(w - 1, Math.max(0, bx + dx)), ty = Math.min(h - 1, Math.max(0, by + dy));
      const ddx = bx + dx - ux, ddy = by + dy - uy;
      const ws = Math.exp(-(ddx * ddx + ddy * ddy) / 0.5);
      const t = ty * w + tx, diff = f.G[i] - g[t], wr = Math.exp(-(diff * diff) / 0.5);
      jr += ws * wr * e[t * 3]; jg += ws * wr * e[t * 3 + 1]; jb += ws * wr * e[t * 3 + 2]; jw += ws * wr;
      sr += ws * e[t * 3]; sg += ws * e[t * 3 + 1]; sb += ws * e[t * 3 + 2]; sw += ws;
    }
    if (jw > 1e-6) { out[i * 3] = jr / jw; out[i * 3 + 1] = jg / jw; out[i * 3 + 2] = jb / jw; }
    else { out[i * 3] = sr / sw; out[i * 3 + 1] = sg / sw; out[i * 3 + 2] = sb / sw; }
  }
  return out;
}

// What perfect accumulation could reach: at every full-resolution pixel, the edit the network
// would give for a footprint of the working size centred on that pixel. Finer detail than the
// footprint is attenuated by the box itself, whatever the number of phases.
function ceilingEdit(f, s) {
  const out = new Float32Array(W * H * 3), k = 4;
  const o = new Float32Array(3), n = new Float32Array(3);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let o0 = 0, o1 = 0, o2 = 0, n0 = 0, n1 = 0, n2 = 0;
    for (let b = 0; b < k; b++) for (let a = 0; a < k; a++) {
      const sx = x + 0.5 + ((a + 0.5) / k - 0.5) / s, sy = y + 0.5 + ((b + 0.5) / k - 0.5) / s;
      sample(f.O, W, H, sx, sy, o); sample(f.N, W, H, sx, sy, n);
      o0 += o[0]; o1 += o[1]; o2 += o[2]; n0 += n[0]; n1 += n[1]; n2 += n[2];
    }
    const i = (y * W + x) * 3;
    out[i] = Math.log2(Math.max(n0 / 16, EPS) / Math.max(o0 / 16, EPS));
    out[i + 1] = Math.log2(Math.max(n1 / 16, EPS) / Math.max(o1 / 16, EPS));
    out[i + 2] = Math.log2(Math.max(n2 / 16, EPS) / Math.max(o2 / 16, EPS));
  }
  return out;
}

// Undo part of the footprint's blur on the shown edit only (never on the history, which would
// feed back): out = e + amount * (e - box(e)), with the box the working footprint.
function sharpen(E, s, amount) {
  if (!amount) return E;
  const b = boxBlur3(E, Math.max(1, Math.round(0.5 / s)));
  const out = new Float32Array(E.length);
  for (let k = 0; k < E.length; k++) out[k] = E[k] + amount * (E[k] - b[k]);
  return out;
}

// ------------------------------------------------------------------ NETS accumulation
const halton = (i, b) => { let f = 1, r = 0; while (i > 0) { f /= b; r += f * (i % b); i = Math.floor(i / b); } return r; };
const phaseOf = (t, L) => [halton((t % L) + 1, 2) - 0.5, halton((t % L) + 1, 3) - 0.5];

class Nets {
  constructor(opts) {
    this.o = opts;
    this.H = new Float32Array(W * H * 3); this.Wt = new Float32Array(W * H);
    this.D = new Float32Array(W * H); this.G = new Float32Array(W * H);
    this.first = true;
  }
  step(f, low) {
    const { sigma, wmax, wfill, depthTol, printTol, catmull = false, velCap = 0, gain = 1, anchor = 0 } = this.o;
    const { e, w, h, s, phase } = low;
    const fill = jbu(low, f);
    const H2 = new Float32Array(W * H * 3), W2 = new Float32Array(W * H), D2 = new Float32Array(W * H), G2 = new Float32Array(W * H);
    const hr = new Float32Array(3);
    const inv2s2 = 1 / (2 * sigma * sigma);
    let rejected = 0;
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const i = y * W + x;
      // 1. History, followed along the motion vector and admitted where depth and the
      //    original's own luminance (the fingerprint) still agree.
      let wh = 0;
      if (!this.first) {
        const px = x + 0.5 + f.MV[i * 2], py = y + 0.5 + f.MV[i * 2 + 1];
        if (px >= 0 && py >= 0 && px < W && py < H) {
          const nx = Math.min(W - 1, Math.floor(px)), ny = Math.min(H - 1, Math.floor(py)), n = ny * W + nx;
          const dOk = Math.abs(this.D[n] - f.D[i]) / f.D[i] < depthTol;
          const pOk = Math.abs(sample1(this.G, W, H, px, py) - f.G[i]) < printTol;
          if (dOk && pOk) {
            if (catmull) sampleCatmull(this.H, W, H, px, py, hr); else sample(this.H, W, H, px, py, hr);
            // Faster motion keeps a shorter history: what reprojection loses grows with speed.
            const vel = Math.hypot(f.MV[i * 2], f.MV[i * 2 + 1]);
            wh = Math.min(sample1(this.Wt, W, H, px, py), wmax / (1 + velCap * vel));
          }
          else rejected++;
        }
      }
      // 2. This frame's samples near the pixel, weighted by their distance in full-resolution
      //    pixels: only samples that land close to the pixel may add detail.
      const ux = (x + 0.5) * s - 0.5 - phase[0], uy = (y + 0.5) * s - 0.5 - phase[1];
      const bx = Math.floor(ux), by = Math.floor(uy);
      let cr = 0, cg = 0, cb = 0, cw = 0;
      for (let dy = 0; dy <= 1; dy++) for (let dx = 0; dx <= 1; dx++) {
        const tx = bx + dx, ty = by + dy;
        if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
        const ddx = (tx - ux) / s, ddy = (ty - uy) / s;  // full-resolution pixels
        const ws = Math.exp(-(ddx * ddx + ddy * ddy) * inv2s2);
        const diff = f.G[i] - low.g[ty * w + tx], wr = Math.exp(-(diff * diff) / 0.5);
        const wgt = gain * ws * wr, t = ty * w + tx;
        cr += wgt * e[t * 3]; cg += wgt * e[t * 3 + 1]; cb += wgt * e[t * 3 + 2]; cw += wgt;
      }
      // 3. Blend. Without history the pixel starts from the edge-aware fill at a low weight,
      //    so the sharp samples of the next frames take over quickly.
      let r, g, b, wt;
      if (wh <= 0) { r = fill[i * 3]; g = fill[i * 3 + 1]; b = fill[i * 3 + 2]; wt = wfill; }
      else { r = hr[0]; g = hr[1]; b = hr[2]; wt = wh; }
      if (cw > 1e-4) {
        r = (wt * r + cr) / (wt + cw); g = (wt * g + cg) / (wt + cw); b = (wt * b + cb) / (wt + cw); wt += cw;
      }
      H2[i * 3] = r; H2[i * 3 + 1] = g; H2[i * 3 + 2] = b; W2[i] = wt; D2[i] = f.D[i]; G2[i] = f.G[i];
    }
    // Low band re-anchored to this frame's own answer: the history may only contribute detail,
    // never its own idea of the lighting. Also how a real network's change of mind gets in.
    if (anchor > 0) {
      const r = Math.max(1, Math.round(1 / s));
      const lh = boxBlur3(H2, r), lf = boxBlur3(fill, r);
      for (let k = 0; k < H2.length; k++) H2[k] += anchor * (lf[k] - lh[k]);
    }
    this.H = H2; this.Wt = W2; this.D = D2; this.G = G2; this.first = false;
    this.rejected = rejected / (W * H);
    return H2;
  }
}

// ------------------------------------------------------------------ metrics
function blurLuma(E) {
  const m = new Float32Array(W * H);
  for (let i = 0; i < W * H; i++) m[i] = (E[i * 3] + E[i * 3 + 1] + E[i * 3 + 2]) / 3;
  const r = 2, tmp = new Float32Array(W * H), out = new Float32Array(W * H);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { let s = 0, n = 0; for (let k = -r; k <= r; k++) { const xx = x + k; if (xx >= 0 && xx < W) { s += m[y * W + xx]; n++; } } tmp[y * W + x] = s / n; }
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { let s = 0, n = 0; for (let k = -r; k <= r; k++) { const yy = y + k; if (yy >= 0 && yy < H) { s += tmp[yy * W + x]; n++; } } out[y * W + x] = s / n; }
  return { m, low: out };
}
function metrics(out, truth, mask) {
  let se = 0, n = 0;
  const a = blurLuma(out), b = blurLuma(truth);
  let sab = 0, saa = 0, sbb = 0;
  for (let y = BORDER; y < H - BORDER; y++) for (let x = BORDER; x < W - BORDER; x++) {
    const i = y * W + x; if (mask && !mask(i)) continue;
    for (let c = 0; c < 3; c++) { const d = out[i * 3 + c] - truth[i * 3 + c]; se += d * d; }
    n++;
    const ha = a.m[i] - a.low[i], hb = b.m[i] - b.low[i];
    sab += ha * hb; saa += ha * ha; sbb += hb * hb;
  }
  return { rmse: Math.sqrt(se / (n * 3)), detailCorr: sab / Math.sqrt(saa * sbb + 1e-12), detailEnergy: Math.sqrt(saa / (sbb + 1e-12)) };
}

// ------------------------------------------------------------------ images
function savePng(file, pixels, w, h) {
  const c = document.createElement('canvas'); c.width = w; c.height = h;
  const x = c.getContext('2d'); const img = x.createImageData(w, h);
  img.data.set(pixels); x.putImageData(img, 0, 0);
  fs.writeFileSync(file, Buffer.from(c.toDataURL('image/png').split(',')[1], 'base64'));
}
// Panels of one frame, cropped to the face: the shown image O * 2^E and the edit itself.
function panels(file, f, edits, crop, zoom = 2) {
  const cw = crop.w * zoom, ch = crop.h * zoom, gap = 8, rows = 2, cols = edits.length;
  const w = cols * cw + (cols - 1) * gap, h = rows * ch + gap;
  const px = new Uint8ClampedArray(w * h * 4);
  for (let k = 0; k < px.length; k += 4) px[k + 3] = 255;
  edits.forEach((E, col) => {
    for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) {
      const sx = crop.x + Math.floor(x / zoom), sy = crop.y + Math.floor(y / zoom), i = sy * W + sx;
      for (let row = 0; row < 2; row++) {
        const o = ((row * (ch + gap) + y) * w + col * (cw + gap) + x) * 4;
        for (let c = 0; c < 3; c++) {
          const v = row === 0
            ? srgbEncode(Math.min(1, f.O[i * 3 + c] * Math.pow(2, E[i * 3 + c])))
            : 0.5 + E[i * 3 + c] * 0.6;
          px[o + c] = Math.round(Math.min(1, Math.max(0, v)) * 255);
        }
      }
    }
  });
  savePng(file, px, w, h);
}

// ------------------------------------------------------------------ main
// ------------------------------------------------------------------ character guard
// Works on the edit, not the picture: the edit's luminance part (light, shading, texture) passes
// untouched; its chroma part is limited where the original is skin or strongly saturated (hair,
// eyes, costume), so the network can relight a face but not recolour it.
const smooth = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
function protection(r, g, b) {
  // Skin: a soft ellipse in Cb/Cr of the display-encoded original (Chai & Ngan ranges).
  const R = srgbEncode(r) * 255, G = srgbEncode(g) * 255, B = srgbEncode(b) * 255;
  const cb = 128 - 0.168736 * R - 0.331264 * G + 0.5 * B, cr = 128 + 0.5 * R - 0.418688 * G - 0.081312 * B;
  const dcb = (cb - 107) / 22, dcr = (cr - 150) / 18;
  const skin = Math.exp(-(dcb * dcb + dcr * dcr)) * smooth(0.02, 0.06, 0.2126 * r + 0.7152 * g + 0.0722 * b);
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b), sat = mx > 1e-4 ? (mx - mn) / mx : 0;
  return { skin, sat: smooth(0.35, 0.7, sat) };
}
function guard(O, E, n, { skin = 0.8, sat = 0.7, limit = 0.08 } = {}) {
  const out = new Float32Array(E.length);
  for (let i = 0; i < n; i++) {
    const k = i * 3, er = E[k], eg = E[k + 1], eb = E[k + 2];
    const y = 0.2126 * er + 0.7152 * eg + 0.0722 * eb;
    const p = protection(O[k], O[k + 1], O[k + 2]), w = Math.max(p.skin * skin, p.sat * sat);
    for (let c = 0; c < 3; c++) {
      const chroma = E[k + c] - y;
      // Shrink the chroma edit by w, and never let a protected pixel shift more than `limit` stops.
      const kept = chroma * (1 - w), capped = Math.max(-limit, Math.min(limit, kept));
      out[k + c] = y + (kept + (capped - kept) * w);
    }
  }
  return out;
}
// Mean chroma shift (Cb, Cr in 8-bit units) of shown = O * 2^E against O, over pixels a mask keeps.
function chromaShift(O, E, n, mask) {
  let scb = 0, scr = 0, m = 0;
  for (let i = 0; i < n; i++) {
    if (!mask(i)) continue;
    const k = i * 3, ycc = (r, g, b) => { const R = srgbEncode(Math.min(1, r)) * 255, G = srgbEncode(Math.min(1, g)) * 255, B = srgbEncode(Math.min(1, b)) * 255; return [-0.168736 * R - 0.331264 * G + 0.5 * B, 0.5 * R - 0.418688 * G - 0.081312 * B]; };
    const a = ycc(O[k], O[k + 1], O[k + 2]), b = ycc(O[k] * 2 ** E[k], O[k + 1] * 2 ** E[k + 1], O[k + 2] * 2 ** E[k + 2]);
    scb += b[0] - a[0]; scr += b[1] - a[1]; m++;
  }
  return { cb: +(scb / m).toFixed(2), cr: +(scr / m).toFixed(2), px: m };
}

window.runGuard = async ({ out }) => {
  fs.mkdirSync(out, { recursive: true });
  const shots = [
    { name: 'man', T: 33.0, crop: { x: 540, y: 110, w: 230, h: 210 }, mask: 'skin' },
    { name: 'jesse', T: 53.0, crop: { x: 300, y: 140, w: 200, h: 220 }, mask: 'sat' },
  ];
  const report = {};
  for (const s of shots) {
    const O = await grab('original', s.T);
    const { shifted: N } = align(O, await grab('renodx', s.T));
    const n = SW * SH, E = new Float32Array(n * 3);
    for (let i = 0; i < n * 3; i++) E[i] = Math.log2(Math.max(N[i], EPS) / Math.max(O[i], EPS));
    const Eg = guard(O, E, n);
    const inCrop = (i) => { const x = i % SW, y = Math.floor(i / SW); return x >= s.crop.x && x < s.crop.x + s.crop.w && y >= s.crop.y && y < s.crop.y + s.crop.h; };
    const prot = (i) => { const p = protection(O[i * 3], O[i * 3 + 1], O[i * 3 + 2]); return (s.mask === 'skin' ? p.skin : p.sat) > 0.5; };
    const mask = (i) => inCrop(i) && prot(i);
    // The luminance part of the edit must survive the guard unchanged.
    let dy = 0, m = 0;
    for (let i = 0; i < n; i++) if (inCrop(i)) { const k = i * 3; dy += Math.abs((0.2126 * E[k] + 0.7152 * E[k + 1] + 0.0722 * E[k + 2]) - (0.2126 * Eg[k] + 0.7152 * Eg[k + 1] + 0.0722 * Eg[k + 2])); m++; }
    report[s.name] = { renodx: chromaShift(O, E, n, mask), guarded: chromaShift(O, Eg, n, mask), lumaEditChange: +(dy / m).toExponential(2) };
    // Panels: original | RenoDX | RenoDX + guard, 2x.
    const z = 2, cw = s.crop.w * z, ch = s.crop.h * z, gap = 8, w = cw * 3 + gap * 2;
    const px = new Uint8ClampedArray(w * ch * 4);
    [null, E, Eg].forEach((ed, col) => {
      for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) {
        const i = (s.crop.y + Math.floor(y / z)) * SW + s.crop.x + Math.floor(x / z), o = (y * w + col * (cw + gap) + x) * 4;
        for (let c = 0; c < 3; c++) px[o + c] = Math.round(Math.min(1, srgbEncode(Math.min(1, O[i * 3 + c] * (ed ? 2 ** ed[i * 3 + c] : 1)))) * 255);
        px[o + 3] = 255;
      }
    });
    savePng(path.join(out, `guard-${s.name}.png`), px, w, ch);
  }
  fs.writeFileSync(path.join(out, 'guard.json'), JSON.stringify(report, null, 1));
  return report;
};

// ------------------------------------------------------------------ real-network experiment
// Export the synthetic sequence for native/prism-core's NR bench, which runs NVIDIA's network on
// it: the full-resolution sequence (the reference) and working-resolution sequences, fixed grid
// and jittered. Then evaluate NETS against the network's own full-resolution answer.
const REAL_SETS = [
  { dir: 'full', s: 1, jitter: false },
  { dir: 'f50', s: 0.5, jitter: false }, { dir: 'n50', s: 0.5, jitter: true },
  { dir: 'f35', s: 0.35, jitter: false }, { dir: 'n35', s: 0.35, jitter: true },
];
const PHASES = 8;
// Box-filtered original at the working grid, shifted by phase; display-encoded for the network.
function proxyAt(f, s, phase) {
  const w = Math.round(W * s), h = Math.round(H * s), k = s === 1 ? 1 : 4;
  const out = new Float32Array(w * h * 4), o = new Float32Array(3);
  for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
    const cx = (i + 0.5 + phase[0]) / s, cy = (j + 0.5 + phase[1]) / s;
    let r = 0, g = 0, b = 0;
    for (let bb = 0; bb < k; bb++) for (let a = 0; a < k; a++) {
      sample(f.O, W, H, cx + ((a + 0.5) / k - 0.5) / s, cy + ((bb + 0.5) / k - 0.5) / s, o);
      r += o[0]; g += o[1]; b += o[2];
    }
    const q = (j * w + i) * 4, n = k * k;
    out[q] = srgbEncode(r / n); out[q + 1] = srgbEncode(g / n); out[q + 2] = srgbEncode(b / n); out[q + 3] = 1;
  }
  return { px: out, w, h };
}
window.runExport = async ({ out }) => {
  const srcO = await grab('original', 33.0), cam = cameraPath();
  for (const set of REAL_SETS) fs.mkdirSync(path.join(out, set.dir), { recursive: true });
  for (let t = 0; t < FRAMES; t++) {
    const f = makeFrame(t, cam, srcO, srcO);
    for (const set of REAL_SETS) {
      const ph = set.jitter ? phaseOf(t, PHASES) : [0, 0], prev = set.jitter ? phaseOf(Math.max(0, t - 1), PHASES) : [0, 0];
      const p = proxyAt(f, set.s, ph);
      // Motion on the working grid: the full-resolution vector scaled, plus the phase change
      // (a static point moves by +dphase working pixels when the sampling window moves).
      const mv = new Float32Array(p.w * p.h * 2);
      for (let j = 0; j < p.h; j++) for (let i = 0; i < p.w; i++) {
        const fx = Math.min(W - 1, Math.floor((i + 0.5 + ph[0]) / set.s)), fy = Math.min(H - 1, Math.floor((j + 0.5 + ph[1]) / set.s)), fi = fy * W + fx;
        mv[(j * p.w + i) * 2] = f.MV[fi * 2] * set.s + (t ? ph[0] - prev[0] : 0);
        mv[(j * p.w + i) * 2 + 1] = f.MV[fi * 2 + 1] * set.s + (t ? ph[1] - prev[1] : 0);
      }
      fs.writeFileSync(path.join(out, set.dir, `c_${t}.f32`), Buffer.from(p.px.buffer));
      fs.writeFileSync(path.join(out, set.dir, `m_${t}.f32`), Buffer.from(mv.buffer));
      if (t === 0) fs.writeFileSync(path.join(out, set.dir, 'meta.txt'), `${p.w} ${p.h} ${FRAMES}\n`);
    }
    if (t % 8 === 0) log(`exported ${t}/${FRAMES}`);
  }
  return { frames: FRAMES };
};
window.runEval = async ({ out }) => {
  const srcO = await grab('original', 33.0), cam = cameraPath();
  const read = (dir, name, n) => new Float32Array(fs.readFileSync(path.join(out, dir, name)).buffer.slice(0), 0, n);
  const editOf = (dir, t, w, h) => {
    const c = read(dir, `c_${t}.f32`, w * h * 4), o = read(dir, `o_${t}.f32`, w * h * 4);
    const e = new Float32Array(w * h * 3), g = new Float32Array(w * h);
    for (let i = 0; i < w * h; i++) {
      for (let ch = 0; ch < 3; ch++) e[i * 3 + ch] = Math.log2(Math.max(srgbDecode(Math.max(0, o[i * 4 + ch])), EPS) / Math.max(srgbDecode(c[i * 4 + ch]), EPS));
      g[i] = Math.log2(Math.max(0.2126 * srgbDecode(c[i * 4]) + 0.7152 * srgbDecode(c[i * 4 + 1]) + 0.0722 * srgbDecode(c[i * 4 + 2]), 1e-4));
    }
    return { e, g, w, h };
  };
  const opts = { sigma: 0.55, wmax: 12, wfill: 0.25, depthTol: 0.05, printTol: 0.25, catmull: true, velCap: 2, gain: 3 };
  const methods = [
    { key: 'fixed50', dir: 'f50', s: 0.5 }, { key: 'nets50', dir: 'n50', s: 0.5, nets: new Nets(opts) },
    { key: 'fixed35', dir: 'f35', s: 0.35 }, { key: 'nets35', dir: 'n35', s: 0.35, nets: new Nets(opts) },
  ];
  const rows = Object.fromEntries(methods.map((m) => [m.key, []]));
  let effect = 0, temporal = 0;
  let prevRef = null;
  for (let t = 0; t < FRAMES; t++) {
    const f = makeFrame(t, cam, srcO, srcO);
    const ref = editOf('full', t, W, H).e;
    for (let i = 0; i < ref.length; i++) effect += Math.abs(ref[i]);
    // How much the network's own full-resolution answer changes frame to frame while still.
    if (prevRef && t < 24) { let d = 0; for (let i = 0; i < ref.length; i++) d += Math.abs(ref[i] - prevRef[i]); temporal += d / ref.length; }
    prevRef = ref;
    const q = spritePos(t);
    const face = (i) => { const x = i % W + 0.5 - q[0], y = Math.floor(i / W) + 0.5 - q[1]; return x >= 0 && y >= 0 && x < SPRITE.w && y < SPRITE.h && inSprite(x, y); };
    for (const m of methods) {
      const lw = Math.round(W * m.s), lh = Math.round(H * m.s), low = editOf(m.dir, t, lw, lh);
      low.s = m.s; low.phase = m.nets ? phaseOf(t, PHASES) : [0, 0];
      const E = m.nets ? sharpen(m.nets.step(f, low), m.s, 0.5) : jbu(low, f);
      rows[m.key].push({ t, all: metrics(E, ref), face: metrics(E, ref, face) });
    }
  }
  const summary = {
    meanEffectStops: +(effect / (FRAMES * W * H * 3)).toFixed(4),
    stillFrameToFrameChange: +(temporal / 23).toFixed(4),
  };
  for (const m of methods) {
    summary[m.key] = {};
    for (const seg of SEGMENTS) {
      const r = rows[m.key].filter((x) => x.t >= seg.from + 8 && x.t < seg.to);
      const avg = (k, n) => +(r.reduce((s, x) => s + x[k][n], 0) / r.length).toFixed(4);
      summary[m.key][seg.name] = { rmse: avg('all', 'rmse'), detail: avg('all', 'detailCorr'), faceRmse: avg('face', 'rmse'), faceDetail: avg('face', 'detailCorr') };
    }
  }
  fs.writeFileSync(path.join(out, 'real-eval.json'), JSON.stringify(summary, null, 1));
  return summary;
};

// ------------------------------------------------------------------ crop consistency
// Does the network answer a crop the way it answers the same region of the whole frame? If so,
// full-resolution tiles evaluated in rotation give full-resolution fidelity at an even cost.
// Static frame, repeated so the network's temporal state settles; the last frame is compared.
const TILE_REPEAT = 8;
function tileRects(margin) {
  const rects = [];
  for (let ty = 0; ty < 2; ty++) for (let tx = 0; tx < 2; tx++) {
    const ix = tx * W / 2, iy = ty * H / 2, iw = W / 2, ih = H / 2;  // interior
    const x0 = Math.max(0, ix - margin), y0 = Math.max(0, iy - margin);
    const x1 = Math.min(W, ix + iw + margin), y1 = Math.min(H, iy + ih + margin);
    rects.push({ ix, iy, iw, ih, x0, y0, w: x1 - x0, h: y1 - y0 });
  }
  return rects;
}
const TILE_MARGINS = [0, 32, 96];
window.runTileExport = async ({ out }) => {
  const srcO = await grab('original', 33.0), f = makeFrame(0, cameraPath(), srcO, srcO);
  const enc = new Float32Array(W * H * 4);
  for (let i = 0; i < W * H; i++) { for (let c = 0; c < 3; c++) enc[i * 4 + c] = srgbEncode(f.O[i * 3 + c]); enc[i * 4 + 3] = 1; }
  const write = (dir, px, w, h) => {
    fs.mkdirSync(dir, { recursive: true });
    const mv = new Float32Array(w * h * 2);
    for (let t = 0; t < TILE_REPEAT; t++) { fs.writeFileSync(path.join(dir, `c_${t}.f32`), Buffer.from(px.buffer)); fs.writeFileSync(path.join(dir, `m_${t}.f32`), Buffer.from(mv.buffer)); }
    fs.writeFileSync(path.join(dir, 'meta.txt'), `${w} ${h} ${TILE_REPEAT}\n`);
  };
  write(path.join(out, 'whole'), enc, W, H);
  // Whole frame at working resolution: candidates for the low band of a dual-band composite.
  for (const s of [0.5, 0.35, 0.25]) { const p = proxyAt(f, s, [0, 0]); write(path.join(out, `low${Math.round(s * 100)}`), p.px, p.w, p.h); }
  for (const m of TILE_MARGINS) tileRects(m).forEach((r, k) => {
    const px = new Float32Array(r.w * r.h * 4);
    for (let y = 0; y < r.h; y++) px.set(enc.subarray(((r.y0 + y) * W + r.x0) * 4, ((r.y0 + y) * W + r.x0 + r.w) * 4), y * r.w * 4);
    write(path.join(out, `m${m}_t${k}`), px, r.w, r.h);
  });
  return { dirs: 1 + TILE_MARGINS.length * 4 };
};
window.runTileEval = async ({ out }) => {
  const last = TILE_REPEAT - 1;
  const load = (dir, w, h) => {
    const c = new Float32Array(fs.readFileSync(path.join(out, dir, `c_${last}.f32`)).buffer.slice(0));
    const o = new Float32Array(fs.readFileSync(path.join(out, dir, `o_${last}.f32`)).buffer.slice(0));
    const e = new Float32Array(w * h * 3);
    for (let i = 0; i < w * h; i++) for (let ch = 0; ch < 3; ch++) e[i * 3 + ch] = Math.log2(Math.max(srgbDecode(Math.max(0, o[i * 4 + ch])), EPS) / Math.max(srgbDecode(c[i * 4 + ch]), EPS));
    return e;
  };
  const whole = load('whole', W, H);
  const result = {};
  for (const m of TILE_MARGINS) {
    // Assemble the tiles' interiors into a whole frame, then compare with the whole-frame answer.
    const mosaic = new Float32Array(W * H * 3);
    tileRects(m).forEach((r, k) => {
      const e = load(`m${m}_t${k}`, r.w, r.h);
      for (let y = r.iy; y < r.iy + r.ih; y++) for (let x = r.ix; x < r.ix + r.iw; x++)
        for (let c = 0; c < 3; c++) mosaic[(y * W + x) * 3 + c] = e[((y - r.y0) * r.w + (x - r.x0)) * 3 + c];
    });
    // Seams: the edit's jump across the tile borders, against the whole frame's own.
    let seam = 0, seamRef = 0, n = 0;
    for (let y = 0; y < H; y++) for (const x of [W / 2 - 1]) for (let c = 0; c < 3; c++) { seam += Math.abs(mosaic[(y * W + x + 1) * 3 + c] - mosaic[(y * W + x) * 3 + c]); seamRef += Math.abs(whole[(y * W + x + 1) * 3 + c] - whole[(y * W + x) * 3 + c]); n++; }
    result[`margin ${m}px`] = { ...metrics(mosaic, whole), seamJump: +(seam / n).toFixed(4), wholeJumpThere: +(seamRef / n).toFixed(4) };
    if (m !== 96) continue;
    // Dual band: low band from the whole frame at a working resolution, high band from the tiles.
    const srcO = await grab('original', 33.0), f = makeFrame(0, cameraPath(), srcO, srcO);
    for (const s of [0.5, 0.35, 0.25]) {
      const lw = Math.round(W * s), lh = Math.round(H * s), key = `low${Math.round(s * 100)}`;
      const c = new Float32Array(fs.readFileSync(path.join(out, key, `c_${last}.f32`)).buffer.slice(0));
      const o = new Float32Array(fs.readFileSync(path.join(out, key, `o_${last}.f32`)).buffer.slice(0));
      const e = new Float32Array(lw * lh * 3), g = new Float32Array(lw * lh);
      for (let i = 0; i < lw * lh; i++) {
        for (let ch = 0; ch < 3; ch++) e[i * 3 + ch] = Math.log2(Math.max(srgbDecode(Math.max(0, o[i * 4 + ch])), EPS) / Math.max(srgbDecode(c[i * 4 + ch]), EPS));
        g[i] = Math.log2(Math.max(0.2126 * srgbDecode(c[i * 4]) + 0.7152 * srgbDecode(c[i * 4 + 1]) + 0.0722 * srgbDecode(c[i * 4 + 2]), 1e-4));
      }
      const lowUp = jbu({ e, g, w: lw, h: lh, s, phase: [0, 0] }, f);
      result[`whole at ${Math.round(s * 100)}% alone`] = metrics(lowUp, whole);
      for (const r of [8, 16, 32]) {
        const lb = boxBlur3(lowUp, r), tb = boxBlur3(mosaic, r), comp = new Float32Array(mosaic.length);
        for (let k = 0; k < comp.length; k++) comp[k] = lb[k] + (mosaic[k] - tb[k]);
        let sj = 0, sn = 0;
        for (let y = 0; y < H; y++) for (let c2 = 0; c2 < 3; c2++) { sj += Math.abs(comp[(y * W + W / 2) * 3 + c2] - comp[(y * W + W / 2 - 1) * 3 + c2]); sn++; }
        result[`dual band: tiles + whole ${Math.round(s * 100)}%, split ${r}px`] = { ...metrics(comp, whole), seamJump: +(sj / sn).toFixed(4) };
      }
    }
  }
  fs.writeFileSync(path.join(out, 'tiles.json'), JSON.stringify(result, null, 1));
  return result;
};

// ------------------------------------------------------------------ temporal reuse with the real network
// Sets the network evaluates: every frame whole (the reference), every other frame whole (the
// community cadence), the left and right halves in rotation (96 px overlap), whole at 50%.
const ROT_MARGIN = 96;
const ROT_SETS = [
  { dir: 'rfull', frames: (t) => true, back: 1, x0: 0, w: W, s: 1 },
  { dir: 'rcad', frames: (t) => t % 2 === 0, back: 2, x0: 0, w: W, s: 1 },
  { dir: 'rL', frames: (t) => t % 2 === 0, back: 2, x0: 0, w: W / 2 + ROT_MARGIN, s: 1 },
  { dir: 'rR', frames: (t) => t % 2 === 1, back: 2, x0: W / 2 - ROT_MARGIN, w: W / 2 + ROT_MARGIN, s: 1 },
  { dir: 'r50', frames: (t) => true, back: 1, x0: 0, w: W, s: 0.5 },
  { dir: 'rc3', frames: (t) => t % 3 === 0, back: 3, x0: 0, w: W, s: 1 },
  { dir: 'rc4', frames: (t) => t % 4 === 0, back: 4, x0: 0, w: W, s: 1 },
];
window.runRotExport = async ({ out }) => {
  const srcO = await grab('original', 33.0), cam = cameraPath();
  const count = {};
  for (const set of ROT_SETS) { fs.mkdirSync(path.join(out, set.dir), { recursive: true }); count[set.dir] = 0; }
  for (let t = 0; t < FRAMES; t++) {
    for (const set of ROT_SETS) {
      if (!set.frames(t)) continue;
      const f = makeFrame(t, cam, srcO, srcO, Math.min(set.back, Math.max(1, t)));
      let px, mv, w, h;
      if (set.s !== 1) {
        const p = proxyAt(f, set.s, [0, 0]); px = p.px; w = p.w; h = p.h;
        mv = new Float32Array(w * h * 2);
        for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
          const fi = Math.min(H - 1, Math.floor((j + 0.5) / set.s)) * W + Math.min(W - 1, Math.floor((i + 0.5) / set.s));
          mv[(j * w + i) * 2] = f.MV[fi * 2] * set.s; mv[(j * w + i) * 2 + 1] = f.MV[fi * 2 + 1] * set.s;
        }
      } else {
        w = set.w; h = H; px = new Float32Array(w * h * 4); mv = new Float32Array(w * h * 2);
        for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
          const i = y * W + set.x0 + x, o = (y * w + x);
          for (let c = 0; c < 3; c++) px[o * 4 + c] = srgbEncode(f.O[i * 3 + c]);
          px[o * 4 + 3] = 1; mv[o * 2] = f.MV[i * 2]; mv[o * 2 + 1] = f.MV[i * 2 + 1];
        }
      }
      const k = count[set.dir]++;
      fs.writeFileSync(path.join(out, set.dir, `c_${k}.f32`), Buffer.from(px.buffer));
      fs.writeFileSync(path.join(out, set.dir, `m_${k}.f32`), Buffer.from(mv.buffer));
      fs.writeFileSync(path.join(out, set.dir, 'meta.txt'), `${w} ${h} ${count[set.dir]}\n`);
    }
    if (t % 8 === 0) log(`exported ${t}/${FRAMES}`);
  }
  return count;
};

// Follow a cached edit along this frame's motion (Catmull-Rom, clamped).
function reproject(prev, f) {
  const out = new Float32Array(W * H * 3), tmp = new Float32Array(3);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x;
    sampleCatmull(prev, W, H, x + 0.5 + f.MV[i * 2], y + 0.5 + f.MV[i * 2 + 1], tmp);
    out[i * 3] = tmp[0]; out[i * 3 + 1] = tmp[1]; out[i * 3 + 2] = tmp[2];
  }
  return out;
}
window.runRotEval = async ({ out }) => {
  const srcO = await grab('original', 33.0), cam = cameraPath();
  const editOf = (dir, k, w, h) => {
    const c = new Float32Array(fs.readFileSync(path.join(out, dir, `c_${k}.f32`)).buffer.slice(0));
    const o = new Float32Array(fs.readFileSync(path.join(out, dir, `o_${k}.f32`)).buffer.slice(0));
    const e = new Float32Array(w * h * 3), g = new Float32Array(w * h);
    for (let i = 0; i < w * h; i++) {
      for (let ch = 0; ch < 3; ch++) e[i * 3 + ch] = Math.log2(Math.max(srgbDecode(Math.max(0, o[i * 4 + ch])), EPS) / Math.max(srgbDecode(c[i * 4 + ch]), EPS));
      g[i] = Math.log2(Math.max(0.2126 * srgbDecode(c[i * 4]) + 0.7152 * srgbDecode(c[i * 4 + 1]) + 0.0722 * srgbDecode(c[i * 4 + 2]), 1e-4));
    }
    return { e, g, w, h };
  };
  // Lay a tile's edit into the cache: weight 1 inside, ramping to 0 across the overlap.
  const layTile = (cache, tile, x0, w) => {
    for (let y = 0; y < H; y++) for (let x = 0; x < w; x++) {
      const gx = x0 + x;
      const fromLeft = x0 > 0 ? x / (2 * ROT_MARGIN) : 1, fromRight = x0 + w < W ? (w - 1 - x) / (2 * ROT_MARGIN) : 1;
      const a = Math.min(1, fromLeft, fromRight), i = y * W + gx;
      for (let c = 0; c < 3; c++) cache[i * 3 + c] += a * (tile[(y * w + x) * 3 + c] - cache[i * 3 + c]);
    }
  };
  const names = ['cadence2', 'halves', 'halves+dual50', 'whole50', 'async3', 'async4'];
  let a3 = null, a4 = null, land3 = null, land4 = null;
  const k2 = { rc3: 0, rc4: 0 };
  const rows = Object.fromEntries(names.map((n) => [n, []]));
  let cad = null, rot = null;
  const k = { rcad: 0, rL: 0, rR: 0 };
  for (let t = 0; t < FRAMES; t++) {
    const f = makeFrame(t, cam, srcO, srcO);
    const ref = editOf('rfull', t, W, H).e;
    // Community cadence: the whole frame every other frame, carried along the motion between.
    cad = t % 2 === 0 ? editOf('rcad', k.rcad++, W, H).e : reproject(cad, f);
    // Halves in rotation: carry the cache, then lay this frame's half into it.
    rot = rot ? reproject(rot, f) : new Float32Array(W * H * 3);
    if (t % 2 === 0) layTile(rot, editOf('rL', k.rL++, W / 2 + ROT_MARGIN, H).e, 0, W / 2 + ROT_MARGIN);
    else layTile(rot, editOf('rR', k.rR++, W / 2 + ROT_MARGIN, H).e, W / 2 - ROT_MARGIN, W / 2 + ROT_MARGIN);
    const low = editOf('r50', t, W / 2, H / 2); low.s = 0.5; low.phase = [0, 0];
    const whole50 = jbu(low, f);
    // Dual band on the shown edit only (the cache keeps the tiles' own answer).
    const lb = boxBlur3(whole50, 16), rb = boxBlur3(rot, 16), dual = new Float32Array(rot.length);
    for (let i = 0; i < dual.length; i++) dual[i] = lb[i] + rot[i] - rb[i];
    const q = spritePos(t);
    const face = (i) => { const x = i % W + 0.5 - q[0], y = Math.floor(i / W) + 0.5 - q[1]; return x >= 0 && y >= 0 && x < SPRITE.w && y < SPRITE.h && inSprite(x, y); };
    // Async: the network's answer for frame t arrives on t+1; until then the older edit is carried.
    a3 = a3 ? reproject(a3, f) : new Float32Array(W * H * 3);
    if (land3) { a3 = reproject(land3, f); land3 = null; }
    if (t % 3 === 0) land3 = editOf('rc3', k2.rc3++, W, H).e;
    a4 = a4 ? reproject(a4, f) : new Float32Array(W * H * 3);
    if (land4) { a4 = reproject(land4, f); land4 = null; }
    if (t % 4 === 0) land4 = editOf('rc4', k2.rc4++, W, H).e;
    const shown = { cadence2: cad, halves: rot, 'halves+dual50': dual, whole50, async3: a3, async4: a4 };
    for (const n of names) rows[n].push({ t, all: metrics(shown[n], ref), face: metrics(shown[n], ref, face) });
    if (t % 8 === 0) log(`eval ${t}/${FRAMES}`);
  }
  const summary = {};
  for (const n of names) {
    summary[n] = {};
    for (const seg of SEGMENTS) {
      const r = rows[n].filter((x) => x.t >= seg.from + 8 && x.t < seg.to);
      const avg = (a, b) => +(r.reduce((s, x) => s + x[a][b], 0) / r.length).toFixed(4);
      summary[n][seg.name] = { rmse: avg('all', 'rmse'), detail: avg('all', 'detailCorr'), faceRmse: avg('face', 'rmse'), faceDetail: avg('face', 'detailCorr') };
    }
  }
  fs.writeFileSync(path.join(out, 'rot-eval.json'), JSON.stringify(summary, null, 1));
  return summary;
};

// One accumulation step's inputs and outputs, packed the way the GPU shaders read them, so the
// D3D12 harness (native/prism-core/tests) can check its result against this reference.
function writeDump(dir, c, t, f, low, before, after) {
  fs.mkdirSync(dir, { recursive: true });
  const put = (name, arr) => fs.writeFileSync(path.join(dir, name + '.f32'), Buffer.from(arr.buffer, arr.byteOffset, arr.byteLength));
  const pack4 = (rgb, a, n) => { const o = new Float32Array(n * 4); for (let i = 0; i < n; i++) { o[i * 4] = rgb[i * 3]; o[i * 4 + 1] = rgb[i * 3 + 1]; o[i * 4 + 2] = rgb[i * 3 + 2]; o[i * 4 + 3] = a[i]; } return o; };
  const pack2 = (a, b, n) => { const o = new Float32Array(n * 2); for (let i = 0; i < n; i++) { o[i * 2] = a[i]; o[i * 2 + 1] = b[i]; } return o; };
  put('low', pack4(low.e, low.g, low.w * low.h));
  put('fullG', f.G); put('fullD', f.D); put('motion', f.MV);
  put('histH', pack4(before.H, before.Wt, W * H)); put('histDG', pack2(before.D, before.G, W * H));
  put('refFill', pack4(jbu(low, f), new Float32Array(W * H), W * H));
  put('refH', pack4(after.H, after.Wt, W * H)); put('refDG', pack2(after.D, after.G, W * H));
  const o = { ...{ sigma: 0.55, wmax: 12, wfill: 0.25, depthTol: 0.05, printTol: 0.25, velCap: 0, gain: 1, catmull: false }, ...after.o };
  fs.writeFileSync(path.join(dir, 'meta.json'), JSON.stringify({
    key: c.key, t, W, H, w: low.w, h: low.h, s: low.s, phase: low.phase,
    sigma: o.sigma, wmax: o.wmax, wfill: o.wfill, depthTol: o.depthTol, printTol: o.printTol, velCap: o.velCap, gain: o.gain, catmull: o.catmull ? 1 : 0,
  }, null, 1));
}

window.runSim = async ({ out, sweep, dump }) => {
  fs.mkdirSync(out, { recursive: true });
  const t0 = performance.now();
  const srcO = await grab('original', 33.0);
  const rawN = await grab('renodx', 33.0);
  const { shifted: srcN, shift } = align(srcO, rawN);
  log(`sources ready, neural run aligned by ${shift.join(',')} px`);

  const cam = cameraPath();
  const base = { sigma: 0.55, wmax: 12, wfill: 0.25, depthTol: 0.05, printTol: 0.25, phases: 8 };
  const configs = sweep ?? [
    { key: 'jbu50', label: 'RenoDX 50% (edge-aware, fixed grid)', s: 0.5, nets: false },
    { key: 'nets50', label: 'NETS 50%', s: 0.5, nets: base },
    { key: 'jbu35', label: 'RenoDX 35% (edge-aware, fixed grid)', s: 0.35, nets: false },
    { key: 'nets35', label: 'NETS 35%', s: 0.35, nets: base },
  ];
  const netsOpts = base;
  const state = Object.fromEntries(configs.map((c) => [c.key, c.nets ? new Nets({ ...base, ...c.nets }) : null]));
  const results = Object.fromEntries(configs.map((c) => [c.key, []]));
  const shots = { 23: {}, 47: {}, 71: {} };

  for (let t = 0; t < FRAMES; t++) {
    const f = makeFrame(t, cam, srcO, srcN);
    const q = spritePos(t);
    const spriteMask = (i) => { const x = i % W + 0.5 - q[0], y = Math.floor(i / W) + 0.5 - q[1]; return x >= 0 && y >= 0 && x < SPRITE.w && y < SPRITE.h && inSprite(x, y); };
    for (const c of configs) {
      let E;
      if (c.ceiling) E = ceilingEdit(f, c.s);
      else {
        const phase = c.nets ? phaseOf(t, c.nets.phases ?? netsOpts.phases) : [0, 0];
        const low = downsample(f, c.s, phase);
        const dumping = dump && c.nets && dump.key === c.key && dump.t === t;
        const before = dumping ? { H: state[c.key].H, Wt: state[c.key].Wt, D: state[c.key].D, G: state[c.key].G } : null;
        E = c.nets ? state[c.key].step(f, low) : jbu(low, f);
        if (dumping) writeDump(path.join(out, 'dump'), c, t, f, low, before, state[c.key]);
        if (c.nets && c.nets.sharpen) E = sharpen(E, c.s, c.nets.sharpen);
      }
      results[c.key].push({
        t,
        all: metrics(E, f.E),
        face: metrics(E, f.E, spriteMask),
        rejected: c.nets ? state[c.key].rejected : 0,
      });
      if (shots[t]) shots[t][c.key] = E;
    }
    if (shots[t]) {
      const q2 = spritePos(t);
      const crop = { x: Math.round(q2[0] + 60), y: Math.round(q2[1] + 40), w: 160, h: 150 };
      const edits = [f.E, ...configs.map((c) => shots[t][c.key])];
      panels(path.join(out, `face-t${t}.png`), f, edits, crop, 2);
      panels(path.join(out, `scene-t${t}.png`), f, edits, { x: 600, y: 200, w: 160, h: 150 }, 2);
    }
    if (t % 8 === 0) log(`frame ${t}/${FRAMES}  ${((performance.now() - t0) / 1000).toFixed(0)}s`);
  }

  // Per segment, skipping each segment's first 8 frames so accumulation is measured in its
  // steady state for that motion.
  const summary = {};
  for (const c of configs) {
    summary[c.key] = { label: c.label, pixels: `${(c.s * c.s * 100).toFixed(1)}%` };
    for (const seg of SEGMENTS) {
      const rows = results[c.key].filter((r) => r.t >= seg.from + 8 && r.t < seg.to);
      const avg = (k, m) => rows.reduce((s, r) => s + r[k][m], 0) / rows.length;
      summary[c.key][seg.name] = {
        rmse: +avg('all', 'rmse').toFixed(4),
        detailCorr: +avg('all', 'detailCorr').toFixed(3),
        faceRmse: +avg('face', 'rmse').toFixed(4),
        faceDetailCorr: +avg('face', 'detailCorr').toFixed(3),
        rejected: +(rows.reduce((s, r) => s + r.rejected, 0) / rows.length).toFixed(3),
      };
    }
  }
  fs.writeFileSync(path.join(out, 'results.json'), JSON.stringify({ shift, netsOpts, summary, perFrame: results }, null, 1));
  return { seconds: Math.round((performance.now() - t0) / 1000), shift, summary };
};

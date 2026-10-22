// Ghosting study: replays Prism Core's carry and landing on a recorded sequence (tools/ghost/prep.cjs
// output with network results from tests/nr_bench seq) and compares what each scheme would show
// with the network's own answer on every frame. Everything mirrors the HLSL in pipeline.cpp.
//   node tools/ghost/eval.cjs <prep dir> [cadence=4] [age=2]
const fs = require('fs');
const path = require('path');

const dir = process.argv[2];
const CAD = Number(process.argv[3] || 4), AGE = Number(process.argv[4] || 2);
const crop = JSON.parse(fs.readFileSync(path.join(dir, 'crop.json'), 'utf8'));
const W = crop.cw, H = crop.ch, N = crop.frames, INV = crop.inverted;
const TOL = 0.05, MARGIN = 24, START = 2 * CAD + AGE;
const rd = (f) => new Float32Array(fs.readFileSync(f).buffer.slice(0));
const dec = (c) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);

const frames = [];
for (let t = 0; t < N; t++) {
  const c = rd(path.join(dir, 'ref', `c_${t}.f32`)), o = rd(path.join(dir, 'ref', `o_${t}.f32`));
  const P = new Float32Array(W * H * 3), E = new Float32Array(W * H * 3), Y = new Float32Array(W * H);
  for (let i = 0; i < W * H; i++) {
    for (let ch = 0; ch < 3; ch++) {
      const p = dec(c[i * 4 + ch]), n = dec(Math.max(0, o[i * 4 + ch]));
      P[i * 3 + ch] = p;
      E[i * 3 + ch] = Math.max(-3, Math.min(3, Math.log2(Math.max(n, 1 / 512) / Math.max(p, 1 / 512))));
    }
    Y[i] = Math.log2(Math.max(0.2126 * P[i * 3] + 0.7152 * P[i * 3 + 1] + 0.0722 * P[i * 3 + 2], 1e-4));
  }
  frames.push({ P, Y, E, mv: rd(path.join(dir, 'ref', `m_${t}.f32`)), d: rd(path.join(dir, 'ref', `d_${t}.f32`)) });
}
// The network's edit at each launch frame, from a cadence run (launch k = frame k*CAD).
function launchEdits(set) {
  const out = [];
  for (let k = 0; k * CAD < N; k++) {
    const t = k * CAD, f = frames[t], o = rd(path.join(dir, set, `o_${k}.f32`)), E = new Float32Array(W * H * 3);
    for (let i = 0; i < W * H; i++) for (let ch = 0; ch < 3; ch++)
      E[i * 3 + ch] = Math.max(-3, Math.min(3, Math.log2(Math.max(dec(Math.max(0, o[i * 4 + ch])), 1 / 512) / Math.max(f.P[i * 3 + ch], 1 / 512))));
    out.push({ t, E, Y: f.Y, d: f.d });
  }
  return out;
}

// Motion vector dilation: the vector of the nearest surface in a 5x5 window (edges of moving
// objects take the object's motion, not the background's).
function dilate(f) {
  const mv = new Float32Array(W * H * 2);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let best = -1, bd = INV ? -Infinity : Infinity;
    for (let j = -2; j <= 2; j++) for (let i = -2; i <= 2; i++) {
      const xx = Math.min(W - 1, Math.max(0, x + i)), yy = Math.min(H - 1, Math.max(0, y + j)), k = yy * W + xx, dd = f.d[k];
      if (INV ? dd > bd : dd < bd) { bd = dd; best = k; }
    }
    mv[(y * W + x) * 2] = f.mv[best * 2]; mv[(y * W + x) * 2 + 1] = f.mv[best * 2 + 1];
  }
  return mv;
}

// Follow (pipeline.cpp): carry last frame's edit and warp along this frame's motion, taps admitted
// where the stored depth matches this pixel's.
function follow(f, mv, hist, histD, warp) {
  const e = new Float32Array(W * H * 3), wo = new Float32Array(W * H * 4), ok = new Uint8Array(W * H);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x, px = x + mv[i * 2], py = y + mv[i * 2 + 1];
    const bx = Math.floor(px), by = Math.floor(py), fx = px - bx, fy = py - by, d = f.d[i], tol = TOL * Math.max(Math.abs(d), 1e-4);
    let ws = 0, qs = 0, e0 = 0, e1 = 0, e2 = 0, qx = 0, qy = 0, qz = 0;
    for (let j = 0; j <= 1; j++) for (let k = 0; k <= 1; k++) {
      const tx = Math.min(W - 1, Math.max(0, bx + k)), ty = Math.min(H - 1, Math.max(0, by + j)), ti = ty * W + tx;
      const wt = (k ? fx : 1 - fx) * (j ? fy : 1 - fy) * (Math.abs(histD[ti] - d) <= tol ? 1 : 0);
      e0 += hist[ti * 3] * wt; e1 += hist[ti * 3 + 1] * wt; e2 += hist[ti * 3 + 2] * wt; ws += wt;
      const wq = wt * warp[ti * 4 + 3];
      qx += (tx + warp[ti * 4]) * wq; qy += (ty + warp[ti * 4 + 1]) * wq; qz += warp[ti * 4 + 2] * wq; qs += wq;
    }
    const inside = px >= -0.5 && py >= -0.5 && px <= W - 0.5 && py <= H - 0.5;
    if (inside && ws > 1e-3) { e[i * 3] = e0 / ws; e[i * 3 + 1] = e1 / ws; e[i * 3 + 2] = e2 / ws; ok[i] = 1; }
    if (inside && qs > 1e-3) { wo[i * 4] = qx / qs - x; wo[i * 4 + 1] = qy / qs - y; wo[i * 4 + 2] = qz / qs; wo[i * 4 + 3] = 1; }
  }
  return { e, warp: wo, ok };
}

// Newly revealed pixels (no landed edit reaches them and no carried one either) borrow the edit of
// nearby pixels on the same depth layer: rings of taps in 8 directions, the nearest ring that has
// any same-depth valid tap wins.
const FILL_RINGS = [2, 4, 8, 16, 32];
const DIRS = [[1, 0], [-1, 0], [0, 1], [0, -1], [1, 1], [1, -1], [-1, 1], [-1, -1]];
function fill(f, e, valid) {
  const out = e.slice();
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x;
    if (valid[i]) continue;
    const d = f.d[i], tol = TOL * Math.max(Math.abs(d), 1e-4) * 2;
    for (const r of FILL_RINGS) {
      let n = 0, a0 = 0, a1 = 0, a2 = 0;
      for (const [dx, dy] of DIRS) {
        const xx = x + dx * r, yy = y + dy * r;
        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
        const j = yy * W + xx;
        if (!valid[j] || Math.abs(f.d[j] - d) > tol) continue;
        a0 += e[j * 3]; a1 += e[j * 3 + 1]; a2 += e[j * 3 + 2]; n++;
      }
      if (n) { out[i * 3] = a0 / n; out[i * 3 + 1] = a1 / n; out[i * 3 + 2] = a2 / n; break; }
    }
  }
  return out;
}

// The network's edit reached through a warp (Land, pipeline.cpp), with an optional consistency
// weight: how far this pixel's brightness moved from the launch frame's at the warped spot.
function throughWarp(f, warp, L, i, x, y, sigma) {
  if (warp[i * 4 + 3] < 0.5) return null;
  const px = x + warp[i * 4], py = y + warp[i * 4 + 1], z = warp[i * 4 + 2], tol = TOL * Math.max(Math.abs(z), 1e-4);
  const bx = Math.floor(px), by = Math.floor(py), fx = px - bx, fy = py - by;
  let ns = 0, n0 = 0, n1 = 0, n2 = 0, yl = 0;
  for (let j = 0; j <= 1; j++) for (let k = 0; k <= 1; k++) {
    const tx = Math.min(W - 1, Math.max(0, bx + k)), ty = Math.min(H - 1, Math.max(0, by + j)), ti = ty * W + tx;
    const wt = (k ? fx : 1 - fx) * (j ? fy : 1 - fy) * (Math.abs(L.d[ti] - z) <= tol ? 1 : 0);
    n0 += L.E[ti * 3] * wt; n1 += L.E[ti * 3 + 1] * wt; n2 += L.E[ti * 3 + 2] * wt; yl += L.Y[ti] * wt; ns += wt;
  }
  if (!(px >= -0.5 && py >= -0.5 && px <= W - 0.5 && py <= H - 0.5) || ns <= 1e-3) return null;
  let a = 1;
  if (sigma > 0) { const dy = (f.Y[i] - yl / ns) / sigma; a = Math.exp(-dy * dy); }
  return [a * n0 / ns, a * n1 / ns, a * n2 / ns];
}

// One scheme over the whole sequence; returns per-frame shown edits.
function run({ net, direct = false, dilateMv = false, sigma = 0, ageOverride, fillHoles = false }) {
  const L = launchEdits(net), age = ageOverride ?? AGE;
  let hist = new Float32Array(W * H * 3), histD = new Float32Array(W * H), warpF = new Float32Array(W * H * 4), warpL = new Float32Array(W * H * 4);
  let landed = null, flight = null;
  const shown = [];
  for (let t = 0; t < N; t++) {
    const f = frames[t], mv = dilateMv ? dilate(f) : f.mv;
    const F = follow(f, mv, hist, histD, warpF);
    let e = F.e;
    const valid = F.ok.slice();
    warpF = F.warp;
    if (direct && landed) warpL = follow(f, mv, hist, histD, warpL).warp;
    // Land first, then launch: a frame may do both (the landed edit is kept apart from the next launch).
    const landNow = flight && t === flight.t + age;
    if (landNow) { landed = flight; warpL = warpF.slice(); flight = null; }
    if (t % CAD === 0) { flight = L[t / CAD]; for (let i = 0; i < W * H; i++) { warpF[i * 4] = 0; warpF[i * 4 + 1] = 0; warpF[i * 4 + 2] = f.d[i]; warpF[i * 4 + 3] = 1; } }
    if (landNow || (direct && landed)) {
      const wp = warpL, out = new Float32Array(W * H * 3);
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const i = y * W + x, v = throughWarp(f, wp, landed, i, x, y, sigma);
        if (v) { out[i * 3] = v[0]; out[i * 3 + 1] = v[1]; out[i * 3 + 2] = v[2]; valid[i] = 1; } else { out[i * 3] = e[i * 3]; out[i * 3 + 1] = e[i * 3 + 1]; out[i * 3 + 2] = e[i * 3 + 2]; }
      }
      e = out;
    }
    if (fillHoles) e = fill(f, e, valid);
    hist = e; histD = f.d;
    shown.push(e);
  }
  return shown;
}

// Masks for the metrics: moving pixels and a band around depth edges.
const masks = frames.map((f) => {
  const moving = new Uint8Array(W * H), edge = new Uint8Array(W * H);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x;
    if (Math.hypot(f.mv[i * 2], f.mv[i * 2 + 1]) > 1.5) moving[i] = 1;
    const d = f.d[i];
    for (let j = -4; j <= 4 && !edge[i]; j += 2) for (let k = -4; k <= 4; k += 2) {
      const xx = Math.min(W - 1, Math.max(0, x + k)), yy = Math.min(H - 1, Math.max(0, y + j));
      if (Math.abs(f.d[yy * W + xx] - d) > TOL * Math.max(Math.abs(d), 1e-4)) { edge[i] = 1; break; }
    }
  }
  return { moving, edge };
});
function score(shown) {
  const acc = { all: [0, 0], moving: [0, 0], edge: [0, 0] };
  for (let t = START; t < N; t++) {
    const s = shown[t], r = frames[t].E, m = masks[t];
    for (let y = MARGIN; y < H - MARGIN; y++) for (let x = MARGIN; x < W - MARGIN; x++) {
      const i = y * W + x;
      let e2 = 0;
      for (let ch = 0; ch < 3; ch++) e2 += (s[i * 3 + ch] - r[i * 3 + ch]) ** 2;
      e2 /= 3;
      acc.all[0] += e2; acc.all[1]++;
      if (m.moving[i]) { acc.moving[0] += e2; acc.moving[1]++; }
      if (m.edge[i]) { acc.edge[0] += e2; acc.edge[1]++; }
    }
  }
  const f = (a) => (a[1] ? +Math.sqrt(a[0] / a[1]).toFixed(4) : null);
  return { rmse: f(acc.all), moving: f(acc.moving), edge: f(acc.edge) };
}

const ap = 'c' + CAD + 'approx', ex = 'c' + CAD + 'exact';
const schemes = process.env.QUICK ? {
  'carry chain, approx motion (1.2)': { net: ap },
  'warp-direct, exact motion': { net: ex, direct: true },
  'warp-direct, approx motion': { net: ap, direct: true },
  'warp-direct, exact motion, depth fill': { net: ex, direct: true, fillHoles: true },
} : {
  'S1 1.1 (wait, age 1, approx motion)': { net: ap, ageOverride: 1 },
  'S0 1.2 (poll, carry chain, approx motion)': { net: ap },
  'S2 warp-direct': { net: ap, direct: true },
  'S3 warp-direct + exact network motion': { net: ex, direct: true },
  'S4 S3 + MV dilation': { net: ex, direct: true, dilateMv: true },
  'S5 S4 + consistency (sigma 0.25)': { net: ex, direct: true, dilateMv: true, sigma: 0.25 },
};
const results = {};
// Without Prism the shown edit is zero: the size of the reference edit itself.
results['no NR (edit 0)'] = score(frames.map(() => new Float32Array(W * H * 3)));
console.log('no NR (edit 0)', results['no NR (edit 0)']);
for (const [name, opt] of Object.entries(schemes)) {
  results[name] = score(run(opt));
  console.log(name, results[name]);
}
const share = (masks.reduce((s, m) => s + m.moving.reduce((a, b) => a + b, 0), 0) / (N * W * H) * 100).toFixed(1);
console.log(`moving pixels: ${share}%  frames scored: ${START}..${N - 1}  cadence ${CAD}, landing age ${AGE}`);
fs.writeFileSync(path.join(dir, `eval-c${CAD}-a${AGE}.json`), JSON.stringify(results, null, 1));

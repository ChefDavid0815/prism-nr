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
function launchEdits(set, cad = CAD) {
  const out = [];
  for (let k = 0; k * cad < N; k++) {
    const t = k * cad, f = frames[t], o = rd(path.join(dir, set, `o_${k}.f32`)), E = new Float32Array(W * H * 3);
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

// Luminance-guided edit: per tile of the launch frame, a least-squares line per channel from the
// pixel's log2 luma to the network's edit (the network mostly re-grades tone, so on fast or
// unreliable surfaces the edit is predicted from the current frame's own luma instead of carried).
// layers: two lines per tile, one for the near and one for the far depth layer (split halfway
// between the tile's depth extremes when they differ), so a tile holding both a car and the road
// under it gives the road the road's line.
function tileFits(L, T, layers = false) {
  const tx = Math.ceil(W / T), ty = Math.ceil(H / T), fit = new Float32Array(tx * ty * 12), split = new Float32Array(tx * ty).fill(NaN);
  // The luma range each tile's line was fitted on: the line is not extrapolated far beyond it.
  const range = new Float32Array(tx * ty * 2);
  for (let q = 0; q < tx * ty; q++) { range[q * 2] = Infinity; range[q * 2 + 1] = -Infinity; }
  for (let j = 0; j < ty; j++) for (let i = 0; i < tx; i++) {
    // A tile plus half a tile around it, so neighbouring lines agree at the seams.
    const x0 = Math.max(0, (i - 0.5) * T | 0), x1 = Math.min(W, (i + 1.5) * T), y0 = Math.max(0, (j - 0.5) * T | 0), y1 = Math.min(H, (j + 1.5) * T);
    let dmin = Infinity, dmax = -Infinity;
    for (let y = y0; y < y1; y += 2) for (let x = x0; x < x1; x += 2) { const d = L.d[y * W + x]; dmin = Math.min(dmin, d); dmax = Math.max(dmax, d); }
    const sp = layers && dmax - dmin > TOL * 2 * Math.max(Math.abs(dmax), 1e-4) ? (dmin + dmax) / 2 : NaN;
    split[j * tx + i] = sp;
    const counts = [0, 0];
    for (let layer = 0; layer < 2; layer++) {
      let n = 0, sy = 0, syy = 0, ylo = Infinity, yhi = -Infinity; const se = [0, 0, 0], sye = [0, 0, 0];
      for (let y = y0; y < y1; y += 2) for (let x = x0; x < x1; x += 2) {
        const p = y * W + x;
        if (!Number.isNaN(sp) && (L.d[p] >= sp ? 1 : 0) !== layer) continue;
        const Y = L.Y[p];
        n++; sy += Y; syy += Y * Y; ylo = Math.min(ylo, Y); yhi = Math.max(yhi, Y);
        for (let c = 0; c < 3; c++) { se[c] += L.E[p * 3 + c]; sye[c] += Y * L.E[p * 3 + c]; }
      }
      const k = (j * tx + i) * 12 + layer * 6;
      counts[layer] = n;
      if (n < 4) continue;
      const my = sy / n, vy = syy / n - my * my;
      for (let c = 0; c < 3; c++) {
        const me = se[c] / n, b = vy > 1e-3 ? Math.max(-1.5, Math.min(1.5, (sye[c] / n - my * me) / (vy + 0.02))) : 0;
        fit[k + c * 2] = me - b * my; fit[k + c * 2 + 1] = b;
      }
      range[(j * tx + i) * 2] = Math.min(range[(j * tx + i) * 2], ylo);
      range[(j * tx + i) * 2 + 1] = Math.max(range[(j * tx + i) * 2 + 1], yhi);
    }
    const k0 = (j * tx + i) * 12;
    if (Number.isNaN(sp) || counts[1] < 4) fit.copyWithin(k0 + 6, k0, k0 + 6);
    else if (counts[0] < 4) fit.copyWithin(k0, k0 + 6, k0 + 12);
  }
  return { fit, split, range, tx, T };
}
// The guided edit at a launch-frame position (bilinear over the tile centres) for luma Y; with
// layers, each tile answers with the layer this pixel's depth belongs to.
function guidedAt(G, lx, ly, Y, out, d) {
  const fx = Math.max(0, Math.min(G.tx - 1, lx / G.T - 0.5)), ty = G.split.length / G.tx;
  const fy = Math.max(0, Math.min(ty - 1, ly / G.T - 0.5));
  const x0 = Math.floor(fx), y0 = Math.floor(fy), x1 = Math.min(G.tx - 1, x0 + 1), y1 = Math.min(ty - 1, y0 + 1), ax = fx - x0, ay = fy - y0;
  const layerOf = (q) => (Number.isNaN(G.split[q]) ? 0 : d >= G.split[q] ? 6 : 0);
  const m = G.margin ?? Infinity;
  const yc = (q) => Math.max(G.range[q * 2] - m, Math.min(G.range[q * 2 + 1] + m, Y));
  for (let c = 0; c < 3; c++) {
    const v = (q) => { const k = q * 12 + layerOf(q); return G.fit[k + c * 2] + G.fit[k + c * 2 + 1] * yc(q); };
    out[c] = (v(y0 * G.tx + x0) * (1 - ax) + v(y0 * G.tx + x1) * ax) * (1 - ay) + (v(y1 * G.tx + x0) * (1 - ax) + v(y1 * G.tx + x1) * ax) * ay;
  }
}

// One scheme over the whole sequence; returns per-frame shown edits.
// guided: { tile, near, far } - where the surface moved more than `near` px since the launch the
// guided edit takes over, fully beyond `far`; holes take it too.
function run({ net, direct = false, dilateMv = false, sigma = 0, ageOverride, fillHoles = false, blend = 1, cad = CAD, guided = null }) {
  const L = launchEdits(net, cad), age = ageOverride ?? AGE;
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
    if (t % cad === 0) { flight = L[t / cad]; for (let i = 0; i < W * H; i++) { warpF[i * 4] = 0; warpF[i * 4 + 1] = 0; warpF[i * 4 + 2] = f.d[i]; warpF[i * 4 + 3] = 1; } }
    if (landNow || (direct && landed)) {
      const wp = warpL, out = new Float32Array(W * H * 3);
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const i = y * W + x, v = throughWarp(f, wp, landed, i, x, y, sigma);
        if (v && blend < 1 && F.ok[i]) { for (let ch = 0; ch < 3; ch++) out[i * 3 + ch] = blend * v[ch] + (1 - blend) * e[i * 3 + ch]; valid[i] = 1; } else if (v) { out[i * 3] = v[0]; out[i * 3 + 1] = v[1]; out[i * 3 + 2] = v[2]; valid[i] = 1; } else { out[i * 3] = e[i * 3]; out[i * 3 + 1] = e[i * 3 + 1]; out[i * 3 + 2] = e[i * 3 + 2]; }
      }
      e = out;
    }
    if (guided && landed) {
      if (landed.G?.T !== guided.tile) { landed.G = tileFits(landed, guided.tile, guided.layers); landed.G.margin = guided.margin ?? Infinity; }
      const g = [0, 0, 0], wp = warpL;
      const ramp = (v, a, b) => (v <= a ? 0 : v >= b ? 1 : (v - a) / (b - a));
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const i = y * W + x, ok = wp[i * 4 + 3] > 0.5;
        // Speed: this frame's own motion, px per frame. Mismatch: how far this pixel's luma is from
        // the launch frame's at the warped spot, in stops (reflections, spray and blur don't follow
        // the vectors). Either one hands the pixel to the guided edit; holes go to it when fast.
        const speed = Math.hypot(f.mv[i * 2], f.mv[i * 2 + 1]);
        let w = guided.near != null ? ramp(speed, guided.near, guided.far) : 0;
        if (guided.m0 != null) {
          if (ok && valid[i]) {
            // Averaged over a 3x3 neighbourhood (same warp offset): raindrops and sparkle on one
            // pixel must not flip it alone, or the switch shows as salt and pepper.
            const r = guided.mr ?? 0, ox = Math.round(wp[i * 4]), oy = Math.round(wp[i * 4 + 1]);
            let s = 0, n = 0;
            for (let dy = -r; dy <= r; dy++) for (let dx = -r; dx <= r; dx++) {
              const xx = Math.min(W - 1, Math.max(0, x + dx)), yy = Math.min(H - 1, Math.max(0, y + dy));
              const px = Math.min(W - 1, Math.max(0, xx + ox)), py = Math.min(H - 1, Math.max(0, yy + oy));
              s += Math.abs(f.Y[yy * W + xx] - landed.Y[py * W + px]); n++;
            }
            w = Math.max(w, ramp(s / n, guided.m0, guided.m1));
          } else if (!valid[i]) w = Math.max(w, ramp(speed, guided.holeNear ?? 16, guided.holeFar ?? 48));
        }
        // edge: within a few pixels of a depth edge, the farther side (road behind a bumper) takes
        // the guided edit when fast: the vectors are render resolution, the edge is off by a pixel or two.
        if (guided.edge && masks[t].edge[i]) {
          const d = f.d[i]; let far = false;
          for (let dy = -guided.edge; dy <= guided.edge && !far; dy += 2) for (let dx = -guided.edge; dx <= guided.edge; dx += 2) {
            const xx = Math.min(W - 1, Math.max(0, x + dx)), yy = Math.min(H - 1, Math.max(0, y + dy)), dd = f.d[yy * W + xx];
            if (INV ? dd > d * (1 + TOL) : dd < d * (1 - TOL)) { far = true; break; }
          }
          if (far) w = Math.max(w, ramp(speed, guided.holeNear ?? 16, guided.holeFar ?? 48));
        }
        if (w <= 0) continue;
        guidedAt(landed.G, ok ? x + wp[i * 4] : x, ok ? y + wp[i * 4 + 1] : y, f.Y[i], g, ok ? wp[i * 4 + 2] : f.d[i]);
        // Smoothed in time like the landed edit: part of what this pixel showed last frame stays.
        const sb = guided.blend ?? 1, carried = F.ok[i];
        for (let c = 0; c < 3; c++) {
          const gv = carried && sb < 1 ? sb * g[c] + (1 - sb) * F.e[i * 3 + c] : g[c];
          e[i * 3 + c] = (1 - w) * e[i * 3 + c] + w * gv;
        }
        valid[i] = 1;
      }
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
  const acc = { all: [0, 0], moving: [0, 0], edge: [0, 0], flicker: [0, 0] };
  for (let t = START; t < N; t++) {
    const s = shown[t], r = frames[t].E, m = masks[t], sp = shown[t - 1], rp = frames[t - 1].E, mv = frames[t].mv, d = frames[t].d, dp = frames[t - 1].d;
    for (let y = MARGIN; y < H - MARGIN; y++) for (let x = MARGIN; x < W - MARGIN; x++) {
      const i = y * W + x;
      let e2 = 0;
      for (let ch = 0; ch < 3; ch++) e2 += (s[i * 3 + ch] - r[i * 3 + ch]) ** 2;
      e2 /= 3;
      acc.all[0] += e2; acc.all[1]++;
      if (m.moving[i]) { acc.moving[0] += e2; acc.moving[1]++; }
      if (m.edge[i]) { acc.edge[0] += e2; acc.edge[1]++; }
      // Flicker: the change since last frame (followed along the motion, same surface only)
      // against the reference's own change there.
      const px = Math.round(x + mv[i * 2]), py = Math.round(y + mv[i * 2 + 1]);
      if (px < 0 || py < 0 || px >= W || py >= H) continue;
      const j = py * W + px;
      if (Math.abs(dp[j] - d[i]) > TOL * Math.max(Math.abs(d[i]), 1e-4)) continue;
      let f2 = 0;
      for (let ch = 0; ch < 3; ch++) f2 += ((s[i * 3 + ch] - sp[j * 3 + ch]) - (r[i * 3 + ch] - rp[j * 3 + ch])) ** 2;
      acc.flicker[0] += f2 / 3; acc.flicker[1]++;
    }
  }
  const f = (a) => (a[1] ? +Math.sqrt(a[0] / a[1]).toFixed(4) : null);
  return { rmse: f(acc.all), moving: f(acc.moving), edge: f(acc.edge), flicker: f(acc.flicker) };
}

const ap = 'c' + CAD + 'approx', ex = 'c' + CAD + 'exact';
const schemes = process.env.SCHEMES ? JSON.parse(process.env.SCHEMES) : process.env.QUICK ? {
  'carry chain, approx motion (1.2)': { net: ap },
  'warp-direct, exact motion': { net: ex, direct: true },
  'warp-direct, approx motion': { net: ap, direct: true },
  'warp-direct, exact motion, depth fill': { net: ex, direct: true, fillHoles: true },
  'fill + blend 0.7': { net: ex, direct: true, fillHoles: true, blend: 0.7 },
  'fill + blend 0.5': { net: ex, direct: true, fillHoles: true, blend: 0.5 },
} : {
  'S1 1.1 (wait, age 1, approx motion)': { net: ap, ageOverride: 1 },
  'S0 1.2 (poll, carry chain, approx motion)': { net: ap },
  'S2 warp-direct': { net: ap, direct: true },
  'S3 warp-direct + exact network motion': { net: ex, direct: true },
  'S4 S3 + MV dilation': { net: ex, direct: true, dilateMv: true },
  'S5 S4 + consistency (sigma 0.25)': { net: ex, direct: true, dilateMv: true, sigma: 0.25 },
};
// Render mode (RENDER=<out dir>): per-frame PNGs of what each scheme shows, for the films.
// off: the proxy itself; ref: the network on every frame; v111: 1.1.1 (every 4 frames, waited
// landing next frame, chained carry, constant-velocity motion); v112: 1.1.2 (every 2 frames, poll
// landing, direct sampling, exact motion, depth fill, smoothing 0.7). err-*: |shown - reference|.
if (process.env.RENDER) {
  const zlib = require('zlib');
  const out = process.env.RENDER;
  const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
  const crc = (b) => { let c = -1; for (const v of b) c = crcT[(c ^ v) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const png = (file, rgb) => {
    const raw = Buffer.alloc((W * 3 + 1) * H);
    for (let y = 0; y < H; y++) { raw[y * (W * 3 + 1)] = 0; rgb.copy(raw, y * (W * 3 + 1) + 1, y * W * 3, (y + 1) * W * 3); }
    const ih = Buffer.alloc(13); ih.writeUInt32BE(W, 0); ih.writeUInt32BE(H, 4); ih[8] = 8; ih[9] = 2;
    fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw, { level: 4 })), chunk('IEND', Buffer.alloc(0))]));
  };
  const enc = (v) => { v = Math.max(0, Math.min(1, v)); return Math.round(255 * (v <= 0.0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - 0.055)); };
  const image = (t, e) => { const P = frames[t].P, b = Buffer.alloc(W * H * 3); for (let i = 0; i < W * H * 3; i++) b[i] = enc(e ? P[i] * 2 ** e[i] : P[i]); return b; };
  // Error heat: 0 black, 0.1 stop deep red, 0.25 orange, 0.5+ white-yellow.
  const heat = (t, e) => {
    const r = frames[t].E, b = Buffer.alloc(W * H * 3);
    for (let i = 0; i < W * H; i++) {
      const d = (Math.abs(e[i * 3] - r[i * 3]) + Math.abs(e[i * 3 + 1] - r[i * 3 + 1]) + Math.abs(e[i * 3 + 2] - r[i * 3 + 2])) / 3;
      const k = Math.min(1, d / 0.5);
      b[i * 3] = Math.round(255 * Math.min(1, k * 2.2)); b[i * 3 + 1] = Math.round(255 * Math.max(0, Math.min(1, k * 2.2 - 0.9))); b[i * 3 + 2] = Math.round(255 * Math.max(0, k * 3 - 2.2));
    }
    return b;
  };
  const schemes = JSON.parse(process.env.RENDER_SCHEMES || '{}');
  for (const k of ['off', 'ref', ...Object.keys(schemes), ...Object.keys(schemes).map((x) => 'err-' + x)]) fs.mkdirSync(path.join(out, k), { recursive: true });
  const shownBy = {};
  for (const [name, opt] of Object.entries(schemes)) { shownBy[name] = run(opt); console.log('ran', name, JSON.stringify(score(shownBy[name]))); }
  for (let t = 0; t < N; t++) {
    png(path.join(out, 'off', `f_${t}.png`), image(t, null));
    png(path.join(out, 'ref', `f_${t}.png`), image(t, frames[t].E));
    for (const name of Object.keys(schemes)) {
      png(path.join(out, name, `f_${t}.png`), image(t, shownBy[name][t]));
      png(path.join(out, 'err-' + name, `f_${t}.png`), heat(t, shownBy[name][t]));
    }
  }
  fs.writeFileSync(path.join(out, 'meta.json'), JSON.stringify({ W, H, N, schemes: Object.keys(schemes) }));
  console.log('rendered', N, 'frames to', out);
  process.exit(0);
}
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

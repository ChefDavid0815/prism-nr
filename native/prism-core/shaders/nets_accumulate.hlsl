#include "nets_common.hlsli"

// One NETS accumulation step at full resolution.
//  1. Follow the history along the motion vector; admit it where depth and the original's own
//     luminance (the fingerprint) still agree. Faster motion keeps a shorter history.
//  2. Gather this frame's working samples that land near the pixel, weighted by their distance
//     in full-resolution pixels and by luminance similarity.
//  3. Blend; a pixel without history starts from the edge-aware fill at a low weight so the
//     sharp samples of the next frames take over quickly.

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= FullSize)) return;
  const uint i = id.y * FullSize.x + id.x;
  const float g = FullG[i], depth = FullD[i];
  const float2 mv = Motion[i];

  float3 history = 0.0;
  float wh = 0.0;
  if (FirstFrame == 0u) {
    const float2 prev = float2(id.xy) + 0.5 + mv;
    if (all(prev >= 0.0) && all(prev < float2(FullSize))) {
      const int2 near = min(int2(floor(prev)), int2(FullSize) - 1);
      const bool depthOk = abs(HistDG[FullIndex(near)].x - depth) / depth < DepthTolerance;
      const bool printOk = abs(BilinearHistG(prev) - g) < PrintTolerance;
      if (depthOk && printOk) {
        history = Catmull != 0u ? CatmullHistory(prev) : BilinearH(prev).rgb;
        wh = min(BilinearH(prev).a, HistoryMax / (1.0 + VelocityCap * length(mv)));
      }
    }
  }

  const float2 u = WorkPosition(id.xy);
  const int2 b = int2(floor(u));
  const float inv2s2 = 1.0 / (2.0 * Sigma * Sigma);
  float3 c = 0.0;
  float cw = 0.0;
  [unroll] for (int dy = 0; dy <= 1; ++dy) {
    [unroll] for (int dx = 0; dx <= 1; ++dx) {
      const int2 tap = b + int2(dx, dy);
      if (any(tap < 0) || any(tap >= int2(WorkSize))) continue;
      const float2 d = (float2(tap) - u) / Scale;  // full-resolution pixels
      const float ws = exp(-dot(d, d) * inv2s2);
      const float4 s = Low[uint(tap.y) * WorkSize.x + uint(tap.x)];
      const float diff = g - s.a;
      const float w = SampleGain * ws * exp(-(diff * diff) / 0.5);
      c += w * s.rgb; cw += w;
    }
  }

  float3 e;
  float wt;
  if (wh <= 0.0) { e = Fill[i].rgb; wt = FillWeight; }
  else { e = history; wt = wh; }
  if (cw > 1e-4) { e = (wt * e + c) / (wt + cw); wt += cw; }
  Out[i] = float4(e, wt);
  OutDG[i] = float2(depth, g);
}

#include "nets_common.hlsli"

// Edge-aware fill of this frame's edit at full resolution: a 4x4 joint-bilateral gather,
// spatial sigma 0.5 working pixels, range sigma 0.5 stops on log2 luminance (the transport
// RenoDX measured best, after Kopf et al. 2007). Used where a pixel has no history.

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= FullSize)) return;
  const uint i = id.y * FullSize.x + id.x;
  const float2 u = WorkPosition(id.xy);
  const int2 b = int2(floor(u));
  const int2 last = int2(WorkSize) - 1;
  const float g = FullG[i];
  float3 joint = 0.0, spatial = 0.0;
  float jw = 0.0, sw = 0.0;
  [unroll] for (int dy = -1; dy <= 2; ++dy) {
    [unroll] for (int dx = -1; dx <= 2; ++dx) {
      const int2 tap = b + int2(dx, dy);
      const float2 d = float2(tap) - u;
      const float ws = exp(-dot(d, d) / 0.5);
      const float4 s = Low[uint(clamp(tap.y, 0, last.y)) * WorkSize.x + uint(clamp(tap.x, 0, last.x))];
      const float diff = g - s.a;
      const float wr = exp(-(diff * diff) / 0.5);
      joint += ws * wr * s.rgb; jw += ws * wr;
      spatial += ws * s.rgb; sw += ws;
    }
  }
  Out[i] = float4(jw > 1e-6 ? joint / jw : spatial / sw, 0.0);
}

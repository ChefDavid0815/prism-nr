// NETS accumulation step, production form: one pass, half-precision storage.
// Same algorithm as nets_fill.hlsl + nets_accumulate.hlsl (the reference pair), with two changes
// measured on the Intel GPU in nets_harness:
//  - the edge-aware fill runs inline, only for pixels without usable history (a few percent),
//    instead of as a full-screen pass of its own;
//  - edits, weights, motion, depth and luminance are stored as 16-bit floats.

cbuffer NetsConstants : register(b0) {
  uint2 FullSize;
  uint2 WorkSize;
  float Scale;
  float2 Phase;
  float Sigma;
  float HistoryMax;
  float FillWeight;
  float DepthTolerance;
  float PrintTolerance;
  float VelocityCap;
  float SampleGain;
  uint FirstFrame;
  uint Catmull;
};

StructuredBuffer<uint2> Low : register(t0);     // f16 x4: edit rgb, log2 luminance
StructuredBuffer<float> FullG : register(t1);
StructuredBuffer<float> FullD : register(t2);
StructuredBuffer<uint> Motion : register(t3);   // f16 x2
StructuredBuffer<uint2> HistH : register(t5);   // f16 x4: edit rgb, weight
StructuredBuffer<uint> HistDG : register(t6);   // f16 x2: depth, log2 luminance
RWStructuredBuffer<uint2> Out : register(u0);
RWStructuredBuffer<uint> OutDG : register(u1);

float4 Unpack4(uint2 v) { return float4(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y), f16tof32(v.y >> 16)); }
uint2 Pack4(float4 v) { return uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16)); }
float2 Unpack2(uint v) { return float2(f16tof32(v), f16tof32(v >> 16)); }
uint Pack2(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }

uint FullIndex(int2 p) {
  p = clamp(p, int2(0, 0), int2(FullSize) - 1);
  return uint(p.y) * FullSize.x + uint(p.x);
}
float4 CatmullWeights(float t) {
  const float t2 = t * t, t3 = t2 * t;
  return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0,
                -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= FullSize)) return;
  const uint i = id.y * FullSize.x + id.x;
  const float g = FullG[i], depth = FullD[i];
  const float2 mv = Unpack2(Motion[i]);

  float3 history = 0.0;
  float wh = 0.0;
  if (FirstFrame == 0u) {
    const float2 prev = float2(id.xy) + 0.5 + mv;
    if (all(prev >= 0.0) && all(prev < float2(FullSize))) {
      const float2 pos = prev - 0.5;
      const int2 b = int2(floor(pos));
      const float2 f = pos - float2(b);
      // The 2x2 footprint serves the depth test, the fingerprint, the weight and the clamp.
      const float2 dg00 = Unpack2(HistDG[FullIndex(b)]), dg10 = Unpack2(HistDG[FullIndex(b + int2(1, 0))]);
      const float2 dg01 = Unpack2(HistDG[FullIndex(b + int2(0, 1))]), dg11 = Unpack2(HistDG[FullIndex(b + int2(1, 1))]);
      const int2 near = min(int2(floor(prev)), int2(FullSize) - 1);
      const float nearDepth = Unpack2(HistDG[FullIndex(near)]).x;
      const float histG = lerp(lerp(dg00.y, dg10.y, f.x), lerp(dg01.y, dg11.y, f.x), f.y);
      if (abs(nearDepth - depth) / depth < DepthTolerance && abs(histG - g) < PrintTolerance) {
        const float4 h00 = Unpack4(HistH[FullIndex(b)]), h10 = Unpack4(HistH[FullIndex(b + int2(1, 0))]);
        const float4 h01 = Unpack4(HistH[FullIndex(b + int2(0, 1))]), h11 = Unpack4(HistH[FullIndex(b + int2(1, 1))]);
        const float4 bilinear = lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
        if (Catmull != 0u) {
          const float4 wx = CatmullWeights(f.x), wy = CatmullWeights(f.y);
          float3 sum = 0.0;
          [unroll] for (int j = 0; j < 4; ++j) {
            [unroll] for (int k = 0; k < 4; ++k) {
              float3 v;
              // The inner 2x2 is already loaded.
              if (j == 1 && k == 1) v = h00.rgb; else if (j == 1 && k == 2) v = h10.rgb;
              else if (j == 2 && k == 1) v = h01.rgb; else if (j == 2 && k == 2) v = h11.rgb;
              else v = Unpack4(HistH[FullIndex(b + int2(k - 1, j - 1))]).rgb;
              sum += v * (wx[k] * wy[j]);
            }
          }
          const float3 lo = min(min(h00.rgb, h10.rgb), min(h01.rgb, h11.rgb));
          const float3 hi = max(max(h00.rgb, h10.rgb), max(h01.rgb, h11.rgb));
          history = clamp(sum, lo, hi);
        } else {
          history = bilinear.rgb;
        }
        wh = min(bilinear.a, HistoryMax / (1.0 + VelocityCap * length(mv)));
      }
    }
  }

  const float2 u = (float2(id.xy) + 0.5) * Scale - 0.5 - Phase;
  const int2 ub = int2(floor(u));
  const int2 last = int2(WorkSize) - 1;
  const float inv2s2 = 1.0 / (2.0 * Sigma * Sigma);
  float3 c = 0.0;
  float cw = 0.0;
  [unroll] for (int dy = 0; dy <= 1; ++dy) {
    [unroll] for (int dx = 0; dx <= 1; ++dx) {
      const int2 tap = ub + int2(dx, dy);
      if (any(tap < 0) || any(tap > last)) continue;
      const float2 d = (float2(tap) - u) / Scale;
      const float4 s = Unpack4(Low[uint(tap.y) * WorkSize.x + uint(tap.x)]);
      const float diff = g - s.a;
      const float w = SampleGain * exp(-dot(d, d) * inv2s2) * exp(-(diff * diff) / 0.5);
      c += w * s.rgb; cw += w;
    }
  }

  float3 e;
  float wt;
  if (wh <= 0.0) {
    // No usable history: the edge-aware fill, computed here only.
    float3 joint = 0.0, spatial = 0.0;
    float jw = 0.0, sw = 0.0;
    [unroll] for (int fy = -1; fy <= 2; ++fy) {
      [unroll] for (int fx = -1; fx <= 2; ++fx) {
        const int2 tap = ub + int2(fx, fy);
        const float2 d = float2(tap) - u;
        const float ws = exp(-dot(d, d) / 0.5);
        const float4 s = Unpack4(Low[uint(clamp(tap.y, 0, last.y)) * WorkSize.x + uint(clamp(tap.x, 0, last.x))]);
        const float diff = g - s.a;
        const float wr = exp(-(diff * diff) / 0.5);
        joint += ws * wr * s.rgb; jw += ws * wr;
        spatial += ws * s.rgb; sw += ws;
      }
    }
    e = jw > 1e-6 ? joint / jw : spatial / sw;
    wt = FillWeight;
  } else {
    e = history;
    wt = wh;
  }
  if (cw > 1e-4) { e = (wt * e + c) / (wt + cw); wt += cw; }
  Out[i] = Pack4(float4(e, wt));
  OutDG[i] = Pack2(float2(depth, g));
}

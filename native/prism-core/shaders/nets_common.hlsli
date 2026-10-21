// NETS (neural edit temporal supersampling) shared declarations.
// The CPU reference is tools/nets-sim/sim.js; every function here mirrors one there.
//
// Edits are log2(neural / proxy) per channel. Positions are continuous with pixel centres at
// i + 0.5. The working grid is the network's resolution, scale s of the full grid per axis,
// shifted this frame by Phase (working pixels).

cbuffer NetsConstants : register(b0) {
  uint2 FullSize;     // W, H
  uint2 WorkSize;     // w, h
  float Scale;        // s
  float2 Phase;
  float Sigma;        // sample kernel, full-resolution pixels
  float HistoryMax;   // history weight cap at rest
  float FillWeight;   // weight given to the edge-aware fill where there is no history
  float DepthTolerance;
  float PrintTolerance;  // stops of the original's log2 luminance
  float VelocityCap;  // history cap divides by 1 + VelocityCap * |motion|
  float SampleGain;
  uint FirstFrame;
  uint Catmull;
};

// t0: working grid, rgb = edit, a = log2 luminance of the proxy footprint.
StructuredBuffer<float4> Low : register(t0);
StructuredBuffer<float> FullG : register(t1);      // log2 luminance of the original
StructuredBuffer<float> FullD : register(t2);      // depth
StructuredBuffer<float2> Motion : register(t3);    // previous position minus current, pixels
StructuredBuffer<float4> Fill : register(t4);      // edge-aware fill of this frame
StructuredBuffer<float4> HistH : register(t5);     // rgb = accumulated edit, a = weight
StructuredBuffer<float2> HistDG : register(t6);    // depth, log2 luminance of last frame

RWStructuredBuffer<float4> Out : register(u0);
RWStructuredBuffer<float2> OutDG : register(u1);

uint FullIndex(int2 p) {
  p = clamp(p, int2(0, 0), int2(FullSize) - 1);
  return uint(p.y) * FullSize.x + uint(p.x);
}

// Bilinear reads of full-resolution buffers at continuous coordinates.
float4 BilinearH(float2 pos) {
  pos -= 0.5;
  const int2 b = int2(floor(pos));
  const float2 f = pos - float2(b);
  return lerp(lerp(HistH[FullIndex(b)], HistH[FullIndex(b + int2(1, 0))], f.x),
              lerp(HistH[FullIndex(b + int2(0, 1))], HistH[FullIndex(b + int2(1, 1))], f.x), f.y);
}
float BilinearHistG(float2 pos) {
  pos -= 0.5;
  const int2 b = int2(floor(pos));
  const float2 f = pos - float2(b);
  return lerp(lerp(HistDG[FullIndex(b)].y, HistDG[FullIndex(b + int2(1, 0))].y, f.x),
              lerp(HistDG[FullIndex(b + int2(0, 1))].y, HistDG[FullIndex(b + int2(1, 1))].y, f.x), f.y);
}

float4 CatmullWeights(float t) {
  const float t2 = t * t, t3 = t2 * t;
  return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0,
                -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}

// Catmull-Rom read of the accumulated edit, clamped to its 2x2 bilinear footprint so it
// cannot ring. Repeated bilinear reads would blur the history under sub-pixel motion.
float3 CatmullHistory(float2 pos) {
  pos -= 0.5;
  const int2 b = int2(floor(pos));
  const float4 wx = CatmullWeights(pos.x - b.x), wy = CatmullWeights(pos.y - b.y);
  float3 sum = 0.0, lo = 1e30, hi = -1e30;
  [unroll] for (int j = 0; j < 4; ++j) {
    [unroll] for (int i = 0; i < 4; ++i) {
      const float3 v = HistH[FullIndex(b + int2(i - 1, j - 1))].rgb;
      sum += v * (wx[i] * wy[j]);
      if ((i == 1 || i == 2) && (j == 1 || j == 2)) { lo = min(lo, v); hi = max(hi, v); }
    }
  }
  return clamp(sum, lo, hi);
}

// Where pixel p sits on the working grid, in working-pixel index space.
float2 WorkPosition(uint2 p) {
  return (float2(p) + 0.5) * Scale - 0.5 - Phase;
}

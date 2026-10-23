#include "pipeline.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "MinHook.h"
#include "look_hlsl.h"
#include "ngx_param.h"
#include "overlay.h"

namespace prism {
namespace {

LogFn Log = nullptr;
std::wstring gameDir;
int gameCreateFlags = 0;
bool failed = false;
bool enabled = true;
bool split = false;
float splitPos = 0.5f;  // the split's column as a fraction of the width (SplitPos, the panel drags it)
bool paint = false;
const char* failure = nullptr;  // why the pipeline stopped (the panel shows it)
// F11 capture: one frame's original, proxy, network output and final result, read back
// kRing frames later (by then the GPU is long done with it) and written beside the game.
struct Capture { ID3D12Resource* src; const char* name; ID3D12Resource* rb = nullptr; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; };
Capture captures[4];
uint64_t captureAt = 0;
// Sequence recorder (Record=N in prism-core.ini): N consecutive frames of the game's upscaled
// frame, motion vectors and depth, read back and written to prism-record/ once all have landed,
// for offline study with the real network (tools/nets-sim, tests/nr_bench seq).
struct RecFrame {
  ID3D12Resource *color = nullptr, *motion = nullptr, *depth = nullptr;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fc = {}, fm = {}, fd = {};
  float mvx = 0, mvy = 0; int reset = 0; uint32_t rw = 0, rh = 0;
};
std::vector<RecFrame> recFrames;
int recordLeft = 0, recordAsked = 0;
uint64_t recordDoneAt = 0;
ID3D12Resource *recMotion = nullptr, *recDepth = nullptr;
// A press: the key went down since the last poll (held now and not before), or the "pressed
// since the last call" bit is set. That bit is shared by the whole process (another add-on
// polling the same key clears it), so the held state with edge detection is the reliable part.
bool Pressed(int vk) {
  static bool wasDown[256] = {};
  const SHORT st = GetAsyncKeyState(vk);
  const bool down = (st & 0x8000) != 0;
  const bool edge = down && !wasDown[vk & 0xff];
  wasDown[vk & 0xff] = down;
  return edge || ((st & 1) != 0 && !down);
}

// ------------------------------------------------------------------ the NR runtime
using InitFn = ngx::Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, uint32_t, const void*);
using CreateFn = ngx::Result (*)(ID3D12GraphicsCommandList*, uint32_t, void*, void**);
using EvaluateFn = ngx::Result (*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
InitFn nrInit = nullptr;
CreateFn nrCreate = nullptr;
EvaluateFn nrEvaluate = nullptr;
using ReleaseFn = ngx::Result (*)(void*);
ReleaseFn nrRelease = nullptr;

// Our own parameter block (the signed runtime exports no allocator): an MSVC-layout object.
struct OwnedParams {
  void** vtable;
  struct Value { bool ptr; double num; void* p; };
  std::vector<std::pair<std::string, Value>> values;
  Value* Find(const char* k) { for (auto& [n, v] : values) if (n == k) return &v; return nullptr; }
  void Put(const char* k, Value v) { if (Value* o = Find(k)) *o = v; else values.emplace_back(k, v); }
};
void PSetPtr(OwnedParams* p, const char* k, void* v) { p->Put(k, {true, 0.0, v}); }
void PSetULL(OwnedParams* p, const char* k, unsigned long long v) { p->Put(k, {false, double(v), nullptr}); }
void PSetF(OwnedParams* p, const char* k, float v) { p->Put(k, {false, double(v), nullptr}); }
void PSetD(OwnedParams* p, const char* k, double v) { p->Put(k, {false, v, nullptr}); }
void PSetUI(OwnedParams* p, const char* k, unsigned v) { p->Put(k, {false, double(v), nullptr}); }
void PSetI(OwnedParams* p, const char* k, int v) { p->Put(k, {false, double(v), nullptr}); }
template <typename T> ngx::Result PGet(OwnedParams* p, const char* k, T* out) {
  auto* v = p->Find(k);
  if (!v || v->ptr || !out) return ngx::kFail;
  *out = T(v->num);
  return ngx::kSuccess;
}
ngx::Result PGetPtr(OwnedParams* p, const char* k, void** out) {
  auto* v = p->Find(k);
  if (!v || !v->ptr || !out) return ngx::kFail;
  *out = v->p;
  return ngx::kSuccess;
}
void PReset(OwnedParams* p) { p->values.clear(); }
void* ownedVtable[17] = {
    (void*)&PSetPtr, (void*)&PSetPtr, (void*)&PSetPtr, (void*)&PSetI, (void*)&PSetUI, (void*)&PSetD, (void*)&PSetF, (void*)&PSetULL,
    (void*)&PGetPtr, (void*)&PGetPtr, (void*)&PGetPtr, (void*)&PGet<int>, (void*)&PGet<unsigned>, (void*)&PGet<double>,
    (void*)&PGet<float>, (void*)&PGet<unsigned long long>, (void*)&PReset};
ngx::Result ScalingRatio(void* p) { ngx::SetF(p, "DLSSNR.ScalingRatio", 1.f); return ngx::kSuccess; }

// ------------------------------------------------------------------ shaders
const char* kShaders = R"(
cbuffer C : register(b0) {
  uint2 Size; uint TileCount; uint TilesX; float Intensity; float Guard; float Adapt; uint Split;
  uint2 RenderSize; float2 MotionScale; float DepthTolerance; uint Flags; float Span; float Blend;
};
// Flags: 1 launch frame (the in-flight warp starts here), 2 landing frame (the in-flight warp
// becomes the landed one), 4 a landed edit exists, 8 the network gets motion from the landed warp,
// 16 luminance-guided edit on surfaces the warp can't follow (Guided=1), 32 reduced network,
// 64 the reduced network's edit comes up through the guided filter (UpFilter=1); bits 8-10 the
// look (Look=0..4, see look_hlsl.h); bits 12-15 the steadying weight (Steady x 15) and bits 16-23
// its band (SteadyBand x 100), see Steady(); bits 24-31 how much fine contrast the reduced edit may
// take away ((1 - DetailKeep) x 255), see GuidedFit.
Texture2D<float4> Orig : register(t0);
Texture2D<float4> Proxy : register(t1);
Texture2D<float4> Neural : register(t2);
Texture2D<float2> Motion : register(t3);   // the game's, render resolution
Texture2D<float> Depth : register(t4);     // the game's, render resolution
Texture2D<float4> History : register(t5);  // last frame's edit (rgb) and depth (a)
// Last frame's warp: per pixel, the offset to where that surface was on the launch frame (xy), its
// depth on the launch frame (z) and whether the chain is unbroken (w).
Texture2D<float4> WarpHist : register(t6);
Texture2D<float> LaunchDepth : register(t7);  // the launch frame's depth, render resolution
Texture2D<float4> WarpLHist : register(t8);   // last frame's warp to the landed launch frame
Texture2D<float4> LandedEdit : register(t9);  // the landed network edit (rgb) and its depth (a), launch space
Texture2D<float4> WarpLNow : register(t10);   // this frame's warp to the landed launch frame
Texture2D<float4> Scratch : register(t11);    // Carry's edit before Fill; a < -0.5 marks a hole
Texture2D<float4> GfA : register(t12);        // the reduced edit's guided-filter slopes (rgb), low size
Texture2D<float4> GfB : register(t13);        // and offsets about mid grey's log2 luma
RWTexture2D<float4> Out0 : register(u0);
RWStructuredBuffer<float> Tiles : register(u1);
RWStructuredBuffer<float> Exposure : register(u2);
RWTexture2D<float4> NewEdit : register(u3);
RWTexture2D<float4> NewWarp : register(u4);
RWTexture2D<float4> NewWarpL : register(u5);

uint2 RenderPixel(uint2 p) { return min(uint2((float2(p) + 0.5) * float2(RenderSize) / float2(Size)), RenderSize - 1); }
float DepthAt(uint2 p) { return Depth.Load(int3(RenderPixel(p), 0)); }

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 Enc(float3 c) { c = max(c, 0.0); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float3 Dec(float3 c) { c = max(c, 0.0); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float Smooth(float a, float b, float x) { float t = saturate((x - a) / (b - a)); return t * t * (3.0 - 2.0 * t); }

// Mean log2 luminance of each 16x16 tile.
groupshared float acc[256];
[numthreads(16, 16, 1)]
void Reduce1(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
  float v = 0.0;
  if (all(id.xy < Size)) v = log2(max(Luma(Orig[id.xy].rgb), 1e-5));
  acc[gi] = v;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint s = 128; s > 0; s >>= 1) { if (gi < s) acc[gi] += acc[gi + s]; GroupMemoryBarrierWithGroupSync(); }
  if (gi == 0) Tiles[gid.y * TilesX + gid.x] = acc[0] / 256.0;
}

// Frame scale that puts the mean at mid grey, eased over time so the proxy does not pump.
[numthreads(256, 1, 1)]
void Reduce2(uint gi : SV_GroupIndex) {
  float v = 0.0;
  for (uint i = gi; i < TileCount; i += 256) v += Tiles[i];
  acc[gi] = v;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint s = 128; s > 0; s >>= 1) { if (gi < s) acc[gi] += acc[gi + s]; GroupMemoryBarrierWithGroupSync(); }
  if (gi == 0) {
    const float target = log2(0.18) - acc[0] / float(TileCount);
    const float prev = Exposure[1];
    const float next = prev == 0.0 ? target : lerp(prev, target, Adapt);
    Exposure[1] = next;
    Exposure[0] = exp2(next);
  }
}

// Display-referred proxy for the network: exposure, hue-preserving shoulder on the max channel.
[numthreads(16, 16, 1)]
void Encode(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  float3 c = max(Orig[id.xy].rgb, 0.0) * Exposure[0];
  const float m = max(c.r, max(c.g, c.b));
  c /= 1.0 + m;
  Out0[id.xy] = float4(Enc(c), 1.0);
}

// Reduced working resolution (NetScale < 1, flag 32, Span holds the scale): the network runs on a
// smaller proxy (Down) and its edit comes back to native resolution through a joint bilateral
// upsample guided by the native proxy's luma, so edges stay on the native picture's edges.
// In that mode Neural is the small output and Scratch the small proxy.
uint2 LowSize() { return uint2(floor(float2(Size) * Span + 0.5)); }

[numthreads(16, 16, 1)]
void Down(uint3 id : SV_DispatchThreadID) {
  const uint2 ls = LowSize();
  if (any(id.xy >= ls)) return;
  // Bilinear at the footprint centre, in linear light.
  const float2 f = (float2(id.xy) + 0.5) / Span - 0.5;
  const int2 b = int2(floor(f)), last = int2(Size) - 1;
  const float2 a = f - float2(b);
  float3 c = 0.0;
  [unroll] for (int j = 0; j <= 1; ++j) {
    [unroll] for (int i = 0; i <= 1; ++i) {
      c += Dec(Proxy.Load(int3(clamp(b + int2(i, j), int2(0, 0), last), 0)).rgb) * ((i ? a.x : 1.0 - a.x) * (j ? a.y : 1.0 - a.y));
    }
  }
  Out0[id.xy] = float4(Enc(c), 1.0);
}

float3 UpEdit(uint2 p, float yFull) {
  const int2 lastL = int2(LowSize()) - 1;
  const float2 f = (float2(p) + 0.5) * Span - 0.5;
  const int2 b = int2(floor(f));
  float3 acc = 0.0, bil = 0.0;
  float ws = 0.0;
  [unroll] for (int j = -1; j <= 2; ++j) {
    [unroll] for (int i = -1; i <= 2; ++i) {
      const int2 q = clamp(b + int2(i, j), int2(0, 0), lastL);
      const float3 pl = Dec(Scratch.Load(int3(q, 0)).rgb), nl = Dec(Neural.Load(int3(q, 0)).rgb);
      const float3 el = clamp(log2(max(nl, 1.0 / 512.0) / max(pl, 1.0 / 512.0)), -3.0, 3.0);
      const float2 dd = f - float2(b + int2(i, j));
      const float r = (yFull - log2(max(Luma(pl), 1e-4))) / 0.15;
      const float w = exp(-dot(dd, dd) / 1.28) * exp(-0.5 * r * r);
      acc += el * w;
      ws += w;
      if (i >= 0 && i <= 1 && j >= 0 && j <= 1) bil += el * ((1.0 - abs(dd.x)) * (1.0 - abs(dd.y)));
    }
  }
  return ws > 1e-4 ? acc / ws : bil;  // nothing alike nearby: plain bilinear
}

// The fast guided filter (He & Sun 2015), the reduced network's other way up (UpFilter=1): in each
// 5x5 window of the small picture the edit is fitted as a straight line in the proxy's log2 luma;
// the fits are averaged over 5x5 again, brought up bilinearly and applied to the native log2 luma.
// Against the native network's edit it is as close as the bilateral upsample and flickers about a
// tenth less under motion (nr_bench ups: 0.0076 against 0.0086 at 75%, native 0.0067): every
// output pixel follows the same smooth fit instead of whichever samples happen to look alike.
// One pass: a 16x16 group loads 24x24 (guide, edit), fits 20x20 windows, averages 16x16 of them.
// The offsets are about mid grey (kGfRef) so half floats keep them exact.
static const float kGfRef = -2.5;
static const float kGfEps = 0.01;
groupshared float4 gfIE[576];
groupshared float3 gfSA[400], gfSB[400];
[numthreads(16, 16, 1)]
void GuidedFit(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
  const int2 ls = int2(LowSize()), org = int2(gid.xy) * 16 - 4;
  for (uint i = gi; i < 576; i += 256) {
    const int2 q = clamp(org + int2(i % 24, i / 24), int2(0, 0), ls - 1);
    const float3 pl = Dec(Scratch.Load(int3(q, 0)).rgb), nl = Dec(Neural.Load(int3(q, 0)).rgb);
    gfIE[i] = float4(log2(max(Luma(pl), 1e-4)) - kGfRef, clamp(log2(max(nl, 1.0 / 512.0) / max(pl, 1.0 / 512.0)), -3.0, 3.0));
  }
  GroupMemoryBarrierWithGroupSync();
  for (uint n = gi; n < 400; n += 256) {
    const int c = int(n / 20 + 2) * 24 + int(n % 20 + 2);
    float mI = 0.0, mII = 0.0;
    float3 mP = 0.0, mIP = 0.0;
    [unroll] for (int j = -2; j <= 2; ++j) {
      [unroll] for (int k = -2; k <= 2; ++k) {
        const float4 v = gfIE[c + j * 24 + k];
        mI += v.x; mII += v.x * v.x; mP += v.yzw; mIP += v.x * v.yzw;
      }
    }
    mI /= 25.0; mII /= 25.0; mP /= 25.0; mIP /= 25.0;
    // Fine detail keeps its contrast: a falling slope means the edit flattens the picture inside
    // this window (the network, at its smaller size, softens hair, lashes and thin strands, and the
    // fit would carry that softening to native resolution as blur). The slope may take away at most
    // the share bits 24-31 allow (DetailKeep); raising contrast is never limited.
    const float aMin = -float((Flags >> 24u) & 255u) / 255.0;
    const float3 a = max((mIP - mI * mP) / (max(mII - mI * mI, 0.0) + kGfEps), aMin);
    gfSA[n] = a;
    gfSB[n] = mP - a * mI;
  }
  GroupMemoryBarrierWithGroupSync();
  const int2 t = int2(gi % 16, gi / 16), px = int2(gid.xy) * 16 + t;
  if (any(px >= ls)) return;
  float3 A = 0.0, B = 0.0;
  [unroll] for (int j = 0; j <= 4; ++j) {
    [unroll] for (int k = 0; k <= 4; ++k) { const int c = (t.y + j) * 20 + t.x + k; A += gfSA[c]; B += gfSB[c]; }
  }
  Out0[px] = float4(A / 25.0, 0.0);
  NewEdit[px] = float4(B / 25.0, 0.0);
}

float3 LowBilinear(Texture2D<float4> t, float2 f) {
  const int2 lastL = int2(LowSize()) - 1, b = int2(floor(f));
  const float2 a = f - float2(b);
  float3 v = 0.0;
  [unroll] for (int j = 0; j <= 1; ++j) {
    [unroll] for (int i = 0; i <= 1; ++i) v += t.Load(int3(clamp(b + int2(i, j), int2(0, 0), lastL), 0)).rgb * ((i ? a.x : 1.0 - a.x) * (j ? a.y : 1.0 - a.y));
  }
  return v;
}

float3 UpGuided(uint2 p, float yFull) {
  const float2 f = (float2(p) + 0.5) * Span - 0.5;
  return clamp(LowBilinear(GfA, f) * (yFull - kGfRef) + LowBilinear(GfB, f), -3.0, 3.0);
}

// Temporal steadying (Steady=, every frame the network runs): on fast-moving characters and fine
// detail the network's answer jitters from one frame to the next, which shows as flicker. Last
// frame's edit, followed along this frame's motion vectors and admitted only on the same surface
// (depth), is blended in, but only within a narrow band around this frame's edit: whatever differs
// by more than the band (newly revealed, really changed) is this frame's answer, so it can never
// draw a trail. At most weight x band of the old edit survives (0.5 x 0.06 stops: about 2%).
float3 Steady(uint2 p, float3 e) {
  const float w = float((Flags >> 12u) & 15u) / 15.0;
  const float2 toOut = float2(Size) / float2(RenderSize);
  const float2 mv = Motion.Load(int3(RenderPixel(p), 0)) * MotionScale * toOut;
  const float2 pos = float2(p) + mv;  // last frame's position, pixel-centre space minus 0.5
  const int2 last = int2(Size) - 1, b = int2(floor(pos));
  const float2 f = pos - float2(b);
  const float d = DepthAt(p), tol = DepthTolerance * max(abs(d), 1e-4);
  float3 h = 0.0;
  float ws = 0.0;
  [unroll] for (int j = 0; j <= 1; ++j) {
    [unroll] for (int i = 0; i <= 1; ++i) {
      const float4 hv = History.Load(int3(clamp(b + int2(i, j), int2(0, 0), last), 0));
      const float wt = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y) * (abs(hv.a - d) <= tol ? 1.0 : 0.0);
      h += hv.rgb * wt;
      ws += wt;
    }
  }
  // Off, off screen last frame, or mostly another surface then: this frame's answer.
  const bool use = w > 0.0 && all(pos >= -0.5) && all(pos <= float2(last) + 0.5) && ws >= 0.5;
  const float band = float((Flags >> 16u) & 255u) / 100.0 + 0.15 * abs(Luma(e));
  return use ? e + clamp(h / max(ws, 1e-4) - e, -band, band) * (w * ws) : e;
}

)" PRISM_LOOK_HLSL R"(
// Edit = log2(neural / proxy) in the proxy's linear light; guard its chroma on skin and strong
// colour; apply it to the game's HDR frame as a ratio, so the frame's range is untouched.
[numthreads(16, 16, 1)]
void Compose(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  const float3 o = Orig[id.xy].rgb;
  const float3 pd = Proxy[id.xy].rgb;
  const float3 p = Dec(pd);
  float3 e;
  if (Flags & 32u) {
    const float yl = log2(max(Luma(p), 1e-4));
    e = (Flags & 64u ? UpGuided(id.xy, yl) : UpEdit(id.xy, yl)) * Intensity;
  } else {
    const float3 n = Dec(Neural[id.xy].rgb);
    e = clamp(log2(max(n, 1.0 / 512.0) / max(p, 1.0 / 512.0)), -3.0, 3.0) * Intensity;
  }
  const float3 d = pd * 255.0;
  const float cb = 128.0 - 0.168736 * d.r - 0.331264 * d.g + 0.5 * d.b;
  const float cr = 128.0 + 0.5 * d.r - 0.418688 * d.g - 0.081312 * d.b;
  const float2 q = float2((cb - 107.0) / 22.0, (cr - 150.0) / 18.0);
  const float skin = exp(-dot(q, q)) * Smooth(0.02, 0.06, Luma(p));
  const float mx = max(p.r, max(p.g, p.b)), mn = min(p.r, min(p.g, p.b));
  const float w = max(skin * 0.8, Smooth(0.35, 0.7, mx > 1e-4 ? (mx - mn) / mx : 0.0) * 0.7) * Guard;
  const float y = Luma(e);
  const float3 kept = (e - y) * (1.0 - w);
  e = y + kept + (clamp(kept, -0.08, 0.08) - kept) * w;
  e = Steady(id.xy, e);
  NewEdit[id.xy] = float4(e, DepthAt(id.xy));
  NewWarp[id.xy] = 0.0;  // a synchronous result: no launch in flight to reach
  NewWarpL[id.xy] = 0.0;
  // Split view (F4): the original left of the split column (Split - 4), a thin line at it. The
  // look (Flags bits 8-10) colours the result.
  const bool original = Split >= 4u && id.x < Split - 4u;
  LkUv = (float2(id.xy) + 0.5) / float2(Size); LkPx = id.xy;
  float3 result = original ? o : LookApply(o, e, (Flags >> 8u) & 7u, Exposure[0]);
  if (Split >= 4u && (id.x == Split - 4u || id.x + 1u == Split - 4u)) result = 2.0;
  if (Split == 2u) result = float3(4.0, 0.0, 4.0);  // debug paint: proves the writes reach the screen
  Out0[id.xy] = float4(result, 1.0);
}

// Follows last frame along this frame's motion vectors: the carried edit, and the carried warp back
// to the launch frame. Each of the four bilinear taps counts only where its stored depth matches
// this pixel's (a surface that was hidden last frame gets nothing borrowed). Only the edit and the
// warp travel, never the picture.
void Follow(uint2 p, out float3 e, out float4 warp, out float4 warpL, out bool reached) {
  const float2 toOut = float2(Size) / float2(RenderSize);
  const float2 mv = Motion.Load(int3(RenderPixel(p), 0)) * MotionScale * toOut;
  const float2 pos = float2(p) + mv;  // previous position, pixel-centre space minus 0.5
  const int2 b = int2(floor(pos));
  const float2 f = pos - float2(b);
  const int2 last = int2(Size) - 1;
  const float d = DepthAt(p);
  const float tol = DepthTolerance * max(abs(d), 1e-4);
  e = 0.0;
  float wsum = 0.0, qsum = 0.0, lsum = 0.0;
  float3 q = 0.0, ql = 0.0;
  [unroll] for (int j = 0; j <= 1; ++j) {
    [unroll] for (int i = 0; i <= 1; ++i) {
      const int2 tap = clamp(b + int2(i, j), int2(0, 0), last);
      const float4 hv = History.Load(int3(tap, 0));
      const float wt = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y) * (abs(hv.a - d) <= tol ? 1.0 : 0.0);
      e += hv.rgb * wt;
      wsum += wt;
      const float4 wp = WarpHist.Load(int3(tap, 0));
      const float wq = wt * wp.w;
      q += float3(float2(tap) + wp.xy, wp.z) * wq;
      qsum += wq;
      const float4 wl = WarpLHist.Load(int3(tap, 0));
      const float wlq = wt * wl.w;
      ql += float3(float2(tap) + wl.xy, wl.z) * wlq;
      lsum += wlq;
    }
  }
  const bool inside = all(pos >= -0.5) && all(pos <= float2(last) + 0.5);
  reached = inside && wsum > 1e-3;
  e = reached ? e / wsum : 0.0;
  warp = (inside && qsum > 1e-3) ? float4(q.xy / qsum - float2(p), q.z / qsum, 1.0) : 0.0;
  warpL = (inside && lsum > 1e-3) ? float4(ql.xy / lsum - float2(p), ql.z / lsum, 1.0) : 0.0;
}

// The landed network edit reached through a warp: the four bilinear taps around where this
// surface was on the launch frame, each admitted where the launch depth matches the surface's.
bool Sample(uint2 p, float4 warp, out float3 e) {
  e = 0.0;
  if (warp.w < 0.5) return false;
  const float2 pos = float2(p) + warp.xy;
  const int2 b = int2(floor(pos));
  const float2 f = pos - float2(b);
  const int2 last = int2(Size) - 1;
  const float tol = DepthTolerance * max(abs(warp.z), 1e-4);
  float nsum = 0.0;
  [unroll] for (int j = 0; j <= 1; ++j) {
    [unroll] for (int i = 0; i <= 1; ++i) {
      const int2 tap = clamp(b + int2(i, j), int2(0, 0), last);
      const float4 le = LandedEdit.Load(int3(tap, 0));
      const float wt = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y) * (abs(le.a - warp.z) <= tol ? 1.0 : 0.0);
      e += le.rgb * wt;
      nsum += wt;
    }
  }
  const bool inside = all(pos >= -0.5) && all(pos <= float2(last) + 0.5);
  if (!inside || nsum <= 1e-3) return false;
  e /= nsum;
  return true;
}

// Every asynchronous frame: the shown edit is sampled straight from the landed network edit through
// the warp to its launch frame (one resample from the network's own answer, however old, instead of
// a resample of a resample each frame). The carried chain fills only where that warp is broken.
// On a landing frame the in-flight warp becomes the landed one; on a launch frame the in-flight
// warp starts here (offset zero, this depth).
[numthreads(16, 16, 1)]
void Carry(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  float3 e;
  float4 warp, warpL;
  bool reached;
  Follow(id.xy, e, warp, warpL, reached);
  const float d = DepthAt(id.xy);
  if (Flags & 2u) warpL = warp;
  float3 n = 0.0;
  // Where last frame's edit also reaches, a little of it stays (Blend): a fresh landing does not pop.
  if ((Flags & 4u) && Sample(id.xy, warpL, n)) { e = reached ? lerp(e, n, Blend) : n; reached = true; }
  if (Flags & 1u) warp = float4(0.0, 0.0, d, 1.0);
  // Into the scratch texture: Fill writes the edit and the frame. A hole keeps its depth as -1 - d.
  NewEdit[id.xy] = float4(e, reached ? d : -1.0 - d);
  NewWarp[id.xy] = warp;
  NewWarpL[id.xy] = warpL;
}

// Newly revealed surfaces (no landed edit reaches them, no carried one either) borrow the edit of
// nearby pixels on the same depth layer: taps in eight directions on rings of 2 to 32 pixels, the
// nearest ring with any same-depth tap wins. Then the frame is composed.
static const uint kRings[5] = {2u, 4u, 8u, 16u, 32u};
[numthreads(16, 16, 1)]
void Fill(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  const float4 sc = Scratch.Load(int3(id.xy, 0));
  float3 e = sc.rgb;
  float d = sc.a;
  if (d < -0.5) {
    d = -1.0 - d;
    e = 0.0;
    const float tol = 2.0 * DepthTolerance * max(abs(d), 1e-4);
    const int2 last = int2(Size) - 1;
    [loop] for (uint r = 0; r < 5; ++r) {
      float3 acc = 0.0;
      float cnt = 0.0;
      [unroll] for (int k = 0; k < 8; ++k) {
        const int2 dir = int2(k < 4 ? (k & 1 ? -1 : 1) * (k < 2 ? 1 : 0) : (k & 1 ? -1 : 1), k < 4 ? (k & 1 ? -1 : 1) * (k < 2 ? 0 : 1) : (k < 6 ? 1 : -1));
        const int2 tap = int2(id.xy) + dir * int(kRings[r]);
        if (any(tap < 0) || any(tap > last)) continue;
        const float4 t = Scratch.Load(int3(tap, 0));
        if (t.a >= -0.5 && abs(t.a - d) <= tol) { acc += t.rgb; cnt += 1.0; }
      }
      if (cnt > 0.0) { e = acc / cnt; break; }
    }
  }
  if ((Flags & 20u) == 20u) {
    // Guided: wet roads, reflections, spray and motion blur don't follow the motion vectors, so the
    // carried edit lands on the wrong content there (the ring around a fast car). Where this pixel's
    // luma disagrees with the launch frame's at the warped spot, or a hole opens on a fast surface,
    // the edit comes from the launch tile's line from luma to edit, applied to this frame's luma.
    const float3 c = max(Orig[id.xy].rgb, 0.0) * Exposure[0];
    const float yNow = log2(max(Luma(c / (1.0 + max(c.r, max(c.g, c.b)))), 1e-4));
    const float4 wl = WarpLNow.Load(int3(id.xy, 0));
    const bool hole = sc.a < -0.5;
    float w;
    if (!hole && wl.w > 0.5) {
      // Averaged over 3x3 (same warp offset): a raindrop on one pixel must not flip it alone, or
      // the switch shows as salt and pepper.
      const int2 off = int2(round(wl.xy)), last = int2(Size) - 1;
      float mis = 0.0;
      [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
          const int2 q = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), last);
          const float3 cq = max(Orig[q].rgb, 0.0) * Exposure[0];
          const float yq = log2(max(Luma(cq / (1.0 + max(cq.r, max(cq.g, cq.b)))), 1e-4));
          mis += abs(yq - NewWarp[clamp(q + off, int2(0, 0), last)].x);
        }
      }
      w = saturate((mis / 9.0 - 0.25) / 0.5);
    } else {
      const float2 mv = Motion.Load(int3(RenderPixel(id.xy), 0)) * MotionScale * float2(Size) / float2(RenderSize);
      w = hole ? saturate((length(mv) - 16.0) / 32.0) : 0.0;
    }
    if (w > 0.0) {
      const float2 lpos = wl.w > 0.5 ? float2(id.xy) + wl.xy : float2(id.xy);
      const float2 tf = clamp(lpos / 16.0 - 0.5, 0.0, float2(TilesX - 1u, TileCount / TilesX - 1u));
      const uint2 t0 = uint2(tf), t1 = min(t0 + 1u, uint2(TilesX - 1u, TileCount / TilesX - 1u));
      const float2 a = tf - float2(t0);
      float3 g = 0.0;
      [unroll] for (uint k = 0; k < 4; ++k) {
        const uint2 t = uint2(k & 1u ? t1.x : t0.x, k & 2u ? t1.y : t0.y);
        const uint base = TileCount + (t.y * TilesX + t.x) * 8u;
        const float wt = (k & 1u ? a.x : 1.0 - a.x) * (k & 2u ? a.y : 1.0 - a.y);
        const float yc = clamp(yNow, Tiles[base + 6], max(Tiles[base + 7], Tiles[base + 6]));
        g += wt * float3(Tiles[base] + Tiles[base + 1] * yc, Tiles[base + 2] + Tiles[base + 3] * yc, Tiles[base + 4] + Tiles[base + 5] * yc);
      }
      e = lerp(e, g, w);
    }
  }
  NewEdit[id.xy] = float4(e, d);
  const bool original = Split >= 4u && id.x < Split - 4u;
  LkUv = (float2(id.xy) + 0.5) / float2(Size); LkPx = id.xy;
  float3 result = original ? Orig[id.xy].rgb : LookApply(Orig[id.xy].rgb, e, (Flags >> 8u) & 7u, Exposure[0]);
  if (Split >= 4u && (id.x == Split - 4u || id.x + 1u == Split - 4u)) result = 2.0;
  Out0[id.xy] = float4(result, 1.0);
}

// The landed edit per 16x16 tile as a line from the launch frame's log2 luma to the edit, fitted
// over the tile and half a tile around it so neighbouring lines agree at the seams. Out0 is the
// landed edit, NewEdit the launch luma (both from Bake); lines go to Tiles after the exposure tiles.
// The tile also keeps the luma range it was fitted on: Fill clamps to it, so a line is never
// extrapolated into colours the network never made there (spray brighter than anything at launch).
groupshared float fitAcc[11][256];
[numthreads(16, 16, 1)]
void Fit(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) {
  float s[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1e9, -1e9};  // n, sy, syy, se.rgb, sye.rgb, ymin, ymax
  [unroll] for (uint j = 0; j < 2; ++j) {
    [unroll] for (uint i = 0; i < 2; ++i) {
      const int2 p = int2(gid.xy) * 16 - 8 + int2(gt.xy) * 2 + int2(i, j);
      if (any(p < 0) || any(p >= int2(Size))) continue;
      const float y = NewEdit[p].x;
      const float3 e = Out0[p].rgb;
      s[0] += 1.0; s[1] += y; s[2] += y * y;
      s[3] += e.r; s[4] += e.g; s[5] += e.b;
      s[6] += y * e.r; s[7] += y * e.g; s[8] += y * e.b;
      s[9] = min(s[9], y); s[10] = max(s[10], y);
    }
  }
  [unroll] for (uint k = 0; k < 11; ++k) fitAcc[k][gi] = s[k];
  GroupMemoryBarrierWithGroupSync();
  for (uint st = 128; st > 0; st >>= 1) {
    if (gi < st) {
      [unroll] for (uint k = 0; k < 9; ++k) fitAcc[k][gi] += fitAcc[k][gi + st];
      fitAcc[9][gi] = min(fitAcc[9][gi], fitAcc[9][gi + st]);
      fitAcc[10][gi] = max(fitAcc[10][gi], fitAcc[10][gi + st]);
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (gi == 0) {
    const float n = max(fitAcc[0][0], 1.0), my = fitAcc[1][0] / n, vy = fitAcc[2][0] / n - my * my;
    const uint base = TileCount + (gid.y * TilesX + gid.x) * 8u;
    [unroll] for (uint c = 0; c < 3; ++c) {
      const float me = fitAcc[3 + c][0] / n;
      const float b = vy > 1e-3 ? clamp((fitAcc[6 + c][0] / n - my * me) / (vy + 0.02), -1.5, 1.5) : 0.0;
      Tiles[base + c * 2] = me - b * my;
      Tiles[base + c * 2 + 1] = b;
    }
    Tiles[base + 6] = fitAcc[9][0];
    Tiles[base + 7] = fitAcc[10][0];
  }
}

// The edit the network made on the launch frame, at one of its pixels: log2(neural / proxy),
// guarded like Compose. The proxy is still the launch frame's (nothing rewrote it since).
float3 EditAt(int2 p) {
  const float3 pd = Proxy.Load(int3(p, 0)).rgb;
  const float3 pl = Dec(pd), n = Dec(Neural.Load(int3(p, 0)).rgb);
  float3 e = clamp(log2(max(n, 1.0 / 512.0) / max(pl, 1.0 / 512.0)), -3.0, 3.0) * Intensity;
  const float3 d = pd * 255.0;
  const float cb = 128.0 - 0.168736 * d.r - 0.331264 * d.g + 0.5 * d.b;
  const float cr = 128.0 + 0.5 * d.r - 0.418688 * d.g - 0.081312 * d.b;
  const float2 q = float2((cb - 107.0) / 22.0, (cr - 150.0) / 18.0);
  const float skin = exp(-dot(q, q)) * Smooth(0.02, 0.06, Luma(pl));
  const float mx = max(pl.r, max(pl.g, pl.b)), mn = min(pl.r, min(pl.g, pl.b));
  const float w = max(skin * 0.8, Smooth(0.35, 0.7, mx > 1e-4 ? (mx - mn) / mx : 0.0) * 0.7) * Guard;
  const float y = Luma(e);
  const float3 kept = (e - y) * (1.0 - w);
  return y + kept + (clamp(kept, -0.08, 0.08) - kept) * w;
}

// The launch frame's motion and depth, copied to our own textures through SRVs (no change to
// the game's resource states).
[numthreads(16, 16, 1)]
void CopyGuides(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= RenderSize)) return;
  float2 mv = Motion.Load(int3(id.xy, 0));
  if (Flags & 8u) {
    // Motion since the network's previous run: where this surface was on that launch frame (the
    // landed warp), in the game's motion-vector units; constant velocity where the warp is broken.
    const float2 toOut = float2(Size) / float2(RenderSize);
    const uint2 op = min(uint2((float2(id.xy) + 0.5) * toOut), Size - 1);
    const float4 wl = WarpLNow.Load(int3(op, 0));
    mv = wl.w > 0.5 ? wl.xy / (MotionScale * toOut) : mv * Span;
  }
  Out0[id.xy] = float4(mv, 0.0, 0.0);
  NewEdit[id.xy] = float4(Depth.Load(int3(id.xy, 0)), 0.0, 0.0, 0.0);
}

// The landing frame: the network's edit over the whole launch frame (guarded like Compose), with
// the launch frame's depth, kept apart so the next launch can reuse the proxy and copies at once.
[numthreads(16, 16, 1)]
void Bake(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  Out0[id.xy] = float4(EditAt(int2(id.xy)), LaunchDepth.Load(int3(RenderPixel(id.xy), 0)));
  // The launch frame's log2 luma, for the guided edit (Fit, Fill).
  NewEdit[id.xy] = float4(log2(max(Luma(Dec(Proxy.Load(int3(id.xy, 0)).rgb)), 1e-4)), 0.0, 0.0, 0.0);
}
)";

struct Constants {
  uint32_t w, h, tileCount, tilesX; float intensity, guard, adapt; uint32_t split;
  uint32_t renderW, renderH; float mvScaleX, mvScaleY, depthTolerance; uint32_t flags; float span; float blend;
};
static_assert(sizeof(Constants) == 64, "16 root constants");

// ------------------------------------------------------------------ GPU state
ID3D12Device* device = nullptr;
ID3D12RootSignature* root = nullptr;
ID3D12PipelineState *psoReduce1 = nullptr, *psoReduce2 = nullptr, *psoEncode = nullptr, *psoCompose = nullptr, *psoCarry = nullptr, *psoBake = nullptr, *psoCopyGuides = nullptr, *psoFill = nullptr, *psoFit = nullptr;
bool guided = false;  // the luminance-guided edit (Guided=1)
int upFilter = 1;     // the reduced network's way up: 0 joint bilateral, 1 guided filter (UpFilter)
int look = 0;         // the colour look (Look=0..5: 默认, 高还原, 高饱和, 低饱和, 浓烈, 自定义; look_hlsl.h)
GradeValues grades[kGradeSlots];  // the custom look's controls (Grade=, Grade2=, Grade3=; grade.h)
int gradeSlot = 0;                // the one in use (GradeSlot=0-2)
float gradeK[kGradeConstants] = {};  // and as the shader takes them (look_hlsl.h's LkCustom, b1)
// LkCustom's slots: every binding of the root signature writes the next one (several a frame, a few
// frames in flight: 64 slots are never reused while the GPU may still read them).
constexpr UINT kGradeSlotBytes = 512, kGradeRing = 64;
ID3D12Resource* gradeBuffer = nullptr;
uint8_t* gradeMapped = nullptr;
unsigned gradeNext = 0, gradeFrame = 0;
bool netReset = false;  // NetReset=1: the network starts without its history on every run (study)
// NetScale (0.5-1, synchronous mode only): the network's working resolution. Below 1 it runs on
// a reduced proxy and Compose brings its edit back with a joint bilateral upsample. The reduced
// textures and the network's features are made at full size and only their top-left part (the
// active size) is used and evaluated: the cost follows the active area, and the size can change
// from one frame to the next without making anything again.
float netScale = 1.0f;      // the size this frame runs at
// The size asked for (NetScale): the ceiling when NetScaleAuto is on. The default is the flagship's
// base, 62.5% a side (the app and the panel call it 100%; 150% is 76.55%, half as many pixels again).
float netScaleUser = 0.625f;
// NetScaleAuto=1 with FpsTarget (rendered frames per second: generated frames are not always seen
// by the Present hook, Forza Horizon 6's come from inside the game's own Present): the
// network takes the largest size up to NetScale that keeps the frame rate at the target, down to
// NetScaleMin. Every second the rate is checked; two short seconds step down, three seconds with
// room for the next size (by the measured cost) step up, and a size holds at least three seconds
// (changing it every frame makes the network flicker). A step down that did not buy at least a
// third of the time it should have (the game is held back by something else, the CPU) is undone
// and not tried again for half a minute.
bool netAuto = false;
int fpsTarget = 60;
float netAutoMin = 0.5f;
float netAutoScale = 1.0f;
double autoNextCheck = 0.0, autoHoldUntil = 0.0, autoBlockUntil = 0.0;
int autoShort = 0, autoRoom = 0;
double autoDownFrom = 0.0, autoDownGain = 0.0;  // the frame time before the last step down, and the gain expected
float autoDownPrev = 0.0f;                       // the size before it
// The sizes the governor steps through: the panel's two (100% and 150%) and the steps around them.
const float kNetLevels[] = {0.5f, 0.5625f, 0.625f, 0.6875f, 0.7655f, 0.875f, 1.0f};
float NetLevelUp(float sc, float ceiling) { for (float l : kNetLevels) if (l > sc + 1e-3f) return std::min(l, ceiling); return ceiling; }
float NetLevelDown(float sc, float floor) { float d = floor; for (float l : kNetLevels) if (l < sc - 1e-3f) d = std::max(d, l); return std::min(d, sc); }
uint32_t nw = 0, nh = 0;  // the network's working size
ID3D12Resource *proxyLow = nullptr, *neuralLow = nullptr, *midsLow[2] = {};
ID3D12Resource *gfA = nullptr, *gfB = nullptr;  // the guided filter's coefficients (UpFilter=1)
// What the network's passes read and write: the native textures, or the reduced ones.
ID3D12Resource *netProxy = nullptr, *netNeural = nullptr, *netMids[2] = {};
ID3D12PipelineState* psoDown = nullptr;
ID3D12PipelineState* psoGuidedFit = nullptr;
ID3D12DescriptorHeap* heap = nullptr;
UINT inc = 0;
constexpr UINT kRing = 8;
// Per ring slot: SRVs t0..t13 at [0..13], then UAV sets of u0..u5 (u1 tiles, u2 exposure, u4 next
// warp, u5 next landed warp in all but U):
// P proxy, -, -, next edit; O (Carry) output, -, -, scratch; G the launch copies: motion, -, -,
// depth; R the recorder's copies: motion, -, -, depth; B (Bake, Fit) the landed edit, -, -, the
// launch luma; F (Compose) output, -, -, next edit; L (Fill) output, -, -, next edit, launch luma;
// U (GuidedFit) slopes, -, -, offsets.
constexpr UINT kSrvs = 14;
constexpr UINT kSetP = kSrvs, kSetO = kSetP + 6, kSetG = kSetO + 6, kSetR = kSetG + 6, kSetB = kSetR + 6, kSetF = kSetB + 6, kSetL = kSetF + 6, kSetU = kSetL + 6;
constexpr UINT kBlock = kSetU + 6;
uint32_t w = 0, h = 0, tilesX = 0, tilesY = 0;
ID3D12Resource *origCopy = nullptr, *proxy = nullptr, *neural = nullptr, *tiles = nullptr, *exposure = nullptr;
ID3D12Resource* edits[2] = {};  // ping-pong: last frame's edit and this frame's
ID3D12Resource* warps[2] = {};  // ping-pong: the warp back to the launch frame in flight
ID3D12Resource* warpsL[2] = {};  // ping-pong: the warp back to the landed launch frame
ID3D12Resource* landedEdit = nullptr;  // the landed network edit and its depth, launch space
ID3D12Resource* landedLuma = nullptr;  // the landed launch frame's log2 luma (guided edit)
ID3D12Resource* scratch = nullptr;     // Carry's edit with its holes, before Fill
bool haveLanded = false;
bool exactMotion = true;  // the network gets its motion from the landed warp (NetMotion=exact)
float stability = 0.3f;   // how much of last frame's edit stays where a landed edit is sampled (0..0.9)
float steady = 0.5f;      // Steady: the weight of last frame's edit in Compose's temporal steadying (0 off, 0..0.9)
float steadyBand = 0.06f; // SteadyBand: how far (stops) last frame's edit may pull this frame's (0.01..0.5)
float detailKeep = 0.85f; // DetailKeep: the share of fine contrast (hair, lashes) the reduced edit must leave (0..1)
int current = 0;
bool historyValid = false;
int cadence = 2;  // the network every N frames (F2 cycles 1-4); 0: adaptive (see Adapt)
// Landing: "wait" makes the game's queue wait for the network on the frame after the launch (1.1);
// "poll" lands only once the network has finished and carries one more frame otherwise.
bool landPoll = true;  // the default: no GPU wait on the game's queue (fixes freezes, see notes)
int netPriority = 0;      // 0 normal, 1 high: the compute queue's priority
int fpsCap = 0;           // rendered frames per second, 0 for no cap
uint64_t lastLaunchFrame = 0;
// Adaptive cadence (Cadence=0 with a frame cap): the network interval the frame budget can carry.
// Once the GPU falls behind the cap it stays behind (queued frames, and the network then competes
// with the game for every frame), so a missed frame pauses launches until the frames are on time
// again, and lengthens the interval. The interval only shortens when the measured idle time per
// frame (the limiter's sleep) times the shorter interval covers the network's measured GPU time.
int adaptK = 4;
uint32_t adaptFrames = 0, adaptMisses = 0, adaptLate = 0, adaptHold = 0;
double adaptLastMs = 0.0, adaptSleepSum = 0.0;
double paceSleptMs = 0.0;    // the limiter's sleep before this frame
double netGpuMs = 9.0;       // the network's GPU time on the compute queue, smoothed
ID3D12QueryHeap* netQueries = nullptr;
ID3D12Resource* netStamps = nullptr;
uint64_t* netStampData = nullptr;
uint64_t netTsFreq = 0;

// Asynchronous network (F3): its own compute queue, launched on frame t, landed on frame t+1.
bool asyncMode = true;  // measured fastest with cadence 4 (CONTROL, RTX 5090 Laptop)
ID3D12CommandQueue* computeQueue = nullptr;
// The state of one run of the network's passes; a run can span several command lists.
struct PassRun { int n = 0, next = 0; bool reset = false; ID3D12Resource* in = nullptr; bool readable[2] = {}; };
// Staggered passes (Stagger=1, the default): the passes of one launch go to the compute queue on
// consecutive frames, one per frame, so no frame shares the GPU with more than one pass. Each
// submitted compute list is a step; its allocator is reused once the step fence passed it.
bool stagger = true;
constexpr int kAllocs = 8;
ID3D12CommandAllocator* computeAlloc[kAllocs] = {};
uint64_t allocStep[kAllocs] = {};
ID3D12Fence* fenceStep = nullptr;    // our queue signals: a step has run
uint64_t stepId = 0;                 // the last step recorded
uint64_t stepSubmitted = 0;          // the last step submitted to our queue
ID3D12CommandList* stepList = nullptr;  // the game list the recorded step rides on
bool stepFinal = false;              // that step finishes the launch
PassRun asyncRun;                    // the passes of the launch in flight
ID3D12GraphicsCommandList* computeList = nullptr;
ID3D12Fence* fenceLaunch = nullptr;  // the game's queue signals: the launch frame's list has run
ID3D12Fence* fenceDone = nullptr;    // our queue signals: the network has run
void* asyncHandle = nullptr;
OwnedParams asyncParams;
ID3D12Resource *motionCopy = nullptr, *depthCopy = nullptr;  // the launch frame's, kept to landing
std::mutex asyncMutex;
ID3D12CommandList* landList = nullptr;    // must wait for the network before it runs
uint64_t launchId = 0, submittedId = 0, landId = 0;
bool inFlight = false;
using ExecuteFn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteFn realExecute = nullptr;

void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
  OverlayNoteQueue(q);
  bool step = false, final = false, land = false;
  uint64_t sid = 0, lid = 0, did = 0;
  {
    std::lock_guard<std::mutex> lock(asyncMutex);
    for (UINT i = 0; i < n; ++i) {
      if (lists[i] == landList && landList) { land = true; did = landId; landList = nullptr; }
      if (lists[i] == stepList && stepList) { step = true; sid = stepId; final = stepFinal; lid = launchId; stepList = nullptr; }
    }
  }
  if (land) q->Wait(fenceDone, did);  // the landing frame reads the network's output
  realExecute(q, n, lists);
  if (step) {
    // The step starts once this frame's list (the proxy, copies or the step before) has run.
    q->Signal(fenceLaunch, sid);
    computeQueue->Wait(fenceLaunch, sid);
    ID3D12CommandList* cl[] = {computeList};
    computeQueue->ExecuteCommandLists(1, cl);
    computeQueue->Signal(fenceStep, sid);
    if (final) computeQueue->Signal(fenceDone, lid);
    std::lock_guard<std::mutex> lock(asyncMutex);
    stepSubmitted = sid;
    if (final) submittedId = lid;
  }
}
void* nrHandle = nullptr;
OwnedParams nrParams;
// Passes (Passes=1..5): each pass after the first runs the network again on the previous pass's
// output, with a feature of its own so every pass keeps its own history. proxy -> mids -> neural.
constexpr int kMaxPasses = 5;
int passes = 2;
ID3D12Resource* mids[2] = {};
struct PassFeatures { void* h[kMaxPasses - 1] = {}; bool fresh[kMaxPasses - 1] = {}; } morePasses, moreAsync;
uint64_t frame = 0;
ID3D12QueryHeap* queries = nullptr;
ID3D12Resource* stamps = nullptr;
uint64_t* stampData = nullptr;
uint64_t tsFreq = 0;
double msSum[2] = {}, msAll = 0.0;
double prismMsSmooth = 0.0;  // Prism's GPU time per frame on the game's queue, for the panel
bool recorded = false;       // a frame went through the pipeline since it was built
uint32_t liveRW = 0, liveRH = 0;  // the render size of the last frame
uint32_t msCount[2] = {}, msTotal = 0;
bool slotRefresh[16] = {};

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i) { auto h0 = heap->GetCPUDescriptorHandleForHeapStart(); h0.ptr += SIZE_T(i) * inc; return h0; }
D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT i) { auto h0 = heap->GetGPUDescriptorHandleForHeapStart(); h0.ptr += UINT64(i) * inc; return h0; }
// The root signature, with the custom look's constants (b1) that every pass of Core may read.
void BindRoot(ID3D12GraphicsCommandList* l) {
  l->SetComputeRootSignature(root);
  const unsigned slot = gradeNext++ % kGradeRing;
  GradeSeed(gradeK, gradeFrame);
  std::memcpy(gradeMapped + slot * kGradeSlotBytes, gradeK, sizeof(gradeK));
  l->SetComputeRootConstantBufferView(3, gradeBuffer->GetGPUVirtualAddress() + UINT64(slot) * kGradeSlotBytes);
}

bool Fail(const char* what, HRESULT hr = S_OK) {
  Log("pipeline off: %s (0x%08lx)", what, hr);
  failed = true;
  failure = what;
  return false;
}

ID3D12Resource* Tex(DXGI_FORMAT f, D3D12_RESOURCE_STATES s, bool uav) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
  d.Format = f; d.SampleDesc.Count = 1; d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  ID3D12Resource* r = nullptr;
  return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, s, nullptr, __uuidof(ID3D12Resource), (void**)&r)) ? r : nullptr;
}
ID3D12Resource* Buf(UINT64 bytes) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
  d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource* r = nullptr;
  return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&r)) ? r : nullptr;
}
void BufferUav(ID3D12Resource* b, UINT count, UINT slot) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
  u.Format = DXGI_FORMAT_UNKNOWN; u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  u.Buffer.NumElements = count; u.Buffer.StructureByteStride = 4;
  device->CreateUnorderedAccessView(b, nullptr, &u, Cpu(slot));
}

void SetupPresentHook(ID3D12Device* dev);  // below, with the frame statistics

// Objects released kRing + 4 frames after they were last used.
std::vector<std::pair<uint64_t, IUnknown*>> grave;
std::vector<std::pair<uint64_t, void*>> graveNgx;
void Bury(IUnknown* o) { if (o) grave.push_back({frame, o}); }
void BuryNgx(void* hnd) { if (hnd) graveNgx.push_back({frame, hnd}); }
void Exhume() {
  auto due = [](uint64_t at) { return frame > at + kRing + 4; };
  for (auto it = grave.begin(); it != grave.end();) { if (due(it->first)) { it->second->Release(); it = grave.erase(it); } else ++it; }
  for (auto it = graveNgx.begin(); it != graveNgx.end();) { if (due(it->first)) { if (nrRelease) nrRelease(it->second); it = graveNgx.erase(it); } else ++it; }
}

// Builds the pipeline for the current output size. The parts that do not depend on the size
// (shaders, the runtime, timestamps, the Present hook) are made once; a resize rebuilds the rest.
bool Setup(ID3D12GraphicsCommandList* list, ID3D12Resource* output) {
  HRESULT hr = S_OK;
  if (!root) {
  if (FAILED(list->GetDevice(__uuidof(ID3D12Device), (void**)&device))) return Fail("GetDevice");
  // Shaders, compiled by the system's d3dcompiler_47.dll.
  using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
  auto compile = (CompileFn)(void*)GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile");
  if (!compile) return Fail("no d3dcompiler_47");
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = kSrvs;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 6;
  D3D12_ROOT_PARAMETER rp[4] = {};
  rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 16;
  rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = {1, &ranges[0]};
  rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable = {1, &ranges[1]};
  // The custom look (b1): a constant buffer, written into a ring of slots in an upload heap.
  rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[3].Descriptor.ShaderRegister = 1;
  D3D12_ROOT_SIGNATURE_DESC rd = {}; rd.NumParameters = 4; rd.pParameters = rp;
  ID3DBlob *blob = nullptr, *err = nullptr;
  hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
  if (FAILED(hr) || FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&root)))
    return Fail("root signature", hr);
  {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = kGradeSlotBytes * kGradeRing; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&gradeBuffer)) ||
        FAILED(hr = gradeBuffer->Map(0, nullptr, reinterpret_cast<void**>(&gradeMapped))))
      return Fail("look constants", hr);
  }
  for (auto [entry, pso] : {std::pair{"Reduce1", &psoReduce1}, std::pair{"Reduce2", &psoReduce2}, std::pair{"Encode", &psoEncode}, std::pair{"Compose", &psoCompose}, std::pair{"Carry", &psoCarry}, std::pair{"Bake", &psoBake}, std::pair{"Fill", &psoFill}, std::pair{"CopyGuides", &psoCopyGuides}, std::pair{"Fit", &psoFit}, std::pair{"Down", &psoDown}, std::pair{"GuidedFit", &psoGuidedFit}}) {
    ID3DBlob *cs = nullptr, *e = nullptr;
    if (FAILED(hr = compile(kShaders, std::strlen(kShaders), "prism", nullptr, nullptr, entry, "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e))) {
      Log("shader %s: %s", entry, e ? (const char*)e->GetBufferPointer() : "?");
      return Fail("shader compile", hr);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = root; pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (FAILED(hr = device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)pso))) return Fail("pipeline state", hr);
  }
  }  // once
  // Resources at the output size.
  const D3D12_RESOURCE_DESC od = output->GetDesc();
  w = UINT(od.Width); h = od.Height; tilesX = (w + 15) / 16; tilesY = (h + 15) / 16;
  origCopy = Tex(od.Format, D3D12_RESOURCE_STATE_COPY_DEST, false);
  proxy = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  neural = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  tiles = Buf(UINT64(tilesX) * tilesY * 9 * 4);  // the exposure tiles, then eight floats of guided fit per tile
  exposure = Buf(16);
  edits[0] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  edits[1] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  warps[0] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  warps[1] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  warpsL[0] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  warpsL[1] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  landedEdit = Tex(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);  // .a holds depth
  landedLuma = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);  // .x: launch log2 luma
  if (!landedLuma) return Fail("guided resources");
  scratch = Tex(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);  // .a holds depth (or a hole mark)
  mids[0] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  mids[1] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  haveLanded = false;
  if (!mids[0] || !mids[1]) return Fail("pass resources");
  if (!origCopy || !proxy || !neural || !tiles || !exposure || !edits[0] || !edits[1] || !warps[0] || !warps[1] || !warpsL[0] || !warpsL[1] || !landedEdit || !scratch) return Fail("resources");
  // Descriptors: kRing blocks of kBlock (see kBlock).
  D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kBlock * kRing;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(hr = device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&heap))) return Fail("descriptor heap", hr);
  inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
  for (UINT k = 0; k < kRing; ++k) {
    const UINT b = k * kBlock;
    sv.Format = od.Format; device->CreateShaderResourceView(origCopy, &sv, Cpu(b + 0));
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; device->CreateShaderResourceView(proxy, &sv, Cpu(b + 1)); device->CreateShaderResourceView(neural, &sv, Cpu(b + 2));
    device->CreateUnorderedAccessView(proxy, nullptr, nullptr, Cpu(b + kSetP));
    device->CreateUnorderedAccessView(landedEdit, nullptr, nullptr, Cpu(b + kSetB));
    sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; device->CreateShaderResourceView(landedEdit, &sv, Cpu(b + 9));
    device->CreateShaderResourceView(scratch, &sv, Cpu(b + 11));
    device->CreateUnorderedAccessView(scratch, nullptr, nullptr, Cpu(b + kSetO + 3));
    for (UINT set : {kSetP, kSetO, kSetG, kSetR, kSetB, kSetF, kSetL, kSetU}) { BufferUav(tiles, tilesX * tilesY * 9, b + set + 1); BufferUav(exposure, 4, b + set + 2); }
    device->CreateUnorderedAccessView(landedLuma, nullptr, nullptr, Cpu(b + kSetB + 3));
    device->CreateUnorderedAccessView(landedLuma, nullptr, nullptr, Cpu(b + kSetL + 4));
  }

  // The network starts native; each frame picks its working size (SelectNetSize). The features
  // below are made at full size either way.
  nw = w; nh = h;
  netProxy = proxy; netNeural = neural; netMids[0] = mids[0]; netMids[1] = mids[1];

  // The network: the signed runtime from the game folder, initialised on the game's device (once).
  ngx::Result r = ngx::kSuccess;
  if (!nrCreate) {
  HMODULE nr = LoadLibraryW((gameDir + L"nvngx_dlssnr.dll").c_str());
  if (!nr) return Fail("nvngx_dlssnr.dll not found beside the game");
  nrInit = (InitFn)(void*)GetProcAddress(nr, "NVSDK_NGX_D3D12_Init_Ext");
  nrCreate = (CreateFn)(void*)GetProcAddress(nr, "NVSDK_NGX_D3D12_CreateFeature");
  nrEvaluate = (EvaluateFn)(void*)GetProcAddress(nr, "NVSDK_NGX_D3D12_EvaluateFeature");
  nrRelease = (ReleaseFn)(void*)GetProcAddress(nr, "NVSDK_NGX_D3D12_ReleaseFeature");
  if (!nrInit || !nrCreate || !nrEvaluate) return Fail("runtime exports");
  r = nrInit(0x876232cULL, gameDir.c_str(), device, 0x15, nullptr);
  if (r != ngx::kSuccess) { Log("NR init 0x%08x", r); nrCreate = nullptr; return Fail("NR init"); }
  }  // once
  nrParams.vtable = ownedVtable;
  void* p = &nrParams;
  ngx::SetUI(p, "Width", nw); ngx::SetUI(p, "Height", nh); ngx::SetUI(p, "OutWidth", nw); ngx::SetUI(p, "OutHeight", nh);
  for (const char* k : {"DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth", "DLSSNR.Output.Width"}) ngx::SetUI(p, k, nw);
  for (const char* k : {"DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight", "DLSSNR.Output.Height"}) ngx::SetUI(p, k, nh);
  ngx::SetF(p, "DLSSNR.ScalingRatio", 1.f); ngx::SetF(p, "DLSSNR.Scale", 1.f);
  ngx::SetPtr(p, "DLSSNRComputeScalingRatioCallback", (void*)&ScalingRatio);
  ngx::SetI(p, "PerfQualityValue", 0);
  // The game's flags (low-resolution motion, jitter, depth sense), minus HDR and auto exposure:
  // the proxy is display-referred.
  ngx::SetI(p, "DLSS.Feature.Create.Flags", gameCreateFlags & ~((1 << 0) | (1 << 6)));
  ngx::SetUI(p, "CreationNodeMask", 1); ngx::SetUI(p, "VisibilityNodeMask", 1);
  r = nrCreate(list, ngx::kFeatureNeuralRendering, p, &nrHandle);
  if (r != ngx::kSuccess || !nrHandle) { Log("NR create 0x%08x", r); return Fail("NR create"); }
  D3D12_QUERY_HEAP_DESC qd = {}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2 * kRing;
  if (!queries && SUCCEEDED(device->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap), (void**)&queries))) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = 16 * kRing; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&stamps)))
      stamps->Map(0, nullptr, (void**)&stampData);
    // The timestamp frequency belongs to the queue type; a short-lived direct queue tells it.
    D3D12_COMMAND_QUEUE_DESC qdesc = {}; qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    if (SUCCEEDED(device->CreateCommandQueue(&qdesc, __uuidof(ID3D12CommandQueue), (void**)&q))) { q->GetTimestampFrequency(&tsFreq); q->Release(); }
  }
  Log("pipeline ready: %ux%u, game create flags 0x%x", w, h, gameCreateFlags);
  return true;
}

// The hook on ExecuteCommandLists, through a queue's vtable (slot 10; every D3D12 queue of the
// driver shares one vtable): the asynchronous network rides on it, and the panel learns the
// game's queues from it.
bool HookExecuteOnce(ID3D12Device* dev) {
  if (realExecute) return true;
  ID3D12CommandQueue* probe = nullptr;
  D3D12_COMMAND_QUEUE_DESC pd = {}; pd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(dev->CreateCommandQueue(&pd, __uuidof(ID3D12CommandQueue), (void**)&probe))) return false;
  void* target = (*reinterpret_cast<void***>(probe))[10];
  probe->Release();
  return MH_CreateHook(target, (void*)&HookExecute, (void**)&realExecute) == MH_OK && MH_EnableHook(target) == MH_OK;
}

// The reduced network's textures (proxy, output, the passes' in-between), full size, made the first
// time a frame runs below native.
bool EnsureLow() {
  if (proxyLow) return true;
  auto full = [&]() -> ID3D12Resource* {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* t = nullptr;
    return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&t)) ? t : nullptr;
  };
  proxyLow = full(); neuralLow = full(); midsLow[0] = full(); midsLow[1] = full(); gfA = full(); gfB = full();
  if (!proxyLow || !neuralLow || !midsLow[0] || !midsLow[1] || !gfA || !gfB) return Fail("reduced network resources");
  return true;
}

// This frame's working size. The native and the reduced textures swap only between asynchronous
// launches (a reduced network runs synchronously); within the reduced mode the active size simply
// follows NetScale. The height follows the width the way the shaders' LowSize() does.
bool SelectNetSize() {
  const bool want = netScale < 0.999f, reduced = netProxy != proxy;
  if (want != reduced && !inFlight) {
    if (want && !EnsureLow()) return false;
    netProxy = want ? proxyLow : proxy; netNeural = want ? neuralLow : neural;
    netMids[0] = want ? midsLow[0] : mids[0]; netMids[1] = want ? midsLow[1] : mids[1];
    historyValid = false;
  }
  const UINT before = nw;
  if (netProxy != proxy) {
    nw = std::clamp(UINT(float(w) * netScale + 0.5f), 16u, w);
    nh = std::clamp(UINT(std::floor(float(h) * float(nw) / float(w) + 0.5f)), 16u, h);
  } else {
    nw = w; nh = h;
  }
  if (nw != before) Log("network at %ux%u (%.1f%% of %ux%u)%s", nw, nh, 100.0 * nw / w, w, h, nw != w ? ", edge-aware upsample" : "");
  return true;
}

// The compute queue, its feature, the copies of the launch frame's motion and depth.
bool SetupAsync(const UpscaleCall& c) {
  HRESULT hr;
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  qd.Priority = netPriority ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  if (FAILED(hr = device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&computeQueue))) return Fail("compute queue", hr);
  for (auto& a : computeAlloc)
    if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, __uuidof(ID3D12CommandAllocator), (void**)&a))) return Fail("compute allocator", hr);
  if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, computeAlloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&computeList))) return Fail("compute list", hr);
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fenceLaunch)) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fenceStep)) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fenceDone))) return Fail("fences");
  stepId = stepSubmitted = 0;
  for (uint64_t& s : allocStep) s = 0;
  // A feature built and evaluated on the compute queue (NGX keeps a feature to its queue).
  asyncParams.vtable = ownedVtable;
  asyncParams.values = nrParams.values;  // the same creation parameters
  ngx::Result r = nrCreate(computeList, ngx::kFeatureNeuralRendering, &asyncParams, &asyncHandle);
  if (r != ngx::kSuccess || !asyncHandle) { Log("async NR create 0x%08x", r); return Fail("async NR create"); }
  computeList->Close();
  ID3D12CommandList* cl[] = {computeList};
  computeQueue->ExecuteCommandLists(1, cl);
  computeQueue->Signal(fenceDone, 0x7fffffff);
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  fenceDone->SetEventOnCompletion(0x7fffffff, ev);
  WaitForSingleObject(ev, 5000);
  CloseHandle(ev);
  // Fresh fences for the frame ids (the setup signal above used a large value).
  fenceDone->Release();
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fenceDone))) return Fail("fence");
  // Render-resolution copies of the launch frame's motion and depth.
  auto copyOf = [&](DXGI_FORMAT fmt) -> ID3D12Resource* {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = c.renderWidth; d.Height = c.renderHeight; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* r2 = nullptr;
    return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&r2)) ? r2 : nullptr;
  };
  if (!motionCopy) motionCopy = copyOf(DXGI_FORMAT_R16G16_FLOAT);
  if (!depthCopy) depthCopy = copyOf(DXGI_FORMAT_R32_FLOAT);
  if (!motionCopy || !depthCopy) return Fail("motion/depth copies");
  if (!HookExecuteOnce(device)) return Fail("ExecuteCommandLists hook");
  if (!netQueries) {
    D3D12_QUERY_HEAP_DESC nq = {}; nq.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; nq.Count = 4;
    D3D12_HEAP_PROPERTIES rb = {}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 32; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(device->CreateQueryHeap(&nq, __uuidof(ID3D12QueryHeap), (void**)&netQueries)) &&
        SUCCEEDED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&netStamps)))
      netStamps->Map(0, nullptr, (void**)&netStampData);
  }
  computeQueue->GetTimestampFrequency(&netTsFreq);
  Log("async network ready (compute queue, %s priority, ExecuteCommandLists hooked)", netPriority ? "high" : "normal");
  return true;
}

// Drops the compute queue and its feature (between launches) so the next frame builds them again,
// with the priority now asked for.
void ReleaseAsync() {
  BuryNgx(asyncHandle);
  asyncHandle = nullptr;
  for (void*& f : moreAsync.h) { BuryNgx(f); f = nullptr; }
  Bury(computeList);
  for (auto& a : computeAlloc) { Bury(a); a = nullptr; }
  Bury(computeQueue); Bury(fenceLaunch); Bury(fenceStep); Bury(fenceDone);
  computeList = nullptr; computeQueue = nullptr; fenceLaunch = nullptr; fenceStep = nullptr; fenceDone = nullptr;
  stepList = nullptr; asyncRun = PassRun{};
}

// Everything that depends on the output or render size, for a rebuild at a new size. Nothing is
// released while the GPU might still use it: it waits in the graveyard for a few frames.
void Teardown() {
  ID3D12Resource** slots[] = {&origCopy, &proxy, &neural, &tiles, &exposure, &edits[0], &edits[1], &warps[0], &warps[1], &warpsL[0], &warpsL[1], &landedEdit, &landedLuma, &scratch,
                              &motionCopy, &depthCopy, &recMotion, &recDepth, &mids[0], &mids[1], &proxyLow, &neuralLow, &midsLow[0], &midsLow[1], &gfA, &gfB};
  for (ID3D12Resource** r : slots) {
    Bury(*r);
    *r = nullptr;
  }
  Bury(heap);
  heap = nullptr;
  BuryNgx(nrHandle);
  nrHandle = nullptr;
  for (void*& f : morePasses.h) { BuryNgx(f); f = nullptr; }
  if (computeQueue) ReleaseAsync();
  for (RecFrame& rf : recFrames) { Bury(rf.color); Bury(rf.motion); Bury(rf.depth); }
  recFrames.clear(); recordLeft = 0; recordDoneAt = 0;
  historyValid = false;
  haveLanded = false;
  recorded = false;
  current = 0;
  w = h = 0;
}

// ------------------------------------------------------------------ frame pacing and statistics
// Times in milliseconds on the performance counter.
double NowMs() {
  static LARGE_INTEGER freq = {};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// A frame cap on the rendered frames: sleep on a high-resolution timer to about 1 ms before the
// target, then spin. Falling behind restarts the schedule instead of bursting to catch up.
// FpsCap=-1 is automatic: the cap follows the frames' own work time (the interval minus the
// limiter's sleep), its 85th percentile plus 2%, re-picked every 60 frames. The slow frames of a
// cycle set the pace and the fast ones wait instead of running ahead: the saw-tooth goes flat.
double autoPeriod = 0.0;

void Pace() {
  static double next = 0.0, lastExit = 0.0;
  static std::vector<float> work;
  if (fpsCap == 0) { next = 0.0; lastExit = 0.0; autoPeriod = 0.0; work.clear(); paceSleptMs = 0.0; return; }
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  double now = NowMs();
  if (fpsCap < 0) {
    if (lastExit > 0.0 && now - lastExit < 250.0) work.push_back(float(now - lastExit));
    if (work.size() >= 60) {
      std::nth_element(work.begin(), work.begin() + work.size() * 85 / 100, work.end());
      const double p85 = work[work.size() * 85 / 100] * 1.02;
      autoPeriod = autoPeriod == 0.0 ? p85 : autoPeriod * 0.7 + p85 * 0.3;
      work.clear();
    }
  }
  const double period = fpsCap > 0 ? 1000.0 / fpsCap : autoPeriod;
  paceSleptMs = 0.0;
  if (period <= 0.0) { lastExit = NowMs(); return; }
  if (next == 0.0 || now >= next) { next = now + period; lastExit = now; return; }
  paceSleptMs = next - now;
  while (now < next) {
    const double left = next - now;
    if (left > 1.5 && timer) {
      LARGE_INTEGER due;
      due.QuadPart = -LONGLONG((left - 1.0) * 10000.0);
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 100);
    } else {
      YieldProcessor();
    }
    now = NowMs();
  }
  next += period;
  lastExit = now;
}

// Frame-time series: presents (every displayed frame, generated ones included) and rendered
// frames (the game's upscale calls). Each window logs mean frame rate, p99 frame time and the
// standard deviation; a change of settings restarts the window after a warm-up.
struct Series {
  std::vector<float> v;
  double last = 0.0;
};
std::mutex statsMutex;
Series sPresent, sRender;
double statsWarmEnd = 0.0, statsEnd = 0.0;
int statsSec = 10;
std::string label;
FILE* frameCsv = nullptr;
bool frameLog = false;
uint32_t statLaunches = 0, statLands = 0, statAgeSum = 0, statAgeMax = 0;
// The panel's view: the last kFrameHistory rendered-frame intervals, and one second of present
// and render times for the live rates.
float renderRing[kFrameHistory] = {};
uint32_t renderRingCount = 0;
std::deque<double> presentTimes, renderTimes;

void Record(Series& sr, double t, char kind) {
  std::deque<double>& times = kind == 'P' ? presentTimes : renderTimes;
  times.push_back(t);
  while (times.size() > 1 && t - times.front() > 1000.0) times.pop_front();
  if (sr.last > 0.0) {
    const double d = t - sr.last;
    if (kind == 'R' && d < 250.0) renderRing[renderRingCount++ % kFrameHistory] = float(d);
    if (d < 250.0) sr.v.push_back(float(d));  // longer: a load or a pause
    if (frameCsv) std::fprintf(frameCsv, "%.3f,%c,%.3f\n", t, kind, d);
  }
  sr.last = t;
}

struct Summary { double fps, p99, sd; size_t n; };
Summary Summarise(std::vector<float>& v) {
  Summary r = {0, 0, 0, v.size()};
  if (v.size() < 10) return r;
  double sum = 0.0, sq = 0.0;
  for (float x : v) { sum += x; sq += double(x) * x; }
  const double mean = sum / v.size();
  r.fps = 1000.0 / mean;
  r.sd = std::sqrt(std::max(0.0, sq / v.size() - mean * mean));
  const size_t k = std::min(v.size() - 1, size_t(std::ceil(0.99 * v.size())) - 1);
  std::nth_element(v.begin(), v.begin() + k, v.end());
  r.p99 = v[k];
  return r;
}

void StatsTick() {
  const double t = NowMs();
  std::lock_guard<std::mutex> lock(statsMutex);
  auto clear = [&] { sPresent.v.clear(); sRender.v.clear(); statLaunches = statLands = statAgeSum = statAgeMax = 0; };
  if (t < statsWarmEnd) { clear(); return; }
  if (statsEnd == 0.0) { clear(); statsEnd = t + statsSec * 1000.0; return; }
  if (t < statsEnd) return;
  const Summary p = Summarise(sPresent.v), r = Summarise(sRender.v);
  Log("stats [%s] present %.1f fps p99 %.2f ms sd %.2f ms (%zu) | render %.1f fps p99 %.2f ms sd %.2f ms (%zu) | network %u launches, %u lands, age %.2f avg %u max | interval %.2f | network gpu %.2f ms | cap %.1f fps",
      label.c_str(), p.fps, p.p99, p.sd, p.n, r.fps, r.p99, r.sd, r.n, statLaunches, statLands, statLands ? double(statAgeSum) / statLands : 0.0, statAgeMax,
      statLaunches ? double(r.n) / statLaunches : 0.0, netGpuMs, fpsCap > 0 ? double(fpsCap) : autoPeriod > 0.0 ? 1000.0 / autoPeriod : 0.0);
  if (frameCsv) std::fflush(frameCsv);
  clear();
  statsEnd = t + statsSec * 1000.0;
}

// NetScaleAuto: this frame's size, from the frame rates of the last second and Prism's measured
// GPU time. The sizes are kNetLevels between NetScaleMin and NetScale.
void AutoNetScale() {
  // The asynchronous network (cadence 2 and up) runs native.
  if (!netAuto || cadence != 1) { netScale = cadence == 1 ? netScaleUser : 1.0f; netAutoScale = netScaleUser; autoShort = autoRoom = 0; autoDownFrom = 0.0; return; }
  const double now = NowMs();
  if (netAutoScale > netScaleUser + 1e-3f) netAutoScale = netScaleUser;
  netScale = netAutoScale;
  if (now < autoNextCheck) return;
  autoNextCheck = now + 1000.0;
  double renderFps = 0.0;
  {
    std::lock_guard<std::mutex> lock(statsMutex);
    if (renderTimes.size() > 2 && renderTimes.back() > renderTimes.front()) renderFps = (renderTimes.size() - 1) * 1000.0 / (renderTimes.back() - renderTimes.front());
  }
  if (renderFps <= 0.0 || now < autoHoldUntil) return;
  const double frameMs = 1000.0 / renderFps, targetMs = 1000.0 / fpsTarget;
  // The network's cost model (bench: 0.87 ms a call + 1.11 ms per megapixel), scaled to what this
  // GPU measured at the current size.
  const double mp = double(w) * h / 1e6;
  auto model = [&](float sc) { return passes * (0.87 + 1.11 * mp * sc * sc); };
  const double k = prismMsSmooth > 0.0 ? prismMsSmooth / model(netAutoScale) : 1.0;
  auto change = [&](float next, const char* why) {
    Log("auto network size: %.1f%% -> %.1f%% (%.1f fps rendered, target %d)%s", netAutoScale * 100.0, next * 100.0, renderFps, fpsTarget, why);
    netAutoScale = next; netScale = next; autoShort = autoRoom = 0; autoHoldUntil = now + 3000.0;
  };
  // The first look after a step down: did it pay?
  if (autoDownFrom > 0.0) {
    const double got = autoDownFrom - frameMs, want = autoDownGain;
    autoDownFrom = 0.0;
    if (got < want / 3.0 && autoDownPrev <= netScaleUser + 1e-3f) {
      autoBlockUntil = now + 30000.0;
      change(autoDownPrev, " (the smaller size did not help: limited elsewhere)");
      return;
    }
  }
  const float up = NetLevelUp(netAutoScale, netScaleUser), down = NetLevelDown(netAutoScale, netAutoMin);
  autoShort = frameMs > targetMs * 1.03 ? autoShort + 1 : 0;
  autoRoom = up > netAutoScale + 1e-3f && frameMs + k * (model(up) - model(netAutoScale)) < targetMs * 0.95 ? autoRoom + 1 : 0;
  if (autoShort >= 2 && down < netAutoScale - 1e-3f && now >= autoBlockUntil) {
    autoDownFrom = frameMs; autoDownGain = k * (model(netAutoScale) - model(down)); autoDownPrev = netAutoScale;
    change(down, "");
  } else if (autoRoom >= 3) {
    change(up, "");
  }
}

// The real DXGI Present (and Present1), found through a throwaway swap chain made by the system's
// dxgi.dll, so every present of the process counts, through any wrapper.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PresentFn realPresent = nullptr;
Present1Fn realPresent1 = nullptr;
thread_local int presentDepth = 0;

void OnPresent(UINT flags) {
  if (flags & DXGI_PRESENT_TEST) return;
  std::lock_guard<std::mutex> lock(statsMutex);
  Record(sPresent, NowMs(), 'P');
}
HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
  if (presentDepth++ == 0) { OnPresent(flags); if (!(flags & DXGI_PRESENT_TEST)) OverlayPresent(sc); }
  const HRESULT hr = realPresent(sc, sync, flags);
  --presentDepth;
  return hr;
}
HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
  if (presentDepth++ == 0) { OnPresent(flags); if (!(flags & DXGI_PRESENT_TEST)) OverlayPresent(sc); }
  const HRESULT hr = realPresent1(sc, sync, flags, pp);
  --presentDepth;
  return hr;
}

void SetupPresentHook(ID3D12Device* dev) {
  wchar_t path[MAX_PATH];
  GetSystemDirectoryW(path, MAX_PATH);
  lstrcatW(path, L"\\dxgi.dll");
  using FactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
  auto create = (FactoryFn)(void*)GetProcAddress(LoadLibraryW(path), "CreateDXGIFactory2");
  IDXGIFactory2* factory = nullptr;
  if (!create || FAILED(create(0, __uuidof(IDXGIFactory2), (void**)&factory))) { Log("present timing off: no DXGI factory"); return; }
  HWND wnd = CreateWindowExW(0, L"STATIC", L"prism", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, nullptr, nullptr);
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ID3D12CommandQueue* q = nullptr;
  IDXGISwapChain1* sc = nullptr;
  DXGI_SWAP_CHAIN_DESC1 d = {};
  d.Width = 8; d.Height = 8; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  if (!HookExecuteOnce(dev)) Log("panel: ExecuteCommandLists not hooked");
  if (wnd && dev && SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q)) &&
      SUCCEEDED(factory->CreateSwapChainForHwnd(q, wnd, &d, nullptr, nullptr, &sc))) {
    void** vt = *reinterpret_cast<void***>(sc);
    const bool ok = MH_CreateHook(vt[8], (void*)&HookPresent, (void**)&realPresent) == MH_OK && MH_EnableHook(vt[8]) == MH_OK &&
                    MH_CreateHook(vt[22], (void*)&HookPresent1, (void**)&realPresent1) == MH_OK && MH_EnableHook(vt[22]) == MH_OK;
    Log(ok ? "present timing on (DXGI Present hooked)" : "present timing off: hook failed");
    if (ok) OverlaySetup(vt);
  } else {
    Log("present timing off: no probe swap chain");
  }
  if (sc) sc->Release();
  if (q) q->Release();
  factory->Release();
  if (wnd) DestroyWindow(wnd);
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
  l->ResourceBarrier(1, &br);
}
void UavBarrier(ID3D12GraphicsCommandList* l) {
  D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  l->ResourceBarrier(1, &br);
}

// One run of the network's passes (PassRun): `first` runs on the proxy, later passes on the pass
// before, the last one writes `neural`. The passes in between ping-pong through two textures; one
// read by the pass before goes back to UAV before it is written again.

// Starts a run: makes the features of the later passes on first use (each starts with a reset); if
// one can't be made, the passes made so far are the run.
ngx::Result BeginPasses(ID3D12GraphicsCommandList* l, PassFeatures& more, void* p, bool reset, PassRun& run) {
  int n = passes;
  for (int i = 1; i < n; ++i) {
    if (more.h[i - 1]) continue;
    const ngx::Result cr = nrCreate(l, ngx::kFeatureNeuralRendering, p, &more.h[i - 1]);
    if (cr != ngx::kSuccess || !more.h[i - 1]) { Log("pass %d feature 0x%08x: running %d pass(es)", i + 1, cr, i); more.h[i - 1] = nullptr; passes = n = i; break; }
    more.fresh[i - 1] = true;
  }
  run = PassRun{};
  run.n = n; run.reset = reset; run.in = netProxy;
  return ngx::kSuccess;
}

// Runs the run's passes up to `until` (exclusive) on this list; the last pass returns the
// in-between textures to UAV.
ngx::Result RunPasses(ID3D12GraphicsCommandList* l, void* first, PassFeatures& more, void* p, PassRun& run, int until) {
  const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ngx::Result r = ngx::kSuccess;
  for (; run.next < std::min(until, run.n) && r == ngx::kSuccess; ++run.next) {
    const int i = run.next, m = i & 1;
    ID3D12Resource* out = i == run.n - 1 ? netNeural : netMids[m];
    if (out == netMids[m] && run.readable[m]) { Barrier(l, out, SRV, UAV); run.readable[m] = false; }
    bool rs = run.reset;
    if (i > 0) { rs = rs || more.fresh[i - 1]; more.fresh[i - 1] = false; }
    ngx::SetRes(p, "DLSSNR.Color", run.in); ngx::SetRes(p, "DLSSNR.Output", out);
    ngx::SetI(p, "DLSSNR.Reset", rs ? 1 : 0);
    r = nrEvaluate(l, i == 0 ? first : more.h[i - 1], p, nullptr);
    if (i < run.n - 1) { Barrier(l, out, UAV, SRV); run.readable[m] = true; }
    run.in = out;
  }
  if (run.next >= run.n)
    for (int m = 0; m < 2; ++m) if (run.readable[m]) { Barrier(l, netMids[m], SRV, UAV); run.readable[m] = false; }
  return r;
}

// All passes on one list.
ngx::Result EvaluatePasses(ID3D12GraphicsCommandList* l, void* first, PassFeatures& more, void* p, bool reset) {
  PassRun run;
  BeginPasses(l, more, p, reset, run);
  return RunPasses(l, first, more, p, run, run.n);
}

// Opens the compute list for the next step, on an allocator whose last step has finished (eight
// steps back; a wait here would mean the GPU is that far behind).
void OpenStep() {
  ++stepId;
  const int a = int(stepId % kAllocs);
  if (allocStep[a] && fenceStep->GetCompletedValue() < allocStep[a]) {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fenceStep->SetEventOnCompletion(allocStep[a], ev);
    WaitForSingleObject(ev, 1000);
    CloseHandle(ev);
  }
  allocStep[a] = stepId;
  computeAlloc[a]->Reset();
  computeList->Reset(computeAlloc[a], nullptr);
}

int lastStepPasses = 1;  // passes in the step whose time the network stamps hold

// Records the launch's passes up to `until` on the open step, timed, and closes it.
ngx::Result RunStep(int until) {
  const UINT q0 = UINT(stepId & 1) * 2;
  if (netQueries) computeList->EndQuery(netQueries, D3D12_QUERY_TYPE_TIMESTAMP, q0);
  const int before = asyncRun.next;
  const ngx::Result r = RunPasses(computeList, asyncHandle, moreAsync, &asyncParams, asyncRun, until);
  if (netQueries && netStamps) {
    computeList->EndQuery(netQueries, D3D12_QUERY_TYPE_TIMESTAMP, q0 + 1);
    computeList->ResolveQueryData(netQueries, D3D12_QUERY_TYPE_TIMESTAMP, q0, 2, netStamps, UINT64(q0) * 8);
  }
  computeList->Close();
  lastStepPasses = std::max(asyncRun.next - before, 1);
  return r;
}

// prism-core.ini beside the game: defaults and hotkeys, written by the Prism NR app. It is
// re-read whenever it changes, so the app can tune a running game.
struct Keys { int toggle = VK_F8, cadence = VK_F2, async = VK_F3, split = VK_F4, capture = VK_F11; } keys;
float intensity = 1.0f, guardStrength = 1.0f;
FILETIME configTime = {};

// "F1".."F24", a virtual-key number, or "none" to disable the key.
int ParseKey(const std::wstring& s, int fallback) {
  if (s.empty()) return fallback;
  if (!lstrcmpiW(s.c_str(), L"none")) return 0;
  static const std::pair<const wchar_t*, int> names[] = {{L"Home", VK_HOME}, {L"End", VK_END}, {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE},
                                                         {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Pause", VK_PAUSE}, {L"ScrollLock", VK_SCROLL}};
  for (const auto& [name, vk] : names) if (!lstrcmpiW(s.c_str(), name)) return vk;
  if ((s[0] == L'F' || s[0] == L'f') && s.size() > 1) {
    const int n = _wtoi(s.c_str() + 1);
    return n >= 1 && n <= 24 ? VK_F1 + n - 1 : fallback;
  }
  const int n = _wtoi(s.c_str());
  return n > 0 && n < 256 ? n : fallback;
}

// The panel's changes: applied on the render thread (ApplyPending), saved by SaveLiveSettings.
// While one is not saved yet, the file is not read back (it still holds the old values).
std::mutex configMutex, liveMutex;
LiveSettings pending;
std::atomic<bool> havePending{false}, unsaved{false};
std::wstring notice;
void Notice(const std::wstring& s) { std::lock_guard<std::mutex> lock(liveMutex); notice = s; }

void LoadConfig() {
  if (unsaved) return;
  std::lock_guard<std::mutex> configLock(configMutex);
  const std::wstring path = gameDir + L"prism-core.ini";
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
  if (CompareFileTime(&fa.ftLastWriteTime, &configTime) == 0) return;
  if (configTime.dwLowDateTime || configTime.dwHighDateTime) Notice(L"已应用 Prism NR 里的设置");
  configTime = fa.ftLastWriteTime;
  auto Get = [&](const wchar_t* key) {
    wchar_t buf[1024] = {};  // Grade= runs to a few hundred characters
    GetPrivateProfileStringW(L"Prism Core", key, L"", buf, 1024, path.c_str());
    return std::wstring(buf);
  };
  auto Num = [&](const wchar_t* key, double fallback, double lo, double hi) {
    const std::wstring v = Get(key);
    const double x = v.empty() ? fallback : _wtof(v.c_str());
    return x < lo ? lo : x > hi ? hi : x;
  };
  const int oldCadence = cadence, oldPasses = passes;
  const bool oldAsync = asyncMode, oldEnabled = enabled;
  enabled = Num(L"Enabled", enabled ? 1 : 0, 0, 1) != 0;
  cadence = int(Num(L"Cadence", cadence, 0, 8));
  passes = int(Num(L"Passes", passes, 1, kMaxPasses));
  stagger = Num(L"Stagger", stagger ? 1 : 0, 0, 1) != 0;
  guided = Num(L"Guided", guided ? 1 : 0, 0, 1) != 0;
  upFilter = int(Num(L"UpFilter", upFilter, 0, 1));
  {
    // A number, or the look's name.
    const std::wstring lk = Get(L"Look");
    static const wchar_t* names[] = {L"default", L"faithful", L"vivid", L"muted", L"punchy", L"custom"};
    for (int i = 0; i < 6; ++i) if (!lstrcmpiW(lk.c_str(), names[i])) look = i;
    if (!lk.empty() && lk[0] >= L'0' && lk[0] <= L'9') look = std::clamp(_wtoi(lk.c_str()), 0, 5);
    for (int i = 0; i < kGradeSlots; ++i) grades[i] = GradeParse(Get(GradeKey(i)));
    gradeSlot = int(Num(L"GradeSlot", gradeSlot, 0, kGradeSlots - 1));
    GradeToShader(grades[gradeSlot], gradeK);
  }
  netReset = Num(L"NetReset", netReset ? 1 : 0, 0, 1) != 0;
  netScaleUser = float(Num(L"NetScale", netScaleUser, 0.5, 1.0));
  netAuto = Num(L"NetScaleAuto", netAuto ? 1 : 0, 0, 1) != 0;
  fpsTarget = int(Num(L"FpsTarget", fpsTarget, 20, 360));
  netAutoMin = float(Num(L"NetScaleMin", netAutoMin, 0.5, 1.0));
  {
    const std::wstring land = Get(L"Land");
    if (!land.empty()) landPoll = !lstrcmpiW(land.c_str(), L"poll");
    const std::wstring prio = Get(L"NetPriority");
    if (!prio.empty()) netPriority = !lstrcmpiW(prio.c_str(), L"high") ? 1 : 0;
    const std::wstring nm = Get(L"NetMotion");
    if (!nm.empty()) exactMotion = lstrcmpiW(nm.c_str(), L"approx") != 0;
    const std::wstring lb = Get(L"Label");
    label.assign(lb.begin(), lb.end());  // ASCII labels
  }
  fpsCap = int(Num(L"FpsCap", fpsCap, -1, 500));  // -1: automatic (see Pace)
  stability = float(Num(L"Stability", stability, 0, 0.9));
  steady = float(Num(L"Steady", steady, 0, 0.9));
  steadyBand = float(Num(L"SteadyBand", steadyBand, 0.01, 0.5));
  detailKeep = float(Num(L"DetailKeep", detailKeep, 0, 1));
  {
    const int asked = int(Num(L"Record", 0, 0, 240));
    if (asked != recordAsked) { recordAsked = asked; if (asked > 0 && recFrames.empty()) recordLeft = asked; }
  }
  statsSec = int(Num(L"StatsSeconds", statsSec, 2, 120));
  frameLog = Num(L"FrameLog", frameLog ? 1 : 0, 0, 1) != 0;
  if (frameLog && !frameCsv) {
    frameCsv = _wfopen((gameDir + L"prism-frames.csv").c_str(), L"w");
    if (frameCsv) std::fprintf(frameCsv, "t_ms,kind,interval_ms\n");
  }
  {
    std::lock_guard<std::mutex> lock(statsMutex);
    statsWarmEnd = NowMs() + 3000.0;  // let the new settings settle before measuring
    statsEnd = 0.0;
  }
  asyncMode = Num(L"Async", asyncMode ? 1 : 0, 0, 1) != 0;
  intensity = float(Num(L"Intensity", intensity, 0, 3));  // above 1 amplifies the network's edit
  guardStrength = float(Num(L"Guard", guardStrength, 0, 1));
  split = Num(L"Split", split ? 1 : 0, 0, 1) != 0;
  splitPos = float(Num(L"SplitPos", splitPos, 0.02, 0.98));
  keys.toggle = ParseKey(Get(L"KeyToggle"), keys.toggle);
  keys.cadence = ParseKey(Get(L"KeyCadence"), keys.cadence);
  keys.async = ParseKey(Get(L"KeyAsync"), keys.async);
  keys.split = ParseKey(Get(L"KeySplit"), keys.split);
  keys.capture = ParseKey(Get(L"KeyCapture"), keys.capture);
  if (cadence != oldCadence || passes != oldPasses || asyncMode != oldAsync || enabled != oldEnabled) historyValid = false;
  if (Log) Log("prism-core.ini: %s, network every %d frame(s) x%d pass(es), async %s, land %s, %s priority, cap %d fps, intensity %.2f, guard %.2f, look %d [%s]", enabled ? "on" : "off", cadence, passes, asyncMode ? "on" : "off", landPoll ? "poll" : "wait", netPriority ? "high" : "normal", fpsCap, intensity, guardStrength, look, label.c_str());
}

ID3D12Resource* Readback(ID3D12Resource* src, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
  D3D12_RESOURCE_DESC d = src->GetDesc();
  UINT rows; UINT64 rowBytes, total;
  device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
  bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* rb = nullptr;
  return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb)) ? rb : nullptr;
}

void CopyOut(ID3D12GraphicsCommandList* l, ID3D12Resource* src, ID3D12Resource* rb, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
  D3D12_TEXTURE_COPY_LOCATION dst = {}, s2 = {};
  dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
  s2.pResource = src; s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  l->CopyTextureRegion(&dst, 0, 0, 0, &s2, nullptr);
}

// One frame of the recording: the upscaled frame from our copy, motion and depth through the
// guide-copy shader (SRV reads, so the game's resource states are never touched).
void RecordFrame(const UpscaleCall& c, ID3D12GraphicsCommandList* l, UINT ring) {
  const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (!recMotion) {
    auto make = [&](DXGI_FORMAT fmt) -> ID3D12Resource* {
      D3D12_RESOURCE_DESC d = {};
      d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = c.renderWidth; d.Height = c.renderHeight; d.DepthOrArraySize = 1; d.MipLevels = 1;
      d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      ID3D12Resource* r = nullptr;
      return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, UAV, nullptr, __uuidof(ID3D12Resource), (void**)&r)) ? r : nullptr;
    };
    recMotion = make(DXGI_FORMAT_R16G16_FLOAT);
    recDepth = make(DXGI_FORMAT_R32_FLOAT);
    if (!recMotion || !recDepth) { Log("record: no textures"); recordLeft = 0; return; }
  }
  RecFrame rf;
  rf.color = Readback(origCopy, rf.fc);
  rf.motion = Readback(recMotion, rf.fm);
  rf.depth = Readback(recDepth, rf.fd);
  if (!rf.color || !rf.motion || !rf.depth) { Log("record: out of readback memory after %zu frames", recFrames.size()); recordLeft = 0; return; }
  rf.mvx = c.motionScaleX; rf.mvy = c.motionScaleY; rf.reset = c.reset; rf.rw = c.renderWidth; rf.rh = c.renderHeight;
  device->CreateUnorderedAccessView(recMotion, nullptr, nullptr, Cpu(ring + kSetR));
  device->CreateUnorderedAccessView(recDepth, nullptr, nullptr, Cpu(ring + kSetR + 3));
  ID3D12DescriptorHeap* heaps[] = {heap};
  l->SetDescriptorHeaps(1, heaps);
  BindRoot(l);
  const Constants k = {w, h, tilesX * tilesY, tilesX, intensity, guardStrength, 0.05f, 0u, c.renderWidth, c.renderHeight, c.motionScaleX, c.motionScaleY, 0.05f, 0u, 1.0f, 1.0f};
  l->SetComputeRoot32BitConstants(0, 16, &k, 0);
  l->SetComputeRootDescriptorTable(1, Gpu(ring));
  l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetR));
  l->SetPipelineState(psoCopyGuides); l->Dispatch((c.renderWidth + 15) / 16, (c.renderHeight + 15) / 16, 1);
  UavBarrier(l);
  Barrier(l, recMotion, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(l, recDepth, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(l, origCopy, SRV, D3D12_RESOURCE_STATE_COPY_SOURCE);
  CopyOut(l, origCopy, rf.color, rf.fc);
  CopyOut(l, recMotion, rf.motion, rf.fm);
  CopyOut(l, recDepth, rf.depth, rf.fd);
  Barrier(l, origCopy, D3D12_RESOURCE_STATE_COPY_SOURCE, SRV);
  Barrier(l, recMotion, D3D12_RESOURCE_STATE_COPY_SOURCE, UAV);
  Barrier(l, recDepth, D3D12_RESOURCE_STATE_COPY_SOURCE, UAV);
  recFrames.push_back(rf);
  if (--recordLeft == 0) recordDoneAt = frame + kRing + 2;  // by then the GPU has long finished them
}

void WriteRecording() {
  recordDoneAt = 0;
  const std::wstring dir = gameDir + L"prism-record\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  auto dump = [&](ID3D12Resource* rb, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, const std::wstring& name) {
    void* data = nullptr;
    if (FAILED(rb->Map(0, nullptr, &data))) return;
    if (FILE* fo = _wfopen((dir + name).c_str(), L"wb")) { std::fwrite(data, 1, size_t(fp.Footprint.RowPitch) * fp.Footprint.Height, fo); std::fclose(fo); }
    rb->Unmap(0, nullptr);
  };
  FILE* meta = _wfopen((dir + L"meta.txt").c_str(), L"w");
  for (size_t i = 0; i < recFrames.size(); ++i) {
    RecFrame& rf = recFrames[i];
    const std::wstring n = std::to_wstring(i);
    dump(rf.color, rf.fc, L"color_" + n + L".raw");
    dump(rf.motion, rf.fm, L"motion_" + n + L".raw");
    dump(rf.depth, rf.fd, L"depth_" + n + L".raw");
    if (meta) std::fprintf(meta, "%zu color %u %u %u %d motion %u %u %u depth %u %u %u mvscale %.6f %.6f reset %d flags %d\n", i,
                           rf.fc.Footprint.Width, rf.fc.Footprint.Height, rf.fc.Footprint.RowPitch, int(rf.fc.Footprint.Format),
                           rf.fm.Footprint.Width, rf.fm.Footprint.Height, rf.fm.Footprint.RowPitch,
                           rf.fd.Footprint.Width, rf.fd.Footprint.Height, rf.fd.Footprint.RowPitch, rf.mvx, rf.mvy, rf.reset, gameCreateFlags);
    rf.color->Release(); rf.motion->Release(); rf.depth->Release();
  }
  if (meta) std::fclose(meta);
  Log("record: %zu frames written to prism-record", recFrames.size());
  recFrames.clear();
}

}  // namespace

void PipelineInit(const std::wstring& dir, int createFlags, LogFn log, ID3D12Device* dev) {
  gameDir = dir;
  gameCreateFlags = createFlags;
  Log = log;
  LoadConfig();
  static bool presentHooked = false;
  if (!presentHooked && dev) { presentHooked = true; SetupPresentHook(dev); }
}

LiveSettings GetLiveSettings() {
  if (havePending) { std::lock_guard<std::mutex> lock(liveMutex); return pending; }
  LiveSettings s;
  s.enabled = enabled; s.cadence = cadence; s.passes = passes; s.intensity = intensity; s.netScale = netScaleUser; s.guard = guardStrength;
  s.netAuto = netAuto; s.fpsTarget = fpsTarget;
  s.async = asyncMode; s.split = split; s.splitPos = splitPos; s.look = look; s.gradeSlot = gradeSlot;
  for (int i = 0; i < kGradeSlots; ++i) s.grade[i] = grades[i];
  return s;
}

void SetLiveSettings(const LiveSettings& s) {
  std::lock_guard<std::mutex> lock(liveMutex);
  pending = s;
  havePending = true;
  unsaved = true;
}

void SaveLiveSettings() {
  if (!unsaved) return;
  const LiveSettings s = GetLiveSettings();
  std::lock_guard<std::mutex> configLock(configMutex);
  const std::wstring path = gameDir + L"prism-core.ini";
  auto put = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"Prism Core", k, v.c_str(), path.c_str()); };
  wchar_t b[32];
  put(L"Enabled", s.enabled ? L"1" : L"0");
  put(L"Cadence", std::to_wstring(s.cadence));
  put(L"Passes", std::to_wstring(s.passes));
  put(L"Async", s.async ? L"1" : L"0");
  std::swprintf(b, 32, L"%.2f", s.intensity); put(L"Intensity", b);
  std::swprintf(b, 32, L"%.2f", s.guard); put(L"Guard", b);
  std::swprintf(b, 32, L"%.4f", s.netScale); put(L"NetScale", b);
  put(L"NetScaleAuto", s.netAuto ? L"1" : L"0");
  put(L"FpsTarget", std::to_wstring(s.fpsTarget));
  put(L"Split", s.split ? L"1" : L"0");
  std::swprintf(b, 32, L"%.3f", s.splitPos); put(L"SplitPos", b);
  put(L"Look", std::to_wstring(s.look));
  for (int i = 0; i < kGradeSlots; ++i) put(GradeKey(i), GradeFormat(s.grade[i]));
  put(L"GradeSlot", std::to_wstring(s.gradeSlot));
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) configTime = fa.ftLastWriteTime;  // our own write: not read back
  unsaved = false;
  if (Log) Log("prism-core.ini: saved from the in-game panel");
}

void GetLiveStats(LiveStats* o) {
  o->running = heap && recorded && !failed && enabled;
  o->failure = failure;
  o->outW = w; o->outH = h; o->netW = nw; o->netH = nh; o->renderW = liveRW; o->renderH = liveRH;
  o->prismMs = float(prismMsSmooth);
  o->netMs = asyncMode && computeQueue ? float(netGpuMs) : 0.0f;
  std::lock_guard<std::mutex> lock(statsMutex);
  auto rate = [](const std::deque<double>& t) { return t.size() > 2 && t.back() > t.front() ? float((t.size() - 1) * 1000.0 / (t.back() - t.front())) : 0.0f; };
  o->presentFps = rate(presentTimes);
  o->renderFps = rate(renderTimes);
  const uint32_t n = std::min<uint32_t>(renderRingCount, kFrameHistory);
  o->frames = int(n);
  for (uint32_t i = 0; i < n; ++i) o->renderMs[i] = renderRing[(renderRingCount - n + i) % kFrameHistory];
}

std::wstring TakeLiveNotice() {
  std::lock_guard<std::mutex> lock(liveMutex);
  std::wstring s;
  s.swap(notice);
  return s;
}

std::wstring GameDir() { return gameDir; }
LogFn PipelineLogger() { return Log; }

// The panel's change, on the render thread: the settings the next frame records with.
void ApplyPending() {
  ++gradeFrame;  // the grain's seed
  if (!havePending) return;
  LiveSettings s;
  {
    std::lock_guard<std::mutex> lock(liveMutex);
    s = pending;
    havePending = false;
  }
  if (s.enabled != enabled || s.cadence != cadence || s.passes != passes || s.async != asyncMode) historyValid = false;
  enabled = s.enabled; cadence = std::clamp(s.cadence, 0, 8); passes = std::clamp(s.passes, 1, kMaxPasses); asyncMode = s.async;
  intensity = std::clamp(s.intensity, 0.0f, 3.0f); guardStrength = std::clamp(s.guard, 0.0f, 1.0f); netScaleUser = std::clamp(s.netScale, 0.5f, 1.0f);
  netAuto = s.netAuto; fpsTarget = std::clamp(s.fpsTarget, 20, 360);
  split = s.split; splitPos = std::clamp(s.splitPos, 0.02f, 0.98f);
  look = std::clamp(s.look, 0, 5);
  const int slot = std::clamp(s.gradeSlot, 0, kGradeSlots - 1);
  if (slot != gradeSlot || s.grade[slot] != grades[gradeSlot]) { gradeSlot = slot; for (int i = 0; i < kGradeSlots; ++i) grades[i] = s.grade[i]; GradeToShader(grades[gradeSlot], gradeK); }
  else for (int i = 0; i < kGradeSlots; ++i) grades[i] = s.grade[i];
}

// Rendered frame rate, measured between the game's upscale calls (one per rendered frame;
// frame generation adds displayed frames on top), logged with the mode it ran in.
void MeasureFrameRate(int mode) {
  static LARGE_INTEGER freq = {}, last = {};
  static double sum = 0.0, worst = 0.0;
  static int count = 0, lastMode = -99;
  static double recent[16] = {};
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  if (mode != lastMode) { sum = 0; worst = 0; count = 0; lastMode = mode; last = now; return; }
  const double ms = double(now.QuadPart - last.QuadPart) * 1000.0 / double(freq.QuadPart);
  last = now;
  if (ms > 200.0) return;  // a load or a pause
  sum += ms; worst = ms > worst ? ms : worst;
  recent[count % 16] = ms;
  if (++count == 240) {
    Log("frame rate [%s]: %.1f fps rendered (%.2f ms, worst %.2f ms)", mode == 0 ? "Prism off" : mode == 1 ? "network every frame" : mode == 2 ? "network every 2nd frame" : mode == 3 ? "network every 3rd frame" : "network every 4th frame",
        1000.0 * count / sum, sum / count, worst);
    char seq[256] = {};
    int n = 0;
    for (int i = 0; i < 16; ++i) n += std::snprintf(seq + n, sizeof seq - n, "%.1f ", recent[i]);
    Log("  last 16 frame intervals (ms): %s", seq);
    sum = 0; worst = 0; count = 0;
  }
}

bool PipelineRecord(const UpscaleCall& c) {
  if (failed || !Log) return false;
  Pace();
  {
    std::lock_guard<std::mutex> lock(statsMutex);
    Record(sRender, NowMs(), 'R');
  }
  StatsTick();
  if (cadence == 0 && fpsCap > 0) {
    const double now = NowMs(), budget = 1000.0 / fpsCap;
    const bool miss = adaptLastMs > 0.0 && now - adaptLastMs > budget * 1.15;
    adaptLastMs = now;
    if (miss) {
      // Behind the cap: stop launching until the queue drains and frames are on time again.
      if (adaptHold == 0) { adaptK = std::min(adaptK + 1, 8); Log("adaptive cadence: frame missed the cap, pausing the network; then every %d frames", adaptK); }
      adaptHold = 45;
      adaptFrames = adaptMisses = adaptLate = 0; adaptSleepSum = 0.0;
    } else if (adaptHold > 0) {
      --adaptHold;
    } else {
      adaptSleepSum += paceSleptMs;
      if (++adaptFrames >= 90) {
        const double idle = adaptSleepSum / adaptFrames;
        const int before = adaptK;
        if (adaptLate * 3 > adaptFrames / std::max(adaptK, 1)) adaptK = std::min(adaptK + 1, 8);
        else if (adaptK > 2 && idle * (adaptK - 1) > netGpuMs * 1.25) adaptK -= 1;
        if (adaptK != before) Log("adaptive cadence: every %d frames (idle %.2f ms per frame, network %.2f ms, %u late)", adaptK, idle, netGpuMs, adaptLate);
        adaptFrames = adaptMisses = adaptLate = 0; adaptSleepSum = 0.0;
      }
    }
  } else {
    adaptLastMs = 0.0; adaptHold = 0;
  }
  MeasureFrameRate(enabled ? std::max(cadence, 1) : 0);
  static uint32_t calls = 0;
  if (++calls % 60 == 0) LoadConfig();  // about once a second; a stat call when nothing changed
  ApplyPending();
  if (keys.toggle && Pressed(keys.toggle)) { enabled = !enabled; historyValid = false; Log("Prism NR %s (key)", enabled ? "on" : "off"); Notice(enabled ? L"Prism 已开启" : L"Prism 已关闭"); }
  if (keys.cadence && Pressed(keys.cadence)) {
    cadence = cadence % 4 + 1; historyValid = false; Log("network every %d frame(s) (key)", cadence);
    Notice(cadence == 1 ? L"网络每帧运行" : L"网络每 " + std::to_wstring(cadence) + L" 帧运行一次");
  }
  if (keys.async && Pressed(keys.async)) { asyncMode = !asyncMode; historyValid = false; Log("asynchronous network %s (key)", asyncMode ? "on" : "off"); Notice(asyncMode ? L"异步网络 开" : L"异步网络 关"); }
  if (keys.split && Pressed(keys.split)) { split = !split; Log("split view %s (key)", split ? "on" : "off"); Notice(split ? L"分屏对比 开：左边原画，右边 Prism" : L"分屏对比 关"); }
  if (keys.capture && Pressed(keys.capture) && !captureAt) { captureAt = frame + 1; Log("capture requested (key)"); }

  if (!enabled) return true;
  Exhume();
  {
    // A new output or render size: let the network in flight finish, then rebuild.
    static uint32_t builtRW = 0, builtRH = 0, waited = 0;
    const D3D12_RESOURCE_DESC nd = c.output->GetDesc();
    if (heap && (UINT(nd.Width) != w || nd.Height != h || c.renderWidth != builtRW || c.renderHeight != builtRH)) {
      bool busy = false;
      {
        std::lock_guard<std::mutex> lock(asyncMutex);
        // Steps submitted to our queue must finish; steps not yet recorded are simply dropped.
        busy = inFlight && fenceStep && fenceStep->GetCompletedValue() < stepSubmitted && waited < 120;
        if (!busy) { inFlight = false; stepList = nullptr; landList = nullptr; asyncRun = PassRun{}; }
      }
      if (busy) { ++waited; return true; }
      Log("output %llux%u, render %ux%u (was %ux%u, %ux%u): rebuilding", (unsigned long long)nd.Width, nd.Height, c.renderWidth, c.renderHeight, w, h, builtRW, builtRH);
      Teardown();
      waited = 0;
    }
    if (!heap) {
      if (!Setup(c.list, c.output)) return false;
      builtRW = c.renderWidth; builtRH = c.renderHeight;
    }
  }
  AutoNetScale();
  if (!SelectNetSize()) return false;

  liveRW = c.renderWidth; liveRH = c.renderHeight;
  auto* l = c.list;
  const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  const UINT ring = UINT(frame % kRing) * kBlock;
  ID3D12Resource* history = edits[current ^ 1];
  ID3D12Resource* next = edits[current];
  ID3D12Resource* warpHistory = warps[current ^ 1];
  ID3D12Resource* warpNext = warps[current];
  ID3D12Resource* warpLHistory = warpsL[current ^ 1];
  ID3D12Resource* warpLNext = warpsL[current];
  // A new network priority: rebuild the compute queue between launches.
  static int builtPriority = -1;
  if (computeQueue && builtPriority != netPriority && !inFlight) ReleaseAsync();
  if (asyncMode && !computeQueue) {
    if (!SetupAsync(c)) return false;
    builtPriority = netPriority;
  }
  bool landNow = false, launchNow = false;
  // Every frame can run asynchronous too: the network of frame t overlaps frame t+1 and lands a
  // frame later (Cadence=1 with Async=1).
  const bool async = asyncMode && historyValid && !c.reset && netProxy == proxy;  // a reduced network runs synchronously
  if (async) {
    std::lock_guard<std::mutex> lock(asyncMutex);
    // Land once the launch was submitted; in poll mode only once the network has also finished on
    // the GPU (otherwise carry one more frame: about 0.2 ms instead of a stall).
    if (inFlight && submittedId == launchId) landNow = !landPoll || fenceDone->GetCompletedValue() >= launchId;
    const int interval = cadence != 0 ? cadence : fpsCap > 0 ? adaptK : 1;  // uncapped adaptive: as soon as landed
    // A landing frame may launch again right away: the landed edit is baked apart first.
    launchNow = (!inFlight || landNow) && frame - lastLaunchFrame >= UINT64(interval) && !(cadence == 0 && adaptHold > 0) && netScale >= 0.999f;
    // A network still running on the frame its last pass should have landed did not fit the idle time.
    if (inFlight && !landNow && frame == lastLaunchFrame + UINT64(stagger ? std::max(asyncRun.n, 1) : 1)) ++adaptLate;
  }
  const bool refresh = async ? false : (!historyValid || c.reset || cadence <= 1 || frame % UINT64(cadence) == 0);
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R16G16_FLOAT; device->CreateShaderResourceView(c.motion, &sv, Cpu(ring + 3));
    if (proxyLow && netProxy == proxy) {
      // Native again after a reduced stretch: this slot's t2 and t11 back to the native output and
      // the scratch (the reduced path points them at its own textures).
      sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; device->CreateShaderResourceView(neural, &sv, Cpu(ring + 2));
      sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; device->CreateShaderResourceView(scratch, &sv, Cpu(ring + 11));
    }
    const DXGI_FORMAT df = c.depth->GetDesc().Format;
    sv.Format = df == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
              : df == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
              : DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView(c.depth, &sv, Cpu(ring + 4));
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(history, &sv, Cpu(ring + 5));
    device->CreateShaderResourceView(warpHistory, &sv, Cpu(ring + 6));
    device->CreateShaderResourceView(warpLHistory, &sv, Cpu(ring + 8));
    device->CreateShaderResourceView(warpLNext, &sv, Cpu(ring + 10));
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView(depthCopy, &sv, Cpu(ring + 7));  // null before the first async setup: unused then
    device->CreateUnorderedAccessView(c.output, nullptr, nullptr, Cpu(ring + kSetO));
    device->CreateUnorderedAccessView(c.output, nullptr, nullptr, Cpu(ring + kSetF));
    device->CreateUnorderedAccessView(c.output, nullptr, nullptr, Cpu(ring + kSetL));
    for (UINT set : {kSetP, kSetF, kSetL}) device->CreateUnorderedAccessView(next, nullptr, nullptr, Cpu(ring + set + 3));
    for (UINT set : {kSetP, kSetO, kSetG, kSetR, kSetB, kSetF}) device->CreateUnorderedAccessView(warpNext, nullptr, nullptr, Cpu(ring + set + 4));
    for (UINT set : {kSetP, kSetO, kSetG, kSetR, kSetB, kSetF, kSetL}) device->CreateUnorderedAccessView(warpLNext, nullptr, nullptr, Cpu(ring + set + 5));
    // Set B's u3 (launch luma) and set L's u4 (launch luma) stay as Setup made them.
  }
  // The frames since the network's previous run (cadence 0 or poll landing make it vary).
  const float span = launchId == 0 ? float(std::max(cadence, 1)) : float(std::min<uint64_t>(frame - lastLaunchFrame, 8));
  const bool landedBefore = haveLanded || landNow;
  const uint32_t splitCol = 4u + uint32_t(std::clamp(splitPos, 0.0f, 1.0f) * float(w) + 0.5f);
  recorded = true;
  Constants k = {w, h, tilesX * tilesY, tilesX, intensity, guardStrength, 0.05f, paint ? 2u : (split ? splitCol : 0u),
                 c.renderWidth, c.renderHeight, c.motionScaleX, c.motionScaleY, 0.05f,
                 (launchNow ? 1u : 0u) | (landNow ? 2u : 0u) | (landedBefore ? 4u : 0u) | (guided ? 16u : 0u) | (uint32_t(look & 7) << 8) |
                     // Steadying needs last frame's edit: none on the first frame, after a reset or a cut.
                     (historyValid && !c.reset ? (uint32_t(std::lround(steady * 15.0f)) << 12) | (uint32_t(std::lround(steadyBand * 100.0f)) << 16) : 0u) |
                     (uint32_t(std::lround((1.0f - detailKeep) * 255.0f)) << 24),
                 span, 1.0f - stability};

  const UINT slot = UINT(frame % kRing);
  // The slot written kRing frames ago is long finished: read it before reusing it.
  if (stampData && frame >= kRing && tsFreq) {
    const uint64_t a = stampData[slot * 2], b = stampData[slot * 2 + 1];
    if (b > a) {
      const double ms = double(b - a) * 1000.0 / double(tsFreq);
      const int kind = slotRefresh[slot] ? 1 : 0;
      msSum[kind] += ms; ++msCount[kind]; msAll += ms; ++msTotal;
      prismMsSmooth = prismMsSmooth == 0.0 ? ms : prismMsSmooth * 0.95 + ms * 0.05;
    }
    if (msTotal == 300) {
      Log("Prism on the GPU: %.2f ms per frame on average | network frames %.2f ms (%u) | carried frames %.2f ms (%u)",
          msAll / msTotal, msCount[1] ? msSum[1] / msCount[1] : 0.0, msCount[1], msCount[0] ? msSum[0] / msCount[0] : 0.0, msCount[0]);
      msSum[0] = msSum[1] = msAll = 0; msCount[0] = msCount[1] = msTotal = 0;
    }
  }
  slotRefresh[slot] = refresh;
  if (queries) l->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
  // 1. Copy the upscaled frame.
  Barrier(l, c.output, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
  l->CopyResource(origCopy, c.output);
  Barrier(l, c.output, D3D12_RESOURCE_STATE_COPY_SOURCE, UAV);
  Barrier(l, origCopy, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
  Barrier(l, history, UAV, SRV);
  Barrier(l, warpHistory, UAV, SRV);
  Barrier(l, warpLHistory, UAV, SRV);
  ID3D12DescriptorHeap* heaps[] = {heap};
  l->SetDescriptorHeaps(1, heaps);
  BindRoot(l);
  l->SetComputeRoot32BitConstants(0, 16, &k, 0);
  l->SetComputeRootDescriptorTable(1, Gpu(ring));
  if (!refresh) {
  if (landNow) {
    // Land: bake the network's edit over the launch frame (the network ran on our queue since the
    // launch), then the proxy and copies are free for the next launch.
    Barrier(l, neural, UAV, SRV);
    Barrier(l, landedEdit, SRV, UAV);
    l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetB));
    l->SetPipelineState(psoBake); l->Dispatch(tilesX, tilesY, 1);
    UavBarrier(l);
    if (guided) {
      // The guided edit's line per tile, from the landed edit and the launch luma Bake just wrote.
      l->SetPipelineState(psoFit); l->Dispatch(tilesX, tilesY, 1);
      UavBarrier(l);
    }
    Barrier(l, landedEdit, UAV, SRV);
    Barrier(l, neural, SRV, UAV);
    Barrier(l, proxy, SRV, UAV);
    Barrier(l, motionCopy, SRV, UAV);
    Barrier(l, depthCopy, SRV, UAV);
    {
      std::lock_guard<std::mutex> lock(asyncMutex);
      landList = l;
      landId = launchId;
      inFlight = false;
    }
    if (netStampData && netTsFreq && landPoll) {
      // Poll landing only lands after the fence passed, so the last step's stamps are in place;
      // scaled to the whole launch when its passes were staggered.
      const UINT q0 = UINT(stepId & 1) * 2;
      const uint64_t a = netStampData[q0], b = netStampData[q0 + 1];
      const double perLaunch = double(b - a) * 1000.0 / double(netTsFreq) * asyncRun.n / lastStepPasses;
      if (b > a) netGpuMs = netGpuMs * 0.8 + 0.2 * perLaunch;
    }
    {
      std::lock_guard<std::mutex> lock(statsMutex);
      const uint32_t age = uint32_t(frame - lastLaunchFrame);
      ++statLands; statAgeSum += age; statAgeMax = std::max(statAgeMax, age);
      if (frameCsv) std::fprintf(frameCsv, "%.3f,L,%u\n", NowMs(), age);
    }
    haveLanded = true;
  }
  // Carry: the landed edit through its warp, the chain where that breaks; the warps follow along.
  l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetO));
  l->SetPipelineState(psoCarry); l->Dispatch(tilesX, tilesY, 1);
  UavBarrier(l);
  // Fill the holes from same-depth neighbours and compose the frame (guided: Fill also reads this
  // frame's warp to the landed launch frame).
  Barrier(l, scratch, UAV, SRV);
  if (guided) Barrier(l, warpLNext, UAV, SRV);
  l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetL));
  l->SetPipelineState(psoFill); l->Dispatch(tilesX, tilesY, 1);
  UavBarrier(l);
  if (guided) Barrier(l, warpLNext, SRV, UAV);
  Barrier(l, scratch, SRV, UAV);
    if (launchNow) {
      // Launch: proxy and copies on the game's list; the network itself on our queue.
      l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetP));
      l->SetPipelineState(psoReduce1); l->Dispatch(tilesX, tilesY, 1); UavBarrier(l);
      l->SetPipelineState(psoReduce2); l->Dispatch(1, 1, 1); UavBarrier(l);
      l->SetPipelineState(psoEncode); l->Dispatch(tilesX, tilesY, 1);
      Barrier(l, proxy, UAV, SRV);
      device->CreateUnorderedAccessView(motionCopy, nullptr, nullptr, Cpu(ring + kSetG));
      device->CreateUnorderedAccessView(depthCopy, nullptr, nullptr, Cpu(ring + kSetG + 3));
      const bool exactNow = exactMotion && landedBefore;
      Constants kg = k;
      kg.flags = exactNow ? 8u : 0u;
      l->SetComputeRoot32BitConstants(0, 16, &kg, 0);
      Barrier(l, warpLNext, UAV, SRV);
      l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetG));
      l->SetPipelineState(psoCopyGuides); l->Dispatch((c.renderWidth + 15) / 16, (c.renderHeight + 15) / 16, 1);
      UavBarrier(l);
      Barrier(l, warpLNext, SRV, UAV);
      Barrier(l, motionCopy, UAV, SRV);
      Barrier(l, depthCopy, UAV, SRV);
      // Record the network on the compute list now; it is submitted when the game submits this list.
      OpenStep();
      void* ap = &asyncParams;
      ngx::SetRes(ap, "DLSSNR.Color", proxy); ngx::SetRes(ap, "DLSSNR.Output", neural);
      ngx::SetRes(ap, "DLSSNR.MVec", motionCopy); ngx::SetRes(ap, "DLSSNR.Depth", depthCopy);
      for (const char* key : {"DLSSNR.ColorSubrect", "DLSSNR.OutputSubrect"}) {
        const std::string s2(key);
        ngx::SetUI(ap, (s2 + "BaseX").c_str(), 0); ngx::SetUI(ap, (s2 + "BaseY").c_str(), 0);
        ngx::SetUI(ap, (s2 + "Width").c_str(), w); ngx::SetUI(ap, (s2 + "Height").c_str(), h);
      }
      for (const char* key : {"DLSSNR.DepthSubrect", "DLSSNR.MVecSubrect"}) {
        const std::string s2(key);
        ngx::SetUI(ap, (s2 + "BaseX").c_str(), 0); ngx::SetUI(ap, (s2 + "BaseY").c_str(), 0);
        ngx::SetUI(ap, (s2 + "Width").c_str(), c.renderWidth); ngx::SetUI(ap, (s2 + "Height").c_str(), c.renderHeight);
      }
      // Motion since the network's previous run: followed through the landed warp (the vectors
      // already span it), or the launch frame's own vectors scaled by the span (constant velocity).
      const float ms = exactNow ? 1.0f : span;
      ngx::SetF(ap, "DLSSNR.MVecScaleX", c.motionScaleX * ms); ngx::SetF(ap, "DLSSNR.MVecScaleY", c.motionScaleY * ms);
      ngx::SetI(ap, "DLSSNR.DepthInverted", (gameCreateFlags & (1 << 3)) ? 1 : 0);
      ngx::SetI(ap, "DLSSNR.Enabled", 1);
      ngx::SetF(ap, "DLSSNR.Intensity", 1.f);
      BeginPasses(computeList, moreAsync, ap, launchId == 0 || netReset, asyncRun);
      // Staggered: the first pass now, one more on each following frame; otherwise all of them.
      const ngx::Result ar = RunStep(stagger ? 1 : asyncRun.n);
      if (ar != ngx::kSuccess) { Log("async NR evaluate 0x%08x", ar); return Fail("async NR evaluate"); }
      {
        std::lock_guard<std::mutex> lock(asyncMutex);
        ++launchId;
        stepList = l;
        stepFinal = asyncRun.next >= asyncRun.n;
        inFlight = true;
      }
      lastLaunchFrame = frame;
      {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++statLaunches;
        if (frameCsv) std::fprintf(frameCsv, "%.3f,N,0\n", NowMs());
      }
      // Bindings again (NGX may have changed nothing here, but the encode pass left its table).
      l->SetDescriptorHeaps(1, heaps);
      BindRoot(l);
      l->SetComputeRoot32BitConstants(0, 16, &k, 0);
      l->SetComputeRootDescriptorTable(1, Gpu(ring));
    } else if (inFlight && asyncRun.next < asyncRun.n) {
      // Staggered: the launch's next pass rides on this frame, once the step before was submitted.
      bool ready = false;
      {
        std::lock_guard<std::mutex> lock(asyncMutex);
        ready = stepList == nullptr && stepSubmitted == stepId;
      }
      if (ready) {
        OpenStep();
        const ngx::Result ar = RunStep(asyncRun.next + 1);
        if (ar != ngx::kSuccess) { Log("async NR evaluate 0x%08x", ar); return Fail("async NR evaluate"); }
        std::lock_guard<std::mutex> lock(asyncMutex);
        stepList = l;
        stepFinal = asyncRun.next >= asyncRun.n;
      }
    }
    if (queries && stamps) {
      l->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
      l->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, stamps, UINT64(slot) * 16);
    }
  } else {
  haveLanded = false;
  // 2-3. Exposure and the proxy.
  l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetP));
  l->SetPipelineState(psoReduce1); l->Dispatch(tilesX, tilesY, 1); UavBarrier(l);
  l->SetPipelineState(psoReduce2); l->Dispatch(1, 1, 1); UavBarrier(l);
  l->SetPipelineState(psoEncode); l->Dispatch(tilesX, tilesY, 1);
  Barrier(l, proxy, UAV, SRV);
  const bool reduced = netProxy != proxy;
  if (reduced) {
    // 3b. The reduced proxy the network runs on (Compose brings the edit back up). This slot's t2
    // and t11 are the reduced output and proxy, set G's u0 the reduced proxy for Down (the scratch
    // and set G belong to the asynchronous path, which does not run at a reduced size).
    D3D12_SHADER_RESOURCE_VIEW_DESC lv = {};
    lv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; lv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; lv.Texture2D.MipLevels = 1;
    lv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(neuralLow, &lv, Cpu(ring + 2));
    device->CreateShaderResourceView(proxyLow, &lv, Cpu(ring + 11));
    device->CreateUnorderedAccessView(proxyLow, nullptr, nullptr, Cpu(ring + kSetG));
    if (upFilter == 1) {
      device->CreateShaderResourceView(gfA, &lv, Cpu(ring + 12));
      device->CreateShaderResourceView(gfB, &lv, Cpu(ring + 13));
      device->CreateUnorderedAccessView(gfA, nullptr, nullptr, Cpu(ring + kSetU));
      device->CreateUnorderedAccessView(gfB, nullptr, nullptr, Cpu(ring + kSetU + 3));
      k.flags |= 64u;
    }
    k.flags |= 32u;
    k.span = float(nw) / float(w);
    l->SetComputeRoot32BitConstants(0, 16, &k, 0);
    l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetG));
    l->SetPipelineState(psoDown); l->Dispatch((nw + 15) / 16, (nh + 15) / 16, 1);
    UavBarrier(l);
    Barrier(l, proxyLow, UAV, SRV);
  }
  // 4. The network.
  void* p = &nrParams;
  ngx::SetRes(p, "DLSSNR.Color", netProxy);
  ngx::SetRes(p, "DLSSNR.Output", netNeural);
  ngx::SetRes(p, "DLSSNR.MVec", c.motion);
  ngx::SetRes(p, "DLSSNR.Depth", c.depth);
  for (const char* key : {"DLSSNR.ColorSubrect", "DLSSNR.OutputSubrect"}) {
    const std::string s(key);
    ngx::SetUI(p, (s + "BaseX").c_str(), 0); ngx::SetUI(p, (s + "BaseY").c_str(), 0);
    ngx::SetUI(p, (s + "Width").c_str(), nw); ngx::SetUI(p, (s + "Height").c_str(), nh);
  }
  for (const char* key : {"DLSSNR.DepthSubrect", "DLSSNR.MVecSubrect"}) {
    const std::string s(key);
    ngx::SetUI(p, (s + "BaseX").c_str(), 0); ngx::SetUI(p, (s + "BaseY").c_str(), 0);
    ngx::SetUI(p, (s + "Width").c_str(), c.renderWidth); ngx::SetUI(p, (s + "Height").c_str(), c.renderHeight);
  }
  ngx::SetF(p, "DLSSNR.MVecScaleX", c.motionScaleX); ngx::SetF(p, "DLSSNR.MVecScaleY", c.motionScaleY);
  ngx::SetI(p, "DLSSNR.DepthInverted", (gameCreateFlags & (1 << 3)) ? 1 : 0);
  ngx::SetI(p, "DLSSNR.Enabled", 1);
  ngx::SetF(p, "DLSSNR.Intensity", 1.f);
  const ngx::Result r = EvaluatePasses(l, nrHandle, morePasses, p, c.reset || frame == 0 || netReset);
  if (r != ngx::kSuccess) { Log("NR evaluate 0x%08x", r); return Fail("NR evaluate"); }
  // 5. Compose onto the game's frame. NGX changed the bindings: set ours again.
  Barrier(l, netNeural, UAV, SRV);
  l->SetDescriptorHeaps(1, heaps);
  BindRoot(l);
  l->SetComputeRoot32BitConstants(0, 16, &k, 0);
  l->SetComputeRootDescriptorTable(1, Gpu(ring));
  const bool fit = reduced && (k.flags & 64u);
  if (fit) {
    l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetU));
    l->SetPipelineState(psoGuidedFit); l->Dispatch((nw + 15) / 16, (nh + 15) / 16, 1);
    Barrier(l, gfA, UAV, SRV);
    Barrier(l, gfB, UAV, SRV);
  }
  l->SetComputeRootDescriptorTable(2, Gpu(ring + kSetF));
  l->SetPipelineState(psoCompose); l->Dispatch(tilesX, tilesY, 1);
  UavBarrier(l);
  if (fit) {
    Barrier(l, gfA, SRV, UAV);
    Barrier(l, gfB, SRV, UAV);
  }
  historyValid = true;
  if (captureAt && captureAt <= frame + 1 && !captures[0].rb) {
    const Capture list4[4] = {{origCopy, "orig"}, {proxy, "proxy"}, {netNeural, "neural"}, {c.output, "final"}};
    const D3D12_RESOURCE_STATES st[4] = {SRV, SRV, SRV, UAV};
    for (int i = 0; i < 4; ++i) {
      Capture& cp = captures[i];
      cp = list4[i];
      D3D12_RESOURCE_DESC d = cp.src->GetDesc();
      UINT rows; UINT64 rowBytes, total;
      device->GetCopyableFootprints(&d, 0, 1, 0, &cp.fp, &rows, &rowBytes, &total);
      D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
      bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&cp.rb))) { cp.rb = nullptr; continue; }
      Barrier(l, cp.src, st[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
      dst.pResource = cp.rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = cp.fp;
      src.pResource = cp.src; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      Barrier(l, cp.src, D3D12_RESOURCE_STATE_COPY_SOURCE, st[i]);
    }
    captureAt = frame + kRing + 1;  // written out kRing frames from now
  } else if (captureAt && frame + 1 >= captureAt && captures[0].rb) {
    CreateDirectoryW((gameDir + L"prism-capture").c_str(), nullptr);
    for (Capture& cp : captures) {
      if (!cp.rb) continue;
      void* data = nullptr;
      if (SUCCEEDED(cp.rb->Map(0, nullptr, &data))) {
        const std::wstring base = gameDir + L"prism-capture\\" + std::wstring(cp.name, cp.name + std::strlen(cp.name));
        if (FILE* f = _wfopen((base + L".raw").c_str(), L"wb")) { std::fwrite(data, 1, size_t(cp.fp.Footprint.RowPitch) * cp.fp.Footprint.Height, f); std::fclose(f); }
        if (FILE* f = _wfopen((base + L".txt").c_str(), L"w")) { std::fprintf(f, "%u %u %u %d\n", cp.fp.Footprint.Width, cp.fp.Footprint.Height, cp.fp.Footprint.RowPitch, int(cp.fp.Footprint.Format)); std::fclose(f); }
        cp.rb->Unmap(0, nullptr);
      }
      cp.rb->Release();
      cp.rb = nullptr;
    }
    captureAt = 0;
    Log("capture written to prism-capture");
  }
  if (queries && stamps) {
    l->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    l->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, stamps, UINT64(slot) * 16);
  }
  // Back to the states the next frame starts from.
  Barrier(l, netNeural, SRV, UAV);
  Barrier(l, proxy, SRV, UAV);
  if (reduced) Barrier(l, proxyLow, SRV, UAV);
  }  // refresh
  if (recordLeft > 0) RecordFrame(c, l, ring);
  if (recordDoneAt && frame >= recordDoneAt) WriteRecording();
  Barrier(l, history, SRV, UAV);
  Barrier(l, warpHistory, SRV, UAV);
  Barrier(l, warpLHistory, SRV, UAV);
  Barrier(l, origCopy, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
  current ^= 1;
  if (++frame == 1 || frame % 600 == 0) {
    const D3D12_RESOURCE_DESC d = c.output->GetDesc();
    Log("pipeline frame %llu output %p %llux%u fmt %d flags 0x%x list %p", (unsigned long long)frame, (void*)c.output, (unsigned long long)d.Width, d.Height, int(d.Format), unsigned(d.Flags), (void*)l);
  }
  return true;
}

}  // namespace prism

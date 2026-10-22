#include "pipeline.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "MinHook.h"
#include "ngx_param.h"

namespace prism {
namespace {

LogFn Log = nullptr;
std::wstring gameDir;
int gameCreateFlags = 0;
bool failed = false;
bool enabled = true;
bool split = false;
bool paint = false;
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
  uint2 RenderSize; float2 MotionScale; float DepthTolerance; uint Flags; uint2 Pad1;
};
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
RWTexture2D<float4> Out0 : register(u0);
RWStructuredBuffer<float> Tiles : register(u1);
RWStructuredBuffer<float> Exposure : register(u2);
RWTexture2D<float4> NewEdit : register(u3);
RWTexture2D<float4> NewWarp : register(u4);

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

// Edit = log2(neural / proxy) in the proxy's linear light; guard its chroma on skin and strong
// colour; apply it to the game's HDR frame as a ratio, so the frame's range is untouched.
[numthreads(16, 16, 1)]
void Compose(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  const float3 o = Orig[id.xy].rgb;
  const float3 pd = Proxy[id.xy].rgb;
  const float3 p = Dec(pd), n = Dec(Neural[id.xy].rgb);
  float3 e = clamp(log2(max(n, 1.0 / 512.0) / max(p, 1.0 / 512.0)), -3.0, 3.0) * Intensity;
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
  NewEdit[id.xy] = float4(e, DepthAt(id.xy));
  NewWarp[id.xy] = 0.0;  // a synchronous result: no launch in flight to reach
  // Split view (F4): the original on the left half, a thin line at the split.
  if (Split != 0u && id.x < Size.x / 2u) e = 0.0;
  float3 result = o * exp2(e);
  if (Split != 0u && (id.x == Size.x / 2u || id.x + 1u == Size.x / 2u)) result = 2.0;
  if (Split == 2u) result = float3(4.0, 0.0, 4.0);  // debug paint: proves the writes reach the screen
  Out0[id.xy] = float4(result, 1.0);
}

// Follows last frame along this frame's motion vectors: the carried edit, and the carried warp back
// to the launch frame. Each of the four bilinear taps counts only where its stored depth matches
// this pixel's (a surface that was hidden last frame gets nothing borrowed). Only the edit and the
// warp travel, never the picture.
void Follow(uint2 p, out float3 e, out float4 warp) {
  const float2 toOut = float2(Size) / float2(RenderSize);
  const float2 mv = Motion.Load(int3(RenderPixel(p), 0)) * MotionScale * toOut;
  const float2 pos = float2(p) + mv;  // previous position, pixel-centre space minus 0.5
  const int2 b = int2(floor(pos));
  const float2 f = pos - float2(b);
  const int2 last = int2(Size) - 1;
  const float d = DepthAt(p);
  const float tol = DepthTolerance * max(abs(d), 1e-4);
  e = 0.0;
  float wsum = 0.0, qsum = 0.0;
  float3 q = 0.0;
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
    }
  }
  const bool inside = all(pos >= -0.5) && all(pos <= float2(last) + 0.5);
  e = (inside && wsum > 1e-3) ? e / wsum : 0.0;
  warp = (inside && qsum > 1e-3) ? float4(q.xy / qsum - float2(p), q.z / qsum, 1.0) : 0.0;
}

// A frame the network skips: the edit follows along, and so does the warp to the launch frame in
// flight. On the launch frame itself (Flags bit 0) the warp starts here: offset zero, this depth.
[numthreads(16, 16, 1)]
void Carry(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  float3 e;
  float4 warp;
  Follow(id.xy, e, warp);
  const float d = DepthAt(id.xy);
  if (Flags & 1u) warp = float4(0.0, 0.0, d, 1.0);
  NewEdit[id.xy] = float4(e, d);
  NewWarp[id.xy] = warp;
  if (Split != 0u && id.x < Size.x / 2u) e = 0.0;
  float3 result = Orig[id.xy].rgb * exp2(e);
  if (Split != 0u && (id.x == Size.x / 2u || id.x + 1u == Size.x / 2u)) result = 2.0;
  Out0[id.xy] = float4(result, 1.0);
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
  Out0[id.xy] = float4(Motion.Load(int3(id.xy, 0)), 0.0, 0.0);
  NewEdit[id.xy] = float4(Depth.Load(int3(id.xy, 0)), 0.0, 0.0, 0.0);
}

// The landing frame of an asynchronous launch, however many frames after it: the network's edit from
// the launch frame, reached through the carried warp, each tap admitted where the launch frame's
// depth matches the depth this surface had on the launch frame. Where the chain broke (a surface
// that was not visible then) the carried older edit stays, so nothing drops to zero.
[numthreads(16, 16, 1)]
void Land(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  float3 e;
  float4 warp;
  Follow(id.xy, e, warp);
  if (warp.w > 0.5) {
    const float2 pos = float2(id.xy) + warp.xy;
    const int2 b = int2(floor(pos));
    const float2 f = pos - float2(b);
    const int2 last = int2(Size) - 1;
    const float tol = DepthTolerance * max(abs(warp.z), 1e-4);
    float3 n = 0.0;
    float nsum = 0.0;
    [unroll] for (int j = 0; j <= 1; ++j) {
      [unroll] for (int i = 0; i <= 1; ++i) {
        const int2 tap = clamp(b + int2(i, j), int2(0, 0), last);
        const float dp = LaunchDepth.Load(int3(RenderPixel(uint2(tap)), 0));
        const float wt = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y) * (abs(dp - warp.z) <= tol ? 1.0 : 0.0);
        n += EditAt(tap) * wt;
        nsum += wt;
      }
    }
    const bool inside = all(pos >= -0.5) && all(pos <= float2(last) + 0.5);
    if (inside && nsum > 1e-3) e = n / nsum;
  }
  NewEdit[id.xy] = float4(e, DepthAt(id.xy));
  NewWarp[id.xy] = 0.0;
  if (Split != 0u && id.x < Size.x / 2u) e = 0.0;
  float3 result = Orig[id.xy].rgb * exp2(e);
  if (Split != 0u && (id.x == Size.x / 2u || id.x + 1u == Size.x / 2u)) result = 2.0;
  Out0[id.xy] = float4(result, 1.0);
}
)";

struct Constants {
  uint32_t w, h, tileCount, tilesX; float intensity, guard, adapt; uint32_t split;
  uint32_t renderW, renderH; float mvScaleX, mvScaleY, depthTolerance; uint32_t flags; uint32_t pad[2];
};
static_assert(sizeof(Constants) == 64, "16 root constants");

// ------------------------------------------------------------------ GPU state
ID3D12Device* device = nullptr;
ID3D12RootSignature* root = nullptr;
ID3D12PipelineState *psoReduce1 = nullptr, *psoReduce2 = nullptr, *psoEncode = nullptr, *psoCompose = nullptr, *psoCarry = nullptr, *psoLand = nullptr, *psoCopyGuides = nullptr;
ID3D12DescriptorHeap* heap = nullptr;
UINT inc = 0;
constexpr UINT kRing = 8;
// Per ring slot: SRVs t0..t7 at [0..7], then three UAV sets of u0..u4:
// [8..12] proxy, tiles, exposure, next edit, next warp; [13..17] output, tiles, exposure, next edit,
// next warp; [18..22] the launch copies: motion, tiles, exposure, depth, next warp; [23..27] the
// recorder's copies: motion, tiles, exposure, depth, next warp.
constexpr UINT kBlock = 28;
uint32_t w = 0, h = 0, tilesX = 0, tilesY = 0;
ID3D12Resource *origCopy = nullptr, *proxy = nullptr, *neural = nullptr, *tiles = nullptr, *exposure = nullptr;
ID3D12Resource* edits[2] = {};  // ping-pong: last frame's edit and this frame's
ID3D12Resource* warps[2] = {};  // ping-pong: the warp back to the launch frame in flight
int current = 0;
bool historyValid = false;
int cadence = 4;  // the network every N frames (F2 cycles 1-4); 0: adaptive (see Adapt)
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
ID3D12CommandAllocator* computeAlloc[2] = {};
ID3D12GraphicsCommandList* computeList = nullptr;
ID3D12Fence* fenceLaunch = nullptr;  // the game's queue signals: the launch frame's list has run
ID3D12Fence* fenceDone = nullptr;    // our queue signals: the network has run
void* asyncHandle = nullptr;
OwnedParams asyncParams;
ID3D12Resource *motionCopy = nullptr, *depthCopy = nullptr;  // the launch frame's, kept to landing
std::mutex asyncMutex;
ID3D12CommandList* launchList = nullptr;  // waiting for the game to submit it
ID3D12CommandList* landList = nullptr;    // must wait for the network before it runs
uint64_t launchId = 0, submittedId = 0, landId = 0;
bool inFlight = false;
using ExecuteFn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteFn realExecute = nullptr;

void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
  bool launch = false, land = false;
  uint64_t lid = 0, did = 0;
  {
    std::lock_guard<std::mutex> lock(asyncMutex);
    for (UINT i = 0; i < n; ++i) {
      if (lists[i] == landList && landList) { land = true; did = landId; landList = nullptr; }
      if (lists[i] == launchList && launchList) { launch = true; lid = launchId; launchList = nullptr; }
    }
  }
  if (land) q->Wait(fenceDone, did);  // the landing frame reads the network's output
  realExecute(q, n, lists);
  if (launch) {
    q->Signal(fenceLaunch, lid);
    computeQueue->Wait(fenceLaunch, lid);
    ID3D12CommandList* cl[] = {computeList};
    computeQueue->ExecuteCommandLists(1, cl);
    computeQueue->Signal(fenceDone, lid);
    std::lock_guard<std::mutex> lock(asyncMutex);
    submittedId = lid;
  }
}
void* nrHandle = nullptr;
OwnedParams nrParams;
uint64_t frame = 0;
ID3D12QueryHeap* queries = nullptr;
ID3D12Resource* stamps = nullptr;
uint64_t* stampData = nullptr;
uint64_t tsFreq = 0;
double msSum[2] = {}, msAll = 0.0;
uint32_t msCount[2] = {}, msTotal = 0;
bool slotRefresh[16] = {};

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i) { auto h0 = heap->GetCPUDescriptorHandleForHeapStart(); h0.ptr += SIZE_T(i) * inc; return h0; }
D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT i) { auto h0 = heap->GetGPUDescriptorHandleForHeapStart(); h0.ptr += UINT64(i) * inc; return h0; }

bool Fail(const char* what, HRESULT hr = S_OK) {
  Log("pipeline off: %s (0x%08lx)", what, hr);
  failed = true;
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

void SetupPresentHook();  // below, with the frame statistics

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
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 8;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 5;
  D3D12_ROOT_PARAMETER rp[3] = {};
  rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 16;
  rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = {1, &ranges[0]};
  rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable = {1, &ranges[1]};
  D3D12_ROOT_SIGNATURE_DESC rd = {}; rd.NumParameters = 3; rd.pParameters = rp;
  ID3DBlob *blob = nullptr, *err = nullptr;
  hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
  if (FAILED(hr) || FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&root)))
    return Fail("root signature", hr);
  for (auto [entry, pso] : {std::pair{"Reduce1", &psoReduce1}, std::pair{"Reduce2", &psoReduce2}, std::pair{"Encode", &psoEncode}, std::pair{"Compose", &psoCompose}, std::pair{"Carry", &psoCarry}, std::pair{"Land", &psoLand}, std::pair{"CopyGuides", &psoCopyGuides}}) {
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
  tiles = Buf(UINT64(tilesX) * tilesY * 4);
  exposure = Buf(16);
  edits[0] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  edits[1] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  warps[0] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  warps[1] = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  if (!origCopy || !proxy || !neural || !tiles || !exposure || !edits[0] || !edits[1] || !warps[0] || !warps[1]) return Fail("resources");
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
    device->CreateUnorderedAccessView(proxy, nullptr, nullptr, Cpu(b + 8));
    for (UINT set : {8u, 13u, 18u, 23u}) { BufferUav(tiles, tilesX * tilesY, b + set + 1); BufferUav(exposure, 4, b + set + 2); }
  }

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
  ngx::SetUI(p, "Width", w); ngx::SetUI(p, "Height", h); ngx::SetUI(p, "OutWidth", w); ngx::SetUI(p, "OutHeight", h);
  for (const char* k : {"DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth", "DLSSNR.Output.Width"}) ngx::SetUI(p, k, w);
  for (const char* k : {"DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight", "DLSSNR.Output.Height"}) ngx::SetUI(p, k, h);
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
  static bool presentHooked = false;
  if (!presentHooked) { presentHooked = true; SetupPresentHook(); }
  return true;
}

// The compute queue, its feature, the copies of the launch frame's motion and depth, and the
// hook on ExecuteCommandLists (every D3D12 queue of the driver shares one vtable).
bool SetupAsync(const UpscaleCall& c) {
  HRESULT hr;
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  qd.Priority = netPriority ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  if (FAILED(hr = device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&computeQueue))) return Fail("compute queue", hr);
  for (auto& a : computeAlloc)
    if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, __uuidof(ID3D12CommandAllocator), (void**)&a))) return Fail("compute allocator", hr);
  if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, computeAlloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&computeList))) return Fail("compute list", hr);
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fenceLaunch)) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fenceDone))) return Fail("fences");
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
  if (realExecute) { Log("async network rebuilt (%s priority)", netPriority ? "high" : "normal"); return true; }
  // Hook ExecuteCommandLists through a queue's vtable (slot 10).
  ID3D12CommandQueue* probe = nullptr;
  D3D12_COMMAND_QUEUE_DESC pd = {}; pd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device->CreateCommandQueue(&pd, __uuidof(ID3D12CommandQueue), (void**)&probe))) return Fail("probe queue");
  void* target = (*reinterpret_cast<void***>(probe))[10];
  probe->Release();
  if (MH_CreateHook(target, (void*)&HookExecute, (void**)&realExecute) != MH_OK || MH_EnableHook(target) != MH_OK) return Fail("ExecuteCommandLists hook");
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
  Bury(computeList);
  for (auto& a : computeAlloc) { Bury(a); a = nullptr; }
  Bury(computeQueue); Bury(fenceLaunch); Bury(fenceDone);
  computeList = nullptr; computeQueue = nullptr; fenceLaunch = nullptr; fenceDone = nullptr;
}

// Everything that depends on the output or render size, for a rebuild at a new size. Nothing is
// released while the GPU might still use it: it waits in the graveyard for a few frames.
void Teardown() {
  ID3D12Resource** slots[] = {&origCopy, &proxy, &neural, &tiles, &exposure, &edits[0], &edits[1], &warps[0], &warps[1], &motionCopy, &depthCopy, &recMotion, &recDepth};
  for (ID3D12Resource** r : slots) {
    Bury(*r);
    *r = nullptr;
  }
  Bury(heap);
  heap = nullptr;
  BuryNgx(nrHandle);
  nrHandle = nullptr;
  if (computeQueue) ReleaseAsync();
  for (RecFrame& rf : recFrames) { Bury(rf.color); Bury(rf.motion); Bury(rf.depth); }
  recFrames.clear(); recordLeft = 0; recordDoneAt = 0;
  historyValid = false;
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
void Pace() {
  static double next = 0.0;
  if (fpsCap <= 0) { next = 0.0; paceSleptMs = 0.0; return; }
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const double period = 1000.0 / fpsCap;
  double now = NowMs();
  paceSleptMs = 0.0;
  if (next == 0.0 || now >= next) { next = now + period; return; }
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

void Record(Series& sr, double t, char kind) {
  if (sr.last > 0.0) {
    const double d = t - sr.last;
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
  Log("stats [%s] present %.1f fps p99 %.2f ms sd %.2f ms (%zu) | render %.1f fps p99 %.2f ms sd %.2f ms (%zu) | network %u launches, %u lands, age %.2f avg %u max | interval %.2f | network gpu %.2f ms",
      label.c_str(), p.fps, p.p99, p.sd, p.n, r.fps, r.p99, r.sd, r.n, statLaunches, statLands, statLands ? double(statAgeSum) / statLands : 0.0, statAgeMax,
      statLaunches ? double(r.n) / statLaunches : 0.0, netGpuMs);
  if (frameCsv) std::fflush(frameCsv);
  clear();
  statsEnd = t + statsSec * 1000.0;
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
  if (presentDepth++ == 0) OnPresent(flags);
  const HRESULT hr = realPresent(sc, sync, flags);
  --presentDepth;
  return hr;
}
HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
  if (presentDepth++ == 0) OnPresent(flags);
  const HRESULT hr = realPresent1(sc, sync, flags, pp);
  --presentDepth;
  return hr;
}

void SetupPresentHook() {
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
  if (wnd && SUCCEEDED(device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q)) &&
      SUCCEEDED(factory->CreateSwapChainForHwnd(q, wnd, &d, nullptr, nullptr, &sc))) {
    void** vt = *reinterpret_cast<void***>(sc);
    const bool ok = MH_CreateHook(vt[8], (void*)&HookPresent, (void**)&realPresent) == MH_OK && MH_EnableHook(vt[8]) == MH_OK &&
                    MH_CreateHook(vt[22], (void*)&HookPresent1, (void**)&realPresent1) == MH_OK && MH_EnableHook(vt[22]) == MH_OK;
    Log(ok ? "present timing on (DXGI Present hooked)" : "present timing off: hook failed");
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

// prism-core.ini beside the game: defaults and hotkeys, written by the Prism NR app. It is
// re-read whenever it changes, so the app can tune a running game.
struct Keys { int toggle = VK_F8, cadence = VK_F2, async = VK_F3, split = VK_F4, capture = VK_F11; } keys;
float intensity = 1.0f, guardStrength = 1.0f;
FILETIME configTime = {};

// "F1".."F24", a virtual-key number, or "none" to disable the key.
int ParseKey(const std::wstring& s, int fallback) {
  if (s.empty()) return fallback;
  if (!lstrcmpiW(s.c_str(), L"none")) return 0;
  if ((s[0] == L'F' || s[0] == L'f') && s.size() > 1) {
    const int n = _wtoi(s.c_str() + 1);
    return n >= 1 && n <= 24 ? VK_F1 + n - 1 : fallback;
  }
  const int n = _wtoi(s.c_str());
  return n > 0 && n < 256 ? n : fallback;
}

void LoadConfig() {
  const std::wstring path = gameDir + L"prism-core.ini";
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
  if (CompareFileTime(&fa.ftLastWriteTime, &configTime) == 0) return;
  configTime = fa.ftLastWriteTime;
  auto Get = [&](const wchar_t* key) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"Prism Core", key, L"", buf, 64, path.c_str());
    return std::wstring(buf);
  };
  auto Num = [&](const wchar_t* key, double fallback, double lo, double hi) {
    const std::wstring v = Get(key);
    const double x = v.empty() ? fallback : _wtof(v.c_str());
    return x < lo ? lo : x > hi ? hi : x;
  };
  const int oldCadence = cadence;
  const bool oldAsync = asyncMode, oldEnabled = enabled;
  enabled = Num(L"Enabled", enabled ? 1 : 0, 0, 1) != 0;
  cadence = int(Num(L"Cadence", cadence, 0, 8));
  {
    const std::wstring land = Get(L"Land");
    if (!land.empty()) landPoll = !lstrcmpiW(land.c_str(), L"poll");
    const std::wstring prio = Get(L"NetPriority");
    if (!prio.empty()) netPriority = !lstrcmpiW(prio.c_str(), L"high") ? 1 : 0;
    const std::wstring lb = Get(L"Label");
    label.assign(lb.begin(), lb.end());  // ASCII labels
  }
  fpsCap = int(Num(L"FpsCap", fpsCap, 0, 500));
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
  intensity = float(Num(L"Intensity", intensity, 0, 1));
  guardStrength = float(Num(L"Guard", guardStrength, 0, 1));
  split = Num(L"Split", split ? 1 : 0, 0, 1) != 0;
  keys.toggle = ParseKey(Get(L"KeyToggle"), keys.toggle);
  keys.cadence = ParseKey(Get(L"KeyCadence"), keys.cadence);
  keys.async = ParseKey(Get(L"KeyAsync"), keys.async);
  keys.split = ParseKey(Get(L"KeySplit"), keys.split);
  keys.capture = ParseKey(Get(L"KeyCapture"), keys.capture);
  if (cadence != oldCadence || asyncMode != oldAsync || enabled != oldEnabled) historyValid = false;
  if (Log) Log("prism-core.ini: %s, network every %d frame(s), async %s, land %s, %s priority, cap %d fps, intensity %.2f, guard %.2f [%s]", enabled ? "on" : "off", cadence, asyncMode ? "on" : "off", landPoll ? "poll" : "wait", netPriority ? "high" : "normal", fpsCap, intensity, guardStrength, label.c_str());
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
  device->CreateUnorderedAccessView(recMotion, nullptr, nullptr, Cpu(ring + 23));
  device->CreateUnorderedAccessView(recDepth, nullptr, nullptr, Cpu(ring + 26));
  ID3D12DescriptorHeap* heaps[] = {heap};
  l->SetDescriptorHeaps(1, heaps);
  l->SetComputeRootSignature(root);
  const Constants k = {w, h, tilesX * tilesY, tilesX, intensity, guardStrength, 0.05f, 0u, c.renderWidth, c.renderHeight, c.motionScaleX, c.motionScaleY, 0.05f, 0u, {0, 0}};
  l->SetComputeRoot32BitConstants(0, 16, &k, 0);
  l->SetComputeRootDescriptorTable(1, Gpu(ring));
  l->SetComputeRootDescriptorTable(2, Gpu(ring + 23));
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

void PipelineInit(const std::wstring& dir, int createFlags, LogFn log) {
  gameDir = dir;
  gameCreateFlags = createFlags;
  Log = log;
  LoadConfig();
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
  if (keys.toggle && Pressed(keys.toggle)) { enabled = !enabled; historyValid = false; Log("Prism NR %s (key)", enabled ? "on" : "off"); }
  if (keys.cadence && Pressed(keys.cadence)) { cadence = cadence % 4 + 1; historyValid = false; Log("network every %d frame(s) (key)", cadence); }
  if (keys.async && Pressed(keys.async)) { asyncMode = !asyncMode; historyValid = false; Log("asynchronous network %s (key)", asyncMode ? "on" : "off"); }
  if (keys.split && Pressed(keys.split)) { split = !split; Log("split view %s (key)", split ? "on" : "off"); }
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
        busy = inFlight && fenceDone && fenceDone->GetCompletedValue() < launchId && waited < 120;
        if (!busy) { inFlight = false; launchList = nullptr; landList = nullptr; }
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

  auto* l = c.list;
  const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  const UINT ring = UINT(frame % kRing) * kBlock;
  ID3D12Resource* history = edits[current ^ 1];
  ID3D12Resource* next = edits[current];
  ID3D12Resource* warpHistory = warps[current ^ 1];
  ID3D12Resource* warpNext = warps[current];
  // A new network priority: rebuild the compute queue between launches.
  static int builtPriority = -1;
  if (computeQueue && builtPriority != netPriority && !inFlight) ReleaseAsync();
  if (asyncMode && !computeQueue) {
    if (!SetupAsync(c)) return false;
    builtPriority = netPriority;
  }
  bool landNow = false, launchNow = false;
  const bool async = asyncMode && historyValid && !c.reset && cadence != 1;
  if (async) {
    std::lock_guard<std::mutex> lock(asyncMutex);
    // Land once the launch was submitted; in poll mode only once the network has also finished on
    // the GPU (otherwise carry one more frame: about 0.2 ms instead of a stall).
    if (inFlight && submittedId == launchId) landNow = !landPoll || fenceDone->GetCompletedValue() >= launchId;
    const int interval = cadence != 0 ? cadence : fpsCap > 0 ? adaptK : 1;  // uncapped adaptive: as soon as landed
    launchNow = !inFlight && frame - lastLaunchFrame >= UINT64(interval) && !(cadence == 0 && adaptHold > 0);
    // A network still running on the frame after its launch did not fit the idle time.
    if (inFlight && !landNow && frame == lastLaunchFrame + 1) ++adaptLate;
  }
  const bool refresh = async ? false : (!historyValid || c.reset || cadence <= 1 || frame % UINT64(cadence) == 0);
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R16G16_FLOAT; device->CreateShaderResourceView(c.motion, &sv, Cpu(ring + 3));
    const DXGI_FORMAT df = c.depth->GetDesc().Format;
    sv.Format = df == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
              : df == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
              : DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView(c.depth, &sv, Cpu(ring + 4));
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(history, &sv, Cpu(ring + 5));
    device->CreateShaderResourceView(warpHistory, &sv, Cpu(ring + 6));
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView(depthCopy, &sv, Cpu(ring + 7));  // null before the first async setup: unused then
    device->CreateUnorderedAccessView(next, nullptr, nullptr, Cpu(ring + 11));
    device->CreateUnorderedAccessView(c.output, nullptr, nullptr, Cpu(ring + 13));
    device->CreateUnorderedAccessView(next, nullptr, nullptr, Cpu(ring + 16));
    for (UINT set : {8u, 13u, 18u, 23u}) device->CreateUnorderedAccessView(warpNext, nullptr, nullptr, Cpu(ring + set + 4));
  }
  const Constants k = {w, h, tilesX * tilesY, tilesX, intensity, guardStrength, 0.05f, paint ? 2u : (split ? 1u : 0u),
                       c.renderWidth, c.renderHeight, c.motionScaleX, c.motionScaleY, 0.05f, launchNow ? 1u : 0u, {0, 0}};

  const UINT slot = UINT(frame % kRing);
  // The slot written kRing frames ago is long finished: read it before reusing it.
  if (stampData && frame >= kRing && tsFreq) {
    const uint64_t a = stampData[slot * 2], b = stampData[slot * 2 + 1];
    if (b > a) {
      const double ms = double(b - a) * 1000.0 / double(tsFreq);
      const int kind = slotRefresh[slot] ? 1 : 0;
      msSum[kind] += ms; ++msCount[kind]; msAll += ms; ++msTotal;
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
  ID3D12DescriptorHeap* heaps[] = {heap};
  l->SetDescriptorHeaps(1, heaps);
  l->SetComputeRootSignature(root);
  l->SetComputeRoot32BitConstants(0, 16, &k, 0);
  l->SetComputeRootDescriptorTable(1, Gpu(ring));
  if (landNow) {
    // Land: the network ran on our queue since the launch frame; the game's queue waits for it.
    Barrier(l, neural, UAV, SRV);
    l->SetComputeRootDescriptorTable(2, Gpu(ring + 13));
    l->SetPipelineState(psoLand); l->Dispatch(tilesX, tilesY, 1);
    UavBarrier(l);
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
      // Poll landing only lands after the fence passed, so this launch's stamps are in place.
      const UINT q0 = UINT((launchId - 1) & 1) * 2;
      const uint64_t a = netStampData[q0], b = netStampData[q0 + 1];
      if (b > a) netGpuMs = netGpuMs * 0.8 + 0.2 * (double(b - a) * 1000.0 / double(netTsFreq));
    }
    {
      std::lock_guard<std::mutex> lock(statsMutex);
      const uint32_t age = uint32_t(frame - lastLaunchFrame);
      ++statLands; statAgeSum += age; statAgeMax = std::max(statAgeMax, age);
      if (frameCsv) std::fprintf(frameCsv, "%.3f,L,%u\n", NowMs(), age);
    }
    if (queries && stamps) {
      l->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
      l->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, stamps, UINT64(slot) * 16);
    }
  } else if (!refresh) {
    if (launchNow) {
      // Launch: proxy and copies on the game's list; the network itself on our queue.
      l->SetComputeRootDescriptorTable(2, Gpu(ring + 8));
      l->SetPipelineState(psoReduce1); l->Dispatch(tilesX, tilesY, 1); UavBarrier(l);
      l->SetPipelineState(psoReduce2); l->Dispatch(1, 1, 1); UavBarrier(l);
      l->SetPipelineState(psoEncode); l->Dispatch(tilesX, tilesY, 1);
      Barrier(l, proxy, UAV, SRV);
      device->CreateUnorderedAccessView(motionCopy, nullptr, nullptr, Cpu(ring + 18));
      device->CreateUnorderedAccessView(depthCopy, nullptr, nullptr, Cpu(ring + 21));
      l->SetComputeRootDescriptorTable(2, Gpu(ring + 18));
      l->SetPipelineState(psoCopyGuides); l->Dispatch((c.renderWidth + 15) / 16, (c.renderHeight + 15) / 16, 1);
      UavBarrier(l);
      Barrier(l, motionCopy, UAV, SRV);
      Barrier(l, depthCopy, UAV, SRV);
      // Record the network on the compute list now; it is submitted when the game submits this list.
      computeAlloc[launchId & 1]->Reset();
      computeList->Reset(computeAlloc[launchId & 1], nullptr);
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
      // Motion since the network's previous run: the launch frame's own vectors scaled by the
      // cadence (constant velocity), so its temporal state lands about where the scene went.
      // The frames since the network's previous run (cadence 0 or poll landing make it vary).
      const float span = launchId == 0 ? float(std::max(cadence, 1)) : float(std::min<uint64_t>(frame - lastLaunchFrame, 8));
      ngx::SetF(ap, "DLSSNR.MVecScaleX", c.motionScaleX * span); ngx::SetF(ap, "DLSSNR.MVecScaleY", c.motionScaleY * span);
      ngx::SetI(ap, "DLSSNR.DepthInverted", (gameCreateFlags & (1 << 3)) ? 1 : 0);
      ngx::SetI(ap, "DLSSNR.Enabled", 1);
      ngx::SetI(ap, "DLSSNR.Reset", launchId == 0 ? 1 : 0);
      ngx::SetF(ap, "DLSSNR.Intensity", 1.f);
      const UINT q0 = UINT(launchId & 1) * 2;
      if (netQueries) computeList->EndQuery(netQueries, D3D12_QUERY_TYPE_TIMESTAMP, q0);
      const ngx::Result ar = nrEvaluate(computeList, asyncHandle, ap, nullptr);
      if (netQueries && netStamps) {
        computeList->EndQuery(netQueries, D3D12_QUERY_TYPE_TIMESTAMP, q0 + 1);
        computeList->ResolveQueryData(netQueries, D3D12_QUERY_TYPE_TIMESTAMP, q0, 2, netStamps, UINT64(q0) * 8);
      }
      computeList->Close();
      if (ar != ngx::kSuccess) { Log("async NR evaluate 0x%08x", ar); return Fail("async NR evaluate"); }
      {
        std::lock_guard<std::mutex> lock(asyncMutex);
        ++launchId;
        launchList = l;
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
      l->SetComputeRootSignature(root);
      l->SetComputeRoot32BitConstants(0, 16, &k, 0);
      l->SetComputeRootDescriptorTable(1, Gpu(ring));
    }
    // Carry: no network result this frame.
    l->SetComputeRootDescriptorTable(2, Gpu(ring + 13));
    l->SetPipelineState(psoCarry); l->Dispatch(tilesX, tilesY, 1);
    UavBarrier(l);
    if (queries && stamps) {
      l->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
      l->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, stamps, UINT64(slot) * 16);
    }
  } else {
  // 2-3. Exposure and the proxy.
  l->SetComputeRootDescriptorTable(2, Gpu(ring + 8));
  l->SetPipelineState(psoReduce1); l->Dispatch(tilesX, tilesY, 1); UavBarrier(l);
  l->SetPipelineState(psoReduce2); l->Dispatch(1, 1, 1); UavBarrier(l);
  l->SetPipelineState(psoEncode); l->Dispatch(tilesX, tilesY, 1);
  Barrier(l, proxy, UAV, SRV);
  // 4. The network.
  void* p = &nrParams;
  ngx::SetRes(p, "DLSSNR.Color", proxy);
  ngx::SetRes(p, "DLSSNR.Output", neural);
  ngx::SetRes(p, "DLSSNR.MVec", c.motion);
  ngx::SetRes(p, "DLSSNR.Depth", c.depth);
  for (const char* key : {"DLSSNR.ColorSubrect", "DLSSNR.OutputSubrect"}) {
    const std::string s(key);
    ngx::SetUI(p, (s + "BaseX").c_str(), 0); ngx::SetUI(p, (s + "BaseY").c_str(), 0);
    ngx::SetUI(p, (s + "Width").c_str(), w); ngx::SetUI(p, (s + "Height").c_str(), h);
  }
  for (const char* key : {"DLSSNR.DepthSubrect", "DLSSNR.MVecSubrect"}) {
    const std::string s(key);
    ngx::SetUI(p, (s + "BaseX").c_str(), 0); ngx::SetUI(p, (s + "BaseY").c_str(), 0);
    ngx::SetUI(p, (s + "Width").c_str(), c.renderWidth); ngx::SetUI(p, (s + "Height").c_str(), c.renderHeight);
  }
  ngx::SetF(p, "DLSSNR.MVecScaleX", c.motionScaleX); ngx::SetF(p, "DLSSNR.MVecScaleY", c.motionScaleY);
  ngx::SetI(p, "DLSSNR.DepthInverted", (gameCreateFlags & (1 << 3)) ? 1 : 0);
  ngx::SetI(p, "DLSSNR.Enabled", 1);
  ngx::SetI(p, "DLSSNR.Reset", (c.reset || frame == 0) ? 1 : 0);
  ngx::SetF(p, "DLSSNR.Intensity", 1.f);
  const ngx::Result r = nrEvaluate(l, nrHandle, p, nullptr);
  if (r != ngx::kSuccess) { Log("NR evaluate 0x%08x", r); return Fail("NR evaluate"); }
  // 5. Compose onto the game's frame. NGX changed the bindings: set ours again.
  Barrier(l, neural, UAV, SRV);
  l->SetDescriptorHeaps(1, heaps);
  l->SetComputeRootSignature(root);
  l->SetComputeRoot32BitConstants(0, 16, &k, 0);
  l->SetComputeRootDescriptorTable(1, Gpu(ring));
  l->SetComputeRootDescriptorTable(2, Gpu(ring + 13));
  l->SetPipelineState(psoCompose); l->Dispatch(tilesX, tilesY, 1);
  UavBarrier(l);
  historyValid = true;
  if (captureAt && captureAt <= frame + 1 && !captures[0].rb) {
    const Capture list4[4] = {{origCopy, "orig"}, {proxy, "proxy"}, {neural, "neural"}, {c.output, "final"}};
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
  Barrier(l, neural, SRV, UAV);
  Barrier(l, proxy, SRV, UAV);
  }  // refresh
  if (recordLeft > 0) RecordFrame(c, l, ring);
  if (recordDoneAt && frame >= recordDoneAt) WriteRecording();
  Barrier(l, history, SRV, UAV);
  Barrier(l, warpHistory, SRV, UAV);
  Barrier(l, origCopy, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
  current ^= 1;
  if (++frame == 1 || frame % 600 == 0) {
    const D3D12_RESOURCE_DESC d = c.output->GetDesc();
    Log("pipeline frame %llu output %p %llux%u fmt %d flags 0x%x list %p", (unsigned long long)frame, (void*)c.output, (unsigned long long)d.Width, d.Height, int(d.Format), unsigned(d.Flags), (void*)l);
  }
  return true;
}

}  // namespace prism

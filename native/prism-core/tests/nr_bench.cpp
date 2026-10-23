// NR bench: drives NVIDIA's signed DLSS Neural Rendering runtime (nvngx_dlssnr.dll, NGX
// feature 18) directly on an RTX GPU, outside any game, on frames we provide.
//
//   prism_nvngx.dll.exe <nvngx_dlssnr.dll> cost
//       network time at 720p, 1080p, 1440p, 1600p and 4K
//   prism_nvngx.dll.exe <nvngx_dlssnr.dll> seq <dir>
//       runs a frame sequence: <dir>/meta.txt holds "width height count"; each frame i has
//       c_i.f32 (RGBA float, display-encoded 0..1) and m_i.f32 (RG float, previous position
//       minus current, pixels), optionally d_i.f32 (R float depth; a fourth meta value 1 marks
//       inverted depth); writes o_i.f32 (RGBA float) and times.txt.
//
// The executable's name contains "nvngx.dll" on purpose: NGX only lets modules whose path
// contains it create features (found by neural-upstream).
//
// Interface notes (from RenoDX DLSS5, MIT, and neural-upstream, MIT): the runtime exports the
// D3D12 NGX entry points itself; the feature is created 1:1 at the working resolution; the
// parameter block is an MSVC C++ object, called here by vtable slot because MinGW lays out
// overloaded virtuals differently. Slots: Set(void*)=0, Set(ID3D12Resource*)=1, Set(int)=3,
// Set(uint)=4, Set(float)=6; Get(int*)=11, Get(uint*)=12, Get(float*)=14 (checked at start).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

#define CHECK(x)                                                                 \
  do {                                                                           \
    HRESULT hr_ = (x);                                                           \
    if (FAILED(hr_)) {                                                           \
      std::fprintf(stderr, "%s failed: 0x%08lx (line %d)\n", #x, hr_, __LINE__); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

using NgxResult = uint32_t;
constexpr NgxResult kNgxSuccess = 0x1;
constexpr uint32_t kFeatureNr = 18;
constexpr uint32_t kNgxVersionApi = 0x15;
constexpr unsigned long long kApplicationId = 0x876232cULL;  // the id RenoDX's direct path uses

using InitFn = NgxResult (*)(unsigned long long, const wchar_t*, ID3D12Device*, uint32_t, const void*);
using AllocateFn = NgxResult (*)(void**);
using CreateFn = NgxResult (*)(ID3D12GraphicsCommandList*, uint32_t, void*, void**);
using EvaluateFn = NgxResult (*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
using ReleaseFn = NgxResult (*)(void*);
using ShutdownFn = NgxResult (*)(ID3D12Device*);

struct Ngx {
  InitFn init;
  AllocateFn allocate;
  CreateFn create;
  EvaluateFn evaluate;
  ReleaseFn release;
  ShutdownFn shutdown;
};

void** Vt(void* p) { return *reinterpret_cast<void***>(p); }
void SetPtr(void* p, const char* k, void* v) { reinterpret_cast<void (*)(void*, const char*, void*)>(Vt(p)[0])(p, k, v); }
void SetRes(void* p, const char* k, ID3D12Resource* v) { reinterpret_cast<void (*)(void*, const char*, ID3D12Resource*)>(Vt(p)[1])(p, k, v); }
void SetI(void* p, const char* k, int v) { reinterpret_cast<void (*)(void*, const char*, int)>(Vt(p)[3])(p, k, v); }
void SetUI(void* p, const char* k, unsigned v) { reinterpret_cast<void (*)(void*, const char*, unsigned)>(Vt(p)[4])(p, k, v); }
void SetF(void* p, const char* k, float v) { reinterpret_cast<void (*)(void*, const char*, float)>(Vt(p)[6])(p, k, v); }
NgxResult GetUI(void* p, const char* k, unsigned* v) { return reinterpret_cast<NgxResult (*)(void*, const char*, unsigned*)>(Vt(p)[12])(p, k, v); }
NgxResult GetF(void* p, const char* k, float* v) { return reinterpret_cast<NgxResult (*)(void*, const char*, float*)>(Vt(p)[14])(p, k, v); }

// Our own NVSDK_NGX_Parameter: the signed NR runtime exports no parameter allocator, so the
// block is ours. It is laid out as an MSVC object (a vtable pointer first) with the 17 slots in
// MSVC order: each overload group reversed. Values convert between numeric types on Get, since
// the runtime may read a key as a different type than it was written.
struct OwnedParams {
  void** vtable;
  struct Value { enum Kind { kNum, kPtr } kind; double num; void* ptr; };
  std::vector<std::pair<std::string, Value>> values;

  Value* Find(const char* k) {
    for (auto& [name, v] : values) if (name == k) return &v;
    return nullptr;
  }
  void Put(const char* k, Value v) {
    if (Value* old = Find(k)) *old = v;
    else values.emplace_back(k, v);
  }
};
constexpr NgxResult kNgxFail = 0xBAD00000u;
void PSetPtr(OwnedParams* p, const char* k, void* v) { p->Put(k, {OwnedParams::Value::kPtr, 0.0, v}); }
void PSetNumULL(OwnedParams* p, const char* k, unsigned long long v) { p->Put(k, {OwnedParams::Value::kNum, double(v), nullptr}); }
void PSetNumF(OwnedParams* p, const char* k, float v) { p->Put(k, {OwnedParams::Value::kNum, double(v), nullptr}); }
void PSetNumD(OwnedParams* p, const char* k, double v) { p->Put(k, {OwnedParams::Value::kNum, v, nullptr}); }
void PSetNumUI(OwnedParams* p, const char* k, unsigned v) { p->Put(k, {OwnedParams::Value::kNum, double(v), nullptr}); }
void PSetNumI(OwnedParams* p, const char* k, int v) { p->Put(k, {OwnedParams::Value::kNum, double(v), nullptr}); }
template <typename T>
NgxResult PGetNum(OwnedParams* p, const char* k, T* out) {
  OwnedParams::Value* v = p->Find(k);
  if (!v || v->kind != OwnedParams::Value::kNum || !out) return kNgxFail;
  *out = T(v->num);
  return kNgxSuccess;
}
NgxResult PGetPtr(OwnedParams* p, const char* k, void** out) {
  OwnedParams::Value* v = p->Find(k);
  if (!v || v->kind != OwnedParams::Value::kPtr || !out) return kNgxFail;
  *out = v->ptr;
  return kNgxSuccess;
}
void PReset(OwnedParams* p) { p->values.clear(); }

void* kOwnedVtable[17] = {
    reinterpret_cast<void*>(&PSetPtr),                    // 0 Set(void*)
    reinterpret_cast<void*>(&PSetPtr),                    // 1 Set(ID3D12Resource*)
    reinterpret_cast<void*>(&PSetPtr),                    // 2 Set(ID3D11Resource*)
    reinterpret_cast<void*>(&PSetNumI),                   // 3 Set(int)
    reinterpret_cast<void*>(&PSetNumUI),                  // 4 Set(unsigned int)
    reinterpret_cast<void*>(&PSetNumD),                   // 5 Set(double)
    reinterpret_cast<void*>(&PSetNumF),                   // 6 Set(float)
    reinterpret_cast<void*>(&PSetNumULL),                 // 7 Set(unsigned long long)
    reinterpret_cast<void*>(&PGetPtr),                    // 8 Get(void**)
    reinterpret_cast<void*>(&PGetPtr),                    // 9 Get(ID3D12Resource**)
    reinterpret_cast<void*>(&PGetPtr),                    // 10 Get(ID3D11Resource**)
    reinterpret_cast<void*>(&PGetNum<int>),               // 11 Get(int*)
    reinterpret_cast<void*>(&PGetNum<unsigned>),          // 12 Get(unsigned int*)
    reinterpret_cast<void*>(&PGetNum<double>),            // 13 Get(double*)
    reinterpret_cast<void*>(&PGetNum<float>),             // 14 Get(float*)
    reinterpret_cast<void*>(&PGetNum<unsigned long long>),// 15 Get(unsigned long long*)
    reinterpret_cast<void*>(&PReset),                     // 16 Reset()
};
NgxResult AllocateOwned(void** out) {
  auto* p = new OwnedParams();
  p->vtable = kOwnedVtable;
  *out = p;
  return kNgxSuccess;
}

// The runtime asks for the scaling ratio of the low-resolution input contract; it is always 1.
// PRISM_RATIO overrides it (probe: does the runtime run the network smaller and guide it back up?).
float EnvF(const char* k, float d) { const char* v = std::getenv(k); return v ? float(std::atof(v)) : d; }
NgxResult ScalingRatioCallback(void* parameters) {
  SetF(parameters, "DLSSNR.ScalingRatio", EnvF("PRISM_RATIO", 1.f));
  return kNgxSuccess;
}

uint16_t ToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  int32_t exp = int32_t((x >> 23) & 0xffu) - 127 + 15;
  uint32_t mant = x & 0x7fffffu;
  if (exp <= 0) return uint16_t(sign);
  if (exp >= 31) return uint16_t(sign | 0x7c00u);
  uint32_t h = sign | (uint32_t(exp) << 10) | (mant >> 13);
  if (mant & 0x1000u) ++h;
  return uint16_t(h);
}
float FromHalf(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const int32_t exp = (h >> 10) & 0x1f;
  const uint32_t mant = h & 0x3ffu;
  uint32_t x = exp == 0 ? sign : sign | (uint32_t(exp - 15 + 127) << 23) | (mant << 13);
  if (exp == 31) x = sign | 0x7f800000u | (mant << 13);
  float f;
  std::memcpy(&f, &x, 4);
  return f;
}

struct Gpu {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE event = nullptr;
  uint64_t value = 0;
  void Submit() {
    CHECK(list->Close());
    ID3D12CommandList* l[] = {list};
    queue->ExecuteCommandLists(1, l);
    CHECK(queue->Signal(fence, ++value));
    CHECK(fence->SetEventOnCompletion(value, event));
    WaitForSingleObject(event, INFINITE);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator, nullptr));
  }
};

void InitGpu(Gpu& g) {
  IDXGIFactory1* factory = nullptr;
  CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)));
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    if (d.VendorId == 0x10de) {
      std::printf("adapter: %ls\n", d.Description);
      break;
    }
    adapter->Release();
    adapter = nullptr;
  }
  if (!adapter) { std::fprintf(stderr, "no NVIDIA GPU\n"); std::exit(1); }
  CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&g.device)));
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  CHECK(g.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g.queue)));
  CHECK(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&g.allocator)));
  CHECK(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&g.list)));
  CHECK(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g.fence)));
  g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

ID3D12Resource* Texture(Gpu& g, uint32_t w, uint32_t h, DXGI_FORMAT f, bool uav, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = f;
  d.SampleDesc.Count = 1;
  d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  ID3D12Resource* r = nullptr;
  CHECK(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&r)));
  return r;
}

ID3D12Resource* Staging(Gpu& g, uint64_t bytes, D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = type;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* r = nullptr;
  const auto state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
  CHECK(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&r)));
  return r;
}

void Transition(Gpu& g, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  if (a == b) return;
  D3D12_RESOURCE_BARRIER br = {};
  br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  br.Transition.pResource = r;
  br.Transition.StateBefore = a;
  br.Transition.StateAfter = b;
  br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  g.list->ResourceBarrier(1, &br);
}

// Uploads tightly packed rows (bytesPerPixel each) into a texture left in `after` state.
void UploadTexture(Gpu& g, ID3D12Resource* tex, const void* data, uint32_t w, uint32_t h, uint32_t bpp, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_DESC d = tex->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows;
  UINT64 rowBytes, total;
  g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
  ID3D12Resource* up = Staging(g, total, D3D12_HEAP_TYPE_UPLOAD);
  uint8_t* p = nullptr;
  CHECK(up->Map(0, nullptr, reinterpret_cast<void**>(&p)));
  for (uint32_t y = 0; y < h; ++y) std::memcpy(p + fp.Offset + y * fp.Footprint.RowPitch, static_cast<const uint8_t*>(data) + size_t(y) * w * bpp, size_t(w) * bpp);
  up->Unmap(0, nullptr);
  Transition(g, tex, before, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
  dst.pResource = tex;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource = up;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = fp;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Transition(g, tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
  g.Submit();
  up->Release();
}

std::vector<uint16_t> ReadTexture(Gpu& g, ID3D12Resource* tex, uint32_t w, uint32_t h, D3D12_RESOURCE_STATES state) {
  D3D12_RESOURCE_DESC d = tex->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows;
  UINT64 rowBytes, total;
  g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
  ID3D12Resource* rb = Staging(g, total, D3D12_HEAP_TYPE_READBACK);
  Transition(g, tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
  dst.pResource = rb;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = fp;
  src.pResource = tex;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Transition(g, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  g.Submit();
  std::vector<uint16_t> out(size_t(w) * h * 4);
  uint8_t* p = nullptr;
  CHECK(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)));
  for (uint32_t y = 0; y < h; ++y) std::memcpy(out.data() + size_t(y) * w * 4, p + fp.Offset + y * fp.Footprint.RowPitch, size_t(w) * 8);
  rb->Unmap(0, nullptr);
  rb->Release();
  return out;
}

Ngx LoadNgx(const std::wstring& path) {
  HMODULE m = LoadLibraryW(path.c_str());
  if (!m) { std::fprintf(stderr, "cannot load %ls (error %lu)\n", path.c_str(), GetLastError()); std::exit(1); }
  Ngx n = {};
  n.init = reinterpret_cast<InitFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext"));
  n.allocate = reinterpret_cast<AllocateFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_AllocateParameters"));
  if (!n.allocate) n.allocate = &AllocateOwned;  // the signed NR runtime has none: use our own block
  n.create = reinterpret_cast<CreateFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature"));
  n.evaluate = reinterpret_cast<EvaluateFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature"));
  n.release = reinterpret_cast<ReleaseFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature"));
  n.shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_Shutdown1"));
  if (!n.init || !n.allocate || !n.create || !n.evaluate || !n.release) { std::fprintf(stderr, "runtime lacks NGX exports\n"); std::exit(1); }
  return n;
}

// One NR feature at a fixed size with its textures. Static depth; motion uploaded per frame.
struct Feature {
  uint32_t w = 0, h = 0;
  void* params = nullptr;
  void* handle = nullptr;
  ID3D12Resource *color = nullptr, *output = nullptr, *motion = nullptr, *depth = nullptr;
  bool first = true;
  int depthInverted = 0;
};

const auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

void CreateFeature(Gpu& g, Ngx& ngx, Feature& f, uint32_t w, uint32_t h) {
  f.w = w;
  f.h = h;
  if (ngx.allocate(&f.params) != kNgxSuccess || !f.params) { std::fprintf(stderr, "AllocateParameters failed\n"); std::exit(1); }
  void* p = f.params;
  const float in = EnvF("PRISM_IN", 1.f);  // probe: a smaller input than output
  const uint32_t iw = uint32_t(w * in), ih = uint32_t(h * in);
  SetUI(p, "Width", iw);
  SetUI(p, "Height", ih);
  SetUI(p, "OutWidth", w);
  SetUI(p, "OutHeight", h);
  for (const char* k : {"DLSSNR.Width", "DLSSNR.OutputWidth", "DLSSNR.Output.Width"}) SetUI(p, k, w);
  for (const char* k : {"DLSSNR.Height", "DLSSNR.OutputHeight", "DLSSNR.Output.Height"}) SetUI(p, k, h);
  SetUI(p, "DLSSNR.InputWidth", iw);
  SetUI(p, "DLSSNR.InputHeight", ih);
  SetF(p, "DLSSNR.ScalingRatio", EnvF("PRISM_RATIO", 1.f));
  SetF(p, "DLSSNR.Scale", EnvF("PRISM_RATIO", 1.f));
  if (!std::getenv("PRISM_NOCB")) SetPtr(p, "DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&ScalingRatioCallback));
  SetI(p, "PerfQualityValue", int(EnvF("PRISM_PQ", 0)));
  if (std::getenv("PRISM_PRESET")) SetI(p, "DLSSNR.Hint.Render.Preset", int(EnvF("PRISM_PRESET", 0)));
  SetI(p, "DLSS.Feature.Create.Flags", 0);
  SetUI(p, "CreationNodeMask", 1);
  SetUI(p, "VisibilityNodeMask", 1);
  const NgxResult r = ngx.create(g.list, kFeatureNr, p, &f.handle);
  if (r != kNgxSuccess || !f.handle) { std::fprintf(stderr, "CreateFeature(18) %ux%u failed: 0x%08x\n", w, h, r); std::exit(1); }
  g.Submit();
  f.color = Texture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, kSrv);
  f.output = Texture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, kUav);
  f.motion = Texture(g, w, h, DXGI_FORMAT_R16G16_FLOAT, false, kSrv);
  f.depth = Texture(g, w, h, DXGI_FORMAT_R32_FLOAT, false, kSrv);
  const std::vector<float> d(size_t(w) * h, 0.5f);
  UploadTexture(g, f.depth, d.data(), w, h, 4, kSrv, kSrv);
  const std::vector<uint32_t> zero(size_t(w) * h, 0);
  UploadTexture(g, f.motion, zero.data(), w, h, 4, kSrv, kSrv);
}

float activeScale = -1.f;  // drs: this frame's active size as a fraction of the feature's
float colorScale = -1.f;   // nvs: the official scaled mode's colour scale
ID3D12Resource* probeBackbuffer = nullptr;  // nvs: the full-size frame given as the backbuffer

void RecordEvaluateOn(ID3D12GraphicsCommandList* list, Ngx& ngx, Feature& f) {
  void* p = f.params;
  SetRes(p, "DLSSNR.Color", f.color);
  SetRes(p, "DLSSNR.Output", f.output);
  SetRes(p, "DLSSNR.MVec", f.motion);
  SetRes(p, "DLSSNR.Depth", f.depth);
  // Probe: PRISM_ACTIVE evaluates only the top-left part of the feature (its active size).
  const float act = activeScale > 0.f ? activeScale : EnvF("PRISM_ACTIVE", 1.f);
  const uint32_t aw = uint32_t(f.w * act), ah = uint32_t(f.h * act);
  // PRISM_SUBONLY: the active size in the subrects only (the sizes stay the feature's), as Core does.
  const bool subOnly = std::getenv("PRISM_SUBONLY") != nullptr;
  SetUI(p, "Width", subOnly ? f.w : aw);
  SetUI(p, "Height", subOnly ? f.h : ah);
  SetUI(p, "OutWidth", subOnly ? f.w : aw);
  SetUI(p, "OutHeight", subOnly ? f.h : ah);
  for (const char* k : {"DLSSNR.ColorSubrect", "DLSSNR.DepthSubrect", "DLSSNR.MVecSubrect", "DLSSNR.OutputSubrect"}) {
    const std::string s(k);
    SetUI(p, (s + "BaseX").c_str(), 0);
    SetUI(p, (s + "BaseY").c_str(), 0);
    SetUI(p, (s + "Width").c_str(), aw);
    SetUI(p, (s + "Height").c_str(), ah);
  }
  // Probe: PRISM_COLORSCALE runs the official scaled mode: a smaller colour input, the output at full
  // size, the full-size frame as the backbuffer (the guide), the scaling ratio set.
  if (const char* cs = colorScale > 0.f ? "" : std::getenv("PRISM_COLORSCALE")) {
    const float k = colorScale > 0.f ? colorScale : float(std::atof(cs));
    const uint32_t cw = uint32_t(f.w * k), chh = uint32_t(f.h * k);
    for (const char* key : {"DLSSNR.ColorSubrect", "DLSSNR.DepthSubrect", "DLSSNR.MVecSubrect"}) {
      const std::string kk(key);
      SetUI(p, (kk + "Width").c_str(), cw); SetUI(p, (kk + "Height").c_str(), chh);
    }
    SetUI(p, "Width", cw); SetUI(p, "Height", chh);
    SetUI(p, "OutWidth", f.w); SetUI(p, "OutHeight", f.h);
    SetF(p, "DLSSNR.ScalingRatio", k);
    if (probeBackbuffer || std::getenv("PRISM_BACKBUFFER")) {
      SetRes(p, "DLSSNR.Backbuffer", probeBackbuffer ? probeBackbuffer : f.color);
      for (const char* kk2 : {"BaseX", "BaseY"}) SetUI(p, (std::string("DLSSNR.BackbufferSubrect") + kk2).c_str(), 0);
      SetUI(p, "DLSSNR.BackbufferSubrectWidth", f.w); SetUI(p, "DLSSNR.BackbufferSubrectHeight", f.h);
    }
  }
  SetF(p, "DLSSNR.MVecScaleX", 1.f);
  SetF(p, "DLSSNR.MVecScaleY", 1.f);
  SetI(p, "DLSSNR.DepthInverted", f.depthInverted);
  SetI(p, "DLSSNR.Enabled", 1);
  SetI(p, "DLSSNR.Reset", f.first ? 1 : 0);
  SetF(p, "DLSSNR.Intensity", 1.f);
  // Probe: PRISM_SETI / PRISM_SETF = "key=value,key=value" set ints / floats on every evaluate.
  for (const char* env : {"PRISM_SETI", "PRISM_SETF"}) {
    const char* v = std::getenv(env);
    if (!v) continue;
    std::string all = v;
    size_t at = 0;
    while (at < all.size()) {
      size_t end = all.find(',', at);
      if (end == std::string::npos) end = all.size();
      const std::string kv = all.substr(at, end - at);
      const size_t eq = kv.find('=');
      if (eq != std::string::npos) {
        const std::string k = kv.substr(0, eq);
        if (env[10] == 'I') SetI(p, k.c_str(), std::atoi(kv.c_str() + eq + 1));
        else SetF(p, k.c_str(), float(std::atof(kv.c_str() + eq + 1)));
      }
      at = end + 1;
    }
  }
  f.first = false;
  const NgxResult r = ngx.evaluate(list, f.handle, p, nullptr);
  if (r != kNgxSuccess) { std::fprintf(stderr, "EvaluateFeature failed: 0x%08x\n", r); std::exit(1); }
}

void RecordEvaluate(Gpu& g, Ngx& ngx, Feature& f) { RecordEvaluateOn(g.list, ngx, f); }

struct Timer {
  ID3D12QueryHeap* heap = nullptr;
  ID3D12Resource* rb = nullptr;
  uint64_t freq = 0;
  void Init(Gpu& g) {
    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2;
    CHECK(g.device->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&heap)));
    rb = Staging(g, 16, D3D12_HEAP_TYPE_READBACK);
    CHECK(g.queue->GetTimestampFrequency(&freq));
  }
  double Read() {
    uint64_t* t = nullptr;
    CHECK(rb->Map(0, nullptr, reinterpret_cast<void**>(&t)));
    const double ms = double(t[1] - t[0]) * 1000.0 / double(freq);
    rb->Unmap(0, nullptr);
    return ms;
  }
};

// Evaluate wrapped in timestamps; returns GPU milliseconds.
double TimedEvaluate(Gpu& g, Ngx& ngx, Feature& f, Timer& t) {
  g.list->EndQuery(t.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
  RecordEvaluate(g, ngx, f);
  g.list->EndQuery(t.heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
  g.list->ResolveQueryData(t.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, t.rb, 0);
  g.Submit();
  return t.Read();
}

void CheckSlots(Ngx& ngx) {
  void* p = nullptr;
  if (ngx.allocate(&p) != kNgxSuccess || !p) { std::fprintf(stderr, "AllocateParameters failed\n"); std::exit(1); }
  SetUI(p, "Prism.Probe.UI", 1234567u);
  SetF(p, "Prism.Probe.F", 0.625f);
  unsigned ui = 0;
  float fl = 0.f;
  const bool ok = GetUI(p, "Prism.Probe.UI", &ui) == kNgxSuccess && ui == 1234567u && GetF(p, "Prism.Probe.F", &fl) == kNgxSuccess && fl == 0.625f;
  std::printf("parameter vtable slots: %s (uint %u, float %g)\n", ok ? "verified" : "WRONG", ui, fl);
  if (!ok) std::exit(1);
}

int Cost(Gpu& g, Ngx& ngx) {
  Timer t;
  t.Init(g);
  const uint32_t sizes[][2] = {{1280, 720}, {1600, 1000}, {1920, 1080}, {1920, 1200}, {2240, 1400}, {2560, 1440}, {2560, 1600}, {3840, 2160}};
  for (auto& s : sizes) {
    Feature f;
    CreateFeature(g, ngx, f, s[0], s[1]);
    // A smooth grey-to-skin gradient so the network has something plausible to work on.
    std::vector<uint16_t> c(size_t(s[0]) * s[1] * 4);
    for (uint32_t y = 0; y < s[1]; ++y)
      for (uint32_t x = 0; x < s[0]; ++x) {
        const float u = float(x) / s[0], v = float(y) / s[1];
        uint16_t* px = &c[(size_t(y) * s[0] + x) * 4];
        px[0] = ToHalf(0.3f + 0.4f * u); px[1] = ToHalf(0.25f + 0.25f * v); px[2] = ToHalf(0.2f + 0.1f * u * v); px[3] = ToHalf(1.f);
      }
    UploadTexture(g, f.color, c.data(), s[0], s[1], 8, kSrv, kSrv);
    std::vector<double> ms;
    for (int i = 0; i < 40; ++i) {
      const double m = TimedEvaluate(g, ngx, f, t);
      if (i >= 10) ms.push_back(m);
    }
    std::sort(ms.begin(), ms.end());
    const double mp = double(s[0]) * s[1] / 1e6;
    std::printf("%5ux%-5u %5.2f MP  network %6.3f ms  (%.3f ms/MP, median of %zu)\n", s[0], s[1], mp, ms[ms.size() / 2], ms[ms.size() / 2] / mp, ms.size());
    ngx.release(f.handle);
    for (auto* r : {f.color, f.output, f.motion, f.depth}) r->Release();
  }
  return 0;
}

std::vector<float> ReadF32(const std::string& file, size_t count) {
  std::ifstream in(file, std::ios::binary);
  if (!in) { std::fprintf(stderr, "missing %s\n", file.c_str()); std::exit(1); }
  std::vector<float> v(count);
  in.read(reinterpret_cast<char*>(v.data()), std::streamsize(count * 4));
  return v;
}

int Sequence(Gpu& g, Ngx& ngx, const std::string& dir) {
  uint32_t w = 0, h = 0, count = 0;
  int inverted = 0;
  {
    std::ifstream m(dir + "/meta.txt");
    m >> w >> h >> count;
    if (!(m >> inverted)) inverted = 0;
  }
  if (!w || !h || !count) { std::fprintf(stderr, "bad %s/meta.txt\n", dir.c_str()); return 1; }
  Feature f;
  CreateFeature(g, ngx, f, w, h);
  f.depthInverted = inverted;
  Timer t;
  t.Init(g);
  std::ofstream times(dir + "/times.txt");
  const size_t px = size_t(w) * h;
  for (uint32_t i = 0; i < count; ++i) {
    const auto c = ReadF32(dir + "/c_" + std::to_string(i) + ".f32", px * 4);
    const auto mv = ReadF32(dir + "/m_" + std::to_string(i) + ".f32", px * 2);
    std::vector<uint16_t> ch(px * 4), mh(px * 2);
    for (size_t k = 0; k < px * 4; ++k) ch[k] = ToHalf(c[k]);
    for (size_t k = 0; k < px * 2; ++k) mh[k] = ToHalf(mv[k]);
    UploadTexture(g, f.color, ch.data(), w, h, 8, kSrv, kSrv);
    UploadTexture(g, f.motion, mh.data(), w, h, 4, kSrv, kSrv);
    const std::string dp = dir + "/d_" + std::to_string(i) + ".f32";
    if (std::ifstream(dp).good()) {
      const auto d = ReadF32(dp, px);
      UploadTexture(g, f.depth, d.data(), w, h, 4, kSrv, kSrv);
    }
    const double ms = TimedEvaluate(g, ngx, f, t);
    times << ms << "\n";
    const auto out = ReadTexture(g, f.output, w, h, kUav);
    std::vector<float> o(px * 4);
    for (size_t k = 0; k < px * 4; ++k) o[k] = FromHalf(out[k]);
    std::ofstream of(dir + "/o_" + std::to_string(i) + ".f32", std::ios::binary);
    of.write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size() * 4));
  }
  std::printf("%s: %u frames at %ux%u\n", dir.c_str(), count, w, h);
  return 0;
}

// ------------------------------------------------------------------ async overlap
// How much of the network's cost hides behind other GPU work when it runs on a compute queue of
// its own? A stand-in "game frame" runs on the direct queue (a compute shader that is either
// arithmetic-bound or bandwidth-bound), the network on the compute queue, and the wall time of
// both together is compared with each alone.
const char* kLoadHlsl = R"(
RWStructuredBuffer<float4> Buf : register(u0);
cbuffer C : register(b0) { uint Iterations; uint Count; uint Mode; uint Pad; };
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= Count) return;
  float4 v = Buf[id.x];
  if (Mode == 0) {
    [loop] for (uint i = 0; i < Iterations; ++i) v = v * 1.0001 + sin(v) * 0.0001;  // arithmetic
  } else {
    [loop] for (uint i = 0; i < Iterations; ++i) v += Buf[(id.x * 7919u + i * 104729u) % Count] * 1e-6;  // memory
  }
  Buf[id.x] = v;
}
)";

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

int Async(Gpu& g, Ngx& ngx, uint32_t w, uint32_t h) {
  // Compute queue, its own allocator and list.
  ID3D12CommandQueue* cq = nullptr;
  ID3D12CommandAllocator* ca = nullptr;
  ID3D12GraphicsCommandList* cl = nullptr;
  ID3D12Fence* cf = nullptr;
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  CHECK(g.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&cq)));
  CHECK(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&ca)));
  CHECK(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, ca, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cl)));
  CHECK(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&cf)));
  uint64_t cv = 0;
  HANDLE cev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  // The network, created and evaluated on the compute list.
  Feature f;
  {
    ID3D12GraphicsCommandList* saved = g.list;
    g.list = cl;  // CreateFeature records into g.list; submit it on the compute queue by hand.
    f.w = w; f.h = h;
    ngx.allocate(&f.params);
    void* p = f.params;
    SetUI(p, "Width", w); SetUI(p, "Height", h); SetUI(p, "OutWidth", w); SetUI(p, "OutHeight", h);
    for (const char* k : {"DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth", "DLSSNR.Output.Width"}) SetUI(p, k, w);
    for (const char* k : {"DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight", "DLSSNR.Output.Height"}) SetUI(p, k, h);
    SetF(p, "DLSSNR.ScalingRatio", 1.f); SetF(p, "DLSSNR.Scale", 1.f);
    SetPtr(p, "DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&ScalingRatioCallback));
    SetI(p, "PerfQualityValue", 0); SetI(p, "DLSS.Feature.Create.Flags", 0);
    SetUI(p, "CreationNodeMask", 1); SetUI(p, "VisibilityNodeMask", 1);
    const NgxResult r = ngx.create(cl, kFeatureNr, p, &f.handle);
    g.list = saved;
    if (r != kNgxSuccess) { std::fprintf(stderr, "CreateFeature on a compute list failed: 0x%08x\n", r); return 1; }
    CHECK(cl->Close());
    ID3D12CommandList* l[] = {cl};
    cq->ExecuteCommandLists(1, l);
    CHECK(cq->Signal(cf, ++cv)); CHECK(cf->SetEventOnCompletion(cv, cev)); WaitForSingleObject(cev, INFINITE);
    CHECK(ca->Reset()); CHECK(cl->Reset(ca, nullptr));
  }
  f.color = Texture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, kSrv);
  f.output = Texture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, kUav);
  f.motion = Texture(g, w, h, DXGI_FORMAT_R16G16_FLOAT, false, kSrv);
  f.depth = Texture(g, w, h, DXGI_FORMAT_R32_FLOAT, false, kSrv);
  {
    std::vector<uint16_t> c(size_t(w) * h * 4);
    for (size_t i = 0; i < c.size(); i += 4) { c[i] = ToHalf(0.5f); c[i + 1] = ToHalf(0.4f); c[i + 2] = ToHalf(0.35f); c[i + 3] = ToHalf(1.f); }
    UploadTexture(g, f.color, c.data(), w, h, 8, kSrv, kSrv);
    const std::vector<float> d(size_t(w) * h, 0.5f);
    UploadTexture(g, f.depth, d.data(), w, h, 4, kSrv, kSrv);
    const std::vector<uint32_t> z(size_t(w) * h, 0);
    UploadTexture(g, f.motion, z.data(), w, h, 4, kSrv, kSrv);
  }

  // The stand-in frame.
  HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = reinterpret_cast<CompileFn>(GetProcAddress(dc, "D3DCompile"));
  ID3DBlob* cs = nullptr;
  ID3DBlob* err = nullptr;
  if (FAILED(compile(kLoadHlsl, std::strlen(kLoadHlsl), "load", nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs, &err))) {
    std::fprintf(stderr, "%s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "compile failed");
    return 1;
  }
  D3D12_ROOT_PARAMETER rp[2] = {};
  rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  rp[0].Constants.Num32BitValues = 4;
  rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rd = {};
  rd.NumParameters = 2;
  rd.pParameters = rp;
  ID3DBlob* rs = nullptr;
  CHECK(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err));
  ID3D12RootSignature* root = nullptr;
  CHECK(g.device->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&root)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = root;
  pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  ID3D12PipelineState* pso = nullptr;
  CHECK(g.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso)));
  const uint32_t count = 16u << 20;  // 16M float4 = 256 MB
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = uint64_t(count) * 16;
  bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource* buf = nullptr;
  CHECK(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, kUav, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buf)));

  auto recordLoad = [&](uint32_t iterations, uint32_t mode) {
    g.list->SetPipelineState(pso);
    g.list->SetComputeRootSignature(root);
    const uint32_t c[4] = {iterations, count, mode, 0};
    g.list->SetComputeRoot32BitConstants(0, 4, c, 0);
    g.list->SetComputeRootUnorderedAccessView(1, buf->GetGPUVirtualAddress());
    g.list->Dispatch(count / 256, 1, 1);
  };
  auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  // Runs: load only, network only, or both at once; returns wall milliseconds.
  auto run = [&](bool load, bool net, uint32_t iterations, uint32_t mode) {
    if (load) recordLoad(iterations, mode);
    CHECK(g.list->Close());
    if (net) RecordEvaluateOn(cl, ngx, f);
    CHECK(cl->Close());
    const double t0 = now();
    ID3D12CommandList* gl[] = {g.list};
    ID3D12CommandList* nl[] = {cl};
    g.queue->ExecuteCommandLists(1, gl);
    cq->ExecuteCommandLists(1, nl);
    CHECK(g.queue->Signal(g.fence, ++g.value));
    CHECK(cq->Signal(cf, ++cv));
    CHECK(g.fence->SetEventOnCompletion(g.value, g.event));
    WaitForSingleObject(g.event, INFINITE);
    CHECK(cf->SetEventOnCompletion(cv, cev));
    WaitForSingleObject(cev, INFINITE);
    const double ms = now() - t0;
    CHECK(g.allocator->Reset()); CHECK(g.list->Reset(g.allocator, nullptr));
    CHECK(ca->Reset()); CHECK(cl->Reset(ca, nullptr));
    return ms;
  };
  auto median = [&](bool load, bool net, uint32_t it, uint32_t mode) {
    std::vector<double> v;
    for (int i = 0; i < 25; ++i) { const double m = run(load, net, it, mode); if (i >= 5) v.push_back(m); }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  const double empty = median(false, false, 0, 0);
  const double net = median(false, true, 0, 0) - empty;
  std::printf("%ux%u: network alone %.2f ms (submission overhead %.2f ms subtracted)\n", w, h, net, empty);
  for (uint32_t mode = 0; mode < 2; ++mode) {
    // Size the stand-in frame to about 2.5x the network, like a game rendering at 60-100 fps.
    uint32_t it = mode == 0 ? 64 : 16;
    for (int k = 0; k < 6; ++k) {
      const double t = median(true, false, it, mode) - empty;
      if (t > 0) it = std::max(1u, uint32_t(double(it) * (net * 2.5) / t));
    }
    const double load = median(true, false, it, mode) - empty;
    const double both = median(true, true, it, mode) - empty;
    const double hidden = (load + net - both) / net;
    std::printf("  %-10s frame %.2f ms + network %.2f ms -> together %.2f ms: %.0f%% of the network hidden\n",
                mode == 0 ? "arithmetic" : "memory", load, net, both, hidden * 100.0);
  }
  return 0;
}

// ------------------------------------------------------------------ dynamic network resolution
// A feature made at full size evaluates only its top-left part (its active size) when the subrects
// say so, and costs in proportion. Is that part's output as good as a feature made at that size,
// and does the network's history stay steady when the active size changes from frame to frame?
// A still panned by dx pixels per frame stands in for a camera move (motion known exactly). Each
// run's edit (log2 luminance ratio of output to input, at full size after a bilinear upsample) is
// compared with the native run's (error), with its own previous frame shifted by the pan
// (flicker), and its mean size is reported (strength).
std::vector<float> LoadStill(const std::wstring& path, uint32_t W, uint32_t H) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* f = nullptr;
  CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f));
  IWICBitmapDecoder* dec = nullptr;
  CHECK(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec));
  IWICBitmapFrameDecode* fr = nullptr;
  CHECK(dec->GetFrame(0, &fr));
  IWICBitmapScaler* sc = nullptr;
  CHECK(f->CreateBitmapScaler(&sc));
  CHECK(sc->Initialize(fr, W, H, WICBitmapInterpolationModeFant));
  IWICFormatConverter* cv = nullptr;
  CHECK(f->CreateFormatConverter(&cv));
  CHECK(cv->Initialize(sc, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
  std::vector<uint8_t> px(size_t(W) * H * 4);
  CHECK(cv->CopyPixels(nullptr, W * 4, UINT(px.size()), px.data()));
  std::vector<float> rgb(size_t(W) * H * 3);
  for (size_t i = 0; i < size_t(W) * H; ++i) for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = px[i * 4 + c] / 255.f;
  return rgb;
}

float Dec1(float c) { c = std::max(c, 0.f); return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float Enc1(float c) { c = std::max(c, 0.f); return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f; }

int Drs(Gpu& g, Ngx& ngx, const std::wstring& png, uint32_t w, uint32_t h, int dx, int frames) {
  const uint32_t W = w + uint32_t(dx * frames), H = h;
  const std::vector<float> src = LoadStill(png, W, H);
  const size_t px = size_t(w) * h;
  // Frame i at full size (sRGB-encoded, as Core's proxy), and its luminance in linear light.
  auto full = [&](int i, int x, int y, int c) { return src[(size_t(y) * W + x + i * dx) * 3 + c]; };
  auto lumIn = [&](int i, int x, int y) { return 0.2126f * Dec1(full(i, x, y, 0)) + 0.7152f * Dec1(full(i, x, y, 1)) + 0.0722f * Dec1(full(i, x, y, 2)); };
  struct Run { const char* name; bool ownFeature; std::function<float(int)> scale; };
  const std::vector<Run> runs = {
      {"native                      ", false, [](int) { return 1.f; }},
      {"active 75%                  ", false, [](int) { return 0.75f; }},
      {"own feature 75% (NetScale)  ", true, [](int) { return 0.75f; }},
      {"active 75%/62.5% each frame ", false, [](int i) { return i % 2 ? 0.625f : 0.75f; }},
      {"active 75%/62.5% every 8    ", false, [](int i) { return (i / 8) % 2 ? 0.625f : 0.75f; }},
      {"active 62.5%                ", false, [](int) { return 0.625f; }},
      {"active 50%                  ", false, [](int) { return 0.5f; }},
  };
  std::vector<std::vector<float>> ref(static_cast<size_t>(frames));
  Timer t;
  t.Init(g);
  const int warm = 8;
  std::printf("%dx%d, pan %d px/frame, %d frames (first %d not scored)\n", w, h, dx, frames, warm);
  std::printf("run                           ms      error   flicker  strength\n");
  for (const Run& run : runs) {
    const float s0 = run.scale(0);
    const uint32_t fw = run.ownFeature ? uint32_t(w * s0 + 0.5f) : w, fh = run.ownFeature ? uint32_t(h * s0 + 0.5f) : h;
    Feature f;
    CreateFeature(g, ngx, f, fw, fh);
    std::vector<float> prev;
    double msSum = 0, errSum = 0, flkSum = 0, magSum = 0;
    int scored = 0;
    for (int i = 0; i < frames; ++i) {
      const float s = run.scale(i);
      const uint32_t aw = run.ownFeature ? fw : uint32_t(w * s + 0.5f), ah = run.ownFeature ? fh : uint32_t(h * s + 0.5f);
      // The input at the active size (bilinear at the footprint centre in linear light, like Core's Down).
      std::vector<uint16_t> ch(size_t(fw) * fh * 4, 0), mh(size_t(fw) * fh * 2, 0);
      for (uint32_t y = 0; y < ah; ++y)
        for (uint32_t x = 0; x < aw; ++x) {
          const float fx = std::clamp((x + 0.5f) * w / aw - 0.5f, 0.f, float(w - 1)), fy = std::clamp((y + 0.5f) * h / ah - 0.5f, 0.f, float(h - 1));
          const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, int(w) - 1), y1 = std::min(y0 + 1, int(h) - 1);
          const float ax = fx - x0, ay = fy - y0;
          for (int c = 0; c < 3; ++c) {
            const float v = Dec1(full(i, x0, y0, c)) * (1 - ax) * (1 - ay) + Dec1(full(i, x1, y0, c)) * ax * (1 - ay) + Dec1(full(i, x0, y1, c)) * (1 - ax) * ay + Dec1(full(i, x1, y1, c)) * ax * ay;
            ch[(size_t(y) * fw + x) * 4 + c] = ToHalf(Enc1(v));
          }
          ch[(size_t(y) * fw + x) * 4 + 3] = ToHalf(1.f);
          mh[(size_t(y) * fw + x) * 2 + 0] = ToHalf(float(dx) * aw / w);  // to the previous frame, in active pixels
        }
      UploadTexture(g, f.color, ch.data(), fw, fh, 8, kSrv, kSrv);
      UploadTexture(g, f.motion, mh.data(), fw, fh, 4, kSrv, kSrv);
      activeScale = run.ownFeature ? 1.f : float(aw) / float(w);
      const double ms = TimedEvaluate(g, ngx, f, t);
      activeScale = -1.f;
      const std::vector<uint16_t> out = ReadTexture(g, f.output, fw, fh, kUav);
      // Back to full size and the edit.
      std::vector<float> e(px);
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const float fx = std::clamp((x + 0.5f) * aw / w - 0.5f, 0.f, float(aw - 1)), fy = std::clamp((y + 0.5f) * ah / h - 0.5f, 0.f, float(ah - 1));
          const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, int(aw) - 1), y1 = std::min(y0 + 1, int(ah) - 1);
          const float ax = fx - x0, ay = fy - y0;
          float lo = 0, li = 0;
          for (int c = 0; c < 3; ++c) {
            auto at = [&](int xx, int yy) { return Dec1(FromHalf(out[(size_t(yy) * fw + xx) * 4 + c])); };
            const float v = at(x0, y0) * (1 - ax) * (1 - ay) + at(x1, y0) * ax * (1 - ay) + at(x0, y1) * (1 - ax) * ay + at(x1, y1) * ax * ay;
            lo += v * (c == 0 ? 0.2126f : c == 1 ? 0.7152f : 0.0722f);
          }
          li = lumIn(i, x, y);
          e[size_t(y) * w + x] = std::clamp(std::log2(std::max(lo, 1.f / 4096) / std::max(li, 1.f / 4096)), -3.f, 3.f);
        }
      if (&run == &runs[0]) ref[size_t(i)] = e;
      if (i >= warm) {
        double er = 0, fl = 0, mg = 0;
        size_t nf = 0;
        for (uint32_t y = 0; y < h; ++y)
          for (uint32_t x = 0; x < w; ++x) {
            const float v = e[size_t(y) * w + x];
            er += std::fabs(v - ref[size_t(i)][size_t(y) * w + x]);
            mg += std::fabs(v);
            // This frame's pixel x shows what the previous frame showed at x + dx.
            if (x + dx < w && !prev.empty()) { fl += std::fabs(v - prev[size_t(y) * w + x + dx]); ++nf; }
          }
        errSum += er / px; magSum += mg / px; flkSum += nf ? fl / nf : 0; msSum += ms;
        ++scored;
      }
      prev = std::move(e);
    }
    std::printf("%s %6.3f  %8.4f  %8.4f  %8.4f\n", run.name, msSum / scored, errSum / scored, flkSum / scored, magSum / scored);
    ngx.release(f.handle);
    for (auto* r : {f.color, f.output, f.motion, f.depth}) r->Release();
  }
  return 0;
}


// ------------------------------------------------------------------ edit upsampling
// The network at a reduced size, its edit brought back to full size by different upsamplers,
// each compared with the native network's edit on the same panned still: error, error after the
// best single gain (what intensity can fix is left out: the rest is structure), and flicker.
//   bilinear   the edit's own bilinear upsample
//   jbu        Core's joint bilateral upsample (4x4 taps, luma-guided, src/pipeline.cpp UpEdit)
//   gf r,eps   fast guided filter (He & Sun 2015): per-window linear fit of the edit against log
//              luma at low size, the fit's coefficients upsampled and applied to full-size log luma
struct Plane { int w = 0, h = 0; std::vector<float> v; float& at(int x, int y) { return v[size_t(y) * w + x]; } float get(int x, int y) const { return v[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; } };
Plane MakePlane(int w, int h) { Plane p; p.w = w; p.h = h; p.v.assign(size_t(w) * h, 0.f); return p; }
float Bilinear(const Plane& p, float fx, float fy) {
  fx = std::clamp(fx, 0.f, float(p.w - 1)); fy = std::clamp(fy, 0.f, float(p.h - 1));
  const int x0 = int(fx), y0 = int(fy);
  const float ax = fx - x0, ay = fy - y0;
  return p.get(x0, y0) * (1 - ax) * (1 - ay) + p.get(x0 + 1, y0) * ax * (1 - ay) + p.get(x0, y0 + 1) * (1 - ax) * ay + p.get(x0 + 1, y0 + 1) * ax * ay;
}
Plane Box(const Plane& in, int r) {  // mean over a (2r+1)^2 window, clamped at the edges
  Plane t = MakePlane(in.w, in.h), o = MakePlane(in.w, in.h);
  for (int y = 0; y < in.h; ++y) for (int x = 0; x < in.w; ++x) { float a = 0; for (int k = -r; k <= r; ++k) a += in.get(x + k, y); t.at(x, y) = a / (2 * r + 1); }
  for (int y = 0; y < in.h; ++y) for (int x = 0; x < in.w; ++x) { float a = 0; for (int k = -r; k <= r; ++k) a += t.get(x, y + k); o.at(x, y) = a / (2 * r + 1); }
  return o;
}

int Ups(Gpu& g, Ngx& ngx, const std::wstring& png, uint32_t w, uint32_t h, int dx, int frames) {
  const uint32_t W = w + uint32_t(dx * frames), H = h;
  const std::vector<float> src = LoadStill(png, W, H);
  auto lin = [&](int i, int x, int y, int c) { return Dec1(src[(size_t(y) * W + x + i * dx) * 3 + c]); };
  const float kw[3] = {0.2126f, 0.7152f, 0.0722f};
  const int warm = 6;
  const float scales[] = {1.f, 0.75f, 0.625f, 0.5f};
  std::vector<Plane> ref(static_cast<size_t>(frames));
  Timer t;
  t.Init(g);
  struct Method { const char* name; int kind; int r; float eps; };
  const Method methods[] = {{"bilinear     ", 0, 0, 0}, {"jbu (Core)   ", 1, 0, 0}, {"gf r2 e.010  ", 2, 2, 0.010f}, {"gf r2 e.003  ", 2, 2, 0.003f}, {"gf r4 e.010  ", 2, 4, 0.010f}};
  constexpr int kM = 5;
  std::printf("%dx%d, pan %d px/frame, %d frames (first %d not scored); edit = log2 luminance ratio\n", w, h, dx, frames, warm);
  std::printf("size   method          error  gain-fixed  flicker  strength\n");
  for (float s : scales) {
    const uint32_t aw = uint32_t(w * s + 0.5f), ah = uint32_t(h * s + 0.5f);
    Feature f;
    CreateFeature(g, ngx, f, aw, ah);
    double err[kM] = {}, errK[kM] = {}, flk[kM] = {}, mag[kM] = {};
    std::vector<Plane> prev(kM);
    int scored = 0;
    for (int i = 0; i < frames; ++i) {
      // Low-size input (bilinear at the footprint centre in linear light) and its upload.
      std::vector<float> inLow(size_t(aw) * ah * 3);
      std::vector<uint16_t> ch(size_t(aw) * ah * 4), mh(size_t(aw) * ah * 2, 0);
      for (uint32_t y = 0; y < ah; ++y)
        for (uint32_t x = 0; x < aw; ++x) {
          const float fx = std::clamp((x + 0.5f) * w / aw - 0.5f, 0.f, float(w - 1)), fy = std::clamp((y + 0.5f) * h / ah - 0.5f, 0.f, float(h - 1));
          const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, int(w) - 1), y1 = std::min(y0 + 1, int(h) - 1);
          const float ax = fx - x0, ay = fy - y0;
          for (int c = 0; c < 3; ++c) {
            const float v = lin(i, x0, y0, c) * (1 - ax) * (1 - ay) + lin(i, x1, y0, c) * ax * (1 - ay) + lin(i, x0, y1, c) * (1 - ax) * ay + lin(i, x1, y1, c) * ax * ay;
            inLow[(size_t(y) * aw + x) * 3 + c] = v;
            ch[(size_t(y) * aw + x) * 4 + c] = ToHalf(Enc1(v));
          }
          ch[(size_t(y) * aw + x) * 4 + 3] = ToHalf(1.f);
          mh[(size_t(y) * aw + x) * 2] = ToHalf(float(dx) * aw / w);
        }
      UploadTexture(g, f.color, ch.data(), aw, ah, 8, kSrv, kSrv);
      UploadTexture(g, f.motion, mh.data(), aw, ah, 4, kSrv, kSrv);
      TimedEvaluate(g, ngx, f, t);
      const std::vector<uint16_t> out = ReadTexture(g, f.output, aw, ah, kUav);
      // The low-size edit per channel (as Core takes it) and the low-size log luma (the guide).
      Plane el[3] = {MakePlane(aw, ah), MakePlane(aw, ah), MakePlane(aw, ah)}, gl = MakePlane(aw, ah);
      for (uint32_t y = 0; y < ah; ++y)
        for (uint32_t x = 0; x < aw; ++x) {
          float yl = 0;
          for (int c = 0; c < 3; ++c) {
            const float pi = inLow[(size_t(y) * aw + x) * 3 + c], po = Dec1(FromHalf(out[(size_t(y) * aw + x) * 4 + c]));
            el[c].at(x, y) = std::clamp(std::log2(std::max(po, 1.f / 512) / std::max(pi, 1.f / 512)), -3.f, 3.f);
            yl += kw[c] * pi;
          }
          gl.at(x, y) = std::log2(std::max(yl, 1e-4f));
        }
      // Full-size guide.
      Plane gf = MakePlane(w, h), lumIn = MakePlane(w, h);
      for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) { float yl = 0; for (int c = 0; c < 3; ++c) yl += kw[c] * lin(i, x, y, c); lumIn.at(x, y) = yl; gf.at(x, y) = std::log2(std::max(yl, 1e-4f)); }
      auto lumEdit = [&](int x, int y, const float e3[3]) {
        float lo = 0;
        for (int c = 0; c < 3; ++c) lo += kw[c] * lin(i, x, y, c) * std::exp2(e3[c]);
        return std::clamp(std::log2(std::max(lo, 1.f / 4096) / std::max(lumIn.at(x, y), 1.f / 4096)), -3.f, 3.f);
      };
      if (s == 1.f) {  // native: the reference
        Plane e = MakePlane(w, h);
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) { const float e3[3] = {el[0].at(x, y), el[1].at(x, y), el[2].at(x, y)}; e.at(x, y) = lumEdit(x, y, e3); }
        ref[size_t(i)] = std::move(e);
        continue;
      }
      // Guided filter coefficients per method (low size), once per frame.
      Plane ga[kM][3], gb[kM][3];
      for (int m = 0; m < kM; ++m) {
        if (methods[m].kind != 2) continue;
        const int rr = methods[m].r;
        Plane mI = Box(gl, rr), II = MakePlane(aw, ah);
        for (size_t k = 0; k < II.v.size(); ++k) II.v[k] = gl.v[k] * gl.v[k];
        Plane mII = Box(II, rr);
        for (int c = 0; c < 3; ++c) {
          Plane IP = MakePlane(aw, ah);
          for (size_t k = 0; k < IP.v.size(); ++k) IP.v[k] = gl.v[k] * el[c].v[k];
          Plane mP = Box(el[c], rr), mIP = Box(IP, rr), a = MakePlane(aw, ah), b = MakePlane(aw, ah);
          for (size_t k = 0; k < a.v.size(); ++k) {
            const float var = mII.v[k] - mI.v[k] * mI.v[k], cov = mIP.v[k] - mI.v[k] * mP.v[k];
            a.v[k] = cov / (var + methods[m].eps);
            b.v[k] = mP.v[k] - a.v[k] * mI.v[k];
          }
          ga[m][c] = Box(a, rr); gb[m][c] = Box(b, rr);
        }
      }
      for (int m = 0; m < kM; ++m) {
        Plane e = MakePlane(w, h);
        for (uint32_t y = 0; y < h; ++y)
          for (uint32_t x = 0; x < w; ++x) {
            const float fx = (x + 0.5f) * aw / w - 0.5f, fy = (y + 0.5f) * ah / h - 0.5f;
            float e3[3];
            if (methods[m].kind == 0) {
              for (int c = 0; c < 3; ++c) e3[c] = Bilinear(el[c], fx, fy);
            } else if (methods[m].kind == 1) {
              const int bx = int(std::floor(fx)), by = int(std::floor(fy));
              float acc[3] = {}, ws = 0, bil[3] = {};
              for (int j = -1; j <= 2; ++j)
                for (int k = -1; k <= 2; ++k) {
                  const int qx = std::clamp(bx + k, 0, int(aw) - 1), qy = std::clamp(by + j, 0, int(ah) - 1);
                  const float ddx = fx - float(bx + k), ddy = fy - float(by + j);
                  const float rr = (gf.at(x, y) - gl.get(qx, qy)) / 0.15f;
                  const float wt = std::exp(-(ddx * ddx + ddy * ddy) / 1.28f) * std::exp(-0.5f * rr * rr);
                  for (int c = 0; c < 3; ++c) acc[c] += el[c].get(qx, qy) * wt;
                  ws += wt;
                  if (k >= 0 && k <= 1 && j >= 0 && j <= 1) for (int c = 0; c < 3; ++c) bil[c] += el[c].get(qx, qy) * ((1 - std::fabs(ddx)) * (1 - std::fabs(ddy)));
                }
              for (int c = 0; c < 3; ++c) e3[c] = ws > 1e-4f ? acc[c] / ws : bil[c];
            } else {
              for (int c = 0; c < 3; ++c) e3[c] = Bilinear(ga[m][c], fx, fy) * gf.at(x, y) + Bilinear(gb[m][c], fx, fy);
            }
            for (int c = 0; c < 3; ++c) e3[c] = std::clamp(e3[c], -3.f, 3.f);
            e.at(x, y) = lumEdit(x, y, e3);
          }
        if (i >= warm) {
          double er = 0, mg = 0, se = 0, ee = 0, fl = 0;
          size_t nf = 0;
          const Plane& rf = ref[size_t(i)];
          for (size_t k = 0; k < e.v.size(); ++k) { er += std::fabs(e.v[k] - rf.v[k]); mg += std::fabs(e.v[k]); se += double(e.v[k]) * rf.v[k]; ee += double(e.v[k]) * e.v[k]; }
          const double gain = ee > 0 ? se / ee : 1.0;
          double ek = 0;
          for (size_t k = 0; k < e.v.size(); ++k) ek += std::fabs(gain * e.v[k] - rf.v[k]);
          if (!prev[size_t(m)].v.empty())
            for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x + dx < w; ++x) { fl += std::fabs(e.at(x, y) - prev[size_t(m)].at(x + dx, y)); ++nf; }
          const double n = double(e.v.size());
          err[m] += er / n; errK[m] += ek / n; mag[m] += mg / n; flk[m] += nf ? fl / nf : 0;
        }
        prev[size_t(m)] = std::move(e);
      }
      if (i >= warm) ++scored;
    }
    if (s < 1.f)
      for (int m = 0; m < kM; ++m)
        std::printf("%4.1f%%  %s  %7.4f   %7.4f   %7.4f  %7.4f\n", s * 100, methods[m].name, err[m] / scored, errK[m] / scored, flk[m] / scored, mag[m] / scored);
    ngx.release(f.handle);
    for (auto* rs : {f.color, f.output, f.motion, f.depth}) rs->Release();
  }
  // The native reference's own flicker and strength, for scale.
  double fl = 0, mg = 0; size_t nf = 0;
  for (int i = warm + 1; i < frames; ++i) for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x + dx < w; ++x) { fl += std::fabs(ref[size_t(i)].at(x, y) - ref[size_t(i - 1)].at(x + dx, y)); mg += std::fabs(ref[size_t(i)].at(x, y)); ++nf; }
  std::printf("native reference: flicker %.4f, strength %.4f\n", fl / nf, mg / nf);
  return 0;
}



// ------------------------------------------------------------------ what the second pass adds
// Two passes on a panned still, as Core runs them (pass 2 on pass 1's output, a feature each). How
// big is pass 1's edit, how big is pass 2's increment on top of it, how much of that increment is
// fine detail (what a 9x9 box blur takes away), and what does running pass 2 at a reduced size
// (its increment brought back with a bilinear upsample) or every other frame (last frame's
// increment, shifted by the pan, reused) cost against the two full passes? Errors are on the total
// edit, log2 luminance.
int Pass2(Gpu& g, Ngx& ngx, const std::wstring& png, uint32_t w, uint32_t h, int dx, int frames) {
  const uint32_t W = w + uint32_t(dx * frames), H = h;
  const std::vector<float> src = LoadStill(png, W, H);
  auto lin = [&](int i, int x, int y, int c) { return Dec1(src[(size_t(y) * W + x + i * dx) * 3 + c]); };
  const float kw[3] = {0.2126f, 0.7152f, 0.0722f};
  const size_t px = size_t(w) * h;
  const int warm = 6;
  const uint32_t lw = w / 2, lh = h / 2;
  Feature f1, f2, f2l;
  CreateFeature(g, ngx, f1, w, h);
  CreateFeature(g, ngx, f2, w, h);
  CreateFeature(g, ngx, f2l, lw, lh);
  Timer t;
  t.Init(g);
  auto lum = [&](const std::vector<float>& rgb, size_t q) { return kw[0] * rgb[q * 3] + kw[1] * rgb[q * 3 + 1] + kw[2] * rgb[q * 3 + 2]; };
  double m1 = 0, m2 = 0, hf = 0, eHalf = 0, eSkip = 0, eOne = 0;
  int scored = 0;
  std::vector<float> prevInc;  // last frame's pass-2 increment, log2 luminance
  for (int i = 0; i < frames; ++i) {
    std::vector<uint16_t> in(px * 4), mv(px * 2, 0);
    std::vector<float> inL(px * 3);
    for (size_t q = 0; q < px; ++q) {
      const int x = int(q % w), y = int(q / w);
      for (int c = 0; c < 3; ++c) { inL[q * 3 + c] = lin(i, x, y, c); in[q * 4 + c] = ToHalf(Enc1(inL[q * 3 + c])); }
      in[q * 4 + 3] = ToHalf(1.f);
      mv[q * 2] = ToHalf(float(dx));
    }
    UploadTexture(g, f1.color, in.data(), w, h, 8, kSrv, kSrv);
    UploadTexture(g, f1.motion, mv.data(), w, h, 4, kSrv, kSrv);
    TimedEvaluate(g, ngx, f1, t);
    const std::vector<uint16_t> o1 = ReadTexture(g, f1.output, w, h, kUav);
    // Pass 2 at full size on pass 1's output.
    UploadTexture(g, f2.color, o1.data(), w, h, 8, kSrv, kSrv);
    UploadTexture(g, f2.motion, mv.data(), w, h, 4, kSrv, kSrv);
    TimedEvaluate(g, ngx, f2, t);
    const std::vector<uint16_t> o2 = ReadTexture(g, f2.output, w, h, kUav);
    // Pass 2 at half size on pass 1's output (2x2 mean in linear light).
    std::vector<uint16_t> lowIn(size_t(lw) * lh * 4), lowMv(size_t(lw) * lh * 2, 0);
    std::vector<float> lowL(size_t(lw) * lh * 3);
    for (uint32_t y = 0; y < lh; ++y)
      for (uint32_t x = 0; x < lw; ++x) {
        for (int c = 0; c < 3; ++c) {
          float a = 0;
          for (int j = 0; j < 2; ++j) for (int k = 0; k < 2; ++k) a += Dec1(FromHalf(o1[((size_t(2 * y + j)) * w + 2 * x + k) * 4 + c]));
          lowL[(size_t(y) * lw + x) * 3 + c] = a / 4;
          lowIn[(size_t(y) * lw + x) * 4 + c] = ToHalf(Enc1(a / 4));
        }
        lowIn[(size_t(y) * lw + x) * 4 + 3] = ToHalf(1.f);
        lowMv[(size_t(y) * lw + x) * 2] = ToHalf(float(dx) / 2);
      }
    UploadTexture(g, f2l.color, lowIn.data(), lw, lh, 8, kSrv, kSrv);
    UploadTexture(g, f2l.motion, lowMv.data(), lw, lh, 4, kSrv, kSrv);
    TimedEvaluate(g, ngx, f2l, t);
    const std::vector<uint16_t> o2l = ReadTexture(g, f2l.output, lw, lh, kUav);
    // Edits in log2 luminance.
    std::vector<float> p1(px * 3), p2(px * 3);
    for (size_t q = 0; q < px * 3; ++q) { p1[q] = Dec1(FromHalf(o1[(q / 3) * 4 + q % 3])); p2[q] = Dec1(FromHalf(o2[(q / 3) * 4 + q % 3])); }
    std::vector<float> e1(px), inc(px), incHalf(px), lowInc(size_t(lw) * lh);
    for (size_t q = 0; q < size_t(lw) * lh; ++q) {
      float a = 0, b = 0;
      for (int c = 0; c < 3; ++c) { a += kw[c] * Dec1(FromHalf(o2l[q * 4 + c])); b += kw[c] * lowL[q * 3 + c]; }
      lowInc[q] = std::clamp(std::log2(std::max(a, 1.f / 4096) / std::max(b, 1.f / 4096)), -3.f, 3.f);
    }
    for (size_t q = 0; q < px; ++q) {
      const float li = lum(inL, q), l1 = lum(p1, q), l2 = lum(p2, q);
      e1[q] = std::clamp(std::log2(std::max(l1, 1.f / 4096) / std::max(li, 1.f / 4096)), -3.f, 3.f);
      inc[q] = std::clamp(std::log2(std::max(l2, 1.f / 4096) / std::max(l1, 1.f / 4096)), -3.f, 3.f);
      const int x = int(q % w), y = int(q / w);
      const float fx = std::clamp((x + 0.5f) / 2 - 0.5f, 0.f, float(lw - 1)), fy = std::clamp((y + 0.5f) / 2 - 0.5f, 0.f, float(lh - 1));
      const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, int(lw) - 1), y1 = std::min(y0 + 1, int(lh) - 1);
      const float ax = fx - x0, ay = fy - y0;
      incHalf[q] = lowInc[size_t(y0) * lw + x0] * (1 - ax) * (1 - ay) + lowInc[size_t(y0) * lw + x1] * ax * (1 - ay) + lowInc[size_t(y1) * lw + x0] * (1 - ax) * ay + lowInc[size_t(y1) * lw + x1] * ax * ay;
    }
    if (i >= warm) {
      // Fine detail in the increment: what a 9x9 box blur removes.
      double a1 = 0, a2 = 0, ah = 0, eh = 0, es = 0, eo = 0;
      std::vector<float> rowBlur(px);
      for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) { float a = 0; for (int k = -4; k <= 4; ++k) a += inc[size_t(y) * w + std::clamp(int(x) + k, 0, int(w) - 1)]; rowBlur[size_t(y) * w + x] = a / 9; }
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const size_t q = size_t(y) * w + x;
          float b = 0; for (int k = -4; k <= 4; ++k) b += rowBlur[size_t(std::clamp(int(y) + k, 0, int(h) - 1)) * w + x];
          b /= 9;
          a1 += std::fabs(e1[q]); a2 += std::fabs(inc[q]); ah += std::fabs(inc[q] - b);
          eh += std::fabs(incHalf[q] - inc[q]);   // total edit e1 + incHalf vs e1 + inc
          eo += std::fabs(inc[q]);                // pass 1 alone
          if (!prevInc.empty()) es += std::fabs((x + dx < w ? prevInc[q + dx] : inc[q]) - inc[q]);
        }
      m1 += a1 / px; m2 += a2 / px; hf += ah / px; eHalf += eh / px; eSkip += es / px; eOne += eo / px;
      ++scored;
    }
    prevInc = inc;
  }
  std::printf("pass 1 edit %.4f | pass 2 increment %.4f (%.0f%% of pass 1), fine detail in it %.4f (%.0f%%)\n", m1 / scored, m2 / scored, 100 * m2 / m1, hf / scored, 100 * hf / m2);
  std::printf("error of the total edit against two full passes: pass 1 only %.4f | pass 2 at half size %.4f | pass 2 every other frame (shifted) %.4f\n", eOne / scored, eHalf / scored, eSkip / scored);
  return 0;
}

// ------------------------------------------------------------------ the runtime's own scaled mode
// The colour input at a reduced size (top-left subrect), the output at full size, the full-size
// frame as the backbuffer: does the runtime bring its result up to full size itself (its guided
// high-source path), and how close is that to the native network? Reports the share of the output
// it wrote beyond the reduced area, error and flicker against native, and strength.
int Nvs(Gpu& g, Ngx& ngx, const std::wstring& png, uint32_t w, uint32_t h, int dx, int frames, float k) {
  const uint32_t W = w + uint32_t(dx * frames), H = h;
  const std::vector<float> src = LoadStill(png, W, H);
  auto lin = [&](int i, int x, int y, int c) { return Dec1(src[(size_t(y) * W + x + i * dx) * 3 + c]); };
  const float kw[3] = {0.2126f, 0.7152f, 0.0722f};
  const int warm = 6;
  const size_t px = size_t(w) * h;
  std::vector<std::vector<float>> ref(static_cast<size_t>(frames));
  ID3D12Resource* bb = Texture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, kSrv);
  Timer t;
  t.Init(g);
  for (int pass = 0; pass < 2; ++pass) {
    const bool scaled = pass == 1;
    const uint32_t aw = scaled ? uint32_t(w * k) : w, ah = scaled ? uint32_t(h * k) : h;
    Feature f;
    CreateFeature(g, ngx, f, w, h);
    std::vector<float> prev;
    double err = 0, flk = 0, mag = 0, cover = 0, ms = 0;
    int scored = 0;
    for (int i = 0; i < frames; ++i) {
      std::vector<uint16_t> full(px * 4), ch(px * 4, 0), mh(px * 2, 0);
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) { for (int c = 0; c < 3; ++c) full[(size_t(y) * w + x) * 4 + c] = ToHalf(Enc1(lin(i, x, y, c))); full[(size_t(y) * w + x) * 4 + 3] = ToHalf(1.f); }
      for (uint32_t y = 0; y < ah; ++y)
        for (uint32_t x = 0; x < aw; ++x) {
          const float fx = std::clamp((x + 0.5f) * w / aw - 0.5f, 0.f, float(w - 1)), fy = std::clamp((y + 0.5f) * h / ah - 0.5f, 0.f, float(h - 1));
          const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, int(w) - 1), y1 = std::min(y0 + 1, int(h) - 1);
          const float ax = fx - x0, ay = fy - y0;
          for (int c = 0; c < 3; ++c) {
            const float v = lin(i, x0, y0, c) * (1 - ax) * (1 - ay) + lin(i, x1, y0, c) * ax * (1 - ay) + lin(i, x0, y1, c) * (1 - ax) * ay + lin(i, x1, y1, c) * ax * ay;
            ch[(size_t(y) * w + x) * 4 + c] = ToHalf(Enc1(v));
          }
          ch[(size_t(y) * w + x) * 4 + 3] = ToHalf(1.f);
          mh[(size_t(y) * w + x) * 2] = ToHalf(float(dx) * aw / w);
        }
      UploadTexture(g, f.color, scaled ? ch.data() : full.data(), w, h, 8, kSrv, kSrv);
      UploadTexture(g, f.motion, mh.data(), w, h, 4, kSrv, kSrv);
      UploadTexture(g, bb, full.data(), w, h, 8, kSrv, kSrv);
      // A marker in the output: whatever the network does not write stays at -1.
      std::vector<uint16_t> mark(px * 4, ToHalf(-1.f));
      UploadTexture(g, f.output, mark.data(), w, h, 8, kUav, kUav);
      colorScale = scaled ? k : -1.f;
      probeBackbuffer = scaled ? bb : nullptr;
      const double m = TimedEvaluate(g, ngx, f, t);
      colorScale = -1.f; probeBackbuffer = nullptr;
      const std::vector<uint16_t> out = ReadTexture(g, f.output, w, h, kUav);
      std::vector<float> e(px);
      size_t written = 0, beyond = 0;
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const size_t q = size_t(y) * w + x;
          const bool wrote = FromHalf(out[q * 4]) > -0.5f;
          if (x >= aw || y >= ah) { ++beyond; written += wrote; }
          float lo = 0, li = 0;
          for (int c = 0; c < 3; ++c) { lo += kw[c] * Dec1(std::max(FromHalf(out[q * 4 + c]), 0.f)); li += kw[c] * lin(i, x, y, c); }
          e[q] = std::clamp(std::log2(std::max(lo, 1.f / 4096) / std::max(li, 1.f / 4096)), -3.f, 3.f);
        }
      if (!scaled) ref[size_t(i)] = e;
      if (i >= warm) {
        double er = 0, mg = 0, fl = 0; size_t nf = 0;
        for (size_t q = 0; q < px; ++q) { er += std::fabs(e[q] - ref[size_t(i)][q]); mg += std::fabs(e[q]); }
        if (!prev.empty()) for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x + dx < w; ++x) { fl += std::fabs(e[size_t(y) * w + x] - prev[size_t(y) * w + x + dx]); ++nf; }
        err += er / px; mag += mg / px; flk += nf ? fl / nf : 0; cover += beyond ? double(written) / beyond : 1.0; ms += m;
        ++scored;
      }
      prev = std::move(e);
    }
    std::printf("%s  %.3f ms  wrote beyond the input area %5.1f%%  error %.4f  flicker %.4f  strength %.4f\n", scaled ? "runtime-scaled" : "native        ", ms / scored, 100 * cover / scored, err / scored, flk / scored, mag / scored);
    ngx.release(f.handle);
    for (auto* rs : {f.color, f.output, f.motion, f.depth}) rs->Release();
  }
  return 0;
}

// PRISM_NGXLOG=1 (verbose: 2): the runtime's own log on stderr, through NVSDK_NGX_FeatureCommonInfo.
void NgxLog(const char* msg, int level, int component) {
  std::fprintf(stderr, "[ngx %d/%d] %s", level, component, msg ? msg : "");
  if (msg && *msg && msg[std::strlen(msg) - 1] != 10) std::fputc(10, stderr);
}
struct NgxLoggingInfo { void (*callback)(const char*, int, int); int minimumLevel; bool disableOtherSinks; };
struct NgxPathListInfo { const wchar_t* const* path; unsigned length; };
struct NgxFeatureCommonInfo { NgxPathListInfo paths; void* internal; NgxLoggingInfo logging; };

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: prism_nvngx.dll.exe <nvngx_dlssnr.dll> cost | seq <dir> | async <w> <h> | drs <still.png> <w> <h> <pan px> <frames>\n");
    return 2;
  }
  Gpu g;
  InitGpu(g);
  const std::string dll = argv[1];
  Ngx ngx = LoadNgx(std::wstring(dll.begin(), dll.end()));
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dataPath(exe);
  dataPath = dataPath.substr(0, dataPath.find_last_of(L"\\/"));
  NgxFeatureCommonInfo info = {{nullptr, 0}, nullptr, {&NgxLog, std::getenv("PRISM_NGXLOG") ? std::atoi(std::getenv("PRISM_NGXLOG")) : 0, false}};
  const NgxResult r = ngx.init(kApplicationId, dataPath.c_str(), g.device, kNgxVersionApi, std::getenv("PRISM_NGXLOG") ? &info : nullptr);
  if (r != kNgxSuccess) { std::fprintf(stderr, "NGX Init failed: 0x%08x\n", r); return 1; }
  CheckSlots(ngx);
  const std::string mode = argv[2];
  int rc = 2;
  if (mode == "cost") rc = Cost(g, ngx);
  else if (mode == "seq" && argc >= 4) rc = Sequence(g, ngx, argv[3]);
  else if (mode == "async" && argc >= 5) rc = Async(g, ngx, uint32_t(std::atoi(argv[3])), uint32_t(std::atoi(argv[4])));
  else if (mode == "pass2" && argc >= 8) {
    const std::string still = argv[3];
    rc = Pass2(g, ngx, std::wstring(still.begin(), still.end()), uint32_t(std::atoi(argv[4])), uint32_t(std::atoi(argv[5])), std::atoi(argv[6]), std::atoi(argv[7]));
  } else if (mode == "nvs" && argc >= 9) {
    const std::string still = argv[3];
    rc = Nvs(g, ngx, std::wstring(still.begin(), still.end()), uint32_t(std::atoi(argv[4])), uint32_t(std::atoi(argv[5])), std::atoi(argv[6]), std::atoi(argv[7]), float(std::atof(argv[8])));
  } else if (mode == "ups" && argc >= 8) {
    const std::string still = argv[3];
    rc = Ups(g, ngx, std::wstring(still.begin(), still.end()), uint32_t(std::atoi(argv[4])), uint32_t(std::atoi(argv[5])), std::atoi(argv[6]), std::atoi(argv[7]));
  } else if (mode == "drs" && argc >= 8) {
    const std::string still = argv[3];
    rc = Drs(g, ngx, std::wstring(still.begin(), still.end()), uint32_t(std::atoi(argv[4])), uint32_t(std::atoi(argv[5])), std::atoi(argv[6]), std::atoi(argv[7]));
  }
  if (ngx.shutdown) ngx.shutdown(g.device);
  return rc;
}

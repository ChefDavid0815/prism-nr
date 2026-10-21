// NR bench: drives NVIDIA's signed DLSS Neural Rendering runtime (nvngx_dlssnr.dll, NGX
// feature 18) directly on an RTX GPU, outside any game, on frames we provide.
//
//   prism_nvngx.dll.exe <nvngx_dlssnr.dll> cost
//       network time at 720p, 1080p, 1440p, 1600p and 4K
//   prism_nvngx.dll.exe <nvngx_dlssnr.dll> seq <dir>
//       runs a frame sequence: <dir>/meta.txt holds "width height count"; each frame i has
//       c_i.f32 (RGBA float, display-encoded 0..1) and m_i.f32 (RG float, previous position
//       minus current, pixels); writes o_i.f32 (RGBA float) and times.txt.
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
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
NgxResult ScalingRatioCallback(void* parameters) {
  SetF(parameters, "DLSSNR.ScalingRatio", 1.f);
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
};

const auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

void CreateFeature(Gpu& g, Ngx& ngx, Feature& f, uint32_t w, uint32_t h) {
  f.w = w;
  f.h = h;
  if (ngx.allocate(&f.params) != kNgxSuccess || !f.params) { std::fprintf(stderr, "AllocateParameters failed\n"); std::exit(1); }
  void* p = f.params;
  SetUI(p, "Width", w);
  SetUI(p, "Height", h);
  SetUI(p, "OutWidth", w);
  SetUI(p, "OutHeight", h);
  for (const char* k : {"DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth", "DLSSNR.Output.Width"}) SetUI(p, k, w);
  for (const char* k : {"DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight", "DLSSNR.Output.Height"}) SetUI(p, k, h);
  SetF(p, "DLSSNR.ScalingRatio", 1.f);
  SetF(p, "DLSSNR.Scale", 1.f);
  SetPtr(p, "DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&ScalingRatioCallback));
  SetI(p, "PerfQualityValue", 0);
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

void RecordEvaluateOn(ID3D12GraphicsCommandList* list, Ngx& ngx, Feature& f) {
  void* p = f.params;
  SetRes(p, "DLSSNR.Color", f.color);
  SetRes(p, "DLSSNR.Output", f.output);
  SetRes(p, "DLSSNR.MVec", f.motion);
  SetRes(p, "DLSSNR.Depth", f.depth);
  SetUI(p, "Width", f.w);
  SetUI(p, "Height", f.h);
  SetUI(p, "OutWidth", f.w);
  SetUI(p, "OutHeight", f.h);
  for (const char* k : {"DLSSNR.ColorSubrect", "DLSSNR.DepthSubrect", "DLSSNR.MVecSubrect", "DLSSNR.OutputSubrect"}) {
    const std::string s(k);
    SetUI(p, (s + "BaseX").c_str(), 0);
    SetUI(p, (s + "BaseY").c_str(), 0);
    SetUI(p, (s + "Width").c_str(), f.w);
    SetUI(p, (s + "Height").c_str(), f.h);
  }
  SetF(p, "DLSSNR.MVecScaleX", 1.f);
  SetF(p, "DLSSNR.MVecScaleY", 1.f);
  SetI(p, "DLSSNR.DepthInverted", 0);
  SetI(p, "DLSSNR.Enabled", 1);
  SetI(p, "DLSSNR.Reset", f.first ? 1 : 0);
  SetF(p, "DLSSNR.Intensity", 1.f);
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
  const uint32_t sizes[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {2560, 1600}, {3840, 2160}};
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
  {
    std::ifstream m(dir + "/meta.txt");
    m >> w >> h >> count;
  }
  if (!w || !h || !count) { std::fprintf(stderr, "bad %s/meta.txt\n", dir.c_str()); return 1; }
  Feature f;
  CreateFeature(g, ngx, f, w, h);
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: prism_nvngx.dll.exe <nvngx_dlssnr.dll> cost | seq <dir>\n");
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
  const NgxResult r = ngx.init(kApplicationId, dataPath.c_str(), g.device, kNgxVersionApi, nullptr);
  if (r != kNgxSuccess) { std::fprintf(stderr, "NGX Init failed: 0x%08x\n", r); return 1; }
  CheckSlots(ngx);
  const std::string mode = argv[2];
  int rc = 2;
  if (mode == "cost") rc = Cost(g, ngx);
  else if (mode == "seq" && argc >= 4) rc = Sequence(g, ngx, argv[3]);
  else if (mode == "async" && argc >= 5) rc = Async(g, ngx, uint32_t(std::atoi(argv[3])), uint32_t(std::atoi(argv[4])));
  if (ngx.shutdown) ngx.shutdown(g.device);
  return rc;
}

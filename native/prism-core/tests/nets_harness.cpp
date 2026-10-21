// Runs the NETS compute shaders on a D3D12 GPU, checks them against the CPU reference
// (tools/nets-sim, --dump) and times them at a chosen output resolution.
//
//   nets_harness <shaderDir> check <dumpDir>
//   nets_harness <shaderDir> time <width> <height> <scale>
//
// It runs on the Intel integrated GPU only, unless PRISM_ADAPTER=any is set: the RTX card is
// not ours to use for this.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
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

struct Constants {
  uint32_t full[2];
  uint32_t work[2];
  float scale;
  float phase[2];
  float sigma;
  float historyMax;
  float fillWeight;
  float depthTolerance;
  float printTolerance;
  float velocityCap;
  float sampleGain;
  uint32_t firstFrame;
  uint32_t catmull;
};
static_assert(sizeof(Constants) == 16 * 4, "16 root constants");

enum Slot { kLow, kFullG, kFullD, kMotion, kFill, kHistH, kHistDG, kOut, kOutDG, kSlots };
const uint32_t kStrideRef[kSlots] = {16, 4, 4, 8, 16, 16, 8, 16, 8};
// nets_step.hlsl stores edits, weights, motion, depth and luminance as packed 16-bit floats.
const uint32_t kStrideStep[kSlots] = {8, 4, 4, 4, 16, 8, 4, 8, 4};
const uint32_t* kStride = kStrideRef;

uint16_t ToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  int32_t exp = int32_t((x >> 23) & 0xffu) - 127 + 15;
  uint32_t mant = x & 0x7fffffu;
  if (exp <= 0) {
    if (exp < -10) return uint16_t(sign);
    mant |= 0x800000u;
    const uint32_t shift = uint32_t(14 - exp);
    uint32_t h = mant >> shift;
    if ((mant >> (shift - 1)) & 1u) ++h;  // round half up
    return uint16_t(sign | h);
  }
  if (exp >= 31) return uint16_t(sign | 0x7c00u);
  uint32_t h = sign | (uint32_t(exp) << 10) | (mant >> 13);
  if (mant & 0x1000u) ++h;  // round to nearest
  return uint16_t(h);
}
float FromHalf(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  int32_t exp = (h >> 10) & 0x1f;
  uint32_t mant = h & 0x3ffu;
  uint32_t x;
  if (exp == 0) {
    if (mant == 0) x = sign;
    else {
      exp = 1;
      while (!(mant & 0x400u)) { mant <<= 1; --exp; }
      mant &= 0x3ffu;
      x = sign | (uint32_t(exp - 15 + 127) << 23) | (mant << 13);
    }
  } else if (exp == 31) x = sign | 0x7f800000u | (mant << 13);
  else x = sign | (uint32_t(exp - 15 + 127) << 23) | (mant << 13);
  float f;
  std::memcpy(&f, &x, 4);
  return f;
}
// Packs float32 values to halves, returned in a float vector's storage for Upload.
std::vector<float> PackHalves(const std::vector<float>& v) {
  std::vector<float> out((v.size() + 1) / 2);
  auto* h = reinterpret_cast<uint16_t*>(out.data());
  for (size_t i = 0; i < v.size(); ++i) h[i] = ToHalf(v[i]);
  return out;
}
std::vector<float> UnpackHalves(const std::vector<float>& packed, size_t count) {
  std::vector<float> out(count);
  const auto* h = reinterpret_cast<const uint16_t*>(packed.data());
  for (size_t i = 0; i < count; ++i) out[i] = FromHalf(h[i]);
  return out;
}

struct Gpu {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE event = nullptr;
  uint64_t fenceValue = 0;
  ID3D12RootSignature* root = nullptr;
  ID3D12PipelineState* fill = nullptr;
  ID3D12PipelineState* accumulate = nullptr;
  ID3D12PipelineState* step = nullptr;

  void Submit() {
    CHECK(list->Close());
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    CHECK(queue->Signal(fence, ++fenceValue));
    CHECK(fence->SetEventOnCompletion(fenceValue, event));
    WaitForSingleObject(event, INFINITE);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator, nullptr));
  }
};

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

IDXGIAdapter1* PickAdapter(IDXGIFactory1* factory) {
  const char* env = std::getenv("PRISM_ADAPTER");
  const bool any = env && std::string(env) == "any";
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    if (!software && (any || desc.VendorId == 0x8086)) {
      std::printf("adapter: %ls\n", desc.Description);
      return adapter;
    }
    adapter->Release();
  }
  std::fprintf(stderr, "no Intel GPU found (set PRISM_ADAPTER=any to allow another)\n");
  std::exit(1);
}

ID3DBlob* Compile(const std::string& dir, const char* file) {
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  const std::wstring path = Widen(dir + "/" + file);
  const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (errors) std::fprintf(stderr, "%s: %s\n", file, static_cast<const char*>(errors->GetBufferPointer()));
  CHECK(hr);
  return code;
}

void Init(Gpu& g, const std::string& shaderDir) {
  IDXGIFactory1* factory = nullptr;
  CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)));
  IDXGIAdapter1* adapter = PickAdapter(factory);
  CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&g.device)));
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  CHECK(g.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g.queue)));
  CHECK(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&g.allocator)));
  CHECK(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&g.list)));
  CHECK(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g.fence)));
  g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  // b0: 16 root constants; t0..t6 root SRVs; u0..u1 root UAVs.
  D3D12_ROOT_PARAMETER params[10] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 16;
  for (int t = 0; t < 7; ++t) {
    params[1 + t].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1 + t].Descriptor.ShaderRegister = t;
  }
  for (int u = 0; u < 2; ++u) {
    params[8 + u].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[8 + u].Descriptor.ShaderRegister = u;
  }
  D3D12_ROOT_SIGNATURE_DESC rd = {};
  rd.NumParameters = 10;
  rd.pParameters = params;
  ID3DBlob* blob = nullptr;
  ID3DBlob* err = nullptr;
  CHECK(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
  CHECK(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&g.root)));

  for (auto [file, pso] : {std::pair{"nets_fill.hlsl", &g.fill}, std::pair{"nets_accumulate.hlsl", &g.accumulate},
                           std::pair{"nets_step.hlsl", &g.step}}) {
    ID3DBlob* cs = Compile(shaderDir, file);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g.root;
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    CHECK(g.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(pso)));
  }
}

ID3D12Resource* Buffer(Gpu& g, uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav) {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = heap;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  ID3D12Resource* r = nullptr;
  CHECK(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&r)));
  return r;
}

void Barrier(Gpu& g, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  g.list->ResourceBarrier(1, &b);
}

// Every slot lives in a default-heap UAV buffer, uploaded once. Shader inputs read through
// root SRVs from the generic-read state; outputs stay in UAV state.
struct Buffers {
  ID3D12Resource* res[kSlots] = {};
  uint64_t count[kSlots] = {};
};

void Upload(Gpu& g, Buffers& b, Slot s, const std::vector<float>& data, uint64_t elements) {
  const uint64_t bytes = elements * kStride[s];
  b.count[s] = elements;
  b.res[s] = Buffer(g, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
  ID3D12Resource* up = Buffer(g, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
  void* p = nullptr;
  CHECK(up->Map(0, nullptr, &p));
  if (!data.empty()) std::memcpy(p, data.data(), bytes);
  else std::memset(p, 0, bytes);
  up->Unmap(0, nullptr);
  g.list->CopyBufferRegion(b.res[s], 0, up, 0, bytes);
  const bool output = s == kOut || s == kOutDG || s == kFill;
  Barrier(g, b.res[s], D3D12_RESOURCE_STATE_COPY_DEST,
          output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  g.Submit();
  up->Release();
}

std::vector<float> Readback(Gpu& g, Buffers& b, Slot s, D3D12_RESOURCE_STATES state) {
  const uint64_t bytes = b.count[s] * kStride[s];
  ID3D12Resource* rb = Buffer(g, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
  Barrier(g, b.res[s], state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  g.list->CopyBufferRegion(rb, 0, b.res[s], 0, bytes);
  Barrier(g, b.res[s], D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  g.Submit();
  std::vector<float> out(bytes / 4);
  void* p = nullptr;
  CHECK(rb->Map(0, nullptr, &p));
  std::memcpy(out.data(), p, bytes);
  rb->Unmap(0, nullptr);
  rb->Release();
  return out;
}

// Records fill then accumulate. The fill buffer flips to SRV for the accumulate and back.
void Record(Gpu& g, Buffers& b, const Constants& c, ID3D12QueryHeap* qh, uint32_t q) {
  auto bind = [&](ID3D12PipelineState* pso, ID3D12Resource* out0) {
    g.list->SetPipelineState(pso);
    g.list->SetComputeRootSignature(g.root);
    g.list->SetComputeRoot32BitConstants(0, 16, &c, 0);
    for (int t = 0; t < 7; ++t) {
      // A buffer the pass writes cannot also be bound as its input: point that slot at Low.
      ID3D12Resource* r = (b.res[t] && b.res[t] != out0) ? b.res[t] : b.res[kLow];
      g.list->SetComputeRootShaderResourceView(1 + t, r->GetGPUVirtualAddress());
    }
    g.list->SetComputeRootUnorderedAccessView(8, out0->GetGPUVirtualAddress());
    g.list->SetComputeRootUnorderedAccessView(9, b.res[kOutDG]->GetGPUVirtualAddress());
  };
  const UINT gx = (c.full[0] + 15) / 16, gy = (c.full[1] + 15) / 16;
  if (kStride == kStrideStep) {
    // Production form: one pass, no fill buffer.
    if (qh) g.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, q);
    bind(g.step, b.res[kOut]);
    g.list->Dispatch(gx, gy, 1);
    if (qh) { g.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, q + 1); g.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, q + 2); }
    return;
  }
  if (qh) g.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, q);
  bind(g.fill, b.res[kFill]);
  // The fill pass writes Fill as its UAV; t4 points at Low meanwhile (bound but unused).
  g.list->Dispatch(gx, gy, 1);
  Barrier(g, b.res[kFill], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (qh) g.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
  bind(g.accumulate, b.res[kOut]);
  g.list->Dispatch(gx, gy, 1);
  if (qh) g.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, q + 2);
  Barrier(g, b.res[kFill], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

std::vector<float> ReadF32(const std::string& file) {
  std::ifstream in(file, std::ios::binary | std::ios::ate);
  if (!in) { std::fprintf(stderr, "missing %s\n", file.c_str()); std::exit(1); }
  const auto size = static_cast<size_t>(in.tellg());
  std::vector<float> v(size / 4);
  in.seekg(0);
  in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(size));
  return v;
}

double JsonNumber(const std::string& json, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const size_t p = json.find(k);
  if (p == std::string::npos) { std::fprintf(stderr, "meta.json lacks %s\n", key); std::exit(1); }
  return std::strtod(json.c_str() + p + k.size(), nullptr);
}

// Largest and mean absolute difference over the first `channels` of each element, and the share
// of pixels over tolerance. `allowed` is the share that may exceed it: with half precision a
// pixel sitting exactly on a depth or fingerprint threshold can take the other branch.
void Compare(const char* name, const std::vector<float>& gpu, const std::vector<float>& ref, int stride, int channels, double tolerance, int& failures, double allowed = 0.0) {
  double maxd = 0.0, sum = 0.0;
  size_t n = 0, pixels = 0, over = 0;
  for (size_t i = 0; i + stride <= ref.size(); i += stride) {
    bool bad = false;
    for (int c = 0; c < channels; ++c) {
      const double d = std::fabs(double(gpu[i + c]) - double(ref[i + c]));
      maxd = std::max(maxd, d);
      sum += d;
      ++n;
      bad = bad || d > tolerance;
    }
    ++pixels;
    over += bad ? 1 : 0;
  }
  const double share = double(over) / double(pixels);
  const bool ok = share <= allowed;
  std::printf("%s %-10s max |d| %.2e  mean |d| %.2e  over %.0e: %zu px (%.4f%%)\n", ok ? "ok  " : "FAIL", name, maxd,
              sum / double(n), tolerance, over, share * 100.0);
  if (!ok) ++failures;
}

int Check(Gpu& g, const std::string& dir) {
  std::ifstream mf(dir + "/meta.json");
  const std::string meta((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
  Constants c = {};
  c.full[0] = uint32_t(JsonNumber(meta, "W"));
  c.full[1] = uint32_t(JsonNumber(meta, "H"));
  c.work[0] = uint32_t(JsonNumber(meta, "w"));
  c.work[1] = uint32_t(JsonNumber(meta, "h"));
  c.scale = float(JsonNumber(meta, "s"));
  const size_t ph = meta.find("\"phase\":");
  std::sscanf(meta.c_str() + meta.find('[', ph), "[ %f , %f", &c.phase[0], &c.phase[1]);
  c.sigma = float(JsonNumber(meta, "sigma"));
  c.historyMax = float(JsonNumber(meta, "wmax"));
  c.fillWeight = float(JsonNumber(meta, "wfill"));
  c.depthTolerance = float(JsonNumber(meta, "depthTol"));
  c.printTolerance = float(JsonNumber(meta, "printTol"));
  c.velocityCap = float(JsonNumber(meta, "velCap"));
  c.sampleGain = float(JsonNumber(meta, "gain"));
  c.catmull = uint32_t(JsonNumber(meta, "catmull"));
  c.firstFrame = 0;
  const uint64_t full = uint64_t(c.full[0]) * c.full[1], work = uint64_t(c.work[0]) * c.work[1];
  std::printf("dump: %ux%u full, %ux%u working, phase %.3f,%.3f\n", c.full[0], c.full[1], c.work[0], c.work[1], c.phase[0], c.phase[1]);

  const bool step = kStride == kStrideStep;
  auto in = [&](const char* name) { auto v = ReadF32(dir + "/" + name); return step ? PackHalves(v) : v; };
  Buffers b;
  Upload(g, b, kLow, in("low.f32"), work);
  Upload(g, b, kFullG, ReadF32(dir + "/fullG.f32"), full);
  Upload(g, b, kFullD, ReadF32(dir + "/fullD.f32"), full);
  Upload(g, b, kMotion, in("motion.f32"), full);
  Upload(g, b, kHistH, in("histH.f32"), full);
  Upload(g, b, kHistDG, in("histDG.f32"), full);
  Upload(g, b, kFill, {}, full);
  Upload(g, b, kOut, {}, full);
  Upload(g, b, kOutDG, {}, full);
  Record(g, b, c, nullptr, 0);
  g.Submit();

  int failures = 0;
  const auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (!step) {
    // The CPU reference computes in double; float32 exp and accumulation stay well inside 1e-3 stop.
    Compare("fill", Readback(g, b, kFill, uav), ReadF32(dir + "/refFill.f32"), 4, 3, 1e-3, failures);
    Compare("edit", Readback(g, b, kOut, uav), ReadF32(dir + "/refH.f32"), 4, 3, 1e-3, failures);
    Compare("weight", Readback(g, b, kOut, uav), ReadF32(dir + "/refH.f32"), 4, 4, 1e-2, failures);
    Compare("depth+luma", Readback(g, b, kOutDG, uav), ReadF32(dir + "/refDG.f32"), 2, 2, 1e-5, failures);
  } else {
    // Half precision: inputs and outputs are quantised to 11 significant bits (~1e-3 relative),
    // well below what a viewer can see in a log2 edit (0.01 stop).
    const auto h = UnpackHalves(Readback(g, b, kOut, uav), full * 4);
    Compare("edit", h, ReadF32(dir + "/refH.f32"), 4, 3, 1e-2, failures, 1e-3);
    Compare("weight", h, ReadF32(dir + "/refH.f32"), 4, 4, 5e-2, failures, 1e-3);
    Compare("depth+luma", UnpackHalves(Readback(g, b, kOutDG, uav), full * 2), ReadF32(dir + "/refDG.f32"), 2, 2, 1e-2, failures);
  }
  return failures ? 1 : 0;
}

int Time(Gpu& g, uint32_t width, uint32_t height, float scale) {
  Constants c = {};
  c.full[0] = width;
  c.full[1] = height;
  c.work[0] = uint32_t(std::lround(width * scale));
  c.work[1] = uint32_t(std::lround(height * scale));
  c.scale = scale;
  c.sigma = 0.55f; c.historyMax = 12.f; c.fillWeight = 0.25f; c.depthTolerance = 0.05f; c.printTolerance = 0.25f;
  c.velocityCap = 2.f; c.sampleGain = 3.f; c.catmull = 1; c.firstFrame = 0;
  const uint64_t full = uint64_t(width) * height, work = uint64_t(c.work[0]) * c.work[1];

  // Plausible data: smooth edits, a slow pan, matching depth so the history path is taken.
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> n(-0.05f, 0.05f);
  std::vector<float> low(work * 4), g1(full), d(full, 0.7f), mv(full * 2), hh(full * 4), hdg(full * 2);
  for (uint64_t i = 0; i < work; ++i) { low[i * 4] = 0.2f + n(rng); low[i * 4 + 1] = 0.1f + n(rng); low[i * 4 + 2] = n(rng); low[i * 4 + 3] = -2.f + n(rng); }
  for (uint64_t i = 0; i < full; ++i) {
    g1[i] = -2.f + n(rng); mv[i * 2] = 0.4f; mv[i * 2 + 1] = 0.15f;
    hh[i * 4] = 0.2f; hh[i * 4 + 1] = 0.1f; hh[i * 4 + 2] = 0.f; hh[i * 4 + 3] = 8.f;
    hdg[i * 2] = 0.7f; hdg[i * 2 + 1] = g1[i];
  }
  const bool step = kStride == kStrideStep;
  auto in = [&](const std::vector<float>& v) { return step ? PackHalves(v) : v; };
  Buffers b;
  Upload(g, b, kLow, in(low), work);
  Upload(g, b, kFullG, g1, full);
  Upload(g, b, kFullD, d, full);
  Upload(g, b, kMotion, in(mv), full);
  Upload(g, b, kHistH, in(hh), full);
  Upload(g, b, kHistDG, in(hdg), full);
  Upload(g, b, kFill, {}, full);
  Upload(g, b, kOut, {}, full);
  Upload(g, b, kOutDG, {}, full);

  const uint32_t runs = 60;
  D3D12_QUERY_HEAP_DESC qd = {};
  qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qd.Count = runs * 3;
  ID3D12QueryHeap* qh = nullptr;
  CHECK(g.device->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&qh)));
  ID3D12Resource* rb = Buffer(g, qd.Count * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
  for (uint32_t r = 0; r < runs; ++r) {
    Record(g, b, c, qh, r * 3);
    D3D12_RESOURCE_BARRIER u = {};
    u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    g.list->ResourceBarrier(1, &u);
  }
  g.list->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, qd.Count, rb, 0);
  g.Submit();
  uint64_t freq = 0;
  CHECK(g.queue->GetTimestampFrequency(&freq));
  uint64_t* ts = nullptr;
  CHECK(rb->Map(0, nullptr, reinterpret_cast<void**>(&ts)));
  std::vector<double> fillMs, accMs;
  for (uint32_t r = 10; r < runs; ++r) {  // skip warm-up
    fillMs.push_back(double(ts[r * 3 + 1] - ts[r * 3]) * 1000.0 / double(freq));
    accMs.push_back(double(ts[r * 3 + 2] - ts[r * 3 + 1]) * 1000.0 / double(freq));
  }
  rb->Unmap(0, nullptr);
  std::sort(fillMs.begin(), fillMs.end());
  std::sort(accMs.begin(), accMs.end());
  std::printf("%ux%u, working %ux%u (scale %.2f): fill %.3f ms, accumulate %.3f ms (median of %zu)\n", width, height,
              c.work[0], c.work[1], scale, fillMs[fillMs.size() / 2], accMs[accMs.size() / 2], fillMs.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: nets_harness <shaderDir> check <dumpDir> | time <w> <h> <scale>\n");
    return 2;
  }
  Gpu g;
  Init(g, argv[1]);
  std::string mode = argv[2];
  // "-step" selects the production shader (nets_step.hlsl) instead of the reference pair.
  if (mode.size() > 5 && mode.compare(mode.size() - 5, 5, "-step") == 0) {
    kStride = kStrideStep;
    mode.resize(mode.size() - 5);
  }
  if (mode == "check" && argc >= 4) return Check(g, argv[3]);
  if (mode == "time" && argc >= 6) return Time(g, uint32_t(std::atoi(argv[3])), uint32_t(std::atoi(argv[4])), float(std::atof(argv[5])));
  std::fprintf(stderr, "bad arguments\n");
  return 2;
}

// Prism Core (prism_nvngx.dll): loaded by the version.dll proxy at start-up, or late, by
// PrismLaunch.exe into a game that is already running (games whose anti-tamper refuses a proxy DLL
// in their folder; nothing about the protection is touched). Finds the NGX modules as they load
// (or already loaded), hooks their D3D12 create / evaluate entry points, and sees every DLSS
// evaluation the game makes: colour, output, motion vectors, depth, sizes, jitter, reset.
//
// Stage 1: observe and log only. The picture is not touched.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include "MinHook.h"
#include "ngx_param.h"
#include "pipeline.h"

namespace {

using namespace prism;

// ------------------------------------------------------------------ log
FILE* logFile = nullptr;
std::mutex logMutex;
void Log(const char* fmt, ...) {
  std::lock_guard<std::mutex> lock(logMutex);
  if (!logFile) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(logFile, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list a;
  va_start(a, fmt);
  std::vfprintf(logFile, fmt, a);
  va_end(a);
  std::fputc('\n', logFile);
  std::fflush(logFile);
}

std::wstring ModuleDir(HMODULE m) {
  wchar_t p[MAX_PATH];
  const DWORD n = GetModuleFileNameW(m, p, MAX_PATH);
  std::wstring s(p, n);
  return s.substr(0, s.find_last_of(L"\\/") + 1);
}

// ------------------------------------------------------------------ hooks
using CreateFn = ngx::Result (*)(ID3D12GraphicsCommandList*, unsigned, void*, ngx::Handle**);
using EvaluateFn = ngx::Result (*)(ID3D12GraphicsCommandList*, const ngx::Handle*, const void*, void*);

struct Hooked {
  const char* module;
  CreateFn create = nullptr;
  EvaluateFn evaluate = nullptr;
  EvaluateFn evaluateC = nullptr;
};
// The NGX core of the driver and the DLSS feature module; each exports the D3D12 API.
Hooked hooked[] = {{"_nvngx.dll"}, {"nvngx_dlss.dll"}, {"nvngx.dll"}};

std::mutex featureMutex;
std::unordered_map<const ngx::Handle*, unsigned> featureOf;
thread_local int depth = 0;  // nested NGX calls (module copies forward into each other)
std::atomic<uint64_t> srEvaluations{0};
std::atomic<bool> pipelineStarted{false};  // PipelineInit ran (at the game's upscale create, or late)
bool lateLoad = false;                     // loaded into a game that had already started its renderer

const char* FormatName(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
    case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_D32_FLOAT: return "D32F";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "RGBA16_TYPELESS";
    default: return nullptr;
  }
}

std::string Describe(const void* p, const char* key) {
  ID3D12Resource* r = nullptr;
  if (!ngx::GetRes(p, key, &r)) return std::string(key) + "=-";
  const D3D12_RESOURCE_DESC d = r->GetDesc();
  char buf[160];
  const char* f = FormatName(d.Format);
  std::snprintf(buf, sizeof buf, "%s=%llux%u %s%s", key, static_cast<unsigned long long>(d.Width), d.Height,
                f ? f : "fmt", f ? "" : std::to_string(int(d.Format)).c_str());
  return buf;
}

bool IsUpscale(unsigned f) { return f == ngx::kFeatureSuperSampling || f == ngx::kFeatureRayReconstruction; }

unsigned FeatureOf(const ngx::Handle* h) {
  std::lock_guard<std::mutex> lock(featureMutex);
  auto it = featureOf.find(h);
  return it != featureOf.end() ? it->second : (h ? h->id : 0);
}

// Super resolution (1) or ray reconstruction (13, DLSS-D): both upscale the frame and carry
// colour, output, motion vectors and depth. An evaluation whose create Core never saw (loaded late,
// or created through another module copy) is told by exactly those inputs; the network's own
// keys are DLSSNR.*.
bool Upscale(const ngx::Handle* h, const void* p) {
  {
    std::lock_guard<std::mutex> lock(featureMutex);
    auto it = featureOf.find(h);
    if (it != featureOf.end()) return IsUpscale(it->second);
  }
  ID3D12Resource* r = nullptr;
  return ngx::GetRes(p, "Color", &r) && ngx::GetRes(p, "Output", &r) && ngx::GetRes(p, "MotionVectors", &r) && ngx::GetRes(p, "Depth", &r);
}

// The game's DLSS create flags (depth inverted, low-resolution motion vectors, ...) when Core
// arrives after the create: games that reuse one parameter block still carry them; otherwise
// LateCreateFlags from prism-core.ini, or the common reverse-Z, low-resolution-vector setup.
int LateCreateFlags(const void* p) {
  int flags = 0;
  if (ngx::GetI(p, "DLSS.Feature.Create.Flags", &flags) && flags) return flags;
  const std::wstring ini = ModuleDir(nullptr) + L"prism-core.ini";
  return int(GetPrivateProfileIntW(L"Prism Core", L"LateCreateFlags", (1 << 1) | (1 << 3), ini.c_str()));
}

void Observe(const char* module, ID3D12GraphicsCommandList* list, const ngx::Handle* h, const void* p) {
  if (!Upscale(h, p)) return;
  const unsigned feature = FeatureOf(h);
  const uint64_t n = ++srEvaluations;
  if (n > 5 && n % 1000 != 0) return;
  unsigned w = 0, hh = 0, ow = 0, oh = 0;
  ngx::GetUI(p, "Width", &w); ngx::GetUI(p, "Height", &hh);
  ngx::GetUI(p, "OutWidth", &ow); ngx::GetUI(p, "OutHeight", &oh);
  float jx = 0, jy = 0, mx = 0, my = 0, exposure = 0;
  ngx::GetF(p, "Jitter.Offset.X", &jx); ngx::GetF(p, "Jitter.Offset.Y", &jy);
  ngx::GetF(p, "MV.Scale.X", &mx); ngx::GetF(p, "MV.Scale.Y", &my);
  ngx::GetF(p, "DLSS.Pre.Exposure", &exposure);
  int reset = 0;
  ngx::GetI(p, "Reset", &reset);
  Log("upscale evaluate #%llu feature %u via %s list=%s render %ux%u out %ux%u jitter %.3f,%.3f mvscale %.3f,%.3f exposure %.3f reset %d | %s %s %s %s",
      static_cast<unsigned long long>(n), feature, module, list && list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute" : "direct",
      w, hh, ow, oh, jx, jy, mx, my, exposure, reset, Describe(p, "Color").c_str(), Describe(p, "Output").c_str(),
      Describe(p, "MotionVectors").c_str(), Describe(p, "Depth").c_str());
}

// After the game's upscale: hand the frame to Prism's pipeline.
void Process(ID3D12GraphicsCommandList* list, const ngx::Handle* h, const void* p) {
  if (!list || !Upscale(h, p)) return;
  if (!pipelineStarted.exchange(true)) {
    // Core arrived after the game created its upscale (late load): start from this evaluation.
    const int flags = LateCreateFlags(p);
    ID3D12Device* dev = nullptr;
    list->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    PipelineInit(ModuleDir(nullptr), flags, &Log, dev);
    if (dev) dev->Release();
    Log("upscale already running when Core arrived: starting from its evaluation, create flags 0x%x", flags);
  }
  UpscaleCall c = {};
  c.list = list;
  ID3D12Resource* color = nullptr;
  if (!ngx::GetRes(p, "Output", &c.output) || !ngx::GetRes(p, "MotionVectors", &c.motion) || !ngx::GetRes(p, "Depth", &c.depth) ||
      !ngx::GetRes(p, "Color", &color))
    return;
  const D3D12_RESOURCE_DESC cd = color->GetDesc();
  c.renderWidth = UINT(cd.Width);
  c.renderHeight = cd.Height;
  unsigned sw = 0, sh = 0;  // the render subrect, where the game declares one
  if (ngx::GetUI(p, "DLSS.Render.Subrect.Dimensions.Width", &sw) && sw && ngx::GetUI(p, "DLSS.Render.Subrect.Dimensions.Height", &sh) && sh) {
    c.renderWidth = sw;
    c.renderHeight = sh;
  }
  ngx::GetF(p, "MV.Scale.X", &c.motionScaleX);
  ngx::GetF(p, "MV.Scale.Y", &c.motionScaleY);
  ngx::GetI(p, "Reset", &c.reset);
  PipelineRecord(c);
}

template <int I>
ngx::Result HookCreate(ID3D12GraphicsCommandList* list, unsigned feature, void* p, ngx::Handle** out) {
  ++depth;
  const ngx::Result r = hooked[I].create(list, feature, p, out);
  --depth;
  if (r == ngx::kSuccess && out && *out) {
    std::lock_guard<std::mutex> lock(featureMutex);
    featureOf[*out] = feature;
  }
  if (r == ngx::kSuccess && depth == 0 && IsUpscale(feature) && p) {
    int flags = 0;
    ngx::GetI(p, "DLSS.Feature.Create.Flags", &flags);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ID3D12Device* dev = nullptr;
    if (list) list->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    PipelineInit(ModuleDir(nullptr), flags, &Log, dev);
    pipelineStarted = true;
    if (dev) dev->Release();
    Log("upscale feature %u created, flags 0x%x", feature, flags);
  }
  if (depth == 0) Log("CreateFeature %u via %s -> 0x%08x", feature, hooked[I].module, r);
  return r;
}

template <int I, bool C>
ngx::Result HookEvaluate(ID3D12GraphicsCommandList* list, const ngx::Handle* h, const void* p, void* cb) {
  const bool outer = depth == 0;
  ++depth;
  const ngx::Result r = (C ? hooked[I].evaluateC : hooked[I].evaluate)(list, h, p, cb);
  --depth;
  if (outer && r == ngx::kSuccess && p) {
    Observe(hooked[I].module, list, h, p);
    Process(list, h, p);
  }
  return r;
}

template <int I>
void HookModule(HMODULE m) {
  auto hook = [&](const char* name, void* detour, void** original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(m, name));
    if (!target) return;
    if (MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK)
      Log("hooked %s!%s", hooked[I].module, name);
  };
  hook("NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void*>(&HookCreate<I>), reinterpret_cast<void**>(&hooked[I].create));
  hook("NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void*>(&HookEvaluate<I, false>), reinterpret_cast<void**>(&hooked[I].evaluate));
  hook("NVSDK_NGX_D3D12_EvaluateFeature_C", reinterpret_cast<void*>(&HookEvaluate<I, true>), reinterpret_cast<void**>(&hooked[I].evaluateC));
}

// Present mode (Present=1): a game without DLSS never creates an upscale, so Core hooks Present on
// its own once the game has D3D12 and DXGI up (a short-lived device of its own finds the vtables;
// every device and swap chain of the process shares them).
DWORD WINAPI PresentWatch(void*) {
  const std::wstring ini = ModuleDir(nullptr) + L"prism-core.ini";
  if (!GetPrivateProfileIntW(L"Prism Core", L"Present", 0, ini.c_str())) return 0;
  int seen = 0;
  for (int tick = 0; tick < 20 * 600 && seen < 40; ++tick) {  // up to ten minutes; two seconds after D3D12 and DXGI load
    if (pipelineStarted) return 0;
    if (GetModuleHandleW(L"d3d12.dll") && GetModuleHandleW(L"dxgi.dll")) ++seen;
    Sleep(50);
  }
  if (pipelineStarted || seen < 40) return 0;
  using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  auto create = (CreateDeviceFn)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateDevice");
  ID3D12Device* dev = nullptr;
  if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev)) || !dev) { Log("present mode: no D3D12 device to find Present with"); return 0; }
  // Reverse depth: present mode's constant depth of zero reads as far.
  PipelineInit(ModuleDir(nullptr), 1 << 3, &Log, dev);
  dev->Release();
  Log("present mode: Present hooked without the game's DLSS");
  return 0;
}

// NGX modules load late (when the game initialises DLSS); watch for them.
DWORD WINAPI Watch(void*) {
  bool done[3] = {};
  for (int tick = 0; tick < 20 * 60 * 10; ++tick) {  // ten minutes
    if (!done[0]) if (HMODULE m = GetModuleHandleA(hooked[0].module)) { HookModule<0>(m); done[0] = true; }
    if (!done[1]) if (HMODULE m = GetModuleHandleA(hooked[1].module)) { HookModule<1>(m); done[1] = true; }
    if (!done[2]) if (HMODULE m = GetModuleHandleA(hooked[2].module)) { HookModule<2>(m); done[2] = true; }
    if (done[0] && done[1]) break;
    Sleep(50);
  }
  return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(self);
    const std::wstring dir = ModuleDir(self);
    logFile = _wfopen((dir + L"prism-core.log").c_str(), L"w");
    // Late: the game had already created its renderer and loaded NGX before Core.
    lateLoad = GetModuleHandleW(L"d3d12.dll") && (GetModuleHandleA("_nvngx.dll") || GetModuleHandleA("nvngx_dlss.dll"));
    Log("Prism Core 1.4.0 (Home panel, F8 on/off, F2 cadence, F3 async, F4 split, F11 capture; prism-core.ini overrides) loaded%s",
        lateLoad ? " late, into a running game" : "");
    if (MH_Initialize() != MH_OK) { Log("MinHook init failed"); return TRUE; }
    CloseHandle(CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, PresentWatch, nullptr, 0, nullptr));
  }
  return TRUE;
}

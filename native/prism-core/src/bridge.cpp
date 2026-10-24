// The game's side of the frame bridge (see bridge_proto.h). Each presented frame goes to
// prism_host.exe through shared memory and comes back processed, before the real Present:
//   DX11 / DX10  the back buffer is copied to a staging texture, mapped and copied out; the result
//                is written into the staging texture and copied back into the back buffer
//   DX9          GetRenderTargetData into a system-memory surface, and back through a default-pool
//                surface with UpdateSurface and StretchRect
// The copies wait for the GPU, so the game's frame and Prism's run one after the other: these
// APIs cannot share video memory with D3D12 in a way every driver and game supports.
#include "bridge.h"

#include <windows.h>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "MinHook.h"
#include "bridge_proto.h"

namespace prism {
namespace {

BridgeLogFn Log = nullptr;
std::wstring gameDir;
std::mutex bridgeMutex;

#define BLOG(...) do { if (Log) Log(__VA_ARGS__); } while (0)

// ------------------------------------------------------------------ the shared memory and the host
struct Shared {
  HANDLE map = nullptr, posted = nullptr, done = nullptr, host = nullptr;
  BridgeHeader* h = nullptr;
  uint8_t* px = nullptr;
  bool failed = false;
  int timeouts = 0;
  uint32_t frames = 0;
} sh;

bool Open() {
  if (sh.h) return true;
  if (sh.failed || gameDir.empty()) return false;
  wchar_t m[64], p[64], d[64];
  BridgeNames(GetCurrentProcessId(), m, p, d, 64);
  sh.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(sizeof(BridgeHeader) + kBridgeMaxBytes), m);
  sh.posted = CreateEventW(nullptr, FALSE, FALSE, p);
  sh.done = CreateEventW(nullptr, FALSE, FALSE, d);
  if (sh.map) sh.h = static_cast<BridgeHeader*>(MapViewOfFile(sh.map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
  if (!sh.h || !sh.posted || !sh.done) { BLOG("bridge off: no shared memory (%lu)", GetLastError()); sh.failed = true; return false; }
  std::memset(sh.h, 0, sizeof(BridgeHeader));
  sh.h->magic = kBridgeMagic;
  sh.h->version = kBridgeVersion;
  sh.px = reinterpret_cast<uint8_t*>(sh.h) + sizeof(BridgeHeader);
  // The host: 64-bit, from the game's folder; it finds the game by its process id.
  const std::wstring exe = gameDir + L"prism_host.exe";
  std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW si = {};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, gameDir.c_str(), &si, &pi)) {
    BLOG("bridge off: prism_host.exe did not start (%lu)", GetLastError());
    sh.failed = true;
    return false;
  }
  CloseHandle(pi.hThread);
  sh.host = pi.hProcess;
  BLOG("bridge: prism_host.exe started (pid %lu)", pi.dwProcessId);
  return true;
}

// The frame is in the pixel area: hand it over and wait. True when the result came back.
bool Exchange() {
  if (!sh.h->hostReady) return false;  // the host is still starting (device, network)
  if (WaitForSingleObject(sh.host, 0) == WAIT_OBJECT_0) { BLOG("bridge off: prism_host.exe exited"); sh.failed = true; return false; }
  const int32_t n = sh.h->posted + 1;
  sh.h->posted = n;
  SetEvent(sh.posted);
  if (WaitForSingleObject(sh.done, sh.frames ? 1000 : 15000) != WAIT_OBJECT_0 || sh.h->done != n) {
    BLOG("bridge: frame %d timed out", n);
    if (++sh.timeouts >= 3) { BLOG("bridge off: the host stopped answering"); sh.failed = true; }
    return false;
  }
  sh.timeouts = 0;
  if (++sh.frames == 1) BLOG("bridge: first frame processed (%ux%u, format %u, DX%u)", sh.h->width, sh.h->height, sh.h->format, sh.h->api);
  return sh.h->status == 0;
}

uint32_t BytesPer(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return 8;
    default: return 0;
  }
}
// The format the host is told: 8-bit formats as UNORM (the bytes are sRGB-encoded either way).
DXGI_FORMAT HostFormat(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
  }
}
// The encoding: scRGB for half floats, HDR10 for a 10-bit chain on an HDR display, else SDR.
uint32_t ModeFor(IDXGISwapChain* sc, DXGI_FORMAT f) {
  if (f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS) return 1;
  if (f != DXGI_FORMAT_R10G10B10A2_UNORM && f != DXGI_FORMAT_R10G10B10A2_TYPELESS) return 0;
  uint32_t mode = 0;
  IDXGIOutput* o = nullptr;
  if (SUCCEEDED(sc->GetContainingOutput(&o))) {
    IDXGIOutput6* o6 = nullptr;
    if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6))) {
      DXGI_OUTPUT_DESC1 od;
      if (SUCCEEDED(o6->GetDesc1(&od)) && od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) mode = 2;
      o6->Release();
    }
    o->Release();
  }
  return mode;
}
void Describe(uint32_t w, uint32_t h, DXGI_FORMAT f, uint32_t mode, uint32_t api) {
  sh.h->width = w; sh.h->height = h; sh.h->format = uint32_t(HostFormat(f)); sh.h->pitch = w * BytesPer(f); sh.h->mode = mode; sh.h->api = api;
}
void CopyOut(const uint8_t* src, uint32_t srcPitch, uint32_t rows, uint32_t rowBytes) {
  for (uint32_t y = 0; y < rows; ++y) std::memcpy(sh.px + size_t(y) * rowBytes, src + size_t(y) * srcPitch, rowBytes);
}
void CopyIn(uint8_t* dst, uint32_t dstPitch, uint32_t rows, uint32_t rowBytes) {
  for (uint32_t y = 0; y < rows; ++y) std::memcpy(dst + size_t(y) * dstPitch, sh.px + size_t(y) * rowBytes, rowBytes);
}
bool Fits(uint32_t w, uint32_t h, uint32_t bpp) { return bpp && w && h && uint64_t(w) * h * bpp <= kBridgeMaxBytes; }

// ------------------------------------------------------------------ DX11
struct D11 {
  ID3D11Device* dev = nullptr;
  ID3D11Texture2D *staging = nullptr, *resolve = nullptr;
  UINT w = 0, h = 0, samples = 0;
  DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
  void Release() { if (staging) staging->Release(); if (resolve) resolve->Release(); staging = resolve = nullptr; w = h = 0; }
} d11;

bool Present11(IDXGISwapChain* sc, ID3D11Device* dev) {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return false;
  D3D11_TEXTURE2D_DESC bd;
  bb->GetDesc(&bd);
  const uint32_t bpp = BytesPer(bd.Format);
  const bool msaa = bd.SampleDesc.Count > 1;
  if (!Fits(bd.Width, bd.Height, bpp)) { bb->Release(); return false; }
  if (dev != d11.dev || bd.Width != d11.w || bd.Height != d11.h || bd.Format != d11.f || bd.SampleDesc.Count != d11.samples) {
    d11.Release();
    d11.dev = dev; d11.w = bd.Width; d11.h = bd.Height; d11.f = bd.Format; d11.samples = bd.SampleDesc.Count;
    D3D11_TEXTURE2D_DESC sd = bd;
    sd.SampleDesc = {1, 0}; sd.MipLevels = 1; sd.ArraySize = 1; sd.MiscFlags = 0;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    dev->CreateTexture2D(&sd, nullptr, &d11.staging);
    if (msaa) {
      D3D11_TEXTURE2D_DESC rd = bd;
      rd.SampleDesc = {1, 0}; rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0; rd.MiscFlags = 0;
      dev->CreateTexture2D(&rd, nullptr, &d11.resolve);
    }
    if (!d11.staging || (msaa && !d11.resolve)) { BLOG("bridge: DX11 copies could not be made (%ux%u format %d)", bd.Width, bd.Height, int(bd.Format)); d11.Release(); bb->Release(); return false; }
  }
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  ID3D11Texture2D* src = bb;
  if (msaa) { ctx->ResolveSubresource(d11.resolve, 0, bb, 0, HostFormat(bd.Format)); src = d11.resolve; }
  ctx->CopyResource(d11.staging, src);
  bool ok = false;
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(d11.staging, 0, D3D11_MAP_READ, 0, &m))) {
    Describe(bd.Width, bd.Height, bd.Format, ModeFor(sc, bd.Format), 11);
    CopyOut(static_cast<const uint8_t*>(m.pData), m.RowPitch, bd.Height, bd.Width * bpp);
    ctx->Unmap(d11.staging, 0);
    // Prism's result back into the back buffer (not into a multisampled one: it is shown as the game made it).
    if (Exchange() && !msaa && SUCCEEDED(ctx->Map(d11.staging, 0, D3D11_MAP_WRITE, 0, &m))) {
      CopyIn(static_cast<uint8_t*>(m.pData), m.RowPitch, bd.Height, bd.Width * bpp);
      ctx->Unmap(d11.staging, 0);
      ctx->CopyResource(bb, d11.staging);
      ok = true;
    }
  }
  ctx->Release();
  bb->Release();
  return ok;
}

// ------------------------------------------------------------------ DX10
struct D10 {
  ID3D10Device* dev = nullptr;
  ID3D10Texture2D* staging = nullptr;
  UINT w = 0, h = 0;
  DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
  void Release() { if (staging) staging->Release(); staging = nullptr; w = h = 0; }
} d10;

bool Present10(IDXGISwapChain* sc, ID3D10Device* dev) {
  ID3D10Texture2D* bb = nullptr;
  if (FAILED(sc->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb)) || !bb) return false;
  D3D10_TEXTURE2D_DESC bd;
  bb->GetDesc(&bd);
  const uint32_t bpp = BytesPer(bd.Format);
  if (!Fits(bd.Width, bd.Height, bpp) || bd.SampleDesc.Count > 1) { bb->Release(); return false; }
  if (dev != d10.dev || bd.Width != d10.w || bd.Height != d10.h || bd.Format != d10.f) {
    d10.Release();
    d10.dev = dev; d10.w = bd.Width; d10.h = bd.Height; d10.f = bd.Format;
    D3D10_TEXTURE2D_DESC sd = bd;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.MiscFlags = 0;
    sd.Usage = D3D10_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ | D3D10_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &d10.staging))) { d10.Release(); bb->Release(); return false; }
  }
  dev->CopyResource(d10.staging, bb);
  bool ok = false;
  D3D10_MAPPED_TEXTURE2D m;
  if (SUCCEEDED(d10.staging->Map(0, D3D10_MAP_READ, 0, &m))) {
    Describe(bd.Width, bd.Height, bd.Format, ModeFor(sc, bd.Format), 10);
    CopyOut(static_cast<const uint8_t*>(m.pData), m.RowPitch, bd.Height, bd.Width * bpp);
    d10.staging->Unmap(0);
    if (Exchange() && SUCCEEDED(d10.staging->Map(0, D3D10_MAP_WRITE, 0, &m))) {
      CopyIn(static_cast<uint8_t*>(m.pData), m.RowPitch, bd.Height, bd.Width * bpp);
      d10.staging->Unmap(0);
      dev->CopyResource(bb, d10.staging);
      ok = true;
    }
  }
  bb->Release();
  return ok;
}

// ------------------------------------------------------------------ DX9
struct D9 {
  IDirect3DDevice9* dev = nullptr;
  IDirect3DSurface9 *sys = nullptr, *plain = nullptr, *rt = nullptr;
  UINT w = 0, h = 0;
  D3DFORMAT f = D3DFMT_UNKNOWN;
  D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE;
  void Release() {
    for (IDirect3DSurface9** s : {&sys, &plain, &rt}) if (*s) { (*s)->Release(); *s = nullptr; }
    w = h = 0;
  }
} d9;

bool Present9(IDirect3DDevice9* dev, IDirect3DSurface9* bb) {
  D3DSURFACE_DESC bd;
  if (FAILED(bb->GetDesc(&bd))) return false;
  if ((bd.Format != D3DFMT_X8R8G8B8 && bd.Format != D3DFMT_A8R8G8B8) || !Fits(bd.Width, bd.Height, 4)) return false;
  const bool msaa = bd.MultiSampleType != D3DMULTISAMPLE_NONE;
  if (dev != d9.dev || bd.Width != d9.w || bd.Height != d9.h || bd.Format != d9.f || bd.MultiSampleType != d9.ms) {
    d9.Release();
    d9.dev = dev; d9.w = bd.Width; d9.h = bd.Height; d9.f = bd.Format; d9.ms = bd.MultiSampleType;
    dev->CreateOffscreenPlainSurface(bd.Width, bd.Height, bd.Format, D3DPOOL_SYSTEMMEM, &d9.sys, nullptr);
    dev->CreateOffscreenPlainSurface(bd.Width, bd.Height, bd.Format, D3DPOOL_DEFAULT, &d9.plain, nullptr);
    if (msaa) dev->CreateRenderTarget(bd.Width, bd.Height, bd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &d9.rt, nullptr);
    if (!d9.sys || !d9.plain || (msaa && !d9.rt)) { BLOG("bridge: DX9 copies could not be made (%ux%u)", bd.Width, bd.Height); d9.Release(); return false; }
  }
  IDirect3DSurface9* src = bb;
  if (msaa) { if (FAILED(dev->StretchRect(bb, nullptr, d9.rt, nullptr, D3DTEXF_NONE))) return false; src = d9.rt; }
  if (FAILED(dev->GetRenderTargetData(src, d9.sys))) return false;
  D3DLOCKED_RECT lr;
  if (FAILED(d9.sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
  // X8R8G8B8 / A8R8G8B8 are B, G, R, A in memory: DXGI's B8G8R8A8.
  Describe(bd.Width, bd.Height, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 9);
  CopyOut(static_cast<const uint8_t*>(lr.pBits), UINT(lr.Pitch), bd.Height, bd.Width * 4);
  d9.sys->UnlockRect();
  if (!Exchange() || msaa) return false;
  if (FAILED(d9.sys->LockRect(&lr, nullptr, 0))) return false;
  CopyIn(static_cast<uint8_t*>(lr.pBits), UINT(lr.Pitch), bd.Height, bd.Width * 4);
  d9.sys->UnlockRect();
  return SUCCEEDED(dev->UpdateSurface(d9.sys, nullptr, d9.plain, nullptr)) && SUCCEEDED(dev->StretchRect(d9.plain, nullptr, bb, nullptr, D3DTEXF_NONE));
}

thread_local int presentDepth = 0;

using Present9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PresentEx9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using SwapPresent9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using Reset9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ResetEx9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
Present9Fn realPresent9 = nullptr;
PresentEx9Fn realPresentEx9 = nullptr;
SwapPresent9Fn realSwapPresent9 = nullptr;
Reset9Fn realReset9 = nullptr;
ResetEx9Fn realResetEx9 = nullptr;

void Frame9(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
  std::lock_guard<std::mutex> lock(bridgeMutex);
  if (!Open()) return;
  IDirect3DSurface9* bb = nullptr;
  if (sc ? FAILED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) : FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  Present9(dev, bb);
  bb->Release();
}
HRESULT STDMETHODCALLTYPE HkPresent9(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
  if (presentDepth++ == 0) Frame9(dev, nullptr);
  const HRESULT hr = realPresent9(dev, a, b, w, r);
  --presentDepth;
  return hr;
}
HRESULT STDMETHODCALLTYPE HkPresentEx9(IDirect3DDevice9Ex* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
  if (presentDepth++ == 0 && !(f & D3DPRESENT_DONOTWAIT)) Frame9(dev, nullptr);
  const HRESULT hr = realPresentEx9(dev, a, b, w, r, f);
  --presentDepth;
  return hr;
}
HRESULT STDMETHODCALLTYPE HkSwapPresent9(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
  if (presentDepth++ == 0) {
    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(&dev))) { Frame9(dev, sc); dev->Release(); }
  }
  const HRESULT hr = realSwapPresent9(sc, a, b, w, r, f);
  --presentDepth;
  return hr;
}
// Reset needs every default-pool resource released first, Prism's included.
HRESULT STDMETHODCALLTYPE HkReset9(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
  { std::lock_guard<std::mutex> lock(bridgeMutex); if (d9.dev == dev) d9.Release(); }
  return realReset9(dev, pp);
}
HRESULT STDMETHODCALLTYPE HkResetEx9(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode) {
  { std::lock_guard<std::mutex> lock(bridgeMutex); if (d9.dev == dev) d9.Release(); }
  return realResetEx9(dev, pp, mode);
}

bool Hook(void* target, void* detour, void** original, const char* what) {
  const MH_STATUS s = MH_CreateHook(target, detour, original);
  if (s == MH_ERROR_ALREADY_CREATED) return true;  // the same function through another vtable
  if (s != MH_OK || MH_EnableHook(target) != MH_OK) { BLOG("bridge: %s not hooked (%d)", what, int(s)); return false; }
  return true;
}

HWND ProbeWindow() {
  return CreateWindowExW(0, L"STATIC", L"prism-bridge", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, nullptr, nullptr);
}
HMODULE SystemModule(const wchar_t* name) {
  wchar_t path[MAX_PATH];
  GetSystemDirectoryW(path, MAX_PATH);
  lstrcatW(path, L"\\");
  lstrcatW(path, name);
  return LoadLibraryW(path);
}

// ------------------------------------------------------------------ DXGI (the 32-bit Core's own hook)
using DxgiPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using DxgiPresent1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
DxgiPresentFn realDxgiPresent = nullptr;
DxgiPresent1Fn realDxgiPresent1 = nullptr;
HRESULT STDMETHODCALLTYPE HkDxgiPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
  if (presentDepth++ == 0 && !(flags & DXGI_PRESENT_TEST)) BridgeDxgiPresent(sc);
  const HRESULT hr = realDxgiPresent(sc, sync, flags);
  --presentDepth;
  return hr;
}
HRESULT STDMETHODCALLTYPE HkDxgiPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
  if (presentDepth++ == 0 && !(flags & DXGI_PRESENT_TEST)) BridgeDxgiPresent(sc);
  const HRESULT hr = realDxgiPresent1(sc, sync, flags, pp);
  --presentDepth;
  return hr;
}

}  // namespace

void BridgeInit(const std::wstring& dir, BridgeLogFn log) {
  gameDir = dir;
  Log = log;
}

bool BridgeDxgiPresent(IDXGISwapChain* sc) {
  ID3D11Device* dev11 = nullptr;
  ID3D10Device* dev10 = nullptr;
  if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev11)) && dev11) {
    { std::lock_guard<std::mutex> lock(bridgeMutex); if (Open()) Present11(sc, dev11); }
    dev11->Release();
    return true;
  }
  if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D10Device), (void**)&dev10)) && dev10) {
    { std::lock_guard<std::mutex> lock(bridgeMutex); if (Open()) Present10(sc, dev10); }
    dev10->Release();
    return true;
  }
  return false;  // a D3D12 swap chain: Prism Core's own path
}

void BridgeHookD3D9() {
  static bool done = false;
  if (done) return;
  done = true;
  HMODULE m = SystemModule(L"d3d9.dll");
  using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
  using Create9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
  auto create = m ? (Create9Fn)(void*)GetProcAddress(m, "Direct3DCreate9") : nullptr;
  auto createEx = m ? (Create9ExFn)(void*)GetProcAddress(m, "Direct3DCreate9Ex") : nullptr;
  HWND wnd = ProbeWindow();
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = wnd;
  int hooked = 0;
  // Direct3D 9Ex first: its device carries PresentEx and ResetEx too.
  IDirect3D9Ex* d3dEx = nullptr;
  if (createEx && SUCCEEDED(createEx(D3D_SDK_VERSION, &d3dEx)) && d3dEx) {
    IDirect3DDevice9Ex* dev = nullptr;
    if (SUCCEEDED(d3dEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, nullptr, &dev)) && dev) {
      void** vt = *reinterpret_cast<void***>(dev);
      hooked += Hook(vt[16], (void*)&HkReset9, (void**)&realReset9, "Reset");
      hooked += Hook(vt[17], (void*)&HkPresent9, (void**)&realPresent9, "Present");
      hooked += Hook(vt[121], (void*)&HkPresentEx9, (void**)&realPresentEx9, "PresentEx");
      hooked += Hook(vt[132], (void*)&HkResetEx9, (void**)&realResetEx9, "ResetEx");
      IDirect3DSwapChain9* sc = nullptr;
      if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        hooked += Hook((*reinterpret_cast<void***>(sc))[3], (void*)&HkSwapPresent9, (void**)&realSwapPresent9, "IDirect3DSwapChain9::Present");
        sc->Release();
      }
      dev->Release();
    }
    d3dEx->Release();
  }
  // And a plain Direct3D 9 device, in case its methods live elsewhere.
  IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
  if (d3d) {
    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) && dev) {
      void** vt = *reinterpret_cast<void***>(dev);
      if (!realReset9) hooked += Hook(vt[16], (void*)&HkReset9, (void**)&realReset9, "Reset");
      if (!realPresent9) hooked += Hook(vt[17], (void*)&HkPresent9, (void**)&realPresent9, "Present");
      dev->Release();
    }
    d3d->Release();
  }
  if (wnd) DestroyWindow(wnd);
  BLOG(realPresent9 ? "bridge: Direct3D 9 Present hooked (%d hooks)" : "bridge: Direct3D 9 Present not hooked (%d hooks)", hooked);
}

void BridgeHookDxgi() {
  static bool done = false;
  if (done) return;
  done = true;
  HMODULE m = SystemModule(L"d3d11.dll");
  auto create = m ? (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(void*)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain") : nullptr;
  HWND wnd = ProbeWindow();
  DXGI_SWAP_CHAIN_DESC d = {};
  d.BufferDesc.Width = 8; d.BufferDesc.Height = 8; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 1; d.OutputWindow = wnd; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* sc = nullptr;
  ID3D11Device* dev = nullptr;
  if (create && wnd && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, nullptr)) && sc) {
    void** vt = *reinterpret_cast<void***>(sc);
    const bool ok = Hook(vt[8], (void*)&HkDxgiPresent, (void**)&realDxgiPresent, "IDXGISwapChain::Present") &&
                    Hook(vt[22], (void*)&HkDxgiPresent1, (void**)&realDxgiPresent1, "IDXGISwapChain1::Present1");
    BLOG(ok ? "bridge: DXGI Present hooked (DX10 / DX11)" : "bridge: DXGI Present not hooked");
  } else {
    BLOG("bridge: no DX11 probe device; DXGI Present not hooked");
  }
  if (sc) sc->Release();
  if (dev) dev->Release();
  if (wnd) DestroyWindow(wnd);
}

}  // namespace prism

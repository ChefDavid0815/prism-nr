// Draws Prism Core's in-game panel over a still, without a game: a hidden window, a D3D12 swap
// chain, the still copied into the back buffer, the panel drawn by the real overlay code (fed by
// a stand-in pipeline), then the back buffer read back and saved as a PNG.
//   overlay_demo <background.png> <out.png> [width height] [rgba8|rgb10a2|fp16]
//   overlay_demo <background.png> live [width height]   a window to try the panel in (Home opens it)
//   PRISM_OVERLAY_TEST=open,focus=N,pad,toast,hud chooses the panel state.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "MinHook.h"
#include "overlay.h"
#include "pipeline.h"

// ------------------------------------------------------------------ the stand-in pipeline
namespace prism {
namespace {
LiveSettings live = [] { LiveSettings s; s.cadence = 1; s.passes = 2; s.async = false; s.intensity = 2.4f; s.netScale = 0.625f; s.guard = 1.0f; s.netAuto = std::getenv("PRISM_DEMO_AUTO") != nullptr; s.fpsTarget = 90; return s; }();
std::wstring dir;
void LogStd(const char* fmt, ...) { va_list a; va_start(a, fmt); std::vfprintf(stderr, fmt, a); va_end(a); std::fputc('\n', stderr); }
}  // namespace
LiveSettings GetLiveSettings() { return live; }
void SetLiveSettings(const LiveSettings& s) { live = s; }
void SaveLiveSettings() {}
void GetLiveStats(LiveStats* o) {
  o->running = true;
  o->outW = 2560; o->outH = 1600; o->renderW = 1707; o->renderH = 1067; o->netW = 1600; o->netH = 1000;
  o->prismMs = 9.8f; o->renderFps = 79.2f; o->presentFps = 158.1f;
  o->frames = kFrameHistory;
  for (int i = 0; i < kFrameHistory; ++i) {
    // A measured-looking series: 12.6 ms with jitter, a few hitches.
    const float j = std::sin(i * 1.7f) * 0.45f + std::sin(i * 0.37f) * 0.3f;
    o->renderMs[i] = 12.6f + j + (i % 97 == 40 ? 9.0f : 0.0f) + (i % 61 == 20 ? 22.0f : 0.0f);
  }
}
std::wstring TakeLiveNotice() { return L""; }
std::wstring GameDir() { return dir; }
LogFn PipelineLogger() { return &LogStd; }
}  // namespace prism

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s failed: 0x%08lx (line %d)\n", #x, hr_, __LINE__); std::exit(1); } } while (0)

std::vector<uint8_t> LoadPng(const wchar_t* path, UINT w, UINT h) {
  IWICImagingFactory* f = nullptr;
  CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f));
  IWICBitmapDecoder* dec = nullptr;
  CHECK(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec));
  IWICBitmapFrameDecode* fr = nullptr;
  CHECK(dec->GetFrame(0, &fr));
  IWICBitmapScaler* sc = nullptr;
  CHECK(f->CreateBitmapScaler(&sc));
  CHECK(sc->Initialize(fr, w, h, WICBitmapInterpolationModeFant));
  IWICFormatConverter* cv = nullptr;
  CHECK(f->CreateFormatConverter(&cv));
  CHECK(cv->Initialize(sc, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
  std::vector<uint8_t> px(size_t(w) * h * 4);
  CHECK(cv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()));
  return px;
}

void SavePng(const wchar_t* path, const uint8_t* rgba, UINT w, UINT h, UINT pitch) {
  IWICImagingFactory* f = nullptr;
  CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f));
  IWICStream* st = nullptr;
  CHECK(f->CreateStream(&st));
  CHECK(st->InitializeFromFilename(path, GENERIC_WRITE));
  IWICBitmapEncoder* enc = nullptr;
  CHECK(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc));
  CHECK(enc->Initialize(st, WICBitmapEncoderNoCache));
  IWICBitmapFrameEncode* fr = nullptr;
  CHECK(enc->CreateNewFrame(&fr, nullptr));
  CHECK(fr->Initialize(nullptr));
  CHECK(fr->SetSize(w, h));
  // The PNG encoder takes BGRA.
  std::vector<uint8_t> bgra(rgba, rgba + size_t(pitch) * h);
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  CHECK(fr->SetPixelFormat(&fmt));
  CHECK(fr->WritePixels(h, pitch, pitch * h, bgra.data()));
  CHECK(fr->Commit());
  CHECK(enc->Commit());
}

uint16_t Half(float f) {
  uint32_t x; std::memcpy(&x, &f, 4);
  const uint32_t s = (x >> 16) & 0x8000u; int e = int((x >> 23) & 0xff) - 112; uint32_t m = x & 0x7fffff;
  if (e <= 0) return uint16_t(s);
  if (e >= 31) return uint16_t(s | 0x7c00);
  return uint16_t(s | (e << 10) | (m >> 13));
}
float HalfToFloat(uint16_t h) {
  const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
  if (!e) return 0.0f;
  const uint32_t x = s | ((e + 112) << 23) | (m << 13);
  float f; std::memcpy(&f, &x, 4); return f;
}
float Lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float Enc(float c) { c = std::fmax(c, 0.0f); return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: overlay_demo <background.png> <out.png|live> [width height] [rgba8|rgb10a2|fp16]\n"); return 2; }
  const bool live = std::wstring(argv[2]) == L"live";
  const UINT W = argc > 4 ? UINT(_wtoi(argv[3])) : 2560, H = argc > 4 ? UINT(_wtoi(argv[4])) : 1600;
  const std::wstring kind = argc > 5 ? argv[5] : L"rgba8";
  const DXGI_FORMAT fmt = kind == L"fp16" ? DXGI_FORMAT_R16G16B16A16_FLOAT : kind == L"rgb10a2" ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MH_Initialize();
  wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
  prism::dir = exe; prism::dir = prism::dir.substr(0, prism::dir.find_last_of(L"\\/") + 1);

  ID3D12Device* dev = nullptr;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dev));
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ID3D12CommandQueue* q = nullptr;
  CHECK(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q));
  HWND wnd = nullptr;
  if (live) {
    RECT rc = {0, 0, LONG(W), LONG(H)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowExW(0, L"STATIC", L"Prism Core panel (Home)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, nullptr, nullptr);
  } else {
    wnd = CreateWindowExW(0, L"STATIC", L"prism overlay demo", WS_POPUP, 0, 0, int(W), int(H), nullptr, nullptr, nullptr, nullptr);
  }
  IDXGIFactory2* fac = nullptr;
  CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&fac));
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = W; sd.Height = H; sd.Format = fmt; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1* sc1 = nullptr;
  CHECK(fac->CreateSwapChainForHwnd(q, wnd, &sd, nullptr, nullptr, &sc1));
  IDXGISwapChain3* sc = nullptr;
  CHECK(sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc));
  if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
  prism::OverlaySetup(*reinterpret_cast<void***>(sc));
  prism::OverlayNoteQueue(q);  // in a game the ExecuteCommandLists hook reports it

  // The still, in the back buffer's encoding.
  const std::vector<uint8_t> bg = LoadPng(argv[1], W, H);
  const UINT bpp = fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
  const UINT pitch = (W * bpp + 255) & ~255u;
  std::vector<uint8_t> enc(size_t(pitch) * H);
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) {
      const uint8_t* s = &bg[(size_t(y) * W + x) * 4];
      uint8_t* d = &enc[size_t(y) * pitch + x * bpp];
      if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
      else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        uint16_t* h = (uint16_t*)d;
        for (int c = 0; c < 3; ++c) h[c] = Half(Lin(s[c] / 255.0f) * 203.0f / 80.0f);  // scRGB at 203 nits white
        h[3] = Half(1.0f);
      } else {
        // HDR10: PQ at 203 nits white (BT.709 primaries left as they are for a still).
        uint32_t v = 3u << 30;
        for (int c = 0; c < 3; ++c) {
          const float l = Lin(s[c] / 255.0f) * 203.0f / 10000.0f;
          const float p = std::pow(l, 0.1593017578125f);
          const float e = std::pow((0.8359375f + 18.8515625f * p) / (1.0f + 18.6875f * p), 78.84375f);
          v |= uint32_t(std::lround(e * 1023.0f)) << (10 * c);
        }
        std::memcpy(d, &v, 4);
      }
    }
  D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_HEAP_PROPERTIES rb = {}; rb.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = enc.size(); bd.Height = 1; bd.DepthOrArraySize = 1;
  bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource *upload = nullptr, *readback = nullptr;
  CHECK(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&upload));
  CHECK(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&readback));
  void* m = nullptr;
  CHECK(upload->Map(0, nullptr, &m));
  std::memcpy(m, enc.data(), enc.size());
  upload->Unmap(0, nullptr);

  ID3D12CommandAllocator* al = nullptr;
  ID3D12GraphicsCommandList* l = nullptr;
  ID3D12Fence* fence = nullptr;
  CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al));
  CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&l));
  l->Close();
  CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence));
  uint64_t fv = 0;
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  auto wait = [&] { q->Signal(fence, ++fv); fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); };
  auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    l->ResourceBarrier(1, &br);
  };
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  fp.Footprint = {fmt, W, H, 1, pitch};

  const int frames = live ? 0x7fffffff : 40;
  for (int f = 0; f < frames; ++f) {
    if (live) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
      if (!IsWindow(wnd) || !IsWindowVisible(wnd)) return 0;
    }
    ID3D12Resource* bb = nullptr;
    CHECK(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&bb));
    wait();
    al->Reset(); l->Reset(al, nullptr);
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    l->Close();
    ID3D12CommandList* ls[] = {l};
    q->ExecuteCommandLists(1, ls);
    prism::OverlayPresent(sc);
    if (!live && f == frames - 1) {
      wait();
      al->Reset(); l->Reset(al, nullptr);
      barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION rdst = {}, rsrc = {};
      rdst.pResource = readback; rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; rdst.PlacedFootprint = fp;
      rsrc.pResource = bb; rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      l->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
      barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
      l->Close();
      q->ExecuteCommandLists(1, ls);
      wait();
    }
    bb->Release();
    sc->Present(0, 0);
    Sleep(16);
  }
  // Back to 8-bit sRGB for the PNG (HDR buffers decoded at the same 203-nit white).
  uint8_t* r = nullptr;
  CHECK(readback->Map(0, nullptr, (void**)&r));
  std::vector<uint8_t> out(size_t(W) * H * 4);
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) {
      const uint8_t* s = r + size_t(y) * pitch + x * bpp;
      uint8_t* d = &out[(size_t(y) * W + x) * 4];
      if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
      else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const uint16_t* h = (const uint16_t*)s;
        for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::lround(std::fmin(1.0f, Enc(HalfToFloat(h[c]) * 80.0f / 203.0f)) * 255.0f));
      } else {
        uint32_t v; std::memcpy(&v, s, 4);
        for (int c = 0; c < 3; ++c) {
          const float e = float((v >> (10 * c)) & 1023u) / 1023.0f;
          const float p = std::pow(e, 1.0f / 78.84375f);
          const float l2 = std::pow(std::fmax(p - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * p), 1.0f / 0.1593017578125f) * 10000.0f / 203.0f;
          d[c] = uint8_t(std::lround(std::fmin(1.0f, Enc(l2)) * 255.0f));
        }
      }
      d[3] = 255;
    }
  // PRISM_DEMO_CROP=x,y,w,h saves only that region (to look at it pixel for pixel).
  if (const char* c = std::getenv("PRISM_DEMO_CROP")) {
    int cx = 0, cy = 0, cw = 0, ch = 0;
    if (std::sscanf(c, "%d,%d,%d,%d", &cx, &cy, &cw, &ch) == 4 && cw > 0 && ch > 0 && cx + cw <= int(W) && cy + ch <= int(H)) {
      std::vector<uint8_t> crop(size_t(cw) * ch * 4);
      for (int y = 0; y < ch; ++y) std::memcpy(&crop[size_t(y) * cw * 4], &out[(size_t(cy + y) * W + cx) * 4], size_t(cw) * 4);
      SavePng(argv[2], crop.data(), UINT(cw), UINT(ch), UINT(cw) * 4);
      std::fprintf(stderr, "saved (crop)\n");
      return 0;
    }
  }
  SavePng(argv[2], out.data(), W, H, W * 4);
  std::fprintf(stderr, "saved\n");
  return 0;
}

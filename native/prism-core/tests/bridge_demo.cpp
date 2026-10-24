// The frame bridge end to end, without a game: a DX9 or DX11 "game" (built 32 or 64-bit) loads the
// Prism Core beside it (or a loader brings it in: PRISM_DEMO_LOADER), presents a still for a few
// seconds, then reads back its back buffer after the last Present (the copy swap effects keep it)
// and saves the still and the result side by side. prism_host.exe, nvngx_dlssnr.dll and
// prism-core.ini go beside the exe.
//   bridge_demo <9|11> <still.png> <out.png> [seconds]
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <wincodec.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s failed: 0x%08lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); std::exit(1); } } while (0)

struct Image { UINT w = 0, h = 0; std::vector<uint8_t> px; };  // BGRA8

Image Load(const wchar_t* path) {
  IWICImagingFactory* f = nullptr;
  CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f));
  IWICBitmapDecoder* dec = nullptr;
  CHECK(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec));
  IWICBitmapFrameDecode* fr = nullptr;
  CHECK(dec->GetFrame(0, &fr));
  IWICFormatConverter* cv = nullptr;
  CHECK(f->CreateFormatConverter(&cv));
  CHECK(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
  Image im;
  CHECK(cv->GetSize(&im.w, &im.h));
  im.px.resize(size_t(im.w) * im.h * 4);
  CHECK(cv->CopyPixels(nullptr, im.w * 4, UINT(im.px.size()), im.px.data()));
  return im;
}

void Save(const wchar_t* path, const Image& im) {
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
  CHECK(fr->SetSize(im.w, im.h));
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  CHECK(fr->SetPixelFormat(&fmt));
  CHECK(fr->WritePixels(im.h, im.w * 4, UINT(im.px.size()), const_cast<uint8_t*>(im.px.data())));
  CHECK(fr->Commit());
  CHECK(enc->Commit());
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: bridge_demo <9|11> <still.png> <out.png> [seconds]\n"); return 2; }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const int api = _wtoi(argv[1]);
  const double seconds = argc > 4 ? _wtof(argv[4]) : 8.0;
  const Image still = Load(argv[2]);
  const UINT W = still.w, H = still.h;
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir = exe;
  dir.resize(dir.find_last_of(L"\\/") + 1);
  if (!std::getenv("PRISM_DEMO_LOADER") && !LoadLibraryW((dir + L"prism_nvngx.dll").c_str())) { std::fprintf(stderr, "prism_nvngx.dll did not load (%lu)\n", GetLastError()); return 1; }
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PrismBridgeDemo";
  RegisterClassW(&wc);
  HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Prism bridge demo", WS_POPUP, 0, 0, int(W), int(H), nullptr, nullptr, wc.hInstance, nullptr);
  std::vector<uint8_t> result(size_t(W) * H * 4);
  UINT frames = 0;
  const DWORD start = GetTickCount();
  auto pump = [] { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m); };

  if (api == 11) {
    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferDesc.Width = W; d.BufferDesc.Height = H; d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 1; d.OutputWindow = wnd; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    CHECK(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx));
    ID3D11Texture2D* bb = nullptr;
    CHECK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb));
    while (GetTickCount() - start < DWORD(seconds * 1000)) {
      pump();
      ctx->UpdateSubresource(bb, 0, nullptr, still.px.data(), W * 4, 0);
      sc->Present(1, 0);
      ++frames;
    }
    D3D11_TEXTURE2D_DESC sd;
    bb->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    CHECK(dev->CreateTexture2D(&sd, nullptr, &st));
    ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    for (UINT y = 0; y < H; ++y) std::memcpy(result.data() + size_t(y) * W * 4, static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, W * 4);
    ctx->Unmap(st, 0);
  } else {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { std::fprintf(stderr, "no Direct3D 9\n"); return 1; }
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_COPY; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferWidth = W; pp.BackBufferHeight = H;
    pp.BackBufferCount = 1; pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
    IDirect3DSurface9 *sys = nullptr, *plain = nullptr, *bb = nullptr;
    CHECK(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    CHECK(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &plain, nullptr));
    D3DLOCKED_RECT lr;
    CHECK(sys->LockRect(&lr, nullptr, 0));
    for (UINT y = 0; y < H; ++y) std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, still.px.data() + size_t(y) * W * 4, W * 4);
    sys->UnlockRect();
    CHECK(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    while (GetTickCount() - start < DWORD(seconds * 1000)) {
      pump();
      dev->UpdateSurface(sys, nullptr, plain, nullptr);
      dev->StretchRect(plain, nullptr, bb, nullptr, D3DTEXF_NONE);
      dev->Present(nullptr, nullptr, nullptr, nullptr);
      ++frames;
    }
    IDirect3DSurface9* back = nullptr;
    CHECK(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &back, nullptr));
    CHECK(dev->GetRenderTargetData(bb, back));
    CHECK(back->LockRect(&lr, nullptr, D3DLOCK_READONLY));
    for (UINT y = 0; y < H; ++y) std::memcpy(result.data() + size_t(y) * W * 4, static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, W * 4);
    back->UnlockRect();
  }

  Image out;
  out.w = W * 2; out.h = H;
  out.px.resize(size_t(out.w) * H * 4);
  double diff = 0;
  for (UINT y = 0; y < H; ++y) {
    std::memcpy(out.px.data() + size_t(y) * out.w * 4, still.px.data() + size_t(y) * W * 4, W * 4);
    std::memcpy(out.px.data() + size_t(y) * out.w * 4 + W * 4, result.data() + size_t(y) * W * 4, W * 4);
    for (UINT x = 0; x < W * 4; ++x) if (x % 4 != 3) diff += std::abs(int(result[size_t(y) * W * 4 + x]) - int(still.px[size_t(y) * W * 4 + x]));
    for (UINT x = 0; x < W; ++x) out.px[(size_t(y) * out.w + W + x) * 4 + 3] = 255;
  }
  Save(argv[3], out);
  std::printf("DX%d, %u-bit: %u frames presented; mean change %.2f of 255 per channel\n", api, unsigned(sizeof(void*) * 8), frames, diff / (double(W) * H * 3));
  return 0;
}

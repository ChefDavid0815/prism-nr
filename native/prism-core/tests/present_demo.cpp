// Present mode end to end, without a game: loads the real prism_nvngx.dll (with Present=1 in the
// prism-core.ini beside this exe), presents a still on a D3D12 swap chain for a few seconds, then
// reads back what reached the screen and saves it, with the still, as a side-by-side PNG.
//   present_demo <still.png> <out.png> [seconds]
// nvngx_dlssnr.dll has to be beside the exe too (Core loads the network from the game's folder).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s failed: 0x%08lx (line %d)\n", #x, hr_, __LINE__); std::exit(1); } } while (0)

struct Image { UINT w = 0, h = 0; std::vector<uint8_t> px; };  // RGBA8

Image Load(const wchar_t* path) {
  IWICImagingFactory* f = nullptr;
  CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f));
  IWICBitmapDecoder* dec = nullptr;
  CHECK(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec));
  IWICBitmapFrameDecode* fr = nullptr;
  CHECK(dec->GetFrame(0, &fr));
  IWICFormatConverter* cv = nullptr;
  CHECK(f->CreateFormatConverter(&cv));
  CHECK(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
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
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
  CHECK(fr->SetPixelFormat(&fmt));
  CHECK(fr->WritePixels(im.h, im.w * 4, UINT(im.px.size()), const_cast<uint8_t*>(im.px.data())));
  CHECK(fr->Commit());
  CHECK(enc->Commit());
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: present_demo <still.png> <out.png> [seconds]\n"); return 2; }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const double seconds = argc > 3 ? _wtof(argv[3]) : 8.0;
  const Image still = Load(argv[1]);
  const UINT W = still.w, H = still.h;

  // Core first, as a loader would bring it in at start-up.
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir = exe;
  dir.resize(dir.find_last_of(L"\\/") + 1);
  // PRISM_DEMO_LOADER: Core comes in through a loader beside the exe (dxgi.dll, d3d12.dll), as in a game.
  if (!std::getenv("PRISM_DEMO_LOADER") && !LoadLibraryW((dir + L"prism_nvngx.dll").c_str())) { std::fprintf(stderr, "prism_nvngx.dll did not load (%lu)\n", GetLastError()); return 1; }

  // The adapter with the most video memory (the NVIDIA card on a laptop, not the integrated GPU).
  IDXGIFactory4* pick = nullptr;
  CHECK(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void**)&pick));
  IDXGIAdapter1* best = nullptr;
  SIZE_T bestMem = 0;
  for (UINT a = 0;; ++a) {
    IDXGIAdapter1* ad = nullptr;
    if (pick->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 ds; ad->GetDesc1(&ds);
    if (ds.DedicatedVideoMemory > bestMem) { bestMem = ds.DedicatedVideoMemory; if (best) best->Release(); best = ad; } else ad->Release();
  }
  ID3D12Device* dev = nullptr;
  CHECK(D3D12CreateDevice(best, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dev));
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ID3D12CommandQueue* q = nullptr;
  CHECK(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q));
  IDXGIFactory4* factory = nullptr;
  CHECK(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void**)&factory));
  WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PrismPresentDemo";
  RegisterClassW(&wc);
  HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Prism present demo", WS_POPUP, 0, 0, int(W), int(H), nullptr, nullptr, wc.hInstance, nullptr);
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  IDXGISwapChain1* sc1 = nullptr;
  CHECK(factory->CreateSwapChainForHwnd(q, wnd, &sd, nullptr, nullptr, &sc1));
  IDXGISwapChain3* sc = nullptr;
  CHECK(sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc));

  ID3D12CommandAllocator* al = nullptr;
  ID3D12GraphicsCommandList* l = nullptr;
  CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al));
  CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&l));
  CHECK(l->Close());
  ID3D12Fence* fence = nullptr;
  CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence));
  uint64_t fv = 0;
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  auto idle = [&] { q->Signal(fence, ++fv); fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); };

  // The still in an upload buffer, copied into each back buffer before it is presented.
  const UINT pitch = (W * 4 + 255) & ~255u;
  D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_HEAP_PROPERTIES rb = {}; rb.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UINT64(pitch) * H; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
  bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource *upload = nullptr, *readback = nullptr;
  CHECK(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&upload));
  CHECK(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&readback));
  uint8_t* m = nullptr;
  CHECK(upload->Map(0, nullptr, (void**)&m));
  for (UINT y = 0; y < H; ++y) std::memcpy(m + size_t(y) * pitch, still.px.data() + size_t(y) * W * 4, W * 4);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  fp.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; fp.Footprint.Width = W; fp.Footprint.Height = H; fp.Footprint.Depth = 1; fp.Footprint.RowPitch = pitch;
  auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    l->ResourceBarrier(1, &br);
  };

  const DWORD start = GetTickCount();
  UINT frames = 0, last = 0;
  while (GetTickCount() - start < DWORD(seconds * 1000)) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    const UINT i = sc->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    CHECK(sc->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&bb));
    idle();
    CHECK(al->Reset()); CHECK(l->Reset(al, nullptr));
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    CHECK(l->Close());
    ID3D12CommandList* ls[] = {l};
    q->ExecuteCommandLists(1, ls);
    CHECK(sc->Present(1, 0));
    bb->Release();
    last = i;
    ++frames;
  }
  // What the last present showed: Core processed it on this queue before the real Present.
  idle();
  ID3D12Resource* bb = nullptr;
  CHECK(sc->GetBuffer(last, __uuidof(ID3D12Resource), (void**)&bb));
  CHECK(al->Reset()); CHECK(l->Reset(al, nullptr));
  barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
  dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
  src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  CHECK(l->Close());
  ID3D12CommandList* ls[] = {l};
  q->ExecuteCommandLists(1, ls);
  idle();
  uint8_t* r = nullptr;
  CHECK(readback->Map(0, nullptr, (void**)&r));
  Image out;
  out.w = W * 2; out.h = H;
  out.px.resize(size_t(out.w) * H * 4);
  double diff = 0;
  for (UINT y = 0; y < H; ++y) {
    std::memcpy(out.px.data() + size_t(y) * out.w * 4, still.px.data() + size_t(y) * W * 4, W * 4);
    std::memcpy(out.px.data() + size_t(y) * out.w * 4 + W * 4, r + size_t(y) * pitch, W * 4);
    for (UINT x = 0; x < W * 4; ++x) if (x % 4 != 3) diff += std::abs(int(r[size_t(y) * pitch + x]) - int(still.px[size_t(y) * W * 4 + x]));
  }
  Save(argv[2], out);
  std::printf("%u frames presented; mean change %.2f of 255 per channel; saved original | presented\n", frames, diff / (double(W) * H * 3));
  return 0;
}

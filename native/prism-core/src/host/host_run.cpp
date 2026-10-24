// The host loop of the frame bridge (bridge_proto.h), inside Prism Core. prism_host.exe (64-bit)
// loads the 64-bit Core and calls PrismHostRun, because NVIDIA's runtime only serves calls from a
// module named like an NGX one (prism_nvngx.dll), not from an executable. Started by a DX9, DX10 or
// DX11 game (32 or 64-bit) with the game's process id: it opens the shared memory the game made,
// builds a D3D12 device on the card with the most video memory, and runs each frame the game posts
// through Prism's pipeline (PipelineFrame, as present mode does on a D3D12 back buffer), then hands
// it back. It returns when the game exits. Settings come from prism-core.ini in the game's folder.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstring>
#include <string>

#include "bridge_proto.h"
#include "pipeline.h"

namespace {
void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER br = {};
  br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
  l->ResourceBarrier(1, &br);
}
}  // namespace

namespace prism {

int HostRun(unsigned long pid, const std::wstring& dir, LogFn Log) {
  Log("prism_host 1.4.2 for process %lu", pid);

  HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
  wchar_t mName[64], pName[64], dName[64];
  BridgeNames(pid, mName, pName, dName, 64);
  HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mName);
  HANDLE posted = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, pName);
  HANDLE done = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, dName);
  auto* h = map ? static_cast<BridgeHeader*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
  if (!game || !h || !posted || !done || h->magic != kBridgeMagic || h->version != kBridgeVersion) { Log("no bridge from the game (%lu)", GetLastError()); return 1; }
  uint8_t* px = reinterpret_cast<uint8_t*>(h) + sizeof(BridgeHeader);

  // The card with the most video memory (the NVIDIA card on a laptop, not the integrated GPU).
  IDXGIFactory4* factory = nullptr;
  if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void**)&factory))) { Log("no DXGI factory"); return 1; }
  IDXGIAdapter1* best = nullptr;
  SIZE_T bestMem = 0;
  for (UINT a = 0;; ++a) {
    IDXGIAdapter1* ad = nullptr;
    if (factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 ds;
    ad->GetDesc1(&ds);
    if (ds.DedicatedVideoMemory > bestMem) { bestMem = ds.DedicatedVideoMemory; if (best) best->Release(); best = ad; } else ad->Release();
  }
  ID3D12Device* dev = nullptr;
  if (FAILED(D3D12CreateDevice(best, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dev))) { Log("no D3D12 device"); return 1; }
  DXGI_ADAPTER_DESC1 ad;
  best->GetDesc1(&ad);
  Log("device: %ls", ad.Description);
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ID3D12CommandQueue* q = nullptr;
  ID3D12CommandAllocator *allocUp = nullptr, *allocDown = nullptr;
  ID3D12GraphicsCommandList *up = nullptr, *down = nullptr;
  ID3D12Fence* fence = nullptr;
  if (FAILED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q)) ||
      FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&allocUp)) ||
      FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&allocDown)) ||
      FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocUp, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&up)) ||
      FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocDown, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&down)) ||
      FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence))) {
    Log("no queue");
    return 1;
  }
  up->Close();
  down->Close();
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  uint64_t fv = 0;
  auto idle = [&] { q->Signal(fence, ++fv); if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 5000); } };

  PipelineInit(dir, -1, Log, nullptr);   // the game's folder and settings; no Present to hook here
  h->hostPid = GetCurrentProcessId();
  h->hostReady = 1;
  Log("ready");

  ID3D12Resource *tex = nullptr, *upload = nullptr, *readback = nullptr;
  uint8_t *upMap = nullptr, *rbMap = nullptr;
  UINT tw = 0, th = 0, alignedPitch = 0;
  DXGI_FORMAT tf = DXGI_FORMAT_UNKNOWN;
  uint32_t frames = 0;
  for (;;) {
    HANDLE wait[2] = {posted, game};
    if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0) break;  // the game exited
    const UINT W = h->width, H = h->height, rowBytes = h->pitch;
    const DXGI_FORMAT F = DXGI_FORMAT(h->format);
    if (!W || !H || !rowBytes || uint64_t(rowBytes) * H > kBridgeMaxBytes) { h->status = 1; h->done = h->posted; SetEvent(done); continue; }
    if (W != tw || H != th || F != tf) {
      idle();
      for (ID3D12Resource** r : {&tex, &upload, &readback}) if (*r) { (*r)->Release(); *r = nullptr; }
      tw = W; th = H; tf = F;
      alignedPitch = (rowBytes + 255u) & ~255u;
      D3D12_HEAP_PROPERTIES hp = {};
      hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC td = {};
      td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
      td.Format = F; td.SampleDesc.Count = 1;
      dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&tex);
      D3D12_RESOURCE_DESC bd = {};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UINT64(alignedPitch) * H; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
      bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      hp.Type = D3D12_HEAP_TYPE_UPLOAD;
      dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&upload);
      hp.Type = D3D12_HEAP_TYPE_READBACK;
      dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&readback);
      if (!tex || !upload || !readback || FAILED(upload->Map(0, nullptr, (void**)&upMap)) || FAILED(readback->Map(0, nullptr, (void**)&rbMap))) {
        Log("frame textures failed (%ux%u format %d)", W, H, int(F));
        tw = th = 0;
        h->status = 1; h->done = h->posted; SetEvent(done);
        continue;
      }
      Log("frames %ux%u, format %d, mode %u, DX%u", W, H, int(F), h->mode, h->api);
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    fp.Footprint.Format = F; fp.Footprint.Width = W; fp.Footprint.Height = H; fp.Footprint.Depth = 1; fp.Footprint.RowPitch = alignedPitch;
    for (UINT y = 0; y < H; ++y) std::memcpy(upMap + size_t(y) * alignedPitch, px + size_t(y) * rowBytes, rowBytes);
    D3D12_TEXTURE_COPY_LOCATION t = {}, b = {};
    t.pResource = tex; t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; b.PlacedFootprint = fp;
    // Up: the game's frame into the texture.
    allocUp->Reset();
    up->Reset(allocUp, nullptr);
    Barrier(up, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    b.pResource = upload;
    up->CopyTextureRegion(&t, 0, 0, 0, &b, nullptr);
    Barrier(up, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    up->Close();
    ID3D12CommandList* l1[] = {up};
    q->ExecuteCommandLists(1, l1);
    // Prism.
    const bool ok = PipelineFrame(q, tex, int(h->mode));
    // Down: the result back to the game.
    allocDown->Reset();
    down->Reset(allocDown, nullptr);
    Barrier(down, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    b.pResource = readback;
    down->CopyTextureRegion(&b, 0, 0, 0, &t, nullptr);
    Barrier(down, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    down->Close();
    ID3D12CommandList* l2[] = {down};
    q->ExecuteCommandLists(1, l2);
    idle();
    for (UINT y = 0; y < H; ++y) std::memcpy(px + size_t(y) * rowBytes, rbMap + size_t(y) * alignedPitch, rowBytes);
    h->status = ok ? 0 : 1;
    h->done = h->posted;
    SetEvent(done);
    if (++frames == 1) Log("first frame %s", ok ? "processed" : "passed through");
  }
  Log("the game exited after %u frames", frames);
  return 0;
}

}  // namespace prism

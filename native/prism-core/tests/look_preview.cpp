// Previews Prism's looks (src/look_hlsl.h, the same HLSL Core runs) on a still, as a contact sheet:
//   原画 | 默认 | 高还原
//   高饱和 | 低饱和 | 浓烈
//   look_preview <original.png> <prism.png> <sheet.png>   an F11 capture pair (the network's edit is
//                                                          recovered from the pair, as Compose has it)
//   look_preview chart <sheet.png>                        a colour checker, skin, grey and sky chart
//   either, then --grade=<Grade= line> or --preset=<0-3>  the last tile shows the custom look (自定义)
//                                                          instead of 浓烈: a preset loaded as the
//                                                          custom look's starting point must match it
// The stills are display-referred; they go back to scene light through the inverse of Core's
// proxy encode (exposure-normalised, hue-preserving shoulder), are graded, and come forward again.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "grade.h"
#include "look_hlsl.h"

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s failed: 0x%08lx (line %d)\n", #x, hr_, __LINE__); std::exit(1); } } while (0)

struct Image { UINT w = 0, h = 0; std::vector<uint8_t> px; };  // RGBA8, sRGB

IWICImagingFactory* Wic() {
  static IWICImagingFactory* f = nullptr;
  if (!f) CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f));
  return f;
}
Image Load(const wchar_t* path) {
  IWICBitmapDecoder* dec = nullptr;
  CHECK(Wic()->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec));
  IWICBitmapFrameDecode* fr = nullptr;
  CHECK(dec->GetFrame(0, &fr));
  IWICFormatConverter* cv = nullptr;
  CHECK(Wic()->CreateFormatConverter(&cv));
  CHECK(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
  Image im;
  CHECK(cv->GetSize(&im.w, &im.h));
  im.px.resize(size_t(im.w) * im.h * 4);
  CHECK(cv->CopyPixels(nullptr, im.w * 4, UINT(im.px.size()), im.px.data()));
  return im;
}
void Save(const wchar_t* path, const Image& im) {
  IWICStream* st = nullptr;
  CHECK(Wic()->CreateStream(&st));
  CHECK(st->InitializeFromFilename(path, GENERIC_WRITE));
  IWICBitmapEncoder* enc = nullptr;
  CHECK(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc));
  CHECK(enc->Initialize(st, WICBitmapEncoderNoCache));
  IWICBitmapFrameEncode* fr = nullptr;
  CHECK(enc->CreateNewFrame(&fr, nullptr));
  CHECK(fr->Initialize(nullptr));
  CHECK(fr->SetSize(im.w, im.h));
  std::vector<uint8_t> bgra = im.px;  // the PNG encoder takes BGRA
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  CHECK(fr->SetPixelFormat(&fmt));
  CHECK(fr->WritePixels(im.h, im.w * 4, UINT(bgra.size()), bgra.data()));
  CHECK(fr->Commit());
  CHECK(enc->Commit());
}

// A colourist's test chart: the 24 ColorChecker patches (sRGB, D65), skin from pale to deep, a grey
// ramp, a saturated hue sweep and a sky gradient.
Image Chart() {
  static const uint8_t cc[24][3] = {{115, 82, 68},   {194, 150, 130}, {98, 122, 157},  {87, 108, 67},  {133, 128, 177}, {103, 189, 170},
                                    {214, 126, 44},  {80, 91, 166},   {193, 90, 99},   {94, 60, 108},  {157, 188, 64},  {224, 163, 46},
                                    {56, 61, 150},   {70, 148, 73},   {175, 54, 60},   {231, 199, 31}, {187, 86, 149},  {8, 133, 161},
                                    {243, 243, 242}, {200, 200, 200}, {160, 160, 160}, {122, 122, 121}, {85, 85, 85},   {52, 52, 52}};
  static const uint8_t skin[8][3] = {{255, 224, 196}, {241, 194, 167}, {224, 172, 138}, {198, 142, 110}, {170, 112, 82}, {141, 85, 62}, {110, 66, 48}, {75, 46, 36}};
  Image im;
  im.w = 1600; im.h = 1000;
  im.px.assign(size_t(im.w) * im.h * 4, 255);
  auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) { uint8_t* p = &im.px[(size_t(y) * im.w + x) * 4]; p[0] = r; p[1] = g; p[2] = b; p[3] = 255; };
  for (UINT y = 0; y < im.h; ++y) for (UINT x = 0; x < im.w; ++x) put(x, y, 40, 40, 40);
  for (int i = 0; i < 24; ++i) {  // 6 x 4 patches
    const int px0 = 40 + (i % 6) * 170, py0 = 40 + (i / 6) * 130;
    for (int y = py0; y < py0 + 120; ++y) for (int x = px0; x < px0 + 160; ++x) put(x, y, cc[i][0], cc[i][1], cc[i][2]);
  }
  for (int i = 0; i < 8; ++i) {  // skin column
    const int py0 = 40 + i * 64;
    for (int y = py0; y < py0 + 58; ++y) for (int x = 1080; x < 1560; ++x) put(x, y, skin[i][0], skin[i][1], skin[i][2]);
  }
  for (int y = 600; y < 680; ++y)  // grey ramp
    for (int x = 40; x < 1560; ++x) { const uint8_t v = uint8_t(std::lround(255.0 * (x - 40) / 1519.0)); put(x, y, v, v, v); }
  for (int y = 700; y < 820; ++y)  // hue sweep at full saturation, then half
    for (int x = 40; x < 1560; ++x) {
      const float hh = float(x - 40) / 1520.0f * 6.0f, s = y < 760 ? 1.0f : 0.5f;
      const float f = hh - std::floor(hh);
      float r, g, b;
      switch (int(hh) % 6) {
        case 0: r = 1; g = f; b = 0; break; case 1: r = 1 - f; g = 1; b = 0; break; case 2: r = 0; g = 1; b = f; break;
        case 3: r = 0; g = 1 - f; b = 1; break; case 4: r = f; g = 0; b = 1; break; default: r = 1; g = 0; b = 1 - f; break;
      }
      auto m = [&](float c) { return uint8_t(std::lround((1.0f - s + s * c) * 220.0f)); };
      put(x, y, m(r), m(g), m(b));
    }
  for (int y = 840; y < 960; ++y)  // sky: deep blue to haze
    for (int x = 40; x < 1560; ++x) {
      const float t = float(y - 840) / 120.0f;
      put(x, y, uint8_t(60 + 140 * t), uint8_t(110 + 110 * t), uint8_t(200 + 45 * t));
    }
  return im;
}

const char* kMain = R"(
Texture2D<float4> A : register(t0);  // the original, display-referred sRGB
Texture2D<float4> B : register(t1);  // Prism's default result
RWTexture2D<float4> O : register(u0);
cbuffer K : register(b0) { uint Look; uint ShowOriginal; };
float3 Dec(float3 c) { c = max(c, 0.0); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 Enc(float3 c) { c = max(c, 0.0); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
// Inverse and forward of Core's proxy encode: c = v / (1 + max(v)).
float3 ToScene(float3 s) { const float3 c = Dec(s); const float m = min(max(c.r, max(c.g, c.b)), 0.985); return c / (1.0 - m); }
float3 ToDisplay(float3 v) { v = max(v, 0.0); return Enc(v / (1.0 + max(v.r, max(v.g, v.b)))); }
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
  uint w, h; A.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  const float3 a = ToScene(A[id.xy].rgb), b = ToScene(B[id.xy].rgb);
  const float3 e = clamp(log2(max(b, 1.0 / 512.0) / max(a, 1.0 / 512.0)), -3.0, 3.0);
  LkUv = (float2(id.xy) + 0.5) / float2(w, h); LkPx = id.xy;
  const float3 r = ShowOriginal != 0u ? a : LookApply(a, e, Look, 1.0);
  O[id.xy] = float4(ToDisplay(r), 1.0);
}
)";

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // The custom look, if asked for: it takes the last tile.
  bool custom = false;
  prism::GradeValues grade;
  if (argc > 1 && !wcsncmp(argv[argc - 1], L"--grade=", 8)) { grade = prism::GradeParse(argv[argc - 1] + 8); custom = true; --argc; }
  else if (argc > 1 && !wcsncmp(argv[argc - 1], L"--preset=", 9)) { grade = prism::GradeFromPreset(_wtoi(argv[argc - 1] + 9)); custom = true; --argc; }
  float gradeK[prism::kGradeConstants];
  prism::GradeToShader(grade, gradeK);
  if (custom) std::printf("custom look: %ls\n", prism::GradeFormat(grade).c_str());
  Image a, b;
  const wchar_t* out;
  if (argc == 3 && !wcscmp(argv[1], L"chart")) { a = Chart(); b = a; out = argv[2]; }
  else if (argc == 4) { a = Load(argv[1]); b = Load(argv[2]); out = argv[3]; }
  else { std::fprintf(stderr, "usage: look_preview <original.png> <prism.png> <sheet.png> | look_preview chart <sheet.png>\n"); return 2; }
  if (a.w != b.w || a.h != b.h) { std::fprintf(stderr, "the two stills differ in size\n"); return 1; }
  const UINT W = a.w, H = a.h;

  ID3D12Device* dev = nullptr;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dev));
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ID3D12CommandQueue* q = nullptr;
  CHECK(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q));
  ID3D12CommandAllocator* al = nullptr;
  ID3D12GraphicsCommandList* l = nullptr;
  CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al));
  CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&l));
  ID3D12Fence* fence = nullptr;
  CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence));
  uint64_t fv = 0;
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  auto run = [&] {
    CHECK(l->Close());
    ID3D12CommandList* ls[] = {l};
    q->ExecuteCommandLists(1, ls);
    q->Signal(fence, ++fv); fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE);
    al->Reset(); l->Reset(al, nullptr);
  };

  // The shader: the looks and the preview's main.
  using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
  auto compile = (CompileFn)(void*)GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile");
  const std::string src = std::string(PRISM_LOOK_HLSL) + kMain;
  ID3DBlob *cs = nullptr, *err = nullptr;
  if (FAILED(compile(src.data(), src.size(), "look", nullptr, nullptr, "Main", "cs_5_1", 1 << 15, 0, &cs, &err))) {
    std::fprintf(stderr, "%s\n", err ? (const char*)err->GetBufferPointer() : "compile failed");
    return 1;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].OffsetInDescriptorsFromTableStart = 2;
  D3D12_ROOT_PARAMETER rp[3] = {};
  rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 2;
  rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = {2, ranges};
  rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[2].Descriptor.ShaderRegister = 1;
  D3D12_ROOT_SIGNATURE_DESC rd = {3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob* blob = nullptr;
  CHECK(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
  ID3D12RootSignature* root = nullptr;
  CHECK(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&root));
  // The custom look's constants (b1).
  ID3D12Resource* cb = nullptr;
  {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = 512; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&cb));
    void* m = nullptr;
    CHECK(cb->Map(0, nullptr, &m));
    std::memcpy(m, gradeK, sizeof(gradeK));
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = root; pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  ID3D12PipelineState* pso = nullptr;
  CHECK(dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&pso));

  // Textures and their copies.
  D3D12_HEAP_PROPERTIES def = {}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_HEAP_PROPERTIES rb = {}; rb.Type = D3D12_HEAP_TYPE_READBACK;
  auto tex = [&](bool uav, D3D12_RESOURCE_STATES st) {
    D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    CHECK(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, __uuidof(ID3D12Resource), (void**)&r));
    return r;
  };
  const UINT pitch = (W * 4 + 255) & ~255u;
  auto buffer = [&](D3D12_HEAP_PROPERTIES& hp, D3D12_RESOURCE_STATES st) {
    D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = UINT64(pitch) * H; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, __uuidof(ID3D12Resource), (void**)&r));
    return r;
  };
  ID3D12Resource *ta = tex(false, D3D12_RESOURCE_STATE_COPY_DEST), *tb = tex(false, D3D12_RESOURCE_STATE_COPY_DEST), *to = tex(true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource *ua = buffer(up, D3D12_RESOURCE_STATE_GENERIC_READ), *ub = buffer(up, D3D12_RESOURCE_STATE_GENERIC_READ), *rbk = buffer(rb, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; fp.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, pitch};
  auto fill = [&](ID3D12Resource* buf, const Image& im) {
    uint8_t* m = nullptr; CHECK(buf->Map(0, nullptr, (void**)&m));
    for (UINT y = 0; y < H; ++y) std::memcpy(m + size_t(y) * pitch, &im.px[size_t(y) * W * 4], W * 4);
    buf->Unmap(0, nullptr);
  };
  fill(ua, a); fill(ub, b);
  auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES s0, D3D12_RESOURCE_STATES s1) {
    D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, s0, s1};
    l->ResourceBarrier(1, &br);
  };
  for (auto [t, u] : {std::pair{ta, ua}, std::pair{tb, ub}}) {
    D3D12_TEXTURE_COPY_LOCATION dst = {}, s = {};
    dst.pResource = t; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = u; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = fp;
    l->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
    barrier(t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  run();
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ID3D12DescriptorHeap* heap = nullptr;
  CHECK(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&heap));
  const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE hc = heap->GetCPUDescriptorHandleForHeapStart();
  dev->CreateShaderResourceView(ta, nullptr, hc); hc.ptr += inc;
  dev->CreateShaderResourceView(tb, nullptr, hc); hc.ptr += inc;
  dev->CreateUnorderedAccessView(to, nullptr, nullptr, hc);

  // Six tiles at half size: original, then looks 0..4.
  Image sheet;
  const UINT tw = W / 2, th = H / 2;
  sheet.w = tw * 3; sheet.h = th * 2;
  sheet.px.assign(size_t(sheet.w) * sheet.h * 4, 0);
  for (int tile = 0; tile < 6; ++tile) {
    l->SetPipelineState(pso);
    l->SetComputeRootSignature(root);
    ID3D12DescriptorHeap* hs[] = {heap};
    l->SetDescriptorHeaps(1, hs);
    const uint32_t k[2] = {uint32_t(tile == 0 ? 0 : custom && tile == 5 ? 5 : tile - 1), tile == 0 ? 1u : 0u};
    l->SetComputeRoot32BitConstants(0, 2, k, 0);
    l->SetComputeRootConstantBufferView(2, cb->GetGPUVirtualAddress());
    l->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    l->Dispatch((W + 15) / 16, (H + 15) / 16, 1);
    barrier(to, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, s = {};
    dst.pResource = rbk; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    s.pResource = to; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
    barrier(to, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    run();
    uint8_t* m = nullptr; CHECK(rbk->Map(0, nullptr, (void**)&m));
    const UINT ox = (tile % 3) * tw, oy = (tile / 3) * th;
    for (UINT y = 0; y < th; ++y)
      for (UINT x = 0; x < tw; ++x)
        for (int c = 0; c < 3; ++c) {
          // 2x2 box in linear light keeps fine detail honest at half size.
          float acc = 0;
          for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) { const float v = m[size_t(2 * y + j) * pitch + (2 * x + i) * 4 + c] / 255.0f; acc += std::pow(v, 2.2f); }
          sheet.px[(size_t(oy + y) * sheet.w + ox + x) * 4 + c] = uint8_t(std::lround(std::pow(acc / 4.0f, 1.0f / 2.2f) * 255.0f));
        }
    for (UINT y = 0; y < th; ++y) for (UINT x = 0; x < tw; ++x) sheet.px[(size_t(oy + y) * sheet.w + ox + x) * 4 + 3] = 255;
    rbk->Unmap(0, nullptr);
  }
  Save(out, sheet);
  std::fprintf(stderr, "saved: 原画 | 默认 | 高还原 / 高饱和 | 低饱和 | 浓烈\n");
  return 0;
}

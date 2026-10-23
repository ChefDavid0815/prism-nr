// Prism Core's in-game panel (see overlay.h). Drawn the way the Prism NR app is drawn: a black
// field, white ink, 1px rules, square corners; selection is inversion and the four spectral state
// colours appear only as 2px marks. Every bar on it is a measurement: the frame-time barcode is
// one bar per rendered frame.
//
// Rendering: our own small D3D12 pipeline (one PSO per back buffer format) on the swap chain's
// queue, glyphs from DirectWrite (JetBrains Mono embedded, Microsoft YaHei UI for Chinese) in an
// R8 atlas. SDR, scRGB and HDR10 back buffers each get the panel at the same apparent brightness.
// Input: polled (cursor, keys, XInput) so it works when the game reads raw input only; while the
// panel is open the game's view of the keyboard, mouse and gamepad is neutral.
#include "overlay.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dwrite_3.h>
#include <dxgi1_6.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "font_mono_bold.h"
#include "font_mono_regular.h"
#include "pipeline.h"

namespace prism {
namespace {

#define LOG(...) do { if (LogFn lf = PipelineLogger()) lf(__VA_ARGS__); } while (0)

double Now() {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) / double(f.QuadPart);
}

// ------------------------------------------------------------------ palette (DESIGN.md)
constexpr uint32_t Rgba(uint32_t rgb, float a = 1.0f) {
  return ((rgb >> 16) & 0xff) | (rgb & 0xff00) | ((rgb & 0xff) << 16) | (uint32_t(a * 255.0f + 0.5f) << 24);
}
constexpr uint32_t kWhite = Rgba(0xffffff), kReadout = Rgba(0x8c8c8c), kQuiet = Rgba(0x5c5c5c), kHair = Rgba(0x262626),
                   kHairFaint = Rgba(0x1a1a1a), kBlack = Rgba(0x000000), kReady = Rgba(0x3ddc97), kCaution = Rgba(0xffc400),
                   kBlocked = Rgba(0xff3b30), kInfo = Rgba(0x4da3ff);
uint32_t Fade(uint32_t c, float a) { return (c & 0x00ffffff) | (uint32_t(float(c >> 24) * std::clamp(a, 0.0f, 1.0f) + 0.5f) << 24); }

// ------------------------------------------------------------------ settings of the panel itself
struct PanelConfig {
  int key = VK_HOME;
  float scale = 1.0f;       // OverlayScale, on top of the resolution's
  bool hint = true;         // OverlayHint: the "Home opens the panel" toast at start
  bool hud = false;         // OverlayHud: frame-rate corner while the panel is closed
  float x = 32, y = 32;     // OverlayX/Y: the panel's top-left, in panel units
  float nits = 203.0f;      // OverlayNits: the panel's white on HDR back buffers (BT.2408 graphics white)
  int hdr = -1;             // OverlayHdr: -1 auto, 0 SDR, 1 scRGB, 2 HDR10
  bool motion = true;       // OverlayMotion
  bool block = true;        // OverlayBlockInput
  bool pad = true;          // OverlayPad
} cfg;
FILETIME cfgTime = {};

int KeyFromName(const std::wstring& s, int fallback) {
  if (s.empty()) return fallback;
  if (!lstrcmpiW(s.c_str(), L"none")) return 0;
  static const std::pair<const wchar_t*, int> names[] = {{L"Home", VK_HOME}, {L"End", VK_END}, {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE},
                                                         {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Pause", VK_PAUSE}, {L"ScrollLock", VK_SCROLL}};
  for (const auto& [name, vk] : names) if (!lstrcmpiW(s.c_str(), name)) return vk;
  if ((s[0] == L'F' || s[0] == L'f') && s.size() > 1) { const int n = _wtoi(s.c_str() + 1); return n >= 1 && n <= 24 ? VK_F1 + n - 1 : fallback; }
  const int n = _wtoi(s.c_str());
  return n > 0 && n < 256 ? n : fallback;
}
const wchar_t* KeyName(int vk) {
  static wchar_t buf[8];
  switch (vk) {
    case VK_HOME: return L"Home"; case VK_END: return L"End"; case VK_INSERT: return L"Insert"; case VK_DELETE: return L"Delete";
    case VK_PRIOR: return L"PageUp"; case VK_NEXT: return L"PageDown"; case VK_PAUSE: return L"Pause"; case VK_SCROLL: return L"ScrollLock";
  }
  if (vk >= VK_F1 && vk <= VK_F24) { std::swprintf(buf, 8, L"F%d", vk - VK_F1 + 1); return buf; }
  std::swprintf(buf, 8, L"#%d", vk);
  return buf;
}

std::wstring IniPath() { return GameDir() + L"prism-core.ini"; }

void LoadPanelConfig() {
  const std::wstring path = IniPath();
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
  if (CompareFileTime(&fa.ftLastWriteTime, &cfgTime) == 0) return;
  cfgTime = fa.ftLastWriteTime;
  auto get = [&](const wchar_t* k) { wchar_t b[64] = {}; GetPrivateProfileStringW(L"Prism Core", k, L"", b, 64, path.c_str()); return std::wstring(b); };
  auto num = [&](const wchar_t* k, double d, double lo, double hi) { const std::wstring v = get(k); const double x = v.empty() ? d : _wtof(v.c_str()); return std::clamp(x, lo, hi); };
  cfg.key = KeyFromName(get(L"KeyOverlay"), VK_HOME);
  cfg.scale = float(num(L"OverlayScale", 1.0, 0.5, 3.0));
  cfg.hint = num(L"OverlayHint", 1, 0, 1) != 0;
  cfg.hud = num(L"OverlayHud", cfg.hud ? 1 : 0, 0, 1) != 0;
  cfg.x = float(num(L"OverlayX", 32, 0, 8000));
  cfg.y = float(num(L"OverlayY", 32, 0, 8000));
  cfg.nits = float(num(L"OverlayNits", 203, 80, 1000));
  const std::wstring hdr = get(L"OverlayHdr");
  cfg.hdr = !lstrcmpiW(hdr.c_str(), L"sdr") ? 0 : !lstrcmpiW(hdr.c_str(), L"scrgb") ? 1 : !lstrcmpiW(hdr.c_str(), L"hdr10") ? 2 : -1;
  cfg.motion = num(L"OverlayMotion", 1, 0, 1) != 0;
  cfg.block = num(L"OverlayBlockInput", 1, 0, 1) != 0;
  cfg.pad = num(L"OverlayPad", 1, 0, 1) != 0;
}

void SavePanelValue(const wchar_t* key, const std::wstring& v) {
  const std::wstring path = IniPath();
  WritePrivateProfileStringW(L"Prism Core", key, v.c_str(), path.c_str());
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) cfgTime = fa.ftLastWriteTime;
}

// ------------------------------------------------------------------ input: what the game sees
// While the panel is open (blocking), the game's reads of keys, cursor, raw input and XInput come
// back neutral and its window gets no mouse or key messages. Prism's own calls pass through.
std::atomic<bool> blocking{false};
const uint8_t* selfBegin = nullptr;
const uint8_t* selfEnd = nullptr;
bool FromSelf(void* ret) { return ret >= selfBegin && ret < selfEnd; }

using AsyncKeyFn = SHORT(WINAPI*)(int);
using KeyboardStateFn = BOOL(WINAPI*)(PBYTE);
using CursorPosFn = BOOL(WINAPI*)(LPPOINT);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using RawDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using RawBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using PadStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
AsyncKeyFn realAsyncKey = GetAsyncKeyState, realKeyState = GetKeyState;
KeyboardStateFn realKeyboardState = GetKeyboardState;
CursorPosFn realGetCursorPos = GetCursorPos;
SetCursorPosFn realSetCursorPos = SetCursorPos;
ClipCursorFn realClipCursor = ClipCursor;
RawDataFn realRawData = GetRawInputData;
RawBufferFn realRawBuffer = GetRawInputBuffer;
POINT frozenCursor = {};       // where the game's cursor was when the panel opened
RECT gameClip = {}; bool gameClipped = false;  // the game's cursor clip, restored on close

SHORT WINAPI HkAsyncKey(int vk) { return blocking && !FromSelf(__builtin_return_address(0)) ? 0 : realAsyncKey(vk); }
SHORT WINAPI HkKeyState(int vk) { return blocking && !FromSelf(__builtin_return_address(0)) ? 0 : realKeyState(vk); }
BOOL WINAPI HkKeyboardState(PBYTE k) {
  if (blocking && !FromSelf(__builtin_return_address(0)) && k) { std::memset(k, 0, 256); return TRUE; }
  return realKeyboardState(k);
}
BOOL WINAPI HkGetCursorPos(LPPOINT p) {
  if (blocking && !FromSelf(__builtin_return_address(0)) && p) { *p = frozenCursor; return TRUE; }
  return realGetCursorPos(p);
}
BOOL WINAPI HkSetCursorPos(int x, int y) {
  if (blocking && !FromSelf(__builtin_return_address(0))) { frozenCursor = {x, y}; return TRUE; }
  return realSetCursorPos(x, y);
}
BOOL WINAPI HkClipCursor(const RECT* r) {
  if (blocking && !FromSelf(__builtin_return_address(0))) { gameClipped = r != nullptr; if (r) gameClip = *r; return TRUE; }
  return realClipCursor(r);
}
// Movement, presses and the wheel are taken out; releases stay, so nothing held when the panel
// opened is left stuck down in the game.
void Neutral(RAWINPUT* ri) {
  if (ri->header.dwType == RIM_TYPEMOUSE) {
    RAWMOUSE& m = ri->data.mouse;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) { m.lLastX = 0; m.lLastY = 0; }
    m.usButtonFlags &= RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
    m.usButtonData = 0;
  } else if (ri->header.dwType == RIM_TYPEKEYBOARD && !(ri->data.keyboard.Flags & RI_KEY_BREAK)) {
    ri->data.keyboard.VKey = 0xff; ri->data.keyboard.MakeCode = 0;
  }
}
UINT WINAPI HkRawData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header) {
  const UINT r = realRawData(h, cmd, data, size, header);
  if (blocking && data && cmd == RID_INPUT && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER)) Neutral(static_cast<RAWINPUT*>(data));
  return r;
}
UINT WINAPI HkRawBuffer(PRAWINPUT data, PUINT size, UINT header) {
  const UINT r = realRawBuffer(data, size, header);
  if (blocking && data && r != UINT(-1)) {
    RAWINPUT* ri = data;
    for (UINT i = 0; i < r; ++i) {  // NEXTRAWINPUTBLOCK, 8-byte aligned in a 64-bit process
      Neutral(ri);
      ri = (RAWINPUT*)((ULONG_PTR(ri) + ri->header.dwSize + 7) & ~ULONG_PTR(7));
    }
  }
  return r;
}
// XInput, per module the game may load. A blocked read is a neutral pad with a packet number of
// its own, so the game takes it as a change of state.
struct PadHook { const char* module; PadStateFn real = nullptr; bool done = false; };
PadHook padHooks[] = {{"xinput1_4.dll"}, {"xinput1_3.dll"}, {"xinput9_1_0.dll"}, {"xinputuap.dll"}};
template <int I>
DWORD WINAPI HkPadState(DWORD i, XINPUT_STATE* st) {
  const DWORD r = padHooks[I].real(i, st);
  if (blocking && r == ERROR_SUCCESS && st && !FromSelf(__builtin_return_address(0))) {
    st->Gamepad = {};
    st->dwPacketNumber ^= 0x40000000u;
  }
  return r;
}
void HookPads() {
  void* detours[] = {(void*)&HkPadState<0>, (void*)&HkPadState<1>, (void*)&HkPadState<2>, (void*)&HkPadState<3>};
  for (int i = 0; i < 4; ++i) {
    PadHook& p = padHooks[i];
    if (p.done) continue;
    HMODULE m = GetModuleHandleA(p.module);
    if (!m) continue;
    p.done = true;
    void* target = (void*)GetProcAddress(m, "XInputGetState");
    if (target && MH_CreateHook(target, detours[i], (void**)&p.real) == MH_OK && MH_EnableHook(target) == MH_OK) LOG("panel: %s hooked", p.module);
  }
}

// Prism's own gamepad reads.
PadStateFn padRead = nullptr;

// The game window's messages: none of the mouse or keys reach the game while the panel is open.
HWND gameWnd = nullptr;
WNDPROC gameProc = nullptr;
std::atomic<int> wheel{0};
LRESULT CALLBACK HkWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_MOUSEWHEEL) wheel += GET_WHEEL_DELTA_WPARAM(w);
  if (blocking) {
    const bool altF4 = m == WM_SYSKEYDOWN && w == VK_F4;
    const bool release = m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP || m == WM_XBUTTONUP;
    if (!release && ((m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST && !altF4))) return 0;
    if (m == WM_INPUT) return DefWindowProcW(h, m, w, l);
  }
  return CallWindowProcW(gameProc, h, m, w, l);
}

void HookInput() {
  auto api = [](const wchar_t* mod, const char* name, void* detour, void** real) {
    if (MH_CreateHookApi(mod, name, detour, real) != MH_OK) { LOG("panel: %s not hooked", name); return; }
    void* target = (void*)GetProcAddress(GetModuleHandleW(mod), name);
    if (target) MH_EnableHook(target);
  };
  api(L"user32", "GetAsyncKeyState", (void*)&HkAsyncKey, (void**)&realAsyncKey);
  api(L"user32", "GetKeyState", (void*)&HkKeyState, (void**)&realKeyState);
  api(L"user32", "GetKeyboardState", (void*)&HkKeyboardState, (void**)&realKeyboardState);
  api(L"user32", "GetCursorPos", (void*)&HkGetCursorPos, (void**)&realGetCursorPos);
  api(L"user32", "SetCursorPos", (void*)&HkSetCursorPos, (void**)&realSetCursorPos);
  api(L"user32", "ClipCursor", (void*)&HkClipCursor, (void**)&realClipCursor);
  api(L"user32", "GetRawInputData", (void*)&HkRawData, (void**)&realRawData);
  api(L"user32", "GetRawInputBuffer", (void*)&HkRawBuffer, (void**)&realRawBuffer);
}

void SetBlocking(bool on) {
  if (on == blocking) return;
  if (on) {
    realGetCursorPos(&frozenCursor);
    RECT clip, desk;
    GetClipCursor(&clip);
    GetWindowRect(GetDesktopWindow(), &desk);
    gameClipped = clip.left > desk.left || clip.top > desk.top || clip.right < desk.right || clip.bottom < desk.bottom;
    gameClip = clip;
    blocking = true;
    realClipCursor(nullptr);
  } else {
    blocking = false;
    if (gameClipped) realClipCursor(&gameClip);
    realSetCursorPos(frozenCursor.x, frozenCursor.y);
  }
}

// ------------------------------------------------------------------ draw list
struct Vtx { float x, y, u, v; uint32_t c; };
std::vector<Vtx> verts;
struct Box { float x0, y0, x1, y1; };
Box clip = {-1e9f, -1e9f, 1e9f, 1e9f};

void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
  if ((c >> 24) == 0) return;
  const float cx0 = std::max(x0, clip.x0), cy0 = std::max(y0, clip.y0), cx1 = std::min(x1, clip.x1), cy1 = std::min(y1, clip.y1);
  if (cx0 >= cx1 || cy0 >= cy1) return;
  if (u0 >= 0.0f && (cx0 != x0 || cy0 != y0 || cx1 != x1 || cy1 != y1)) {
    const float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);
    u0 += (cx0 - x0) * du; u1 -= (x1 - cx1) * du; v0 += (cy0 - y0) * dv; v1 -= (y1 - cy1) * dv;
  }
  const Vtx a = {cx0, cy0, u0, v0, c}, b = {cx1, cy0, u1, v0, c}, d = {cx0, cy1, u0, v1, c}, e = {cx1, cy1, u1, v1, c};
  verts.insert(verts.end(), {a, b, d, b, e, d});
}
void Fill(float x0, float y0, float x1, float y1, uint32_t c) { Quad(x0, y0, x1, y1, -1, -1, -1, -1, c); }
void Frame(float x0, float y0, float x1, float y1, float t, uint32_t c) {
  Fill(x0, y0, x1, y0 + t, c); Fill(x0, y1 - t, x1, y1, c); Fill(x0, y0 + t, x0 + t, y1 - t, c); Fill(x1 - t, y0 + t, x1, y1 - t, c);
}
void Tri(float ax, float ay, float bx, float by, float cx, float cy, uint32_t c) {
  verts.insert(verts.end(), {Vtx{ax, ay, -1, -1, c}, Vtx{bx, by, -1, -1, c}, Vtx{cx, cy, -1, -1, c}});
}

// ------------------------------------------------------------------ glyphs
constexpr int kAtlas = 1024;
std::vector<uint8_t> atlas(size_t(kAtlas) * kAtlas);
bool atlasDirty = true;
int shelfX = 1, shelfY = 1, shelfH = 0;

IDWriteFactory* dw = nullptr;
// Faces: 0 mono regular, 1 mono bold, 2 Chinese regular, 3 Chinese bold.
struct Face { IDWriteFontFace* f = nullptr; float upem = 1, ascent = 0, descent = 0; } faces[4];
struct Glyph { int16_t x = 0, y = 0, w = 0, h = 0, left = 0, top = 0; float advance = 0; bool ok = false; };
std::unordered_map<uint64_t, Glyph> glyphs;
std::unordered_map<uint64_t, uint16_t> glyphIndex;  // (face, code point) -> glyph

IDWriteFontFace* SystemFace(const wchar_t* const* families, DWRITE_FONT_WEIGHT weight) {
  IDWriteFontCollection* col = nullptr;
  if (FAILED(dw->GetSystemFontCollection(&col, FALSE))) return nullptr;
  IDWriteFontFace* face = nullptr;
  for (const wchar_t* const* fam = families; *fam && !face; ++fam) {
    UINT32 i = 0; BOOL exists = FALSE;
    if (FAILED(col->FindFamilyName(*fam, &i, &exists)) || !exists) continue;
    IDWriteFontFamily* family = nullptr; IDWriteFont* font = nullptr;
    if (SUCCEEDED(col->GetFontFamily(i, &family)) && SUCCEEDED(family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
      font->CreateFontFace(&face);
    if (font) font->Release();
    if (family) family->Release();
  }
  col->Release();
  return face;
}

bool InitFonts() {
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), (IUnknown**)&dw))) return false;
  // JetBrains Mono from memory (Windows 10 1703+), Consolas otherwise.
  IDWriteFactory5* dw5 = nullptr;
  if (SUCCEEDED(dw->QueryInterface(__uuidof(IDWriteFactory5), (void**)&dw5))) {
    IDWriteInMemoryFontFileLoader* loader = nullptr;
    if (SUCCEEDED(dw5->CreateInMemoryFontFileLoader(&loader)) && SUCCEEDED(dw5->RegisterFontFileLoader(loader))) {
      const unsigned char* data[2] = {kMonoRegular, kMonoBold};
      const UINT32 size[2] = {sizeof kMonoRegular, sizeof kMonoBold};
      for (int i = 0; i < 2; ++i) {
        IDWriteFontFile* file = nullptr;
        if (SUCCEEDED(loader->CreateInMemoryFontFileReference(dw5, data[i], size[i], nullptr, &file))) {
          dw5->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &faces[i].f);
          file->Release();
        }
      }
    }
    dw5->Release();
  }
  static const wchar_t* mono[] = {L"Consolas", L"Courier New", nullptr};
  static const wchar_t* han[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimHei", L"Segoe UI", nullptr};
  if (!faces[0].f) faces[0].f = SystemFace(mono, DWRITE_FONT_WEIGHT_REGULAR);
  if (!faces[1].f) faces[1].f = SystemFace(mono, DWRITE_FONT_WEIGHT_BOLD);
  faces[2].f = SystemFace(han, DWRITE_FONT_WEIGHT_REGULAR);
  faces[3].f = SystemFace(han, DWRITE_FONT_WEIGHT_BOLD);
  for (Face& f : faces) {
    if (!f.f) return false;
    DWRITE_FONT_METRICS m;
    f.f->GetMetrics(&m);
    f.upem = m.designUnitsPerEm; f.ascent = m.ascent; f.descent = m.descent;
  }
  return true;
}

uint16_t GlyphOf(int face, uint32_t cp) {
  const uint64_t key = (uint64_t(face) << 32) | cp;
  auto it = glyphIndex.find(key);
  if (it != glyphIndex.end()) return it->second;
  UINT16 g = 0;
  faces[face].f->GetGlyphIndices(&cp, 1, &g);
  glyphIndex[key] = g;
  return g;
}

// The glyph rasterised at one of four sub-pixel x offsets, its coverage in the atlas.
const Glyph& Raster(int face, float px, uint16_t gi, int bucket) {
  const uint64_t key = (uint64_t(face) << 60) | (uint64_t(bucket) << 56) | (uint64_t(px * 8.0f + 0.5f) << 16) | gi;
  auto it = glyphs.find(key);
  if (it != glyphs.end()) return it->second;
  Glyph& g = glyphs[key];
  DWRITE_GLYPH_METRICS gm;
  faces[face].f->GetDesignGlyphMetrics(&gi, 1, &gm, FALSE);
  g.advance = gm.advanceWidth * px / faces[face].upem;
  const FLOAT adv = 0; const DWRITE_GLYPH_OFFSET off = {};
  DWRITE_GLYPH_RUN run = {faces[face].f, px, 1, &gi, &adv, &off, FALSE, 0};
  IDWriteGlyphRunAnalysis* an = nullptr;
  if (FAILED(dw->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL, bucket * 0.25f, 0.0f, &an))) return g;
  RECT b;
  an->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &b);
  const int w = b.right - b.left, h = b.bottom - b.top;
  if (w > 0 && h > 0 && w < 256 && h < 256) {
    if (shelfX + w + 1 > kAtlas) { shelfX = 1; shelfY += shelfH + 1; shelfH = 0; }
    if (shelfY + h + 1 > kAtlas) {  // full: start over (the next frames re-rasterise what they use)
      glyphs.clear(); std::fill(atlas.begin(), atlas.end(), 0); shelfX = shelfY = 1; shelfH = 0; atlasDirty = true;
      an->Release();
      return Raster(face, px, gi, bucket);
    }
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    an->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &b, rgb.data(), UINT32(rgb.size()));
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const uint8_t* s = &rgb[(size_t(y) * w + x) * 3];
        const float a = (s[0] + s[1] + s[2]) / (3.0f * 255.0f);
        // A little contrast for light ink on a dark field (thin strokes otherwise read grey).
        atlas[size_t(shelfY + y) * kAtlas + shelfX + x] = uint8_t(std::min(255.0f, std::pow(a, 0.82f) * 255.0f + 0.5f));
      }
    g.x = int16_t(shelfX); g.y = int16_t(shelfY); g.w = int16_t(w); g.h = int16_t(h);
    g.left = int16_t(b.left); g.top = int16_t(b.top); g.ok = true;
    shelfX += w + 1; shelfH = std::max(shelfH, h);
    atlasDirty = true;
  }
  an->Release();
  return g;
}

// Text styles, in panel units (scaled to pixels at draw time).
enum Style { kTitle, kBody, kLabel, kValue, kValueLarge, kSmall, kStyles };
struct StyleDef { float size; bool bold; } styles[kStyles] = {{15, true}, {13, false}, {11, false}, {14, false}, {22, true}, {12, false}};
float ui = 1.0f;  // pixels per panel unit

int FaceFor(uint32_t cp, bool bold, uint16_t* gi) {
  const int mono = bold ? 1 : 0;
  if (cp < 0x2e80 && (*gi = GlyphOf(mono, cp)) != 0) return mono;  // Latin, digits, punctuation: the mono face
  *gi = GlyphOf(mono + 2, cp);
  return mono + 2;
}

float LineHeight(Style s) { return std::round(styles[s].size * 1.45f * ui); }

// Draws (or only measures) a line; y is the top of its line box. Returns the advance in pixels.
float Text(float x, float y, const std::wstring& s, Style st, uint32_t c, bool draw = true) {
  const float px = styles[st].size * ui;
  const Face& han = faces[styles[st].bold ? 3 : 2];
  const float asc = han.ascent * px / han.upem, desc = han.descent * px / han.upem;
  const float baseline = std::round(y + (LineHeight(st) - (asc + desc)) * 0.5f + asc);
  float pen = x;
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t cp = s[i];
    if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < s.size()) { cp = 0x10000 + ((cp - 0xd800) << 10) + (s[i + 1] - 0xdc00); ++i; }
    uint16_t gi = 0;
    const int face = FaceFor(cp, styles[st].bold, &gi);
    const float ix = std::floor(pen);
    const int bucket = std::min(3, int((pen - ix) * 4.0f));
    const Glyph& g = Raster(face, px, gi, bucket);
    if (draw && g.ok) {
      const float gx = ix + g.left, gy = baseline + g.top;
      Quad(gx, gy, gx + g.w, gy + g.h, float(g.x) / kAtlas, float(g.y) / kAtlas, float(g.x + g.w) / kAtlas, float(g.y + g.h) / kAtlas, c);
    }
    pen += g.advance;
  }
  return pen - x;
}
float Measure(const std::wstring& s, Style st) { return Text(0, 0, s, st, 0, false); }
float TextRight(float right, float y, const std::wstring& s, Style st, uint32_t c) { const float w = Measure(s, st); Text(right - w, y, s, st, c); return w; }

// Wraps a paragraph into lines no wider than `width`; Chinese breaks anywhere, Latin at spaces.
std::vector<std::wstring> Wrap(const std::wstring& s, Style st, float width) {
  std::vector<std::wstring> lines;
  std::wstring line;
  size_t lastSpace = std::wstring::npos;
  for (wchar_t ch : s) {
    line += ch;
    if (ch == L' ') lastSpace = line.size() - 1;
    if (Measure(line, st) > width && line.size() > 1) {
      if (ch == L' ') { line.pop_back(); lines.push_back(line); line.clear(); lastSpace = std::wstring::npos; continue; }
      if (ch < 0x2e80 && lastSpace != std::wstring::npos && ch != L' ') {
        lines.push_back(line.substr(0, lastSpace));
        line = line.substr(lastSpace + 1);
      } else {
        // Keep closing punctuation with the line before.
        const bool closing = wcschr(L"，。、；：！？）」』", ch) != nullptr;
        if (closing) { lines.push_back(line); line.clear(); }
        else { lines.push_back(line.substr(0, line.size() - 1)); line = std::wstring(1, ch); }
      }
      lastSpace = std::wstring::npos;
    }
  }
  if (!line.empty()) lines.push_back(line);
  for (std::wstring& l : lines) { while (!l.empty() && l.front() == L' ') l.erase(0, 1); while (!l.empty() && l.back() == L' ') l.pop_back(); }
  return lines;
}

// ------------------------------------------------------------------ GPU
const char* kHlsl = R"(
cbuffer K : register(b0) { float2 InvView; uint Mode; float Nits; };
Texture2D<float> Atlas : register(t0);
SamplerState Point : register(s0);
struct V { float2 p : POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };
struct P { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };
P VS(V v) {
  P o;
  o.p = float4(v.p * InvView * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  o.uv = v.uv; o.c = v.c;
  return o;
}
float3 Lin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 Pq(float3 l) {
  const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  const float3 p = pow(max(l, 0.0), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
// Mode 0: an SDR back buffer (sRGB-encoded values). 1: scRGB (linear, 1.0 = 80 nits). 2: HDR10 (PQ, BT.2020).
float4 PS(P i) : SV_Target {
  float a = i.c.a;
  if (i.uv.x >= 0.0) {
    // Glyph coverage is tuned for blending in sRGB. Blending in linear light (scRGB) makes light ink
    // heavier and dark ink lighter; in PQ (HDR10) the other way round. Reshape the coverage by the
    // ink's lightness so a half-covered pixel lands where it would in SDR.
    float cov = Atlas.SampleLevel(Point, i.uv, 0);
    const float ink = dot(i.c.rgb, float3(0.2126, 0.7152, 0.0722));
    if (Mode == 1u) cov = pow(cov, lerp(0.35, 2.2, ink));
    else if (Mode == 2u) cov = pow(cov, lerp(2.0, 0.45, ink));
    a *= cov;
  }
  float3 c = i.c.rgb;
  if (Mode == 1u) {
    c = Lin(c) * (Nits / 80.0);
  } else if (Mode == 2u) {
    const float3 l = Lin(c);
    const float3 w = float3(dot(l, float3(0.6274, 0.3293, 0.0433)), dot(l, float3(0.0691, 0.9195, 0.0114)), dot(l, float3(0.0164, 0.0880, 0.8956)));
    c = Pq(w * (Nits / 10000.0));
  }
  return float4(c * a, a);
}
)";

constexpr UINT kSlots = 3;
struct Slot { ID3D12CommandAllocator* alloc = nullptr; ID3D12Resource* vb = nullptr; size_t vbSize = 0; void* vbMap = nullptr; ID3D12Resource* up = nullptr; uint64_t fence = 0; };
struct Gpu {
  ID3D12Device* device = nullptr;
  ID3D12RootSignature* root = nullptr;
  ID3DBlob *vs = nullptr, *ps = nullptr;
  std::unordered_map<int, ID3D12PipelineState*> pso;  // per render target format
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  uint64_t fenceValue = 0;
  HANDLE event = nullptr;
  Slot slots[kSlots];
  UINT slot = 0;
  ID3D12Resource* atlasTex = nullptr;
  bool atlasReadable = false;
  ID3D12DescriptorHeap *srvHeap = nullptr, *rtvHeap = nullptr;
  bool ok = false, failed = false;
} gpu;

bool GpuFail(const char* what, HRESULT hr) { LOG("panel off: %s (0x%08lx)", what, hr); gpu.failed = true; return false; }

bool InitGpu(ID3D12Device* d) {
  HRESULT hr;
  gpu.device = d;
  d->AddRef();
  using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
  auto compile = (CompileFn)(void*)GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile");
  if (!compile) return GpuFail("no d3dcompiler_47", E_FAIL);
  ID3DBlob* err = nullptr;
  if (FAILED(hr = compile(kHlsl, std::strlen(kHlsl), "panel", nullptr, nullptr, "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &gpu.vs, &err)) ||
      FAILED(hr = compile(kHlsl, std::strlen(kHlsl), "panel", nullptr, nullptr, "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &gpu.ps, &err))) {
    LOG("panel shader: %s", err ? (const char*)err->GetBufferPointer() : "?");
    return GpuFail("shader", hr);
  }
  D3D12_DESCRIPTOR_RANGE range = {}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER rp[2] = {};
  rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 4;
  rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = {1, &range}; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC ss = {};
  ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rd = {2, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob* blob = nullptr;
  if (FAILED(hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
      FAILED(hr = d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&gpu.root)))
    return GpuFail("root signature", hr);
  for (Slot& s : gpu.slots)
    if (FAILED(hr = d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&s.alloc))) return GpuFail("allocator", hr);
  if (FAILED(hr = d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.slots[0].alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&gpu.list))) return GpuFail("list", hr);
  gpu.list->Close();
  if (FAILED(hr = d->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&gpu.fence))) return GpuFail("fence", hr);
  gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = kAtlas; td.Height = kAtlas; td.DepthOrArraySize = 1; td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
  if (FAILED(hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&gpu.atlasTex))) return GpuFail("atlas", hr);
  D3D12_DESCRIPTOR_HEAP_DESC sh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (FAILED(hr = d->CreateDescriptorHeap(&sh, __uuidof(ID3D12DescriptorHeap), (void**)&gpu.srvHeap)) ||
      FAILED(hr = d->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void**)&gpu.rtvHeap))) return GpuFail("descriptor heaps", hr);
  d->CreateShaderResourceView(gpu.atlasTex, nullptr, gpu.srvHeap->GetCPUDescriptorHandleForHeapStart());
  gpu.ok = true;
  return true;
}

ID3D12PipelineState* PsoFor(DXGI_FORMAT f) {
  auto it = gpu.pso.find(int(f));
  if (it != gpu.pso.end()) return it->second;
  const D3D12_INPUT_ELEMENT_DESC il[] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                         {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                         {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = gpu.root;
  pd.VS = {gpu.vs->GetBufferPointer(), gpu.vs->GetBufferSize()};
  pd.PS = {gpu.ps->GetBufferPointer(), gpu.ps->GetBufferSize()};
  D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
  b.BlendEnable = TRUE;
  b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
  b.SrcBlendAlpha = D3D12_BLEND_ZERO; b.DestBlendAlpha = D3D12_BLEND_ONE; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  b.LogicOp = D3D12_LOGIC_OP_NOOP;
  b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
  pd.InputLayout = {il, 3};
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1; pd.RTVFormats[0] = f; pd.SampleDesc.Count = 1;
  ID3D12PipelineState* p = nullptr;
  const HRESULT hr = gpu.device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&p);
  if (FAILED(hr)) LOG("panel: no pipeline for back buffer format %d (0x%08lx)", int(f), hr);
  gpu.pso[int(f)] = p;
  return p;
}

// ------------------------------------------------------------------ the swap chain
struct Chain {
  IDXGISwapChain* sc = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  bool unsupported = false;
} chain;
std::unordered_map<IDXGISwapChain*, Chain> chains;  // every swap chain seen: its queue, or unsupported (D3D11, another device)
std::mutex colourMutex;
std::unordered_map<IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE> colourSpaces;
using SetColourSpaceFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
SetColourSpaceFn realSetColourSpace = nullptr;
HRESULT STDMETHODCALLTYPE HkSetColourSpace(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs) {
  const HRESULT hr = realSetColourSpace(sc, cs);
  if (SUCCEEDED(hr)) { std::lock_guard<std::mutex> lock(colourMutex); colourSpaces[sc] = cs; }
  return hr;
}
std::wstring gpuName;

// The game's direct queues, as they execute (no references held: only compared with pointers).
std::mutex queueMutex;
std::vector<ID3D12CommandQueue*> directQueues;

// The queue a swap chain presents with: DXGI keeps it inside the swap chain object, at an offset
// that changes between Windows builds, so look for one of the game's direct queues in there.
ID3D12CommandQueue* FindQueue(IDXGISwapChain* sc) {
  std::vector<ID3D12CommandQueue*> known;
  { std::lock_guard<std::mutex> lock(queueMutex); known = directQueues; }
  if (known.empty()) return nullptr;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(sc, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return nullptr;
  const uintptr_t end = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
  const uintptr_t* p = reinterpret_cast<const uintptr_t*>(sc);
  for (int i = 0; i < 1024 && uintptr_t(p + i + 1) <= end; ++i)
    for (ID3D12CommandQueue* q : known)
      if (p[i] == uintptr_t(q)) return q;
  return nullptr;
}

bool Bind(IDXGISwapChain* sc) {
  auto known = chains.find(sc);
  if (known != chains.end() && (known->second.queue || known->second.unsupported)) { chain = known->second; return !chain.unsupported; }
  chain = Chain{};
  chain.sc = sc;
  ID3D12Device* d = nullptr;
  if (FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&d)) || !d) {  // D3D11 or another API: not ours
    chain.unsupported = true; chains[sc] = chain;
    LOG("panel: swap chain %p is not D3D12: not drawing on it", (void*)sc);
    return false;
  }
  ID3D12CommandQueue* q = FindQueue(sc);
  if (!q) { d->Release(); chains[sc] = chain; return false; }  // its queue has not executed yet: try again
  q->AddRef();
  if (!gpu.ok && !gpu.failed) {
    InitGpu(d);
    IDXGIFactory4* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&f))) {
      IDXGIAdapter1* a = nullptr;
      if (SUCCEEDED(f->EnumAdapterByLuid(d->GetAdapterLuid(), __uuidof(IDXGIAdapter1), (void**)&a))) {
        DXGI_ADAPTER_DESC1 ad; a->GetDesc1(&ad); gpuName = ad.Description; a->Release();
      }
      f->Release();
    }
  }
  const bool same = d == gpu.device;
  d->Release();
  if (!gpu.ok || !same) { q->Release(); chain.unsupported = true; chains[sc] = chain; return false; }
  chain.queue = q;
  chain.unsupported = false;
  chains[sc] = chain;
  DXGI_SWAP_CHAIN_DESC desc;
  if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow && desc.OutputWindow != gameWnd) {
    gameWnd = desc.OutputWindow;
    gameProc = (WNDPROC)SetWindowLongPtrW(gameWnd, GWLP_WNDPROC, (LONG_PTR)&HkWndProc);
  }
  LOG("panel: drawing on the swap chain's queue (%ls)", gpuName.c_str());
  return true;
}

int ColourMode(IDXGISwapChain* sc, DXGI_FORMAT f) {
  if (cfg.hdr >= 0) return cfg.hdr;
  if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) return 1;
  if (f != DXGI_FORMAT_R10G10B10A2_UNORM) return 0;
  {
    std::lock_guard<std::mutex> lock(colourMutex);
    auto it = colourSpaces.find(sc);
    if (it != colourSpaces.end()) return it->second == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2 : 0;
  }
  // The colour space was set before Prism loaded: a 10-bit swap chain on an HDR display is HDR10.
  static int byOutput = -1;
  if (byOutput < 0) {
    byOutput = 0;
    IDXGIOutput* o = nullptr;
    if (SUCCEEDED(sc->GetContainingOutput(&o))) {
      IDXGIOutput6* o6 = nullptr;
      if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6))) {
        DXGI_OUTPUT_DESC1 od; o6->GetDesc1(&od);
        byOutput = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2 : 0;
        o6->Release();
      }
      o->Release();
    }
  }
  return byOutput;
}

bool Submit(IDXGISwapChain* sc, UINT bw, UINT bh, DXGI_FORMAT fmt) {
  IDXGISwapChain3* sc3 = nullptr;
  if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) return false;
  const UINT index = sc3->GetCurrentBackBufferIndex();
  ID3D12Resource* bb = nullptr;
  const HRESULT hr = sc3->GetBuffer(index, __uuidof(ID3D12Resource), (void**)&bb);
  const int mode = ColourMode(sc, fmt);
  sc3->Release();
  if (FAILED(hr) || !bb) return false;
  ID3D12PipelineState* pso = PsoFor(fmt);
  if (!pso) { bb->Release(); return false; }

  Slot& s = gpu.slots[gpu.slot];
  gpu.slot = (gpu.slot + 1) % kSlots;
  if (s.fence && gpu.fence->GetCompletedValue() < s.fence) {
    gpu.fence->SetEventOnCompletion(s.fence, gpu.event);
    WaitForSingleObject(gpu.event, 200);
  }
  const size_t need = std::max<size_t>(verts.size() * sizeof(Vtx), 64 * 1024);
  D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
  auto buffer = [&](size_t bytes) -> ID3D12Resource* {
    D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    return SUCCEEDED(gpu.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&r)) ? r : nullptr;
  };
  if (s.vbSize < need) {
    if (s.vb) s.vb->Release();
    s.vbSize = need * 2;
    s.vb = buffer(s.vbSize);
    s.vbMap = nullptr;
    if (!s.vb || FAILED(s.vb->Map(0, nullptr, &s.vbMap))) { s.vb = nullptr; s.vbSize = 0; bb->Release(); return false; }
  }
  std::memcpy(s.vbMap, verts.data(), verts.size() * sizeof(Vtx));

  s.alloc->Reset();
  ID3D12GraphicsCommandList* l = gpu.list;
  l->Reset(s.alloc, pso);
  auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    l->ResourceBarrier(1, &br);
  };
  if (atlasDirty) {
    // The whole atlas through this slot's upload buffer (only when new glyphs appeared).
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowBytes, total;
    const D3D12_RESOURCE_DESC td = gpu.atlasTex->GetDesc();
    gpu.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    if (!s.up) s.up = buffer(total);
    uint8_t* m = nullptr;
    if (s.up && SUCCEEDED(s.up->Map(0, nullptr, (void**)&m))) {
      for (UINT y = 0; y < rows; ++y) std::memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, &atlas[size_t(y) * kAtlas], kAtlas);
      s.up->Unmap(0, nullptr);
      if (gpu.atlasReadable) barrier(gpu.atlasTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
      dst.pResource = gpu.atlasTex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.pResource = s.up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
      l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      barrier(gpu.atlasTex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      gpu.atlasReadable = true;
      atlasDirty = false;
    }
  }
  barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = fmt; rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  gpu.device->CreateRenderTargetView(bb, &rd, rtv);
  l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp = {0, 0, float(bw), float(bh), 0, 1};
  const D3D12_RECT sr = {0, 0, LONG(bw), LONG(bh)};
  l->RSSetViewports(1, &vp);
  l->RSSetScissorRects(1, &sr);
  l->SetGraphicsRootSignature(gpu.root);
  ID3D12DescriptorHeap* heaps[] = {gpu.srvHeap};
  l->SetDescriptorHeaps(1, heaps);
  struct { float ix, iy; uint32_t mode; float nits; } k = {1.0f / bw, 1.0f / bh, uint32_t(mode), cfg.nits};
  l->SetGraphicsRoot32BitConstants(0, 4, &k, 0);
  l->SetGraphicsRootDescriptorTable(1, gpu.srvHeap->GetGPUDescriptorHandleForHeapStart());
  l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VERTEX_BUFFER_VIEW vbv = {s.vb->GetGPUVirtualAddress(), UINT(verts.size() * sizeof(Vtx)), sizeof(Vtx)};
  l->IASetVertexBuffers(0, 1, &vbv);
  if (gpu.atlasReadable) l->DrawInstanced(UINT(verts.size()), 1, 0, 0);
  barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  l->Close();
  ID3D12CommandList* lists[] = {l};
  chain.queue->ExecuteCommandLists(1, lists);
  s.fence = ++gpu.fenceValue;
  chain.queue->Signal(gpu.fence, s.fence);
  bb->Release();
  return true;
}

// ------------------------------------------------------------------ input for the panel
struct Input {
  float mx = -1, my = -1;     // cursor in back buffer pixels
  bool mouseMoved = false, down = false, pressed = false, released = false;
  int wheelSteps = 0;
  bool up = false, dn = false, left = false, right = false, enter = false, back = false, toggle = false;
  bool pad = false;           // the last navigation came from a gamepad
  bool padConnected = false;
  WORD padHeld = 0;           // gamepad buttons down now
} in;

struct Repeat { bool was = false; double next = 0; };
bool Rep(bool down, Repeat& r, double now) {
  if (down && !r.was) { r.was = true; r.next = now + 0.38; return true; }
  if (down && now >= r.next) { r.next = now + 0.055; return true; }
  r.was = down;
  return false;
}
bool Key(int vk) { return (realAsyncKey(vk) & 0x8000) != 0; }

bool Foreground() {
  HWND f = GetForegroundWindow();
  if (!f) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(f, &pid);
  return pid == GetCurrentProcessId();
}

void PollInput(UINT bw, UINT bh, bool open) {
  const double now = Now();
  const bool fg = Foreground();
  static bool toggleWas = false, lWas = false;
  static POINT lastCursor = {};
  static Repeat rUp, rDn, rLeft, rRight;
  static WORD padWas = 0;
  static Repeat pUp, pDn, pLeft, pRight;
  in.mouseMoved = in.pressed = in.released = false;
  in.up = in.dn = in.left = in.right = in.enter = in.back = in.toggle = false;
  in.wheelSteps = 0;
  const int wh = wheel.exchange(0);
  if (!fg) { toggleWas = Key(cfg.key); lWas = false; in.down = false; return; }
  const bool t = cfg.key && Key(cfg.key);
  if (t && !toggleWas) in.toggle = true;
  toggleWas = t;
  // The cursor, in back buffer pixels (the window's client area may be scaled to it).
  POINT p;
  if (gameWnd && realGetCursorPos(&p)) {
    if (p.x != lastCursor.x || p.y != lastCursor.y) in.mouseMoved = true;
    lastCursor = p;
    ScreenToClient(gameWnd, &p);
    RECT cr; GetClientRect(gameWnd, &cr);
    const float cw = float(std::max<LONG>(1, cr.right - cr.left)), ch = float(std::max<LONG>(1, cr.bottom - cr.top));
    in.mx = float(p.x) * bw / cw; in.my = float(p.y) * bh / ch;
  }
  const bool l = Key(VK_LBUTTON);
  in.pressed = l && !lWas; in.released = !l && lWas; in.down = l;
  lWas = l;
  in.wheelSteps = wh / WHEEL_DELTA;
  if (in.mouseMoved || in.pressed) in.pad = false;
  if (open) {
    in.up = Rep(Key(VK_UP), rUp, now); in.dn = Rep(Key(VK_DOWN), rDn, now);
    in.left = Rep(Key(VK_LEFT), rLeft, now); in.right = Rep(Key(VK_RIGHT), rRight, now);
    static bool enterWas = false, escWas = false;
    const bool e = Key(VK_RETURN) || Key(VK_SPACE), x = Key(VK_ESCAPE);
    in.enter = e && !enterWas; in.back = x && !escWas;
    enterWas = e; escWas = x;
    if (in.up || in.dn || in.left || in.right || in.enter) in.pad = false;
  }
  // The gamepad: View + Y opens and closes; the d-pad or left stick moves, A changes, B closes.
  in.padConnected = false;
  in.padHeld = 0;
  if (cfg.pad && padRead) {
    static double nextProbe = 0;
    static int pad = -1;
    XINPUT_STATE st = {};
    if (pad >= 0 && padRead(DWORD(pad), &st) != ERROR_SUCCESS) pad = -1;
    if (pad < 0 && now >= nextProbe) {
      nextProbe = now + 2.0;  // probing absent pads is slow: once in a while
      for (int i = 0; i < 4 && pad < 0; ++i) if (padRead(DWORD(i), &st) == ERROR_SUCCESS) pad = i;
    }
    if (pad >= 0) {
      in.padConnected = true;
      const WORD b = st.Gamepad.wButtons, pressed = WORD(b & ~padWas);
      padWas = b;
      in.padHeld = b;
      if ((b & XINPUT_GAMEPAD_BACK) && (pressed & XINPUT_GAMEPAD_Y)) { in.toggle = true; in.pad = true; }
      if (open) {
        const SHORT ly = st.Gamepad.sThumbLY, lx = st.Gamepad.sThumbLX;
        const bool u = (b & XINPUT_GAMEPAD_DPAD_UP) || ly > 20000, d = (b & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -20000;
        const bool le = (b & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -20000, ri = (b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > 20000;
        const bool pu = Rep(u, pUp, now), pd = Rep(d, pDn, now), pl = Rep(le, pLeft, now), pr = Rep(ri, pRight, now);
        if (pu || pd || pl || pr || (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B))) in.pad = true;
        in.up |= pu; in.dn |= pd; in.left |= pl; in.right |= pr;
        in.enter |= (pressed & XINPUT_GAMEPAD_A) != 0;
        in.back |= (pressed & XINPUT_GAMEPAD_B) != 0;
      }
    }
  }
}

// ------------------------------------------------------------------ tiers (electron/shared/catalog.ts)
struct Tier { const wchar_t* name; int cadence, passes; bool async; float intensity, guard, netScale; const wchar_t* help; bool needsHigh; };  // needsHigh: RTX 5080 / 5090 only
const Tier kTiers[] = {
    {L"极致", 1, 2, false, 2.40f, 1.0f, 0.7655f, L"每帧 · 2 遍 · 分辨率 150% · 240%。比旗舰多算一半像素，细节最足，开销最大，高速运动没有拖影。", true},
    {L"旗舰", 1, 2, false, 2.40f, 1.0f, 0.625f, L"每帧 · 2 遍 · 分辨率 100% · 240%。RTX 5080 / 5090 的默认档位，帧率和细节最均衡，高速运动没有拖影。", false},
    {L"高画质", 2, 2, true, 2.00f, 1.0f, 1.00f, L"每 2 帧 · 2 遍 · 200%。快速运动的物体旁可能有轻微残影。", false},
    {L"标准", 2, 1, true, 1.50f, 1.0f, 1.00f, L"每 2 帧 · 1 遍 · 150%。", false},
    {L"流畅", 4, 1, true, 1.20f, 1.0f, 1.00f, L"每 4 帧 · 1 遍 · 120%。开销最小。", false},
};
constexpr int kTierCount = 5;
bool Near(float a, float b) { return std::fabs(a - b) < 0.005f; }
int TierOf(const LiveSettings& s) {
  for (int i = 0; i < kTierCount; ++i) {
    const Tier& t = kTiers[i];
    if (s.cadence == t.cadence && s.passes == t.passes && s.async == t.async && Near(s.intensity, t.intensity) && Near(s.guard, t.guard) &&
        Near(t.cadence == 1 ? s.netScale : 1.0f, t.netScale))
      return i;
  }
  return -1;
}
bool TierLocked(int i) { return kTiers[i].needsHigh && gpuName.find(L"5090") == std::wstring::npos && gpuName.find(L"5080") == std::wstring::npos; }

// ------------------------------------------------------------------ the panel
enum Row { kOn, kTier, kLook, kIntensity, kPasses, kCadence, kNetScale, kTarget, kGuard, kSplit, kHud, kRows };
struct Panel {
  bool open = false;
  double openedAt = 0, closedAt = -10;
  int focus = kTier;
  int hoverOption = -1;       // the option under the cursor in the focused row, for its help
  enum Drag { kNone, kMove, kSlide, kSplitLine } drag = kNone;
  int dragRow = -1;
  float dragDX = 0, dragDY = 0;
  float seg[kRows][2] = {};   // the inverted block's animated left and right, per segmented row
  bool segInit[kRows] = {};
  double changedAt = -1, savedAt = -10;
  std::wstring toast;
  uint32_t toastMark = kReady;
  double toastAt = -10, toastFor = 1.8;
  bool hinted = false;
  double startedAt = 0;
  float graphTop = 20.0f;      // the frame-time graph's scale (ms), eased
  float px0 = 0, py0 = 0;      // the panel's pixel origin this frame
} panel;

float EaseOutExpo(float t) { return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }
float U(float v) { return std::round(v * ui); }  // panel units to whole pixels
float Hair() { return std::max(1.0f, std::floor(ui)); }

void Toast(const std::wstring& s, uint32_t mark = kReady, double seconds = 1.8) {
  panel.toast = s; panel.toastMark = mark; panel.toastAt = Now(); panel.toastFor = seconds;
}

void Changed(const LiveSettings& s) {
  SetLiveSettings(s);
  panel.changedAt = Now();
}

std::wstring Fmt(const wchar_t* f, double v) { wchar_t b[32]; std::swprintf(b, 32, f, v); return b; }

// A bar run for a value from 0 to `max`: one 2px bar per step, filled up to the value, with a taller
// bar at each reference value and a 2px caution mark under the playground range.
struct Bars { float x0, y0, x1, y1; };
Bars BarSlider(float x0, float y, float x1, float h, float value, float max, float ref, float playground, bool enabled, bool hot) {
  const float bw = std::max(2.0f, std::round(2.0f * ui)), gap = std::max(1.0f, std::round(2.0f * ui));
  const int n = int((x1 - x0 + gap) / (bw + gap));
  const float span = n * (bw + gap) - gap;
  const float top = y, bottom = y + h;
  for (int i = 0; i < n; ++i) {
    const float v = (i + 0.5f) / n * max;
    const float bx = x0 + i * (bw + gap);
    const bool filled = v <= value + 1e-4f;
    const bool isRef = ref > 0 && std::fabs(v - ref) < max / n * 0.5f;
    const float bt = isRef ? top - U(3) : top + U(3);
    uint32_t c = filled ? (enabled ? (hot ? kWhite : Rgba(0xe6e6e6)) : kQuiet) : (isRef ? kQuiet : kHair);
    Fill(bx, bt, bx + bw, bottom, c);
    if (playground > 0 && v >= playground) Fill(bx, bottom + U(3), bx + bw + (i + 1 < n ? gap : 0), bottom + U(3) + std::max(2.0f, U(2)), enabled ? kCaution : kQuiet);
  }
  return {x0, top, x0 + span, bottom};
}

// Segmented options; the chosen one is inverted, the block slides between options.
// Returns the option clicked (or -1); `hover` gets the option under the cursor.
// An off switch's "off" (quietOff) is a #262626 block: the panel's white goes to what is on.
int Segments(int row, float x0, float y, float x1, float h, const std::vector<std::wstring>& opts, int chosen, uint32_t enabledMask,
             uint32_t playgroundMask, int* hover, float dt, bool quietOff = false) {
  const int n = int(opts.size());
  const float w = (x1 - x0) / n;
  const float t = Hair();
  int clicked = -1;
  *hover = -1;
  // The block: eased toward the chosen option.
  float& a = panel.seg[row][0];
  float& b = panel.seg[row][1];
  const float ta = chosen >= 0 ? std::round(x0 + chosen * w) : x0, tb = chosen >= 0 ? std::round(x0 + (chosen + 1) * w) : x0;
  if (!panel.segInit[row] || !cfg.motion) { a = ta; b = tb; panel.segInit[row] = true; }
  const float k = 1.0f - std::exp(-dt / 0.045f);
  a += (ta - a) * k; b += (tb - b) * k;
  if (std::fabs(a - ta) < 0.5f) a = ta;
  if (std::fabs(b - tb) < 0.5f) b = tb;
  Frame(x0, y, x1, y + h, t, kHair);
  for (int i = 1; i < n; ++i) { const float sx = std::round(x0 + i * w); Fill(sx, y + t, sx + t, y + h - t, kHair); }
  const bool quiet = quietOff && chosen == 0;
  if (chosen >= 0 && b > a) Fill(a, y, b, y + h, quiet ? kHair : kWhite);
  for (int i = 0; i < n; ++i) {
    const float sx0 = std::round(x0 + i * w), sx1 = std::round(x0 + (i + 1) * w);
    const bool en = (enabledMask >> i) & 1;
    const bool over = in.mx >= sx0 && in.mx < sx1 && in.my >= y && in.my < y + h && !in.pad;
    if (over) *hover = i;
    // Ink: black where the block covers the label's centre, else white, grey or quiet.
    const float cx = (sx0 + sx1) * 0.5f;
    const bool inverted = chosen >= 0 && cx >= a && cx < b;
    uint32_t ink = inverted ? (quiet ? kWhite : kBlack) : !en ? kQuiet : over ? kWhite : kReadout;
    const float tw = Measure(opts[i], kBody);
    Text(std::round(cx - tw * 0.5f), y + std::round((h - LineHeight(kBody)) * 0.5f), opts[i], kBody, ink);
    if ((playgroundMask >> i) & 1) Fill(sx0 + U(6), y + h + U(3), sx1 - U(6), y + h + U(3) + std::max(2.0f, U(2)), en ? kCaution : kQuiet);
    if (over && in.pressed) clicked = i;
  }
  return clicked;
}

// The arrow cursor (the game usually hides the system one), drawn as triangles.
void Cursor(float x, float y) {
  const float s = std::max(1.0f, ui * 0.9f);
  auto arrow = [&](float g, uint32_t c) {
    const float pts[7][2] = {{0, 0}, {0, 16}, {4, 12.5f}, {7, 19}, {9.5f, 18}, {6.5f, 11.5f}, {11.5f, 11.5f}};
    auto P = [&](int i, float* px, float* py) {
      const float cx = 3.5f, cy = 11.0f;  // grow around the arrow's middle for the outline
      *px = x + (cx + (pts[i][0] - cx) * g) * s; *py = y + (cy + (pts[i][1] - cy) * g) * s;
    };
    float ax, ay, bx, by, cx2, cy2;
    const int tris[5][3] = {{0, 1, 2}, {0, 2, 5}, {0, 5, 6}, {2, 3, 4}, {2, 4, 5}};
    for (auto& t : tris) { P(t[0], &ax, &ay); P(t[1], &bx, &by); P(t[2], &cx2, &cy2); Tri(ax, ay, bx, by, cx2, cy2, c); }
  };
  arrow(1.18f, kBlack);
  arrow(1.0f, kWhite);
}

const wchar_t* RowLabel(int r) {
  static const wchar_t* labels[kRows] = {L"神经渲染", L"档位", L"色彩", L"强度", L"遍数", L"节奏", L"网络分辨率", L"目标帧率", L"角色守护", L"分屏对比", L"帧率角标"};
  return labels[r];
}

std::wstring RowHelp(int r, const LiveSettings& s, int option) {
  switch (r) {
    case kOn: return L"关闭后画面就是游戏原画，Prism 不占用 GPU。快捷键 F8。";
    case kTier: {
      const int i = option >= 0 ? option : TierOf(s);
      if (i < 0) return L"当前参数不属于任何档位。选一个档位会同时改掉下面的强度、遍数、节奏和网络分辨率。";
      if (TierLocked(i)) return L"极致档只在 RTX 5080 / 5090 上开放：网络每帧跑两遍、分辨率 150%，其他显卡的帧率会掉得太多。可以用旗舰档。";
      return kTiers[i].help;
    }
    case kLook: {
      static const wchar_t* looks[5] = {
          L"网络原样的色彩。",
          L"保留原画的颜色，只取网络的光影和质感；强改动被柔和压住，皮肤上更克制。AI 味最低，适合原画党。",
          L"颜色更鲜更干净：越灰的颜色加得越多，灰和白保持中性，皮肤只跟一小部分。适合二次元和卡通风格。",
          L"收住颜色，白色微冷，暗部微青，像美剧和电影的质感，皮肤保留自然血色。适合科技和现代题材。",
          L"深对比加浓郁色彩，暗部偏青、高光微暖。真正的重度渲染。"};
      return looks[std::clamp(option >= 0 ? option : s.look, 0, 4)];
    }
    case kIntensity: return L"网络改动的放大倍数，100% 是网络原样输出。250% 以上是游乐场，画面容易过度。";
    case kPasses: return L"网络在自己的结果上再跑几遍，效果更强，开销按遍数成倍增加。3 遍以上是游乐场。";
    case kCadence: return L"每帧都跑网络，高速运动没有拖影；隔帧运行更省，快速运动的物体旁可能有残影。快捷键 F2。";
    case kNetScale:
      if (s.cadence != 1) return L"只在网络每帧运行时生效。先把节奏改成每帧。";
      return (option >= 0 ? option : s.netScale > 0.7f ? 1 : 0) == 1 ? L"比旗舰基准多算一半像素，细节更足，开销更高。改动按原生画面的边缘放大回来。"
                                                                     : L"旗舰的基准分辨率：网络算的像素少，改动按原生画面的边缘放大回来，帧率和细节最均衡。";
    case kTarget: {
      if (s.cadence != 1) return L"只在网络每帧运行时生效。先把节奏改成每帧。";
      static const int targets[5] = {0, 40, 50, 60, 90};
      const int t = option >= 0 ? targets[std::clamp(option, 0, 4)] : s.netAuto ? s.fpsTarget : 0;
      if (t == 0) return L"关闭时网络始终按上一行的分辨率运行。开启后渲染帧率不够就逐级降低网络分辨率（最低 50%），有余量再升回，最高到上一行的设定。";
      std::wstring msg = L"渲染帧率低于 " + std::to_wstring(t) + L" 时逐级降低网络分辨率（最低 50%），有余量再升回，最高到上一行；降了不见效会自动退回。开 2 倍帧生成时显示帧率约为它的两倍。";
      if (option < 0) {
        LiveStats st;
        GetLiveStats(&st);
        if (st.netW && st.outW) msg += L"当前" + Fmt(L" %.3g%%。", 100.0 * st.netW / st.outW);
      }
      return msg;
    }
    case kGuard: return L"压住网络在肤色和高饱和颜色上的色度改动，人物肤色更自然。0% 等于不干预。";
    case kSplit: return L"左边原画，右边 Prism。面板打开时可以拖动画面上的分割线。快捷键 F4。";
    case kHud: return L"面板收起后在左上角显示帧率和帧时间。";
  }
  return L"";
}

// Frame-time barcode: one bar per rendered frame, newest on the right; the height is the frame
// time, a caution bar is half again the median, a blocked bar two and a half times it.
void FrameBars(float x0, float y0, float x1, float y1, const LiveStats& st, float dt, bool withScale) {
  const float bw = std::max(2.0f, std::round(2.0f * ui)), gap = std::max(1.0f, std::round(1.0f * ui));
  const int n = std::min(st.frames, int((x1 - x0 + gap) / (bw + gap)));
  if (n <= 0) return;
  std::vector<float> v(st.renderMs + st.frames - n, st.renderMs + st.frames);
  std::vector<float> sorted = v;
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
  const float median = sorted[sorted.size() / 2];
  const float want = std::max(8.0f, std::ceil(median * 2.2f / 4.0f) * 4.0f);
  panel.graphTop += (want - panel.graphTop) * (1.0f - std::exp(-dt / 0.6f));
  const float top = panel.graphTop, h = y1 - y0;
  for (int i = 0; i < n; ++i) {
    const float ms = v[size_t(i)];
    const float bx = x1 - (n - i) * (bw + gap) + gap;
    const float bh = std::max(Hair(), std::round(std::min(1.0f, ms / top) * h));
    const uint32_t c = ms > median * 2.5f ? kBlocked : ms > median * 1.5f ? kCaution : i == n - 1 ? kWhite : kReadout;
    Fill(bx, y1 - bh, bx + bw, y1, c);
  }
  if (withScale) {
    Fill(x0, y0, x1, y0 + Hair(), kHairFaint);
    TextRight(x1, y0 - LineHeight(kLabel) - U(2), Fmt(L"%.0f ms", top), kLabel, kQuiet);
  }
}

// Sorted copy for percentiles.
float Percentile(const LiveStats& st, float q) {
  if (st.frames < 10) return 0;
  std::vector<float> v(st.renderMs, st.renderMs + st.frames);
  const size_t k = std::min(v.size() - 1, size_t(q * v.size()));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}

void DrawPanel(UINT bw, UINT bh, float dt) {
  const double now = Now();
  LiveSettings s = GetLiveSettings();
  LiveStats st;
  GetLiveStats(&st);

  const float W = U(420), pad = U(20), rowH = U(38), labelW = U(108);
  // Height: header, telemetry, rows, help, footer.
  const float headH = U(52), telH = U(144), helpH = U(64), footH = U(40);
  const float H = headH + telH + rowH * float(kRows) + U(12) + helpH + footH;
  float x0 = std::round(cfg.x * ui), y0 = std::round(cfg.y * ui);
  x0 = std::clamp(x0, 0.0f, std::max(0.0f, float(bw) - W)); y0 = std::clamp(y0, 0.0f, std::max(0.0f, float(bh) - H));
  panel.px0 = x0; panel.py0 = y0;
  const float x1 = x0 + W, y1 = y0 + H;
  const float t = Hair();

  // Opening: a wipe from the left edge.
  const float open = cfg.motion ? EaseOutExpo(float(now - panel.openedAt) / 0.18f) : 1.0f;
  clip = {x0, y0, x0 + std::max(1.0f, W * open), y1};

  Fill(x0, y0, x1, y1, Rgba(0x000000, 0.93f));
  Frame(x0, y0, x1, y1, t, kHair);

  // Header: name, version, state.
  {
    const float cy = y0 + std::round((headH - LineHeight(kTitle)) * 0.5f);
    const float nw = Text(x0 + pad, cy, L"Prism Core", kTitle, kWhite);
    Text(x0 + pad + nw + U(8), cy + U(2), L"1.3", kLabel, kQuiet);
    const wchar_t* state; uint32_t mark;
    if (st.failure) { state = L"已停止"; mark = kBlocked; }
    else if (!s.enabled) { state = L"已关闭"; mark = kCaution; }
    else if (!st.running) { state = L"等待画面"; mark = kInfo; }
    else { state = L"运行中"; mark = kReady; }
    const float sw = TextRight(x1 - pad, y0 + std::round((headH - LineHeight(kBody)) * 0.5f), state, kBody, kReadout);
    const float mx = x1 - pad - sw - U(10);
    Fill(mx, y0 + std::round(headH * 0.5f - U(7)), mx + std::max(2.0f, U(2)), y0 + std::round(headH * 0.5f + U(7)), mark);
    Fill(x0, y0 + headH - t, x1, y0 + headH, kHair);
    // Dragging the header moves the panel.
    const bool overHead = in.mx >= x0 && in.mx < x1 && in.my >= y0 && in.my < y0 + headH;
    if (overHead && in.pressed) { panel.drag = Panel::kMove; panel.dragDX = in.mx - x0; panel.dragDY = in.my - y0; }
  }

  // Telemetry.
  float y = y0 + headH;
  {
    const float gx0 = x0 + pad, gx1 = x1 - pad;
    if (st.failure) {
      const std::string what = st.failure;
      std::wstring msg = what.find("nvngx_dlssnr") != std::string::npos ? L"游戏目录里缺少 nvngx_dlssnr.dll。在 Prism NR 里对这个游戏重新点一次启用。"
                                                                          : L"Prism 遇到错误，已经停止：" + std::wstring(what.begin(), what.end()) + L"。重启游戏可以重试，详情在游戏目录的 prism-core.log。";
      float ly = y + U(18);
      Fill(gx0, ly + U(3), gx0 + std::max(2.0f, U(2)), ly + LineHeight(kBody) - U(3), kBlocked);
      for (const std::wstring& line : Wrap(msg, kBody, gx1 - gx0 - U(12))) { Text(gx0 + U(12), ly, line, kBody, kWhite); ly += LineHeight(kBody); }
    } else {
      const float graphY0 = y + U(28), graphY1 = y + U(70);
      FrameBars(gx0, graphY0, gx1, graphY1, st, dt, true);
      const float barStep = std::max(2.0f, std::round(2.0f * ui)) + std::max(1.0f, std::round(1.0f * ui));
      const int shown = std::min(st.frames, int((gx1 - gx0) / barStep));
      const std::wstring graphLabel = shown > 0 ? L"最近 " + std::to_wstring(shown) + L" 帧" : L"帧时间";
      Text(gx0, y + U(8), graphLabel, kLabel, kReadout);
      if (st.presentFps > st.renderFps * 1.3f && st.renderFps > 1) {
        const std::wstring fg = Fmt(L"帧生成开启，显示 %.0f fps", st.presentFps);
        Text(gx0 + Measure(graphLabel, kLabel) + U(12), y + U(8), fg, kLabel, kQuiet);
      }
      // Readouts: label above value, four columns.
      const float colW = (gx1 - gx0) / 4.0f, ly = graphY1 + U(12), vy = ly + LineHeight(kLabel);
      const float p99 = Percentile(st, 0.99f), med = Percentile(st, 0.5f);
      const float prism = st.prismMs + (s.async && s.cadence > 0 ? st.netMs / float(std::max(1, s.cadence)) : 0.0f);
      const std::wstring labels[4] = {L"渲染帧率", L"帧时间", L"1% 低帧", L"Prism GPU"};
      const std::wstring values[4] = {st.renderFps > 0 ? Fmt(L"%.1f", st.renderFps) : L"—", med > 0 ? Fmt(L"%.1f ms", med) : L"—",
                                      p99 > 0 ? Fmt(L"%.0f", 1000.0 / p99) : L"—", st.running && prism > 0 ? Fmt(L"%.1f ms", prism) : L"—"};
      for (int i = 0; i < 4; ++i) {
        Text(gx0 + colW * i, ly, labels[i], kLabel, kReadout);
        Text(gx0 + colW * i, vy, values[i], kValue, kWhite);
      }
      // What runs at which size.
      std::wstring sizes;
      if (st.outW) {
        wchar_t b[128];
        if (st.renderW) std::swprintf(b, 128, L"输出 %u×%u　渲染 %u×%u", st.outW, st.outH, st.renderW, st.renderH);
        else std::swprintf(b, 128, L"输出 %u×%u", st.outW, st.outH);
        sizes = b;
        if (st.netW && (st.netW != st.outW || st.netH != st.outH)) { std::swprintf(b, 128, L"　网络 %u×%u", st.netW, st.netH); sizes += b; }
      } else {
        sizes = L"等待游戏的 DLSS 画面";
      }
      Text(gx0, vy + LineHeight(kValue) + U(6), sizes, kLabel, kQuiet);
    }
    y += telH;
    Fill(x0, y - t, x1, y, kHair);
  }

  // Rows.
  const float cx0 = x0 + pad + labelW, cx1 = x1 - pad;
  y += U(6);
  int hoverRow = -1;
  for (int r = 0; r < kRows; ++r) {
    const float ry = y + r * rowH;
    if (in.mx >= x0 && in.mx < x1 && in.my >= ry && in.my < ry + rowH && !in.pad) hoverRow = r;
  }
  if (hoverRow >= 0 && in.mouseMoved && panel.drag == Panel::kNone) panel.focus = hoverRow;
  // Keyboard and gamepad: up and down move the focus, left and right change the value.
  if (in.up) panel.focus = (panel.focus + kRows - 1) % kRows;
  if (in.dn) panel.focus = (panel.focus + 1) % kRows;
  const int step = in.right ? 1 : in.left ? -1 : 0;
  panel.hoverOption = -1;
  const bool netScaleOn = s.cadence == 1;
  LiveSettings n = s;
  bool changed = false;
  for (int r = 0; r < kRows; ++r) {
    const float ry = y + r * rowH;
    const bool focused = panel.focus == r;
    const bool rowEnabled = (r != kNetScale && r != kTarget) || netScaleOn;
    const float ty = ry + std::round((rowH - LineHeight(kBody)) * 0.5f);
    if (focused) Fill(x0 + U(8), ry + U(10), x0 + U(8) + std::max(2.0f, U(2)), ry + rowH - U(10), kWhite);
    Text(x0 + pad, ty, RowLabel(r), kBody, !rowEnabled ? kQuiet : focused ? kWhite : kReadout);
    const float sh = U(26), sy = ry + std::round((rowH - sh) * 0.5f);
    int hover = -1, clicked = -1;
    const bool keyStep = focused && step != 0;
    switch (r) {
      case kOn: case kSplit: case kHud: {
        bool v = r == kOn ? n.enabled : r == kSplit ? n.split : cfg.hud;
        clicked = Segments(r, cx1 - U(120), sy, cx1, sh, {L"关", L"开"}, v ? 1 : 0, 3, 0, &hover, dt, true);
        bool nv = v;
        if (clicked >= 0) nv = clicked == 1;
        if (keyStep) nv = step > 0;
        if (focused && in.enter) nv = !v;
        if (nv != v) {
          if (r == kOn) { n.enabled = nv; changed = true; }
          else if (r == kSplit) { n.split = nv; changed = true; }
          else { cfg.hud = nv; SavePanelValue(L"OverlayHud", nv ? L"1" : L"0"); panel.savedAt = now; }
        }
        break;
      }
      case kTier: {
        const int cur = TierOf(n);
        uint32_t mask = 0;
        std::vector<std::wstring> names;
        for (int i = 0; i < kTierCount; ++i) { names.push_back(kTiers[i].name); if (!TierLocked(i)) mask |= 1u << i; }
        if (cur < 0) TextRight(cx0 - U(10), ty + U(1), L"自定义", kLabel, kQuiet);
        clicked = Segments(r, cx0, sy, cx1, sh, names, cur, mask, 0, &hover, dt);
        int want = clicked;
        if (keyStep) {
          int i = cur < 0 ? (step > 0 ? -1 : kTierCount) : cur;
          do { i += step; } while (i >= 0 && i < kTierCount && TierLocked(i));
          if (i >= 0 && i < kTierCount) want = i;
        }
        if (want >= 0 && TierLocked(want)) { Toast(L"极致档只在 RTX 5080 / 5090 上开放", kCaution); want = -1; }
        if (want >= 0 && want != cur) {
          const Tier& tr = kTiers[want];
          n.cadence = tr.cadence; n.passes = tr.passes; n.async = tr.async; n.intensity = tr.intensity; n.guard = tr.guard; n.netScale = tr.netScale;
          changed = true;
        }
        if (focused) panel.hoverOption = hover;
        break;
      }
      case kLook: {
        clicked = Segments(r, cx0, sy, cx1, sh, {L"默认", L"高还原", L"高饱和", L"低饱和", L"浓烈"}, std::clamp(n.look, 0, 4), 31, 0, &hover, dt);
        int want = clicked;
        if (keyStep) want = std::clamp(n.look + step, 0, 4);
        if (want >= 0 && want != n.look) { n.look = want; changed = true; }
        if (focused) panel.hoverOption = hover;
        break;
      }
      case kIntensity: case kGuard: {
        const bool isI = r == kIntensity;
        const float max = isI ? 300.0f : 100.0f, val = (isI ? n.intensity : n.guard) * 100.0f;
        const float vw = U(52);
        const Bars b = BarSlider(cx0, sy + U(5), cx1 - vw - U(8), sh - U(14), val, max, 100.0f, isI ? 250.0f : 0.0f, true, focused);
        TextRight(cx1, ty, Fmt(L"%.0f%%", val), kValue, focused ? kWhite : kReadout);
        float nv = val;
        const bool over = in.mx >= b.x0 - U(4) && in.mx < b.x1 + U(4) && in.my >= ry && in.my < ry + rowH && !in.pad;
        if (over && in.pressed) { panel.drag = Panel::kSlide; panel.dragRow = r; }
        if (panel.drag == Panel::kSlide && panel.dragRow == r && in.down)
          nv = std::round(std::clamp((in.mx - b.x0) / (b.x1 - b.x0), 0.0f, 1.0f) * max / 5.0f) * 5.0f;
        if (over && in.wheelSteps) nv = std::clamp(nv + in.wheelSteps * 5.0f, 0.0f, max);
        if (keyStep) nv = std::clamp(std::round((nv + step * 10.0f) / 10.0f) * 10.0f, 0.0f, max);
        if (std::fabs(nv - val) > 0.01f) { (isI ? n.intensity : n.guard) = nv / 100.0f; changed = true; }
        break;
      }
      case kPasses: {
        const int cur = n.passes - 1;
        clicked = Segments(r, cx0, sy, cx1, sh, {L"1", L"2", L"3", L"4", L"5"}, cur, 31, 0b11100, &hover, dt);
        int want = clicked;
        if (keyStep) want = std::clamp(cur + step, 0, 4);
        if (want >= 0 && want != cur) { n.passes = want + 1; changed = true; }
        break;
      }
      case kCadence: {
        const int values[3] = {1, 2, 4};
        const int cur = n.cadence == 1 ? 0 : n.cadence == 2 ? 1 : n.cadence == 4 ? 2 : -1;
        clicked = Segments(r, cx0, sy, cx1, sh, {L"每帧", L"每2帧", L"每4帧"}, cur, 7, 0, &hover, dt);
        int want = clicked;
        if (keyStep) want = std::clamp((cur < 0 ? 1 : cur) + step, 0, 2);
        if (want >= 0 && want != cur) { n.cadence = values[want]; n.async = n.cadence > 1; changed = true; }
        break;
      }
      case kNetScale: {
        // 100% is the flagship's base (62.5% a side), 150% half as many pixels again (76.55%).
        const int cur = Near(n.netScale, 0.625f) ? 0 : Near(n.netScale, 0.7655f) ? 1 : -1;
        if (cur < 0 && netScaleOn) TextRight(cx0 - U(10), ty + U(1), Fmt(L"%.0f%%", 100.0 * n.netScale * n.netScale / (0.625 * 0.625)), kLabel, kQuiet);
        clicked = Segments(r, cx0, sy, cx1, sh, {L"100%", L"150%"}, netScaleOn ? cur : -1, netScaleOn ? 3 : 0, 0, &hover, dt);
        int want = netScaleOn ? clicked : -1;
        if (keyStep && netScaleOn) want = step > 0 ? 1 : 0;
        if (focused && in.enter && netScaleOn) want = cur == 1 ? 0 : 1;
        if (want >= 0 && want != cur) { n.netScale = want == 1 ? 0.7655f : 0.625f; changed = true; }
        if (focused) panel.hoverOption = hover;
        break;
      }
      case kTarget: {
        static const int targets[5] = {0, 40, 50, 60, 90};
        int cur = 0;
        if (n.netAuto) { cur = -1; for (int i = 1; i < 5; ++i) if (targets[i] == n.fpsTarget) cur = i; }
        clicked = Segments(r, cx0, sy, cx1, sh, {L"关", L"40", L"50", L"60", L"90"}, netScaleOn ? cur : -1, netScaleOn ? 31 : 0, 0, &hover, dt);
        if (n.netAuto && cur < 0 && netScaleOn) TextRight(cx0 - U(10), ty + U(1), std::to_wstring(n.fpsTarget), kLabel, kQuiet);
        int want = netScaleOn ? clicked : -1;
        if (keyStep && netScaleOn) want = std::clamp((cur < 0 ? 2 : cur) + step, 0, 4);
        if (want >= 0 && want != cur) {
          n.netAuto = want > 0;
          if (want > 0) n.fpsTarget = targets[want];
          changed = true;
        }
        if (focused) panel.hoverOption = hover;
        break;
      }
    }
    if (r + 1 < kRows) Fill(x0 + pad, ry + rowH - t, x1 - pad, ry + rowH, kHairFaint);
  }
  if (panel.drag == Panel::kSlide && !in.down) panel.drag = Panel::kNone;
  if (changed) Changed(n);
  y += rowH * float(kRows) + U(6);
  Fill(x0, y - t, x1, y, kHair);

  // Help for the focused row.
  {
    float hy = y + U(10);
    for (const std::wstring& line : Wrap(RowHelp(panel.focus, n, panel.hoverOption), kSmall, W - pad * 2)) {
      Text(x0 + pad, hy, line, kSmall, kReadout);
      hy += LineHeight(kSmall);
    }
    y += helpH;
    Fill(x0, y - t, x1, y, kHair);
  }

  // Footer: the keys, and the save state.
  {
    const float fy = y + std::round((footH - LineHeight(kLabel)) * 0.5f);
    std::wstring keys;
    if (in.pad) keys = L"View + Y 收起　十字键 选择和调整　A 切换　B 收起";
    else keys = std::wstring(KeyName(cfg.key)) + L" 收起　↑↓ 选择　←→ 调整　拖动标题栏移动";
    Text(x0 + pad, fy, keys, kLabel, kQuiet);
    const float since = float(now - panel.savedAt);
    if (since < 2.0f) TextRight(x1 - pad, fy, L"已保存", kLabel, Fade(kReadout, since < 1.4f ? 1.0f : (2.0f - since) / 0.6f));
  }
  clip = {-1e9f, -1e9f, 1e9f, 1e9f};

  // Moving the panel.
  if (panel.drag == Panel::kMove) {
    if (in.down) {
      cfg.x = std::max(0.0f, (in.mx - panel.dragDX) / ui);
      cfg.y = std::max(0.0f, (in.my - panel.dragDY) / ui);
    } else {
      panel.drag = Panel::kNone;
      SavePanelValue(L"OverlayX", Fmt(L"%.0f", cfg.x));
      SavePanelValue(L"OverlayY", Fmt(L"%.0f", cfg.y));
    }
  }
}

// The split's labels, and its handle while the panel is open (drag it to move the split).
void DrawSplit(UINT bw, UINT bh, bool interactive) {
  LiveSettings s = GetLiveSettings();
  if (!s.split || !s.enabled) return;
  const float sx = std::round(s.splitPos * bw);
  const float ly = U(24);
  auto tag = [&](const std::wstring& text, bool leftOfLine) {
    const float tw = Measure(text, kLabel), bx0 = leftOfLine ? sx - U(10) - tw - U(16) : sx + U(10), bx1 = bx0 + tw + U(16);
    Fill(bx0, ly, bx1, ly + LineHeight(kLabel) + U(8), Rgba(0x000000, 0.8f));
    Text(bx0 + U(8), ly + U(4), text, kLabel, kWhite);
  };
  tag(L"原画", true);
  tag(L"Prism", false);
  if (!interactive) return;
  const float hy = std::round(bh * 0.5f), hw = U(7), hh = U(28);
  const bool over = std::fabs(in.mx - sx) < U(14) && std::fabs(in.my - hy) < U(40) && !in.pad;
  Fill(sx - hw, hy - hh, sx + hw, hy + hh, over || panel.drag == Panel::kSplitLine ? kWhite : Rgba(0xe6e6e6));
  for (int i = -1; i <= 1; ++i) Fill(sx - U(3), hy + i * U(5) - Hair() * 0.5f, sx + U(3), hy + i * U(5) + Hair() * 0.5f, kBlack);
  if (over && in.pressed && panel.drag == Panel::kNone) panel.drag = Panel::kSplitLine;
  if (panel.drag == Panel::kSplitLine) {
    if (in.down) { s.splitPos = std::clamp(in.mx / float(bw), 0.02f, 0.98f); Changed(s); }
    else panel.drag = Panel::kNone;
  }
}

void DrawHud(const LiveStats& st, const LiveSettings& s) {
  const float x0 = U(24), y0 = U(24), w = U(176), h = U(64);
  Fill(x0, y0, x0 + w, y0 + h, Rgba(0x000000, 0.8f));
  const float fw = Text(x0 + U(12), y0 + U(6), st.renderFps > 0 ? Fmt(L"%.0f", st.renderFps) : L"—", kValueLarge, kWhite);
  Text(x0 + U(12) + fw + U(6), y0 + U(14), L"fps", kLabel, kReadout);
  const float p50 = Percentile(st, 0.5f);
  TextRight(x0 + w - U(12), y0 + U(14), s.enabled ? (p50 > 0 ? Fmt(L"%.1f ms", p50) : L"") : L"Prism 关", kLabel, s.enabled ? kReadout : kCaution);
  FrameBars(x0 + U(12), y0 + U(40), x0 + w - U(12), y0 + h - U(8), st, 1.0f / 60.0f, false);
}

void DrawToast(double now) {
  const float age = float(now - panel.toastAt);
  if (panel.toast.empty() || age > panel.toastFor + 0.3f) return;
  const float a = age < panel.toastFor ? 1.0f : 1.0f - (age - float(panel.toastFor)) / 0.3f;
  const float x0 = U(24), y0 = cfg.hud ? U(100) : U(24);
  const float tw = Measure(panel.toast, kBody), w = tw + U(40), h = LineHeight(kBody) + U(20);
  const float wipe = cfg.motion ? EaseOutExpo(age / 0.15f) : 1.0f;
  clip = {x0, y0, x0 + w * wipe, y0 + h};
  Fill(x0, y0, x0 + w, y0 + h, Fade(Rgba(0x000000, 0.9f), a));
  Fill(x0 + U(12), y0 + U(12), x0 + U(12) + std::max(2.0f, U(2)), y0 + h - U(12), Fade(panel.toastMark, a));
  Text(x0 + U(24), y0 + U(10), panel.toast, kBody, Fade(kWhite, a));
  clip = {-1e9f, -1e9f, 1e9f, 1e9f};
}

// Test hooks for tests/overlay_demo.cpp: PRISM_OVERLAY_TEST=open,focus=N,pad,toast
void TestSetup() {
  const char* e = std::getenv("PRISM_OVERLAY_TEST");
  if (!e) return;
  const std::string t = e;
  if (t.find("open") != std::string::npos) { panel.open = true; panel.openedAt = -10; }
  const size_t f = t.find("focus=");
  if (f != std::string::npos) panel.focus = std::clamp(std::atoi(t.c_str() + f + 6), 0, kRows - 1);
  if (t.find("pad") != std::string::npos) in.pad = true;
  if (t.find("toast") != std::string::npos) Toast(L"按 Home 打开 Prism 面板", kInfo, 1e6);
  if (t.find("hud") != std::string::npos) cfg.hud = true;
  if (t.find("split") != std::string::npos) { LiveSettings s = GetLiveSettings(); s.split = true; s.splitPos = 0.62f; SetLiveSettings(s); }
  if (t.find("nohint") != std::string::npos) panel.hinted = true;
}

bool started = false;

}  // namespace

void OverlaySetup(void** vt) {
  // vtable slot 38: IDXGISwapChain3::SetColorSpace1.
  if (MH_CreateHook(vt[38], (void*)&HkSetColourSpace, (void**)&realSetColourSpace) == MH_OK) MH_EnableHook(vt[38]);
}

void OverlayNoteQueue(ID3D12CommandQueue* q) {
  static thread_local ID3D12CommandQueue* last = nullptr;
  if (q == last || !q) return;
  last = q;
  std::lock_guard<std::mutex> lock(queueMutex);
  if (std::find(directQueues.begin(), directQueues.end(), q) != directQueues.end()) return;
  if (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
  if (directQueues.size() >= 32) directQueues.erase(directQueues.begin());
  directQueues.push_back(q);
}

void OverlayPresent(IDXGISwapChain* sc) {
  if (gpu.failed) return;
  if (!started) {
    started = true;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&OverlayPresent, &self);
    if (self) {
      const auto* dos = (const IMAGE_DOS_HEADER*)self;
      const auto* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)self + dos->e_lfanew);
      selfBegin = (const uint8_t*)self; selfEnd = selfBegin + nt->OptionalHeader.SizeOfImage;
    }
    LoadPanelConfig();
    if (!InitFonts()) { LOG("panel off: no fonts"); gpu.failed = true; return; }
    if (cfg.block) HookInput();
    HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
    if (xi) padRead = (PadStateFn)(void*)GetProcAddress(xi, "XInputGetState");
    panel.startedAt = Now();
    TestSetup();
    LOG("panel ready (%ls opens it, View + Y on a gamepad)", KeyName(cfg.key));
  }
  if (sc != chain.sc) Bind(sc);
  if (chain.unsupported || !chain.queue) return;

  static double last = Now();
  static uint32_t presents = 0;
  const double now = Now();
  const float dt = float(std::min(0.1, now - last));
  last = now;
  if (++presents % 120 == 0) { LoadPanelConfig(); if (cfg.block) HookPads(); }
  if (panel.drag == Panel::kNone && panel.changedAt > 0 && now - panel.changedAt > 0.4) { SaveLiveSettings(); panel.changedAt = -1; panel.savedAt = now; }

  DXGI_SWAP_CHAIN_DESC d;
  if (FAILED(sc->GetDesc(&d)) || !d.BufferDesc.Width || !d.BufferDesc.Height) return;
  const UINT bw = d.BufferDesc.Width, bh = d.BufferDesc.Height;
  ui = std::clamp(std::round(float(bh) / 1080.0f * 4.0f) / 4.0f, 1.0f, 3.0f) * cfg.scale;

  PollInput(bw, bh, panel.open);
  if (in.toggle || (panel.open && in.back)) {
    panel.open = in.toggle ? !panel.open : false;
    if (panel.open) panel.openedAt = now; else panel.closedAt = now;
    panel.drag = Panel::kNone;
    if (!panel.open && panel.changedAt > 0) { SaveLiveSettings(); panel.changedAt = -1; }
    panel.hinted = true;
  }
  // After closing, the game gets its input back only once the keys that closed the panel are up
  // (otherwise it sees Esc, Home or B go down and opens its own menu).
  const bool closingKeysDown = Key(VK_ESCAPE) || (cfg.key && Key(cfg.key)) || Key(VK_RETURN) || Key(VK_LBUTTON) ||
                               (in.padHeld & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_BACK));
  SetBlocking(cfg.block && (panel.open || (blocking && closingKeysDown)));
  const std::wstring note = TakeLiveNotice();
  if (!note.empty() && !panel.open) Toast(note);
  if (!panel.hinted && cfg.hint && now - panel.startedAt > 1.0) {
    panel.hinted = true;
    Toast(in.padConnected ? L"按 Home，或在手柄上按住 View 再按 Y，打开 Prism 面板" : std::wstring(L"按 ") + KeyName(cfg.key) + L" 打开 Prism 面板", kInfo, 6.0);
  }

  verts.clear();
  LiveStats st;
  GetLiveStats(&st);
  const LiveSettings s = GetLiveSettings();
  DrawSplit(bw, bh, panel.open);
  if (panel.open) {
    DrawPanel(bw, bh, dt);
    CURSORINFO ci = {};
    ci.cbSize = sizeof ci;
    const bool systemCursor = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
    const bool inside = in.mx >= 0 && in.my >= 0 && in.mx < float(bw) && in.my < float(bh);
    if (!in.pad && !systemCursor && inside && Foreground()) Cursor(in.mx, in.my);
  } else {
    if (cfg.hud) DrawHud(st, s);
    DrawToast(now);
  }
  if (verts.empty()) return;
  Submit(sc, bw, bh, d.BufferDesc.Format);
}

}  // namespace prism

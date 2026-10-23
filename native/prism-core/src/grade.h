// The custom look (Look=5, 自定义): every control the looks are made of, and more, as the panel and
// the app show them. One table drives the panel's rows, the ini lines (Grade=Contrast:24,Sat:-34,...:
// only what differs from neutral; three slots) and the shader's parameters (look_hlsl.h's LkCustom,
// a constant buffer at b1). electron/shared/grade.ts mirrors the table for the app's grading page.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

namespace prism {

enum Grade {
  // 影调
  kGExposure, kGContrast, kGPivot, kGHighlights, kGShadows, kGWhites, kGBlacks, kGMidtones,
  // 色彩
  kGTemp, kGTint, kGSat, kGVibrance, kGSkinKeep, kGSkinAlign, kGMix,
  // 色轮: shadows, midtones, highlights (hue, amount)
  kGShadowHue, kGShadowAmt, kGMidHue, kGMidAmt, kGHiHue, kGHiAmt,
  // 色相: by hue (red, yellow, green, cyan, blue, magenta)
  kGHueShift, kGHueSat = kGHueShift + 6, kGHueLight = kGHueSat + 6,
  // 曲线: luminance, red, green, blue; five points each (-6, -3, 0, +3, +6 stops from mid grey)
  kGCurve = kGHueLight + 6,
  // 胶片
  kGDensity = kGCurve + 20, kGHiTwist, kGSky, kGHiDesat, kGLoDesat, kGVignette, kGGrain,
  kGradeCount
};
constexpr int kCurvePoints = 5;
constexpr float kCurveStops[kCurvePoints] = {-6, -3, 0, 3, 6};

// How a value reads: signed (+25), plain (25), stops (+0.50 EV), a hue (200°), a hue turn (+12°).
enum GradeUnit { kUSigned, kUPlain, kUStops, kUHue, kUTurn };
struct GradeSpec { const wchar_t* key; const wchar_t* label; float min, max, def, step; GradeUnit unit; const wchar_t* help; };

inline const wchar_t* HueName(int h) { static const wchar_t* n[6] = {L"红", L"黄", L"绿", L"青", L"蓝", L"品红"}; return n[std::clamp(h, 0, 5)]; }
inline const wchar_t* CurveChannel(int c) { static const wchar_t* n[4] = {L"亮度", L"红", L"绿", L"蓝"}; return n[std::clamp(c, 0, 3)]; }
inline const wchar_t* CurvePoint(int i) { static const wchar_t* n[kCurvePoints] = {L"最暗", L"暗部", L"中灰", L"亮部", L"最亮"}; return n[std::clamp(i, 0, kCurvePoints - 1)]; }

inline const GradeSpec& GradeInfo(int i) {
  static const wchar_t* hueKeys[3][6] = {{L"HueRed", L"HueYellow", L"HueGreen", L"HueCyan", L"HueBlue", L"HueMagenta"},
                                         {L"SatRed", L"SatYellow", L"SatGreen", L"SatCyan", L"SatBlue", L"SatMagenta"},
                                         {L"LumRed", L"LumYellow", L"LumGreen", L"LumCyan", L"LumBlue", L"LumMagenta"}};
  static const wchar_t* curveKeys[4][kCurvePoints] = {{L"CurveY1", L"CurveY2", L"CurveY3", L"CurveY4", L"CurveY5"},
                                                      {L"CurveR1", L"CurveR2", L"CurveR3", L"CurveR4", L"CurveR5"},
                                                      {L"CurveG1", L"CurveG2", L"CurveG3", L"CurveG4", L"CurveG5"},
                                                      {L"CurveB1", L"CurveB2", L"CurveB3", L"CurveB4", L"CurveB5"}};
  static GradeSpec t[kGradeCount] = {};
  static bool made = false;
  if (!made) {
    made = true;
    t[kGExposure] = {L"Exposure", L"曝光", -3, 3, 0, 0.05f, kUStops, L"整体亮度，单位是档（EV）：+1 亮一倍。在游戏自己的色调映射之前调，高光不会被硬切。"};
    t[kGContrast] = {L"Contrast", L"对比", -100, 100, 0, 1, kUSigned, L"以对比轴心为中心拉开或收拢明暗，只动亮度、不动颜色比例，所以色相不偏。"};
    t[kGPivot] = {L"Pivot", L"对比轴心", -2, 2, 0, 0.05f, kUStops, L"对比围绕的亮度，单位是档：负值让暗部也参与拉开，正值只拉开亮处。"};
    t[kGHighlights] = {L"Highlights", L"高光", -100, 100, 0, 1, kUSigned, L"只调比中灰亮的部分：负值找回天空和灯光的层次，正值让亮处更通透。"};
    t[kGShadows] = {L"Shadows", L"阴影", -100, 100, 0, 1, kUSigned, L"只调比中灰暗的部分：正值提亮暗部细节，负值压暗让画面更沉。"};
    t[kGWhites] = {L"Whites", L"白位", -100, 100, 0, 1, kUSigned, L"最亮的一档：负值把灯光和太阳附近收回来，正值让最亮处更耀眼。"};
    t[kGBlacks] = {L"Blacks", L"黑位", -100, 100, 0, 1, kUSigned, L"最深的黑：正值抬起成深灰（胶片的灰黑），负值压到更纯的黑。"};
    t[kGMidtones] = {L"Midtones", L"中间调", -100, 100, 0, 1, kUSigned, L"中灰附近的亮度：正值更明亮通透，负值更沉，最亮和最暗处基本不动。"};
    t[kGTemp] = {L"Temperature", L"色温", -100, 100, 0, 1, kUSigned, L"白平衡的冷暖：正值偏暖（黄），负值偏冷（蓝）。整体亮度保持不变。"};
    t[kGTint] = {L"Tint", L"色调", -100, 100, 0, 1, kUSigned, L"白平衡的绿与品红：正值偏品红，负值偏绿。用来去掉画面的偏色。"};
    t[kGSat] = {L"Saturation", L"饱和", -100, 100, 0, 1, kUSigned, L"所有颜色的浓度。灰和白始终保持中性，-100 是黑白。"};
    t[kGVibrance] = {L"Vibrance", L"自然饱和", -100, 100, 0, 1, kUSigned, L"越灰的颜色加得越多、已经浓的颜色几乎不动，比饱和更不容易过头。"};
    t[kGSkinKeep] = {L"SkinProtect", L"肤色保护", 0, 100, 50, 1, kUPlain, L"皮肤少跟一些颜色改动（饱和、色相、明度、染色）：100 时皮肤完全不动，0 时和其他颜色一样。"};
    t[kGSkinAlign] = {L"SkinAlign", L"肤色校正", 0, 100, 0, 1, kUPlain, L"把偏红或偏黄的皮肤往自然肤色线上拉。偏红显得斑驳，偏黄显得苍白。"};
    t[kGMix] = {L"Mix", L"整体强度", 0, 100, 100, 1, kUPlain, L"整套调色的浓淡：100 是调好的样子，0 回到网络原样的颜色。"};
    t[kGShadowHue] = {L"ShadowHue", L"暗部色相", 0, 360, 210, 5, kUHue, L"三向色轮的暗部：暗处染上的颜色，色相环上的空心方块。电影常用青蓝。"};
    t[kGShadowAmt] = {L"ShadowAmount", L"暗部染色", 0, 100, 0, 1, kUPlain, L"暗部染色的浓度。越接近黑越淡，纯黑始终是黑。"};
    t[kGMidHue] = {L"MidHue", L"中间色相", 0, 360, 40, 5, kUHue, L"三向色轮的中间调：中灰附近染上的颜色，色相环上的十字。"};
    t[kGMidAmt] = {L"MidAmount", L"中间染色", 0, 100, 0, 1, kUPlain, L"中间调染色的浓度，影响面最大，少量就够。"};
    t[kGHiHue] = {L"HighlightHue", L"亮部色相", 0, 360, 60, 5, kUHue, L"三向色轮的亮部：亮处染上的颜色，色相环上的实心方块。常用暖黄或橙。"};
    t[kGHiAmt] = {L"HighlightAmount", L"亮部染色", 0, 100, 0, 1, kUPlain, L"亮部染色的浓度。"};
    for (int h = 0; h < 6; ++h) {
      t[kGHueShift + h] = {hueKeys[0][h], HueName(h), -30, 30, 0, 1, kUTurn, L"把这一类颜色的色相转向相邻的颜色，单位是度。"};
      t[kGHueSat + h] = {hueKeys[1][h], HueName(h), -100, 100, 0, 1, kUSigned, L"这一类颜色的饱和度，-100 时这类颜色变灰。"};
      t[kGHueLight + h] = {hueKeys[2][h], HueName(h), -100, 100, 0, 1, kUSigned, L"这一类颜色的明度：比如压暗蓝色让天空更深，提亮绿色让草木更鲜。"};
    }
    for (int c = 0; c < 4; ++c)
      for (int i = 0; i < kCurvePoints; ++i)
        t[kGCurve + c * kCurvePoints + i] = {curveKeys[c][i], CurvePoint(i), -100, 100, 0, 1, kUSigned,
                                             c == 0 ? L"亮度曲线上的一个点：在这一档亮度上整体变亮或变暗，颜色比例不变。点之间平滑过渡。"
                                                    : L"这个通道的曲线上的一个点：只抬高或压低这一档亮度里的这个颜色，用来给暗部、高光分别校色。"};
    t[kGDensity] = {L"Density", L"胶片密度", 0, 100, 0, 1, kUPlain, L"颜色变浓时同时变深，像胶片染料：浓的颜色显得厚重，不会像荧光色。蓝和品红最明显，黄色基本不变。"};
    t[kGHiTwist] = {L"HighlightTwist", L"高光走向", 0, 100, 0, 1, kUPlain, L"很亮的红转向橙、很亮的蓝转向青，像胶片：火焰不会发粉，天空不会发紫。皮肤不受影响。"};
    t[kGSky] = {L"Sky", L"天空加深", 0, 100, 0, 1, kUPlain, L"压暗明亮的蓝和青，像偏振镜下的天空。"};
    t[kGHiDesat] = {L"HighlightDesat", L"高光去色", 0, 100, 0, 1, kUPlain, L"很亮的地方褪掉一些颜色，像胶片高光走向白色，而不是浓到发假。"};
    t[kGLoDesat] = {L"ShadowDesat", L"暗部去色", 0, 100, 0, 1, kUPlain, L"很暗的地方褪掉一些颜色，暗部更干净，不会有彩色噪点感。"};
    t[kGVignette] = {L"Vignette", L"暗角", -100, 100, 0, 1, kUSigned, L"画面四角的亮度：负值压暗四角、把视线引向中间，正值提亮四角。"};
    t[kGGrain] = {L"Grain", L"颗粒", 0, 100, 0, 1, kUPlain, L"胶片颗粒，每帧变化，中间调最明显。少量能盖住色带，让画面更有质感。"};
  }
  return t[std::clamp(i, 0, kGradeCount - 1)];
}

struct GradeValues {
  float v[kGradeCount];
  GradeValues() { Reset(); }
  void Reset() { for (int i = 0; i < kGradeCount; ++i) v[i] = GradeInfo(i).def; }
  bool operator==(const GradeValues& o) const { for (int i = 0; i < kGradeCount; ++i) if (std::fabs(v[i] - o.v[i]) > 1e-4f) return false; return true; }
  bool operator!=(const GradeValues& o) const { return !(*this == o); }
};

// The shader's parameters (look_hlsl.h, cbuffer LkCustom: 18 float4s).
constexpr int kGradeConstants = 72;
inline void GradeToShader(const GradeValues& g, float k[kGradeConstants]) {
  const float* v = g.v;
  auto s = [&](int i) { return v[i] / 100.0f; };
  const float deg = 3.14159265f / 180.0f;
  for (int i = 0; i < kGradeConstants; ++i) k[i] = 0.0f;
  k[0] = 1.0f + 0.6f * s(kGContrast);   // contrast: 0.4 to 1.6 at the pivot
  k[1] = 1.0f + s(kGSat);                // sat
  k[2] = s(kGVibrance);                  // vibrance
  k[3] = v[kGExposure];                  // bias, stops
  k[4] = 1.5f * s(kGHighlights);         // stops above grey
  k[5] = 1.5f * s(kGShadows);            // stops below grey
  k[6] = s(kGBlacks);
  k[7] = s(kGDensity);
  k[8] = 0.5f * s(kGHiDesat);
  k[9] = 0.6f * s(kGLoDesat);
  k[10] = 1.0f - s(kGSkinKeep);          // skinKeep: the share of a change that reaches skin
  k[11] = 0.6f * s(kGSkinAlign);
  k[12] = 0.15f * s(kGSky);
  k[13] = 0.25f * s(kGHiTwist);
  k[14] = s(kGTemp);
  k[15] = s(kGTint);
  // The three wheels: a hue and an amount each, as an Oklab (a, b) offset.
  auto wheel = [&](int at, int hue, int amt, float scale) {
    const float m = scale * s(amt);
    k[at] = m * std::cos(v[hue] * deg); k[at + 1] = m * std::sin(v[hue] * deg);
  };
  wheel(16, kGShadowHue, kGShadowAmt, 0.03f);
  wheel(18, kGHiHue, kGHiAmt, 0.02f);
  for (int h = 0; h < 6; ++h) {
    k[20 + h] = 1.0f + s(kGHueSat + h);
    k[26 + h] = v[kGHueShift + h] * deg;
    k[32 + h] = 0.3f * s(kGHueLight + h);
  }
  wheel(38, kGMidHue, kGMidAmt, 0.02f);
  k[40] = 1.5f * s(kGWhites);
  k[41] = 1.0f * s(kGMidtones);
  k[42] = s(kGVignette);
  k[43] = s(kGGrain);
  k[44] = 0.0f;                          // the grain's frame seed (GradeSeed)
  k[45] = s(kGMix);
  k[46] = v[kGPivot];
  bool curves = false;
  for (int i = 0; i < 4 * kCurvePoints; ++i) { k[48 + i] = 1.5f * s(kGCurve + i); curves |= v[kGCurve + i] != 0.0f; }
  k[68] = curves ? 1.0f : 0.0f;
}
inline void GradeSeed(float k[kGradeConstants], unsigned frame) { k[44] = float(frame & 1023u); }

// The curve through a channel's five points (stops in, stops added), as the shader draws it:
// Catmull-Rom between the points, easing to the end points' values beyond them.
inline float GradeCurveAt(const float pts[kCurvePoints], float l) {
  if (l <= kCurveStops[0]) return pts[0];
  if (l >= kCurveStops[kCurvePoints - 1]) return pts[kCurvePoints - 1];
  const float f = (l - kCurveStops[0]) / 3.0f;
  const int i = std::min(int(f), kCurvePoints - 2);
  const float t = f - i;
  const float p0 = pts[std::max(i - 1, 0)], p1 = pts[i], p2 = pts[i + 1], p3 = pts[std::min(i + 2, kCurvePoints - 1)];
  return 0.5f * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}

// The tone the panel draws: stops out for stops in (from mid grey), luminance only.
inline float GradeTone(const GradeValues& g, float l) {
  float k[kGradeConstants];
  GradeToShader(g, k);
  const float lp = l - k[46];
  float o = l + (k[0] - 1.0f) * 2.5f * std::tanh(lp / 2.5f);
  auto ss = [](float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3 - 2 * t); };
  o += k[4] * ss(0.0f, 3.0f, o) + k[5] * (1.0f - ss(-4.0f, 0.0f, o));
  o += k[40] * ss(2.0f, 6.0f, o) + k[41] * std::exp(-0.5f * o * o / 2.25f);
  o += GradeCurveAt(k + 48, o);
  o += k[3];
  float y = 0.18f * std::exp2(o);
  if (k[6] > 0) y += k[6] * 0.004f;
  else if (k[6] < 0) y *= y / (y + -k[6] * 0.004f);
  return std::log2(std::max(y, 1e-7f) / 0.18f);
}

// A preset as a starting point (look_hlsl.h's LkFor, in the panel's units): 中性, 高饱和, 低饱和, 浓烈.
inline GradeValues GradeFromPreset(int which) {
  GradeValues g;
  float* v = g.v;
  auto split = [&](int hueI, int amtI, float a, float b, float scale) {
    const float m = std::sqrt(a * a + b * b);
    if (m < 1e-6f) return;
    float h = std::atan2(b, a) * 180.0f / 3.14159265f;
    if (h < 0) h += 360.0f;
    v[hueI] = std::round(h); v[amtI] = std::round(std::min(100.0f, m / scale * 100.0f));
  };
  auto set = [&](float contrast, float sat, float vib, float skinKeep, float hiD, float loD, const float hue[6], float tlA, float tlB, float thA, float thB,
                 float density, float skinAlign, float sky, float bias, float twist) {
    v[kGContrast] = std::round((contrast - 1.0f) / 0.6f * 100.0f);
    v[kGSat] = std::round((sat - 1.0f) * 100.0f);
    v[kGVibrance] = std::round(vib * 100.0f);
    v[kGSkinKeep] = std::round((1.0f - skinKeep) * 100.0f);
    v[kGHiDesat] = std::round(hiD / 0.5f * 100.0f);
    v[kGLoDesat] = std::round(loD / 0.6f * 100.0f);
    for (int h = 0; h < 6; ++h) v[kGHueSat + h] = std::round((hue[h] - 1.0f) * 100.0f);
    split(kGShadowHue, kGShadowAmt, tlA, tlB, 0.03f);
    split(kGHiHue, kGHiAmt, thA, thB, 0.02f);
    v[kGDensity] = std::round(density * 100.0f);
    v[kGSkinAlign] = std::round(skinAlign / 0.6f * 100.0f);
    v[kGSky] = std::round(sky / 0.15f * 100.0f);
    v[kGExposure] = bias;
    v[kGHiTwist] = std::round(twist / 0.25f * 100.0f);
  };
  if (which == 1) {
    const float hue[6] = {0.92f, 1.08f, 1.12f, 1.15f, 1.00f, 1.05f};
    set(1.10f, 1.38f, 0.55f, 0.40f, 0.10f, 0.08f, hue, 0, 0, 0, 0, 0.35f, 0.25f, 0.04f, 0.0f, 0.10f);
  } else if (which == 2) {
    const float hue[6] = {0.95f, 0.85f, 0.78f, 1.00f, 1.00f, 0.85f};
    set(1.24f, 0.66f, 0.0f, 0.45f, 0.12f, 0.25f, hue, -0.010f, -0.016f, -0.002f, -0.007f, 0.0f, 0.30f, 0.0f, -0.2f, 0.08f);
  } else if (which == 3) {
    const float hue[6] = {0.95f, 1.05f, 1.08f, 1.12f, 1.08f, 1.02f};
    set(1.42f, 1.50f, 0.35f, 0.40f, 0.10f, 0.0f, hue, -0.008f, -0.014f, 0.004f, 0.012f, 0.55f, 0.35f, 0.07f, 0.0f, 0.14f);
  }
  return g;
}

// The custom look keeps three slots (方案 1-3): Grade=, Grade2=, Grade3= and GradeSlot=.
constexpr int kGradeSlots = 3;
inline const wchar_t* GradeKey(int slot) { static const wchar_t* k[kGradeSlots] = {L"Grade", L"Grade2", L"Grade3"}; return k[std::clamp(slot, 0, kGradeSlots - 1)]; }

// The ini line: "Key:value" pairs for what differs from neutral, comma-separated.
inline std::wstring GradeFormat(const GradeValues& g) {
  std::wstring s;
  for (int i = 0; i < kGradeCount; ++i) {
    const GradeSpec& p = GradeInfo(i);
    if (std::fabs(g.v[i] - p.def) < 1e-4f) continue;
    wchar_t b[64];
    std::swprintf(b, 64, p.unit == kUStops ? L"%ls:%.2f" : L"%ls:%.0f", p.key, double(g.v[i]));
    if (!s.empty()) s += L",";
    s += b;
  }
  return s;
}
inline GradeValues GradeParse(const std::wstring& s) {
  GradeValues g;
  size_t at = 0;
  while (at < s.size()) {
    size_t end = s.find(L',', at);
    if (end == std::wstring::npos) end = s.size();
    const std::wstring item = s.substr(at, end - at);
    const size_t colon = item.find(L':');
    if (colon != std::wstring::npos) {
      std::wstring key = item.substr(0, colon);
      while (!key.empty() && key.front() == L' ') key.erase(key.begin());
      for (int i = 0; i < kGradeCount; ++i) {
        const GradeSpec& p = GradeInfo(i);
        if (_wcsicmp(key.c_str(), p.key) == 0) g.v[i] = std::clamp(float(_wtof(item.c_str() + colon + 1)), p.min, p.max);
      }
    }
    at = end + 1;
  }
  return g;
}

}  // namespace prism

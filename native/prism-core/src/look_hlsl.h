// Prism's looks (Look=0..4 in prism-core.ini), as HLSL source: part of Core's shaders
// (pipeline.cpp) and of tests/look_preview.cpp, so the preview runs exactly what ships.
//
// The grade happens in scene-referred light, before the game's own tone mapping, the way a
// colourist grades log footage: contrast in stops around mid grey on luminance alone, colour in
// Oklab (perceptual and hue-linear). Muted colours gain more than strong ones (vibrance), greys
// and whites stay neutral, skin takes only part of any change, highlights and deep shadows lose a
// little chroma as film does, and colours that would leave the gamut are pulled back along their
// own hue and lightness instead of being clipped.
//   0 默认    the network's result as it is
//   1 高还原  the original's colour: only the network's light and shade, softly limited, less on skin
//   2 高饱和  vivid and clean, for anime and stylised games
//   3 低饱和  restrained colour with cool, clean whites: the modern film and series look
//   4 浓烈    deep contrast and rich colour
#pragma once

#define PRISM_LOOK_HLSL R"(
static const float kLkMidGrey = 0.18;
static const float kLkPi = 3.14159265;

// Oklab from linear sRGB (Ottosson 2020); cube roots keep their sign for out-of-range values.
float3 LkOklab(float3 c) {
  float3 lms = float3(dot(c, float3(0.4122214708, 0.5363325363, 0.0514459929)),
                      dot(c, float3(0.2119034982, 0.6806995451, 0.1073969566)),
                      dot(c, float3(0.0883024619, 0.2817188376, 0.6299787005)));
  lms = sign(lms) * pow(abs(lms), 1.0 / 3.0);
  return float3(dot(lms, float3(0.2104542553, 0.7936177850, -0.0040720468)),
                dot(lms, float3(1.9779984951, -2.4285922050, 0.4505937099)),
                dot(lms, float3(0.0259040371, 0.7827717662, -0.8086757660)));
}
float3 LkLinear(float3 lab) {
  float3 lms = float3(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
                      lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
                      lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
  lms = lms * lms * lms;
  return float3(dot(lms, float3(4.0767416621, -3.3077115913, 0.2309699292)),
                dot(lms, float3(-1.2684380046, 2.6097574011, -0.3413193965)),
                dot(lms, float3(-0.0041960863, -0.7034186147, 1.7076147010)));
}
float LkY(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float LkMin(float3 c) { return min(c.r, min(c.g, c.b)); }

// A raised-cosine bump around a hue (radians, wrapping).
float LkHue(float h, float centre, float width) {
  float d = abs(h - centre);
  d = min(d, 2.0 * kLkPi - d);
  return d < width ? 0.5 + 0.5 * cos(kLkPi * d / width) : 0.0;
}

// Skin: warm hues around 55 degrees in Oklab, moderate chroma, not in deep shadow.
float LkSkin(float3 lab) {
  const float c = length(lab.yz);
  return LkHue(atan2(lab.z, lab.y), 0.96, 0.52) * smoothstep(0.018, 0.045, c) * (1.0 - smoothstep(0.15, 0.22, c)) * smoothstep(0.22, 0.38, lab.x);
}

// The largest Oklab chroma at lightness L along a hue direction that has no negative channel. With
// no upper bound (scene light), it grows with L.
float LkMaxChroma(float L, float2 dir) {
  float c0 = 0.0, c1 = 0.8 * max(L, 0.0) + 0.01;
  [unroll] for (int i = 0; i < 10; ++i) {
    const float m = 0.5 * (c0 + c1);
    // A channel that only grazes zero along the way (red, on the way to blue) must not stop it.
    const float3 q = LkLinear(float3(L, dir * m));
    if (LkMin(q) >= -0.002 * max(q.r, max(q.g, q.b))) c0 = m; else c1 = m;
  }
  return c0;
}

struct LkParams {
  float contrast;   // slope at mid grey; eases back to 1 a few stops away
  float sat;        // chroma scale
  float vibrance;   // extra chroma for muted colours
  float skinKeep;   // the share of the chroma change that reaches skin
  float hiDesat;    // chroma lost above about +2 stops
  float loDesat;    // chroma lost below about -4 stops
  float hue[6];     // chroma scale by hue: red, yellow, green, cyan, blue, magenta
  float2 tintLo;    // Oklab (a, b) offsets, relative to lightness, in shadows
  float2 tintHi;    // and in highlights
};

LkParams LkFor(uint look) {
  LkParams p;
  p.contrast = 1.0; p.sat = 1.0; p.vibrance = 0.0; p.skinKeep = 1.0; p.hiDesat = 0.0; p.loDesat = 0.0;
  p.hue[0] = 1.0; p.hue[1] = 1.0; p.hue[2] = 1.0; p.hue[3] = 1.0; p.hue[4] = 1.0; p.hue[5] = 1.0;
  p.tintLo = 0.0; p.tintHi = 0.0;
  if (look == 2u) {  // 高饱和
    p.contrast = 1.10; p.sat = 1.38; p.vibrance = 0.55; p.skinKeep = 0.40; p.hiDesat = 0.10; p.loDesat = 0.08;
    p.hue[0] = 0.92; p.hue[1] = 1.08; p.hue[2] = 1.12; p.hue[3] = 1.15; p.hue[4] = 1.00; p.hue[5] = 1.05;
  } else if (look == 3u) {  // 低饱和
    p.contrast = 1.14; p.sat = 0.58; p.vibrance = 0.0; p.skinKeep = 0.45; p.hiDesat = 0.25; p.loDesat = 0.25;
    p.hue[0] = 0.95; p.hue[1] = 0.85; p.hue[2] = 0.78; p.hue[3] = 1.00; p.hue[4] = 1.00; p.hue[5] = 0.85;
    p.tintLo = float2(-0.010, -0.016); p.tintHi = float2(-0.004, -0.014);
  } else if (look == 4u) {  // 浓烈
    p.contrast = 1.42; p.sat = 1.50; p.vibrance = 0.35; p.skinKeep = 0.40; p.hiDesat = 0.10; p.loDesat = 0.0;
    p.hue[0] = 0.95; p.hue[1] = 1.05; p.hue[2] = 1.08; p.hue[3] = 1.12; p.hue[4] = 1.08; p.hue[5] = 1.02;
    p.tintLo = float2(-0.008, -0.014); p.tintHi = float2(0.004, 0.012);
  }
  return p;
}

// x: linear, exposure-normalised (the frame's mean at mid grey). Returns the graded colour.
float3 LkGrade(float3 x, LkParams p) {
  x = max(x, 0.0);
  // Contrast in stops around mid grey, as one ratio for all channels (hue and chroma ratios stay).
  // It follows the mean of luminance and the largest channel: on luminance alone a saturated blue
  // or red (bright to the eye, low in luminance) would be pushed down with the shadows.
  const float l = log2(max(0.5 * (LkY(x) + max(x.r, max(x.g, x.b))), 1e-7) / kLkMidGrey);
  const float l2 = l + (p.contrast - 1.0) * 2.5 * tanh(l / 2.5);
  x *= exp2(l2 - l);
  // Chroma in Oklab.
  const float3 lab0 = LkOklab(x);
  const float c = length(lab0.yz);
  const float h = atan2(lab0.z, lab0.y);
  static const float centres[6] = {0.506, 1.920, 2.478, -2.880, -1.676, -0.559};  // 29, 110, 142, 195, 264, 328 degrees
  float hk = 0.0, hw = 0.0;
  [unroll] for (int i = 0; i < 6; ++i) { const float w = LkHue(h, centres[i], 1.08); hk += w * p.hue[i]; hw += w; }
  float k = p.sat * (hw > 1e-3 ? hk / hw : 1.0);
  k *= 1.0 + p.vibrance * (1.0 - smoothstep(0.0, 0.16, c));
  // Greys and whites stay neutral, and so does a grey with a cast (a blue-grey room): saturation
  // separates colours, it must not deepen a cast.
  if (k > 1.0) k = lerp(1.0, k, smoothstep(0.01, 0.05, c));
  const float skin = LkSkin(lab0);
  k = lerp(k, 1.0 + (k - 1.0) * p.skinKeep, skin);
  k *= lerp(1.0, 1.0 - p.hiDesat, smoothstep(1.5, 4.5, l2));
  k *= lerp(1.0, 1.0 - p.loDesat, smoothstep(-3.0, -6.0, l2));
  float3 lab = float3(lab0.x, lab0.yz * k);
  // Split toning, growing with lightness so black stays black; skin keeps its own hue.
  const float hi = smoothstep(-0.5, 2.5, l2), lo = 1.0 - smoothstep(-4.0, -0.5, l2);
  lab.yz += (p.tintHi * hi + p.tintLo * lo) * (1.0 - 0.75 * skin) * min(lab0.x, 1.2);
  // Into gamut (no negative light), softly: chroma gained beyond the input's approaches the most
  // this lightness and hue can hold without reaching it, so colours already at the edge (a pure
  // blue) stay put instead of jumping, and nothing is clipped into flat patches.
  const float ct = length(lab.yz);
  if (ct > 1e-6) {
    const float2 dir = lab.yz / ct;
    const float cm = LkMaxChroma(lab0.x, dir);
    const float base = min(c, cm);
    const float co = ct <= base ? ct : base + (cm - base) * (1.0 - exp(-(ct - base) / max(cm - base, 1e-5)));
    lab.yz = dir * co;
  }
  return max(LkLinear(lab), 0.0);
}

// The frame with the network's edit e (log2 ratio per channel) and the chosen look. o is the
// game's linear frame; exposure brings its mean to mid grey.
float3 LookApply(float3 o, float3 e, uint look, float exposure) {
  if (look == 1u) {
    // 高还原: one ratio for all three channels keeps the original's chromaticity exactly; strong
    // edits are eased (to at most about 1.5 stops) and skin, where an edit shows first, takes less.
    // A gentle film contrast (one ratio too) gives it the same tonal depth as the other looks.
    const float s1 = max(exposure, 1e-8);
    const float skin = LkSkin(LkOklab(max(o, 0.0) * s1));
    const float y = 1.5 * tanh(LkY(e) / 1.5) * (1.0 - 0.3 * skin);
    const float3 x = o * exp2(y);
    const float l = log2(max(0.5 * (LkY(x) + max(x.r, max(x.g, x.b))) * s1, 1e-7) / kLkMidGrey);
    return x * exp2(0.10 * 2.5 * tanh(l / 2.5));
  }
  const float3 x = o * exp2(e);
  if (look == 0u || look > 4u) return x;
  const float s = max(exposure, 1e-8);
  return LkGrade(x * s, LkFor(look)) / s;
}
)"

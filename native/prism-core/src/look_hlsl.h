// Prism's looks (Look=0..4 in prism-core.ini), as HLSL source: part of Core's shaders
// (pipeline.cpp) and of tests/look_preview.cpp, so the preview runs exactly what ships.
//
// The grade happens in scene-referred light, before the game's own tone mapping, the way a
// colourist grades log footage: contrast in stops around mid grey on luminance alone, colour in
// Oklab (perceptual and hue-linear). Muted colours gain more than strong ones (vibrance), greys
// and whites stay neutral, skin takes only part of any change, highlights and deep shadows lose a
// little chroma as film does, and colours that would leave the gamut are compressed softly back
// into it (ACES-style) instead of being clipped.
//   0 默认    the network's result as it is
//   1 高还原  the original's colour: only the network's light and shade, softly limited, less on skin
//   2 高饱和  vivid and clean, for anime and stylised games
//   3 低饱和  restrained colour with cool, clean whites: the modern film and series look
//   4 浓烈    deep contrast and rich colour
//   5 自定义  every control below and more, set in the panel or the app (grade.h; constant buffer b1)
#pragma once

#define PRISM_LOOK_HLSL R"(
static const float kLkMidGrey = 0.18;
static const float kLkPi = 3.14159265;

// The custom look's controls (grade.h's GradeToShader packs them).
//   0 contrast, sat, vibrance, bias   4 highlights, shadows (stops), blacks, density
//   8 hiDesat, loDesat, skinKeep, skinAlign   12 skyDeepen, hiTwist, temperature, tint
//  16 tintLo (a, b), tintHi (a, b)   20-25 chroma by hue   26-31 hue turn (radians)   32-37 lightness by hue
//  38 tintMid (a, b)   40 whites, midtones (stops), vignette, grain   44 seed, mix, pivot (stops)
//  48-67 curves (stops added at -6, -3, 0, +3, +6): luminance, red, green, blue   68 curves on
cbuffer LkCustom : register(b1) { float4 LkG[18]; };
float LkK(uint i) { return LkG[i >> 2u][i & 3u]; }
// Where the pixel is (the caller sets them; the centre and no grain otherwise).
static float2 LkUv = 0.5;
static uint2 LkPx = 0;

// A curve through five points at -6, -3, 0, +3 and +6 stops (Catmull-Rom; grade.h's GradeCurveAt).
float LkCurve(uint at, float l) {
  const float f = clamp((l + 6.0) / 3.0, 0.0, 4.0);
  const uint i = min(uint(f), 3u);
  const float t = f - float(i);
  const float p0 = LkK(at + (i > 0u ? i - 1u : 0u)), p1 = LkK(at + i), p2 = LkK(at + i + 1u), p3 = LkK(at + min(i + 2u, 4u));
  return 0.5 * (2.0 * p1 + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
}

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

// A raised-cosine bump around a hue (radians, wrapping).
float LkHue(float h, float centre, float width) {
  float d = abs(h - centre);
  d = min(d, 2.0 * kLkPi - d);
  return d < width ? 0.5 + 0.5 * cos(kLkPi * d / width) : 0.0;
}

// Skin: warm hues between red and yellow (Oklab 40-64 degrees from pale to deep, the vectorscope's
// skin line at about 50), moderate chroma, not in deep shadow.
static const float kLkSkinHue = 0.873;  // 50 degrees
float LkSkin(float3 lab) {
  const float c = length(lab.yz);
  return LkHue(atan2(lab.z, lab.y), kLkSkinHue, 0.52) * smoothstep(0.018, 0.045, c) * (1.0 - smoothstep(0.15, 0.22, c)) * smoothstep(0.22, 0.38, lab.x);
}

// Into gamut (no negative light), softly, the way ACES's reference gamut compression does it: each
// channel's distance below the largest, relative to it (0 the largest itself, 1 no light, beyond 1
// negative), is compressed past the input's own distance (at least 0.75), approaching 1 without
// reaching it. Colours keep the saturation they came with and nothing is clipped into flat patches.
// It is continuous everywhere: Oklab's constant-lightness slices of the gamut are not convex
// around blue, so the most chroma a hue can hold jumps there, and fitting along hue made hard steps
// in saturated blue gradients.
float3 LkDist(float3 x) {
  const float a = max(x.r, max(x.g, x.b));
  return a > 1e-9 ? (a - x) / a : 0.0;
}
float3 LkCompress(float3 x, float3 dIn) {
  const float a = max(x.r, max(x.g, x.b));
  const float3 d = LkDist(x), t = clamp(dIn, 0.75, 0.98);
  const float3 dc = d <= t ? d : t + (1.0 - t) * (1.0 - exp(-(d - t) / (1.0 - t)));
  return a > 1e-9 ? a - dc * a : 0.0;
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
  float density;    // subtractive saturation: lightness given up per unit of chroma gained (film dyes)
  float skinAlign;  // the share of the way skin's hue moves toward the skin line
  float skyDeepen;  // lightness taken from bright blues and cyans (a polariser's sky)
  float bias;       // stops of exposure after the contrast (a restrained look sits lower, not milky)
  float hiTwist;    // radians: bright reds turn toward yellow and bright blues toward cyan (film)
  bool custom;      // the custom look: the controls below come from LkCustom
  float highlights; // stops added above mid grey
  float shadows;    // stops added below mid grey
  float blacks;     // positive lifts black to a dark grey, negative crushes toward pure black
  float temp;       // white balance: warm (+) or cool (-)
  float tint;       // white balance: magenta (+) or green (-)
  float hueTurn[6]; // radians, by hue (the same six as hue[])
  float hueLight[6];// lightness share added, by hue
};

LkParams LkFor(uint look) {
  LkParams p;
  p.contrast = 1.0; p.sat = 1.0; p.vibrance = 0.0; p.skinKeep = 1.0; p.hiDesat = 0.0; p.loDesat = 0.0;
  p.hue[0] = 1.0; p.hue[1] = 1.0; p.hue[2] = 1.0; p.hue[3] = 1.0; p.hue[4] = 1.0; p.hue[5] = 1.0;
  p.tintLo = 0.0; p.tintHi = 0.0; p.density = 0.0; p.skinAlign = 0.0; p.skyDeepen = 0.0; p.hiTwist = 0.0; p.bias = 0.0;
  p.highlights = 0.0; p.shadows = 0.0; p.blacks = 0.0; p.temp = 0.0; p.tint = 0.0; p.custom = look == 5u;
  [unroll] for (int i = 0; i < 6; ++i) { p.hueTurn[i] = 0.0; p.hueLight[i] = 0.0; }
  if (p.custom) {  // 自定义
    p.contrast = LkK(0); p.sat = LkK(1); p.vibrance = LkK(2); p.bias = LkK(3);
    p.highlights = LkK(4); p.shadows = LkK(5); p.blacks = LkK(6); p.density = LkK(7);
    p.hiDesat = LkK(8); p.loDesat = LkK(9); p.skinKeep = LkK(10); p.skinAlign = LkK(11);
    p.skyDeepen = LkK(12); p.hiTwist = LkK(13); p.temp = LkK(14); p.tint = LkK(15);
    p.tintLo = float2(LkK(16), LkK(17)); p.tintHi = float2(LkK(18), LkK(19));
    [unroll] for (uint j = 0u; j < 6u; ++j) { p.hue[j] = LkK(20u + j); p.hueTurn[j] = LkK(26u + j); p.hueLight[j] = LkK(32u + j); }
  } else if (look == 2u) {  // 高饱和
    p.contrast = 1.10; p.sat = 1.38; p.vibrance = 0.55; p.skinKeep = 0.40; p.hiDesat = 0.10; p.loDesat = 0.08;
    p.hue[0] = 0.92; p.hue[1] = 1.08; p.hue[2] = 1.12; p.hue[3] = 1.15; p.hue[4] = 1.00; p.hue[5] = 1.05;
    p.density = 0.35; p.skinAlign = 0.25; p.skyDeepen = 0.04; p.hiTwist = 0.10;
  } else if (look == 3u) {  // 低饱和
    // Restrained, not washed out: taking colour away brightens how a picture reads, so it sits
    // a fifth of a stop lower with deeper blacks, and highlights keep some of their colour.
    p.contrast = 1.24; p.sat = 0.66; p.vibrance = 0.0; p.skinKeep = 0.45; p.hiDesat = 0.12; p.loDesat = 0.25; p.bias = -0.2;
    p.hue[0] = 0.95; p.hue[1] = 0.85; p.hue[2] = 0.78; p.hue[3] = 1.00; p.hue[4] = 1.00; p.hue[5] = 0.85;
    p.tintLo = float2(-0.010, -0.016); p.tintHi = float2(-0.002, -0.007);
    p.skinAlign = 0.30; p.hiTwist = 0.08;
  } else if (look == 4u) {  // 浓烈
    p.contrast = 1.42; p.sat = 1.50; p.vibrance = 0.35; p.skinKeep = 0.40; p.hiDesat = 0.10; p.loDesat = 0.0;
    p.hue[0] = 0.95; p.hue[1] = 1.05; p.hue[2] = 1.08; p.hue[3] = 1.12; p.hue[4] = 1.08; p.hue[5] = 1.02;
    p.tintLo = float2(-0.008, -0.014); p.tintHi = float2(0.004, 0.012);
    p.density = 0.55; p.skinAlign = 0.35; p.skyDeepen = 0.07; p.hiTwist = 0.14;
  }
  return p;
}

// x: linear, exposure-normalised (the frame's mean at mid grey). Returns the graded colour.
float3 LkGrade(float3 x, LkParams p) {
  x = max(x, 0.0);
  // White balance: channel gains that keep luminance, so only the cast changes.
  if (p.temp != 0.0 || p.tint != 0.0) {
    const float3 g = exp2(float3(0.32 * p.temp + 0.12 * p.tint, -0.24 * p.tint, -0.40 * p.temp + 0.12 * p.tint));
    x *= g / LkY(g);
  }
  // Contrast in stops around mid grey, as one ratio for all channels (hue and chroma ratios stay).
  // It follows the mean of luminance and the largest channel: on luminance alone a saturated blue
  // or red (bright to the eye, low in luminance) would be pushed down with the shadows.
  const float l = log2(max(0.5 * (LkY(x) + max(x.r, max(x.g, x.b))), 1e-7) / kLkMidGrey);
  const float pivot = p.custom ? LkK(46) : 0.0;
  float l2 = l + (p.contrast - 1.0) * 2.5 * tanh((l - pivot) / 2.5);
  // Highlights and shadows: stops added on either side of mid grey, fading in over a few stops.
  l2 += p.highlights * smoothstep(0.0, 3.0, l2) + p.shadows * (1.0 - smoothstep(-4.0, 0.0, l2));
  if (p.custom) {
    // Whites (the top end), midtones (a bump around grey), the luminance curve and a vignette.
    l2 += LkK(40) * smoothstep(2.0, 6.0, l2) + LkK(41) * exp(-0.5 * l2 * l2 / 2.25);
    if (LkK(68) != 0.0) l2 += LkCurve(48u, l2);
    if (LkK(42) != 0.0) {
      const float2 d = (LkUv - 0.5) * float2(16.0 / 9.0, 1.0);
      l2 += LkK(42) * 1.5 * smoothstep(0.1, 1.0, dot(d, d) / 1.04);
    }
  }
  x *= exp2(l2 - l + p.bias);
  // The colour curves: each channel lifted or lowered by its own curve, read at its own level.
  if (p.custom && LkK(68) != 0.0) {
    const float3 lc = log2(max(x, 1e-7) / kLkMidGrey);
    x *= exp2(float3(LkCurve(53u, lc.r), LkCurve(58u, lc.g), LkCurve(63u, lc.b)));
  }
  // Black: a lift adds a little neutral light (black becomes a grey about 5.5 stops under mid grey);
  // a crush takes it away softly, as one ratio for all channels so dark colours keep their hue.
  if (p.blacks > 0.0) x += p.blacks * 0.004;
  else if (p.blacks < 0.0) { const float m = max(x.r, max(x.g, x.b)); x *= m / (m + -p.blacks * 0.004); }
  // Chroma in Oklab.
  const float3 dIn = LkDist(x);
  const float3 lab0 = LkOklab(x);
  const float c = length(lab0.yz);
  const float h = atan2(lab0.z, lab0.y);
  static const float centres[6] = {0.506, 1.920, 2.478, -2.880, -1.676, -0.559};  // 29, 110, 142, 195, 264, 328 degrees
  float hk = 0.0, hw = 0.0, ht = 0.0, hl = 0.0;
  [unroll] for (int i = 0; i < 6; ++i) {
    const float w = LkHue(h, centres[i], 1.08);
    hk += w * p.hue[i]; ht += w * p.hueTurn[i]; hl += w * p.hueLight[i]; hw += w;
  }
  ht = hw > 1e-3 ? ht / hw : 0.0; hl = hw > 1e-3 ? hl / hw : 0.0;
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
  // A turn of hue by hue, faded out toward grey; skin takes its share.
  const float colourful = smoothstep(0.01, 0.05, c), skinShare = lerp(1.0, p.skinKeep, skin);
  if (ht != 0.0) {
    const float turn = ht * colourful * skinShare, cs = cos(turn), sn = sin(turn);
    lab.yz = float2(lab.y * cs - lab.z * sn, lab.y * sn + lab.z * cs);
  }
  // Skin toward the skin line: a turn of its hue, the share by how much it reads as skin (too red
  // looks blotchy, too yellow pale).
  if (p.skinAlign > 0.0 && skin > 0.0) {
    float dh = kLkSkinHue - h;
    dh = dh > kLkPi ? dh - 2.0 * kLkPi : dh < -kLkPi ? dh + 2.0 * kLkPi : dh;
    const float turn = dh * p.skinAlign * skin, cs = cos(turn), sn = sin(turn);
    lab.yz = float2(lab.y * cs - lab.z * sn, lab.y * sn + lab.z * cs);
  }
  // Bright colours take film's hue path: a red pushed toward white passes through orange and a
  // blue through cyan (dye cross-talk; a flame that goes pink or a sky that goes violet reads as
  // video). Skin is left alone.
  if (p.hiTwist > 0.0) {
    const float turn = p.hiTwist * smoothstep(1.0, 4.0, l2) * smoothstep(0.03, 0.10, c) * (1.0 - skin) * (LkHue(h, 0.506, 0.9) - 0.6 * LkHue(h, -1.676, 0.9));
    const float cs = cos(turn), sn = sin(turn);
    lab.yz = float2(lab.y * cs - lab.z * sn, lab.y * sn + lab.z * cs);
  }
  // Split toning, growing with lightness so black stays black; skin keeps its own hue.
  const float hi = smoothstep(-0.5, 2.5, l2), lo = 1.0 - smoothstep(-4.0, -0.5, l2);
  const float mid = saturate(1.0 - hi - lo);
  const float2 tintMid = p.custom ? float2(LkK(38), LkK(39)) : 0.0;
  lab.yz += (p.tintHi * hi + p.tintLo * lo + tintMid * mid) * lerp(1.0 - 0.75 * skin, skinShare, p.custom ? 1.0 : 0.0) * min(lab0.x, 1.2);
  // Subtractive saturation: the chroma a colour really gains (what the gamut lets it keep) is paid
  // for in lightness, the way film dyes darken as they grow denser, so a richer colour reads deeper
  // instead of neon. The price follows how much brighter than grey each hue looks at the same
  // luminance (Helmholtz-Kohlrausch; Fairchild and Pirrotta's hue weight, yellow least, blue most),
  // so blues and magentas deepen and yellows stay clean instead of turning olive. Bright blues and
  // cyans give a little lightness too (a polarised sky).
  const float hkw = (0.116 * abs(sin((h - 1.920) * 0.5)) + 0.085) / 0.159;
  const float gained = max(length(LkOklab(LkCompress(LkLinear(float3(lab0.x, lab.yz)), dIn)).yz) - c, 0.0) * hkw;
  const float sky = LkHue(h, -1.676, 0.9) * smoothstep(0.03, 0.10, c) * smoothstep(0.55, 0.85, lab0.x);
  lab.x = lab0.x - p.density * gained * (1.0 - 0.5 * skin) - p.skyDeepen * sky * lab0.x;
  lab.x *= 1.0 + hl * colourful * skinShare;
  // Film grain: a per-frame hash, in lightness, strongest in the mid tones.
  if (p.custom && LkK(43) > 0.0) {
    uint n = LkPx.x * 1973u + LkPx.y * 9277u + uint(LkK(44)) * 26699u;
    n = (n << 13u) ^ n; n = n * (n * n * 15731u + 789221u) + 1376312589u;
    const float r = float(n & 0x7fffffffu) / 2147483647.0 - 0.5;
    lab.x *= 1.0 + LkK(43) * 0.22 * r * (0.35 + 0.65 * mid);
  }
  return max(LkCompress(LkLinear(lab), dIn), 0.0);
}

// The frame with the network's edit e (log2 ratio per channel) and the chosen look. o is the
// game's linear frame; exposure brings its mean to mid grey.
float3 LookApply(float3 o, float3 e, uint look, float exposure) {
  float3 result = o * exp2(e);  // 默认
  const float s = max(exposure, 1e-8);
  if (look == 1u) {
    // 高还原: one ratio for all three channels keeps the original's chromaticity exactly; strong
    // edits are eased (to at most about 1.5 stops) and skin, where an edit shows first, takes less.
    // A gentle film contrast (one ratio too) gives it the same tonal depth as the other looks.
    const float skin = LkSkin(LkOklab(max(o, 0.0) * s));
    const float y = 1.5 * tanh(LkY(e) / 1.5) * (1.0 - 0.3 * skin);
    const float3 x = o * exp2(y);
    const float l = log2(max(0.5 * (LkY(x) + max(x.r, max(x.g, x.b))) * s, 1e-7) / kLkMidGrey);
    result = x * exp2(0.10 * 2.5 * tanh(l / 2.5));
  } else if (look >= 2u && look <= 4u) {
    result = LkGrade(result * s, LkFor(look)) / s;
  } else if (look == 5u) {
    result = lerp(result, LkGrade(result * s, LkFor(look)) / s, LkK(45));
  }
  return result;
}
)"

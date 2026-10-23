/**
 * The custom look (Look=5, 自定义), as native/prism-core/src/grade.h has it: the same controls, keys,
 * ranges and ini lines (Grade=Contrast:24,Sat:-34,...: only what differs from neutral; three slots),
 * the same packing into the shader's parameters (the app's preview runs a WebGL port of the shader)
 * and the same starting points. The selftest checks this table against grade.h.
 */

export type GradeUnit = 'signed' | 'plain' | 'stops' | 'hue' | 'turn';
export interface GradeSpec { key: string; label: string; min: number; max: number; def: number; step: number; unit: GradeUnit; help: string }

export const HUE_NAMES = ['红', '黄', '绿', '青', '蓝', '品红'];
export const HUE_CENTRES = [29, 110, 142, 195, 264, 328]; // Oklab degrees
export const CURVE_CHANNELS = ['亮度', '红', '绿', '蓝'];
export const CURVE_POINTS = ['最暗', '暗部', '中灰', '亮部', '最亮'];
export const CURVE_STOPS = [-6, -3, 0, 3, 6];
export const GRADE_SLOTS = 3;

const s = (key: string, label: string, help: string, min = -100, max = 100, def = 0): GradeSpec => ({ key, label, min, max, def, step: 1, unit: 'signed', help });
const p = (key: string, label: string, help: string, def = 0): GradeSpec => ({ key, label, min: 0, max: 100, def, step: 1, unit: 'plain', help });
const hue = (key: string, label: string, help: string, def: number): GradeSpec => ({ key, label, min: 0, max: 360, def, step: 5, unit: 'hue', help });
const stops = (key: string, label: string, help: string, min: number, max: number): GradeSpec => ({ key, label, min, max, def: 0, step: 0.05, unit: 'stops', help });

/** In grade.h's order (its enum Grade). */
export const GRADE: GradeSpec[] = [
  stops('Exposure', '曝光', '整体亮度，单位是档（EV）：+1 亮一倍。在游戏自己的色调映射之前调，高光不会被硬切。', -3, 3),
  s('Contrast', '对比', '以对比轴心为中心拉开或收拢明暗，只动亮度、不动颜色比例，所以色相不偏。'),
  stops('Pivot', '对比轴心', '对比围绕的亮度，单位是档：负值让暗部也参与拉开，正值只拉开亮处。', -2, 2),
  s('Highlights', '高光', '只调比中灰亮的部分：负值找回天空和灯光的层次，正值让亮处更通透。'),
  s('Shadows', '阴影', '只调比中灰暗的部分：正值提亮暗部细节，负值压暗让画面更沉。'),
  s('Whites', '白位', '最亮的一档：负值把灯光和太阳附近收回来，正值让最亮处更耀眼。'),
  s('Blacks', '黑位', '最深的黑：正值抬起成深灰（胶片的灰黑），负值压到更纯的黑。'),
  s('Midtones', '中间调', '中灰附近的亮度：正值更明亮通透，负值更沉，最亮和最暗处基本不动。'),
  s('Temperature', '色温', '白平衡的冷暖：正值偏暖（黄），负值偏冷（蓝）。整体亮度保持不变。'),
  s('Tint', '色调', '白平衡的绿与品红：正值偏品红，负值偏绿。用来去掉画面的偏色。'),
  s('Saturation', '饱和', '所有颜色的浓度。灰和白始终保持中性，-100 是黑白。'),
  s('Vibrance', '自然饱和', '越灰的颜色加得越多、已经浓的颜色几乎不动，比饱和更不容易过头。'),
  p('SkinProtect', '肤色保护', '皮肤少跟一些颜色改动（饱和、色相、明度、染色）：100 时皮肤完全不动，0 时和其他颜色一样。', 50),
  p('SkinAlign', '肤色校正', '把偏红或偏黄的皮肤往自然肤色线上拉。偏红显得斑驳，偏黄显得苍白。'),
  p('Mix', '整体强度', '整套调色的浓淡：100 是调好的样子，0 回到网络原样的颜色。', 100),
  hue('ShadowHue', '暗部色相', '三向色轮的暗部：暗处染上的颜色，色相环上的空心方块。电影常用青蓝。', 210),
  p('ShadowAmount', '暗部染色', '暗部染色的浓度。越接近黑越淡，纯黑始终是黑。'),
  hue('MidHue', '中间色相', '三向色轮的中间调：中灰附近染上的颜色，色相环上的十字。', 40),
  p('MidAmount', '中间染色', '中间调染色的浓度，影响面最大，少量就够。'),
  hue('HighlightHue', '亮部色相', '三向色轮的亮部：亮处染上的颜色，色相环上的实心方块。常用暖黄或橙。', 60),
  p('HighlightAmount', '亮部染色', '亮部染色的浓度。'),
  ...['Red', 'Yellow', 'Green', 'Cyan', 'Blue', 'Magenta'].map((h, i): GradeSpec => ({ key: `Hue${h}`, label: HUE_NAMES[i], min: -30, max: 30, def: 0, step: 1, unit: 'turn', help: '把这一类颜色的色相转向相邻的颜色，单位是度。' })),
  ...['Red', 'Yellow', 'Green', 'Cyan', 'Blue', 'Magenta'].map((h, i) => s(`Sat${h}`, HUE_NAMES[i], '这一类颜色的饱和度，-100 时这类颜色变灰。')),
  ...['Red', 'Yellow', 'Green', 'Cyan', 'Blue', 'Magenta'].map((h, i) => s(`Lum${h}`, HUE_NAMES[i], '这一类颜色的明度：比如压暗蓝色让天空更深，提亮绿色让草木更鲜。')),
  ...['Y', 'R', 'G', 'B'].flatMap((c, ci) =>
    CURVE_POINTS.map((label, i) =>
      s(`Curve${c}${i + 1}`, label, ci === 0 ? '亮度曲线上的一个点：在这一档亮度上整体变亮或变暗，颜色比例不变。点之间平滑过渡。' : '这个通道的曲线上的一个点：只抬高或压低这一档亮度里的这个颜色，用来给暗部、高光分别校色。'),
    ),
  ),
  p('Density', '胶片密度', '颜色变浓时同时变深，像胶片染料：浓的颜色显得厚重，不会像荧光色。蓝和品红最明显，黄色基本不变。'),
  p('HighlightTwist', '高光走向', '很亮的红转向橙、很亮的蓝转向青，像胶片：火焰不会发粉，天空不会发紫。皮肤不受影响。'),
  p('Sky', '天空加深', '压暗明亮的蓝和青，像偏振镜下的天空。'),
  p('HighlightDesat', '高光去色', '很亮的地方褪掉一些颜色，像胶片高光走向白色，而不是浓到发假。'),
  p('ShadowDesat', '暗部去色', '很暗的地方褪掉一些颜色，暗部更干净，不会有彩色噪点感。'),
  s('Vignette', '暗角', '画面四角的亮度：负值压暗四角、把视线引向中间，正值提亮四角。'),
  p('Grain', '颗粒', '胶片颗粒，每帧变化，中间调最明显。少量能盖住色带，让画面更有质感。'),
];
export const G = Object.fromEntries(GRADE.map((g, i) => [g.key, i])) as Record<string, number>;
export const G_HUE_SHIFT = G.HueRed, G_HUE_SAT = G.SatRed, G_HUE_LIGHT = G.LumRed, G_CURVE = G.CurveY1;

export type GradeValues = number[];
export const neutralGrade = (): GradeValues => GRADE.map((g) => g.def);

/** The six pages, as the in-game desk has them. */
export const GRADE_PAGES: { name: string; keys: string[] }[] = [
  { name: '影调', keys: ['Exposure', 'Contrast', 'Pivot', 'Highlights', 'Shadows', 'Whites', 'Blacks', 'Midtones'] },
  { name: '曲线', keys: [] },
  { name: '色彩', keys: ['Temperature', 'Tint', 'Saturation', 'Vibrance', 'SkinProtect', 'SkinAlign', 'Mix'] },
  { name: '色轮', keys: ['ShadowHue', 'ShadowAmount', 'MidHue', 'MidAmount', 'HighlightHue', 'HighlightAmount'] },
  { name: '色相', keys: [] },
  { name: '胶片', keys: ['Density', 'HighlightTwist', 'Sky', 'HighlightDesat', 'ShadowDesat', 'Vignette', 'Grain'] },
];

export function gradeFormat(v: GradeValues): string {
  return GRADE.map((g, i) => (Math.abs(v[i] - g.def) < 1e-4 ? '' : `${g.key}:${g.unit === 'stops' ? v[i].toFixed(2) : Math.round(v[i])}`))
    .filter(Boolean)
    .join(',');
}
export function gradeParse(text: string | undefined): GradeValues {
  const v = neutralGrade();
  for (const item of (text ?? '').split(',')) {
    const m = item.trim().match(/^(\w+):\s*(-?[\d.]+)$/);
    if (!m) continue;
    const i = GRADE.findIndex((g) => g.key.toLowerCase() === m[1].toLowerCase());
    if (i >= 0 && Number.isFinite(Number(m[2]))) v[i] = Math.max(GRADE[i].min, Math.min(GRADE[i].max, Number(m[2])));
  }
  return v;
}

/** The shader's 72 parameters (grade.h's GradeToShader). */
export function gradeToShader(v: GradeValues): Float32Array {
  const k = new Float32Array(72);
  const x = (key: string) => v[G[key]] / 100;
  const deg = Math.PI / 180;
  k[0] = 1 + 0.6 * x('Contrast'); k[1] = 1 + x('Saturation'); k[2] = x('Vibrance'); k[3] = v[G.Exposure];
  k[4] = 1.5 * x('Highlights'); k[5] = 1.5 * x('Shadows'); k[6] = x('Blacks'); k[7] = x('Density');
  k[8] = 0.5 * x('HighlightDesat'); k[9] = 0.6 * x('ShadowDesat'); k[10] = 1 - x('SkinProtect'); k[11] = 0.6 * x('SkinAlign');
  k[12] = 0.15 * x('Sky'); k[13] = 0.25 * x('HighlightTwist'); k[14] = x('Temperature'); k[15] = x('Tint');
  const wheel = (at: number, h: string, a: string, scale: number) => { const m = scale * x(a); k[at] = m * Math.cos(v[G[h]] * deg); k[at + 1] = m * Math.sin(v[G[h]] * deg); };
  wheel(16, 'ShadowHue', 'ShadowAmount', 0.03);
  wheel(18, 'HighlightHue', 'HighlightAmount', 0.02);
  for (let h = 0; h < 6; h++) { k[20 + h] = 1 + v[G_HUE_SAT + h] / 100; k[26 + h] = v[G_HUE_SHIFT + h] * deg; k[32 + h] = 0.3 * v[G_HUE_LIGHT + h] / 100; }
  wheel(38, 'MidHue', 'MidAmount', 0.02);
  k[40] = 1.5 * x('Whites'); k[41] = x('Midtones'); k[42] = x('Vignette'); k[43] = x('Grain'); k[45] = x('Mix'); k[46] = v[G.Pivot];
  let curves = false;
  for (let i = 0; i < 20; i++) { k[48 + i] = 1.5 * v[G_CURVE + i] / 100; curves ||= v[G_CURVE + i] !== 0; }
  k[68] = curves ? 1 : 0;
  return k;
}

/** A curve through five points (stops in, stops added): Catmull-Rom, flat past the ends. */
export function curveAt(pts: ArrayLike<number>, at: number, l: number): number {
  if (l <= CURVE_STOPS[0]) return pts[at];
  if (l >= CURVE_STOPS[4]) return pts[at + 4];
  const f = (l + 6) / 3, i = Math.min(Math.floor(f), 3), t = f - i;
  const p0 = pts[at + Math.max(i - 1, 0)], p1 = pts[at + i], p2 = pts[at + i + 1], p3 = pts[at + Math.min(i + 2, 4)];
  return 0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}
const ss = (a: number, b: number, x: number) => { const t = Math.max(0, Math.min(1, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
/** Stops out for stops in (from mid grey), luminance only: the tone curve the desk draws. */
export function gradeTone(k: Float32Array, l: number): number {
  let o = l + (k[0] - 1) * 2.5 * Math.tanh((l - k[46]) / 2.5);
  o += k[4] * ss(0, 3, o) + k[5] * (1 - ss(-4, 0, o));
  o += k[40] * ss(2, 6, o) + k[41] * Math.exp((-0.5 * o * o) / 2.25);
  o += curveAt(k, 48, o) + k[3];
  let y = 0.18 * 2 ** o;
  if (k[6] > 0) y += k[6] * 0.004;
  else if (k[6] < 0) y *= y / (y - k[6] * 0.004);
  return Math.log2(Math.max(y, 1e-7) / 0.18);
}
/** A by-hue control at a hue (degrees), blended as the shader blends the six. */
export function byHue(v: GradeValues, base: number, deg: number): number {
  let sum = 0, wsum = 0;
  for (let i = 0; i < 6; i++) {
    let d = Math.abs(deg - HUE_CENTRES[i]) % 360;
    d = Math.min(d, 360 - d);
    const w = d < 61.9 ? 0.5 + 0.5 * Math.cos((Math.PI * d) / 61.9) : 0;
    sum += w * v[base + i]; wsum += w;
  }
  return wsum > 1e-3 ? sum / wsum : 0;
}

/** A preset as a starting point (grade.h's GradeFromPreset): 中性, 高饱和, 低饱和, 浓烈. */
export function gradeFromPreset(which: number): GradeValues {
  const v = neutralGrade();
  const r = Math.round;
  const split = (h: string, a: string, ta: number, tb: number, scale: number) => {
    const m = Math.hypot(ta, tb);
    if (m < 1e-6) return;
    let d = (Math.atan2(tb, ta) * 180) / Math.PI;
    if (d < 0) d += 360;
    v[G[h]] = r(d); v[G[a]] = r(Math.min(100, (m / scale) * 100));
  };
  const set = (contrast: number, sat: number, vib: number, skinKeep: number, hiD: number, loD: number, hues: number[], tlA: number, tlB: number, thA: number, thB: number, density: number, skinAlign: number, sky: number, bias: number, twist: number) => {
    v[G.Contrast] = r(((contrast - 1) / 0.6) * 100); v[G.Saturation] = r((sat - 1) * 100); v[G.Vibrance] = r(vib * 100);
    v[G.SkinProtect] = r((1 - skinKeep) * 100); v[G.HighlightDesat] = r((hiD / 0.5) * 100); v[G.ShadowDesat] = r((loD / 0.6) * 100);
    hues.forEach((h, i) => (v[G_HUE_SAT + i] = r((h - 1) * 100)));
    split('ShadowHue', 'ShadowAmount', tlA, tlB, 0.03); split('HighlightHue', 'HighlightAmount', thA, thB, 0.02);
    v[G.Density] = r(density * 100); v[G.SkinAlign] = r((skinAlign / 0.6) * 100); v[G.Sky] = r((sky / 0.15) * 100);
    v[G.Exposure] = bias; v[G.HighlightTwist] = r((twist / 0.25) * 100);
  };
  // Math.fround: the native side computes these in single precision.
  const f = Math.fround;
  if (which === 1) set(f(1.1), f(1.38), f(0.55), f(0.4), f(0.1), f(0.08), [0.92, 1.08, 1.12, 1.15, 1, 1.05].map(f), 0, 0, 0, 0, f(0.35), f(0.25), f(0.04), 0, f(0.1));
  else if (which === 2) set(f(1.24), f(0.66), 0, f(0.45), f(0.12), f(0.25), [0.95, 0.85, 0.78, 1, 1, 0.85].map(f), f(-0.01), f(-0.016), f(-0.002), f(-0.007), 0, f(0.3), 0, -0.2, f(0.08));
  else if (which === 3) set(f(1.42), f(1.5), f(0.35), f(0.4), f(0.1), 0, [0.95, 1.05, 1.08, 1.12, 1.08, 1.02].map(f), f(-0.008), f(-0.014), f(0.004), f(0.012), f(0.55), f(0.35), f(0.07), 0, f(0.14));
  return v;
}

export function gradeText(i: number, x: number): string {
  const u = GRADE[i].unit;
  if (u === 'stops') return Math.abs(x) < 0.005 ? '0.00' : `${x > 0 ? '+' : ''}${x.toFixed(2)}`;
  if (u === 'hue') return `${Math.round(x)}°`;
  if (u === 'turn') return Math.abs(x) < 0.5 ? '0°' : `${x > 0 ? '+' : ''}${Math.round(x)}°`;
  if (u === 'signed') return Math.abs(x) < 0.5 ? '0' : `${x > 0 ? '+' : ''}${Math.round(x)}`;
  return String(Math.round(x));
}

/** An Oklab hue's colour as CSS (lightness and chroma fixed). */
export function hueCss(deg: number, L = 0.75, C = 0.13): string {
  return `oklch(${(L * 100).toFixed(1)}% ${C} ${deg.toFixed(1)})`;
}

/** The app's values for the custom look: three slots and the one in use. */
export const GRADE_VALUE_KEYS = ['prism-core.grade', 'prism-core.grade2', 'prism-core.grade3', 'prism-core.gradeSlot'];
export const gradeValueKey = (slot: number) => GRADE_VALUE_KEYS[Math.max(0, Math.min(2, slot))];

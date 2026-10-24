import { useCallback, useEffect, useId, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { encodeGradeCode, type GradeCodeGame } from '../../electron/shared/gradecode';
import { api } from '../api';
import {
  byHue, curveAt, CURVE_CHANNELS, CURVE_STOPS, G, G_CURVE, G_HUE_LIGHT, G_HUE_SAT, G_HUE_SHIFT, GRADE, GRADE_PAGES, GRADE_SLOTS,
  gradeFormat, gradeFromPreset, gradeParse, gradeText, gradeTone, gradeToShader, gradeValueKey, HUE_CENTRES, hueCss, neutralGrade, type GradeValues,
} from '../../electron/shared/grade';

type CoreValues = Record<string, number | boolean | string>;

interface Props {
  gameName: string;
  /** The game's facts a grade code carries (name, executable, Steam id). */
  codeGame: GradeCodeGame;
  values: CoreValues;
  /** Writes through to prism-core.ini (debounced by the caller). */
  set: (patch: CoreValues) => void;
  status: string;
  onClose: () => void;
}

const PRESET_NAMES = ['中性', '高饱和', '低饱和', '浓烈'];
const CHANNEL_INK = ['#ffffff', hueCss(29, 0.68, 0.2), hueCss(142, 0.75, 0.2), hueCss(264, 0.62, 0.2)];
const WHEELS = [
  { hue: 'ShadowHue', amt: 'ShadowAmount', name: '暗部' },
  { hue: 'MidHue', amt: 'MidAmount', name: '中间' },
  { hue: 'HighlightHue', amt: 'HighlightAmount', name: '亮部' },
];

/**
 * The grading desk: the custom look's 66 controls over six pages, three slots, a preset to start
 * from, the tone curve and hue ring (both draggable), and a live preview that runs the same grade
 * as the game (a WebGL port of look_hlsl.h) on a chart or a screenshot of your own.
 */
export function GradeDesk({ gameName, codeGame, values, set, status, onClose }: Props) {
  const slot = Math.max(0, Math.min(GRADE_SLOTS - 1, Math.round(Number(values['prism-core.gradeSlot'] ?? 0)) || 0));
  const grade = useMemo(() => gradeParse(String(values[gradeValueKey(slot)] ?? '')), [values, slot]);
  const [page, setPage] = useState(0);
  const [channel, setChannel] = useState(0);
  const [attr, setAttr] = useState(1);
  const [help, setHelp] = useState<string | null>(null);
  const [wheel, setWheel] = useState(0);

  const write = useCallback((next: GradeValues) => set({ [gradeValueKey(slot)]: gradeFormat(next), 'prism-core.look': '5' }), [set, slot]);
  const update = useCallback((i: number, v: number) => { const next = grade.slice(); next[i] = v; write(next); }, [grade, write]);
  const updateMany = useCallback((patch: Record<number, number>) => { const next = grade.slice(); for (const [i, v] of Object.entries(patch)) next[Number(i)] = v; write(next); }, [grade, write]);
  const match = PRESET_NAMES.findIndex((_, i) => gradeFormat(gradeFromPreset(i)) === gradeFormat(grade));
  const touched = GRADE.filter((g, i) => Math.abs(grade[i] - g.def) > 1e-4).length;

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose(); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onClose]);

  const rows: number[] =
    page === 1 ? [0, 1, 2, 3, 4].map((i) => G_CURVE + channel * 5 + i)
      : page === 4 ? [0, 1, 2, 3, 4, 5].map((i) => (attr === 0 ? G_HUE_SHIFT : attr === 1 ? G_HUE_SAT : G_HUE_LIGHT) + i)
        : GRADE_PAGES[page].keys.map((k) => G[k]);
  const pageReset = () => { const n = neutralGrade(); updateMany(Object.fromEntries(rows.map((i) => [i, n[i]]))); };

  return createPortal(
    <div className="grade-desk" role="dialog" aria-modal="true" aria-label="调色台">
      <header className="grade-top">
        <div className="grade-title">
          <h1>调色台</h1>
          <span className="dim">{gameName} · 自定义色彩 · 已调整 {touched} 项</span>
        </div>
        <div className="segmented segmented-3 grade-slots" role="radiogroup" aria-label="方案">
          {Array.from({ length: GRADE_SLOTS }, (_, i) => (
            <button key={i} role="radio" aria-checked={slot === i} className={slot === i ? 'is-on' : ''} onClick={() => set({ 'prism-core.gradeSlot': String(i), 'prism-core.look': '5' })}>
              方案 {i + 1}
            </button>
          ))}
        </div>
        <ExportShare code={encodeGradeCode(codeGame, grade)} />
        <button className="btn btn-line" onClick={onClose}>完成</button>
      </header>

      <div className="grade-body">
        <div className="grade-stage">
          <Preview grade={grade} />
          <div className="grade-scopes">
            <ToneScope grade={grade} page={page} channel={channel} onPoint={(i, v) => update(G_CURVE + channel * 5 + i, v)} />
            <HueRing grade={grade} focusWheel={page === 3 ? wheel : -1} onWheel={(w, h, a) => { setWheel(w); updateMany({ [G[WHEELS[w].hue]]: h, [G[WHEELS[w].amt]]: a }); }} />
          </div>
        </div>

        <aside className="grade-side">
          <div className="segmented grade-pages" role="tablist" aria-label="分页">
            {GRADE_PAGES.map((pg, i) => (
              <button key={pg.name} role="tab" aria-selected={page === i} className={page === i ? 'is-on' : ''} onClick={() => setPage(i)}>
                {pg.name}
              </button>
            ))}
          </div>

          <div className="grade-rows">
            {page === 1 && (
              <div className="grade-switch">
                <span>通道</span>
                <div className="segmented segmented-4" role="radiogroup" aria-label="曲线通道">
                  {CURVE_CHANNELS.map((c, i) => (
                    <button key={c} role="radio" aria-checked={channel === i} className={channel === i ? 'is-on' : ''} onClick={() => setChannel(i)}>{c}</button>
                  ))}
                </div>
              </div>
            )}
            {page === 4 && (
              <div className="grade-switch">
                <span>调整</span>
                <div className="segmented segmented-3" role="radiogroup" aria-label="色相页的属性">
                  {['色相', '饱和', '明度'].map((c, i) => (
                    <button key={c} role="radio" aria-checked={attr === i} className={attr === i ? 'is-on' : ''} onClick={() => setAttr(i)}>{c}</button>
                  ))}
                </div>
              </div>
            )}
            {rows.map((i) => (
              <GradeRow
                key={i}
                index={i}
                value={grade[i]}
                mark={i >= G_HUE_SHIFT && i < G_CURVE ? hueCss(HUE_CENTRES[(i - G_HUE_SHIFT) % 6], 0.72, 0.14) : i >= G_CURVE && i < G_CURVE + 20 ? CHANNEL_INK[Math.floor((i - G_CURVE) / 5)] : undefined}
                onChange={(v) => update(i, v)}
                onFocus={() => { setHelp(GRADE[i].help); const w = WHEELS.findIndex((x) => G[x.hue] === i || G[x.amt] === i); if (w >= 0) setWheel(w); }}
              />
            ))}
          </div>

          <div className="grade-presets">
            <span>起点</span>
            <div className="segmented segmented-4" role="radiogroup" aria-label="从预设开始">
              {PRESET_NAMES.map((n, i) => (
                <button key={n} role="radio" aria-checked={match === i} className={match === i ? 'is-on' : ''} onClick={() => write(gradeFromPreset(i))}>{n}</button>
              ))}
            </div>
          </div>

          <p className="block-note grade-help">{help ?? '把鼠标移到一项上看它的作用。双击或按 Delete 把一项恢复到中性；曲线的点和色轮的标记可以直接在左边拖动。'}</p>
          <div className="grade-foot">
            <p className="block-note" role="status" aria-live="polite">{status}</p>
            <button className="btn btn-line btn-small" onClick={pageReset}>重置本页</button>
          </div>
        </aside>
      </div>
    </div>,
    document.body,
  );
}

// ---------------------------------------------------------------------------

function GradeRow({ index, value, mark, onChange, onFocus }: { index: number; value: number; mark?: string; onChange: (v: number) => void; onFocus: () => void }) {
  const id = useId();
  const g = GRADE[index];
  const ticks = 41;
  const t = (value - g.min) / (g.max - g.min), c0 = g.min < 0 ? -g.min / (g.max - g.min) : 0;
  const lo = Math.min(t, c0), hi = Math.max(t, c0);
  const neutral = Math.abs(value - g.def) < 1e-4;
  const fine = g.unit === 'stops' ? 0.01 : 1;
  return (
    <div className={`grade-row ${neutral ? '' : 'is-set'}`} onPointerEnter={onFocus} onFocus={onFocus} onDoubleClick={() => onChange(g.def)}>
      <label htmlFor={id}>
        {mark && <i className="grade-mark" style={{ background: mark }} aria-hidden="true" />}
        {g.label}
      </label>
      <div className="grade-track">
        <span className="grade-ticks" aria-hidden="true">
          {Array.from({ length: ticks }, (_, k) => {
            const f = k / (ticks - 1);
            const at = Math.abs(f - t) <= 0.5 / (ticks - 1);
            if (g.unit === 'hue') return <i key={k} className={at ? 'is-value' : ''} style={at ? undefined : { background: hueCss(g.min + f * (g.max - g.min), 0.72, 0.12) }} />;
            const on = hi - lo > 1e-4 && f >= lo - 0.25 / ticks && f <= hi + 0.25 / ticks;
            const ref = g.min < 0 && Math.abs(f - c0) < 0.5 / (ticks - 1);
            return <i key={k} className={on ? 'is-on' : ref ? 'is-ref' : ''} />;
          })}
        </span>
        <input
          id={id}
          type="range"
          min={g.min}
          max={g.max}
          step={fine}
          value={value}
          aria-valuetext={gradeText(index, value)}
          onChange={(e) => onChange(Number(e.target.value))}
          onKeyDown={(e) => { if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); onChange(g.def); } }}
        />
      </div>
      <span className="grade-value mono">{gradeText(index, value)}</span>
    </div>
  );
}

// ---------------------------------------------------------------------------

/** The tone curve as a bar run (stops in, stops out, -8 to +6), with the colour curves' caps. */
function ToneScope({ grade, page, channel, onPoint }: { grade: GradeValues; page: number; channel: number; onPoint: (i: number, v: number) => void }) {
  const W = 340, H = 196, pad = 8;
  const k = useMemo(() => gradeToShader(grade), [grade]);
  const mapX = (l: number) => pad + ((l + 8) / 14) * (W - 2 * pad);
  const mapY = (o: number) => H - pad - Math.max(0, Math.min(1, (o + 8) / 14)) * (H - 2 * pad);
  const bars = 84, bw = (W - 2 * pad) / bars;
  const on = [true, 1, 2, 3].map((c, i) => i === 0 || [0, 1, 2, 3, 4].some((p) => grade[G_CURVE + i * 5 + p] !== 0));
  const drag = useRef<{ i: number; y: number; from: number } | null>(null);
  const svg = useRef<SVGSVGElement>(null);
  const scale = () => (svg.current ? svg.current.getBoundingClientRect().height / H : 1);
  return (
    <figure className="grade-scope">
      <figcaption><span>影调曲线</span><span className="dim mono">−8 → +6 档</span></figcaption>
      <svg ref={svg} viewBox={`0 0 ${W} ${H}`} role="img" aria-label="影调曲线"
        onPointerMove={(e) => {
          const d = drag.current;
          if (!d) return;
          const perStop = ((H - 2 * pad) / 14) * scale();
          const v = Math.max(-100, Math.min(100, Math.round(d.from - ((e.clientY - d.y) / perStop / 1.5) * 100)));
          onPoint(d.i, v);
        }}
        onPointerUp={() => (drag.current = null)}>
        <line x1={pad} x2={W - pad} y1={mapY(0)} y2={mapY(0)} className="scope-faint" />
        <line x1={mapX(0)} x2={mapX(0)} y1={pad} y2={H - pad} className="scope-faint" />
        {Array.from({ length: bars }, (_, i) => {
          const l = -8 + ((i + 0.5) / bars) * 14, o = gradeTone(k, l), x = pad + i * bw;
          return (
            <g key={i}>
              <rect x={x} y={mapY(o)} width={bw * 0.6} height={H - pad - mapY(o)} fill="#262626" />
              {i % 2 === 0 && <rect x={x} y={mapY(l)} width={bw * 0.6} height={1} fill="#5c5c5c" />}
              {[1, 2, 3].filter((c) => on[c]).map((c) => <rect key={c} x={x} y={mapY(o + curveAt(k, 48 + c * 5, o))} width={bw * 0.6} height={2} fill={CHANNEL_INK[c]} />)}
              <rect x={x} y={mapY(o)} width={bw * 0.6} height={2} fill="#fff" />
            </g>
          );
        })}
        {page === 1 && CURVE_STOPS.map((l, i) => {
          const o = gradeTone(k, l), y = mapY(channel === 0 ? o : o + curveAt(k, 48 + channel * 5, o));
          return (
            <rect key={i} x={mapX(l) - 5} y={y - 5} width={10} height={10} className="scope-point" style={{ stroke: CHANNEL_INK[channel] }}
              onPointerDown={(e) => { (e.target as Element).setPointerCapture(e.pointerId); drag.current = { i, y: e.clientY, from: grade[G_CURVE + channel * 5 + i] }; }}
              onPointerUp={(e) => { (e.target as Element).releasePointerCapture(e.pointerId); drag.current = null; }}
              onPointerMove={(e) => {
                const d = drag.current;
                if (!d) return;
                const perStop = ((H - 2 * pad) / 14) * scale();
                onPoint(d.i, Math.max(-100, Math.min(100, Math.round(d.from - ((e.clientY - d.y) / perStop / 1.5) * 100))));
              }} />
          );
        })}
      </svg>
    </figure>
  );
}

/** The hue ring: a spoke per hue (its saturation gain, turned by its hue shift) and the wheels. */
function HueRing({ grade, focusWheel, onWheel }: { grade: GradeValues; focusWheel: number; onWheel: (w: number, hue: number, amt: number) => void }) {
  const S = 196, c = S / 2, R = 78, r0 = 8;
  const svg = useRef<SVGSVGElement>(null);
  const dragging = useRef(-1);
  const at = (deg: number, rad: number) => [c + Math.cos((deg * Math.PI) / 180) * rad, c - Math.sin((deg * Math.PI) / 180) * rad];
  const sat = 1 + grade[G.Saturation] / 100;
  const pick = (e: React.PointerEvent, w: number) => {
    const b = svg.current!.getBoundingClientRect();
    const x = ((e.clientX - b.left) / b.width) * S - c, y = ((e.clientY - b.top) / b.height) * S - c;
    let deg = (Math.atan2(-y, x) * 180) / Math.PI;
    if (deg < 0) deg += 360;
    onWheel(w, Math.round(deg) % 360, Math.max(0, Math.min(100, Math.round(((Math.hypot(x, y) - r0) / (R - r0)) * 100))));
  };
  return (
    <figure className="grade-scope">
      <figcaption><span>色相环</span><span className="dim">长短是饱和 · 方块是色轮</span></figcaption>
      <svg ref={svg} viewBox={`0 0 ${S} ${S}`} role="img" aria-label="色相环"
        onPointerDown={(e) => { const w = focusWheel >= 0 ? focusWheel : 1; dragging.current = w; (e.currentTarget as Element).setPointerCapture(e.pointerId); pick(e, w); }}
        onPointerMove={(e) => { if (dragging.current >= 0) pick(e, dragging.current); }}
        onPointerUp={() => (dragging.current = -1)}>
        {Array.from({ length: 72 }, (_, j) => {
          const [ax, ay] = at(j * 5, R + 5), [bx, by] = at(j * 5 + 5.5, R + 5), [cx, cy] = at(j * 5 + 5.5, R + 8), [dx, dy] = at(j * 5, R + 8);
          return <path key={j} d={`M${ax} ${ay}L${bx} ${by}L${cx} ${cy}L${dx} ${dy}Z`} fill={hueCss(j * 5 + 2.5, 0.72, 0.13)} />;
        })}
        {Array.from({ length: 36 }, (_, j) => { const [x, y] = at(j * 10, r0 + (R - r0) * 0.5); return <rect key={j} x={x - 0.6} y={y - 0.6} width={1.2} height={1.2} fill="#5c5c5c" />; })}
        {Array.from({ length: 72 }, (_, j) => {
          const h = j * 5 + 2.5;
          const gain = Math.max(0, sat * (1 + byHue(grade, G_HUE_SAT, h) / 100));
          const turn = byHue(grade, G_HUE_SHIFT, h), light = byHue(grade, G_HUE_LIGHT, h) / 100;
          const [x1, y1] = at(h + turn, r0), [x2, y2] = at(h + turn, r0 + (R - r0) * Math.min(1, gain / 2));
          return <line key={j} x1={x1} y1={y1} x2={x2} y2={y2} stroke={hueCss(h + turn, Math.max(0.3, Math.min(0.95, 0.74 * (1 + 0.3 * light))), 0.14)} strokeWidth={2} />;
        })}
        {WHEELS.map((w, i) => {
          const [x, y] = at(grade[G[w.hue]], r0 + ((R - r0) * grade[G[w.amt]]) / 100);
          const s = focusWheel === i ? 6 : 4.5;
          return (
            <g key={w.name} className="scope-wheel" onPointerDown={(e) => { e.stopPropagation(); dragging.current = i; svg.current!.setPointerCapture(e.pointerId); }}>
              {i === 0 && <rect x={x - s} y={y - s} width={s * 2} height={s * 2} fill="#000" stroke="#fff" strokeWidth={2} />}
              {i === 2 && <rect x={x - s} y={y - s} width={s * 2} height={s * 2} fill="#fff" stroke="#000" strokeWidth={1} />}
              {i === 1 && <path d={`M${x - s} ${y}H${x + s}M${x} ${y - s}V${y + s}`} stroke="#fff" strokeWidth={2} />}
              <title>{w.name}</title>
            </g>
          );
        })}
      </svg>
    </figure>
  );
}

// ---------------------------------------------------------------------------

/** The colour chart the preview grades by default (tests/look_preview.cpp's, at 16:10). */
function drawChart(cv: HTMLCanvasElement) {
  cv.width = 1600; cv.height = 1000;
  const x = cv.getContext('2d')!;
  x.fillStyle = 'rgb(40,40,40)'; x.fillRect(0, 0, 1600, 1000);
  const cc = [[115, 82, 68], [194, 150, 130], [98, 122, 157], [87, 108, 67], [133, 128, 177], [103, 189, 170], [214, 126, 44], [80, 91, 166], [193, 90, 99], [94, 60, 108], [157, 188, 64], [224, 163, 46],
    [56, 61, 150], [70, 148, 73], [175, 54, 60], [231, 199, 31], [187, 86, 149], [8, 133, 161], [243, 243, 242], [200, 200, 200], [160, 160, 160], [122, 122, 121], [85, 85, 85], [52, 52, 52]];
  cc.forEach((c, i) => { x.fillStyle = `rgb(${c})`; x.fillRect(40 + (i % 6) * 170, 40 + Math.floor(i / 6) * 130, 160, 120); });
  [[255, 224, 196], [241, 194, 167], [224, 172, 138], [198, 142, 110], [170, 112, 82], [141, 85, 62], [110, 66, 48], [75, 46, 36]].forEach((c, i) => { x.fillStyle = `rgb(${c})`; x.fillRect(1080, 40 + i * 64, 480, 58); });
  const grey = x.createLinearGradient(40, 0, 1560, 0); grey.addColorStop(0, '#000'); grey.addColorStop(1, '#fff');
  x.fillStyle = grey; x.fillRect(40, 600, 1520, 80);
  for (let px = 40; px < 1560; px++) {
    const h = ((px - 40) / 1520) * 360;
    x.fillStyle = `hsl(${h} 100% 43%)`; x.fillRect(px, 700, 1, 60);
    x.fillStyle = `hsl(${h} 50% 64%)`; x.fillRect(px, 760, 1, 60);
  }
  const sky = x.createLinearGradient(0, 840, 0, 960); sky.addColorStop(0, 'rgb(60,110,200)'); sky.addColorStop(1, 'rgb(200,220,245)');
  x.fillStyle = sky; x.fillRect(40, 840, 1520, 120);
}

const VS = `#version 300 es
in vec2 p; out vec2 uv;
void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }`;

// look_hlsl.h's LkGrade for the custom look, in GLSL (K: grade.ts's gradeToShader), and the
// preview tool's way between a display-referred still and scene light.
const FS = `#version 300 es
precision highp float;
uniform sampler2D src; uniform float K[72]; uniform float split; uniform float seed; uniform vec2 size;
in vec2 uv; out vec4 outColour;
const float PI = 3.14159265, SKIN = 0.873;
float mx(vec3 x) { return max(x.r, max(x.g, x.b)); }
vec3 Dec(vec3 c) { c = max(c, 0.0); return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, vec3(lessThanEqual(c, vec3(0.04045)))); }
vec3 Enc(vec3 c) { c = max(c, 0.0); return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, vec3(lessThanEqual(c, vec3(0.0031308)))); }
vec3 ToScene(vec3 s) { vec3 c = Dec(s); float m = min(mx(c), 0.985); return c / (1.0 - m); }
vec3 ToDisplay(vec3 v) { v = max(v, 0.0); return Enc(v / (1.0 + mx(v))); }
vec3 Oklab(vec3 c) {
  vec3 l = vec3(dot(c, vec3(0.4122214708, 0.5363325363, 0.0514459929)), dot(c, vec3(0.2119034982, 0.6806995451, 0.1073969566)), dot(c, vec3(0.0883024619, 0.2817188376, 0.6299787005)));
  l = sign(l) * pow(abs(l), vec3(1.0 / 3.0));
  return vec3(dot(l, vec3(0.2104542553, 0.7936177850, -0.0040720468)), dot(l, vec3(1.9779984951, -2.4285922050, 0.4505937099)), dot(l, vec3(0.0259040371, 0.7827717662, -0.8086757660)));
}
vec3 Linear(vec3 b) {
  vec3 l = vec3(b.x + 0.3963377774 * b.y + 0.2158037573 * b.z, b.x - 0.1055613458 * b.y - 0.0638541728 * b.z, b.x - 0.0894841775 * b.y - 1.2914855480 * b.z);
  l = l * l * l;
  return vec3(dot(l, vec3(4.0767416621, -3.3077115913, 0.2309699292)), dot(l, vec3(-1.2684380046, 2.6097574011, -0.3413193965)), dot(l, vec3(-0.0041960863, -0.7034186147, 1.7076147010)));
}
float Y(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float Hue(float h, float c, float w) { float d = abs(h - c); d = min(d, 2.0 * PI - d); return d < w ? 0.5 + 0.5 * cos(PI * d / w) : 0.0; }
float Skin(vec3 b) { float c = length(b.yz); return Hue(atan(b.z, b.y), SKIN, 0.52) * smoothstep(0.018, 0.045, c) * (1.0 - smoothstep(0.15, 0.22, c)) * smoothstep(0.22, 0.38, b.x); }
vec3 Dist(vec3 x) { float a = mx(x); return a > 1e-9 ? (a - x) / a : vec3(0.0); }
vec3 Compress(vec3 x, vec3 dIn) {
  float a = mx(x); vec3 d = Dist(x), t = clamp(dIn, 0.75, 0.98);
  vec3 dc = mix(t + (1.0 - t) * (1.0 - exp(-(d - t) / (1.0 - t))), d, vec3(lessThanEqual(d, t)));
  return a > 1e-9 ? a - dc * a : vec3(0.0);
}
vec2 Rot(vec2 v, float a) { float c = cos(a), s = sin(a); return vec2(v.x * c - v.y * s, v.x * s + v.y * c); }
float Curve(int at, float l) {
  float f = clamp((l + 6.0) / 3.0, 0.0, 4.0); int i = min(int(f), 3); float t = f - float(i);
  float p0 = K[at + max(i - 1, 0)], p1 = K[at + i], p2 = K[at + i + 1], p3 = K[at + min(i + 2, 4)];
  return 0.5 * (2.0 * p1 + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
}
vec3 Grade(vec3 x) {
  x = max(x, 0.0);
  if (K[14] != 0.0 || K[15] != 0.0) { vec3 g = exp2(vec3(0.32 * K[14] + 0.12 * K[15], -0.24 * K[15], -0.40 * K[14] + 0.12 * K[15])); x *= g / Y(g); }
  float l = log2(max(0.5 * (Y(x) + mx(x)), 1e-7) / 0.18);
  float l2 = l + (K[0] - 1.0) * 2.5 * tanh((l - K[46]) / 2.5);
  l2 += K[4] * smoothstep(0.0, 3.0, l2) + K[5] * (1.0 - smoothstep(-4.0, 0.0, l2));
  l2 += K[40] * smoothstep(2.0, 6.0, l2) + K[41] * exp(-0.5 * l2 * l2 / 2.25);
  if (K[68] != 0.0) l2 += Curve(48, l2);
  if (K[42] != 0.0) { vec2 d = (uv - 0.5) * vec2(16.0 / 9.0, 1.0); l2 += K[42] * 1.5 * smoothstep(0.1, 1.0, dot(d, d) / 1.04); }
  x *= exp2(l2 - l + K[3]);
  if (K[68] != 0.0) { vec3 lc = log2(max(x, 1e-7) / 0.18); x *= exp2(vec3(Curve(53, lc.r), Curve(58, lc.g), Curve(63, lc.b))); }
  if (K[6] > 0.0) x += K[6] * 0.004; else if (K[6] < 0.0) { float m = mx(x); x *= m / (m - K[6] * 0.004); }
  vec3 dIn = Dist(x), lab0 = Oklab(x);
  float c = length(lab0.yz), h = atan(lab0.z, lab0.y);
  float centres[6] = float[6](0.506, 1.920, 2.478, -2.880, -1.676, -0.559);
  float hk = 0.0, hw = 0.0, ht = 0.0, hl = 0.0;
  for (int i = 0; i < 6; ++i) { float w = Hue(h, centres[i], 1.08); hk += w * K[20 + i]; ht += w * K[26 + i]; hl += w * K[32 + i]; hw += w; }
  ht = hw > 1e-3 ? ht / hw : 0.0; hl = hw > 1e-3 ? hl / hw : 0.0;
  float k = K[1] * (hw > 1e-3 ? hk / hw : 1.0);
  k *= 1.0 + K[2] * (1.0 - smoothstep(0.0, 0.16, c));
  if (k > 1.0) k = mix(1.0, k, smoothstep(0.01, 0.05, c));
  float skin = Skin(lab0);
  k = mix(k, 1.0 + (k - 1.0) * K[10], skin);
  k *= mix(1.0, 1.0 - K[8], smoothstep(1.5, 4.5, l2));
  k *= mix(1.0, 1.0 - K[9], 1.0 - smoothstep(-6.0, -3.0, l2));
  vec3 lab = vec3(lab0.x, lab0.yz * k);
  float colourful = smoothstep(0.01, 0.05, c), skinShare = mix(1.0, K[10], skin);
  if (ht != 0.0) lab.yz = Rot(lab.yz, ht * colourful * skinShare);
  if (K[11] > 0.0 && skin > 0.0) { float dh = SKIN - h; dh = dh > PI ? dh - 2.0 * PI : dh < -PI ? dh + 2.0 * PI : dh; lab.yz = Rot(lab.yz, dh * K[11] * skin); }
  if (K[13] > 0.0) lab.yz = Rot(lab.yz, K[13] * smoothstep(1.0, 4.0, l2) * smoothstep(0.03, 0.10, c) * (1.0 - skin) * (Hue(h, 0.506, 0.9) - 0.6 * Hue(h, -1.676, 0.9)));
  float hi = smoothstep(-0.5, 2.5, l2), lo = 1.0 - smoothstep(-4.0, -0.5, l2), mid = clamp(1.0 - hi - lo, 0.0, 1.0);
  lab.yz += (vec2(K[18], K[19]) * hi + vec2(K[16], K[17]) * lo + vec2(K[38], K[39]) * mid) * skinShare * min(lab0.x, 1.2);
  float hkw = (0.116 * abs(sin((h - 1.920) * 0.5)) + 0.085) / 0.159;
  float gained = max(length(Oklab(Compress(Linear(vec3(lab0.x, lab.yz)), dIn)).yz) - c, 0.0) * hkw;
  float sky = Hue(h, -1.676, 0.9) * smoothstep(0.03, 0.10, c) * smoothstep(0.55, 0.85, lab0.x);
  lab.x = lab0.x - K[7] * gained * (1.0 - 0.5 * skin) - K[12] * sky * lab0.x;
  lab.x *= 1.0 + hl * colourful * skinShare;
  if (K[43] > 0.0) {
    uvec2 px = uvec2(uv * size);
    uint n = px.x * 1973u + px.y * 9277u + uint(seed) * 26699u;
    n = (n << 13u) ^ n; n = n * (n * n * 15731u + 789221u) + 1376312589u;
    float r = float(n & 0x7fffffffu) / 2147483647.0 - 0.5;
    lab.x *= 1.0 + K[43] * 0.22 * r * (0.35 + 0.65 * mid);
  }
  return max(Compress(Linear(lab), dIn), 0.0);
}
void main() {
  vec3 a = ToScene(texture(src, vec2(uv.x, 1.0 - uv.y)).rgb);
  vec3 r = uv.x < split ? a : mix(a, Grade(a), K[45]);
  outColour = vec4(ToDisplay(r), 1.0);
}`;

/** The live preview: the grade on the chart or a still of your own, original left of the split. */
function Preview({ grade }: { grade: GradeValues }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const gl = useRef<{ g: WebGL2RenderingContext; prog: WebGLProgram; tex: WebGLTexture; w: number; h: number } | null>(null);
  const [split, setSplit] = useState(0.5);
  const [source, setSource] = useState<'chart' | 'image'>('chart');
  const [failed, setFailed] = useState(false);
  const k = useMemo(() => gradeToShader(grade), [grade]);
  const file = useRef<HTMLInputElement>(null);
  const draggingSplit = useRef(false);

  const upload = useCallback((img: TexImageSource, w: number, h: number) => {
    const s = gl.current;
    if (!s) return;
    s.g.bindTexture(s.g.TEXTURE_2D, s.tex);
    s.g.texImage2D(s.g.TEXTURE_2D, 0, s.g.RGBA8, s.g.RGBA, s.g.UNSIGNED_BYTE, img);
    s.w = w; s.h = h;
  }, []);

  useEffect(() => {
    const cv = canvas.current!;
    const g = cv.getContext('webgl2', { antialias: false, preserveDrawingBuffer: false });
    if (!g) { setFailed(true); return; }
    const sh = (type: number, src: string) => { const x = g.createShader(type)!; g.shaderSource(x, src); g.compileShader(x); if (!g.getShaderParameter(x, g.COMPILE_STATUS)) throw new Error(g.getShaderInfoLog(x) ?? 'shader'); return x; };
    try {
      const prog = g.createProgram()!;
      g.attachShader(prog, sh(g.VERTEX_SHADER, VS)); g.attachShader(prog, sh(g.FRAGMENT_SHADER, FS)); g.linkProgram(prog);
      if (!g.getProgramParameter(prog, g.LINK_STATUS)) throw new Error(g.getProgramInfoLog(prog) ?? 'link');
      const buf = g.createBuffer(); g.bindBuffer(g.ARRAY_BUFFER, buf);
      g.bufferData(g.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), g.STATIC_DRAW);
      const loc = g.getAttribLocation(prog, 'p'); g.enableVertexAttribArray(loc); g.vertexAttribPointer(loc, 2, g.FLOAT, false, 0, 0);
      const tex = g.createTexture()!; g.bindTexture(g.TEXTURE_2D, tex);
      g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MIN_FILTER, g.LINEAR); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MAG_FILTER, g.LINEAR);
      g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_S, g.CLAMP_TO_EDGE); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_T, g.CLAMP_TO_EDGE);
      gl.current = { g, prog, tex, w: 1600, h: 1000 };
      const chart = document.createElement('canvas'); drawChart(chart); upload(chart, 1600, 1000);
    } catch (e) {
      console.error(e);
      setFailed(true);
    }
  }, [upload]);

  // Draw on every change, and keep drawing while there is grain (it moves every frame).
  useEffect(() => {
    let raf = 0, frame = 0;
    const draw = () => {
      const s = gl.current, cv = canvas.current;
      if (!s || !cv) return;
      const b = cv.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
      const w = Math.max(1, Math.round(b.width * dpr)), h = Math.max(1, Math.round(b.height * dpr));
      if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
      const { g, prog } = s;
      g.viewport(0, 0, w, h);
      g.useProgram(prog);
      g.uniform1fv(g.getUniformLocation(prog, 'K'), k);
      g.uniform1f(g.getUniformLocation(prog, 'split'), split);
      g.uniform1f(g.getUniformLocation(prog, 'seed'), frame++ & 1023);
      g.uniform2f(g.getUniformLocation(prog, 'size'), w, h);
      g.drawArrays(g.TRIANGLE_STRIP, 0, 4);
      if (k[43] > 0) raf = requestAnimationFrame(draw);
    };
    draw();
    const ro = new ResizeObserver(() => draw());
    if (canvas.current) ro.observe(canvas.current);
    return () => { cancelAnimationFrame(raf); ro.disconnect(); };
  }, [k, split]);

  const load = (f: File | undefined) => {
    if (!f || !f.type.startsWith('image/')) return;
    const img = new Image();
    img.onload = () => { upload(img, img.naturalWidth, img.naturalHeight); setSource('image'); setSplit((v) => v + 1e-6); URL.revokeObjectURL(img.src); };
    img.src = URL.createObjectURL(f);
  };
  const useChart = () => { const chart = document.createElement('canvas'); drawChart(chart); upload(chart, 1600, 1000); setSource('chart'); setSplit((v) => v + 1e-6); };
  const aspect = gl.current ? gl.current.w / gl.current.h : 1.6;

  return (
    <div className="grade-preview">
      <div className="grade-preview-bar">
        <div className="segmented segmented-2" role="radiogroup" aria-label="预览图">
          <button role="radio" aria-checked={source === 'chart'} className={source === 'chart' ? 'is-on' : ''} onClick={useChart}>色卡</button>
          <button role="radio" aria-checked={source === 'image'} className={source === 'image' ? 'is-on' : ''} onClick={() => file.current?.click()}>载入截图</button>
        </div>
        <input ref={file} type="file" accept="image/png,image/jpeg,image/webp,image/bmp" hidden onChange={(e) => load(e.target.files?.[0])} />
        <span className="dim">预览用同一套算法；游戏里调色作用在神经渲染之后的画面上。可以把截图拖进来。</span>
      </div>
      <div className="grade-canvas-wrap" style={{ aspectRatio: String(aspect) }}
        onDragOver={(e) => e.preventDefault()} onDrop={(e) => { e.preventDefault(); load(e.dataTransfer.files[0]); }}
        onPointerDown={(e) => { draggingSplit.current = true; (e.currentTarget as Element).setPointerCapture(e.pointerId); const b = e.currentTarget.getBoundingClientRect(); setSplit(Math.max(0, Math.min(1, (e.clientX - b.left) / b.width))); }}
        onPointerMove={(e) => { if (!draggingSplit.current) return; const b = e.currentTarget.getBoundingClientRect(); setSplit(Math.max(0, Math.min(1, (e.clientX - b.left) / b.width))); }}
        onPointerUp={() => (draggingSplit.current = false)}>
        {failed ? <p className="block-note grade-nogl">这台电脑的显卡驱动没有提供 WebGL2，预览不可用；调色本身照常写入游戏。</p> : <canvas ref={canvas} aria-label="调色预览：左边原画，右边自定义色彩" />}
        <span className="grade-split" style={{ left: `${split * 100}%` }} aria-hidden="true" />
        <span className="grade-tag grade-tag-left">原画</span>
        <span className="grade-tag grade-tag-right">自定义</span>
      </div>
    </div>
  );
}

// ---------------------------------------------------------------------------

/** The slot's look as a grade code: copied, or shared to the community (with an account). */
function ExportShare({ code }: { code: string }) {
  const [copied, setCopied] = useState(false);
  const [open, setOpen] = useState(false);
  const [title, setTitle] = useState('');
  const [description, setDescription] = useState('');
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [signedIn, setSignedIn] = useState<boolean | null>(null);
  useEffect(() => {
    if (open) api.account.state().then((s) => setSignedIn(s.signedIn)).catch(() => setSignedIn(false));
  }, [open]);
  const share = async () => {
    setBusy(true);
    setError(null);
    try {
      await api.community.share({ title, description, code });
      setNote('已分享到社区，所有人都能看到并一键导入。');
      setTitle('');
      setDescription('');
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return (
    <div className="grade-export">
      <button className="btn btn-line btn-small" onClick={() => void navigator.clipboard.writeText(code).then(() => { setCopied(true); setTimeout(() => setCopied(false), 1600); })}>
        {copied ? '已复制调色码' : '导出调色码'}
      </button>
      <button className="btn btn-line btn-small" aria-expanded={open} onClick={() => { setOpen(!open); setNote(null); setError(null); }}>分享到社区</button>
      {open && (
        <div className="grade-share" role="dialog" aria-label="分享到社区">
          {signedIn === false ? (
            <p className="block-text">登录以后才能分享：在左侧栏点"登录"。不登录也可以点"导出调色码"，把码直接发给朋友。</p>
          ) : (
            <form className="acct-form" onSubmit={(e) => { e.preventDefault(); void share(); }}>
              <label className="acct-field">
                <span className="acct-label">名字</span>
                <input autoFocus maxLength={40} value={title} onChange={(e) => setTitle(e.target.value)} placeholder="例如：雨夜胶片" />
              </label>
              <label className="acct-field">
                <span className="acct-label">说明（选填）</span>
                <textarea maxLength={300} rows={3} value={description} onChange={(e) => setDescription(e.target.value)} placeholder="适合什么场景、调了哪里" />
              </label>
              <p className="acct-hint">分享的是当前方案的调色，会标上这个游戏，大家导入时会自动对上。</p>
              {error && <p className="foot-error" role="alert">{error}</p>}
              {note && <p className="block-note" role="status">{note}</p>}
              <div className="acct-row">
                <button className="btn btn-line btn-small" type="submit" disabled={busy || !title.trim()}>{busy ? '正在分享' : '分享'}</button>
                <button className="btn btn-line btn-small" type="button" onClick={() => setOpen(false)}>关闭</button>
              </div>
            </form>
          )}
        </div>
      )}
    </div>
  );
}

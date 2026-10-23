import * as fs from 'node:fs';
import * as path from 'node:path';
import { netScaleFor, netSizeFor, pluginById, PLUGINS, PRESETS, resolveValues, tierLocked } from '../shared/catalog';
import { matchProfile, styleById } from '../shared/profiles';
import type {
  BackupRecord,
  ComponentInfo,
  FileOp,
  GameAnalysis,
  GameEntry,
  GpuInfo,
  InstallPlan,
  InstallProgress,
  InstallRecord,
  NvidiaDll,
  PlanNeed,
  PlanRequest,
  LegacyPresetId,
  PresetId,
  Route,
} from '../shared/types';
import { componentFile, componentFiles, ensureComponent, getComponent, isBundled, SOURCES } from './components';
import { bestDll } from './nvidia';
import { dirs, store } from './store';
import { ensureDir, exists, log, readText, sha256File, sha256Text } from './util';

let shaderDir = '';
export function setShaderDir(dir: string): void {
  shaderDir = dir;
}

// Prism Core's two binaries ship with the app (extraResources), built from native/prism-core.
let coreDir = '';
export function setCoreDir(dir: string): void {
  coreDir = dir;
}

type Values = Record<string, number | boolean | string>;

const MANIFEST = 'prism-nr.manifest.json';
const PRESET_FILE = 'PrismNR.ini';
const SHADER_SUBDIR = path.join('reshade-shaders', 'Shaders', 'PrismNR');
const CORE_INI = 'prism-core.ini';
const CORE_FILES = ['version.dll', 'prism_nvngx.dll'];

/** A version.dll that loads prism_nvngx.dll is Prism Core's loader; anything else belongs to another mod. */
function isPrismLoader(file: string): boolean {
  try {
    return fs.readFileSync(file).includes(Buffer.from('prism_nvngx.dll', 'utf16le'));
  } catch {
    return false;
  }
}

/** DLSS 5 add-ons that hook the same NGX calls as Prism Core; two hooks make one of them fail silently. */
function conflictingAddons(exeDir: string): string[] {
  try {
    return fs
      .readdirSync(exeDir)
      .filter((f) => /dlss5.*\.addon(64|32)$/i.test(f))
      .map((f) => path.join(exeDir, f));
  } catch {
    return [];
  }
}

/** The in-game panel's own lines in prism-core.ini (its place, its frame-rate corner, its key, the split's column, the target frame rate). */
const PANEL_KEY = /^(Overlay\w+|KeyOverlay|SplitPos|NetScaleAuto|NetScaleMin|FpsTarget)=/i;

/** The panel's lines of an existing prism-core.ini, so a rewrite by the app keeps them. */
export function panelLines(file: string): string[] {
  try {
    return fs.readFileSync(file, 'utf8').split(/\r?\n/).map((l) => l.trim()).filter((l) => PANEL_KEY.test(l));
  } catch {
    return [];
  }
}

/** prism-core.ini: Prism Core reads it at start and again whenever it changes. ASCII only. */
export function coreIni(values: Values, keep: string[] = []): string {
  const pct = (k: string, d: number, max = 100) => (Math.max(0, Math.min(max, Number(values[k] ?? d))) / 100).toFixed(2);
  const cadence = Math.max(1, Math.min(4, Math.round(Number(values['prism-core.cadence'] ?? 2))));
  // Every frame runs synchronously (the app's async switch is off there): no carry, no trail.
  const async = cadence === 1 ? false : values['prism-core.async'] ?? true;
  const passes = Math.max(1, Math.min(5, Math.round(Number(values['prism-core.passes'] ?? 2))));
  return [
    '; Written by Prism NR. Prism Core re-reads this file while the game runs.',
    '[Prism Core]',
    'Enabled=1',
    `Cadence=${cadence}`,
    `Async=${async === true || async === '1' || async === 1 ? 1 : 0}`,
    `Intensity=${pct('prism-core.intensity', 200, 300)}`,
    `Guard=${pct('prism-core.guard', 100)}`,
    `Passes=${passes}`,
    `Guided=${values['prism-core.guided'] === true || values['prism-core.guided'] === 1 || values['prism-core.guided'] === '1' ? 1 : 0}`,
    // The network's size only applies to every-frame tiers; the asynchronous network runs native.
    `NetScale=${cadence === 1 ? netScaleFor(values['prism-core.netSize']).toFixed(4) : '1.00'}`,
    `Look=${Math.max(0, Math.min(4, Math.round(Number(values['prism-core.look'] ?? 0)) || 0))}`,
    'KeyToggle=F8',
    'KeyCadence=F2',
    'KeyAsync=F3',
    'KeySplit=F4',
    'KeyCapture=F11',
    ...(keep.some((l) => /^KeyOverlay=/i.test(l)) ? [] : ['KeyOverlay=Home']),
    ...keep,
    '',
  ].join('\r\n');
}

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

/** The Core tier for the GPU: high on flagship and high-end cards, lighter below. */
function recommendedPreset(gpu: GpuInfo | null): PresetId {
  switch (gpu?.tier) {
    case 'flagship':
      return 'flagship';
    case 'high':
      return 'high';
    case 'mainstream':
      return 'standard';
    default:
      return 'light';
  }
}

/** Prism Core is the only route: games it can't run on are blocked, with the reason. */
function chooseRoute(analysis: GameAnalysis): Route {
  if (analysis.recommendedRoute === 'blocked') return 'blocked';
  return analysis.routes.includes('prism-core') ? 'prism-core' : 'blocked';
}

/** Swaps plugins that do not fit the chosen route for ones that do. */
function pluginsForRoute(route: Route, wanted: string[]): string[] {
  if (route === 'prism-core') {
    // Core needs no ReShade, so only plugins that work without it stay (frame generation).
    const keep = new Set(wanted.filter((id) => pluginById(id)?.routes.includes('prism-core')));
    keep.add('nr-prism-core');
    return PLUGINS.filter((p) => keep.has(p.id)).map((p) => p.id);
  }
  const set = new Set(wanted);
  if (route === 'optiscaler') {
    set.delete('nr-renodx');
    set.delete('nr-dfc');
    set.add('nr-optiscaler');
  } else {
    if (set.has('nr-optiscaler')) {
      set.delete('nr-optiscaler');
      set.add('nr-renodx');
    }
    if (!set.has('nr-renodx') && !set.has('nr-dfc')) set.add('nr-renodx');
  }
  if (route === 'feeder') {
    set.add('feeder');
    set.add('motion-lumenite');
    set.add('prism-capture');
  } else {
    set.delete('feeder');
    set.delete('motion-lumenite');
  }
  // Drop Prism plugins whose route list excludes this route (Guard needs the Feeder reference).
  for (const id of [...set]) {
    const p = pluginById(id);
    if (p && !p.routes.includes(route)) set.delete(id);
  }
  for (const id of [...set]) for (const req of pluginById(id)?.requires ?? []) set.add(req);
  return PLUGINS.filter((p) => set.has(p.id)).map((p) => p.id);
}

function componentIdsFor(route: Route, plugins: string[]): string[] {
  if (route === 'prism-core') return [];
  const ids: string[] = [];
  const reshadeNeeded = route !== 'optiscaler' || plugins.some((p) => pluginById(p)?.origin === 'prism');
  if (reshadeNeeded) ids.push('reshade');
  if (route === 'optiscaler') ids.push('optiscaler-presr');
  if (plugins.includes('nr-renodx')) ids.push('renodx-dlss5');
  if (plugins.includes('nr-dfc')) ids.push('dfc');
  if (plugins.includes('feeder')) ids.push('feeder');
  if (plugins.includes('motion-lumenite')) ids.push('lumenite');
  if (plugins.includes('mgpu')) ids.push('mgpu');
  return ids;
}

export function buildPlan(game: GameEntry, analysis: GameAnalysis, gpu: GpuInfo | null, dlls: NvidiaDll[], req: PlanRequest): InstallPlan {
  const profile = matchProfile(game.name, game.steamAppId);
  const savedStyle = store.settings().styles[game.id];
  const style = styleById(req.style ?? savedStyle ?? profile?.style ?? 'realistic');
  // Prism Core only (the add-on routes were removed in 1.2): a tier picks the Core values.
  const fits = (id: PresetId | undefined) => Boolean(id) && PRESETS.some((p) => p.id === id && !tierLocked(p, gpu?.name));
  const preset: PresetId = req.preset && PRESETS.some((p) => p.id === req.preset) ? req.preset : fits(profile?.coreTier) ? profile!.coreTier! : recommendedPreset(gpu);
  const route = chooseRoute(analysis);
  const plugins = pluginsForRoute('prism-core', req.plugins ?? ['nr-prism-core']);
  const values: Values = { ...resolveValues(preset), ...(profile?.coreValues ?? {}), ...(req.overrides ?? {}) };

  const blockers: string[] = [];
  const warnings = [...analysis.warnings];
  const manualSteps: string[] = [];
  const steps: string[] = [];

  if (analysis.recommendedRoute === 'blocked') blockers.push(analysis.routeReason);
  else if (route === 'blocked') blockers.push('这个游戏用不了 Prism Core：它需要 DX12、64 位并且自带 DLSS 的游戏。');
  if (!analysis.exe) blockers.push('没有找到游戏主程序。');
  const lock = tierLocked(PRESETS.find((p) => p.id === preset)!, gpu?.name);
  if (lock) blockers.push(lock);
  if (Number(values['prism-core.passes'] ?? 2) >= 3) warnings.push('3 遍以上是实验性质的游乐场：开销成倍增加，画面容易过度，不建议实际游玩。');
  if (route === 'prism-core' && analysis.exe) {
    const exeDir = path.dirname(analysis.exe);
    const loader = path.join(exeDir, 'version.dll');
    if (exists(loader) && !isPrismLoader(loader))
      blockers.push('游戏目录里已有别的 version.dll（可能是其他 Mod）。Prism Core 的加载器也叫这个名字，为避免弄坏那个 Mod，这次不安装。');
    if (CORE_FILES.some((f) => !exists(path.join(coreDir, f)))) blockers.push('这个 Prism NR 安装包里缺少 Prism Core 文件，请重新安装 Prism NR。');
    if (analysis.importsVersionDll === false)
      warnings.push('主程序没有直接导入 version.dll，Prism Core 可能不会被加载。启用后进一次游戏，游戏目录里出现 prism-core.log 就说明加载成功。');
    const ours = new Set(store.installs().filter((r) => r.gameId === game.id && r.status === 'active').flatMap((r) => r.files.map((x) => x.target.toLowerCase())));
    const addons = conflictingAddons(exeDir).filter((a) => !ours.has(a.toLowerCase()));
    if (addons.length) warnings.push(`检测到 ${addons.map((a) => path.basename(a)).join('、')}，它和 Prism Core 挂钩同一个 DLSS 调用。启用时会先移走，还原时放回。`);
    warnings.push('Prism Core 是实验性预览，目前在 CONTROL Resonant 和地平线 6 上实测过。');
  }
  if (gpu && gpu.nrSupport === 'none') blockers.push(gpu.expectation);
  if (gpu?.nrSupport === 'experimental') warnings.push(gpu.expectation);

  const nr = bestDll(dlls, 'nvngx_dlssnr.dll', true);
  if (!nr) blockers.push('没有找到可用的神经渲染文件 nvngx_dlssnr.dll（安装包自带的副本缺失或签名无效）。请重新安装 Prism NR。');
  else if (!nr.signed) warnings.push(`将使用本机的未签名版 nvngx_dlssnr.dll（${nr.version ?? '未知版本'}，可能是社区修改版）。找到 NVIDIA 签名版后 Prism 会自动改用签名版。`);
  // Prism Core downloads nothing: the runtime and NVIDIA's model ship inside the installer.
  const needs: PlanNeed[] = [];
  if (existsAnyPrevious(game.id)) steps.push('还原这个游戏上一次的 Prism 安装');
  steps.push('备份游戏目录里会被覆盖的文件');
  steps.push('部署 Prism Core（version.dll 加载器和 prism_nvngx.dll），写入 prism-core.ini');
  steps.push(`复制${nr?.bundled ? ' Prism NR 自带的' : '本机的'} NVIDIA 签名版 nvngx_dlssnr.dll${nr?.version ? `（${nr.version}）` : ''}`);
  steps.push('写入安装清单，之后可以一键还原');

  manualSteps.push('在游戏设置里开启 DLSS（超分、DLAA 或光线重建都可以）。');
  if (plugins.includes('framegen') && values['framegen.mode'] !== 'off') manualSteps.push(`在游戏设置或 NVIDIA App 里开启 ${values['framegen.mode']} 帧生成。`);
  manualSteps.push('游戏里按 F8 开关 Prism Core、F4 分屏对比原画。档位、遍数和强度在这里调，游戏运行中约 1 秒内生效。');

  const riskConsent = analysis.antiCheatOptIn ? { ...analysis.antiCheatOptIn, accepted: req.acceptRisk === true } : undefined;
  if (riskConsent) manualSteps.unshift(riskConsent.how);
  return {
    riskConsent,
    gameId: game.id,
    gameName: game.name,
    route,
    preset,
    style: style.id,
    plugins,
    values,
    needs,
    steps,
    manualSteps,
    blockers,
    warnings,
    profileNotes: profile?.coreNotes ?? [],
  };
}

function existsAnyPrevious(gameId: string): boolean {
  return store.installs().some((r) => r.gameId === gameId && r.status === 'active');
}

// ---------------------------------------------------------------------------
// File generation
// ---------------------------------------------------------------------------

function fmt(v: number | boolean | string): string {
  if (typeof v === 'boolean') return v ? '1' : '0';
  if (typeof v === 'number') return Number.isInteger(v) ? String(v) : v.toFixed(6);
  return v;
}

/** Technique names declared in an .fx file. */
function techniquesIn(file: string): string[] {
  const src = readText(file) ?? '';
  return [...src.matchAll(/^\s*technique\s+([A-Za-z_][A-Za-z0-9_]*)/gm)].map((m) => m[1]);
}

/**
 * RenoDX DLSS 5 add-on settings, written as the [RenoDX.DLSS5] section the add-on reads from
 * ReShade.ini (keys observed from RenoDX DLSS5 Generic 6.5.3). Values are 0..1 except passes.
 */
function renodxSection(values: Values, route: Route, preset: PresetId | LegacyPresetId): string {
  const pct = (k: string, d: number) => Math.max(0, Math.min(1, Number(values[k] ?? d) / 100));
  const passes = Math.max(1, Math.min(4, Number(values['nr-renodx.passes'] ?? 1)));
  const modelRes = Number(values['nr-optiscaler.modelResolution'] ?? 100);
  // Pre-upscale NR is not supported by every game (it needs a single-mip colour buffer), so Prism
  // lowers the NR working resolution instead: it follows the game's DLSS render resolution.
  const followRender = route === 'native-hook' && (preset === 'balanced' || preset === 'fluid') && modelRes < 100;
  const lines = [
    'ConfigVersion=6',
    `NRIntensity=${pct('nr-renodx.intensity', 30).toFixed(2)}`,
    `NRLocalStructure=${pct('nr-renodx.structure', 100).toFixed(2)}`,
    `NRColorStrength=${pct('nr-renodx.color', 100).toFixed(2)}`,
    `NRPasses=${passes}`,
    'NRPreUpscale=0',
    `NRFollowInputRes=${followRender ? 1 : 0}`,
    'NRResolutionScale=1',
    'NRToggleKey=117',
    'NRScreenshotKey=116',
  ];
  return lines.join('\r\n');
}

function reshadeIni(learned: Record<string, string>, renodx: string | null): string {
  const lines = [
    '; Written by Prism NR. Rolling back in Prism restores the previous file.',
    '[GENERAL]',
    'EffectSearchPaths=.\\reshade-shaders\\Shaders\\**',
    'TextureSearchPaths=.\\reshade-shaders\\Textures\\**',
    `PresetPath=.\\${PRESET_FILE}`,
    'PerformanceMode=0',
    'NoReloadOnInit=0',
    '',
    '[INPUT]',
    // Home opens the panel, F7 toggles every Prism plugin at once (quick A/B), PrtSc saves a lossless still.
    'KeyOverlay=36,0,0,0',
    'KeyEffects=118,0,0,0',
    'KeyScreenshot=44,0,0,0',
    '',
    '[OVERLAY]',
    'TutorialProgress=4',
    'ShowFPS=0',
    '',
    '[SCREENSHOT]',
    'SavePath=.\\PrismNR-Screenshots',
    'FileFormat=1',
    'ClearAlpha=1',
    '',
  ];
  for (const [section, body] of Object.entries(learned)) {
    // Prism's own RenoDX values win over learned ones; everything else learned is carried over.
    if (renodx && section === 'RenoDX.DLSS5') continue;
    lines.push(`[${section}]`, body.trim(), '');
  }
  if (renodx) lines.push('[RenoDX.DLSS5]', renodx, '');
  return lines.join('\r\n');
}

interface ChainItem {
  technique: string;
  file: string;
  order: number;
  enabled: boolean;
}

function presetIni(chain: ChainItem[], plugins: string[], values: Values, defines: string[]): string {
  const sorted = [...chain].sort((a, b) => a.order - b.order);
  const ref = (c: ChainItem) => `${c.technique}@${c.file}`;
  const lines = [
    `PreprocessorDefinitions=${defines.join(',')}`,
    `Techniques=${sorted.filter((c) => c.enabled).map(ref).join(',')}`,
    `TechniqueSorting=${sorted.map(ref).join(',')}`,
    '',
  ];
  for (const id of plugins) {
    const p = pluginById(id);
    if (!p?.shader || !p.params.length) continue;
    lines.push(`[${p.shader.file}]`);
    for (const param of p.params) {
      const uniform = param.key.split('.')[1];
      lines.push(`${uniform}=${fmt(values[param.key] ?? param.default)}`);
    }
    lines.push('');
  }
  return lines.join('\r\n');
}

/** Updates `key=value` in an INI section, adding the section or key when missing. */
export function setIniValue(text: string, section: string, key: string, value: string): string {
  const lines = text.split(/\r?\n/);
  let inSection = false;
  let sectionFound = false;
  for (let i = 0; i < lines.length; i++) {
    const s = lines[i].trim();
    if (/^\[.*\]$/.test(s)) {
      if (inSection) {
        lines.splice(i, 0, `${key}=${value}`);
        return lines.join('\r\n');
      }
      inSection = s.slice(1, -1).toLowerCase() === section.toLowerCase();
      sectionFound ||= inSection;
      continue;
    }
    if (inSection && new RegExp(`^\\s*${key}\\s*=`, 'i').test(lines[i])) {
      lines[i] = `${key}=${value}`;
      return lines.join('\r\n');
    }
  }
  if (inSection) lines.push(`${key}=${value}`);
  else if (!sectionFound) lines.push('', `[${section}]`, `${key}=${value}`);
  return lines.join('\r\n');
}

function reshadeDllName(analysis: GameAnalysis): string {
  if (analysis.apis.includes('dx12') || analysis.apis.includes('dx11') || analysis.apis.includes('dx10')) return 'dxgi.dll';
  if (analysis.apis.includes('dx9')) return 'd3d9.dll';
  if (analysis.apis.includes('opengl')) return 'opengl32.dll';
  return 'dxgi.dll';
}

/**
 * Where a component's shader file goes: its path under the component's own Shaders folder is kept
 * (LumeniteFX includes "./include/..."), anything outside a Shaders folder lands at the top.
 */
export function shaderTarget(c: ComponentInfo, file: string, shadersDir: string): string {
  const parts = path.relative(c.path, file).split(/[\\/]/);
  const i = parts.map((p) => p.toLowerCase()).lastIndexOf('shaders');
  return i >= 0 && i < parts.length - 1 ? path.join(shadersDir, ...parts.slice(i + 1)) : path.join(shadersDir, path.basename(file));
}

/**
 * LumeniteFX as Feeder's motion source: all its shader files are copied (paths kept), but only the
 * motion technique is enabled (Kernel, provider 3; QuantMotion, provider 4, if Kernel is missing).
 * Its AO, bloom, reflection and AA effects stay off.
 */
export function lumeniteOps(lum: ComponentInfo, shadersDir: string, texturesDir: string): { ops: FileOp[]; chain: ChainItem[]; provider: number } {
  const ops: FileOp[] = [];
  const chain: ChainItem[] = [];
  let provider = 0;
  for (const fx of componentFiles(lum, /\.(fx|fxh)$/i)) {
    const to = shaderTarget(lum, fx, shadersDir);
    ops.push({ kind: 'copy', from: fx, to, label: `LumeniteFX ${path.relative(shadersDir, to)}` });
  }
  for (const [file, re, id] of [[/(^|[\\/])lumenite_Kernel\.fx$/i, /kernel/i, 3], [/(^|[\\/])lumenite_QuantMotion\.fx$/i, /quant/i, 4]] as const) {
    if (provider) break;
    const fx = componentFiles(lum, file)[0];
    const t = fx ? techniquesIn(fx).find((x) => re.test(x)) : undefined;
    if (fx && t) {
      chain.push({ technique: t, file: path.relative(shadersDir, shaderTarget(lum, fx, shadersDir)).replace(/\\/g, '/'), order: 5, enabled: true });
      provider = id;
    }
  }
  for (const tex of componentFiles(lum, /\.(png|dds|jpg)$/i)) ops.push({ kind: 'copy', from: tex, to: path.join(texturesDir, path.basename(tex)), label: `LumeniteFX 纹理 ${path.basename(tex)}` });
  return { ops, chain, provider };
}

interface Components {
  [id: string]: ComponentInfo;
}

function buildOps(plan: InstallPlan, analysis: GameAnalysis, comps: Components, dlls: NvidiaDll[]): { ops: FileOp[]; warnings: string[] } {
  const exeDir = path.dirname(analysis.exe!);
  const x64 = analysis.bitness !== 'x86';
  const ops: FileOp[] = [];
  const warnings: string[] = [];
  const shadersDir = path.join(exeDir, 'reshade-shaders', 'Shaders');
  const texturesDir = path.join(exeDir, 'reshade-shaders', 'Textures');
  const chain: ChainItem[] = [];
  const defines: string[] = [];
  let useReshade = plan.route !== 'optiscaler';

  // NVIDIA runtime from this PC.
  const nr = bestDll(dlls, 'nvngx_dlssnr.dll', true)!;
  ops.push({ kind: 'copy', from: nr.path, to: path.join(exeDir, 'nvngx_dlssnr.dll'), label: 'NVIDIA 神经渲染运行库' });

  if (plan.route === 'prism-core') {
    for (const f of CORE_FILES) ops.push({ kind: 'copy', from: path.join(coreDir, f), to: path.join(exeDir, f), label: `Prism Core：${f}` });
    ops.push({ kind: 'write', to: path.join(exeDir, CORE_INI), content: coreIni(plan.values), label: 'Prism Core 设置' });
    for (const a of conflictingAddons(exeDir)) ops.push({ kind: 'remove', to: a, label: `移走 ${path.basename(a)}` });
    return { ops, warnings };
  }

  if (plan.route === 'optiscaler') {
    const opti = comps['optiscaler-presr'];
    const main = componentFile(opti, /(^|\\)OptiScaler\.dll$/i);
    if (!main) throw new Error('OptiScaler 包里没有 OptiScaler.dll。');
    const pkgRoot = path.dirname(main);
    for (const f of fs.readdirSync(pkgRoot)) {
      const full = path.join(pkgRoot, f);
      if (!fs.statSync(full).isFile() || /\.(bat|cmd|ps1|md|txt)$/i.test(f) || /^nvngx_dlssnr\.dll$/i.test(f)) continue;
      const target = /^OptiScaler\.dll$/i.test(f) ? 'dxgi.dll' : f;
      if (/^OptiScaler\.ini$/i.test(f)) continue;
      ops.push({ kind: 'copy', from: full, to: path.join(exeDir, target), label: `OptiScaler：${target}` });
    }
    const iniSrc = path.join(pkgRoot, 'OptiScaler.ini');
    let ini = readText(iniSrc) ?? '';
    ini = setIniValue(ini, 'DlssNr', 'Enabled', 'true');
    const wantsPrism = plan.plugins.some((p) => pluginById(p)?.origin === 'prism');
    if (wantsPrism && /LoadReshade/i.test(ini) && comps.reshade) {
      ini = setIniValue(ini, 'Plugins', 'LoadReshade', 'true');
      useReshade = true;
    } else if (wantsPrism) {
      warnings.push('这个 OptiScaler 版本不支持加载 ReShade，Prism 原生插件这次不会生效。');
    }
    ops.push({ kind: 'write', to: path.join(exeDir, 'OptiScaler.ini'), content: ini, label: 'OptiScaler.ini（开启神经渲染）' });
  }

  if (useReshade) {
    const rs = comps.reshade;
    const dll = componentFile(rs, x64 ? /ReShade64\.dll$/i : /ReShade32\.dll$/i);
    if (!dll) throw new Error('ReShade 组件里没有对应位数的 DLL。');
    const name = plan.route === 'optiscaler' ? (x64 ? 'ReShade64.dll' : 'ReShade32.dll') : reshadeDllName(analysis);
    ops.push({ kind: 'copy', from: dll, to: path.join(exeDir, name), label: `ReShade（${name}）` });
    const renodx = plan.plugins.includes('nr-renodx') ? renodxSection(plan.values, plan.route, plan.preset) : null;
    ops.push({ kind: 'write', to: path.join(exeDir, 'ReShade.ini'), content: reshadeIni(store.settings().learnedAddonSections, renodx), label: 'ReShade.ini（含神经渲染参数）' });

    // Prism shaders.
    for (const f of fs.readdirSync(shaderDir)) {
      if (/\.(fx|fxh)$/i.test(f)) ops.push({ kind: 'copy', from: path.join(shaderDir, f), to: path.join(exeDir, SHADER_SUBDIR, f), label: `Prism 着色器 ${f}` });
    }
    for (const id of plan.plugins) {
      const p = pluginById(id);
      if (p?.shader) chain.push({ technique: p.shader.technique, file: p.shader.file, order: p.shader.order, enabled: id !== 'prism-compare' });
    }
    if (!plan.plugins.includes('prism-compare') && plan.plugins.includes('prism-capture'))
      chain.push({ technique: 'PrismCompare', file: 'Prism_Compare.fx', order: 90, enabled: false });
  }

  if (plan.plugins.includes('nr-renodx')) {
    const addon = componentFile(comps['renodx-dlss5'], /\.addon64$/i);
    if (!addon) throw new Error('RenoDX 组件里没有 .addon64 文件。');
    ops.push({ kind: 'copy', from: addon, to: path.join(exeDir, path.basename(addon)), label: 'RenoDX 神经渲染插件' });
  }
  if (plan.plugins.includes('nr-dfc')) {
    for (const f of componentFiles(comps.dfc, /\.(dll|addon64)$/i)) ops.push({ kind: 'copy', from: f, to: path.join(exeDir, path.basename(f)), label: `Deep Fried Chicken：${path.basename(f)}` });
  }

  if (plan.route === 'feeder') {
    const feeder = comps.feeder;
    const addon = componentFile(feeder, x64 ? /dlss5-feed\.addon64$/i : /dlss5-feed\.addon32$/i);
    if (!addon) throw new Error('Feeder 包里没有 dlss5-feed 插件。');
    ops.push({ kind: 'copy', from: addon, to: path.join(exeDir, path.basename(addon)), label: 'Feeder 插件' });
    for (const fx of componentFiles(feeder, /\.(fx|fxh)$/i)) {
      ops.push({ kind: 'copy', from: fx, to: path.join(shadersDir, path.basename(fx)), label: `Feeder 着色器 ${path.basename(fx)}` });
      if (/\.fx$/i.test(fx)) for (const t of techniquesIn(fx)) chain.push({ technique: t, file: path.basename(fx), order: 10, enabled: true });
    }
    const lum = lumeniteOps(comps.lumenite, shadersDir, texturesDir);
    if (!lum.provider) throw new Error('LumeniteFX 组件里没有 lumenite_Kernel.fx 或 lumenite_QuantMotion.fx。');
    ops.push(...lum.ops);
    chain.push(...lum.chain);
    defines.push(`DLSS5_MV_PROVIDER=${lum.provider}`);
    if (!analysis.nativeDlss.present) {
      const dlss = bestDll(dlls, 'nvngx_dlss.dll')!;
      ops.push({ kind: 'copy', from: dlss.path, to: path.join(exeDir, 'nvngx_dlss.dll'), label: 'NVIDIA DLSS 运行库' });
    }
    const wr = Number(plan.values['feeder.work_resolution'] ?? 100);
    const ws = Number(plan.values['feeder.work_sharpness'] ?? 0.3);
    if (wr !== 100 && analysis.apis.includes('dx11')) {
      const cfg = ['enabled=1', 'mode=2', `work_resolution=${wr}`, `work_sharpness=${ws}`, 'hdr=-1'].join('\r\n');
      ops.push({ kind: 'write', to: path.join(exeDir, 'dlss5-feed.cfg'), content: cfg, label: `Feeder 工作分辨率 ${wr}%` });
    }
  }

  if (useReshade && chain.length) {
    defines.push('PRISM_MOTION_VECTORS=0');
    ops.push({ kind: 'write', to: path.join(exeDir, PRESET_FILE), content: presetIni(chain, plan.plugins, plan.values, defines), label: 'Prism 方案文件' });
  }
  return { ops, warnings };
}

// ---------------------------------------------------------------------------
// Transactional apply / rollback
// ---------------------------------------------------------------------------

function applyOps(recordId: string, gameId: string, ops: FileOp[]): BackupRecord[] {
  const backupDir = path.join(dirs.backups, gameId, recordId);
  ensureDir(backupDir);
  const done: BackupRecord[] = [];
  try {
    ops.forEach((op, i) => {
      if (op.kind === 'remove' && !exists(op.to)) return;
      let backup: string | null = null;
      if (exists(op.to)) {
        backup = path.join(backupDir, `${String(i).padStart(3, '0')}-${path.basename(op.to)}`);
        fs.copyFileSync(op.to, backup);
      }
      ensureDir(path.dirname(op.to));
      if (exists(op.to)) fs.chmodSync(op.to, 0o666);  // a read-only file in the way (see below)
      if (op.kind === 'copy') {
        fs.copyFileSync(op.from, op.to);
        // Copies keep the source's read-only flag (the shipped NVIDIA file has it), which would make
        // a later rollback or reinstall fail to replace them.
        fs.chmodSync(op.to, 0o666);
      } else if (op.kind === 'write') fs.writeFileSync(op.to, op.content, 'utf8');
      else fs.rmSync(op.to, { force: true });
      // A removed file records an empty hash: it is meant to be absent until rollback puts it back.
      done.push({ target: op.to, backup, sha256After: op.kind === 'copy' ? sha256File(op.to) : op.kind === 'write' ? sha256Text(op.content) : '' });
    });
  } catch (e) {
    restore(done);
    throw new Error(`写入失败，已自动撤回：${(e as Error).message}`);
  }
  return done;
}

function restore(files: BackupRecord[]): string[] {
  const notes: string[] = [];
  for (const f of [...files].reverse()) {
    try {
      // prism-core.ini is Prism's own: Core's in-game panel writes it too.
      if (f.sha256After && !/prism-core\.ini$/i.test(f.target) && exists(f.target) && sha256File(f.target) !== f.sha256After) notes.push(`${path.basename(f.target)} 在安装后被改动过，已按备份还原。`);
      if (exists(f.target)) fs.chmodSync(f.target, 0o666);
      if (f.backup && exists(f.backup)) fs.copyFileSync(f.backup, f.target);
      else fs.rmSync(f.target, { force: true });
    } catch (e) {
      notes.push(`${path.basename(f.target)} 还原失败：${(e as Error).message}`);
    }
  }
  // Remove folders Prism created if they are now empty.
  const dirsToCheck = new Set(files.filter((f) => !f.backup).map((f) => path.dirname(f.target)));
  for (const d of [...dirsToCheck].sort((a, b) => b.length - a.length)) {
    try {
      let cur = d;
      while (fs.readdirSync(cur).length === 0 && /reshade-shaders/i.test(cur)) {
        fs.rmdirSync(cur);
        cur = path.dirname(cur);
      }
    } catch {
      /* not empty or gone */
    }
  }
  return notes;
}

/** Files Prism Core writes itself while the game runs; captures are the user's and stay. */
function cleanupCore(r: InstallRecord): void {
  if (r.route !== 'prism-core') return;
  const loader = r.files.find((f) => /[\\/]version\.dll$/i.test(f.target));
  if (loader) fs.rmSync(path.join(path.dirname(loader.target), 'prism-core.log'), { force: true });
}

export async function runInstall(
  plan: InstallPlan,
  game: GameEntry,
  analysis: GameAnalysis,
  dlls: NvidiaDll[],
  progress: (p: InstallProgress) => void,
): Promise<InstallRecord> {
  if (plan.blockers.length) throw new Error(plan.blockers[0]);
  if (analysis.antiCheatOptIn && !plan.riskConsent?.accepted) throw new Error('这款游戏有反作弊，需要先了解并接受风险。');
  if (analysis.antiCheat.length && !analysis.antiCheatOptIn) throw new Error('检测到反作弊，Prism 不会在这个游戏里安装。');
  const emit = (stage: InstallProgress['stage'], message: string, fraction: number) => progress({ gameId: game.id, stage, message, fraction });

  const comps: Components = {};
  const ids = plan.needs.map((n) => n.id);
  for (let i = 0; i < ids.length; i++) {
    comps[ids[i]] = await ensureComponent(ids[i], (m) => emit('fetch', m, (i + 0.5) / (ids.length + 2)));
  }

  // Supersede an earlier install so the game folder never holds two Prism layers.
  const records = store.installs();
  for (const r of records.filter((x) => x.gameId === game.id && x.status === 'active')) {
    emit('deploy', '还原上一次的安装', 0.7);
    r.rollbackNotes = restore(r.files);
    cleanupCore(r);
    r.status = 'superseded';
    r.rolledBackAt = new Date().toISOString();
  }

  emit('deploy', '生成文件清单', 0.75);
  const { ops, warnings } = buildOps(plan, analysis, comps, dlls);
  const recordId = new Date().toISOString().replace(/[:.]/g, '-');
  const exeDir = path.dirname(analysis.exe!);
  const manifest = { app: 'Prism NR', recordId, route: plan.route, preset: plan.preset, plugins: plan.plugins, installedAt: new Date().toISOString() };
  ops.push({ kind: 'write', to: path.join(exeDir, MANIFEST), content: JSON.stringify(manifest, null, 2), label: '安装清单' });

  emit('deploy', `写入 ${ops.length} 个文件`, 0.85);
  const files = applyOps(recordId, game.id, ops);

  emit('verify', '校验写入结果', 0.95);
  // A removed file (empty hash) is meant to be absent; anything else must be on disk.
  const missing = files.find((f) => f.sha256After && !exists(f.target));
  if (missing) {
    restore(files);
    throw new Error(`校验失败：${missing.target} 不存在，已自动撤回这次安装。`);
  }

  const record: InstallRecord = {
    id: recordId,
    gameId: game.id,
    gameName: game.name,
    installDir: game.installDir,
    route: plan.route,
    preset: plan.preset,
    plugins: plan.plugins,
    values: plan.values,
    files,
    installedAt: new Date().toISOString(),
    status: 'active',
    rollbackNotes: warnings.length ? warnings : undefined,
    riskAcceptedAt: plan.riskConsent?.accepted ? new Date().toISOString() : undefined,
  };
  store.saveInstalls([record, ...records]);
  const settings = store.settings();
  settings.styles[game.id] = plan.style;
  store.saveSettings(settings);
  emit('done', '已就绪，可以直接启动游戏', 1);
  log(`install ${game.name}: ${files.length} files, route ${plan.route}`);
  return record;
}

export function rollback(recordId: string): InstallRecord {
  const records = store.installs();
  const r = records.find((x) => x.id === recordId);
  if (!r) throw new Error('找不到这条安装记录。');
  if (r.status !== 'active') throw new Error('这条记录已经还原过了。');
  r.rollbackNotes = restore(r.files);
  cleanupCore(r);
  r.status = 'rolled-back';
  r.rolledBackAt = new Date().toISOString();
  store.saveInstalls(records);
  log(`rollback ${r.gameName}`);
  return r;
}

/**
 * Brings Prism Core installs made by an older Prism NR up to the shipped version, in place: Core's
 * two DLLs, the NVIDIA runtime when the shipped one is newer, and the cadence when it is still the
 * old default (every 4 frames, 1.1.1) now that every 2 frames is the default. A file the running
 * game holds is left for the next start. Backups stay as they were, so rollback still returns the
 * folder to how it was before Prism.
 */
export function upgradeCoreInstalls(dlls: NvidiaDll[]): string[] {
  const done: string[] = [];
  const records = store.installs();
  const nr = bestDll(dlls, 'nvngx_dlssnr.dll', true);
  let changed = false;
  for (const r of records.filter((x) => x.status === 'active' && x.route === 'prism-core')) {
    const notes: string[] = [];
    for (const f of r.files) {
      const name = path.basename(f.target).toLowerCase();
      const from = CORE_FILES.includes(name) ? path.join(coreDir, name) : name === 'nvngx_dlssnr.dll' && nr ? nr.path : null;
      if (!from || !exists(from) || !exists(f.target)) continue;
      const want = sha256File(from);
      if (want === sha256File(f.target)) continue;
      if (name === 'nvngx_dlssnr.dll') {
        // Only ever move the network forward, and only to an NVIDIA-signed copy.
        const cur = dlls.find((d) => d.path.toLowerCase() === f.target.toLowerCase());
        if (!nr?.signed || (cur?.version && versionNewer(cur.version, nr.version ?? '0'))) continue;
      }
      try {
        fs.chmodSync(f.target, 0o666);
        fs.copyFileSync(from, f.target);
        fs.chmodSync(f.target, 0o666);
        f.sha256After = want;
        notes.push(name);
      } catch (e) {
        log(`upgrade ${r.gameName}: ${name} busy (${(e as Error).message}), next start`);
      }
    }
    // Installs from before the tiers move to the recommended tier, unless the user tuned them; a
    // game-specific build (profile coreValues) is added where the install lacks it.
    const legacy = r.preset === 'core';
    // 1.2's network resolution (prism-core.netScale: native or 75%) becomes 1.3's size by pixels
    // (prism-core.netSize): 旗舰 and 极致 take their new tier values, a tuned install the nearest size.
    const oldScale = r.values?.['prism-core.netScale'];
    if (oldScale !== undefined && r.values) {
      const { ['prism-core.netScale']: _old, ...rest } = r.values;
      // 1.2's every-frame tiers: 2 passes, guard 100%; 极致 native at 210%, 旗舰 75% at 240%.
      const v = r.values, every = String(v['prism-core.cadence']) === '1' && String(v['prism-core.passes']) === '2' && Number(v['prism-core.guard']) === 100;
      const was = every && Number(oldScale) >= 99 && Number(v['prism-core.intensity']) === 210 ? 'extreme' : every && Number(oldScale) === 75 && Number(v['prism-core.intensity']) === 240 ? 'flagship' : null;
      r.values = was ? { ...rest, ...resolveValues(was) } : { ...rest, 'prism-core.netSize': Number(oldScale) >= 99 ? '150' : '100' };
    }
    const resized = oldScale !== undefined;
    const extra = Object.entries(matchProfile(r.gameName)?.coreValues ?? {}).filter(([k]) => r.values?.[k] === undefined);
    if (legacy || resized || extra.length) {
      const ini = r.files.find((f) => /prism-core\.ini$/i.test(f.target));
      if (ini && exists(ini.target)) {
        const tuned = Boolean(r.values?.['prism-core.cadenceChosen']);
        let values = !legacy ? { ...(r.values ?? {}) } : tuned ? { ...resolveValues('high'), ...r.values } : { ...(r.values ?? {}), ...resolveValues('high') };
        values = { ...values, ...Object.fromEntries(extra) };
        const content = coreIni(values, panelLines(ini.target));
        try {
          fs.writeFileSync(ini.target, content, 'utf8');
          ini.sha256After = sha256Text(content);
          r.values = values;
          if (legacy) r.preset = 'high';
          notes.push([legacy ? (tuned ? 'passes 2' : 'tier high') : '', resized ? '网络分辨率' : '', ...extra.map(([k]) => k.replace('prism-core.', ''))].filter(Boolean).join(' + '));
        } catch (e) {
          log(`upgrade ${r.gameName}: prism-core.ini busy (${(e as Error).message})`);
        }
      }
    }
    if (notes.length) {
      changed = true;
      done.push(`${r.gameName}：${notes.join('、')}`);
      log(`upgrade ${r.gameName}: ${notes.join(', ')}`);
    }
  }
  if (changed) store.saveInstalls(records);
  return done;
}

function versionNewer(a: string, b: string): boolean {
  const pa = a.split('.').map((n) => Number(n) || 0), pb = b.split('.').map((n) => Number(n) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) if ((pa[i] ?? 0) !== (pb[i] ?? 0)) return (pa[i] ?? 0) > (pb[i] ?? 0);
  return false;
}

/**
 * Takes back what Core's in-game panel saved into prism-core.ini (the tier values), so the app
 * shows the settings the game runs with. Returns whether a record changed.
 */
export function syncCoreFromIni(): boolean {
  const records = store.installs();
  let changed = false;
  for (const r of records) {
    if (r.status !== 'active' || r.route !== 'prism-core') continue;
    const file = r.files.find((f) => /prism-core\.ini$/i.test(f.target));
    if (!file || !exists(file.target)) continue;
    let text: string;
    try {
      text = fs.readFileSync(file.target, 'utf8');
    } catch {
      continue;
    }
    if (file.sha256After && sha256Text(text) === file.sha256After) continue; // as the app wrote it
    const ini: Record<string, string> = {};
    for (const line of text.split(/\r?\n/)) {
      const m = line.trim().match(/^(\w+)=(.*)$/);
      if (m) ini[m[1].toLowerCase()] = m[2].trim();
    }
    const num = (k: string) => (ini[k] !== undefined && ini[k] !== '' && Number.isFinite(Number(ini[k])) ? Number(ini[k]) : undefined);
    const next: Values = { ...(r.values ?? {}) };
    const cadence = num('cadence'), passes = num('passes'), intensity = num('intensity'), guard = num('guard'), netScale = num('netscale'), async = num('async'), look = num('look');
    if (cadence !== undefined) next['prism-core.cadence'] = String(Math.round(cadence));
    if (passes !== undefined) next['prism-core.passes'] = String(Math.round(passes));
    if (intensity !== undefined) next['prism-core.intensity'] = Math.round(intensity * 100);
    if (guard !== undefined) next['prism-core.guard'] = Math.round(guard * 100);
    if (netScale !== undefined && netScale < 0.999) next['prism-core.netSize'] = netSizeFor(netScale);
    if (async !== undefined) next['prism-core.async'] = async !== 0;
    if (look !== undefined) next['prism-core.look'] = String(Math.max(0, Math.min(4, Math.round(look))));
    r.values = next;
    file.sha256After = sha256Text(text);
    changed = true;
  }
  if (changed) store.saveInstalls(records);
  return changed;
}

/** Rewrites prism-core.ini of an active Core install; the running game re-reads it within a second. */
export function updateCoreConfig(recordId: string, values: Values): InstallRecord {
  const records = store.installs();
  const r = records.find((x) => x.id === recordId);
  if (!r || r.status !== 'active' || r.route !== 'prism-core') throw new Error('这个游戏当前没有启用 Prism Core。');
  const file = r.files.find((f) => /prism-core\.ini$/i.test(f.target));
  if (!file) throw new Error('安装记录里没有 prism-core.ini，请重新应用一次。');
  const merged = { ...resolveValues('core'), ...(r.values ?? {}), ...values, ...('prism-core.cadence' in values ? { 'prism-core.cadenceChosen': true } : {}) };
  const content = coreIni(merged, panelLines(file.target));
  fs.writeFileSync(file.target, content, 'utf8');
  // Keep rollback's change check quiet: this edit is Prism's own.
  file.sha256After = sha256Text(content);
  r.values = merged;
  store.saveInstalls(records);
  return r;
}

const RESHADE_CORE_SECTIONS = /^(GENERAL|INPUT|OVERLAY|SCREENSHOT|STYLE|DEPTH|APP)$/i;

/**
 * Reads add-on sections that ReShade add-ons wrote into ReShade.ini after the user
 * enabled them once, so later installs can carry them and skip that manual step.
 */
export function learnAddonSettings(): number {
  const settings = store.settings();
  let learned = 0;
  for (const r of store.installs().filter((x) => x.status === 'active')) {
    const iniPath = r.files.find((f) => /ReShade\.ini$/i.test(f.target))?.target;
    const text = iniPath ? readText(iniPath) : null;
    if (!text) continue;
    const sections = text.split(/^\[/m).slice(1);
    for (const s of sections) {
      const end = s.indexOf(']');
      const name = s.slice(0, end);
      if (RESHADE_CORE_SECTIONS.test(name)) continue;
      const body = s.slice(end + 1).trim();
      if (body && settings.learnedAddonSections[name] !== body) {
        settings.learnedAddonSections[name] = body;
        learned++;
      }
    }
  }
  if (learned) store.saveSettings(settings);
  return learned;
}

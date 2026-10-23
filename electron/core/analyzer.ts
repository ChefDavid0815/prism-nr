import * as fs from 'node:fs';
import * as path from 'node:path';
import type { AntiCheatHit, GameAnalysis, GameEntry, GraphicsApi, Route } from '../shared/types';
import { matchProfile } from '../shared/profiles';
import { denuvoSections, readPe } from './pe';
import { exists, log, psJson, psQuote, walk } from './util';

/** Bumped when the analysis gains a field or a route, so cached analyses are redone. */
export const ANALYSIS_SCHEMA = 7;

// Executables that are never the game itself.
const NOT_GAME_EXE =
  /(bypass|inject|trainer|crack|cheat|unins\d*|setup|install|redist|vc_?redist|dxsetup|crash|report|launcher|uplay|easyanticheat|battleye|be(service|launcher)|start_protected_game|cefprocess|ue4prereq|helper|update|patch|config|settings|benchmark|dotnet|7z|python|node|ffmpeg|unitycrashhandler|steamerrorreporter|eac|server|editor|tool)/i;

const ANTI_CHEAT: { name: string; test: (rel: string) => boolean }[] = [
  { name: 'Easy Anti-Cheat', test: (r) => /(^|\\)easyanticheat(_eos)?(\\|\.|$)|start_protected_game\.exe$|easyanticheat_x64\.dll$/i.test(r) },
  { name: 'BattlEye', test: (r) => /(^|\\)battleye(\\|$)|beservice(_x64)?\.exe$|beclient(_x64)?\.dll$/i.test(r) },
  { name: 'EA Javelin', test: (r) => /eaanticheat\.gameservice/i.test(r) },
  { name: 'HoYoProtect / mhyprot', test: (r) => /mhyprot\d*\.sys$|hoyokprotect\.sys$/i.test(r) },
  { name: 'Tencent ACE', test: (r) => /(^|\\)anticheatexpert(\\|$)|ace-base/i.test(r) },
  { name: 'nProtect GameGuard', test: (r) => /(^|\\)gameguard(\\|$)|gamemon(64)?\.des$/i.test(r) },
  { name: 'XIGNCODE3', test: (r) => /xigncode/i.test(r) },
  { name: 'PunkBuster', test: (r) => /pnkbstr[ab]\.exe$/i.test(r) },
  { name: 'Vanguard', test: (r) => /vgk\.sys$|vgc\.exe$/i.test(r) },
  { name: 'NetEase NeacSafe', test: (r) => /neac(safe|_?protect)/i.test(r) },
];

const PROXY_DLLS = ['dxgi.dll', 'd3d11.dll', 'd3d12.dll', 'd3d9.dll', 'opengl32.dll', 'winmm.dll', 'version.dll', 'dinput8.dll', 'winhttp.dll', 'nvngx.dll'];

function apiFromImports(imports: string[]): GraphicsApi[] {
  const set = new Set<GraphicsApi>();
  for (const i of imports) {
    if (i === 'd3d12.dll') set.add('dx12');
    else if (i === 'd3d11.dll') set.add('dx11');
    else if (i.startsWith('d3d10')) set.add('dx10');
    else if (i === 'd3d9.dll') set.add('dx9');
    else if (i === 'vulkan-1.dll') set.add('vulkan');
    else if (i === 'opengl32.dll') set.add('opengl');
  }
  return [...set];
}

/** Ranks candidate executables: Unreal shipping binaries first, then by size. */
function pickExe(entry: GameEntry, exes: { file: string; size: number }[]): string[] {
  const scored = exes
    .filter((e) => !NOT_GAME_EXE.test(path.basename(e.file, '.exe')))
    .map((e) => {
      const base = path.basename(e.file).toLowerCase();
      let score = Math.log2(Math.max(e.size, 1));
      if (/-win64-shipping\.exe$/.test(base)) score += 40;
      if (/\\binaries\\win64\\/i.test(e.file)) score += 10;
      if (/\\bin\\x64\\/i.test(e.file)) score += 8;
      if (entry.storeExe && path.resolve(entry.storeExe).toLowerCase() === path.resolve(e.file).toLowerCase()) score += 15;
      const nameTokens = entry.name.toLowerCase().replace(/[^a-z0-9]/g, '');
      if (nameTokens && base.replace(/[^a-z0-9]/g, '').includes(nameTokens.slice(0, 6))) score += 6;
      return { file: e.file, score };
    })
    .sort((a, b) => b.score - a.score);
  return scored.map((s) => s.file);
}

async function fileVersion(p: string): Promise<string | undefined> {
  const v = await psJson<string>(`(Get-Item -LiteralPath ${psQuote(p)}).VersionInfo.FileVersion | ConvertTo-Json -Compress`);
  return typeof v === 'string' && v.trim() ? v.trim().replace(/,\s*/g, '.') : undefined;
}

export async function analyzeGame(entry: GameEntry): Promise<GameAnalysis> {
  const root = entry.installDir;
  const exes: { file: string; size: number }[] = [];
  const antiCheat: AntiCheatHit[] = [];
  let dlssPath: string | undefined;
  let streamline = false;
  let dlssFrameGen = false;
  let otherUpscaler = false;
  const engineDlls: string[] = [];

  walk(
    root,
    (file, _depth, isDirectory) => {
      const rel = path.relative(root, file);
      for (const ac of ANTI_CHEAT) {
        if (!antiCheat.some((h) => h.name === ac.name) && ac.test(rel)) antiCheat.push({ name: ac.name, evidence: rel });
      }
      if (isDirectory) return;
      const lower = file.toLowerCase();
      if (lower.endsWith('.exe')) {
        try {
          exes.push({ file, size: fs.statSync(file).size });
        } catch {
          /* unreadable */
        }
      } else if (lower.endsWith('nvngx_dlss.dll')) dlssPath ??= file;
      else if (lower.endsWith('sl.interposer.dll') || lower.endsWith('sl.dlss.dll')) streamline = true;
      else if (lower.endsWith('nvngx_dlssg.dll') || lower.endsWith('sl.dlss_g.dll')) dlssFrameGen = true;
      else if (/(ffx_fsr2_api_x64|amd_fidelityfx_dx12|amd_fidelityfx_vk|libxess)\.dll$/.test(lower)) otherUpscaler = true;
      else if (/(unityplayer|gameassembly|cryrenderd3d1[12])\.dll$/.test(lower)) engineDlls.push(file);
    },
    { maxDepth: 5, maxEntries: 40000, skipDirs: /^(\.git|__pycache__|movies|videos|cinematics|paks|content\\paks)$/i },
  );

  const candidates = pickExe(entry, exes);
  const exe = candidates[0] ?? null;
  const pe = exe ? readPe(exe) : null;
  let apis = pe ? apiFromImports(pe.imports) : [];
  // Unity and some engines import the graphics API from a side DLL.
  for (const dll of engineDlls) {
    const info = readPe(dll);
    if (info) apis = [...new Set([...apis, ...apiFromImports(info.imports)])];
  }
  const exeDir = exe ? path.dirname(exe) : root;
  // Renderers often live in a side DLL next to the executable (Northlight, id Tech, custom engines).
  if (exe && !apis.includes('dx12') && !apis.includes('dx11')) {
    try {
      const sideDlls = fs
        .readdirSync(exeDir)
        .filter((f) => /\.dll$/i.test(f) && !PROXY_DLLS.includes(f.toLowerCase()) && !/^(nvngx|sl\.|nvapi|amd_|ffx_|libxess|steam_api|vcruntime|msvcp|ucrt|api-ms|d3dcompiler|dxcompiler|dxil)/i.test(f))
        .slice(0, 80);
      for (const f of sideDlls) {
        const info = readPe(path.join(exeDir, f));
        if (info) apis = [...new Set([...apis, ...apiFromImports(info.imports)])];
      }
    } catch {
      /* unreadable folder */
    }
    // The D3D12 Agility SDK ships as a D3D12 folder beside the executable.
    if (exists(path.join(exeDir, 'D3D12', 'D3D12Core.dll'))) apis = [...new Set<GraphicsApi>([...apis, 'dx12'])];
  }
  // OpenGL imports on a DX game are usually a launcher dependency; drop them when DX is present.
  if ((apis.includes('dx12') || apis.includes('dx11')) && apis.includes('opengl')) apis = apis.filter((a) => a !== 'opengl');
  // A dxgi-only import with DLSS present is almost always DX12 or DX11.
  if (!apis.length && (pe?.imports.includes('dxgi.dll') || dlssPath)) apis = ['dx12'];
  const proxies = PROXY_DLLS.filter((d) => exists(path.join(exeDir, d)));
  const existing = {
    reshade: exists(path.join(exeDir, 'ReShade.ini')) || exists(path.join(exeDir, 'reshade-shaders')),
    optiscaler: exists(path.join(exeDir, 'OptiScaler.ini')),
    prism: exists(path.join(exeDir, 'prism-nr.manifest.json')),
    proxies,
  };

  const nativeDlss = { present: Boolean(dlssPath), path: dlssPath, version: dlssPath ? await fileVersion(dlssPath) : undefined };
  const warnings: string[] = [];
  const routes: Route[] = [];
  let recommendedRoute: Route = 'unknown';
  let routeReason = '';

  // Anti-cheat blocks the install, except in games whose profile says it can be avoided offline
  // (every detected anti-cheat must be one the profile names).
  const profile = matchProfile(entry.name, entry.steamAppId);
  if (profile?.antiCheatKnown && !antiCheat.some((a) => a.name === profile.antiCheatKnown))
    antiCheat.push({ name: profile.antiCheatKnown, evidence: '已知这款游戏使用该反作弊（与目录里有没有它的文件无关）' });
  const optIn = profile?.antiCheatOptIn;
  const optInApplies = Boolean(antiCheat.length && optIn && antiCheat.every((a) => optIn.names.includes(a.name)));
  if (antiCheat.length && !optInApplies) {
    recommendedRoute = 'blocked';
    routeReason = `检测到 ${antiCheat.map((a) => a.name).join('、')}。注入 DLL 可能导致封号，Prism 不会在这个游戏里安装。`;
  } else if (!exe) {
    routeReason = '没有找到游戏主程序。可以在详情里手动指定。';
  } else {
    const modern = apis.includes('dx12') || apis.includes('dx11');
    // Prism Core drives the network through its own D3D12 pipeline: DX12, 64-bit, the game's own DLSS.
    const core = nativeDlss.present && apis.includes('dx12') && pe?.bitness === 'x64';
    if (nativeDlss.present && modern) {
      if (core) routes.push('prism-core');
      routes.push('native-hook');
      routes.push('optiscaler');
      recommendedRoute = core ? 'prism-core' : 'native-hook';
      routeReason = core
        ? '游戏是 DX12 并自带 DLSS，可以用 Prism Core：拦截游戏的 DLSS 调用，网络隔几帧跑一次全分辨率，不需要 ReShade。'
        : '游戏自带 DLSS，可以直接挂钩它的 DLSS 调用。性能优先的方案会改用 OptiScaler，在超分之前执行神经渲染。';
    } else if (apis.includes('dx12') && pe?.bitness === 'x64') {
      // No DLSS: Prism Core's present mode runs on the frame as it is presented (Present=1).
      routes.push('prism-core');
      recommendedRoute = 'prism-core';
      routeReason = '游戏是 DX12 但没有 DLSS，Prism Core 用画面递交模式：在游戏把画面交给屏幕时处理整帧（包括界面文字），没有运动矢量，网络每帧重新计算。';
    } else if (apis.length) {
      routes.push('feeder');
      // OptiScaler needs an upscaler call to replace; FSR 2/3 or XeSS works too.
      if (modern && pe?.bitness === 'x64' && otherUpscaler) routes.push('optiscaler');
      recommendedRoute = 'feeder';
      routeReason = '游戏没有 DLSS，需要用 Feeder 从 ReShade 深度缓冲和估算的运动矢量补全输入。快速运动时可能拖影。';
      if (pe?.bitness === 'x86') warnings.push('32 位游戏要走辅助进程，帧率大约减半。');
      if (apis.includes('dx9') || apis.includes('dx10')) warnings.push('DX9 / DX10 只能用 DLAA 模式，需要 dgVoodoo2 或 DXVK 转换。');
    } else {
      routeReason = '没有识别出图形 API。可能是启动器而不是游戏本体。';
    }
  }
  if (optInApplies) routeReason = `检测到 ${antiCheat.map((a) => a.name).join('、')}，这款游戏可以离线玩、不启动反作弊。了解并接受风险后才能启用。${routeReason ? ' ' + routeReason : ''}`;
  if (proxies.length && !existing.prism) warnings.push(`游戏目录里已有 ${proxies.join('、')}，可能是其他 Mod。安装时会先备份。`);
  if (existing.optiscaler && !existing.prism) warnings.push('检测到已安装的 OptiScaler，同时使用两个神经渲染插件会让其中一个静默失效。');
  if (/witcher|巫师/i.test(entry.name)) warnings.push('这个游戏的原作风格容易被神经渲染改变，建议用“原片优先”方案。');

  log(`analyze ${entry.name}: exe=${exe ? path.basename(exe) : '-'} apis=${apis.join('/')} dlss=${nativeDlss.present} ac=${antiCheat.length} route=${recommendedRoute}`);
  return {
    schema: ANALYSIS_SCHEMA,
    gameId: entry.id,
    exe,
    exeCandidates: candidates.slice(0, 8),
    bitness: pe?.bitness ?? 'unknown',
    apis,
    nativeDlss,
    streamline,
    dlssFrameGen,
    antiCheatOptIn: optInApplies ? optIn : undefined,
    importsVersionDll: pe ? pe.imports.includes('version.dll') : undefined,
    loaderImports: pe ? ['version', 'winmm', 'dinput8', 'dxgi', 'd3d12'].filter((n) => pe.imports.includes(n + '.dll')) : undefined,
    denuvo: pe && denuvoSections(pe).length ? { sections: denuvoSections(pe) } : undefined,
    antiCheat,
    existing,
    routes,
    recommendedRoute,
    routeReason,
    warnings,
    analyzedAt: new Date().toISOString(),
  };
}

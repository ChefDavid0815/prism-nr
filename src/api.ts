import { PLUGINS, PRESETS, resolveValues } from '../electron/shared/catalog';
import { matchProfile, styleById } from '../electron/shared/profiles';
import type { GameAnalysis, GameEntry, InstallPlan, InstallProgress, InstallRecord, PrismApi, SystemInfo } from '../electron/shared/types';

declare global {
  interface Window {
    prism?: PrismApi;
  }
}

/**
 * Browser preview only: when the page runs outside Electron there is no backend, so a
 * small in-memory stand-in serves synthetic data shaped like a real scan.
 */
function createPreviewApi(): PrismApi {
  const system: SystemInfo = {
    gpus: [{ name: 'NVIDIA GeForce RTX 5090 Laptop GPU', vendor: 'nvidia', arch: 'blackwell', laptop: true, vramMB: 24463, driver: '617.42', tier: 'flagship', nrSupport: 'official', expectation: '可以开满模型分辨率，追求最好画质。' }],
    primary: null,
    os: { release: '10.0.26300', build: 26300, isWin11: true },
    notes: [],
  };
  system.primary = system.gpus[0];
  const games: GameEntry[] = [
    { id: 'g1', name: 'CONTROL Resonant', store: 'steam', installDir: 'E:\\Steam\\steamapps\\common\\CONTROL Resonant' },
    { id: 'g2', name: 'Cyberpunk 2077', store: 'steam', installDir: 'D:\\Games\\Cyberpunk 2077', steamAppId: '1091500' },
    { id: 'g3', name: 'The Witcher 3: Wild Hunt', store: 'gog', installDir: 'D:\\GOG\\The Witcher 3', steamAppId: '292030' },
    { id: 'g4', name: 'Black Myth: Wukong', store: 'steam', installDir: 'D:\\Games\\BlackMythWukong', steamAppId: '2358720' },
    { id: 'g5', name: 'The Elder Scrolls V: Skyrim Special Edition', store: 'steam', installDir: 'D:\\Games\\Skyrim', steamAppId: '489830' },
    { id: 'g6', name: 'Alan Wake 2', store: 'epic', installDir: 'D:\\Epic\\AlanWake2' },
    { id: 'g7', name: 'ACE COMBAT 8: WINGS OF THEVE', store: 'steam', installDir: 'E:\\Steam\\steamapps\\common\\ACE COMBAT 8' },
    { id: 'g8', name: 'Apex Legends', store: 'steam', installDir: 'D:\\Games\\Apex' },
  ];
  const a = (id: string, partial: Partial<GameAnalysis>): GameAnalysis => ({
    gameId: id,
    exe: 'game.exe',
    exeCandidates: [],
    bitness: 'x64',
    apis: ['dx12'],
    nativeDlss: { present: true, version: '310.7.130.0' },
    streamline: true,
    dlssFrameGen: true,
    antiCheat: [],
    existing: { reshade: false, optiscaler: false, prism: false, proxies: [] },
    routes: ['native-hook', 'optiscaler'],
    recommendedRoute: 'native-hook',
    routeReason: '游戏自带 DLSS，可以直接挂钩它的 DLSS 调用。性能优先的方案会改用 OptiScaler，在超分之前执行神经渲染。',
    warnings: [],
    analyzedAt: new Date().toISOString(),
    ...partial,
  });
  const analyses: Record<string, GameAnalysis> = {
    g1: a('g1', { exe: 'E:\\Steam\\steamapps\\common\\CONTROL Resonant\\CONTROLResonant.exe' }),
    g2: a('g2', { exe: 'D:\\Games\\Cyberpunk 2077\\bin\\x64\\Cyberpunk2077.exe', nativeDlss: { present: true, version: '310.4.0.0' } }),
    g3: a('g3', { exe: 'D:\\GOG\\The Witcher 3\\bin\\x64_dx12\\witcher3.exe', warnings: ['这个游戏的原作风格容易被神经渲染改变，建议用“原片优先”方案。'] }),
    g4: a('g4', { exe: 'D:\\Games\\BlackMythWukong\\b1\\Binaries\\Win64\\b1-Win64-Shipping.exe' }),
    g5: a('g5', { exe: 'D:\\Games\\Skyrim\\SkyrimSE.exe', apis: ['dx11'], nativeDlss: { present: false }, streamline: false, dlssFrameGen: false, routes: ['feeder'], recommendedRoute: 'feeder', routeReason: '游戏没有 DLSS，需要用 Feeder 从 ReShade 深度缓冲和估算的运动矢量补全输入。快速运动时可能拖影。' }),
    g6: a('g6', { exe: 'D:\\Epic\\AlanWake2\\AlanWake2.exe' }),
    g7: a('g7', { exe: 'AceCombat8.exe', apis: ['dx12'], nativeDlss: { present: false }, antiCheat: [{ name: 'Easy Anti-Cheat', evidence: 'EasyAntiCheat' }], routes: [], recommendedRoute: 'blocked', routeReason: '检测到 Easy Anti-Cheat。注入 DLL 可能导致封号，Prism 不会在这个游戏里安装。' }),
    g8: a('g8', { exe: 'r5apex.exe', apis: ['dx12', 'dx11'], nativeDlss: { present: false }, antiCheat: [{ name: 'Easy Anti-Cheat', evidence: 'EasyAntiCheat' }], routes: [], recommendedRoute: 'blocked', routeReason: '检测到 Easy Anti-Cheat。注入 DLL 可能导致封号，Prism 不会在这个游戏里安装。' }),
  };
  let installs: InstallRecord[] = [];
  const listeners = new Set<(p: InstallProgress) => void>();
  let motion: 'max' | 'standard' | 'off' = 'max';
  const delay = (ms: number) => new Promise((r) => setTimeout(r, ms));

  return {
    getSystem: async () => system,
    scanGames: async () => {
      await delay(900);
      return games;
    },
    analyzeGame: async (id) => analyses[id],
    addManualGame: async () => null,
    getCatalog: async () => ({ plugins: PLUGINS, presets: PRESETS }),
    listComponents: async () => [],
    importComponent: async () => null,
    locateNvidiaDlls: async () => [],
    planInstall: async (req) => {
      const g = games.find((x) => x.id === req.gameId)!;
      const an = analyses[req.gameId];
      const profile = matchProfile(g.name, g.steamAppId);
      const preset = req.preset ?? profile?.preset ?? 'flagship';
      const style = styleById(req.style ?? profile?.style ?? 'realistic');
      const route = an.recommendedRoute === 'blocked' ? 'blocked' : preset === 'balanced' || preset === 'fluid' ? (an.routes.includes('optiscaler') ? 'optiscaler' : an.recommendedRoute) : an.recommendedRoute;
      const p = PRESETS.find((x) => x.id === preset)!;
      let plugins = req.plugins ?? p.plugins;
      if (route === 'feeder') plugins = [...new Set([...plugins.filter((x) => x !== 'nr-optiscaler'), 'nr-renodx', 'feeder', 'motion-lumenite', 'prism-capture'])];
      if (route !== 'feeder') plugins = plugins.filter((x) => x !== 'prism-guard');
      const plan: InstallPlan = {
        gameId: g.id,
        gameName: g.name,
        route,
        preset,
        style: style.id,
        plugins,
        values: { ...resolveValues(preset), ...style.values, ...(profile?.values ?? {}), ...(req.overrides ?? {}) },
        needs: [{ id: 'reshade', name: 'ReShade（含插件支持）', status: 'auto' }],
        steps: ['下载并校验 ReShade、RenoDX DLSS 5 插件', '备份游戏目录里会被覆盖的文件', '部署 ReShade 和神经渲染插件', '复制本机的 nvngx_dlssnr.dll', '部署 Prism 原生插件，写好参数和顺序', '写入安装清单，之后可以一键还原'],
        manualSteps: route === 'feeder' ? [] : ['在游戏设置里开启 DLSS 超分或 DLAA。'],
        blockers: route === 'blocked' ? [an.routeReason] : [],
        warnings: an.warnings,
        profileNotes: profile?.notes ?? [],
      };
      return plan;
    },
    runInstall: async (plan) => {
      const stages: [InstallProgress['stage'], string][] = [
        ['fetch', '查询 ReShade 最新版本'],
        ['fetch', '下载 ReShade 6.8.2'],
        ['fetch', '下载 RenoDX DLSS 5 插件'],
        ['deploy', '备份 3 个文件'],
        ['deploy', '写入 17 个文件'],
        ['verify', '校验写入结果'],
      ];
      for (let i = 0; i < stages.length; i++) {
        listeners.forEach((l) => l({ gameId: plan.gameId, stage: stages[i][0], message: stages[i][1], fraction: (i + 1) / (stages.length + 1) }));
        await delay(420);
      }
      listeners.forEach((l) => l({ gameId: plan.gameId, stage: 'done', message: '已就绪，可以直接启动游戏', fraction: 1 }));
      const rec: InstallRecord = { id: String(Date.now()), gameId: plan.gameId, gameName: plan.gameName, installDir: '', route: plan.route, preset: plan.preset, plugins: plan.plugins, files: new Array(17).fill(0).map((_, i) => ({ target: `file${i}`, backup: i < 3 ? 'b' : null, sha256After: '' })), installedAt: new Date().toISOString(), status: 'active' };
      installs = [rec, ...installs.map((r) => (r.gameId === plan.gameId && r.status === 'active' ? { ...r, status: 'superseded' as const } : r))];
      return rec;
    },
    onProgress: (cb) => {
      listeners.add(cb);
      return () => listeners.delete(cb);
    },
    listInstalls: async () => installs,
    rollback: async (id) => {
      installs = installs.map((r) => (r.id === id ? { ...r, status: 'rolled-back' as const, rolledBackAt: new Date().toISOString() } : r));
      return installs.find((r) => r.id === id)!;
    },
    diagnostics: async () => ({ generatedAt: new Date().toISOString(), system, games: games.length, installs, components: [], nvidiaDlls: [], log: ['preview mode'] }),
    exportDiagnostics: async () => null,
    getSettings: async () => ({ motion }),
    setMotion: async (m) => {
      motion = m;
    },
    openPath: async () => {},
    openExternal: async (url) => {
      window.open(url, '_blank');
    },
  };
}

export const api: PrismApi = window.prism ?? createPreviewApi();
export const isPreview = !window.prism;

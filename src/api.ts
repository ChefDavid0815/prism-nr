import { PLUGINS, PRESETS, resolveValues } from '../electron/shared/catalog';
import { matchProfile } from '../electron/shared/profiles';
import type { GameAnalysis, GameEntry, InstallPlan, InstallProgress, InstallRecord, PresetId, PrismApi, Route, SystemInfo } from '../electron/shared/types';

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
    { id: 'g9', name: 'Downloads', store: 'manual', installDir: 'C:\\Users\\me\\Downloads' },
    { id: 'g10', name: 'NBA 2K27', store: 'steam', installDir: 'E:\\Steam\\steamapps\\common\\NBA 2K27' },
    { id: 'g11', name: 'Crimson Desert', store: 'steam', installDir: 'E:\\Steam\\steamapps\\common\\Crimson Desert' },
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
    routes: ['prism-core', 'native-hook', 'optiscaler'],
    recommendedRoute: 'prism-core',
    routeReason: '游戏是 DX12 并自带 DLSS，可以用 Prism Core：拦截游戏的 DLSS 调用，网络隔几帧跑一次全分辨率，不需要 ReShade。',
    warnings: [],
    analyzedAt: new Date().toISOString(),
    ...partial,
  });
  const analyses: Record<string, GameAnalysis> = {
    g1: a('g1', { exe: 'E:\\Steam\\steamapps\\common\\CONTROL Resonant\\CONTROLResonant.exe' }),
    g2: a('g2', { exe: 'D:\\Games\\Cyberpunk 2077\\bin\\x64\\Cyberpunk2077.exe', nativeDlss: { present: true, version: '310.4.0.0' } }),
    g3: a('g3', { exe: 'D:\\GOG\\The Witcher 3\\bin\\x64_dx12\\witcher3.exe', warnings: ['这个游戏的原作风格容易被神经渲染改变，建议用流畅档或把强度调低。'] }),
    g4: a('g4', { exe: 'D:\\Games\\BlackMythWukong\\b1\\Binaries\\Win64\\b1-Win64-Shipping.exe' }),
    g5: a('g5', { exe: 'D:\\Games\\Skyrim\\SkyrimSE.exe', apis: ['dx11'], nativeDlss: { present: false }, streamline: false, dlssFrameGen: false, routes: ['feeder'], recommendedRoute: 'feeder', routeReason: '游戏没有 DLSS，Prism Core 拿不到运动矢量和深度，暂时用不了。' }),
    g6: a('g6', { exe: 'D:\\Epic\\AlanWake2\\AlanWake2.exe' }),
    g7: a('g7', { exe: 'AceCombat8.exe', apis: ['dx12'], nativeDlss: { present: false }, antiCheat: [{ name: 'Easy Anti-Cheat', evidence: 'EasyAntiCheat' }], routes: [], recommendedRoute: 'blocked', routeReason: '检测到 Easy Anti-Cheat。注入 DLL 可能导致封号，Prism 不会在这个游戏里安装。' }),
    g9: a('g9', { exe: null, apis: [], nativeDlss: { present: false }, streamline: false, dlssFrameGen: false, routes: [], recommendedRoute: 'unknown', routeReason: '没有识别出图形 API。可能是启动器而不是游戏本体。' }),
    g11: a('g11', { exe: 'E:\\Steam\\steamapps\\common\\Crimson Desert\\bin64\\CrimsonDesert.exe', denuvo: { sections: ['.xtext', '.xtls'] } }),
    g10: a('g10', { exe: 'NBA2K27.exe', antiCheat: [{ name: 'Easy Anti-Cheat', evidence: 'EasyAntiCheat' }], antiCheatOptIn: { names: ['Easy Anti-Cheat'], how: '只在离线模式下使用：不要进入 MyCAREER 在线、公园、排位、MyTEAM 等任何联网模式。要联网玩之前，先在 Prism 里点"还原"。' }, routeReason: '检测到 Easy Anti-Cheat，这款游戏可以离线玩、不启动反作弊。了解并接受风险后才能启用。' }),
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
    coreUpgrades: async () => [],
    removeManualGame: async (id) => {
      if (installs.some((r) => r.gameId === id && r.status === 'active')) throw new Error('这个目录里还有 Prism 的安装，请先还原，再移除。');
      const i = games.findIndex((g) => g.id === id);
      if (i >= 0) games.splice(i, 1);
    },
    getCatalog: async () => ({ plugins: PLUGINS, presets: PRESETS }),
    listComponents: async () => [],
    importComponent: async () => null,
    locateNvidiaDlls: async () => [],
    planInstall: async (req) => {
      const g = games.find((x) => x.id === req.gameId)!;
      const an = analyses[req.gameId];
      const profile = matchProfile(g.name, g.steamAppId);
      const preset: PresetId = req.preset ?? 'high';
      const core = an.recommendedRoute !== 'blocked' && an.routes.includes('prism-core');
      const route: Route = core ? 'prism-core' : 'blocked';
      const values = { ...resolveValues(preset), ...(req.overrides ?? {}) };
      const plan: InstallPlan = {
        gameId: g.id,
        gameName: g.name,
        route,
        preset,
        style: profile?.style ?? 'realistic',
        plugins: ['nr-prism-core'],
        values,
        needs: [],
        steps: ['备份游戏目录里会被覆盖的文件', '部署 Prism Core（version.dll 加载器和 prism_nvngx.dll），写入 prism-core.ini', '复制 Prism NR 自带的 NVIDIA 签名版 nvngx_dlssnr.dll（310.9.16.0）', '写入安装清单，之后可以一键还原'],
        manualSteps: ['在游戏设置里开启 DLSS（超分、DLAA 或光线重建都可以）。', '游戏里按 F8 开关 Prism Core、F4 分屏对比原画。档位、遍数和强度在这里调，游戏运行中约 1 秒内生效。'],
        blockers: an.recommendedRoute === 'blocked' ? [an.routeReason] : core ? [] : ['这个游戏用不了 Prism Core：它需要 DX12、64 位并且自带 DLSS 的游戏。'],
        warnings: [...an.warnings, ...(Number(values['prism-core.passes']) >= 3 ? ['3 遍以上是实验性质的游乐场：开销成倍增加，画面容易过度，不建议实际游玩。'] : []), 'Prism Core 是实验性预览，目前在 CONTROL Resonant 和地平线 6 上实测过。'],
        profileNotes: profile?.coreNotes ?? [],
        riskConsent: an.antiCheatOptIn ? { ...an.antiCheatOptIn, accepted: req.acceptRisk === true } : undefined,
        lateLoad: an.denuvo && an.exe ? { launchOption: `"${an.exe.slice(0, an.exe.lastIndexOf('\\') + 1)}PrismLaunch.exe" %command%`, why: '检测到 D 加密' } : undefined,
      };
      return plan;
    },
    runInstall: async (plan) => {
      const stages: [InstallProgress['stage'], string][] = [
        ['deploy', '备份 2 个文件'],
        ['deploy', '写入 5 个文件'],
        ['verify', '校验写入结果'],
      ];
      for (let i = 0; i < stages.length; i++) {
        listeners.forEach((l) => l({ gameId: plan.gameId, stage: stages[i][0], message: stages[i][1], fraction: (i + 1) / (stages.length + 1) }));
        await delay(420);
      }
      listeners.forEach((l) => l({ gameId: plan.gameId, stage: 'done', message: '已就绪，可以直接启动游戏', fraction: 1 }));
      const rec: InstallRecord = { launchOption: plan.lateLoad?.launchOption, id: String(Date.now()), gameId: plan.gameId, gameName: plan.gameName, installDir: '', route: plan.route, preset: plan.preset, plugins: plan.plugins, values: plan.values, files: new Array(17).fill(0).map((_, i) => ({ target: `file${i}`, backup: i < 3 ? 'b' : null, sha256After: '' })), installedAt: new Date().toISOString(), status: 'active' };
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
    updateCoreConfig: async (id, values) => {
      await delay(120);
      installs = installs.map((r) => (r.id === id ? { ...r, values: { ...(r.values ?? {}), ...values } } : r));
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

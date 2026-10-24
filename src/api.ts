import { PLUGINS, PRESETS, resolveValues } from '../electron/shared/catalog';
import { matchProfile } from '../electron/shared/profiles';
import { isRedeemCode, normPhone, normRedeemCode, REDEEM_ALPHABET, type AccountPlan, type AccountProfile, type AdminCode, type AdminPack, type AdminUser, type AdminUserPack, type CommunityShare } from '../electron/shared/account';
import { encodeGradeCode } from '../electron/shared/gradecode';
import { gradeFromPreset, gradeParse } from '../electron/shared/grade';
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
    { id: 'g12', name: 'Half-Life 2', store: 'manual', installDir: 'D:\\Games\\Half-Life 2' },
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
    g5: a('g5', { exe: 'D:\\Games\\Skyrim\\SkyrimSE.exe', apis: ['dx11'], nativeDlss: { present: false }, streamline: false, dlssFrameGen: false, coreMode: 'bridge', existing: { reshade: true, optiscaler: false, prism: false, proxies: ['dxgi.dll'] }, routeReason: '游戏是 64 位 DX11，Prism Core 用画面桥：每帧把画面交给 64 位的辅助进程 prism_host.exe 跑网络再送回（包括界面文字）。画面经过内存来回，帧率损失比 DX12 大。', warnings: ['检测到 ReShade：保留不动，两者可以同时使用。'] }),
    g12: a('g12', { exe: 'D:\\Games\\Half-Life 2\\hl2.exe', bitness: 'x86', apis: ['dx9'], nativeDlss: { present: false }, streamline: false, dlssFrameGen: false, coreMode: 'bridge', routeReason: '游戏是 32 位 DX9，Prism Core 用画面桥：每帧把画面交给 64 位的辅助进程 prism_host.exe 跑网络再送回（包括界面文字）。画面经过内存来回，帧率损失比 DX12 大。' }),
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
        blockers: an.recommendedRoute === 'blocked' ? [an.routeReason] : core ? [] : [an.routeReason || '没有找到游戏主程序。'],
        bridge: an.coreMode === 'bridge' || undefined,
        x86: an.bitness === 'x86' || undefined,
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
    // Preview: CONTROL Resonant is "running", to show the community's now-playing section.
    runningExes: async (names) => names.filter((n) => /^controlresonant\.exe$/i.test(n)),
    account: previewAccount(),
    community: previewCommunity(),
  };
}

/** Community in preview mode: a few shares for the sample library, official ones first, in memory. */
function previewCommunity(): PrismApi['community'] {
  const day = (n: number) => new Date(Date.now() - n * 86400000).toISOString();
  const AUTHORS: Record<string, { id: string; plan: AccountPlan; founder: boolean; contributor: boolean }> = {
    ChefZC: { id: 'u0', plan: 'ultra', founder: true, contributor: false },
    晨风: { id: 'u1', plan: 'pro', founder: false, contributor: true },
    pixel_hunter: { id: 'u2', plan: 'basic', founder: false, contributor: false },
    retro: { id: 'u5', plan: 'free', founder: false, contributor: false },
  };
  const mk = (id: string, title: string, description: string, game: { name: string; exe?: string; steamAppId?: string }, grade: ReturnType<typeof gradeParse>, author: string, imports: number, age: number, official = false): CommunityShare => {
    const a = AUTHORS[author];
    return { id, title, description, game_name: game.name, game_exe: game.exe ?? null, steam_app_id: game.steamAppId ?? null, code: encodeGradeCode(game, grade), imports, created_at: day(age), author, own: false, official,
      author_id: a.id, author_avatar: null, author_plan: a.plan, author_founder: a.founder, author_contributor: a.contributor };
  };
  const shares: CommunityShare[] = [
    mk('o1', '野兽派冷光', '混凝土的冷灰配上标志性的红：暗部偏青、整体收一点饱和，只把红色留得浓烈。', { name: 'CONTROL Resonant', exe: 'controlresonant.exe', steamAppId: '3669870' }, gradeParse('Contrast:18,Pivot:-0.20,Highlights:-12,Blacks:-10,Temperature:-12,Saturation:-18,ShadowHue:200,ShadowAmount:25,SatRed:35,Density:30,Vignette:-15'), 'ChefZC', 312, 0, true),
    mk('o2', '霓虹夜之城', '夜之城的青与品红：暗部染青、亮部带一点品红，霓虹更浓但皮肤基本不动。', { name: 'Cyberpunk 2077', exe: 'cyberpunk2077.exe', steamAppId: '1091500' }, gradeParse('Contrast:20,Pivot:-0.30,Blacks:-8,Tint:6,Saturation:10,Vibrance:20,ShadowHue:195,ShadowAmount:40,HighlightHue:320,HighlightAmount:15,Density:35'), 'ChefZC', 205, 0, true),
    mk('o3', '黑泽明胶片', '黑白胶片：高反差、纯黑、明显的颗粒和暗角。', { name: "Ghost of Tsushima DIRECTOR'S CUT", steamAppId: '2215430' }, gradeParse('Contrast:30,Saturation:-100,SkinProtect:0,Vignette:-22,Grain:30'), 'ChefZC', 96, 0, true),
    mk('s1', '雨夜胶片', '压暗高光、暗部偏青，霓虹更有层次。适合夜里的城区。', { name: 'Cyberpunk 2077', exe: 'Cyberpunk2077.exe', steamAppId: '1091500' }, gradeParse('Exposure:-0.20,Contrast:30,Highlights:-35,ShadowHue:200,ShadowAmount:60,HighlightHue:40,HighlightAmount:30,Density:40,Grain:15,Vignette:-25'), '晨风', 128, 1),
    mk('s2', '控制局冷白', '低饱和起点，白色更冷，混凝土的质感更硬。', { name: 'CONTROL Resonant', exe: 'CONTROLResonant.exe', steamAppId: '3669870' }, gradeFromPreset(2), 'pixel_hunter', 42, 2),
    mk('s3', '黑神话·暖阳', '提亮中间调、黄绿更鲜，秋天的山更暖。', { name: 'Black Myth: Wukong', exe: 'b1-Win64-Shipping.exe', steamAppId: '2358720' }, gradeParse('Midtones:20,Temperature:18,Vibrance:25,SatYellow:15,SatGreen:12,HueGreen:-6,Sky:30,HighlightTwist:40'), '晨风', 77, 3),
    mk('s4', '赛博老胶片', '整体褪色加颗粒，像 90 年代的录像带。', { name: 'Cyberpunk 2077', exe: 'Cyberpunk2077.exe', steamAppId: '1091500' }, gradeParse('Contrast:-20,Blacks:35,Saturation:-30,Grain:60,Vignette:-40,CurveR5:20,CurveB1:30'), 'retro', 9, 0),
  ];
  const keyOf = (s: CommunityShare) => s.steam_app_id || 'n:' + s.game_name.trim().toLowerCase();
  const ZH: Record<string, [string, string[]]> = {
    '3669870': ['控制：共振', ['共振']], '1091500': ['赛博朋克 2077', ['cp2077', '2077']], '2215430': ['对马岛之魂 导演剪辑版', ['对马岛']], '2358720': ['黑神话：悟空', ['悟空', '黑神话']],
  };
  const norm = (t: string) => t.toLowerCase().replace(/[^a-z0-9一-龥]/g, '');
  return {
    games: async (q) => {
      const byKey = new Map<string, CommunityShare[]>();
      for (const s of shares) byKey.set(keyOf(s), [...(byKey.get(keyOf(s)) ?? []), s]);
      const nq = norm(q);
      const games = [...byKey.entries()].map(([key, list]) => ({
        key, steam_app_id: list[0].steam_app_id, name_en: list[0].game_name, name_zh: ZH[key]?.[0] ?? '', game_exe: list[0].game_exe,
        shares: list.length, official: list.filter((s) => s.official).length, imports: list.reduce((n, s) => n + s.imports, 0), latest: list[0].created_at,
      })).filter((g) => !nq || [g.name_en, g.name_zh, ...(ZH[g.key]?.[1] ?? [])].some((t) => norm(t).includes(nq)))
        .sort((a, b) => b.official - a.official || b.imports - a.imports);
      return { games, total: games.length };
    },
    list: async (query) => {
      const q = (query.q ?? '').toLowerCase();
      const rows = shares.filter((s) => (!q || s.title.toLowerCase().includes(q) || s.game_name.toLowerCase().includes(q) || s.author.toLowerCase().includes(q)) &&
        (!query.steam && !query.exe ? true : s.steam_app_id === query.steam || (query.exe && s.game_exe?.toLowerCase() === query.exe.toLowerCase())) && (!query.mine || s.own) && (!query.official || s.official) && (!query.gkey || keyOf(s) === query.gkey));
      return { shares: rows, total: rows.length };
    },
    share: async ({ title, description, code }) => {
      const { decodeGradeCode } = await import('../electron/shared/gradecode');
      const { game } = decodeGradeCode(code);
        shares.splice(shares.filter((x) => x.official).length, 0, { id: 's' + (shares.length + 1), title, description, game_name: game.name, game_exe: game.exe ?? null, steam_app_id: game.steamAppId ?? null, code, imports: 0, created_at: new Date().toISOString(), author: '你', own: true, official: false, author_id: 'me', author_avatar: null, author_plan: 'free', author_founder: false, author_contributor: false });
    },
    remove: async (id) => {
      const i = shares.findIndex((s) => s.id === id);
      if (i >= 0) shares.splice(i, 1);
    },
    imported: async (id) => {
      const s = shares.find((x) => x.id === id);
      if (s) s.imports++;
    },
  };
}

/**
 * Accounts in preview mode: in memory, the code is always 123456; sign in as admin / admin to see the
 * admin page. Redeem code TEST-PACK-C0DE-0001 gives the sample pack.
 */
function previewAccount(): PrismApi['account'] {
  const day = (n: number) => new Date(Date.now() - n * 86400000).toISOString();
  const users: (AdminUser & { password: string })[] = [
    { id: 'u0', username: 'ChefZC', email: 'owner@example.com', phone: null, role: 'owner', confirmed: true, banned: false, created_at: day(3), last_sign_in_at: day(0), password: 'admin', plan: 'ultra', founder: true, contributor: false, avatar: null, bio: 'Prism NR 作者。喜欢把游戏调成电影的样子。' },
    { id: 'u1', username: '晨风', email: 'chenfeng@example.com', phone: '+8613800138000', role: 'admin', confirmed: true, banned: false, created_at: day(2), last_sign_in_at: day(0), password: 'password1', plan: 'pro', founder: false, contributor: true, avatar: null, bio: '' },
    { id: 'u2', username: 'pixel_hunter', email: 'pixel@example.com', phone: '+14155550123', role: 'user', confirmed: true, banned: false, created_at: day(1), last_sign_in_at: day(1), password: 'password1', plan: 'basic', founder: false, contributor: false, avatar: null, bio: '' },
    { id: 'u3', username: 'user_3f9a21c0', email: 'newbie@example.com', phone: null, role: 'user', confirmed: false, banned: false, created_at: day(0), last_sign_in_at: null, password: 'password1', plan: 'free', founder: false, contributor: false, avatar: null, bio: '' },
    { id: 'u4', username: '刷号的', email: 'spam@example.com', phone: null, role: 'user', confirmed: true, banned: true, created_at: day(0), last_sign_in_at: day(0), password: 'password1', plan: 'free', founder: false, contributor: false, avatar: null, bio: '' },
  ];
  let me: string | null = null;
  const packs: Omit<AdminPack, 'owners' | 'codes' | 'codes_left'>[] = [
    { id: 'cinema', name: '电影感调色包', description: '预留的示例插件包：内容之后再定。', active: true, created_at: day(2) },
    { id: 'early', name: '早期测试包', description: '给参与测试的群友。', active: false, created_at: day(5) },
  ];
  const codes: AdminCode[] = [
    { code: 'TESTPACKC0DE0001', pack_id: 'cinema', max_uses: 1, uses: 0, expires_at: null, disabled: false, note: '预览用', created_at: day(1) },
    { code: 'A7K2M9QXR4T8V1WZ', pack_id: 'cinema', max_uses: 1, uses: 1, expires_at: null, disabled: false, note: 'B站抽奖', created_at: day(1) },
    { code: 'H3N6P0S5Y2B8C4DE', pack_id: 'cinema', max_uses: 50, uses: 12, expires_at: new Date(Date.now() + 20 * 86400000).toISOString(), disabled: false, note: '群内发放', created_at: day(1) },
    { code: 'K9F1G7J3M5Q2R8ST', pack_id: 'early', max_uses: 1, uses: 0, expires_at: null, disabled: true, note: '', created_at: day(4) },
  ];
  const owners: (AdminUserPack & { user: string })[] = [
    { user: 'u1', pack_id: 'cinema', source: 'code', code: 'A7K2M9QXR4T8V1WZ', created_at: day(1) },
    { user: 'u2', pack_id: 'early', source: 'admin', code: null, created_at: day(4) },
  ];
  const left = (c: AdminCode) => !c.disabled && c.uses < c.max_uses && (!c.expires_at || Date.parse(c.expires_at) > Date.now());
  const delay = (ms: number) => new Promise((res) => setTimeout(res, ms));
  const profileOf = (u: AdminUser): AccountProfile => ({ id: u.id, username: u.username, email: u.email, phone: u.phone, role: u.role, createdAt: u.created_at, bio: u.bio, avatar: u.avatar, plan: u.plan, founder: u.founder, contributor: u.contributor });
  const state = () => {
    const u = users.find((x) => x.id === me);
    return u ? { signedIn: true, profile: profileOf(u) } : { signedIn: false };
  };
  const find = (id: string) => {
    const t = id.trim().toLowerCase();
    const phone = normPhone(t);
    return users.find((u) => u.email === t || u.username.toLowerCase() === t || (phone && u.phone === phone));
  };
  return {
    state: async () => state(),
    signUp: async ({ email, password, username, phone }) => {
      await delay(500);
      if (users.some((u) => u.email === email.toLowerCase())) throw new Error('这个邮箱已经注册过了，请直接登录');
      if (username && users.some((u) => u.username.toLowerCase() === username.toLowerCase())) throw new Error('这个用户名已经有人用了');
      if (password.length < 8) throw new Error('密码至少 8 位');
      users.push({ id: 'u' + users.length, username: username || 'user_' + Math.random().toString(16).slice(2, 10), email: email.toLowerCase(), phone: phone ? normPhone(phone) : null, role: 'user', confirmed: false, banned: false, created_at: new Date().toISOString(), last_sign_in_at: null, password, plan: 'free', founder: false, contributor: false, avatar: null, bio: '' });
      return { needsCode: true, email: email.toLowerCase() };
    },
    verify: async (email, code) => {
      await delay(400);
      const u = users.find((x) => x.email === email);
      if (!u || code.trim() !== '123456') throw new Error('验证码不对或已过期，请重新发送。');
      u.confirmed = true;
      me = u.id;
      return state();
    },
    resend: async () => { await delay(300); },
    signIn: async (identifier, password) => {
      await delay(500);
      const u = find(identifier);
      if (!u || u.password !== password) throw new Error('账号或密码不对');
      if (u.banned) throw new Error('这个账号已被管理员停用');
      if (!u.confirmed) return { needsCode: true, email: identifier.includes('@') ? u.email : null, hint: u.email.replace(/^(.{1,2})[^@]*(@.*)$/, '$1***$2') };
      me = u.id;
      u.last_sign_in_at = new Date().toISOString();
      return state();
    },
    signOut: async () => {
      me = null;
    },
    updateProfile: async (patch) => {
      await delay(300);
      const u = users.find((x) => x.id === me)!;
      if (patch.username !== undefined) {
        if (users.some((x) => x.id !== u.id && x.username.toLowerCase() === patch.username!.toLowerCase())) throw new Error('这个用户名已经有人用了。');
        u.username = patch.username.trim();
      }
      if (patch.phone !== undefined) u.phone = patch.phone ? normPhone(patch.phone) : null;
      if (patch.bio !== undefined) u.bio = patch.bio.trim();
      if (patch.avatar !== undefined) u.avatar = patch.avatar;
      return state();
    },
    packs: async () => {
      await delay(200);
      return packs
        .map((p) => {
          const o = owners.find((x) => x.user === me && x.pack_id === p.id);
          return { id: p.id, name: p.name, description: p.description, owned: Boolean(o), owned_at: o?.created_at ?? null, source: o?.source ?? null };
        })
        .filter((p) => p.owned || packs.find((x) => x.id === p.id)!.active)
        .sort((a, b) => Number(b.owned) - Number(a.owned));
    },
    redeem: async (raw) => {
      await delay(500);
      if (!me) throw new Error('请先登录。');
      const code = normRedeemCode(raw);
      if (!isRedeemCode(code)) throw new Error('兑换码是 16 位字母和数字，形如 ABCD-EFGH-JKMN-PQRS，请检查有没有输错或漏掉。');
      const c = codes.find((x) => x.code === code);
      if (!c) throw new Error('没有这个兑换码，请检查有没有输错。');
      const pack = packs.find((p) => p.id === c.pack_id)!;
      if (owners.some((o) => o.user === me && o.pack_id === c.pack_id)) return { pack: pack.id, name: pack.name, already: true };
      if (c.disabled) throw new Error('这个兑换码已经停用了。');
      if (c.expires_at && Date.parse(c.expires_at) < Date.now()) throw new Error('这个兑换码已经过期了。');
      if (c.uses >= c.max_uses) throw new Error('这个兑换码已经被用完了。');
      c.uses++;
      owners.push({ user: me, pack_id: pack.id, source: 'code', code, created_at: new Date().toISOString() });
      return { pack: pack.id, name: pack.name, already: false };
    },
    admin: async (a) => {
      await delay(250);
      if (a.action === 'packs') {
        return { packs: packs.map((p) => ({ ...p, owners: owners.filter((o) => o.pack_id === p.id).length, codes: codes.filter((c) => c.pack_id === p.id).length, codes_left: codes.filter((c) => c.pack_id === p.id && left(c)).length })) };
      }
      if (a.action === 'savePack') {
        if (!/^[a-z0-9][a-z0-9-]{1,31}$/.test(a.pack)) throw new Error('编号要 2–32 位，只能用小写字母、数字和连字符，以字母或数字开头');
        const p = packs.find((x) => x.id === a.pack);
        if (a.create) {
          if (p) throw new Error('这个编号已经有插件包在用了');
          packs.push({ id: a.pack, name: a.name, description: a.description, active: a.active ?? true, created_at: new Date().toISOString() });
        } else if (p) Object.assign(p, { name: a.name, description: a.description, ...(a.active !== undefined ? { active: a.active } : {}) });
        return { ok: true };
      }
      if (a.action === 'codes') return { codes: codes.filter((c) => c.pack_id === a.pack).sort((x, y) => y.created_at.localeCompare(x.created_at)) };
      if (a.action === 'makeCodes') {
        const made: string[] = [];
        for (let i = 0; i < a.count; i++) {
          const code = Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => REDEEM_ALPHABET[b & 31]).join('');
          made.push(code);
          codes.push({ code, pack_id: a.pack, max_uses: a.maxUses, uses: 0, expires_at: a.days ? new Date(Date.now() + a.days * 86400000).toISOString() : null, disabled: false, note: a.note, created_at: new Date().toISOString() });
        }
        return { codes: made };
      }
      if (a.action === 'setCode') {
        const c = codes.find((x) => x.code === a.code);
        if (c) c.disabled = a.disabled;
        return { ok: true };
      }
      if (a.action === 'userPacks') return { packs: owners.filter((o) => o.user === a.id).map(({ user: _u, ...o }) => o) };
      if (a.action === 'grant') {
        if (!owners.some((o) => o.user === a.id && o.pack_id === a.pack)) owners.push({ user: a.id, pack_id: a.pack, source: 'admin', code: null, created_at: new Date().toISOString() });
        return { ok: true };
      }
      if (a.action === 'revoke') {
        const i = owners.findIndex((o) => o.user === a.id && o.pack_id === a.pack);
        if (i >= 0) owners.splice(i, 1);
        return { ok: true };
      }
      if (a.action === 'stats') {
        const recent = (d: string | null, n: number) => d && Date.now() - Date.parse(d) < n * 86400000;
        return { stats: { total: users.length, confirmed: users.filter((u) => u.confirmed).length, admins: users.filter((u) => u.role === 'admin').length, banned: users.filter((u) => u.banned).length, new_7d: users.filter((u) => recent(u.created_at, 7)).length, active_7d: users.filter((u) => recent(u.last_sign_in_at, 7)).length, new_today: users.filter((u) => recent(u.created_at, 1)).length } };
      }
      if (a.action === 'list') {
        const q = (a.q ?? '').toLowerCase();
        const rows = users.filter((u) => !q || u.username.toLowerCase().includes(q) || u.email.includes(q) || (u.phone ?? '').includes(q)).map(({ password: _p, ...u }) => u);
        return { users: rows, total: rows.length, page: 0, size: 50 };
      }
      if (a.action === 'setOfficial' || a.action === 'deleteShare') return { ok: true };
      const u = users.find((x) => x.id === a.id);
      if (!u) throw new Error('缺少账号');
      if (a.action === 'update') {
        if (a.username !== undefined) u.username = a.username;
        if (a.phone !== undefined) u.phone = a.phone ? normPhone(a.phone) : null;
        if (u.role === 'owner' && u.id !== me) throw new Error('站长账号不能被修改、停用或删除');
        if (a.role !== undefined) {
          if (u.id === me) throw new Error('不能改变自己的身份');
          u.role = a.role;
        }
        if (a.plan !== undefined) u.plan = a.plan;
        if (a.founder !== undefined) u.founder = a.founder;
        if (a.contributor !== undefined) u.contributor = a.contributor;
      } else if (a.action === 'ban') {
        if (u.id === me) throw new Error('不能停用自己的账号');
        u.banned = a.banned;
      } else if (a.action === 'delete') {
        if (u.id === me) throw new Error('不能删除自己的账号');
        users.splice(users.indexOf(u), 1);
      } else if (a.action === 'password') u.password = a.password;
      return { ok: true };
    },
  };
}

export const api: PrismApi = window.prism ?? createPreviewApi();
export const isPreview = !window.prism;

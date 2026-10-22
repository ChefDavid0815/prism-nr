import type { LegacyPresetId } from './types';

export type ArtStyle = 'realistic' | 'stylized' | 'anime' | 'dark' | 'retro';

export interface StyleDef {
  id: ArtStyle;
  name: string;
  summary: string;
  /** Applied on top of the preset values. */
  values: Record<string, number | boolean | string>;
}

export const STYLES: StyleDef[] = [
  {
    id: 'realistic',
    name: '写实',
    summary: '照片级画面，神经渲染收益最大，可以给足强度。',
    values: {},
  },
  {
    id: 'stylized',
    name: '风格化',
    summary: '手绘、油画、低多边形一类美术，守住原作的笔触和配色。',
    values: { 'nr-renodx.intensity': 18, 'nr-renodx.color': 30, 'prism-guard.GuardStrength': 0.85, 'prism-guard.MaxChromaShift': 0.03, 'prism-guard.DetailTransfer': 0.55, 'prism-texture.GrainAmount': 0 },
  },
  {
    id: 'anime',
    name: '动漫',
    summary: '赛璐璐和描边画面。神经渲染容易给角色加上写实光影，强度压到最低，颗粒关闭。',
    values: { 'nr-renodx.intensity': 12, 'nr-renodx.structure': 40, 'nr-renodx.color': 20, 'prism-guard.GuardStrength': 0.9, 'prism-guard.MaxLumaShift': 0.06, 'prism-guard.MaxChromaShift': 0.02, 'prism-texture.DetailAmount': 0.1, 'prism-texture.GrainAmount': 0, 'prism-tone.SaturationGuard': 0.1 },
  },
  {
    id: 'dark',
    name: '暗黑',
    summary: '恐怖和暗调画面，重点防止暗部被提亮，保住氛围。',
    values: { 'prism-guard.ShadowProtect': 0.9, 'prism-tone.BlackAnchor': 0.75, 'prism-tone.HighlightRolloff': 0.45, 'prism-texture.GrainAmount': 0.12 },
  },
  {
    id: 'retro',
    name: '复古',
    summary: '老游戏和低分辨率贴图。神经渲染能补材质，但要避免把像素风抹平。',
    values: { 'nr-renodx.intensity': 24, 'prism-guard.DetailTransfer': 0.2, 'prism-texture.DetailAmount': 0.35, 'feeder.work_resolution': 100 },
  },
];

export interface GameProfile {
  key: string;
  title: string;
  steamAppIds: string[];
  names: RegExp;
  style: ArtStyle;
  /** The add-on preset of 1.1.2 and earlier; Prism Core picks its tier from the GPU. */
  preset: LegacyPresetId;
  /** Notes shown with a Prism Core plan. */
  coreNotes?: string[];
  values: Record<string, number | boolean | string>;
  notes: string[];
  /**
   * Anti-cheat that can be avoided by playing offline or launching without it. Only games listed
   * here can be installed despite anti-cheat, and only after the user accepts the risk. Online-only
   * and competitive games are never listed.
   */
  antiCheatOptIn?: { names: string[]; how: string };
  /**
   * Anti-cheat this game is known to use. It blocks the install even when its files are missing
   * from the folder (removed, bypassed, or the game not fully installed).
   */
  antiCheatKnown?: string;
}

/**
 * Curated per-game recommendations. Notes cite what the community reported; the
 * values are Prism's starting point and can be tuned in the app.
 */
export const PROFILES: GameProfile[] = [
  { key: 'f1-25', title: 'F1 25', steamAppIds: ['3059520'], names: /^f1[^a-z0-9]*25\b/i, style: 'realistic', preset: 'flagship', values: {}, notes: [], antiCheatKnown: 'EA Javelin' },
  { key: 'fc26', title: 'EA SPORTS FC 26', steamAppIds: [], names: /\bfc\s*26\b/i, style: 'realistic', preset: 'flagship', values: {}, notes: [], antiCheatKnown: 'EA Javelin' },
  { key: 'apex', title: 'Apex Legends', steamAppIds: ['1172470'], names: /apex legends/i, style: 'realistic', preset: 'flagship', values: {}, notes: [], antiCheatKnown: 'Easy Anti-Cheat' },
  { key: 'cyberpunk2077', title: '赛博朋克 2077', steamAppIds: ['1091500'], names: /cyberpunk\s*2077|赛博朋克/i, style: 'realistic', preset: 'flagship', values: { 'nr-renodx.intensity': 28 }, notes: ['社区反馈人脸、材质和光影变化明显，强度建议不超过 35%。', '游戏内开启 DLSS 超分或 DLAA，路径追踪和光线重建可以保留。'] },
  { key: 'control', title: '控制', steamAppIds: ['870780'], names: /^control|控制/i, style: 'realistic', preset: 'flagship', values: {}, notes: ['最早跑通 RenoDX 实现的游戏之一，兼容性好。', 'Prism Core 在这款游戏上实测：比每帧全分辨率跑网络快 40%。'], coreNotes: ['Prism Core 的首个实测游戏。'] },
  { key: 'witcher3', title: '巫师 3：狂猎 次世代版', steamAppIds: ['292030'], names: /witcher\s*3|巫师\s*3/i, style: 'stylized', preset: 'faithful', values: {}, notes: ['原作风格容易被明显改变，默认用原片优先。', 'NPC 脸部可能显得不自然，强度不要高。'] },
  { key: 'gtav-enhanced', title: 'GTA V 增强版', steamAppIds: ['3240220'], names: /grand theft auto v|gta\s*v/i, style: 'realistic', preset: 'balanced', values: {}, notes: ['只在故事模式使用。GTA 在线模式有反作弊，Prism 会阻止。'] },
  { key: 'stalker2', title: 'S.T.A.L.K.E.R. 2', steamAppIds: ['1643320'], names: /stalker\s*2/i, style: 'dark', preset: 'balanced', values: {}, notes: ['本身就很吃配置，建议均衡或流畅优先。'] },
  { key: 'stellarblade', title: '剑星', steamAppIds: ['3489700'], names: /stellar\s*blade|剑星/i, style: 'realistic', preset: 'balanced', values: {}, notes: ['社区反馈显存占用高，16 GB 以下显卡建议流畅优先。'] },
  { key: 'ff7rebirth', title: '最终幻想 VII 重生', steamAppIds: ['2909400'], names: /final fantasy vii rebirth|最终幻想.*重生/i, style: 'stylized', preset: 'faithful', values: { 'nr-renodx.intensity': 20 }, notes: ['角色是日式写实风格，强度过高会改变人物长相。'] },
  { key: 'acshadows', title: '刺客信条：影', steamAppIds: ['3159330'], names: /assassin'?s creed shadows|刺客信条.*影/i, style: 'realistic', preset: 'balanced', values: {}, notes: ['社区实测 RTX 4080 4K 下开销很大，前置神经渲染能挽回大部分帧率。'] },
  { key: 'oblivion-remastered', title: '上古卷轴 IV：湮没 重制版', steamAppIds: ['2623190'], names: /oblivion remastered|湮没.*重制/i, style: 'realistic', preset: 'balanced', values: {}, notes: ['社区实现仍属实验性。'] },
  { key: 'tlou2', title: '最后生还者 第二部 复刻版', steamAppIds: ['2531310'], names: /last of us part ii|最后生还者.*第二部/i, style: 'realistic', preset: 'flagship', values: { 'nr-renodx.intensity': 24 }, notes: ['社区反馈人脸可能被明显改变，强度保守一些。'] },
  { key: 'tlou1', title: '最后生还者 第一部', steamAppIds: ['1888930'], names: /last of us part i\b|最后生还者.*第一部/i, style: 'realistic', preset: 'flagship', values: { 'nr-renodx.intensity': 24 }, notes: [] },
  { key: 'avatar', title: '阿凡达：潘多拉边境', steamAppIds: ['2840770'], names: /avatar.*pandora|阿凡达/i, style: 'realistic', preset: 'balanced', values: {}, notes: ['实验性，需要兼容的渲染管线。'] },
  { key: 'wukong', title: '黑神话：悟空', steamAppIds: ['2358720'], names: /black myth|黑神话/i, style: 'realistic', preset: 'balanced', values: {}, notes: ['虚幻 5 游戏，本身负载高，推荐前置神经渲染。'] },
  { key: 'alanwake2', title: '心灵杀手 2', steamAppIds: [], names: /alan wake 2|心灵杀手\s*2/i, style: 'dark', preset: 'flagship', values: {}, notes: ['暗调画面，暗部保护已调高。'] },
  { key: 'skyrim', title: '上古卷轴 V：天际', steamAppIds: ['489830'], names: /skyrim|天际/i, style: 'retro', preset: 'balanced', values: {}, notes: ['没有原生 DLSS，走 Feeder 路线；社区已修复特别版的神经渲染。'] },
  { key: 'fallout4', title: '辐射 4', steamAppIds: ['377160'], names: /fallout\s*4|辐射\s*4/i, style: 'retro', preset: 'balanced', values: {}, notes: ['没有原生 DLSS，走 Feeder 路线。'] },
  { key: 'nba2k27', title: 'NBA 2K27', steamAppIds: [], names: /nba\s*2k27/i, style: 'realistic', preset: 'flagship', values: {}, notes: ['游戏自带 DLSS 5 选项；用 Prism 时把游戏里的神经渲染关掉，避免两层叠加。'], coreNotes: ['游戏自带 DLSS 5 选项；用 Prism Core 时把游戏里的神经渲染关掉，避免两层叠加。'], antiCheatOptIn: { names: ['Easy Anti-Cheat'], how: '只在离线模式下使用：不要进入 MyCAREER 在线、公园、排位、MyTEAM 等任何联网模式。要联网玩之前，先在 Prism 里点"还原"。' } },
];

export function matchProfile(name: string, steamAppId?: string): GameProfile | null {
  if (steamAppId) {
    const byId = PROFILES.find((p) => p.steamAppIds.includes(steamAppId));
    if (byId) return byId;
  }
  return PROFILES.find((p) => p.names.test(name)) ?? null;
}

export function styleById(id: string): StyleDef {
  return STYLES.find((s) => s.id === id) ?? STYLES[0];
}

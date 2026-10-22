// Types shared by the Electron main process and the renderer.
// Keep this file free of Node or DOM imports.

export type GpuVendor = 'nvidia' | 'amd' | 'intel' | 'unknown';
export type GpuArch = 'blackwell' | 'ada' | 'ampere' | 'turing' | 'other';
/** Hardware class used to pick presets. */
export type GpuTier = 'flagship' | 'high' | 'mainstream' | 'entry' | 'legacy' | 'unsupported';
/** How DLSS 5 neural rendering is expected to behave on this GPU. */
export type NrSupport = 'official' | 'experimental' | 'none';

export interface GpuInfo {
  name: string;
  vendor: GpuVendor;
  arch: GpuArch;
  laptop: boolean;
  vramMB: number | null;
  driver: string | null;
  tier: GpuTier;
  nrSupport: NrSupport;
  /** Plain-language expectation shown to the user. */
  expectation: string;
}

export interface SystemInfo {
  gpus: GpuInfo[];
  primary: GpuInfo | null;
  os: { release: string; build: number; isWin11: boolean };
  /** Problems worth showing on the dashboard. */
  notes: string[];
}

export type GameStore = 'steam' | 'epic' | 'gog' | 'xbox' | 'manual';

export interface GameEntry {
  id: string;
  name: string;
  store: GameStore;
  installDir: string;
  /** Launch executable reported by the store, if any. */
  storeExe?: string;
  steamAppId?: string;
}

export type GraphicsApi = 'dx9' | 'dx10' | 'dx11' | 'dx12' | 'vulkan' | 'opengl';
export type Bitness = 'x64' | 'x86' | 'unknown';

/**
 * How neural rendering gets into a game.
 * - prism-core: Prism's own runtime (version.dll loader) hooks the game's DX12 DLSS calls and runs the
 *   network itself, on one frame in N, carrying its edit in between. No ReShade.
 * - native-hook: the game already calls DLSS; a ReShade add-on hooks those calls (post-upscale, best quality).
 * - optiscaler: OptiScaler replaces the upscaler and can run neural rendering before upscaling (cheapest).
 * - feeder: the game has no DLSS; DLSS5-Feeder builds the inputs from ReShade depth and estimated motion.
 * - blocked: anti-cheat or an online-only game; Prism refuses to install.
 */
export type Route = 'prism-core' | 'native-hook' | 'optiscaler' | 'feeder' | 'blocked' | 'unknown';

export interface AntiCheatHit {
  name: string;
  evidence: string;
}

export interface ExistingMods {
  reshade: boolean;
  optiscaler: boolean;
  prism: boolean;
  /** Proxy DLLs next to the executable that could conflict (dxgi.dll, d3d11.dll ...). */
  proxies: string[];
}

export interface GameAnalysis {
  /** Analyzer version; cached analyses from an older one are redone. */
  schema?: number;
  gameId: string;
  exe: string | null;
  exeCandidates: string[];
  bitness: Bitness;
  apis: GraphicsApi[];
  nativeDlss: { present: boolean; path?: string; version?: string };
  streamline: boolean;
  dlssFrameGen: boolean;
  /** The executable imports VERSION.dll, so Prism Core's loader is picked up at start-up. */
  importsVersionDll?: boolean;
  antiCheat: AntiCheatHit[];
  /** Anti-cheat this game lets players avoid offline: installable after the user accepts the risk. */
  antiCheatOptIn?: { names: string[]; how: string };
  existing: ExistingMods;
  routes: Route[];
  recommendedRoute: Route;
  routeReason: string;
  warnings: string[];
  analyzedAt: string;
}

export type PluginCategory = 'core' | 'performance' | 'quality' | 'deai' | 'stability';
export type PluginOrigin = 'prism' | 'community' | 'nvidia';

export interface PluginParam {
  key: string;
  label: string;
  help: string;
  type: 'range' | 'toggle' | 'choice';
  min?: number;
  max?: number;
  step?: number;
  unit?: string;
  choices?: { value: string; label: string }[];
  default: number | boolean | string;
}

export interface PluginDef {
  /** Stable code shown everywhere, e.g. PR-04. */
  code: string;
  id: string;
  name: string;
  category: PluginCategory;
  origin: PluginOrigin;
  /** One sentence: what it is for. */
  summary: string;
  /** What it actually does, in plain language. */
  does: string[];
  /** Frame cost, as honest as we can state it. */
  cost: string;
  /** Which GPUs it is meant for. */
  gpu: string;
  routes: Route[];
  /** Ids of plugins that cannot be enabled together with this one. */
  conflicts: string[];
  /** Ids of plugins that must also be enabled. */
  requires: string[];
  risk?: string;
  /** Third-party source the user imports, or the Prism shader file. */
  source?: { name: string; url?: string; license?: string; note?: string };
  shader?: { file: string; technique: string; order: number };
  params: PluginParam[];
}

/** Prism Core tiers. */
export type PresetId = 'extreme' | 'high' | 'standard' | 'light';
/** Presets of earlier versions, still found in install records and history. */
export type LegacyPresetId = 'core' | 'flagship' | 'balanced' | 'fluid' | 'faithful';

export interface PresetDef {
  id: PresetId;
  name: string;
  forTiers: GpuTier[];
  gpuLabel: string;
  summary: string;
  /** Route preference order for native-DLSS games. */
  nativeRoutePreference: Route[];
  plugins: string[];
  values: Record<string, number | boolean | string>;
  /** Honest expectation, never a promise. */
  expectation: string;
}

export interface ComponentInfo {
  id: string;
  name: string;
  version: string | null;
  path: string;
  files: string[];
  importedAt: string;
}

export interface NvidiaDll {
  file: 'nvngx_dlssnr.dll' | 'nvngx_dlss.dll' | 'nvngx_dlssg.dll';
  path: string;
  version: string | null;
  signed: boolean;
  signer: string | null;
  /** Shipped inside the Prism NR installer rather than found on this PC. */
  bundled?: boolean;
}

export type FileOp =
  | { kind: 'copy'; from: string; to: string; label: string }
  | { kind: 'write'; to: string; content: string; label: string }
  /** Moves a file out of the game folder (kept in the backup, put back on rollback). */
  | { kind: 'remove'; to: string; label: string };

export interface PlanNeed {
  id: string;
  name: string;
  /** ready: on disk. auto: Prism downloads it. manual: the user must import it once. */
  status: 'ready' | 'auto' | 'manual';
  hint?: string;
}

export interface InstallPlan {
  gameId: string;
  gameName: string;
  route: Route;
  preset: PresetId;
  style: string;
  plugins: string[];
  values: Record<string, number | boolean | string>;
  needs: PlanNeed[];
  /** What the install will do, in order, in plain language. */
  steps: string[];
  /** Things the user still has to do by hand (enable DLSS in game, etc.). */
  manualSteps: string[];
  /** Things missing before the plan can run. */
  blockers: string[];
  warnings: string[];
  profileNotes: string[];
  /** Set for games with anti-cheat that can be avoided offline: install needs the user's consent. */
  riskConsent?: { names: string[]; how: string; accepted: boolean };
}

export interface InstallProgress {
  gameId: string;
  stage: 'fetch' | 'deploy' | 'verify' | 'done' | 'error';
  message: string;
  /** 0..1 */
  fraction: number;
}

export interface BackupRecord {
  target: string;
  /** Null when the file did not exist before install. */
  backup: string | null;
  sha256After: string;
}

export interface InstallRecord {
  id: string;
  gameId: string;
  gameName: string;
  installDir: string;
  route: Route;
  preset: PresetId | LegacyPresetId;
  plugins: string[];
  /** Values written at install (and later tuning), so the app can show and edit them. */
  values?: Record<string, number | boolean | string>;
  files: BackupRecord[];
  installedAt: string;
  status: 'active' | 'rolled-back' | 'superseded';
  rolledBackAt?: string;
  /** Files the user changed after install; rollback left them alone or restored them on request. */
  rollbackNotes?: string[];
  /** When the user accepted the anti-cheat risk for this install. */
  riskAcceptedAt?: string;
}

export interface DiagnosticsReport {
  generatedAt: string;
  system: SystemInfo;
  games: number;
  installs: InstallRecord[];
  components: ComponentInfo[];
  nvidiaDlls: NvidiaDll[];
  log: string[];
}

export interface PlanRequest {
  gameId: string;
  /** Omit to use the game profile or the GPU's recommended preset. */
  preset?: PresetId;
  style?: string;
  /** Omit to use the preset's plugin list. */
  plugins?: string[];
  overrides?: Record<string, number | boolean | string>;
  route?: Route;
  /** The user read and accepted the anti-cheat risk shown for this game. */
  acceptRisk?: boolean;
}

export interface PrismApi {
  getSystem(): Promise<SystemInfo>;
  scanGames(): Promise<GameEntry[]>;
  analyzeGame(id: string): Promise<GameAnalysis>;
  addManualGame(): Promise<GameEntry | null>;
  /** Games whose Prism Core install was brought up to date in place on this start. */
  coreUpgrades(): Promise<string[]>;
  /** Takes a manually added folder off the list (files are untouched). */
  removeManualGame(id: string): Promise<void>;
  getCatalog(): Promise<{ plugins: PluginDef[]; presets: PresetDef[] }>;
  listComponents(): Promise<ComponentInfo[]>;
  importComponent(id: string): Promise<ComponentInfo | null>;
  locateNvidiaDlls(): Promise<NvidiaDll[]>;
  planInstall(req: PlanRequest): Promise<InstallPlan>;
  runInstall(plan: InstallPlan): Promise<InstallRecord>;
  onProgress(cb: (p: InstallProgress) => void): () => void;
  getSettings(): Promise<{ motion: 'max' | 'standard' | 'off' }>;
  setMotion(m: 'max' | 'standard' | 'off'): Promise<void>;
  listInstalls(): Promise<InstallRecord[]>;
  rollback(recordId: string): Promise<InstallRecord>;
  /** Rewrites prism-core.ini of an active Prism Core install; a running game picks it up within a second. */
  updateCoreConfig(recordId: string, values: Record<string, number | boolean | string>): Promise<InstallRecord>;
  diagnostics(): Promise<DiagnosticsReport>;
  exportDiagnostics(): Promise<string | null>;
  openPath(p: string): Promise<void>;
  openExternal(url: string): Promise<void>;
}

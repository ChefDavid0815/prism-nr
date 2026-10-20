import { PLUGINS, PRESETS } from '../shared/catalog';
import type { DiagnosticsReport, GameAnalysis, GameEntry, InstallPlan, InstallProgress, NvidiaDll, PlanRequest, SystemInfo } from '../shared/types';
import { analyzeGame } from './analyzer';
import { importComponent, listComponents } from './components';
import { buildPlan, learnAddonSettings, rollback, runInstall } from './installer';
import { locateNvidiaDlls } from './nvidia';
import { leftoverDirs, manualEntry, scanAllGames } from './scanner';
import { store } from './store';
import { detectSystem } from './system';
import { getLog } from './util';

/** The application layer shared by the Electron IPC handlers and the self-test CLI. */
export class PrismService {
  private system: Promise<SystemInfo> | null = null;
  private games: GameEntry[] = [];
  private dlls: Promise<NvidiaDll[]> | null = null;
  private analyses: Record<string, GameAnalysis> = store.analysisCache();

  async getSystem(): Promise<SystemInfo> {
    // Cache the promise so concurrent callers share one detection.
    this.system ??= detectSystem();
    return this.system;
  }

  async scanGames(): Promise<GameEntry[]> {
    this.games = await scanAllGames(store.manualGames());
    learnAddonSettings();
    return this.games;
  }

  private game(id: string): GameEntry {
    const g = this.games.find((x) => x.id === id);
    if (!g) throw new Error('找不到这个游戏，请重新扫描。');
    return g;
  }

  async analyzeGame(id: string, force = false): Promise<GameAnalysis> {
    const cached = this.analyses[id];
    if (cached && !force) return cached;
    const a = await analyzeGame(this.game(id));
    this.analyses[id] = a;
    store.saveAnalysisCache(this.analyses);
    return a;
  }

  addManual(dir: string): GameEntry {
    const entry = manualEntry(dir);
    const manual = store.manualGames().filter((g) => g.id !== entry.id);
    store.saveManualGames([...manual, entry]);
    if (!this.games.some((g) => g.id === entry.id)) this.games.push(entry);
    return entry;
  }

  catalog() {
    return { plugins: PLUGINS, presets: PRESETS };
  }

  listComponents() {
    return listComponents();
  }

  importComponent(id: string, source: string) {
    return importComponent(id, source);
  }

  async nvidiaDlls(force = false): Promise<NvidiaDll[]> {
    if (!this.dlls || force) this.dlls = locateNvidiaDlls(this.games, leftoverDirs);
    return this.dlls;
  }

  async plan(req: PlanRequest): Promise<InstallPlan> {
    const game = this.game(req.gameId);
    const [system, analysis, dlls] = await Promise.all([this.getSystem(), this.analyzeGame(req.gameId), this.nvidiaDlls()]);
    return buildPlan(game, analysis, system.primary, dlls, req);
  }

  async install(plan: InstallPlan, progress: (p: InstallProgress) => void) {
    const game = this.game(plan.gameId);
    const analysis = await this.analyzeGame(plan.gameId);
    return runInstall(plan, game, analysis, await this.nvidiaDlls(), progress);
  }

  listInstalls() {
    learnAddonSettings();
    return store.installs();
  }

  rollback(id: string) {
    return rollback(id);
  }

  getSettings() {
    return { motion: store.settings().motion };
  }

  setMotion(m: 'max' | 'standard' | 'off') {
    const s = store.settings();
    s.motion = m;
    store.saveSettings(s);
  }

  async diagnostics(): Promise<DiagnosticsReport> {
    return {
      generatedAt: new Date().toISOString(),
      system: await this.getSystem(),
      games: this.games.length,
      installs: store.installs(),
      components: listComponents(),
      nvidiaDlls: this.dlls ? await this.dlls : [],
      log: getLog(),
    };
  }
}

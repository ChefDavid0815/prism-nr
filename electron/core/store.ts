import * as fs from 'node:fs';
import * as path from 'node:path';
import type { GameAnalysis, GameEntry, InstallRecord } from '../shared/types';
import { ensureDir } from './util';

let base = '';

export function initStore(dataDir: string): void {
  base = dataDir;
  for (const d of ['components', 'downloads', 'backups', 'installs']) ensureDir(path.join(base, d));
}

export const dirs = {
  get base() {
    return base;
  },
  get components() {
    return path.join(base, 'components');
  },
  get downloads() {
    return path.join(base, 'downloads');
  },
  get backups() {
    return path.join(base, 'backups');
  },
  get installs() {
    return path.join(base, 'installs');
  },
};

function readJson<T>(name: string, fallback: T): T {
  try {
    return JSON.parse(fs.readFileSync(path.join(base, name), 'utf8')) as T;
  } catch {
    return fallback;
  }
}

function writeJson(name: string, value: unknown): void {
  const file = path.join(base, name);
  const tmp = file + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(value, null, 2), 'utf8');
  fs.renameSync(tmp, file);
}

export interface Settings {
  motion: 'max' | 'standard' | 'off';
  /** Addon settings learned from ReShade.ini after the user enabled them once, keyed by section. */
  learnedAddonSections: Record<string, string>;
  /** User-chosen art style per game. */
  styles: Record<string, string>;
}

const defaultSettings: Settings = { motion: 'max', learnedAddonSections: {}, styles: {} };

export const store = {
  manualGames: () => readJson<GameEntry[]>('manual-games.json', []),
  saveManualGames: (g: GameEntry[]) => writeJson('manual-games.json', g),
  analysisCache: () => readJson<Record<string, GameAnalysis>>('analysis-cache.json', {}),
  saveAnalysisCache: (c: Record<string, GameAnalysis>) => writeJson('analysis-cache.json', c),
  installs: () => readJson<InstallRecord[]>('installs.json', []),
  saveInstalls: (r: InstallRecord[]) => writeJson('installs.json', r),
  settings: (): Settings => ({ ...defaultSettings, ...readJson<Partial<Settings>>('settings.json', {}) }),
  saveSettings: (s: Settings) => writeJson('settings.json', s),
};

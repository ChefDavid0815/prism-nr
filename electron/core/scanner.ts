import * as fs from 'node:fs';
import * as path from 'node:path';
import type { GameEntry } from '../shared/types';
import { exists, isDir, log, readText, regSubkeys, regValue, stableId } from './util';

/** Minimal Valve KeyValues parser, enough for libraryfolders.vdf and appmanifest_*.acf. */
export function parseVdf(text: string): Record<string, unknown> {
  const tokens: string[] = [];
  const re = /"((?:[^"\\]|\\.)*)"|([{}])/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(text))) tokens.push(m[1] !== undefined ? m[1].replace(/\\\\/g, '\\') : m[2]);
  let i = 0;
  const readObject = (): Record<string, unknown> => {
    const obj: Record<string, unknown> = {};
    while (i < tokens.length) {
      const key = tokens[i++];
      if (key === '}') return obj;
      const next = tokens[i++];
      if (next === '{') obj[key] = readObject();
      else obj[key] = next;
    }
    return obj;
  };
  return readObject();
}

// Steam app ids that are runtimes or tools, not games.
const STEAM_TOOL_IDS = new Set(['228980', '1070560', '1391110', '1628350', '1493710', '2180100', '1161040', '1826330', '250820', '1054830']);
const STEAM_TOOL_NAME = /^(Steamworks Common|Proton|Steam Linux Runtime|SteamVR|Steam Audio|Wallpaper Engine)/i;

async function scanSteam(): Promise<GameEntry[]> {
  const steamPath =
    (await regValue('HKCU\\Software\\Valve\\Steam', 'SteamPath')) ??
    (await regValue('HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam', 'InstallPath'));
  if (!steamPath) return [];
  const root = path.normalize(steamPath.replace(/\//g, '\\'));
  const libs = new Set<string>([root]);
  const vdf = readText(path.join(root, 'steamapps', 'libraryfolders.vdf'));
  if (vdf) {
    const data = parseVdf(vdf);
    const folders = (data.libraryfolders ?? data.LibraryFolders ?? {}) as Record<string, unknown>;
    for (const v of Object.values(folders)) {
      if (typeof v === 'string' && isDir(v)) libs.add(path.normalize(v));
      else if (v && typeof v === 'object' && typeof (v as Record<string, unknown>).path === 'string') libs.add(path.normalize((v as Record<string, string>).path));
    }
  }
  const games: GameEntry[] = [];
  for (const lib of libs) {
    const appsDir = path.join(lib, 'steamapps');
    let files: string[] = [];
    try {
      files = fs.readdirSync(appsDir).filter((f) => /^appmanifest_\d+\.acf$/i.test(f));
    } catch {
      continue;
    }
    for (const f of files) {
      const acf = readText(path.join(appsDir, f));
      if (!acf) continue;
      const state = (parseVdf(acf).AppState ?? {}) as Record<string, string>;
      if (!state.appid || !state.installdir || STEAM_TOOL_IDS.has(state.appid) || STEAM_TOOL_NAME.test(state.name ?? '')) continue;
      const installDir = path.join(appsDir, 'common', state.installdir);
      if (!isDir(installDir)) continue;
      games.push({ id: stableId('steam', state.appid), name: state.name || state.installdir, store: 'steam', installDir, steamAppId: state.appid });
    }
  }
  return games;
}

async function scanEpic(): Promise<GameEntry[]> {
  const dir = path.join(process.env.ProgramData ?? 'C:\\ProgramData', 'Epic', 'EpicGamesLauncher', 'Data', 'Manifests');
  let files: string[] = [];
  try {
    files = fs.readdirSync(dir).filter((f) => f.toLowerCase().endsWith('.item'));
  } catch {
    return [];
  }
  const games: GameEntry[] = [];
  for (const f of files) {
    try {
      const j = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8')) as Record<string, unknown>;
      const cats = (j.AppCategories as string[] | undefined) ?? [];
      if (cats.length && !cats.includes('games')) continue;
      if (cats.some((c) => /plugins|engines|assets/i.test(c)) || /\b(plugin|unreal engine|launcher|quixel|bridge|twinmotion|realitycapture|realityscan|metahuman)\b/i.test(String(j.DisplayName ?? ''))) continue;
      const installDir = String(j.InstallLocation ?? '');
      if (!installDir || !isDir(installDir)) continue;
      const exe = j.LaunchExecutable ? path.join(installDir, String(j.LaunchExecutable)) : undefined;
      games.push({ id: stableId('epic', String(j.AppName ?? installDir)), name: String(j.DisplayName ?? path.basename(installDir)), store: 'epic', installDir, storeExe: exe && exists(exe) ? exe : undefined });
    } catch {
      /* ignore broken manifest */
    }
  }
  return games;
}

async function scanGog(): Promise<GameEntry[]> {
  const games: GameEntry[] = [];
  for (const base of ['HKLM\\SOFTWARE\\WOW6432Node\\GOG.com\\Games', 'HKLM\\SOFTWARE\\GOG.com\\Games']) {
    for (const key of await regSubkeys(base)) {
      const installDir = await regValue(key, 'path');
      if (!installDir || !isDir(installDir)) continue;
      const name = (await regValue(key, 'gameName')) ?? path.basename(installDir);
      const exe = await regValue(key, 'exe');
      games.push({ id: stableId('gog', key.split('\\').pop() ?? installDir), name, store: 'gog', installDir, storeExe: exe && exists(exe) ? exe : undefined });
    }
  }
  return games;
}

async function scanXbox(): Promise<GameEntry[]> {
  const games: GameEntry[] = [];
  for (let c = 67; c <= 90; c++) {
    const root = `${String.fromCharCode(c)}:\\XboxGames`;
    if (!isDir(root)) continue;
    for (const name of fs.readdirSync(root)) {
      if (name === 'GameSave') continue;
      const content = path.join(root, name, 'Content');
      // A real Xbox install has Content\MicrosoftGame.config; anything else is a user folder.
      if (!exists(path.join(content, 'MicrosoftGame.config'))) continue;
      const config = readText(path.join(content, 'MicrosoftGame.config')) ?? '';
      const title = config.match(/<ShellVisuals[^>]*DefaultDisplayName="([^"]+)"/i)?.[1];
      games.push({ id: stableId('xbox', content), name: title && !/^ms-resource/i.test(title) ? title : name, store: 'xbox', installDir: content });
    }
  }
  return games;
}

export async function scanAllGames(manual: GameEntry[]): Promise<GameEntry[]> {
  const results = await Promise.allSettled([scanSteam(), scanEpic(), scanGog(), scanXbox()]);
  const all: GameEntry[] = [];
  results.forEach((r, i) => {
    if (r.status === 'fulfilled') all.push(...r.value);
    else log(`scanner ${['steam', 'epic', 'gog', 'xbox'][i]} failed: ${String(r.reason)}`);
  });
  all.push(...manual);
  // De-duplicate by install directory; the first store wins.
  const seen = new Set<string>();
  const unique = all.filter((g) => {
    const k = path.resolve(g.installDir).toLowerCase();
    if (seen.has(k)) return false;
    seen.add(k);
    return true;
  });
  unique.sort((a, b) => a.name.localeCompare(b.name, 'zh-CN'));
  log(`scanner: ${unique.length} games`);
  return unique;
}

export function manualEntry(dir: string): GameEntry {
  return { id: stableId('manual', dir), name: path.basename(dir), store: 'manual', installDir: dir };
}

import * as fs from 'node:fs';
import * as path from 'node:path';
import type { ComponentInfo } from '../shared/types';
import { dirs } from './store';
import { ensureDir, log, run, sha256File, walk } from './util';

/**
 * Where each third-party component comes from. Prism downloads only from the
 * project's own release channel and verifies the SHA-256 GitHub publishes for
 * the asset. NVIDIA binaries are never downloaded; see nvidia.ts.
 */
export interface ComponentSource {
  id: string;
  name: string;
  kind: 'github' | 'reshade-site' | 'manual';
  repo?: string;
  asset?: RegExp;
  /** Only releases whose tag matches; the repository may publish unrelated releases. */
  tag?: RegExp;
  homepage: string;
  license: string;
  /** Shown when the component cannot be fetched automatically. */
  manualHint?: string;
}

export const SOURCES: ComponentSource[] = [
  { id: 'reshade', name: 'ReShade（含插件支持）', kind: 'reshade-site', homepage: 'https://reshade.me', license: 'BSD-3-Clause' },
  { id: 'feeder', name: 'DLSS5-Feeder', kind: 'github', repo: 'jlrouzies-fr/DLSS5-Feeder', asset: /^DLSS5-Feeder-[\d.]+\.zip$/i, homepage: 'https://github.com/jlrouzies-fr/DLSS5-Feeder', license: '见项目仓库' },
  { id: 'renodx-dlss5', name: 'RenoDX DLSS 5 插件', kind: 'github', repo: 'RankFTW/rhi-repo', tag: /^renodx-dlss5-[\d.]+$/i, asset: /^renodx-dlss5_[\d.]+\.zip$/i, homepage: 'https://github.com/RankFTW/rhi-repo', license: '见项目仓库', manualHint: '如果自动获取失败，从 RenoDX 官方 Discord 下载 .addon64 文件后导入。' },
  { id: 'optiscaler-nr', name: 'OptiScaler DLSS-NR（稳定版）', kind: 'github', repo: 'Dagherbou/OptiScaler_DLSSNR', asset: /\.(7z|zip)$/i, homepage: 'https://github.com/Dagherbou/OptiScaler_DLSSNR', license: 'GPL-3.0' },
  { id: 'optiscaler-presr', name: 'OptiScaler DLSS-NR Pre-SR', kind: 'github', repo: 'wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass', asset: /\.(7z|zip)$/i, homepage: 'https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass', license: 'GPL-3.0' },
  { id: 'lumenite', name: 'LumeniteFX 运动矢量', kind: 'manual', homepage: 'https://github.com/umar-afzaal/LumeniteFX', license: 'AGNYA License（作者授权随 Prism NR 分发）', manualHint: 'Prism NR 安装包自带，一般不用手动导入；需要换版本时从作者的官方链接下载后导入。' },
  { id: 'dfc', name: 'Deep Fried Chicken', kind: 'manual', homepage: 'https://github.com/jlrouzies-fr/DLSS5-Feeder', license: '见作者说明', manualHint: '下载后导入。Prism 默认用 RenoDX 插件，不装也可以。' },
  { id: 'mgpu', name: 'MGPU Bridge', kind: 'manual', homepage: 'https://github.com/maohgad-web/Neural-coprocessor', license: '见项目仓库', manualHint: '双卡用户下载后导入。' },
];

// Third-party components shipped inside the installer (resources/components/<id>/), imported on
// first use so nobody has to import them by hand. Only components whose authors allow it go here.
let bundledDir = '';
export function setBundledComponentsDir(dir: string): void {
  bundledDir = dir;
}
function bundledSource(id: string): string | null {
  if (!bundledDir) return null;
  const d = path.join(bundledDir, id);
  try {
    const entries = fs.readdirSync(d);
    if (!entries.length) return null;
    // One archive: import the archive; anything else: import the folder.
    return entries.length === 1 && /\.(zip|7z|rar)$/i.test(entries[0]) ? path.join(d, entries[0]) : d;
  } catch {
    return null;
  }
}
export function isBundled(id: string): boolean {
  return bundledSource(id) !== null;
}

const SEVEN_ZIP = (() => {
  try {
    // eslint-disable-next-line @typescript-eslint/no-var-requires
    const p = require('7zip-bin').path7za as string;
    // In a packaged app the binary is unpacked next to app.asar.
    return p.replace('app.asar', 'app.asar.unpacked');
  } catch {
    return '7za.exe';
  }
})();

export async function extractArchive(archive: string, outDir: string): Promise<void> {
  ensureDir(outDir);
  // The bundled 7za has no RAR support; Windows' own tar (libarchive) reads RAR.
  if (/\.rar$/i.test(archive)) {
    const tar = path.join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'tar.exe');
    const t = await run(tar, ['-xf', archive, '-C', outDir], 180000);
    if (t.code !== 0) throw new Error(`解压失败：${path.basename(archive)}（${t.stderr.trim() || 'RAR 需要 Windows 10 1803 以上自带的 tar'}）`);
    return;
  }
  const r = await run(SEVEN_ZIP, ['x', '-y', `-o${outDir}`, archive], 180000);
  if (r.code !== 0) throw new Error(`解压失败：${path.basename(archive)}（${r.stderr.trim() || r.stdout.slice(-200)}）`);
}

function listFiles(dir: string): string[] {
  const out: string[] = [];
  walk(dir, (f, _d, isDir) => {
    if (!isDir) out.push(path.relative(dir, f));
  }, { maxDepth: 8, maxEntries: 5000 });
  return out;
}

function metaPath(id: string): string {
  return path.join(dirs.components, id, 'component.json');
}

export function listComponents(): ComponentInfo[] {
  const out: ComponentInfo[] = [];
  for (const s of SOURCES) {
    try {
      out.push(JSON.parse(fs.readFileSync(metaPath(s.id), 'utf8')) as ComponentInfo);
    } catch {
      /* not installed */
    }
  }
  return out;
}

export function getComponent(id: string): ComponentInfo | null {
  return listComponents().find((c) => c.id === id) ?? null;
}

function saveComponent(id: string, version: string | null, contentDir: string): ComponentInfo {
  const info: ComponentInfo = {
    id,
    name: SOURCES.find((s) => s.id === id)?.name ?? id,
    version,
    path: contentDir,
    files: listFiles(contentDir),
    importedAt: new Date().toISOString(),
  };
  fs.writeFileSync(metaPath(id), JSON.stringify(info, null, 2));
  return info;
}

async function download(url: string, dest: string, expectedSha256?: string): Promise<void> {
  const res = await fetch(url, { headers: { 'User-Agent': 'PrismNR', Accept: 'application/octet-stream' }, redirect: 'follow' });
  if (!res.ok) throw new Error(`下载失败 ${res.status}：${url}`);
  const buf = Buffer.from(await res.arrayBuffer());
  fs.writeFileSync(dest, buf);
  if (expectedSha256) {
    const got = sha256File(dest);
    if (got !== expectedSha256.toLowerCase()) {
      fs.rmSync(dest, { force: true });
      throw new Error(`校验失败：${path.basename(dest)} 的 SHA-256 与发布页不一致，已删除。`);
    }
  }
}

interface GhAsset {
  name: string;
  browser_download_url: string;
  digest?: string | null;
}
interface GhRelease {
  tag_name: string;
  prerelease: boolean;
  draft: boolean;
  assets: GhAsset[];
}

async function fetchGithub(src: ComponentSource, onProgress: (msg: string) => void): Promise<ComponentInfo> {
  onProgress(`查询 ${src.name} 的最新版本`);
  const res = await fetch(`https://api.github.com/repos/${src.repo}/releases?per_page=100`, { headers: { 'User-Agent': 'PrismNR', Accept: 'application/vnd.github+json' } });
  if (!res.ok) throw new Error(`无法访问 ${src.repo} 的发布页（${res.status}）`);
  const releases = ((await res.json()) as GhRelease[]).filter((r) => !r.draft && (!src.tag || src.tag.test(r.tag_name)));
  const ordered = [...releases.filter((r) => !r.prerelease), ...releases.filter((r) => r.prerelease)];
  for (const rel of ordered) {
    const asset = rel.assets.find((a) => src.asset!.test(a.name));
    if (!asset) continue;
    const sha = asset.digest?.startsWith('sha256:') ? asset.digest.slice(7) : undefined;
    const dl = path.join(dirs.downloads, `${src.id}-${rel.tag_name}-${asset.name}`);
    onProgress(`下载 ${asset.name}`);
    await download(asset.browser_download_url, dl, sha);
    if (!sha) log(`component ${src.id}: release has no digest, size check only`);
    const outDir = path.join(dirs.components, src.id, rel.tag_name);
    fs.rmSync(outDir, { recursive: true, force: true });
    if (/\.(zip|7z)$/i.test(asset.name)) {
      onProgress(`解压 ${asset.name}`);
      await extractArchive(dl, outDir);
    } else {
      ensureDir(outDir);
      fs.copyFileSync(dl, path.join(outDir, asset.name));
    }
    stripNvidiaBinaries(outDir);
    return saveComponent(src.id, rel.tag_name, outDir);
  }
  throw new Error(`${src.repo} 最近的发布里没有找到匹配的文件。`);
}

async function fetchReshade(onProgress: (msg: string) => void): Promise<ComponentInfo> {
  onProgress('查询 ReShade 最新版本');
  const page = await (await fetch('https://reshade.me/', { headers: { 'User-Agent': 'PrismNR' } })).text();
  const m = page.match(/href="([^"]*ReShade_Setup_([\d.]+)_Addon\.exe)"/i);
  if (!m) throw new Error('没有在 reshade.me 找到带插件支持的安装包链接。');
  const url = new URL(m[1], 'https://reshade.me/').toString();
  const version = m[2];
  const dl = path.join(dirs.downloads, `ReShade_Setup_${version}_Addon.exe`);
  onProgress(`下载 ReShade ${version}`);
  await download(url, dl);
  // The setup executable carries the DLLs as an embedded archive.
  const outDir = path.join(dirs.components, 'reshade', version);
  fs.rmSync(outDir, { recursive: true, force: true });
  onProgress('解出 ReShade64.dll / ReShade32.dll');
  await extractArchive(dl, outDir);
  const files = listFiles(outDir);
  if (!files.some((f) => /ReShade64\.dll$/i.test(f))) throw new Error('ReShade 安装包里没有找到 ReShade64.dll。');
  return saveComponent('reshade', version, outDir);
}

/** Prism never keeps NVIDIA binaries that arrive inside third-party archives; only local signed copies are used. */
function stripNvidiaBinaries(dir: string): void {
  walk(dir, (f, _d, isDir) => {
    if (!isDir && /^(nvngx|_nvngx|sl\.)[^\\/]*\.dll$/i.test(path.basename(f))) {
      fs.rmSync(f, { force: true });
      log(`removed bundled NVIDIA binary ${path.basename(f)}`);
    }
  }, { maxDepth: 8, maxEntries: 5000 });
}

/** Fetches a component automatically. Throws with a user-facing message when that is not possible. */
export async function ensureComponent(id: string, onProgress: (msg: string) => void = () => {}): Promise<ComponentInfo> {
  const have = getComponent(id);
  if (have && fs.existsSync(have.path)) return have;
  const src = SOURCES.find((s) => s.id === id);
  if (!src) throw new Error(`未知组件 ${id}`);
  ensureDir(path.join(dirs.components, id));
  const shipped = bundledSource(id);
  if (shipped) {
    onProgress(`安装 Prism NR 自带的 ${src.name}`);
    const info = await importComponent(id, shipped);
    log(`component ${id}: imported the bundled copy`);
    return info;
  }
  if (src.kind === 'reshade-site') return fetchReshade(onProgress);
  if (src.kind === 'github') return fetchGithub(src, onProgress);
  throw new Error(`${src.name} 需要手动导入。${src.manualHint ?? ''}`);
}

/** Imports a user-provided archive or folder as a component. */
export async function importComponent(id: string, source: string): Promise<ComponentInfo> {
  const outDir = path.join(dirs.components, id, 'imported-' + Date.now());
  ensureDir(outDir);
  const stat = fs.statSync(source);
  if (stat.isDirectory()) fs.cpSync(source, outDir, { recursive: true });
  else if (/\.(zip|7z|rar|exe)$/i.test(source)) await extractArchive(source, outDir);
  else fs.copyFileSync(source, path.join(outDir, path.basename(source)));
  return saveComponent(id, 'imported', outDir);
}

/** Finds the first file in a component whose relative path matches. */
export function componentFile(c: ComponentInfo, pattern: RegExp): string | null {
  const rel = c.files.find((f) => pattern.test(f.replace(/\//g, '\\')));
  return rel ? path.join(c.path, rel) : null;
}

export function componentFiles(c: ComponentInfo, pattern: RegExp): string[] {
  return c.files.filter((f) => pattern.test(f.replace(/\//g, '\\'))).map((f) => path.join(c.path, f));
}

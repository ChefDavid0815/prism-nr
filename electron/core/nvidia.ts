import * as path from 'node:path';
import type { GameEntry, NvidiaDll } from '../shared/types';
import { exists, isDir, log, psJson, walk } from './util';

const NAMES = ['nvngx_dlssnr.dll', 'nvngx_dlss.dll', 'nvngx_dlssg.dll'] as const;

/**
 * Finds NVIDIA NGX runtimes already on this PC and verifies each is signed by NVIDIA.
 * Prism never downloads or redistributes these files; it only reuses copies the
 * user legally has (a game that ships them, the NGX model cache, the driver store).
 */
export async function locateNvidiaDlls(games: GameEntry[], extraDirs: string[] = []): Promise<NvidiaDll[]> {
  const found = new Map<string, string>();
  const roots: { dir: string; depth: number }[] = [
    { dir: path.join(process.env.ProgramData ?? 'C:\\ProgramData', 'NVIDIA', 'NGX'), depth: 6 },
    { dir: path.join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'DriverStore', 'FileRepository'), depth: 2 },
  ];
  // Games come first for the neural model: today it ships inside games, not the driver.
  const priority = [...games].sort((a, b) => Number(/nba\s*2k27/i.test(b.name)) - Number(/nba\s*2k27/i.test(a.name)));
  for (const g of priority) roots.push({ dir: g.installDir, depth: 4 });
  for (const d of extraDirs) if (!games.some((g) => g.installDir === d)) roots.push({ dir: d, depth: 4 });

  for (const r of roots) {
    if (!isDir(r.dir)) continue;
    walk(
      r.dir,
      (file, _d, isDirectory) => {
        if (isDirectory) return;
        const base = path.basename(file).toLowerCase();
        if ((NAMES as readonly string[]).includes(base)) {
          const key = file.toLowerCase();
          if (!found.has(key)) found.set(key, file);
        }
      },
      { maxDepth: r.depth, maxEntries: 60000 },
    );
  }
  // Never pick up copies Prism itself placed in game folders.
  const candidates = [...found.values()].filter((p) => !exists(path.join(path.dirname(p), 'prism-nr.manifest.json')));
  if (!candidates.length) return [];

  const list = candidates.map((p) => `'${p.replace(/'/g, "''")}'`).join(',');
  const rows = await psJson<{ Path: string; Version: string | null; Status: string; Signer: string | null }[] | { Path: string; Version: string | null; Status: string; Signer: string | null }>(
    `@(${list}) | ForEach-Object { $s = Get-AuthenticodeSignature -LiteralPath $_; [pscustomobject]@{ Path = $_; Version = (Get-Item -LiteralPath $_).VersionInfo.FileVersion; Status = [string]$s.Status; Signer = $s.SignerCertificate.Subject } } | ConvertTo-Json -Compress`,
    120000,
  );
  const arr = rows ? (Array.isArray(rows) ? rows : [rows]) : [];
  const result: NvidiaDll[] = arr.map((r) => ({
    file: path.basename(r.Path).toLowerCase() as NvidiaDll['file'],
    path: r.Path,
    version: r.Version ? r.Version.replace(/,\s*/g, '.') : null,
    // Driver-store copies are catalog-signed through WHQL rather than by NVIDIA directly.
    signed: r.Status === 'Valid' && /NVIDIA|Microsoft Windows Hardware Compatibility Publisher/i.test(r.Signer ?? ''),
    signer: r.Signer,
  }));
  log(`nvidia: ${result.length} NGX files, ${result.filter((r) => r.signed).length} signed`);
  return result;
}

function versionKey(v: string | null): number[] {
  return (v ?? '0').split('.').map((n) => Number(n) || 0);
}

/**
 * Picks the newest NVIDIA-signed copy of a runtime. With allowUnsigned it falls back to
 * an unsigned copy (community-modified builds for older GPUs) when no signed one exists.
 */
export function bestDll(dlls: NvidiaDll[], file: NvidiaDll['file'], allowUnsigned = false): NvidiaDll | null {
  const pool = dlls.filter((d) => d.file === file);
  const signed = pool.some((d) => d.signed) || !allowUnsigned ? pool.filter((d) => d.signed) : pool;
  signed.sort((a, b) => {
    const va = versionKey(a.version);
    const vb = versionKey(b.version);
    for (let i = 0; i < Math.max(va.length, vb.length); i++) if ((va[i] ?? 0) !== (vb[i] ?? 0)) return (vb[i] ?? 0) - (va[i] ?? 0);
    return 0;
  });
  return signed[0] ?? null;
}

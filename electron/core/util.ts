import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import * as fs from 'node:fs';
import * as path from 'node:path';

const logLines: string[] = [];

export function log(msg: string): void {
  const line = `${new Date().toISOString()} ${msg}`;
  logLines.push(line);
  if (logLines.length > 2000) logLines.shift();
  if (process.env.PRISM_DEV) console.log(line);
}

export function getLog(): string[] {
  return [...logLines];
}

export function run(file: string, args: string[], timeoutMs = 20000): Promise<{ code: number; stdout: string; stderr: string }> {
  return new Promise((resolve) => {
    execFile(file, args, { windowsHide: true, timeout: timeoutMs, maxBuffer: 32 * 1024 * 1024, encoding: 'utf8' }, (err, stdout, stderr) => {
      const code = err && typeof (err as NodeJS.ErrnoException & { code?: unknown }).code === 'number' ? Number((err as { code: number }).code) : err ? 1 : 0;
      resolve({ code, stdout: stdout ?? '', stderr: stderr ?? '' });
    });
  });
}

/** Runs a PowerShell snippet and parses its JSON output. The snippet must end with ConvertTo-Json. */
export async function psJson<T>(script: string, timeoutMs = 30000): Promise<T | null> {
  const prelude = '[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;$ErrorActionPreference="SilentlyContinue";';
  const r = await run('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', prelude + script], timeoutMs);
  const text = r.stdout.trim();
  if (!text) return null;
  try {
    return JSON.parse(text) as T;
  } catch {
    log(`psJson parse failed: ${text.slice(0, 200)}`);
    return null;
  }
}

/** Quotes a value for a single-quoted PowerShell string. */
export function psQuote(s: string): string {
  return `'${s.replace(/'/g, "''")}'`;
}

/** Reads a registry value with reg.exe. Returns null when missing. */
export async function regValue(key: string, name: string): Promise<string | null> {
  const r = await run('reg.exe', ['query', key, '/v', name]);
  if (r.code !== 0) return null;
  const m = r.stdout.match(new RegExp(`\\s${name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\s+REG_\\w+\\s+(.*)`, 'i'));
  return m ? m[1].trim() : null;
}

/** Lists subkeys of a registry key. */
export async function regSubkeys(key: string): Promise<string[]> {
  const r = await run('reg.exe', ['query', key]);
  if (r.code !== 0) return [];
  return r.stdout
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l.toUpperCase().startsWith(key.toUpperCase() + '\\'));
}

export function exists(p: string): boolean {
  try {
    fs.accessSync(p);
    return true;
  } catch {
    return false;
  }
}

export function isDir(p: string): boolean {
  try {
    return fs.statSync(p).isDirectory();
  } catch {
    return false;
  }
}

export function readText(p: string): string | null {
  try {
    return fs.readFileSync(p, 'utf8');
  } catch {
    return null;
  }
}

export function sha256File(p: string): string {
  const h = createHash('sha256');
  const fd = fs.openSync(p, 'r');
  try {
    const buf = Buffer.allocUnsafe(1 << 20);
    let n: number;
    while ((n = fs.readSync(fd, buf, 0, buf.length, null)) > 0) h.update(buf.subarray(0, n));
  } finally {
    fs.closeSync(fd);
  }
  return h.digest('hex');
}

export function sha256Text(s: string): string {
  return createHash('sha256').update(s).digest('hex');
}

export interface WalkOptions {
  maxDepth: number;
  maxEntries?: number;
  skipDirs?: RegExp;
}

/** Breadth-first walk that never follows symlinks and stops at maxEntries. */
export function walk(root: string, visit: (file: string, depth: number, isDirectory: boolean) => void, opts: WalkOptions): void {
  const queue: { dir: string; depth: number }[] = [{ dir: root, depth: 0 }];
  let seen = 0;
  const limit = opts.maxEntries ?? 20000;
  while (queue.length) {
    const { dir, depth } = queue.shift()!;
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const e of entries) {
      if (++seen > limit) return;
      const full = path.join(dir, e.name);
      if (e.isSymbolicLink()) continue;
      if (e.isDirectory()) {
        visit(full, depth, true);
        if (depth < opts.maxDepth && !(opts.skipDirs && opts.skipDirs.test(e.name))) queue.push({ dir: full, depth: depth + 1 });
      } else if (e.isFile()) {
        visit(full, depth, false);
      }
    }
  }
}

export function stableId(...parts: string[]): string {
  return sha256Text(parts.join('|').toLowerCase()).slice(0, 12);
}

export function ensureDir(p: string): void {
  fs.mkdirSync(p, { recursive: true });
}

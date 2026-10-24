/**
 * Grade codes: a custom look (grade.ts) as one line of text to copy, paste and share, carrying the
 * game it was made for, so the community can tell which game a code belongs to and an import can
 * find that game in the library on its own.
 *
 *   PRISM1.<base64url of JSON>.<checksum>
 *   JSON: { n: game name, e: executable file name, s: Steam app id, g: the Grade= line }
 *
 * The checksum (FNV-1a of the payload, 6 hex digits) catches a code cut short in copying.
 */
import { gradeFormat, gradeParse, type GradeValues } from './grade';
import type { GameAnalysis, GameEntry } from './types';

export interface GradeCodeGame {
  name: string;
  exe?: string;
  steamAppId?: string;
}
export interface GradeCode {
  game: GradeCodeGame;
  grade: GradeValues;
}

const PREFIX = 'PRISM1.';

function fnv(s: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return (h >>> 8).toString(16).padStart(6, '0');
}
function toB64url(text: string): string {
  const bytes = new TextEncoder().encode(text);
  let bin = '';
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
function fromB64url(s: string): string {
  const bin = atob(s.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - (s.length % 4)) % 4));
  return new TextDecoder().decode(Uint8Array.from(bin, (c) => c.charCodeAt(0)));
}

export function encodeGradeCode(game: GradeCodeGame, grade: GradeValues): string {
  const body: Record<string, string> = { n: game.name.slice(0, 120), g: gradeFormat(grade) };
  if (game.exe) body.e = game.exe.slice(0, 120);
  if (game.steamAppId && /^\d{1,10}$/.test(game.steamAppId)) body.s = game.steamAppId;
  const payload = toB64url(JSON.stringify(body));
  return `${PREFIX}${payload}.${fnv(payload)}`;
}

/** The code's game and look, or an error a person can act on. */
export function decodeGradeCode(raw: string): GradeCode {
  const code = raw.trim().replace(/\s+/g, '');
  if (!code.startsWith(PREFIX)) throw new Error('这不是 Prism 的调色码（调色码以 PRISM1. 开头）。');
  const [payload, sum] = code.slice(PREFIX.length).split('.');
  if (!payload || !sum || fnv(payload) !== sum) throw new Error('调色码不完整，可能复制时少了一部分，请重新复制。');
  let body: Record<string, unknown>;
  try {
    body = JSON.parse(fromB64url(payload));
  } catch {
    throw new Error('调色码已损坏，请重新复制。');
  }
  if (typeof body.n !== 'string' || typeof body.g !== 'string') throw new Error('调色码已损坏，请重新复制。');
  return {
    game: { name: body.n, exe: typeof body.e === 'string' ? body.e : undefined, steamAppId: typeof body.s === 'string' ? body.s : undefined },
    grade: gradeParse(body.g),
  };
}

const normName = (s: string) => s.toLowerCase().replace(/[^a-z0-9一-龥]/g, '');
const exeBase = (p: string | null | undefined) => (p ? p.replace(/^.*[\\/]/, '').toLowerCase() : '');

/**
 * The game in this library a code was made for: by Steam app id, then by executable name, then by
 * name. Null when the library has no such game.
 */
export function matchGame(game: GradeCodeGame, games: GameEntry[], analyses: Record<string, GameAnalysis>): GameEntry | null {
  if (game.steamAppId) {
    const hit = games.find((g) => g.steamAppId === game.steamAppId);
    if (hit) return hit;
  }
  if (game.exe) {
    const want = game.exe.toLowerCase();
    const hit = games.find((g) => exeBase(analyses[g.id]?.exe) === want);
    if (hit) return hit;
  }
  const want = normName(game.name);
  return (want && games.find((g) => normName(g.name) === want)) || null;
}

/** The code's game facts for a game in the library. */
export function gameOf(entry: GameEntry, analysis?: GameAnalysis | null): GradeCodeGame {
  return { name: entry.name, exe: exeBase(analysis?.exe) || undefined, steamAppId: entry.steamAppId };
}

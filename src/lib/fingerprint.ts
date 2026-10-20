import type { GameAnalysis, GameEntry, Route } from '../../electron/shared/types';

export interface Bar {
  /** Width in units (1 unit = 1 hairline). */
  w: number;
  on: boolean;
  /** Optional spectral state colour for this bar. */
  tone?: Tone;
}

export type Tone = 'ready' | 'caution' | 'blocked' | 'info';

export const TONE_COLOR: Record<Tone, string> = {
  ready: '#3DDC97',
  caution: '#FFC400',
  blocked: '#FF3B30',
  info: '#4DA3FF',
};

export function routeTone(route: Route | undefined): Tone {
  switch (route) {
    case 'native-hook':
    case 'optiscaler':
      return 'ready';
    case 'feeder':
      return 'caution';
    case 'blocked':
      return 'blocked';
    default:
      return 'info';
  }
}

function hash(s: string): number {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}

function bitsOf(n: number, count: number): boolean[] {
  return Array.from({ length: count }, (_, i) => Boolean((n >> (count - 1 - i)) & 1));
}

/**
 * Encodes a game's analysis as a barcode. Every segment is data, read left to right:
 * guard | API set (6) | bitness (2) | DLSS version digits | route (3, toned) | anti-cheat | name hash (16) | guard.
 */
export function fingerprint(game: GameEntry, a: GameAnalysis | undefined): Bar[] {
  const bars: Bar[] = [];
  const guard = () => bars.push({ w: 1, on: true }, { w: 1, on: false }, { w: 1, on: true });
  const push = (bits: boolean[], tone?: Tone) => bits.forEach((b) => bars.push({ w: b ? 2 : 1, on: b, tone: b ? tone : undefined }, { w: 1, on: false }));

  guard();
  bars.push({ w: 2, on: false });
  const apiOrder = ['dx9', 'dx10', 'dx11', 'dx12', 'vulkan', 'opengl'];
  push(apiOrder.map((x) => Boolean(a?.apis.includes(x as never))));
  push(a ? [a.bitness === 'x64', a.bitness === 'x86'] : [false, false]);
  const digits = (a?.nativeDlss.version ?? '').split('.').slice(0, 3).map((n) => Number(n) || 0);
  for (const d of digits) push(bitsOf(d % 16, 4));
  bars.push({ w: 2, on: false });
  const route = a?.recommendedRoute;
  const tone = routeTone(route);
  const routeCode = { 'native-hook': 0b101, optiscaler: 0b110, feeder: 0b011, blocked: 0b111, unknown: 0b001 }[route ?? 'unknown'];
  push(bitsOf(routeCode, 3), tone);
  push([Boolean(a?.antiCheat.length)], a?.antiCheat.length ? 'blocked' : undefined);
  bars.push({ w: 2, on: false });
  push(bitsOf(hash(game.name) & 0xffff, 16));
  guard();
  return bars;
}

export function totalUnits(bars: Bar[]): number {
  return bars.reduce((s, b) => s + b.w, 0);
}

import { createContext, useContext, useEffect, useRef } from 'react';

export type MotionLevel = 'max' | 'standard' | 'off';

export const MotionContext = createContext<MotionLevel>('max');
export const useMotion = () => useContext(MotionContext);

/** requestAnimationFrame loop that pauses while the window is hidden. */
export function useFrame(cb: (t: number, dt: number) => void, active: boolean): void {
  const ref = useRef(cb);
  ref.current = cb;
  useEffect(() => {
    if (!active) return;
    let raf = 0;
    let last = performance.now();
    const loop = (t: number) => {
      const dt = Math.min(64, Math.max(0, t - last));
      last = t;
      raf = requestAnimationFrame(loop);
      if (!document.hidden) {
        try {
          ref.current(t, dt);
        } catch (e) {
          console.error(e);
        }
      }
    };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, [active]);
}

/** Seeded PRNG (mulberry32) so fields are reproducible from data. */
export function rng(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Sizes a canvas to its CSS box at device pixel ratio and returns the 2D context. */
export function fitCanvas(c: HTMLCanvasElement): { ctx: CanvasRenderingContext2D; w: number; h: number; dpr: number } {
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const rect = c.getBoundingClientRect();
  const w = Math.max(1, Math.round(rect.width));
  const h = Math.max(1, Math.round(rect.height));
  if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
    c.width = Math.round(w * dpr);
    c.height = Math.round(h * dpr);
  }
  const ctx = c.getContext('2d')!;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w, h, dpr };
}

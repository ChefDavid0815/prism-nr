import { useEffect, useRef } from 'react';
import { fitCanvas, rng } from '../lib/motion';

/**
 * Full-window apply transition: barcode columns sweep across the window, then the
 * whole window snaps to its negative for one beat. One flash per apply, far below
 * the 3-flashes-per-second photosensitivity limit.
 */
export function Sweep({ run, onDone }: { run: number; onDone: () => void }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const done = useRef(onDone);
  done.current = onDone;

  useEffect(() => {
    if (!run) return;
    const c = canvas.current!;
    const r = rng(run);
    const widths = Array.from({ length: 400 }, () => (r() > 0.6 ? 1 + Math.floor(r() * 4) : 0));
    const start = performance.now();
    const dur = 620;
    let raf = 0;
    const frame = (t: number) => {
      const p = Math.min(1, (t - start) / dur);
      const { ctx, w, h } = fitCanvas(c);
      ctx.clearRect(0, 0, w, h);
      // Exponential ease-out head with a trailing tail.
      const head = 1 - Math.pow(1 - p, 3);
      const tail = Math.max(0, head - 0.35);
      let x = 0;
      for (let i = 0; x < w; i++) {
        const bw = widths[i % widths.length];
        const fx = x / w;
        if (bw && fx < head && fx > tail) {
          const a = 1 - (head - fx) / 0.35;
          ctx.fillStyle = `rgba(255,255,255,${0.15 + a * 0.85})`;
          ctx.fillRect(x, 0, bw, h);
        }
        x += bw + 2;
      }
      ctx.fillStyle = '#fff';
      ctx.fillRect(w * head, 0, 2, h);
      if (p < 1) raf = requestAnimationFrame(frame);
      else {
        ctx.clearRect(0, 0, w, h);
        document.documentElement.classList.add('negative');
        setTimeout(() => {
          document.documentElement.classList.remove('negative');
          done.current();
        }, 110);
      }
    };
    raf = requestAnimationFrame(frame);
    return () => cancelAnimationFrame(raf);
  }, [run]);

  return <canvas ref={canvas} className="sweep" aria-hidden="true" />;
}

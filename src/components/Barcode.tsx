import { useEffect, useRef } from 'react';
import { type Bar, TONE_COLOR, totalUnits } from '../lib/fingerprint';
import { fitCanvas, useFrame, useMotion } from '../lib/motion';

interface Props {
  bars: Bar[];
  inverted?: boolean;
  /** Animate bars scanning in once on mount and shimmering on hover. */
  live?: boolean;
  hover?: boolean;
  label?: string;
}

/** A game's data fingerprint drawn as a barcode. */
export function Barcode({ bars, inverted = false, live = true, hover = false, label }: Props) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const motion = useMotion();
  const born = useRef(performance.now());
  const settled = useRef(false);

  useEffect(() => {
    born.current = performance.now();
    settled.current = false;
  }, [bars]);

  const draw = (t: number) => {
    const c = canvas.current;
    if (!c) return;
    const { ctx, w, h } = fitCanvas(c);
    ctx.clearRect(0, 0, w, h);
    const units = totalUnits(bars);
    const unit = w / units;
    const reveal = motion === 'off' || !live ? 1 : Math.min(1, (t - born.current) / 520);
    const ink = inverted ? '#000' : '#fff';
    let x = 0;
    let idx = 0;
    for (const b of bars) {
      const bw = b.w * unit;
      if (b.on && x / w <= reveal) {
        const shimmer = hover && motion === 'max' ? 0.55 + 0.45 * Math.abs(Math.sin(t * 0.012 + idx * 0.7)) : 1;
        ctx.globalAlpha = shimmer;
        ctx.fillStyle = b.tone ? TONE_COLOR[b.tone] : ink;
        const bh = b.tone ? h : h - 4;
        ctx.fillRect(Math.round(x), b.tone ? 0 : 4, Math.max(1, Math.round(bw)), bh);
      }
      x += bw;
      idx++;
    }
    ctx.globalAlpha = 1;
    // Scan head while revealing.
    if (reveal < 1) {
      ctx.fillStyle = ink;
      ctx.fillRect(Math.round(w * reveal), 0, 1, h);
    } else settled.current = true;
  };

  // Only animate while revealing or hovered; otherwise one static paint.
  useFrame(draw, live && motion !== 'off' && (hover || !settled.current));
  // Static repaint (e.g. when the row inverts) once the reveal has finished.
  useEffect(() => {
    if (!live || motion === 'off' || settled.current) {
      const id = requestAnimationFrame((t) => draw(t + 1e6));
      return () => cancelAnimationFrame(id);
    }
  });

  return <canvas ref={canvas} className="barcode" role="img" aria-label={label ?? '数据指纹'} />;
}

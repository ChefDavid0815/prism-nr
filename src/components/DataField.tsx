import { useEffect, useRef } from 'react';
import { type Bar, TONE_COLOR, totalUnits } from '../lib/fingerprint';
import { fitCanvas, rng, useFrame, useMotion } from '../lib/motion';

interface Props {
  /** The selected game's fingerprint; the field repeats it as its column pattern. */
  bars: Bar[];
  /** Neural rendering intensity, 0..1; drives the sine lattice amplitude. */
  intensity: number;
  /** Strobe columns while scanning or installing. */
  busy: boolean;
  /** 0..1 progress for installs; draws the filled region. */
  progress?: number | null;
  seed: number;
  labels: { left: string[]; right: string[] };
}

/**
 * The signature data field: a wall of hairline columns drifting left, built from the
 * selected game's barcode, with a sine lattice whose amplitude is the neural intensity.
 * The cursor brightens the columns under it.
 */
export function DataField({ bars, intensity, busy, progress, seed, labels }: Props) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const motion = useMotion();
  const state = useRef({ offset: 0, amp: intensity, mouseX: -1, phase: 0, flash: 0, columns: [] as { on: boolean; tone?: string; noise: number }[] });

  // Rebuild the column pattern when the fingerprint changes.
  useEffect(() => {
    const r = rng(seed);
    const cols: { on: boolean; tone?: string; noise: number }[] = [];
    const pattern = bars.length ? bars : [{ w: 1, on: true }];
    for (let rep = 0; rep < 8; rep++) {
      for (const b of pattern) {
        for (let i = 0; i < b.w; i++) cols.push({ on: b.on, tone: b.tone ? TONE_COLOR[b.tone] : undefined, noise: r() });
      }
      for (let i = 0; i < 3 + Math.floor(r() * 6); i++) cols.push({ on: r() > 0.55, noise: r() });
    }
    state.current.columns = cols;
    state.current.flash = 1;
  }, [bars, seed]);

  const draw = (t: number, dt: number) => {
    const c = canvas.current;
    if (!c) return;
    const { ctx, w, h } = fitCanvas(c);
    const s = state.current;
    const speed = motion === 'max' ? 0.035 : motion === 'standard' ? 0.012 : 0;
    s.offset += dt * speed * (busy ? 6 : 1);
    s.amp += (intensity - s.amp) * Math.min(1, dt / 220);
    s.phase += dt * (motion === 'off' ? 0 : 0.0016);
    s.flash = Math.max(0, s.flash - dt / 380);

    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, w, h);

    const cols = s.columns;
    if (!cols.length) return;
    const unit = 2;
    const n = Math.ceil(w / unit) + 2;
    const shift = Math.floor(s.offset);
    const top = 18;
    const bottom = h - 18;
    for (let i = 0; i < n; i++) {
      const col = cols[(i + shift) % cols.length];
      const x = i * unit - (s.offset % 1) * unit;
      let on = col.on;
      // Strobe: while busy, columns flip with a per-column frequency.
      if (busy && motion !== 'off') on = Math.sin(t * 0.02 * (0.5 + col.noise) + col.noise * 40) > 0.1;
      const near = s.mouseX >= 0 && motion === 'max' ? Math.max(0, 1 - Math.abs(x - s.mouseX) / 90) : 0;
      if (!on && near < 0.6) continue;
      const alpha = on ? 0.55 + col.noise * 0.45 : near * 0.5;
      const lift = near * 10 + (busy ? col.noise * 8 : 0);
      ctx.fillStyle = col.tone ?? `rgba(255,255,255,${Math.min(1, alpha + near * 0.4 + s.flash * 0.3)})`;
      const len = (bottom - top) * (0.35 + 0.65 * Math.abs(Math.sin(i * 0.013 + col.noise * 3 + s.phase * 0.2)));
      ctx.fillRect(x, bottom - len - lift, 1, len + lift);
    }

    // Install progress: the filled part of the field turns solid white hairlines.
    if (progress != null) {
      const px = w * progress;
      ctx.fillStyle = 'rgba(255,255,255,0.12)';
      ctx.fillRect(0, top, px, bottom - top);
      ctx.fillStyle = '#fff';
      ctx.fillRect(px, top - 6, 1, bottom - top + 12);
    }

    // Sine lattice: amplitude is the neural rendering intensity.
    const mid = (top + bottom) / 2;
    const amp = (bottom - top) * 0.42 * s.amp;
    ctx.strokeStyle = '#fff';
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let x = 0; x <= w; x += 3) {
      const y = mid + Math.sin(x * 0.0085 + s.phase) * amp + Math.sin(x * 0.031 - s.phase * 1.7) * amp * 0.12;
      if (x === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
    // Nodes on the curve.
    ctx.fillStyle = '#fff';
    for (const fx of [0.18, 0.46, 0.74]) {
      const x = w * fx;
      const y = mid + Math.sin(x * 0.0085 + s.phase) * amp + Math.sin(x * 0.031 - s.phase * 1.7) * amp * 0.12;
      ctx.fillRect(x - 2, y - 2, 4, 4);
    }
    // Frame rules.
    ctx.fillStyle = '#262626';
    ctx.fillRect(0, top - 1, w, 1);
    ctx.fillRect(0, bottom, w, 1);
  };

  useFrame(draw, true);

  return (
    <div className="field" onMouseMove={(e) => (state.current.mouseX = e.clientX - e.currentTarget.getBoundingClientRect().left)} onMouseLeave={() => (state.current.mouseX = -1)}>
      <canvas ref={canvas} className="field-canvas" aria-hidden="true" />
      <div className="field-col field-col-left">
        {labels.left.map((l, i) => (
          <span key={i}>{l}</span>
        ))}
      </div>
      <div className="field-col field-col-right">
        {labels.right.map((l, i) => (
          <span key={i}>{l}</span>
        ))}
      </div>
    </div>
  );
}

export function barsUnits(bars: Bar[]): number {
  return totalUnits(bars);
}

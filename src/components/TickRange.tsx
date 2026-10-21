import { useId } from 'react';
import { RollingNumber } from './RollingNumber';

interface Props {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  unit?: string;
  help: string;
  disabled?: boolean;
  onChange: (v: number) => void;
}

/**
 * A range input drawn as a run of hairline ticks: one tick per step, white up to the value.
 * The native input stays on top (transparent track, a white bar for the thumb), so keyboard,
 * pointer and screen readers work as with any slider.
 */
export function TickRange({ label, value, min, max, step, unit = '', help, disabled, onChange }: Props) {
  const id = useId();
  const count = Math.round((max - min) / step);
  const on = Math.round((value - min) / step);
  return (
    <div className={`tick-range ${disabled ? 'is-disabled' : ''}`}>
      <div className="tick-range-head">
        <label htmlFor={id}>{label}</label>
        <RollingNumber className="mono" value={`${value}${unit}`} />
      </div>
      <div className="tick-range-track">
        <span className="tick-range-ticks" aria-hidden="true">
          {Array.from({ length: count + 1 }, (_, i) => (
            <i key={i} className={i <= on ? 'is-on' : ''} />
          ))}
        </span>
        <input
          id={id}
          type="range"
          min={min}
          max={max}
          step={step}
          value={value}
          disabled={disabled}
          aria-describedby={`${id}-help`}
          aria-valuetext={`${value}${unit}`}
          onChange={(e) => onChange(Number(e.target.value))}
        />
      </div>
      <p id={`${id}-help`} className="block-note">
        {help}
      </p>
    </div>
  );
}

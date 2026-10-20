import { useMotion } from '../lib/motion';

/** Digits roll into place like a counter; non-digits render as-is. */
export function RollingNumber({ value, className }: { value: string | number; className?: string }) {
  const motion = useMotion();
  const text = String(value);
  if (motion === 'off') return <span className={className}>{text}</span>;
  return (
    <span className={`roll ${className ?? ''}`} aria-label={text}>
      {text.split('').map((ch, i) =>
        /\d/.test(ch) ? (
          <span className="roll-digit" key={i} aria-hidden="true">
            <span className="roll-strip" style={{ transform: `translateY(${-Number(ch) * 10}%)`, transitionDelay: `${i * 40}ms` }}>
              {'0123456789'.split('').map((d) => (
                <span key={d}>{d}</span>
              ))}
            </span>
          </span>
        ) : (
          <span key={i} aria-hidden="true">
            {ch}
          </span>
        ),
      )}
    </span>
  );
}

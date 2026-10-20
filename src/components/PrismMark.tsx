/** Wordmark glyph: one white ray enters a hairline prism and leaves as four spectral rays. */
export function PrismMark({ size = 22 }: { size?: number }) {
  return (
    <svg width={size * 1.6} height={size} viewBox="0 0 35 22" aria-hidden="true" className="prism-mark">
      <line x1="0" y1="13" x2="12" y2="11" stroke="#fff" strokeWidth="1" />
      <path d="M17.5 1.5 L27 20.5 H8 Z" fill="none" stroke="#fff" strokeWidth="1.2" strokeLinejoin="miter" />
      <line x1="21" y1="10.5" x2="35" y2="6" stroke="#FF3B30" strokeWidth="1" />
      <line x1="21.5" y1="11.5" x2="35" y2="9.5" stroke="#FFC400" strokeWidth="1" />
      <line x1="22" y1="12.5" x2="35" y2="13" stroke="#3DDC97" strokeWidth="1" />
      <line x1="22.5" y1="13.5" x2="35" y2="16.5" stroke="#4DA3FF" strokeWidth="1" />
    </svg>
  );
}

// Generates the Prism NR poster and logo as SVG from a seeded sequence.
// Usage: node scripts/poster.cjs
const fs = require('node:fs');
const path = require('node:path');

function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const W = 1600;
const H = 900;
const r = rng(5090);
const MONO = "'JetBrains Mono', 'Cascadia Mono', Consolas, 'Microsoft YaHei', monospace";
const SPECTRAL = ['#ff3b30', '#ffc400', '#3ddc97', '#4da3ff'];
const out = [];

out.push(`<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">`);
out.push(`<title>Prism NR 0.1.0 — light in, spectrum out.</title>`);
out.push(`<rect width="${W}" height="${H}" fill="#000"/>`);

// Barcode field: hairline columns of varying height, a few spectral bars.
const top = 150;
const bottom = 640;
let x = 0;
let i = 0;
const bars = [];
while (x < W) {
  const w = r() > 0.82 ? 2 + Math.floor(r() * 3) : 1;
  if (r() > 0.42) {
    const len = (bottom - top) * (0.3 + 0.7 * Math.abs(Math.sin(i * 0.021 + r() * 0.6)));
    const spectral = r() > 0.965 ? SPECTRAL[Math.floor(r() * 4)] : null;
    const op = spectral ? 1 : (0.45 + r() * 0.55).toFixed(2);
    bars.push(`<rect x="${x}" y="${(bottom - len).toFixed(1)}" width="${spectral ? 3 : w}" height="${len.toFixed(1)}" fill="${spectral ?? '#fff'}"${spectral ? '' : ` fill-opacity="${op}"`}/>`);
  }
  x += w + 1 + (r() > 0.7 ? Math.floor(r() * 4) : 0);
  i++;
}
out.push(`<g>${bars.join('')}</g>`);
out.push(`<rect x="0" y="${top - 1}" width="${W}" height="1" fill="#262626"/><rect x="0" y="${bottom}" width="${W}" height="1" fill="#262626"/>`);

// Sine lattice.
const mid = (top + bottom) / 2;
const amp = (bottom - top) * 0.3;
const pts = [];
for (let px = 0; px <= W; px += 4) pts.push(`${px},${(mid + Math.sin(px * 0.0062 + 0.8) * amp + Math.sin(px * 0.023) * amp * 0.1).toFixed(1)}`);
out.push(`<polyline points="${pts.join(' ')}" fill="none" stroke="#fff" stroke-width="1.5"/>`);
for (const fx of [0.18, 0.46, 0.74]) {
  const px = W * fx;
  const py = mid + Math.sin(px * 0.0062 + 0.8) * amp + Math.sin(px * 0.023) * amp * 0.1;
  out.push(`<rect x="${(px - 4).toFixed(1)}" y="${(py - 4).toFixed(1)}" width="8" height="8" fill="#fff"/>`);
}

// Readout columns on black backings.
const left = ['API   DX12', 'BIT   x64', 'DLSS  310.7.130', 'ROUTE NATIVE'];
const right = ['NR    32%', 'MODEL 100%', 'GUARD 0.60', 'PLUG  6'];
out.push(`<rect x="40" y="176" width="232" height="104" fill="#000" fill-opacity="0.85"/><rect x="${W - 272}" y="176" width="232" height="104" fill="#000" fill-opacity="0.85"/>`);
left.forEach((t, k) => out.push(`<text x="56" y="${204 + k * 22}" font-family="${MONO}" font-size="15" fill="#fff" xml:space="preserve">${t}</text>`));
right.forEach((t, k) => out.push(`<text x="${W - 56}" y="${204 + k * 22}" font-family="${MONO}" font-size="15" fill="#fff" text-anchor="end" xml:space="preserve">${t}</text>`));

// Header strip.
out.push(`<text x="40" y="62" font-family="${MONO}" font-size="15" fill="#8c8c8c">GPU</text><text x="40" y="88" font-family="${MONO}" font-size="19" fill="#fff">RTX 50 · NEURAL RENDERING</text>`);
out.push(`<text x="${W - 40}" y="62" font-family="${MONO}" font-size="15" fill="#8c8c8c" text-anchor="end">VERSION</text><text x="${W - 40}" y="88" font-family="${MONO}" font-size="19" fill="#fff" text-anchor="end">0.1.0</text>`);

// Prism mark + wordmark.
const mx = 40;
const my = 700;
out.push(`<g transform="translate(${mx},${my}) scale(3.4)"><line x1="0" y1="13" x2="12" y2="11" stroke="#fff" stroke-width="0.6"/><path d="M17.5 1.5 L27 20.5 H8 Z" fill="none" stroke="#fff" stroke-width="0.7"/><line x1="21" y1="10.5" x2="35" y2="6" stroke="#ff3b30" stroke-width="0.6"/><line x1="21.5" y1="11.5" x2="35" y2="9.5" stroke="#ffc400" stroke-width="0.6"/><line x1="22" y1="12.5" x2="35" y2="13" stroke="#3ddc97" stroke-width="0.6"/><line x1="22.5" y1="13.5" x2="35" y2="16.5" stroke="#4da3ff" stroke-width="0.6"/></g>`);
out.push(`<text x="176" y="772" font-family="${MONO}" font-size="84" font-weight="700" fill="#fff" letter-spacing="-1">Prism NR</text>`);
out.push(`<text x="178" y="822" font-family="${MONO}" font-size="24" fill="#fff">light in, spectrum out. / 一束光进去，一道光谱出来。</text>`);
out.push(`<text x="178" y="856" font-family="${MONO}" font-size="16" fill="#8c8c8c">DLSS 5 neural rendering manager · one-click install · guarded · reversible</text>`);
out.push('</svg>');

const dir = path.join(__dirname, '..', 'docs', 'brand');
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(dir, 'poster.svg'), out.join('\n'));

const logo = `<svg xmlns="http://www.w3.org/2000/svg" width="140" height="88" viewBox="-2 -2 39 26"><title>Prism NR</title><rect x="-2" y="-2" width="39" height="26" fill="#000"/><line x1="0" y1="13" x2="12" y2="11" stroke="#fff" stroke-width="0.8"/><path d="M17.5 1.5 L27 20.5 H8 Z" fill="none" stroke="#fff" stroke-width="1"/><line x1="21" y1="10.5" x2="35" y2="6" stroke="#ff3b30" stroke-width="0.8"/><line x1="21.5" y1="11.5" x2="35" y2="9.5" stroke="#ffc400" stroke-width="0.8"/><line x1="22" y1="12.5" x2="35" y2="13" stroke="#3ddc97" stroke-width="0.8"/><line x1="22.5" y1="13.5" x2="35" y2="16.5" stroke="#4da3ff" stroke-width="0.8"/></svg>`;
fs.writeFileSync(path.join(dir, 'logo.svg'), logo);
console.log('poster.svg', fs.statSync(path.join(dir, 'poster.svg')).size, 'bytes');

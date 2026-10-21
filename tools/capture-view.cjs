// Turns a Prism Core F11 capture (prism-capture/*.raw + *.txt) into PNGs and statistics.
//   node tools/capture-view.cjs "<game>/prism-capture" [x y w h]
// orig / final are the game's HDR frame (R11G11B10F), shown with one shared exposure; proxy /
// neural are the network's display-referred input and output (RGBA16F).
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

const dir = process.argv[2];
const crop = process.argv.slice(3, 7).map(Number);

function half(h) {
  const s = h & 0x8000 ? -1 : 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
  if (e === 0) return s * m * 2 ** -24;
  if (e === 31) return m ? NaN : s * Infinity;
  return s * (1 + m / 1024) * 2 ** (e - 15);
}
// R11G11B10_FLOAT: 6-bit exponent, 6/6/5-bit mantissas, no sign.
function small(bits, mbits) {
  const e = bits >> mbits, m = bits & ((1 << mbits) - 1);
  if (e === 0) return m * 2 ** (-14 - mbits);
  if (e === 31) return m ? NaN : Infinity;
  return (1 + m / (1 << mbits)) * 2 ** (e - 15);
}
function load(name) {
  const [w, h, pitch, fmt] = fs.readFileSync(path.join(dir, name + '.txt'), 'utf8').trim().split(/\s+/).map(Number);
  const raw = fs.readFileSync(path.join(dir, name + '.raw'));
  const px = new Float32Array(w * h * 3);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const o = (y * w + x) * 3;
    if (fmt === 26) {
      const v = raw.readUInt32LE(y * pitch + x * 4);
      px[o] = small(v & 0x7ff, 6); px[o + 1] = small((v >> 11) & 0x7ff, 6); px[o + 2] = small(v >>> 22, 5);
    } else {
      const b = y * pitch + x * 8;
      px[o] = half(raw.readUInt16LE(b)); px[o + 1] = half(raw.readUInt16LE(b + 2)); px[o + 2] = half(raw.readUInt16LE(b + 4));
    }
  }
  return { w, h, px };
}
const srgb = (c) => (c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055);
const dec = (c) => (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));
function png(file, w, h, rgb) {
  const rows = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { rows[y * (w * 3 + 1)] = 0; rgb.copy(rows, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3); }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(rows)), chunk('IEND', Buffer.alloc(0))]));
}

const orig = load('orig'), fin = load('final'), proxy = load('proxy'), neural = load('neural');
const { w, h } = orig;
const [cx, cy, cw, ch] = crop.length === 4 ? crop : [0, 0, w, h];
// One exposure for orig and final, from orig's mean log luminance.
let sum = 0;
for (let i = 0; i < w * h; i++) sum += Math.log2(Math.max(0.2126 * orig.px[i * 3] + 0.7152 * orig.px[i * 3 + 1] + 0.0722 * orig.px[i * 3 + 2], 1e-5));
const k = 0.18 / 2 ** (sum / (w * h));
const view = (img, hdr) => {
  const out = Buffer.alloc(cw * ch * 3);
  for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) for (let c = 0; c < 3; c++) {
    let v = img.px[((cy + y) * w + cx + x) * 3 + c];
    v = hdr ? srgb((v * k) / (1 + v * k)) : v;
    out[(y * cw + x) * 3 + c] = Math.max(0, Math.min(255, Math.round(v * 255)));
  }
  return out;
};
for (const [name, img, hdr] of [['orig', orig, true], ['final', fin, true], ['proxy', proxy, false], ['neural', neural, false]]) png(path.join(dir, name + '.png'), cw, ch, view(img, hdr));
// How much the network changed, and how much reached the final frame.
let e = 0, d = 0, n = 0;
for (let i = 0; i < w * h; i++) for (let c = 0; c < 3; c++) {
  e += Math.abs(Math.log2(Math.max(dec(Math.max(0, neural.px[i * 3 + c])), 1 / 512) / Math.max(dec(Math.max(0, proxy.px[i * 3 + c])), 1 / 512)));
  d += Math.abs(Math.log2(Math.max(fin.px[i * 3 + c], 1e-5) / Math.max(orig.px[i * 3 + c], 1e-5)));
  n++;
}
console.log(JSON.stringify({ size: `${w}x${h}`, exposure: +k.toFixed(4), networkEditStops: +(e / n).toFixed(4), finalVsOrigStops: +(d / n).toFixed(4) }));

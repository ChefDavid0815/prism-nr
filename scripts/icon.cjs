// Renders the Prism mark into build/icon.png (512 × 512) for the Windows installer.
// Usage: npx electron scripts/icon.cjs
const { app, BrowserWindow } = require('electron');
const fs = require('node:fs');
const path = require('node:path');

const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">
<rect width="512" height="512" fill="#000"/>
<g transform="translate(44,120) scale(12)">
<line x1="0" y1="13" x2="12" y2="11" stroke="#fff" stroke-width="0.9"/>
<path d="M17.5 1.5 L27 20.5 H8 Z" fill="none" stroke="#fff" stroke-width="1.1"/>
<line x1="21" y1="10.5" x2="35" y2="6" stroke="#ff3b30" stroke-width="0.9"/>
<line x1="21.5" y1="11.5" x2="35" y2="9.5" stroke="#ffc400" stroke-width="0.9"/>
<line x1="22" y1="12.5" x2="35" y2="13" stroke="#3ddc97" stroke-width="0.9"/>
<line x1="22.5" y1="13.5" x2="35" y2="16.5" stroke="#4da3ff" stroke-width="0.9"/>
</g></svg>`;

app.whenReady().then(async () => {
  const win = new BrowserWindow({ width: 512, height: 512, show: false, useContentSize: true, webPreferences: { offscreen: true, zoomFactor: 1 } });
  const html = `<html><body style="margin:0;background:#000">${svg}</body></html>`;
  await win.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
  await new Promise((r) => setTimeout(r, 600));
  const img = (await win.webContents.capturePage({ x: 0, y: 0, width: 512, height: 512 })).resize({ width: 512, height: 512 });
  const out = path.join(__dirname, '..', 'build');
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, 'icon.png'), img.toPNG());
  console.log('icon', img.getSize());
  app.quit();
});

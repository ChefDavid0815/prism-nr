// Renders the built UI (preview data, no backend) and saves PNG captures.
// Usage: npx vite build && npx electron scripts/capture.cjs <outDir>
const { app, BrowserWindow } = require('electron');
const fs = require('node:fs');
const path = require('node:path');

const outDir = path.resolve(process.argv[2] || 'docs/screenshots');
const shots = [
  { name: 'library', width: 1440, height: 900, steps: [] },
  { name: 'library-1120', width: 1120, height: 720, steps: [] },
  { name: 'plugins', width: 1440, height: 900, steps: ["[...document.querySelectorAll('.rail-item')][1].click()", "document.querySelectorAll('.prow-main')[8]?.click()"] },
  { name: 'history', width: 1440, height: 900, steps: ["document.querySelector('.btn-primary').click()", 'WAIT:4200', "[...document.querySelectorAll('.rail-item')][2].click()"] },
];

// Keep running between captures; closing the last window would otherwise quit.
app.on('window-all-closed', () => {});

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

app.whenReady().then(async () => {
  fs.mkdirSync(outDir, { recursive: true });
  for (const s of shots) {
    const win = new BrowserWindow({ width: s.width, height: s.height, show: false, useContentSize: true, backgroundColor: '#000000', webPreferences: { offscreen: true } });
    await win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
    await sleep(3500);
    for (const step of s.steps) {
      if (step.startsWith('WAIT:')) await sleep(Number(step.slice(5)));
      else {
        await win.webContents.executeJavaScript(step).catch(() => {});
        await sleep(900);
      }
    }
    await sleep(1200);
    const img = await win.webContents.capturePage({ x: 0, y: 0, width: s.width, height: s.height });
    fs.writeFileSync(path.join(outDir, `${s.name}.png`), img.toPNG());
    console.log('saved', s.name, img.getSize());
    win.destroy();
  }
  app.quit();
});

// CPU reference for NETS (neural edit temporal supersampling), run against a real
// DLSS 5 edit taken from our CONTROL recordings. Usage:
//   npx electron tools/nets-sim/run.cjs [outDir]
const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('node:path');

const out = path.resolve(process.argv[2] || 'nets-sim-out');
// Optional sweep file: a JSON array of { key, label, s, nets: false | {options} }.
const sweepFile = process.argv[3] && !process.argv[3].startsWith('--') ? process.argv[3] : undefined;
const sweep = sweepFile ? JSON.parse(require('node:fs').readFileSync(sweepFile, 'utf8')) : undefined;
// Optional "--dump key:frame": write one accumulation step for the GPU harness.
const dumpArg = process.argv.find((a) => a.startsWith('--dump='));
const dump = dumpArg ? { key: dumpArg.slice(7).split(':')[0], t: Number(dumpArg.split(':')[1]) } : undefined;

app.on('window-all-closed', () => {});
ipcMain.on('log', (_e, msg) => console.log(msg));

app.whenReady().then(async () => {
  const win = new BrowserWindow({
    show: false,
    webPreferences: { nodeIntegration: true, contextIsolation: false, webSecurity: false, backgroundThrottling: false },
  });
  await win.loadFile(path.join(__dirname, 'sim.html'));
  const fn = process.argv.includes('--guard') ? 'runGuard'
    : process.argv.includes('--export') ? 'runExport'
    : process.argv.includes('--eval') ? 'runEval'
    : process.argv.includes('--tile-export') ? 'runTileExport'
    : process.argv.includes('--tile-eval') ? 'runTileEval'
    : process.argv.includes('--rot-export') ? 'runRotExport'
    : process.argv.includes('--rot-eval') ? 'runRotEval' : 'runSim';
  const result = await win.webContents
    .executeJavaScript(`window.${fn}(${JSON.stringify({ out, sweep, dump })}).catch((e) => ({ error: String((e && e.stack) || e) }))`);
  console.log('DONE', JSON.stringify(result, null, 2));
  app.quit();
});

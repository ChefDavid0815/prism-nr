// Drives prism-core.ini during the 1.1.2 film recording at the top setting (network every frame, 2
// passes, intensity 170%): split view, an intensity sweep, full frame, on/off, then steady off/on
// stretches for the frame rates. Every change is logged with its performance-counter time so the
// film can line the frame log up with the recording.
//   node scripts/film/timeline-112.cjs <game dir> <log file>
const fs = require('fs');
const path = require('path');
const [dir, logFile] = process.argv.slice(2);
const ini = path.join(dir, 'prism-core.ini');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const qpcMs = () => Number(process.hrtime.bigint()) / 1e6;  // QueryPerformanceCounter on Windows
const out = [];
const write = (phase, v) => {
  const body = ['[Prism Core]', `Enabled=${v.on ?? 1}`, 'Cadence=1', 'Async=0', 'Passes=2', `Intensity=${(v.i ?? 1.7).toFixed(2)}`, `Guard=${(v.guard ?? 1).toFixed(2)}`,
    `Split=${v.split ? 1 : 0}`, 'Label=film112', 'StatsSeconds=2', 'FrameLog=1', ''].join('\r\n');
  fs.writeFileSync(ini, body, 'ascii');
  const line = `${qpcMs().toFixed(1)}\t${phase}\t${JSON.stringify(v)}`;
  out.push(line); console.log(line);
  fs.writeFileSync(logFile, out.join('\n') + '\n');
};
(async () => {
  write('split', { split: 1 });
  await sleep(15000);
  for (const i of [0, 0, 0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.7, 1.7]) { write('sweep', { split: 1, i }); await sleep(1500); }
  write('full', {});
  await sleep(12000);
  for (const on of [0, 1, 0, 1]) { write('toggle', { on }); await sleep(3000); }
  write('steady-off', { on: 0 });
  await sleep(12000);
  write('steady-on', {});
  await sleep(12000);
  write('split-end', { split: 1 });
  await sleep(8000);
  console.log('done');
})();

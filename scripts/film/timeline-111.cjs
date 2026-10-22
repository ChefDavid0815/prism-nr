// Drives prism-core.ini during the 1.1.1 film recording: split view, a live intensity sweep, on/off.
// Core re-reads the file within a second, so every phase holds at least that long.
const fs = require('fs');
const ini = 'E:/steam/steamapps/common/CONTROL Resonant/prism-core.ini';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const t0 = Date.now();
const write = (v) => {
  const body = ['[Prism Core]', `Enabled=${v.on ?? 1}`, 'Cadence=2', `Intensity=${(v.i ?? 1).toFixed(2)}`, `Split=${v.split ? 1 : 0}`, 'Label=film', ''].join('\r\n');
  fs.writeFileSync(ini, body, 'ascii');
  console.log(((Date.now() - t0) / 1000).toFixed(1), JSON.stringify(v));
};
(async () => {
  write({ split: 0 });
  await sleep(14000);               // loading and the first shots
  write({ split: 1 });              // split view: original left, Prism right
  await sleep(14000);
  for (const i of [0.8, 0.6, 0.4, 0.2, 0, 0, 0.25, 0.5, 0.75, 1, 1]) { write({ split: 1, i }); await sleep(1300); }
  await sleep(4000);
  write({ split: 0 });              // full frame with Prism
  await sleep(8000);
  for (const on of [0, 1, 0, 1]) { write({ on }); await sleep(3000); }
  write({});
  console.log('done');
})();

// A/B runner for a running game with Prism Core: writes each configuration to prism-core.ini,
// waits for Core's next statistics window and collects it. Core re-reads the file within a second
// and restarts its window after a 3 s warm-up, so every row is measured on settled settings.
//   node tests/ab_run.cjs <game dir> <plan.json> [out.json]
// plan.json: { "seconds": 12, "base": { "Intensity": "1.00" }, "runs": [ { "Label": "off", "Enabled": 0 }, ... ] }
const fs = require('fs');
const path = require('path');

const [dir, planFile, outFile] = process.argv.slice(2);
const plan = JSON.parse(fs.readFileSync(planFile, 'utf8'));
const ini = path.join(dir, 'prism-core.ini');
const log = path.join(dir, 'prism-core.log');
const seconds = plan.seconds ?? 12;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const defaults = { Enabled: 1, Cadence: 4, Async: 1, Land: 'wait', NetPriority: 'normal', FpsCap: 0, Intensity: '1.00', Guard: '1.00' };
function write(run) {
  const v = { ...defaults, ...(plan.base ?? {}), ...run, StatsSeconds: seconds };
  const body = ['[Prism Core]', ...Object.entries(v).map(([k, x]) => `${k}=${x}`), ''].join('\r\n');
  fs.writeFileSync(ini, body, 'ascii');
}

const re = /stats \[([^\]]*)\] present ([\d.]+) fps p99 ([\d.]+) ms sd ([\d.]+) ms \((\d+)\) \| render ([\d.]+) fps p99 ([\d.]+) ms sd ([\d.]+) ms \((\d+)\) \| network (\d+) launches, (\d+) lands, age ([\d.]+) avg (\d+) max/;
function statsLines() {
  return fs.readFileSync(log, 'utf8').split(/\r?\n/).filter((l) => re.test(l));
}

(async () => {
  const results = [];
  for (const run of plan.runs) {
    const before = statsLines().length;
    write(run);
    const deadline = Date.now() + (seconds + 12) * 1000;
    let row = null;
    while (Date.now() < deadline && !row) {
      await sleep(1000);
      const lines = statsLines().slice(before).filter((l) => l.includes(`[${run.Label}]`));
      if (lines.length) {
        const m = lines[0].match(re);
        row = {
          label: m[1], presentFps: +m[2], presentP99: +m[3], presentSd: +m[4], presents: +m[5],
          renderFps: +m[6], renderP99: +m[7], renderSd: +m[8], renders: +m[9],
          launches: +m[10], lands: +m[11], ageAvg: +m[12], ageMax: +m[13],
        };
      }
    }
    if (!row) row = { label: run.Label, error: 'no stats window' };
    results.push({ ...row, config: run });
    const r = row;
    console.log(r.error ? `${run.Label}: ${r.error}` : `${r.label.padEnd(16)} present ${r.presentFps.toFixed(1)} fps p99 ${r.presentP99.toFixed(2)} sd ${r.presentSd.toFixed(2)} | render ${r.renderFps.toFixed(1)} fps p99 ${r.renderP99.toFixed(2)} sd ${r.renderSd.toFixed(2)} | net ${r.launches}/${r.lands} age ${r.ageAvg}/${r.ageMax}`);
  }
  if (outFile) fs.writeFileSync(outFile, JSON.stringify(results, null, 2));
})();

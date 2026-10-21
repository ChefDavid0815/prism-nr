/* Runs the core against this PC without Electron: system, scan, analysis, plan. Read-only. */
import * as os from 'node:os';
import * as path from 'node:path';
import { setShaderDir } from '../core/installer';
import { parseVdf } from '../core/scanner';
import { PrismService } from '../core/service';
import { initStore } from '../core/store';
import { setIniValue } from '../core/installer';
import { nrCostFor, planNr } from '../shared/nrplan';

function assert(cond: boolean, msg: string): void {
  if (!cond) {
    console.error('FAIL', msg);
    process.exitCode = 1;
  } else console.log('ok  ', msg);
}

async function main(): Promise<void> {
  // Unit checks.
  const vdf = parseVdf('"libraryfolders" { "0" { "path" "D:\\\\Steam" "apps" { "10" "1" } } }');
  assert(((vdf.libraryfolders as Record<string, Record<string, string>>)['0'].path) === 'D:\\Steam', 'VDF parser reads nested paths');
  const ini = setIniValue('[A]\r\nx=1\r\n[DlssNr]\r\nEnabled=false\r\n', 'DlssNr', 'Enabled', 'true');
  assert(/Enabled=true/.test(ini) && !/Enabled=false/.test(ini), 'INI writer replaces an existing key');
  assert(/\[Plugins\]\r\nLoadReshade=true/.test(setIniValue('[A]\r\nx=1', 'Plugins', 'LoadReshade', 'true')), 'INI writer adds a missing section');

  const c5060 = nrCostFor('NVIDIA GeForce RTX 5060', false);
  assert(c5060?.source === 'published' && Math.abs(c5060.msPerMP - 2.27) < 1e-9 && Math.abs(c5060.fixedMs - 0.07) < 1e-9, 'NR cost: RTX 5060 uses the published figure');
  assert(nrCostFor('NVIDIA GeForce RTX 5070 Ti Laptop GPU', true)?.source === 'estimated', 'NR cost: laptop parts without a measurement are estimated');
  assert(nrCostFor('NVIDIA GeForce RTX 5090 Laptop GPU', true)?.source === 'measured', 'NR cost: RTX 5090 Laptop uses the bench measurement');
  assert(nrCostFor('NVIDIA GeForce RTX 4090', false) === null, 'NR cost: no figure without official support');
  const p60 = planNr({ cost: c5060!, outputWidth: 2560, outputHeight: 1440, targetFps: 60, baseFrameMs: 12 });
  assert(p60.fits && p60.scale === 0.67 && p60.estimatedMs <= p60.budgetMs, `NR plan: RTX 5060 1440p60 -> ${p60.label} ${p60.estimatedMs} ms of ${p60.budgetMs}`);
  const p144 = planNr({ cost: c5060!, outputWidth: 2560, outputHeight: 1440, targetFps: 144, frameGen: 2, baseFrameMs: 10 });
  assert(p144.fits && p144.scale === 0.5, `NR plan: RTX 5060 1440p144 with 2x frame generation -> ${p144.label} ${p144.estimatedMs} ms of ${p144.budgetMs}`);
  const p5090 = planNr({ cost: nrCostFor('RTX 5090', false)!, outputWidth: 2560, outputHeight: 1440, targetFps: 60, baseFrameMs: 8 });
  assert(p5090.scale === 1, `NR plan: RTX 5090 keeps native (${p5090.label})`);
  const tight = planNr({ cost: c5060!, outputWidth: 3840, outputHeight: 2160, targetFps: 120, baseFrameMs: 8 });
  assert(!tight.fits && tight.scale === 0.35, 'NR plan: an impossible budget reports it does not fit');

  initStore(path.join(os.tmpdir(), 'prism-nr-selftest'));
  setShaderDir(path.join(__dirname, '..', '..', 'plugins', 'prism-shaders', 'Shaders'));
  const svc = new PrismService();
  const sys = await svc.getSystem();
  console.log('GPU:', sys.gpus.map((g) => `${g.name} tier=${g.tier} nr=${g.nrSupport} driver=${g.driver}`).join(' | '));
  console.log('OS build', sys.os.build, 'notes', sys.notes);
  const games = await svc.scanGames();
  console.log(`games: ${games.length}`);
  for (const g of games.slice(0, 40)) {
    const a = await svc.analyzeGame(g.id, true);
    const plan = await svc.plan({ gameId: g.id });
    console.log(
      `- [${g.store}] ${g.name} | exe=${a.exe ? path.basename(a.exe) : '-'} ${a.bitness} ${a.apis.join('/') || '?'} dlss=${a.nativeDlss.present ? a.nativeDlss.version ?? 'y' : 'n'} ac=${a.antiCheat.map((x) => x.name).join(',') || '-'} route=${plan.route} preset=${plan.preset} style=${plan.style} blockers=${plan.blockers.length}`,
    );
  }
  const dlls = await svc.nvidiaDlls();
  console.log('NGX files:', dlls.map((d) => `${d.file} ${d.version} signed=${d.signed} @ ${d.path}`));
}

void main();

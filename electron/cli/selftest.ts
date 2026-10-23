/* Runs the core against this PC without Electron: system, scan, analysis, plan. Read-only. */
import * as os from 'node:os';
import * as path from 'node:path';
import { coreIni, setCoreDir, setShaderDir } from '../core/installer';
import { parseVdf } from '../core/scanner';
import { PrismService } from '../core/service';
import { initStore } from '../core/store';
import { setIniValue } from '../core/installer';
import { PRESETS, resolveValues, tierLocked, tierOf } from '../shared/catalog';
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

  const ini0 = coreIni({});
  assert(/\[Prism Core\]\r\nEnabled=1\r\nCadence=2\r\nAsync=1\r\nIntensity=2\.00\r\nGuard=1\.00\r\nPasses=2/.test(ini0), 'Core INI: defaults are the high tier (every 2 frames, async, 200%, 2 passes)');
  const ini1 = coreIni({ 'prism-core.cadence': '2', 'prism-core.async': false, 'prism-core.intensity': 60, 'prism-core.guard': 150 });
  assert(/Cadence=2/.test(ini1) && /Async=0/.test(ini1) && /Intensity=0\.60/.test(ini1) && /Guard=1\.00/.test(ini1), 'Core INI: values are written and clamped');
  const ini2 = coreIni({ 'prism-core.intensity': 400, 'prism-core.passes': '9' });
  assert(/Intensity=3\.00/.test(ini2) && /Passes=5/.test(ini2), 'Core INI: intensity goes up to 300%, passes up to 5');
  const extreme = coreIni(resolveValues('extreme'));
  assert(/Cadence=1/.test(extreme) && /Async=0/.test(extreme) && /Intensity=2\.10/.test(extreme) && /Passes=2/.test(extreme), 'Core INI: the extreme tier is every frame, 2 passes, 210%');
  const flagship = coreIni(resolveValues('flagship'));
  assert(/Cadence=1/.test(flagship) && /Async=0/.test(flagship) && /Passes=2/.test(flagship) && /Intensity=2\.40/.test(flagship) && /NetScale=0\.75/.test(flagship), 'Core INI: the flagship tier is every frame, 2 passes, 75%, 240%');
  assert(/NetScale=1\.00/.test(extreme), 'Core INI: the extreme tier runs the network at native resolution');
  const ext = PRESETS.find((p) => p.id === 'extreme')!;
  assert(tierLocked(ext, 'NVIDIA GeForce RTX 5090 Laptop GPU') === null && tierLocked(ext, 'NVIDIA GeForce RTX 5080') !== null, 'Tiers: extreme is open on RTX 5090 only');
  assert(tierOf(resolveValues('flagship'))?.id === 'flagship' && tierOf(resolveValues('extreme'))?.id === 'extreme', 'Tiers: flagship and extreme are told apart by the network resolution');
  assert(tierOf(resolveValues('high'))?.id === 'high' && tierOf({ ...resolveValues('high'), 'prism-core.intensity': 205 }) === undefined, 'Tiers: values name their tier, a tweak makes them custom');
  assert(/^[\x00-\x7f]*$/.test(ini0), 'Core INI: ASCII only (GetPrivateProfileString reads it as ANSI)');
  assert(/KeyOverlay=Home/.test(ini0), 'Core INI: the in-game panel opens with Home');
  assert(/Look=0/.test(ini0) && /Look=3/.test(coreIni({ 'prism-core.look': '3' })) && /Look=4/.test(coreIni({ 'prism-core.look': '9' })), 'Core INI: the colour look is written (0-4)');
  assert(tierOf({ ...resolveValues('flagship'), 'prism-core.look': '2' })?.id === 'flagship', 'Tiers: the colour look does not change the tier');
  const kept = coreIni(resolveValues('high'), ['KeyOverlay=Insert', 'OverlayX=900', 'OverlayHud=1', 'SplitPos=0.620']);
  assert(/KeyOverlay=Insert/.test(kept) && !/KeyOverlay=Home/.test(kept) && /OverlayX=900/.test(kept) && /OverlayHud=1/.test(kept) && /SplitPos=0\.620/.test(kept),
    'Core INI: a rewrite keeps the in-game panel\'s own keys');

  const c5060 = nrCostFor('NVIDIA GeForce RTX 5060', false);
  assert(c5060?.source === 'published' && Math.abs(c5060.msPerMP - 4.54 * 0.56) < 1e-9 && Math.abs(c5060.fixedMs - 0.14 * 0.56) < 1e-9, 'NR cost: RTX 5060 uses the published figure');
  assert(nrCostFor('NVIDIA GeForce RTX 5070 Ti Laptop GPU', true)?.source === 'estimated', 'NR cost: laptop parts without a measurement are estimated');
  assert(nrCostFor('NVIDIA GeForce RTX 5090 Laptop GPU', true)?.source === 'measured', 'NR cost: RTX 5090 Laptop uses the bench measurement');
  assert(nrCostFor('NVIDIA GeForce RTX 4090', false) === null, 'NR cost: no figure without official support');
  const p60 = planNr({ cost: c5060!, outputWidth: 2560, outputHeight: 1440, targetFps: 60, baseFrameMs: 12 });
  assert(p60.fits && p60.scale < 1 && p60.estimatedMs <= p60.budgetMs, `NR plan: RTX 5060 1440p60 -> ${p60.label} ${p60.estimatedMs} ms of ${p60.budgetMs}`);
  const p144 = planNr({ cost: c5060!, outputWidth: 2560, outputHeight: 1440, targetFps: 144, frameGen: 2, baseFrameMs: 10 });
  assert(p144.fits && p144.scale === 0.5, `NR plan: RTX 5060 1440p144 with 2x frame generation -> ${p144.label} ${p144.estimatedMs} ms of ${p144.budgetMs}`);
  const p5090 = planNr({ cost: nrCostFor('RTX 5090', false)!, outputWidth: 2560, outputHeight: 1440, targetFps: 60, baseFrameMs: 8 });
  assert(p5090.scale === 1, `NR plan: RTX 5090 keeps native (${p5090.label})`);
  const tight = planNr({ cost: c5060!, outputWidth: 3840, outputHeight: 2160, targetFps: 120, baseFrameMs: 8 });
  assert(!tight.fits && tight.scale === 0.35, 'NR plan: an impossible budget reports it does not fit');

  initStore(path.join(os.tmpdir(), 'prism-nr-selftest'));
  setShaderDir(path.join(__dirname, '..', '..', 'plugins', 'prism-shaders', 'Shaders'));
  setCoreDir(path.join(__dirname, '..', '..', 'native', 'prism-core', 'build'));
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
      `- [${g.store}] ${g.name} | v=${a.importsVersionDll ?? '?'} exe=${a.exe ? path.basename(a.exe) : '-'} ${a.bitness} ${a.apis.join('/') || '?'} dlss=${a.nativeDlss.present ? a.nativeDlss.version ?? 'y' : 'n'} ac=${a.antiCheat.map((x) => x.name).join(',') || '-'} route=${plan.route} preset=${plan.preset} style=${plan.style} blockers=${plan.blockers.length}`,
    );
    if (plan.route === 'prism-core') {
      console.log('    plugins', plan.plugins.join(','), '| needs', plan.needs.length, '| blockers', plan.blockers.join(' / ') || '-');
      console.log('    warnings', plan.warnings.join(' / ') || '-');
    }
  }
  const dlls = await svc.nvidiaDlls();
  console.log('NGX files:', dlls.map((d) => `${d.file} ${d.version} signed=${d.signed} @ ${d.path}`));
}

void main();

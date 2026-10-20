/* Runs the core against this PC without Electron: system, scan, analysis, plan. Read-only. */
import * as os from 'node:os';
import * as path from 'node:path';
import { setShaderDir } from '../core/installer';
import { parseVdf } from '../core/scanner';
import { PrismService } from '../core/service';
import { initStore } from '../core/store';
import { setIniValue } from '../core/installer';

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

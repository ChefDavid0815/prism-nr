/*
 * Runs the same one-click install the app's button runs, from the command line.
 * Uses the installed app's data folder so the record shows up in Prism NR.
 * Usage: node dist-electron/cli/install.js "<game name>" [preset] [--plan-only]
 */
import * as os from 'node:os';
import * as path from 'node:path';
import { setShaderDir } from '../core/installer';
import { PrismService } from '../core/service';
import { initStore } from '../core/store';
import type { PresetId, Route } from '../shared/types';

async function main(): Promise<void> {
  const [name, presetArg] = process.argv.slice(2).filter((a) => !a.startsWith('--'));
  const planOnly = process.argv.includes('--plan-only');
  initStore(path.join(process.env.APPDATA ?? path.join(os.homedir(), 'AppData', 'Roaming'), 'Prism NR'));
  setShaderDir(path.join(__dirname, '..', '..', 'plugins', 'prism-shaders', 'Shaders'));
  const svc = new PrismService();
  const games = await svc.scanGames();
  const game = games.find((g) => g.name.toLowerCase().includes((name ?? '').toLowerCase()));
  if (!game) throw new Error(`没有找到游戏：${name}。已扫描：${games.map((g) => g.name).join('、')}`);
  const styleArg = process.argv.find((a) => a.startsWith('--style='))?.slice(8);
  const routeArg = process.argv.find((a) => a.startsWith('--route='))?.slice(8) as Route | undefined;
  const plan = await svc.plan({ gameId: game.id, preset: presetArg as PresetId | undefined, style: styleArg, route: routeArg });
  console.log(JSON.stringify({ game: game.name, route: plan.route, preset: plan.preset, style: plan.style, plugins: plan.plugins, needs: plan.needs, steps: plan.steps, manualSteps: plan.manualSteps, blockers: plan.blockers, warnings: plan.warnings }, null, 2));
  if (planOnly || plan.blockers.length) return;
  const record = await svc.install(plan, (p) => console.log(`[${p.stage}] ${Math.round(p.fraction * 100)}% ${p.message}`));
  console.log(`installed ${record.files.length} files, backups ${record.files.filter((f) => f.backup).length}, record ${record.id}`);
  for (const f of record.files) console.log('  ' + f.target + (f.backup ? '  (backed up)' : ''));
}

main().catch((e) => {
  console.error('FAILED', (e as Error).message);
  process.exitCode = 1;
});

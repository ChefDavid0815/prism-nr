/* Runs the core against this PC without Electron: system, scan, analysis, plan. Read-only. */
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { coreIni, launchOptionFor, panelLines, pickLoader, setCoreDir, setShaderDir, upgradeCoreInstalls } from '../core/installer';
import { denuvoSections, fileMentions } from '../core/pe';
import { store } from '../core/store';
import type { InstallRecord } from '../shared/types';
import { parseVdf } from '../core/scanner';
import { PrismService } from '../core/service';
import { initStore } from '../core/store';
import { setIniValue } from '../core/installer';
import { netScaleFor, netSizeFor, PRESETS, resolveValues, tierLocked, tierOf } from '../shared/catalog';
import { nrCostFor, planNr } from '../shared/nrplan';
import { GRADE, gradeFormat, gradeFromPreset, gradeParse, gradeToShader } from '../shared/grade';
import { ACCOUNT_KEY, isRedeemCode, normPhone, normRedeemCode, REDEEM_ALPHABET, showPhone, showRedeemCode, USERNAME_RULE } from '../shared/account';
import { decodeGradeCode, encodeGradeCode, matchGame } from '../shared/gradecode';

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
  assert(/Cadence=1/.test(extreme) && /Async=0/.test(extreme) && /Intensity=1\.00/.test(extreme) && /Passes=2/.test(extreme), 'Core INI: the extreme tier is every frame, 2 passes, 100%');
  const flagship = coreIni(resolveValues('flagship'));
  assert(/Cadence=1/.test(flagship) && /Async=0/.test(flagship) && /Passes=2/.test(flagship) && /Intensity=1\.00/.test(flagship) && /NetScale=0\.6250/.test(flagship), 'Core INI: the flagship tier is every frame, 2 passes, the base size (62.5%), 100%');
  assert(/NetScale=0\.7655/.test(extreme), 'Core INI: the extreme tier runs the network at 150% of the base\'s pixels');
  assert(/NetScale=1\.00/.test(coreIni(resolveValues('high'))), 'Core INI: the asynchronous tiers run the network native');
  assert(netSizeFor(0.75) === '150' && netSizeFor(0.63) === '100' && Math.abs(netScaleFor('150') ** 2 / netScaleFor('100') ** 2 - 1.5) < 0.002, 'Net sizes: 150% is half as many pixels again as 100%');
  const ext = PRESETS.find((p) => p.id === 'extreme')!;
  assert(tierLocked(ext, 'NVIDIA GeForce RTX 5090 Laptop GPU') === null && tierLocked(ext, 'NVIDIA GeForce RTX 5080') === null && tierLocked(ext, 'NVIDIA GeForce RTX 5070 Ti') !== null, 'Tiers: extreme is open on RTX 5080 and 5090 only');
  assert(tierOf(resolveValues('flagship'))?.id === 'flagship' && tierOf(resolveValues('extreme'))?.id === 'extreme', 'Tiers: flagship and extreme are told apart by the network resolution');
  assert(tierOf(resolveValues('high'))?.id === 'high' && tierOf({ ...resolveValues('high'), 'prism-core.intensity': 205 }) === undefined, 'Tiers: values name their tier, a tweak makes them custom');
  assert(/^[\x00-\x7f]*$/.test(ini0), 'Core INI: ASCII only (GetPrivateProfileString reads it as ANSI)');
  assert(/KeyOverlay=Home/.test(ini0), 'Core INI: the in-game panel opens with Home');
  assert(/Look=0/.test(ini0) && /Look=3/.test(coreIni({ 'prism-core.look': '3' })) && /Look=5/.test(coreIni({ 'prism-core.look': '9' })), 'Core INI: the colour look is written (0-5, 5 the custom look)');
  // The custom look: grade.ts against native/prism-core/src/grade.h.
  {
    const h = fs.readFileSync(path.join(__dirname, '..', '..', 'native', 'prism-core', 'src', 'grade.h'), 'utf8');
    const native = [...h.matchAll(/t\[kG\w+\] = \{L"(\w+)", L"[^"]+", (-?[\d.]+), (-?[\d.]+), (-?[\d.]+), ([\d.]+)f?, (kU\w+)/g)].map((m) => ({ key: m[1], min: +m[2], max: +m[3], def: +m[4], step: +m[5], unit: m[6] }));
    const units: Record<string, string> = { kUSigned: 'signed', kUPlain: 'plain', kUStops: 'stops', kUHue: 'hue', kUTurn: 'turn' };
    const fixed = GRADE.filter((g) => !/^(Hue|Sat|Lum)(Red|Yellow|Green|Cyan|Blue|Magenta)$|^Curve[YRGB]\d$/.test(g.key));
    const same = native.length === fixed.length && native.every((n, i) => n.key === fixed[i].key && n.min === fixed[i].min && n.max === fixed[i].max && n.def === fixed[i].def && n.step === fixed[i].step && units[n.unit] === fixed[i].unit);
    assert(same && GRADE.length === 66, `Custom look: the app's table matches grade.h (${native.length} named controls, ${GRADE.length} in all)`);
    // Starting points, as Core's look_preview printed them (single precision on both sides).
    const want = [
      '',
      'Contrast:17,Saturation:38,Vibrance:55,SkinProtect:60,SkinAlign:42,SatRed:-8,SatYellow:8,SatGreen:12,SatCyan:15,SatMagenta:5,Density:35,HighlightTwist:40,Sky:27,HighlightDesat:20,ShadowDesat:13',
      'Exposure:-0.20,Contrast:40,Saturation:-34,SkinProtect:55,SkinAlign:50,ShadowHue:238,ShadowAmount:63,HighlightHue:254,HighlightAmount:36,SatRed:-5,SatYellow:-15,SatGreen:-22,SatMagenta:-15,HighlightTwist:32,HighlightDesat:24,ShadowDesat:42',
      'Contrast:70,Saturation:50,Vibrance:35,SkinProtect:60,SkinAlign:58,ShadowHue:240,ShadowAmount:54,HighlightHue:72,HighlightAmount:63,SatRed:-5,SatYellow:5,SatGreen:8,SatCyan:12,SatBlue:8,SatMagenta:2,Density:55,HighlightTwist:56,Sky:47,HighlightDesat:20',
    ];
    const order = (x: string) => x.split(',').filter(Boolean).sort().join(',');
    assert(want.every((w, i) => order(gradeFormat(gradeFromPreset(i))) === order(w)), 'Custom look: the presets as starting points match Core\'s');
    const g = gradeParse('Contrast:24, saturation:-34,Exposure:9,Bogus:3,ShadowHue:200');
    assert(gradeFormat(g) === 'Exposure:3.00,Contrast:24,Saturation:-34,ShadowHue:200', 'Custom look: the ini line parses (any case, clamped, unknown keys dropped) and formats back');
    const k = gradeToShader(gradeParse(''));
    assert(k[0] === 1 && k[1] === 1 && Math.abs(k[10] - 0.5) < 1e-6 && k[45] === 1 && k[68] === 0, 'Custom look: neutral packs to no change');
    const gi = coreIni({ 'prism-core.look': '5', 'prism-core.grade': 'Contrast:24', 'prism-core.grade3': 'Sat:9,Saturation:-10', 'prism-core.gradeSlot': '2' });
    assert(/Present=0/.test(ini0) && /Present=1/.test(coreIni({ 'prism-core.present': true })), 'Core INI: present mode (games without DLSS) is written');
    assert(/Bridge=0/.test(ini0) && /Bridge=1/.test(coreIni({ 'prism-core.bridge': true })) && /\r\nTarget=hl2\.exe\r\n/.test(coreIni({ 'prism-core.target': 'hl2.exe' })) && !/Target=/.test(ini0) && !/Target=/.test(coreIni({ 'prism-core.target': '游戏.exe' })),
      'Core INI: the frame bridge and the target executable are written; a non-ASCII name means no filter');
    assert(/Look=5/.test(gi) && /\r\nGrade=Contrast:24\r\n/.test(gi) && /\r\nGrade2=\r\n/.test(gi) && /\r\nGrade3=Saturation:-10\r\n/.test(gi) && /GradeSlot=2/.test(gi) && /^[\x00-\x7f]*$/.test(gi), 'Core INI: the custom look\'s three slots and the slot in use');
  }
  assert(tierOf({ ...resolveValues('flagship'), 'prism-core.look': '2' })?.id === 'flagship', 'Tiers: the colour look does not change the tier');
  const kept = coreIni(resolveValues('high'), ['KeyOverlay=Insert', 'OverlayX=900', 'OverlayHud=1', 'SplitPos=0.620']);
  assert(/KeyOverlay=Insert/.test(kept) && !/KeyOverlay=Home/.test(kept) && /OverlayX=900/.test(kept) && /OverlayHud=1/.test(kept) && /SplitPos=0\.620/.test(kept),
    'Core INI: a rewrite keeps the in-game panel\'s own keys');
  const panelIni = path.join(os.tmpdir(), 'prism-nr-selftest-panel.ini');
  fs.writeFileSync(panelIni, 'Intensity=2.40\r\nNetScale=1.00\r\nNetScaleAuto=1\r\nFpsTarget=90\r\nOverlayHud=1\r\n');
  const panel = panelLines(panelIni);
  fs.rmSync(panelIni, { force: true });
  assert(panel.join(',') === 'NetScaleAuto=1,FpsTarget=90,OverlayHud=1', 'Core INI: the panel\'s target frame rate survives a rewrite, the tier\'s own keys do not');

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
  // Denuvo games load Core late: told by the EXE's section names.
  assert(denuvoSections({ bitness: 'x64', imports: [], isGui: true, sections: ['.text', '.xtext', '.rdata', '.xtls'] }).join(',') === '.xtext,.xtls' &&
    denuvoSections({ bitness: 'x64', imports: [], isGui: true, sections: ['.text', '.rdata', '.detourc', '.xbld'] }).length === 0, 'Denuvo: found by its section names, and only by them');
  assert(launchOptionFor('E:\\Games\\Crimson Desert\\bin64') === '"E:\\Games\\Crimson Desert\\bin64\\PrismLaunch.exe" %command%', 'Late load: the Steam launch option quotes the launcher path');
  {
    const f = path.join(os.tmpdir(), 'prism-nr-selftest-mentions.bin');
    fs.writeFileSync(f, Buffer.concat([Buffer.alloc(9 * 1024 * 1024 - 7, 1), Buffer.from('D3D12.DLL', 'utf16le'), Buffer.alloc(64, 2)]));
    const g = path.join(os.tmpdir(), 'prism-nr-selftest-mentions2.bin');
    fs.writeFileSync(g, Buffer.concat([Buffer.from('MZ d3d11.dll '), Buffer.alloc(1024, 3)]));
    assert(fileMentions(f, ['d3d12.dll', 'D3D12CreateDevice']) && !fileMentions(g, ['d3d12.dll', 'D3D12CreateDevice']),
      'DX12 first: a renderer loaded at run time is found by name in the binary (UTF-16, any case, across a chunk boundary)');
  }
  assert(normPhone('138 0013 8000') === '+8613800138000' && normPhone('+1 (415) 555-0123') === '+14155550123' && normPhone('0044 20 7946 0958') === '+442079460958' &&
    normPhone('12345') === null && normPhone('23800138000') === null && showPhone('+8613800138000') === '+86 138 0013 8000' && showPhone('+14155550123') === '+14155550123',
    'Accounts: phone numbers from any country to E.164 (11-digit mainland numbers get +86), shown readably');
  assert(USERNAME_RULE.test('晨风') && USERNAME_RULE.test('pixel_hunter') && !USERNAME_RULE.test('a') && !USERNAME_RULE.test('has space') && !USERNAME_RULE.test('x'.repeat(21)),
    'Accounts: usernames of 2-20 Chinese characters, letters, digits and underscores');
  assert(normRedeemCode('prism-abcd-efgh-jkmn-pqro') === 'ABCDEFGHJKMNPQR0' && normRedeemCode(' a1b2 c3d4 e5f6 g7hl ') === 'A1B2C3D4E5F6G7H1' && isRedeemCode('ABCDEFGHJKMNPQR0') &&
    !isRedeemCode('ABCDEFGHJKMNPQRU') && !isRedeemCode('ABCD') && showRedeemCode('ABCDEFGHJKMNPQR0') === 'ABCD-EFGH-JKMN-PQR0' && REDEEM_ALPHABET.length === 32 && !/[ILOU]/.test(REDEEM_ALPHABET),
    'Plugin packs: redeem codes read leniently (any case and separators, O as 0, I and L as 1, an optional PRISM-), shown in fours');
  assert(/^sb_publishable_/.test(ACCOUNT_KEY), 'Accounts: the app carries only the publishable key, never a service key');
  {
    const g = gradeParse('Contrast:24,Saturation:-34,ShadowHue:200,ShadowAmount:60,CurveB1:25,Grain:15');
    const code = encodeGradeCode({ name: '赛博朋克 2077', exe: 'Cyberpunk2077.exe', steamAppId: '1091500' }, g);
    const back = decodeGradeCode('  ' + code.slice(0, 20) + '\n' + code.slice(20) + ' ');
    assert(/^PRISM1\./.test(code) && back.game.name === '赛博朋克 2077' && back.game.steamAppId === '1091500' && gradeFormat(back.grade) === gradeFormat(g),
      'Grade codes: a look and its game survive the round trip (spaces and line breaks from pasting ignored)');
    let cut = '';
    try { decodeGradeCode(code.slice(0, -10) + code.slice(-9)); } catch (e) { cut = (e as Error).message; }
    let wrong = '';
    try { decodeGradeCode('hello'); } catch (e) { wrong = (e as Error).message; }
    assert(/不完整/.test(cut) && /不是 Prism 的调色码/.test(wrong), 'Grade codes: a code cut short or not a code at all is refused with a reason');
    const lib = [{ id: 'a', name: 'CONTROL Resonant', store: 'steam' as const, installDir: 'x', evidence: '' }, { id: 'b', name: 'Cyberpunk 2077', store: 'gog' as const, installDir: 'y', evidence: '' }];
    const an = { b: { exe: 'D:\\GOG\\Cyberpunk 2077\\bin\\x64\\Cyberpunk2077.exe' } } as never;
    assert(matchGame(back.game, lib, an)?.id === 'b' && matchGame({ name: 'control  resonant' }, lib, {})?.id === 'a' && matchGame({ name: 'Other' }, lib, {}) === null,
      'Grade codes: the game is found in the library by Steam id, executable or name (GOG copy of a Steam code matched by its executable)');
  }
  const pl = (i: string[] | undefined, o = {}) => pickLoader(i, o).name;
  assert(pl(['dxgi', 'version', 'd3d12']) === 'version.dll' && pl(['d3d12', 'winmm']) === 'winmm.dll' && pl(['d3d12']) === 'd3d12.dll' && pl([]) === 'dxgi.dll' && pl(undefined) === 'dxgi.dll',
    'Loaders: version.dll first, then what the EXE imports, dxgi.dll when it imports none of them');
  const ts = (names: string[]) => (n: string) => names.includes(n);
  assert(pl(['dxgi', 'winmm'], { taken: ts(['winmm.dll']) }) === 'dxgi.dll' && pickLoader(['dxgi'], { taken: ts(['dxgi.dll']) }).chain && pickLoader(['dxgi'], { taken: ts(['dxgi.dll']) }).name === 'dxgi.dll' && !pickLoader(['dxgi', 'd3d11'], { taken: ts(['dxgi.dll']) }).chain,
    'Loaders: a name another mod holds (ReShade, OptiScaler) is passed over; only when all are taken is it chained');
  assert(pl(['d3d9'], { x86: true }) === 'd3d9.dll' && pl(['d3d12'], { x86: true }) === 'dxgi.dll' && pl([], { apis: ['dx9'] }) === 'd3d9.dll' && pl(['version'], { x86: true }) === 'version.dll',
    'Loaders: 32-bit games have their own list (no d3d12); a DX9 game with no known import loads through d3d9.dll');
  // 1.2 installs keep working in 1.3: the old network resolution key moves to the new sizes.
  {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'prism-nr-upgrade-'));
    const rec = (id: string, preset: string, values: Record<string, number | boolean | string>) => {
      fs.mkdirSync(path.join(dir, id));
      const target = path.join(dir, id, 'prism-core.ini');
      fs.writeFileSync(target, coreIni(values));
      return { id, gameId: id, gameName: id, installDir: dir, route: 'prism-core', preset, plugins: ['nr-prism-core'], values, files: [{ target, sha256After: '' }], installedAt: '', status: 'active' } as unknown as InstallRecord;
    };
    const before = store.installs();
    store.saveInstalls([
      rec('old-extreme', 'extreme', { 'prism-core.cadence': '1', 'prism-core.async': false, 'prism-core.passes': '2', 'prism-core.intensity': 210, 'prism-core.guard': 100, 'prism-core.netScale': '100' }),
      rec('old-flagship', 'flagship', { 'prism-core.cadence': '1', 'prism-core.async': false, 'prism-core.passes': '2', 'prism-core.intensity': 240, 'prism-core.guard': 100, 'prism-core.netScale': '75' }),
      rec('old-custom', 'flagship', { 'prism-core.cadence': '1', 'prism-core.async': false, 'prism-core.passes': '3', 'prism-core.intensity': 180, 'prism-core.guard': 50, 'prism-core.netScale': '100' }),
      // From before 1.2: no resolution key at all; Core ran native then.
      rec('pre12-extreme', 'high', { 'prism-core.cadence': '1', 'prism-core.async': false, 'prism-core.passes': '2', 'prism-core.intensity': 210, 'prism-core.guard': 100 }),
      rec('pre12-every', 'high', { 'prism-core.cadence': '1', 'prism-core.async': false, 'prism-core.passes': '1', 'prism-core.intensity': 280, 'prism-core.guard': 100 }),
      rec('pre12-async', 'high', { 'prism-core.cadence': '2', 'prism-core.async': true, 'prism-core.passes': '2', 'prism-core.intensity': 200, 'prism-core.guard': 100 }),
    ]);
    upgradeCoreInstalls([]);
    const after = Object.fromEntries(store.installs().map((r) => [r.id, r]));
    const ini = (id: string) => fs.readFileSync(path.join(dir, id, 'prism-core.ini'), 'utf8');
    assert(tierOf(after['old-extreme'].values!)?.id === 'extreme' && /NetScale=0\.7655/.test(ini('old-extreme')) && /Intensity=1\.00/.test(ini('old-extreme')), 'Upgrade: a 1.2 极致 install becomes 1.3 极致 (150%)');
    assert(tierOf(after['old-flagship'].values!)?.id === 'flagship' && /NetScale=0\.6250/.test(ini('old-flagship')), 'Upgrade: a 1.2 旗舰 install becomes 1.3 旗舰 (100%)');
    assert(after['old-custom'].values!['prism-core.netSize'] === '150' && after['old-custom'].values!['prism-core.passes'] === '3' && after['old-custom'].values!['prism-core.netScale'] === undefined, 'Upgrade: tuned values stay, native becomes 150%');
    assert(tierOf(after['pre12-extreme'].values!)?.id === 'extreme' && /NetScale=0\.7655/.test(ini('pre12-extreme')), 'Upgrade: a pre-1.2 every-frame 极致 (no resolution key) becomes 1.3 极致');
    assert(after['pre12-every'].values!['prism-core.netSize'] === '150' && Number(after['pre12-every'].values!['prism-core.intensity']) === 280 && /NetScale=0\.7655/.test(ini('pre12-every')), 'Upgrade: a pre-1.2 tuned every-frame install keeps its values at 150%, not 62.5%');
    assert(after['pre12-async'].values!['prism-core.netSize'] === undefined && /NetScale=1\.00/.test(ini('pre12-async')), 'Upgrade: asynchronous installs are left as they are (native)');
    store.saveInstalls(before);
    fs.rmSync(dir, { recursive: true, force: true });
  }
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

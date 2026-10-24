# Changelog

## 1.4.2 — 2026-10-24 · the best path first

- A game that has DX12 now goes the DX12 way: the best path first, the others only behind it
  (the game's DLSS, then present mode, then the frame bridge). The analysis finds DX12 where 1.4.1
  missed it: an executable that imports D3D11 is still searched for a D3D12 renderer in its side
  DLLs, an engine that loads D3D12 at run time is found by its name in the binary, and of a game's
  DX11 and DX12 executables (bin\x64 and bin\x64_dx12) the DX12 one is picked. A game that can run
  either gets both paths, and Core takes whichever the game presents with, D3D12 first: the bridge
  stands aside while D3D12 frames come. The app suggests choosing DX12 in such a game's settings.
- Fixed: present mode and the frame bridge failed (the pipeline stopped) when the settings had the
  asynchronous network on (every 2 frames, the default outside every-frame tiers); they run
  synchronously now, whatever that setting is.

## 1.4.1 — 2026-10-24 · any game: DX9 to DX12, 32 and 64-bit, any store

- Any game, wherever it came from: Prism no longer depends on a store. Games added by folder
  install like any other, and late-loading games (Denuvo) get a desktop shortcut that starts them
  through PrismLaunch.exe, so Steam's launch option is no longer needed.
- Games without DLSS, and no more DX12 check: 64-bit DX12 games without DLSS use present mode;
  DX9, DX10, DX11, games whose API is not recognised, and every 32-bit game use the new frame
  bridge. NVIDIA's network runtime is 64-bit and D3D12 only, so those games hand each presented
  frame to prism_host.exe, a 64-bit helper that runs Prism's pipeline and hands it back: DX10 / DX11
  through a staging texture, DX9 through a system-memory surface (Reset is hooked so Prism's own
  video memory is released first). The copies wait for the GPU, so the frame-rate cost is higher
  than in DX12, and the game's UI goes through the network with the picture. These games have no
  in-game panel yet: their settings are changed in the app (live, as before). Verified end to end
  with test programs (DX9 and DX11, 32 and 64-bit, through the real helper and network); not yet in
  a game.
- 32-bit games: a 32-bit Prism Core (the bridge only) and 32-bit loaders; the 64-bit Core sits
  beside them as prism64_nvngx.dll for the helper.
- ReShade and OptiScaler are kept: the loader takes a DLL name the game imports that no other mod
  holds. Only when every such name is taken does the other mod's file move aside, to
  <name>.prism-chain.dll, and Prism's loader loads it in the system DLL's place, so it keeps
  working. Rollback puts it back. Prism hooks every module that exports NGX's upscaler API and
  acts at the outermost call, so with OptiScaler it gets the frame after OptiScaler's upscale.
- Fixed: in some games loaded through a loader other than version.dll the in-game panel would not
  open. Core now hooks Present as soon as the game's graphics are up, instead of waiting for the
  game to create DLSS through a module it watched; and loaders load Core only into the game's own
  executable (Target=), not into a launcher or crash reporter beside it, each of which writes its
  own log.
- New loaders: d3d9.dll and d3d11.dll (both bitnesses).

## 1.4.0 — 2026-10-23 · a grading desk, games without DLSS, more loaders

- 自定义 (Look=5): a custom look with 66 controls over six pages — tone (exposure, contrast and
  its pivot, highlights, shadows, whites, blacks, midtones), curves (luminance, red, green and blue,
  five points each), colour (white balance, saturation, vibrance, skin protection and alignment,
  overall mix), three-way wheels, hue by hue (turn, saturation, lightness for six hues) and film
  (dye density, highlight hue path, sky, desaturation, vignette, grain). Three slots; any preset
  loads as a starting point (and matches it exactly). The five presets stay as they were.
- The in-game panel opens a grading desk beside itself for the custom look: a tone curve drawn as
  bars and a hue ring (spoke length is saturation, turn is hue, markers are the wheels), both
  draggable, every control by mouse, keyboard (Shift for fine steps) or gamepad (LB / RB pages).
- The app has the same desk, full screen, with a live preview that runs the same grade (a WebGL
  port of the shader) on a colour chart or a screenshot of your own. Both desks write the same
  lines in prism-core.ini; the app no longer drops what the panel saved.
- Present mode: DX12 games without DLSS now install. Core runs on the frame as it is presented
  (SDR, scRGB or HDR10), with no motion vectors: the network starts fresh every frame and the UI
  goes through it with the picture. The app picks it by itself for those games.
- More loaders: where the game does not import version.dll, Core loads through winmm.dll,
  dinput8.dll, dxgi.dll or d3d12.dll (whichever the EXE imports; dxgi.dll otherwise). A file of
  another mod by that name is backed up, overwritten, and put back on rollback.
- 低饱和 no longer looks milky: it sits a fifth of a stop lower with deeper blacks and keeps a
  little colour in the highlights.
- Every tier now defaults to 100% intensity (the network's own answer) with 2 passes.

## 1.3.1 — 2026-10-23 · Denuvo games, steadier characters, sharper hair

- Games protected by Denuvo Anti-Tamper (Crimson Desert crashed at start with Prism installed)
  now work alongside it. The app recognises them by their executable's section names and loads
  Prism Core late: nothing is loaded at start-up, and on the first press of Home in the game,
  PrismLaunch.exe (set once as the game's Steam launch option) loads Core into the running game
  the ordinary way and the panel opens. No game file is changed and the protection is not
  touched. The app shows the launch option to copy, and reminds you to clear it after a rollback.
  Other games install as before.
- Fast-moving characters flicker less: last frame's edit, followed along the motion vectors and
  checked by depth, is blended in within a narrow band around this frame's, so frame-to-frame
  jitter is damped without trails (Steady, SteadyBand).
- Hair, lashes and thin strands stay sharp at the 100% and 150% sizes: the edit may no longer
  take away more than 15% of the native picture's fine contrast (DetailKeep).
- Upgrading from before 1.2: every-frame installs without a resolution setting keep native-like
  detail (150%) instead of dropping to 62.5%.

## 1.3.0 — 2026-10-23 · in-game panel, colour looks, 旗舰 at 100% (+30% against RenoDX 8.5)

- In-game panel: Home (or View + Y on a gamepad) opens Prism Core's own panel in the game. It
  shows a frame-time barcode (one bar per rendered frame), the rendered and displayed frame rates,
  the 1% low and Prism's GPU time, and changes every setting live: tier, intensity, passes,
  cadence, network resolution, skin guard, split view (drag its line on screen) and a frame-rate
  corner. Mouse, keyboard and gamepad all work; while the panel is open the game gets no input.
  It draws on generated frames too, and at the same brightness in SDR, scRGB and HDR10. Verified
  in Forza Horizon 6 with 2x frame generation.
- Colour looks (色彩风格), on top of tier and intensity: 默认 (the network as it is), 高还原 (the
  original's colour with only the network's light and shade: the least AI-looking), 高饱和 (vivid
  and clean, for anime and stylised games), 低饱和 (restrained colour, cool whites: the film and
  series look) and 浓烈 (deep contrast, rich colour). They grade in scene light before the game's
  tone mapping, keep greys neutral and skin natural, and never clip colours. In the app and the panel.
- Looks, third pass: the film-dye density now follows how much brighter each hue looks than grey
  (Helmholtz-Kohlrausch), so blues and magentas deepen while yellows stay clean instead of olive;
  bright reds pass through orange and bright blues through cyan on their way to white, as on film
  (skin untouched); and colours are brought back into gamut by an ACES-style soft compression in
  linear light, which removes the hard steps saturated blue gradients showed in 高饱和 and 浓烈.
- New resolution scale for the every-frame tiers. The network's resolution is named by pixels
  relative to the flagship's base: 100% is the network at 62.5% of the output a side, its edit
  brought back up to native along the native picture's edges; 150% has half as many pixels again
  (76.5% a side). 旗舰 runs at 100%, 极致 at 150% (both 2 passes, 240%), and native is gone: next
  to 150% it costs far more for detail the upsample keeps anyway. 极致 opens on RTX 5080 too.
- Target frame rate (目标帧率, in the panel: off, 40, 50, 60, 90 rendered frames per second):
  when the game falls short, the network's resolution steps down a size at a time (to 50% a side at most) and
  steps back up once the measured cost says the next size fits. Each size holds at least three
  seconds, so it never flickers from changing every frame, and a step that did not make the game
  faster (it was held back by the CPU) is undone. It changes size without rebuilding anything.
- Below native resolution the network's edit now comes back up through a guided filter instead
  of the joint bilateral upsample: just as close to the native result, and about 12% less flicker
  in motion (`UpFilter=0` restores the 1.2 behaviour).
- Changes made in the panel are saved to `prism-core.ini` and show up in the app; the app keeps
  the panel's own settings (its place, the frame-rate corner, its key, the target frame rate)
  when it rewrites the file.
- Rolling back no longer reports `prism-core.ini` as changed after an install.

## 1.2.0 — 2026-10-22 · Prism Core only; no trails, flat frame times

Forza Horizon 6 benchmark, RTX 5090 Laptop, 2560×1600, DLSS Quality, 2× frame generation, SDR:
Prism off 118 FPS; Prism 旗舰 79 FPS; Prism 极致 67 FPS; RenoDX DLSS 5 8.5.0-rc10 at native
resolution, 2 passes, intensity 2.0: 65 FPS. Prism tiers and RenoDX had 0 stutters.

- Prism Core is the only route. The ReShade presets (旗舰 / 均衡 / 流畅 / 原片优先) and their
  community components (ReShade, RenoDX, OptiScaler, Feeder, LumeniteFX) are gone from the app and
  the installer, so nothing is downloaded any more (the "访问 RankFTW/rhi-repo 的发布页失败" error
  can no longer happen). Earlier add-on installs can still be rolled back from 还原记录.
- Five tiers: 极致 (every frame, 2 passes, native, 210%; RTX 5090 only), 旗舰 (every frame,
  2 passes, network at 75% with an edge-aware upsample, 240%; the default on RTX 5080/5090),
  高画质 (every 2 frames, 2 passes, 200%), 标准 (every 2 frames, 1 pass, 150%), 流畅 (every 4
  frames, 1 pass, 120%). Any setting can still be tuned; the tier then shows as 自定义.
- No trails on every-frame tiers: the network answers the frame on screen, nothing is carried. In
  Forza Horizon 6, wet roads, reflections and spray stream past at 100–260 px a frame and don't
  follow the motion vectors, so a carried edit trailed behind fast cars; the game now starts on 旗舰.
- Flat frame times: the passes of an asynchronous launch are staggered over consecutive frames
  (CONTROL, every 2 frames, 2 passes: frame-time deviation 4.6–5.1 → 1.8–1.9 ms). Every-frame tiers
  do the same work on every frame (deviation 0.6–0.9 ms).
- Network passes 1–5 (3 and up marked as a playground) and intensity 0–300%.
- Core's log names its real version.

## 1.1.2 — 2026-10-22 · Less ghosting, no freezes, NVIDIA's new model built in

- Ghosting under motion: a study on a recorded game sequence with the real network as reference
  (`docs/research/ghosting`) showed the error follows the age of the shown edit. Prism Core now
  runs the network every 2 frames by default, lands and launches on the same frame, samples the
  network's landed answer directly through a warp to its launch frame, gives the network the true
  motion since its previous run, fills newly revealed surfaces from same-depth neighbours and keeps
  30% of the previous edit where both agree. Offline, against 1.1.1 as it shipped: 29% less error on moving pixels,
  38% less flicker.
- No more freezes from a GPU wait: the network lands only after the CPU sees it finished; the
  game's queue never waits on the GPU (field reports tied the old wait to freezes in Crimson Desert
  and FF16).
- NVIDIA's model 310.9.16.0 ships inside the installer and installs itself (re-verified as signed
  by NVIDIA every time). On Prism's bench it is 44% cheaper than 310.8 (2560×1600: 9.2 → 5.1 ms).
- Prism Core rebuilds itself when the resolution, window mode or DLSS mode changes instead of
  switching off.
- Anti-cheat: games that can be played offline without their anti-cheat (NBA 2K27 for now) can be
  installed after the user accepts the risk; every other anti-cheat game stays blocked, and known
  online titles (F1 25, FC 26, Apex Legends) are blocked even when their anti-cheat files are gone.
- A manually added folder can be removed from the list (files are untouched).
- Component downloads (kept for the code paths that still use them) show their progress, give up
  on a connection that sends nothing for 30 seconds and retry up to 3 times.
- Core records frame sequences (`Record=N`) for offline studies, and has frame-time statistics and
  an experimental frame cap for the 1.2 frame-pacing work.

## 1.1.1 — 2026-10-21 · One-click Prism Core

- Prism Core is now a route and a preset in the app: DX12, 64-bit games with their own DLSS get it
  by default. No ReShade and nothing to download; Core ships inside the installer. Install and
  rollback go through the same transactional installer as every other route.
- Core settings in the game panel: network cadence (every 1–4 frames), asynchronous queue, edit
  intensity and character guard. With Core installed, changes are written to `prism-core.ini` at
  once and a running game picks them up within a second.
- Prism Core reads `prism-core.ini` beside the game (defaults, intensity, guard, remappable
  hotkeys) and re-reads it when it changes. Verified in CONTROL Resonant: a rewrite of the file
  switched the running game from every 2nd to every 4th frame within a second.
- Safety checks before installing Core: anti-cheat (as before; F1 25's EA Javelin is detected and
  blocked), another mod's `version.dll` (blocks the install rather than overwrite it), and DLSS 5
  add-ons that hook the same calls (moved out on install, put back on rollback).
- Installer: a failed post-install check now rolls the install back instead of leaving files
  behind without a record. Rollback also removes Core's log.
- The rail shows the real app version (it said 0.1.0).

## 1.1.0 — 2026-10-21 · Prism Core

Prism Core, Prism NR's own DLSS 5 neural rendering runtime (experimental preview,
`PrismCore-1.1.0-win64.zip`, manual install).

- Standalone: a `version.dll` loader, NGX hooks on the game's DLSS evaluate (super resolution and
  ray reconstruction), its own parameter block for the signed runtime (which exports no allocator)
  and its own D3D12 compute pipeline. No ReShade, OptiScaler or RenoDX.
- Temporal reuse: the network on one frame in N at full resolution; the edit carried along the
  game's motion vectors with a depth test between runs (0.13–0.2 ms per carried frame).
- Asynchronous network on its own compute queue, synchronised with the game through a hook on
  `ExecuteCommandLists` and two fences. CONTROL Resonant, RTX 5090 Laptop: 32.0 → 43.0 rendered
  FPS against the network on every frame, NR cost 12.1 → 4.0 ms.
- Character guard on the edit's chroma: skin colour shift −82%, red hair −44%, lighting unchanged.
- Hotkeys: F8 on/off, F2 cadence, F3 asynchronous, F4 split view, F11 frame capture (F2/F3 rather than F5/F6, which RenoDX also polls).
- Research and tools: `docs/research`, `docs/architecture`, the NR bench that drives the signed
  network outside any game, the NETS GPU harness and CPU reference.
- App: a fixed-plus-linear network cost model per GPU (`electron/shared/nrplan.ts`), from Prism's
  own bench measurement.

Disproved on the way, kept in the notes: running the network at a jittered lower resolution and
accumulating (the network synthesises different detail at low resolution), spatial tiles (the
network grades a crop differently), and asynchronous overlap at a cadence of 2 (it competes with
the game for the same shader cores).

## 0.1.0 — 2026-10-21 · First release

First Windows installer (`PrismNR-0.1.0-x64-setup.exe`, per-user NSIS, not code-signed).

- RenoDX settings written by Prism: intensity, local structure, colour strength and passes per preset, F6 to toggle neural rendering. Balanced and fluid presets follow the render resolution on the native-hook route.
- Preset tuning after in-game testing: flagship matches RenoDX strength at 1.0 with two passes; art styles lowered so stylised and anime games keep their look.
- The RenoDX add-on is fetched from its own release tag, and NVIDIA DLLs bundled in downloaded archives are stripped.
- Prism shaders compile with no warnings in ReShade 6.8.
- Command-line install (`dist-electron/cli/install.js`) with `--style` and `--route`.
- Verified end to end on CONTROL Resonant with an RTX 5090 Laptop.

## 0.1.0-preview — 2026-10-20 · Source preview

First public source.

- Game library scan for Steam, Epic, GOG and Xbox.
- PE analysis with delay-load imports and side renderer DLLs; anti-cheat detection.
- Route choice: native DLSS hook, OptiScaler Pre-SR, DLSS5-Feeder.
- Component store: ReShade, OptiScaler DLSS-NR, DLSS5-Feeder and RenoDX fetched from official releases with SHA-256 checks; manual import for the rest.
- NVIDIA NGX file discovery with signature checks; nothing NVIDIA is downloaded.
- Transactional install with backups, automatic rollback on failure, and one-click restore.
- Four presets by GPU tier, five art styles and 17 curated game profiles.
- Prism shader pack: Reference, Fidelity Guard, Temporal Stabilise, Texture, Tone Anchor, Compare.
- Data-field interface with maximal motion and a 极致 / 标准 / 关 control.


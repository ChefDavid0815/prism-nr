# Changelog

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


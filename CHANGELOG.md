# Changelog

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


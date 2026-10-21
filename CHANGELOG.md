# Changelog

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


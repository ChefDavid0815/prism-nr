# Changelog

## 0.1.0 — 2026-10-20 · Source preview

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

Not yet verified: an end-to-end install into a game, live component downloads, and the shaders compiled in ReShade. No signed installer.

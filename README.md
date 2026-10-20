<div align="center">

<img src="docs/brand/logo.svg" width="112" alt="Prism NR logo: a white ray enters a hairline prism and leaves as four spectral rays" />

# Prism NR

**Light in, spectrum out.**
A DLSS 5 neural rendering manager. One click, guarded, reversible.

![Version](https://img.shields.io/badge/version-0.1.0%20source%20preview-000000)
![Platform](https://img.shields.io/badge/platform-Windows%2011-000000)
![GPU](https://img.shields.io/badge/GPU-RTX%2050-000000)
![License](https://img.shields.io/badge/license-MIT-000000)

[中文说明](README.zh-CN.md) ·
[Safety model](SECURITY.md) ·
[Changelog](CHANGELOG.md) ·
[Design system](DESIGN.md)

</div>

![Prism NR poster: a black field of hairline barcode columns with a sine lattice and the Prism NR wordmark](docs/brand/poster.svg)

Community DLSS 5 mods work, but getting them running means picking between
ReShade add-ons, OptiScaler forks and Feeder builds, copying the right DLLs,
matching driver versions and editing preprocessor macros, then living with
faces that look regenerated, lifted shadows and a HUD that gets "neuralised"
too. **Prism NR does that work for you.** It finds your games, reads each
executable, picks the route that fits, downloads the open-source parts from
their official release pages, verifies them, installs everything in one
transaction and can put the game folder back exactly as it was. Its own shader
plugins keep neural rendering an enhancement instead of a replacement.

## What it does

| | |
|---|---|
| **Game library scan** | Reads Steam (`libraryfolders.vdf`, app manifests), Epic manifests, GOG registry entries and Xbox `MicrosoftGame.config` installs. Store tools, runtimes and stray folders are filtered out. |
| **Executable analysis** | Parses each game's PE headers, including delay-load imports and side renderer DLLs, to find DX9 to DX12, Vulkan and OpenGL; reads bitness, native DLSS and its version, Streamline, frame generation and other upscalers. |
| **Route choice** | *Native hook* when the game already calls DLSS; *OptiScaler Pre-SR* when performance matters (neural rendering runs before upscaling, on the smaller image); *Feeder* for games with no DLSS. The choice is made for you and explained. |
| **Anti-cheat block** | Easy Anti-Cheat, BattlEye, EA Javelin, HoYoProtect, Tencent ACE, GameGuard, XIGNCODE3, PunkBuster, Vanguard and NetEase protection are detected from files on disk. Those games are refused, not warned about. |
| **One-click install** | Fetches ReShade, OptiScaler DLSS-NR builds, DLSS5-Feeder and the RenoDX add-on from their release channels, verifies the SHA-256 GitHub publishes for each asset, writes `ReShade.ini`, the preset, technique order and every parameter, then records a manifest. A failure mid-way rolls back what was written. |
| **NVIDIA files stay yours** | Prism never downloads or redistributes NVIDIA binaries. It finds `nvngx_dlssnr.dll` and `nvngx_dlss.dll` already on your PC, checks the Authenticode signature, prefers NVIDIA-signed copies and says so when only an unsigned one exists. |
| **Presets and art styles** | Four presets by GPU tier (Flagship, Balanced, Fluid, Faithful) and five art styles (Realistic, Stylised, Anime, Dark, Retro), plus curated profiles for 17 games with community notes. |
| **Restore** | Every overwritten file is backed up. Restore puts the folder back, notes files you changed after install, and keeps the record stamped *restored*. |
| **Diagnostics** | GPU class and expectation, NGX files with signature state, a log, and a JSON report you can share. |

<table>
<tr>
<td width="50%"><img src="docs/screenshots/plugins.png" alt="Plugin hub listing 13 plugins with code, purpose, cost and origin" /></td>
<td width="50%"><img src="docs/screenshots/history.png" alt="Restore history with one active install and a restore button" /></td>
</tr>
</table>

![Game library: data field across the top, games grouped by route with barcode fingerprints, and the selected game's pipeline on the right](docs/screenshots/library.png)

<sub>Screenshots use the app's preview data, not a live scan.</sub>

## Prism plugins

Five shaders written for this project (ReShade FX, self-contained, MIT):

| Code | Plugin | What it is for |
|---|---|---|
| PR-10 | **Reference** | Saves the frame before neural rendering so the other plugins have an original to work against. |
| PR-11 | **Fidelity Guard** | Soft-limits how far each pixel's brightness and colour may move from the original, puts the original's lost detail back with a frequency split, stops shadows being lifted, and keeps marked HUD rectangles untouched. Needs the Feeder route today. |
| PR-12 | **Temporal Stabilise** | Removes frame-to-frame flicker with history clipped to the local neighbourhood, so motion does not ghost. |
| PR-13 | **Texture** | Contrast-adaptive micro detail and luma-weighted grain against the waxy look. |
| PR-14 | **Tone Anchor** | Black-level toe, highlight shoulder and a saturation guard, in a working space that also handles scRGB HDR. |
| PR-15 | **Compare** | Split view between the reference and the final image. |

Community and NVIDIA components appear in the same hub with their code, cost,
GPU and source: RenoDX DLSS 5 add-on, Deep Fried Chicken, DLSS5-Feeder,
LumeniteFX, OptiScaler DLSS-NR Pre-SR, DLSS multi-frame generation and MGPU
Bridge.

## Presets

| Preset | For | What changes | Expectation |
|---|---|---|---|
| **Flagship** | RTX 5080 / 5090 / 5090 Laptop | Neural rendering after upscaling, full Prism chain | Best image. At 4K the neural pass alone can take close to half the frame rate. |
| **Balanced** | RTX 5070 Ti / 5080 Laptop | Pre-SR at 67 % model resolution, 2× frame generation | Close to Flagship; community tests cut the loss to roughly a quarter or a third. |
| **Fluid** | RTX 5060 / 5070 | Pre-SR at 50 %, 3× frame generation, stronger texture restore | Displayed frame rate often above native thanks to frame generation. Base frame rate and latency still pay; it is not free. |
| **Faithful** | Any RTX 50 | Low intensity, strict guard | Lighting improves; faces and art direction stay. |

## Build from source

Requirements: Windows 11, Node.js 20+, an RTX 50 GPU for neural rendering.

```bash
npm install
npm run dev        # Vite + Electron with live reload
npm run build      # type-check and production build
npm run test:core  # read-only self-test against this PC: GPU, scan, analysis, plans
```

## What has been verified

- Type-check and production build pass.
- The self-test ran on an RTX 5090 Laptop (driver 617.42, Windows build 26300): GPU classification, Steam and Epic scanning, PE analysis (including delay-loaded DX12), anti-cheat detection and NGX file discovery with signature checks.
- The Electron app ran against that real backend; the UI was checked at 1440 × 900 and 1120 × 720.

Not verified yet: an end-to-end install into a game, the component downloads
against live release pages, and the Prism shaders compiled inside ReShade. There
is no signed installer; this is a source preview.

## Credits and license

Prism NR is MIT-licensed open source. It is not an NVIDIA product and is not
affiliated with NVIDIA; DLSS is an NVIDIA trademark. It builds on
[ReShade](https://reshade.me) (BSD-3-Clause),
[OptiScaler DLSS-NR](https://github.com/Dagherbou/OptiScaler_DLSSNR) and
[Pre-SR Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) (GPL-3.0),
[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) and
[RenoDX](https://github.com/RankFTW/rhi-repo), each under its own license.
Do not use injection mods in online games.

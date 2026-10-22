# Prism Core

Prism Core is Prism NR's own DLSS 5 neural rendering runtime. It needs no ReShade, no
OptiScaler and no RenoDX: a `version.dll` loader brings it into the game, it intercepts the
game's own DLSS call, runs NVIDIA's signed network itself and composes the result back onto the
frame. **Experimental preview, tested on CONTROL Resonant with an RTX 5090 Laptop.**

## What it does differently

- **Temporal reuse instead of lower resolution.** The network runs on one frame in N at full
  resolution; in between, Prism carries the network's *edit* (`log2(neural / input)`, never the
  picture) along the game's motion vectors, admitting it only where depth still matches. A carried
  frame costs 0.13–0.2 ms of GPU time.
- **The network on its own compute queue.** On the launch frame the network is recorded on a
  compute list and submitted when the game submits its frame; it runs alongside the next frame and
  lands one frame later. The game's queue waits on a fence only on the landing frame.
- **Character guard.** The edit's chroma is limited on skin and strongly coloured surfaces, so the
  network can relight a face but not recolour it (skin colour shift −82% on CONTROL).
- **Measured, not assumed.** Every design choice is backed by an experiment with the real network;
  see [the architecture notes](../../docs/architecture/prism-1.1.md).

## Measured in CONTROL Resonant

Same static shot, output 2560×1600, render 1280×800, ray reconstruction, RTX 5090 Laptop:

| Mode | Rendered FPS | NR cost per frame |
|---|---:|---:|
| Off | 52.2 | — |
| Network every frame (what RenoDX does, one pass) | 32.0 | +12.1 ms |
| Every 2nd frame, edit carried | 37.5 | +7.4 ms |
| **Every 4th frame, asynchronous (default)** | **43.0** | **+4.0 ms** |

A second session with frame generation ×2 and Performance ray reconstruction (the numbers in the
1.1 video): off 62.3 FPS, network every frame 34.0 (+13.4 ms), Prism 1.1 default 47.6 (+5.0 ms).
Prism 1.0's balanced preset measured 39.7 against 64.3 off (+9.65 ms) in a session of its own.

Quality of the default, measured offline against the network's own full-resolution answer on
every frame: 0.06–0.09 stop mean error, against 0.16 for running the network at half resolution.
Fast-moving faces lose the most detail; motion-adaptive cadence is next.

## Install

Prism NR 1.1.1 and later install Core in one click: pick a DX12 game with its own DLSS, keep the
Prism Core preset, apply. Rolling back in the app removes everything. By hand:

1. Close the game. Copy `version.dll` and `prism_nvngx.dll` next to the game's executable.
2. Make sure a signed `nvngx_dlssnr.dll` is in the same folder (Prism NR's app finds the one on
   your PC; Prism never downloads or redistributes NVIDIA files).
3. Turn off any other DLSS 5 add-on in that game (for example rename `renodx-dlss5.addon64`).
4. Start the game with DLSS (super resolution or ray reconstruction) on. `prism-core.log` beside
   the game says what Core found.

To remove it, delete the two DLLs (and `prism-core.ini` if you made one).

## Settings: prism-core.ini

Core reads `prism-core.ini` beside the game at start and again whenever the file changes (it checks
about once a second), so it can be tuned while the game runs. The app writes it; by hand:

```ini
[Prism Core]
Enabled=1        ; 0 starts with Core off (the toggle key still works)
Cadence=2        ; the network every 1-4 frames
Passes=2         ; network passes per run, 1-3: each pass refines the one before (own feature, own history)
Async=1          ; the network on its own compute queue (cadence 2 and up)
Stability=0.3    ; how much of the previous edit stays where a landed edit is sampled, 0-0.9
NetMotion=exact  ; exact: the network gets the true motion since its last run; approx: x span
Land=poll        ; poll: land only once the network finished (no GPU wait); wait: 1.1 behaviour
Intensity=1.00   ; how much of the network's edit is kept, 0-1
Guard=1.00       ; character guard strength on skin and strong colour, 0-1
KeyToggle=F8     ; F1-F24, a virtual-key number, or none
KeyCadence=F2
KeyAsync=F3
KeySplit=F4
KeyCapture=F11
```

Missing keys keep their defaults. Intensity and guard apply from the next network frame.

For studies: `Record=90` writes 90 consecutive frames (upscaled frame, motion vectors, depth) to
`prism-record/` (about 24 MB per frame at 2560×1600); `StatsSeconds` and `Label` set the
frame-time statistics written to the log; `FpsCap`, `Cadence=0` (adaptive) and `NetPriority`
are experimental.

| Key | Action |
|---|---|
| F8 | Prism Core on / off |
| F2 | network every 1 / 2 / 3 / 4 frames |
| F3 | asynchronous network on / off |
| F4 | split view: original left, Prism right |
| F11 | capture one frame's original, input, network output and result to `prism-capture/` |

## Build

MinGW-w64 g++ (C++20). Shaders compile at run time with the system's `d3dcompiler_47.dll`.

```bash
bash native/prism-core/build.sh
```

| Path | What |
|---|---|
| `src/proxy/` | the `version.dll` loader |
| `src/core.cpp` | NGX hooks (create / evaluate), feature tracking, logging |
| `src/pipeline.cpp` | exposure, encode, network (sync and async), carry, bake, fill, guard, compose, recorder |
| `src/ngx_param.h` | NGX parameter blocks by MSVC vtable slot |
| `tests/nr_bench.cpp` | drives the signed network outside any game: cost, sequences, async overlap |
| `tests/nets_harness.cpp` | GPU check of the NETS shaders against the CPU reference |
| `tests/shader_check.cpp` | compiles every runtime shader entry point at build time |
| `tests/ab_run.cjs` | A/B runner: switches prism-core.ini settings in a running game and collects the statistics |
| `shaders/` | NETS and guard shaders used by the harness |

## Limits

- One game tested. Games without DLSS need another source of motion vectors and depth.
- Frame pacing spreads the network frame's cost over the others; the asynchronous mode removes most
  of it at a cadence of 4, not at 2.
- Not for games with anti-cheat.

Prism Core contains no NVIDIA code or binaries. Interface details come from RenoDX DLSS5 (MIT)
and neural-upstream (MIT); MinHook is BSD-2 (`external/minhook/LICENSE.txt`).

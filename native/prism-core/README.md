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

Quality of the default, measured offline against the network's own full-resolution answer on
every frame: 0.06–0.09 stop mean error, against 0.16 for running the network at half resolution.
Fast-moving faces lose the most detail; motion-adaptive cadence is next.

## Install (manual, for now)

1. Close the game. Copy `version.dll` and `prism_nvngx.dll` next to the game's executable.
2. Make sure a signed `nvngx_dlssnr.dll` is in the same folder (Prism NR's app finds the one on
   your PC; Prism never downloads or redistributes NVIDIA files).
3. Turn off any other DLSS 5 add-on in that game (for example rename `renodx-dlss5.addon64`).
4. Start the game with DLSS (super resolution or ray reconstruction) on. `prism-core.log` beside
   the game says what Core found.

To remove it, delete the two DLLs.

| Key | Action |
|---|---|
| F8 | Prism Core on / off |
| F5 | network every 1 / 2 / 3 / 4 frames |
| F6 | asynchronous network on / off |
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
| `src/pipeline.cpp` | exposure, encode, network (sync and async), carry, land, guard, compose |
| `src/ngx_param.h` | NGX parameter blocks by MSVC vtable slot |
| `tests/nr_bench.cpp` | drives the signed network outside any game: cost, sequences, async overlap |
| `tests/nets_harness.cpp` | GPU check of the NETS shaders against the CPU reference |
| `shaders/` | NETS and guard shaders used by the harness |

## Limits

- One game tested. Games without DLSS need another source of motion vectors and depth.
- Frame pacing spreads the network frame's cost over the others; the asynchronous mode removes most
  of it at a cadence of 4, not at 2.
- Not for games with anti-cheat.

Prism Core contains no NVIDIA code or binaries. Interface details come from RenoDX DLSS5 (MIT)
and neural-upstream (MIT); MinHook is BSD-2 (`external/minhook/LICENSE.txt`).

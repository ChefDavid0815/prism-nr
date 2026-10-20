# Safety model / 安全模型

Prism NR changes files inside game folders. These are the rules it follows.

## What Prism will not do

- **No NVIDIA redistribution.** Prism never downloads, bundles or uploads `nvngx_dlssnr.dll`, `nvngx_dlss.dll` or any other NVIDIA binary. It copies only files already on your PC, checks their Authenticode signature, prefers NVIDIA- or WHQL-signed copies, and tells you when the only copy it found is unsigned.
- **No online games.** Games with Easy Anti-Cheat, BattlEye, EA Javelin, HoYoProtect, Tencent ACE, nProtect GameGuard, XIGNCODE3, PunkBuster, Vanguard or NetEase protection files are refused. Prism does not bypass anti-cheat.
- **No anti-virus workarounds.** Prism never asks you to disable Windows Security or whitelist anything.
- **No telemetry.** Nothing about your PC or games leaves your machine. The only network requests are to GitHub release APIs and reshade.me when a component is needed.

## Downloads

Third-party components come only from the project's own release channel:
ReShade from reshade.me, OptiScaler DLSS-NR, DLSS5-Feeder and the RenoDX add-on
from their GitHub releases. When GitHub publishes a SHA-256 digest for an asset,
Prism verifies it and deletes the file on mismatch. Components without a public
release channel (LumeniteFX, Deep Fried Chicken, MGPU Bridge) are imported by you
once, from a file you chose.

## Install and restore

1. Before writing, every file that would be overwritten is copied to
   `%APPDATA%\Prism NR\backups\<game>\<install>\`.
2. Files are written in order; if any write fails, everything already written is
   rolled back immediately.
3. Each write is hashed (SHA-256) and recorded in `installs.json`, and a
   `prism-nr.manifest.json` is placed next to the game executable.
4. Restore puts backed-up files back and removes files Prism created. Files you
   changed after install are restored too, and listed in the record's notes.
5. Re-applying a preset restores the previous install first, so a game folder
   never holds two Prism layers.

## Reporting

Open an issue with the diagnostics report (Diagnostics → Export). The report
contains your GPU, driver, game install paths, install records and the app log;
review it before sharing.

---
version: 1
slug: "src-app-tsx"
primary_target: "src/App.tsx"
related_targets: []
---

# Surface: Prism NR main window

Scope: the Electron app window (game library, plugin hub, presets, restore history, diagnostics, settings). Mode: Operate.
Audience: heavy PC players on RTX 50 hardware, at night in a dim room, launching the tool before a game session.
Task: pick a game, see how neural rendering would get into it and what it costs, pick a preset or plugins, apply, roll back.
Constraints: Chinese UI first; honest expectations; never fabricate live frame data (estimates are labelled 估算).

## Direction contract

THESIS: The window is a data field. Every game, plugin and frame budget is encoded as hairline bars and dense numerals on pure black. It refuses the gamer-dashboard default: cover-art grid, glowing RGB, NVIDIA green.

OWN-WORLD: #000 ground, #FFF ink, two greys only for hierarchy (#8C8C8C secondary text, #262626 hairlines). One monospace face (JetBrains Mono) for every Latin numeral and label, Microsoft YaHei UI for Chinese. Colour exists only as spectral dispersion: blocked #FF3B30, caution #FFC400, ready #3DDC97, info #4DA3FF, drawn as 2px bars or strokes on state, never as fills. Selection is inversion: white field, black ink.

STORY: The user reads the GPU verdict, sees the library scanned and sorted by route, selects a game, reads its route and cost budget, picks a preset, applies, and can restore from history.

FIRST VIEWPORT: Top strip with wordmark and GPU readout in tabular columns. Left rail of text navigation with mono counts. Centre: the game index as dense rows, each carrying a 160px barcode fingerprint drawn from its analysis. Right: the selected game, its pipeline as a bar lattice of stages with estimated ms, preset selector, inverted white Apply button.

FORM: Catalog world ikeda-datamatics, picked by the user over the assigned grounded direction 5 (darkroom contact sheet). Seed key f1230efc. Raises that fix the source's weaknesses: spectral state colour so states read; greys allowed for text hierarchy and contrast; every barcode encodes real data, never decoration. Motion is maximal by user decree: a living canvas field (bars drift, the sine lattice breathes with neural intensity, scan columns stream), digits roll into place, selection inverts with a bar wipe, Apply fires a full negative snap plus a barcode sweep transition. Hard floor: large-area flashes never exceed 3 per second. A Settings control (极致 default / 标准 / 关) governs intensity. Signature move: the per-game barcode fingerprint and the negative snap on Apply.

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance

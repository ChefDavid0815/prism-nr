---
name: Prism NR
description: A DLSS 5 neural rendering manager drawn as a living data field.
colors:
  field-black: "#000000"
  signal-white: "#ffffff"
  readout-grey: "#8c8c8c"
  quiet-grey: "#5c5c5c"
  hairline: "#262626"
  hairline-faint: "#1a1a1a"
  spectral-ready: "#3ddc97"
  spectral-caution: "#ffc400"
  spectral-blocked: "#ff3b30"
  spectral-info: "#4da3ff"
typography:
  headline:
    fontFamily: "JetBrains Mono, Microsoft YaHei UI, Microsoft YaHei, monospace"
    fontSize: "28px"
    fontWeight: 700
    lineHeight: 1.2
  title:
    fontFamily: "JetBrains Mono, Microsoft YaHei UI, Microsoft YaHei, monospace"
    fontSize: "26px"
    fontWeight: 700
    lineHeight: 1.25
    letterSpacing: "-0.01em"
  body:
    fontFamily: "JetBrains Mono, Microsoft YaHei UI, Microsoft YaHei, monospace"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: 1.6
    fontFeature: "'tnum' 1"
  label:
    fontFamily: "JetBrains Mono, Microsoft YaHei UI, Microsoft YaHei, monospace"
    fontSize: "11px"
    fontWeight: 400
    lineHeight: 1.4
    letterSpacing: "0.02em"
rounded:
  none: "0px"
spacing:
  xs: "8px"
  sm: "12px"
  md: "16px"
  lg: "24px"
  xl: "32px"
components:
  button-primary:
    backgroundColor: "{colors.signal-white}"
    textColor: "{colors.field-black}"
    rounded: "{rounded.none}"
    height: "48px"
    typography: "{typography.body}"
  button-primary-disabled:
    backgroundColor: "{colors.hairline}"
    textColor: "{colors.readout-grey}"
  button-line:
    backgroundColor: "{colors.field-black}"
    textColor: "{colors.signal-white}"
    rounded: "{rounded.none}"
    height: "36px"
    padding: "0 16px"
  row-selected:
    backgroundColor: "{colors.signal-white}"
    textColor: "{colors.field-black}"
    height: "52px"
---

# Design System: Prism NR

## Overview

**Creative North Star: "The Data Field"**

Prism NR is drawn the way Ryoji Ikeda draws data: pure black, pure white, hairline bars and dense tabular numerals. Every barcode on screen is real information. A game's fingerprint encodes its graphics APIs, bitness, DLSS version digits, install route and anti-cheat state. The top field repeats the selected game's fingerprint as drifting columns, and its sine lattice rises and falls with neural rendering intensity. Nothing is decorative noise.

Colour exists only as spectral dispersion. That is the product's own metaphor: white light enters the prism and leaves as four state colours. They appear as 2px strokes and bars, never as fills. Selection is inversion: the row, rail item, preset or segment wipes to a white field with black ink.

Motion is maximal by the user's decision. The field drifts and strobes while scanning, the cursor lights the columns under it, digits roll into place, selections wipe in, and Apply fires a full-window barcode sweep followed by one negative frame. Large-area flashes never exceed three per second. A Settings control (极致 / 标准 / 关) governs intensity.

**Key Characteristics:**
- Black ground, white ink, two greys for hierarchy, no other neutrals.
- One monospace face for all Latin text and numerals; Chinese falls back to Microsoft YaHei UI.
- Square corners everywhere; hairline rules carry all structure.
- Barcodes and bar lattices are data encodings, each legible as a measurement.
- Selection by inversion with a clip-path wipe.

## Colors

A monochrome field with four spectral state colours used as strokes only.

### Primary
- **Signal White** (#ffffff): ink, rules that matter, selected fills, the primary action.

### Neutral
- **Field Black** (#000000): the ground of every surface. Not tinted.
- **Readout Grey** (#8c8c8c): labels, column heads, secondary values. 6.9:1 on black.
- **Quiet Grey** (#5c5c5c): version strings and inert placeholders only.
- **Hairline** (#262626): structural rules between regions and table heads.
- **Faint Hairline** (#1a1a1a): rules between rows inside a table.

### Spectral states
- **Ready** (#3ddc97): installable routes, active installs, Prism-native plugins in the pipeline.
- **Caution** (#ffc400): Feeder route, warnings, unsigned NVIDIA files, preview-mode notice.
- **Blocked** (#ff3b30): anti-cheat, blockers, errors.
- **Info** (#4da3ff): community components, profile notes.

### Named Rules
**The Dispersion Rule.** Spectral colours appear only as 2px bars, strokes or barcode segments that carry a state. A spectral fill behind text is never used.

**The Inversion Rule.** The only filled highlight is white. Anything selected, current or primary becomes white with black ink.

## Typography

**Body Font:** JetBrains Mono (with Microsoft YaHei UI, Microsoft YaHei for Chinese)

**Character:** A single monospace voice, so every number lines up in columns and the interface reads like a data print-out; Chinese text sits in a clean system sans beside it.

### Hierarchy
- **Headline** (700, 28px, 1.2): page titles.
- **Title** (700, 26px, 1.25): the selected game's name in the detail panel.
- **Section** (700, 15px): block headings in the detail panel and diagnostics.
- **Body** (400, 14px, 1.6): descriptions, kept under 68ch.
- **Label** (400, 11px, 0.02em): readout keys, column heads, field data columns.

### Named Rules
**The Tabular Rule.** Every number uses tabular figures so values align across rows and roll in place without jitter.

## Layout

A fixed application shell: a 56px top readout strip spanning the window, a 184px text rail, and the stage. The library stage splits into the data field (176px tall) over the game index, beside a 448px detail panel whose footer pins the primary action. Tables use fixed grid columns with a 16px gap and 24px side padding. At widths of 1280px and below the rail narrows to 160px, the detail panel to 400px, and the index drops its platform column so game names keep their room. The window's minimum is 1120 × 720.

## Elevation & Depth

Flat. There are no shadows. Depth comes from inversion (white fills), hairline rules and the field canvas behind the index. The apply sweep uses `mix-blend-mode: difference` over the whole window, which is the only layering effect.

## Shapes

Every corner is square (0px). Rules are 1px. State marks are 2px wide vertical bars. Checkboxes are 14px squares whose inner bar wipes in from the left.

## Components

### Buttons
- **Shape:** square (0px).
- **Primary:** white field, black 16px bold label, 48px tall, fills the detail footer. On hover a barcode band sweeps across it.
- **Line:** black with a #262626 border that turns white on hover; 36px tall (28px small).
- **Disabled:** primary becomes #262626 with grey text.

### Navigation
- **Rail:** 15px labels with tabular counts on the right. Hover brightens the text; the active item inverts with a left-to-right wipe (320ms, ease-out-expo).
- **Tabs:** text with a 1px white underline that scales in.

### Game index row
- 52px rows: name, platform, API, route (2px spectral bar plus label) and a 168px barcode fingerprint. Selected rows invert with a 360ms wipe and the barcode redraws in black.

### Data field (signature)
- A canvas of hairline columns built from the selected game's fingerprint, drifting left, with a white sine lattice whose amplitude equals neural intensity and three node markers. Monospace data columns sit in its corners on a translucent black backing. While scanning or installing the columns strobe; during install the filled portion marks progress.

### Pipeline lattice
- One row per render stage: label, a run of hairline bars counting relative estimated cost, and the number. Prism-native stages draw in Ready green.

### Presets and segments
- Stacked or segmented options separated by hairlines; the chosen option inverts. Presets show name, a "适合你的显卡" outline tag when the tier fits, the GPU line and a one-line summary.

## Do's and Don'ts

### Do:
- **Do** encode real data in every barcode or bar run; label estimates as 估算.
- **Do** use inversion (#ffffff field, #000000 ink) for every selected or primary state.
- **Do** keep spectral colours to 2px state marks.
- **Do** keep large-area flashes at or below three per second at every motion level.

### Don't:
- **Don't** add rounded corners, shadows, gradients or glass.
- **Don't** introduce greys beyond #8c8c8c and #5c5c5c, or tint the black.
- **Don't** draw barcodes or bars that encode nothing.
- **Don't** use NVIDIA green or RGB gamer styling.

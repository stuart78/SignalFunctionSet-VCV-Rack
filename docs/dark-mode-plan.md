# Dark panels: audit and plan

Status: proposal, 2026-11. Nothing here is built yet. The dark colours were only
tried on panel art outside Rack, without knobs or jacks.

## How Rack does it

Rack has a global setting, `settings::preferDarkPanels` (View menu, "Use dark
panels if available"). A plugin takes part by:

- passing two files to `createPanel(lightPath, darkPath)`, which makes a
  `ThemedSvgPanel` that follows the setting live;
- using `ThemedPJ301MPort`, which swaps to Rack's dark jack by itself;
- reading the setting in any code that draws on the faceplate.

Rack has no themed knobs. Trimpot (body #0A0A0A), the white Rogans, CKSS and
VCVSlider are the same in both modes.

Fundamental's dark art uses #1f1f1f and #ebebeb, and it inverts the output plate:
a light plate with dark text.

## What the audit found

58 registered modules (5 hidden). None supports dark panels today. All 529 jacks
are the fixed silver `PJ301MPort`.

| Panel source | Count | Where the labels are |
|---|---|---|
| Figma exports (`FINISHED`) | 22 | Outlined in the art |
| Legacy, outlined in the art | 25 | Outlined in the art |
| Code-drawn | 9 (6 shipping, 3 hidden) | `sfs::PanelLabels` at runtime |
| Placeholder | 2 (both hidden) | `<text>`, which Rack ignores |

| Display | Count | Dark treatment |
|---|---|---|
| Opaque screen | 38 | Unchanged |
| No display | 14 | Art only |
| Transparent, drawn on the faceplate | 4 (Wheel, Flock, Carillon, Canopy) | A second palette each |
| Round screen (Gravity) | 1 | Screen unchanged, check its surroundings |
| Colour tiles on the faceplate (Field) | 1 | Unchanged |

The panel art is more uniform than expected, with some drift:

- Faceplates are #f0f0f0 except Overtone (#f4f4f4) and Gravity (#efefef).
- Plates come in six near-blacks: #000000, #030303, #121212, #1a1a1a, #2a2a2a, #323232.
- Screen slabs come in #1a1a32 and #1a1a2e.
- 47 panels use #231f20 ink.
- No panel uses CSS, gradients or embedded images, so colours can be changed by
  rewriting attributes.
- Legacy art has coloured placement marks under the controls. They stay hidden as
  long as the components sit on top of them.

A plain colour swap is not enough. In a trial it turned plates written as
`fill="black"` white, because black is also text ink, and it left Gravity
unconverted. Many outlined glyphs also have no fill of their own and inherit one
from a group. The converter has to work by role.

## Decisions

Decided 2026-11-11:

- **Our own dark style, not Fundamental's inverted plate.** The faceplate is a
  medium dark grey, the output plates are black and keep their white labels.
- **Screens carry less blue in dark mode.** Taking the blue out of the screen
  background also removes most of the contrast between it and the dim purples
  used for cells, grids and inactive tabs (`SCREEN_PURP`, `SCREEN_PMID`), so
  those tones need lightening as well, or Sigma's matrix and Canon's tabs fade
  into the background. Bright accents stay as they are.
- **Every legacy panel is redrawn** in the house style, so that all panels
  follow the same rules in both modes.

Candidates, mocked from the real light screenshots in
`docs/dark-mode/palette-options.jpg` (jacks and knobs there are still the light
ones):

| | Faceplate | Plates | Screens |
|---|---|---|---|
| A | #3a3a3e | #0d0d0e | #1c1c22, a little blue |
| B | #3a3a3e | #0d0d0e | #19191b, neutral |
| C | #48484c | #0d0d0e | #1c1c22, a little blue |

## Progress

2026-11-11:

- Theme layer done in `src/panel-style.hpp`: `sfs::Theme` (light and dark),
  `sfs::themedPanel(name)`, `sfs::Jack`, `PanelLabels` in theme colours, and the
  screen colours (`SCREEN_BG`, `SCREEN_PURP`, `SCREEN_PMID`, `SCREEN_DIM`,
  `SCREEN_TEXT`) following the theme.
- `tools/panel_dark.py` generates `res/dark/*.svg` and `--check`s them.
- **32 modules work in dark mode**: the 22 designer panels, the 9 code-drawn ones,
  and Drift, the first legacy panel redrawn in code. Wheel, Flock, Carillon and
  Canopy have dark palettes for their faceplate drawings; Crystal's on-panel mode,
  Chime's slider and key readout and Crystal's switch follow the theme.
- Light mode is unchanged on every converted module that was not redrawn
  (checked against the committed screenshots; the only differences are animated
  displays and two screenshots that were already out of date).
- `Rack -t` always draws light panels; render dark with `SFS_PREFER_DARK=1`.

2026-11-12: **all 56 non-placeholder modules have dark panels.** The 24
remaining legacy panels were redrawn in code (controls unchanged, plates in
`tools/panel-layout/<key>.json`). Along the way: Overtone's old art had MASK and
FILTER swapped against the code, now labelled as the code is; Muse's tap labels
now match its real taps (`TAP_LABELS`). Screens that draw their own greys use
`sfs::screenTone()`, which takes the blue out in dark mode. OP MORPH's matrix
follows the theme. `static-check.sh` fails on an unthemed panel or stale dark
art; the panel conventions, launch checklist and CLAUDE.md describe it.

Left: the hidden placeholders (Wave, Ratio) when they are finished; the hidden
reticules the tool cannot place inside nested loops (Fugue X, MetaFugue, Muse
sliders, Cycle's link latches), which affect nothing visible.

## The plan

The light SVG stays the only hand-maintained file. Dark art is generated from it.

1. **Prototype in Rack.** Canon (Figma), Drift (legacy) and Canopy (code-drawn and
   transparent) with real jacks and knobs. Settle the faceplate, the plate and
   whether jacks on a plate should be dark or silver.
2. **Theme layer.** Add `sfs::dark()`, a light and a dark palette in
   `panel-style.hpp`, and one panel helper. `PanelLabels` picks its colours from
   the palette. Replace `PJ301MPort` with one plugin-wide alias of
   `ThemedPJ301MPort`.
3. **`tools/panel_dark.py`.** Classify each element as faceplate, ink, plate,
   plate ink, rule, logo, screen or guide, including fills inherited from groups.
   Write `res/dark/<name>.svg`. Never edit a light file, so `FINISHED` panels are
   safe. Per-panel exceptions go in one table. Fail on any colour it cannot place.
4. **Redraw the 25 legacy panels** as code-drawn panels in the measured house
   style (`PanelLabels`, art from `panel_reticules.py`), which then get dark art
   the same way as the other code-drawn panels. Every control stays where it is,
   so saved patches and cables are unaffected. Each one is reviewed beside its
   old screenshot.
5. **Generate dark art for the rest** (Figma and code-drawn) and review it on a
   contact sheet.
6. **Screen palette.** Screen backgrounds and the dim tones drawn on them
   (`SCREEN_BG`, `SCREEN_PURP`, `SCREEN_PMID`, `SCREEN_LINE` and their literal
   copies in each display) come from the theme, so screens lose their blue in dark
   mode without losing contrast. 38 modules have a screen.
7. **Small code items**: the rows in the table below that name a widget.
8. **The four transparent displays**, one at a time. The geometry stays the same
   and the drawing becomes light ink on a dark faceplate. Every literal faceplate
   fill has to come from the palette, since those fills hide things behind them.
9. **Enforce it.** A check that each registered panel has a dark file and that it
   is current, added to `static-check.sh`, the launch checklist and the panel
   conventions. New modules then get dark art from their first commit.
10. **Nightly for testers, then release.** Screenshots stay light unless wanted.

**Checks.** Apart from the redrawn legacy panels, light mode has to stay
identical: compare `Rack -t` screenshots before and after. Dark mode is reviewed from `Rack -t` with the setting on, which needs an
unlocked screen and no other Rack open. Parameters and saved patches do not change.

## Every panel

"Beyond generating the dark art" is blank when generated art is all the panel needs.

| Module | HP | Panel today | Faceplate | Plate (share of panel) | Display | Beyond generating the dark art |
|---|---|---|---|---|---|---|
| Drift | 10 | Legacy outlined | #f0f0f0 | small or none | None |  |
| GSX | 12 | Legacy outlined | #f0f0f0 | #000000 3.0% | None |  |
| Fugue | 20 | Legacy outlined | #f0f0f0 | small or none | None | Custom horizontal sliders. Check against the dark faceplate. |
| Phase | 19 | Legacy outlined | #f0f0f0 | #000000 3.5% | Screen |  |
| Overtone | 10 | Legacy outlined | #f4f4f4 | #000000 1.8% | Screen | Background is #f4f4f4, normalise. Uses the custom horizontal switch. |
| Intone | 16 | Legacy outlined | #f0f0f0 | #000000 1.1% | Screen | Custom horizontal slider. Check against the dark faceplate. |
| Tine | 8 | Legacy outlined | #f0f0f0 | #000000 2.6% | None |  |
| Fugue X | 24 | Legacy outlined | #f0f0f0 | #000000 20.2% | None |  |
| MetaFugue | 44 | Legacy outlined | #f0f0f0 | #000000 9.2% | None | Custom horizontal sliders. Check against the dark faceplate. |
| Meter | 18 | Legacy outlined | #f0f0f0 | #2a2a2a 12.1% | Screen |  |
| Meter X | 8 | Legacy outlined | #f0f0f0 | #000000 25.0% | None | Pies are drawn on the plate. No colour change. |
| Beat | 10 | Legacy outlined | #f0f0f0 | #2a2a2a 6.3% | Screen |  |
| Note | 10 | Legacy outlined | #f0f0f0 | #2a2a2a 12.5% | Screen |  |
| Swell | 6 | Legacy outlined | #f0f0f0 | #2a2a2a 2.2% | Screen |  |
| Shift | 16 | Legacy outlined | #f0f0f0 | #2a2a2a 20.5% | None |  |
| Wave (hidden) | 16 | Placeholder | #f0f0f0 | small or none | Screen | Hidden placeholder. Its labels are `<text>`, which Rack ignores. Skip until it is finished. |
| Vac | 6 | Legacy outlined | #f0f0f0 | #2a2a2a 6.7% | None |  |
| Muse | 30 | Legacy outlined | #f0f0f0 | #2a2a2a 1.5% | Screen | `MuseWidget` draws a #f0f0f0 wash on the faceplate at line 1426. Slider art is dark already. |
| Gravity | 30 | Figma | #efefef | #2a2a2a 7.1% | Round screen | Round screen stays. Background is #efefef, normalise. Check what the 60 mm display box draws outside the circle. |
| Band | 24 | Legacy outlined | #f0f0f0 | #2a2a2a 2.3% | Screen |  |
| Cycle | 26 | Legacy outlined | #f0f0f0 | #2a2a2a 11.2% | Screen | `CycleLabels` draws text in code at #404040, which disappears on dark. Move it to the theme ink. |
| Operator | 10 | Legacy outlined | #f0f0f0 | #2a2a2a 6.5% | Screen |  |
| OP ENV | 20 | Legacy outlined | #f0f0f0 | #2a2a2a 1.4% | Screen |  |
| Chance | 26 | Legacy outlined | #f0f0f0 | #1a1a1a 3.7%, #000000 3.0% | Screen |  |
| Record | 16 | Legacy outlined | #f0f0f0 | #000000 5.5% | Screen |  |
| Play | 16 | Legacy outlined | #f0f0f0 | #000000 3.3% | Screen |  |
| Ratio (hidden) | 20 | Placeholder | #f0f0f0 | small or none | Screen | Hidden placeholder. Its labels are `<text>`, which Rack ignores. Skip until it is finished. |
| Arrange | 34 | Legacy outlined | #f0f0f0 | #000000 14.3%, #1a1a1a 4.5% | Screen | `LinkDotButton` sits on the plate. Check it against the final plate colour. |
| Fill | 32 | Figma | #f0f0f0 | #000000 21.9% | Screen |  |
| Chime | 28 | Figma | #f0f0f0 | #121212 18.6% | Screen | `ChimeKeyReadout` draws text in code at #404040. Move it to the theme ink. |
| Crystal | 25 | Figma | #f0f0f0 | #323232 3.4% | Screen | Has a `lite` branch filling #f0f0f0 at lines 1639 and 1856. Check what selects it. `CrystalToggle` is code-drawn. |
| Loom | 28 | Figma | #f0f0f0 | #121212 11.1% | Screen |  |
| Key | 14 | Figma | #f0f0f0 | #1a1a1a 11.9% | Screen |  |
| Slide | 28 | Figma | #f0f0f0 | #000000 4.3% | Screen |  |
| OP MORPH | 12 | Figma | #f0f0f0 | small or none | Screen |  |
| SLIDE XP | 12 | Figma | #f0f0f0 | #000000 23.0% | None |  |
| Slice | 22 | Figma | #f0f0f0 | #030303 5.3% | Screen |  |
| Kit | 22 | Figma | #f0f0f0 | #030303 4.1% | Screen |  |
| PolyKit In | 35 | Figma | #f0f0f0 | small or none | None |  |
| Field | 16 | Figma | #f0f0f0 | #1a1a1a 1.8% | Tiles on faceplate | Colour tiles stay as they are. Art only. |
| Trace | 34 | Figma | #f0f0f0 | #030303 6.4% | Screen |  |
| Sigma | 34 | Figma | #f0f0f0 | #030303 2.1% | Screen |  |
| Touch (hidden) | 16 | Code-drawn | #f0f0f0 | #1a1a1a 35.6% | None | Hidden. Generated with the code-drawn set. |
| Wheel | 42 | Figma | #f0f0f0 | #030303 3.3% | **Transparent** | Second palette for `W_*` and about 6 literal greys (wheel, strings, stroke strip). Wheel body fills need a dark ramp. |
| Helix (hidden) | 16 | Code-drawn | #f0f0f0 | #1a1a1a 5.8% | Screen | Hidden. Generated with the code-drawn set. |
| Brigade (hidden) | 16 | Code-drawn | #f0f0f0 | #1a1a1a 5.8% | Screen | Hidden. Generated with the code-drawn set. |
| Spool | 22 | Figma | #f0f0f0 | #000000 6.0% | Screen | `laneColour()` reads colours out of res/spool.svg, so it must keep reading the light file. |
| Flock | 18 | Figma | #f0f0f0 | #000000 2.9% | **Transparent** | Second palette for `FL_*`. Birds are near-black and must flip, with near birds brightest. Line 1113 paints literal #f0f0f0 to hide things. |
| Flock In | 4 | Figma | #f0f0f0 | small or none | None |  |
| Canon | 26 | Figma | #f0f0f0 | #000000 5.9% | Screen |  |
| Count | 8 | Figma | #f0f0f0 | #000000 16.7% | Screen |  |
| Carillon | 26 | Figma | #f0f0f0 | #030303 4.5% | **Transparent** | Second palette for `PL_*`. Literal #f0f0f0 fills at lines 1070, 1098 and 1109, among others. |
| Strata | 24 | Code-drawn | #f0f0f0 | #1a1a1a 7.4% | Screen |  |
| Cistern | 22 | Code-drawn | #f0f0f0 | #1a1a1a 3.0% | Screen |  |
| Hocket | 22 | Code-drawn | #f0f0f0 | #1a1a1a 32.7% | Screen |  |
| Concrète | 32 | Code-drawn | #f0f0f0 | #1a1a1a 9.3% | Screen |  |
| Concrète In | 10 | Code-drawn | #f0f0f0 | small or none | None |  |
| Canopy | 28 | Code-drawn | #f0f0f0 | #1a1a1a 3.1% | **Transparent** | Second palette for `CP_*`. Dome, tongues and shell fills need dark steel; leaf greens stay. `darkLeaf` ink test must follow the theme. |

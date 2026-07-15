# Screen (NanoVG display) style heuristic

How every custom on-screen display in this plugin should be sized and driven, so
Note / Beat / Chance / Arrange / … all feel like one instrument. Font and palette
were already consistent; the part that drifts is **scale** and **interaction**.

## 1. Design-unit coordinate space (the scale fix)

Never size things as a fraction of `box.size` (e.g. `box.size.y * 0.1`). That makes
fonts/cells balloon or shrink with the display's physical size and look nothing like
the other modules. Instead lay everything out in a **fixed design-unit grid** and
scale to pixels once:

```cpp
float s = box.size.x / DESIGN_W;   // DESIGN_W = display width in design units
// every rect / font / radius is <design units> * s
```

Pick `DESIGN_W` so the grid is **≈ 3.78 design units per mm** (Note uses 174 units for a
46 mm-wide display → 174/46 = 3.78). Equivalently `DESIGN_W = displayWidthMM * 3.783`.
Because both px and design units are proportional to mm, **`s` lands at ≈ 0.78 on every
module** — that's the tell you got the scale right.

- Note: `s = w / 174` (46 mm wide).  Beat: `s = w / 174`.
- A 93.6 mm-wide display → `DESIGN_W ≈ 354`, so `s = w / 354`.

Use the SAME `s` for x and y (keep the display's mm aspect = design aspect).

## 2. Standard sizes (in design units, multiply by `s`)

| element                     | design units            |
|-----------------------------|-------------------------|
| **standard font** (labels, numbers, values) | **9** — this is the default; use it for almost everything |
| big/title readout           | 11–13                   |
| selectable cell (pattern, step, tab body) | ~18 × 18            |
| thin bar / dot / length box | ~18 × 8                 |
| mode tab                    | ~38 × 18                |
| column pitch (cell + gap)   | **20** (18 cell + 2 gap) |
| left margin                 | ~7                      |
| row rails / dividers        | 1 unit thick            |

Cap heights: 9-unit font ≈ 2.4 mm. Keep almost all text at `9*s`; only a title
readout goes bigger. If text feels big, you're probably not multiplying by `s`.

## 3. Palette (already correct — keep)

- display bg `#1A1A2E`; inset stroke `#404060`
- primary blue `#0097DE`; darker structural blue `#0D5986`/`#0D5988`
- accent orange `#EC652E` (playhead / current / active-emphasis)
- rail / connector `#0D5988`; dim purple `#35354D`; dimmer `#2A2A3E`
- text bright `#E6E6F0`; label grey `#8A8AAA`; white `#FFFFFF` on the focused cell
- Font: `res/fonts/ShareTechMono-Regular.ttf` (VCV ignores SVG `<text>` — draw labels in NanoVG).

## 4. Interaction conventions (the "how you edit it" fix)

Match Note/Beat, not scroll-only editing:

- **Click a cell** = select / toggle it (pattern-select, step on/off, tab).
- **Click + drag** = paint or scrub: dragging across a run of cells paints them the
  same new state (steps), or drags a value up/down (velocity/probability), or scrubs
  a selection/length across boxes. Track a `dragKind` + `dragPos` (accumulate
  `e.mouseDelta`); use `hitTest…` helpers to map a point to a cell/col/row.
- **Length / count / repeats** = a row of **boxes** (Note's length dots, Beat's
  repeats bar): click box *i* sets the value to *i+1*; drag across the row scrubs it.
  Prefer this over scroll-wheel for a primary value.
- **Double-click** = the secondary toggle (e.g. pattern active/inactive) — reuse the
  last press position since `DoubleClickEvent` carries none.
- **Scroll** = a *secondary* nudge on the hovered cell, never the only way to set a value.
- Focus vs play: **focused/edited** cell = blue fill / light inner border; **playing**
  cell = orange border + progress; **inactive** = dim.
- `drawLayer(layer==1)` for the glowing display; provide a `module==nullptr` browser
  preview (either `drawPreview()` or an inline demo in the null branch).

## 5. Checklist for a new/edited screen

1. `float s = box.size.x / DESIGN_W;` with `DESIGN_W = mm * 3.783`. No `box.size.*`
   fractions for sizing.
2. All fonts `9*s` (title 11–13*s). All rects in design-units*s.
3. Primary values edited by **click + click/drag on boxes/cells**, scroll only as a nudge.
4. Palette + font per §3. Playhead orange, focus blue, inactive dim.
5. Browser preview branch for `module == nullptr`.

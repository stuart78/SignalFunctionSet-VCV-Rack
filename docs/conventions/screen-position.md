# Display (screen) position convention

Any module with an on-panel NanoVG display ("screen") places its **top edge at
`y = 12 mm`** from the panel top. The band above it (`y = 0 … ~11 mm`) is the
header: the module title (drawn by VCV from the panel SVG `<text>`, or by hand)
and, where used, the SFS wave/dots glyph in the top-right.

This keeps the screen lined up across modules so they read as one family when
placed side by side.

## The rule

- **Screen top edge: `y = 12.0 mm`.** Use this for every new module.
- Left edge / width are per-module (panel width dependent); only the top edge is
  fixed by this convention.
- The display box in the widget (`disp->box.pos.y`) **must equal the screen
  rectangle in the panel SVG** (`<rect ... y=...>`, in viewBox units → mm by
  `units / 2.83465`). If they disagree the live screen won't sit inside the
  drawn bezel.

## Current modules

| Module   | screen top (mm) |
|----------|-----------------|
| Note     | 12.0  |
| Operator | 12.0  |
| Beat     | 12.2  |
| OP ENV   | 12.33 |

Beat (12.2) and OP ENV (12.33) predate this note and sit a hair low; new work
should use **12.0**, and those two can be nudged to 12.0 when their panels are
next revised.

## Why a rule

Screen Y had drifted (9–12.3 mm) as panels were hand-laid-out. Fixing it at
12 mm removes the guesswork and the side-by-side misalignment.

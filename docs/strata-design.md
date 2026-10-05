# Strata: a wavetable voice you travel through

Design written 2026-10-02. Strata performs what WaveStack builds: WaveStack is
where a table is drawn, keyed and exported, and Strata is where it is played,
sequenced and shaped. Built 2026-10-02 (`src/strata.cpp`, `tools/strata-harness.cpp`); hidden.

## The idea

A wavetable is a set of single-cycle frames. WaveStack already arranges them in
a 2D grid (8 x 8 by default). Stack several grids and the table becomes a
**volume**: X is the column, Y the row, Z the layer. Strata is an oscillator
whose sound is one point in that volume, and everything it does is about where
that point is and how it moves:

- **Navigate**: X, Y and Z knobs and CVs place the point.
- **Sequence**: a clocked list of waypoints moves it, gliding between them.
- **Morph**: between grid points the frames are blended, never stepped.
- **Shape**: live, CV-able macros bend whatever frame the point is reading.

Editing stays in WaveStack. Strata has no frame editor.

## The data

- **Frame size 2048.** Other sizes are resampled to 2048 at load (FFT, as
  WaveStack does), so the engine has one size.
- **Volume**: cols x rows x layers, each up to 16, at most **1024 frames**
  total (8 x 8 x 16). With the mipmaps that is about 35 MB, built on a loader
  thread as Play loads samples, and silent until ready.
- **Mipmaps**: every frame band-limited per octave by FFT (Wave's code), so it
  is alias-free at any pitch.
- **Layers can come from one file or several.** A WaveStack 3D export carries
  the whole volume; loading several 2D files (or a folder) stacks them as
  layers in name order. A 1D file loads as one row, or squares up when its
  frame count is a perfect square and the menu says so.

## The file format: WAV with a `wt3d` chunk

One file, so a volume travels as one thing, and a WAV, so any wavetable synth
can still open it (Serum and Surge read all its frames as one long 1D table).

- **Samples**: mono, 32-bit float (16/24-bit accepted on read), frames
  concatenated in the order **layer, then row, then column** (column fastest).
- **`clm ` and `srge`**: written as WaveStack writes them today, so other
  synths know the frame size.
- **`wt3d`** (wavetable dimensions): the table's shape. It's an open format
  (CC0), specified in WaveStack's `docs/wt3d.md`, which also ships reference
  files that the harness reads. In short, little-endian:

  | Offset | Type | Field |
  |---|---|---|
  | 0 | u32 | version (1) |
  | 4 | u32 | frame size |
  | 8 | u32 | cols |
  | 12 | u32 | rows |
  | 16 | u32 | layers |
  | 20 | u32 | flags, hints (bit 0 normalized, bit 1 DC removed, bit 2 zero-aligned) |
  | 24 | u32 | name block length N (0 for none) |
  | 28 | N bytes | layer names, UTF-8, each NUL-terminated |

  Row 1 is the top. A reader uses the first `wt3d` only, checks (without
  overflow) that frame size x cols x rows x layers equals the sample count and
  falls back to 1D if it does not, and then trusts the grid's frame size over
  `clm `. Later versions only append fields, so they are read as far as
  version 1's. Layer names keep their places and are dropped unless there is
  one per layer. The flags tell Strata which conform steps the file already
  had, so it does not repeat them.

- A 2D table is the same chunk with layers = 1, so this replaces any need for a
  separate 2D grid chunk. WaveStack writes it (since 2026-10-02) on Serum-style
  WAV exports and library saves of 2D tables; Plain WAV stays untagged and
  device exports are unchanged. Other synths ignore unknown chunks. WaveStack
  opens a file carrying the chunk as the same grid (a 3D file opens with its
  layers stacked as extra rows until WaveStack has 3D tables).

## Navigation

- **X, Y, Z knobs with polyphonic CVs** (1 V per cell, so a sequencer's
  integer volts land on grid points).
- **Wrap or clamp at the edges** (menu, per axis). Wrapping X and Y makes the
  grid a torus, as OP MORPH's field is; clamping is the default.
- **Position outputs X, Y, Z** (the point actually being read, after the
  sequence, the glide and the CVs), so other modules can follow it.

## The sequence

The sequence and the index slew are one feature: **a list of waypoints, and a
glide between them.**

- **Up to 16 steps**, each a point (x, y, z), usually a grid cell. Set on screen:
  click a frame to add it as the next step, click a step's marker to select it,
  drag it to move it, right-click to remove it.
- **CLOCK advances, RESET returns to step 1.** Unpatched, the sequence holds
  and the knobs place the point.
- **CVs add on top** of the sequenced position, so a sequence can be
  transposed through the volume.
- **GLIDE is rate-based by default**: the point crosses the volume at a steady
  speed, so a jump across the table takes longer than a hop to the next cell.
  That is what makes it travel rather than crossfade (it is why Slide's bar
  sounds like a slide). Time-based glide (every move takes the same time) is
  the menu alternative. A glide that has not finished when the next clock comes
  continues from where it is.
- **PATH**: straight through the volume, or along the grid lines (one axis at a
  time, so it passes through the in-between frames rather than blending across
  them). They sound different, which is the reason to offer both.
- **Step gate out**: a trigger as each step begins, for envelopes.

## Morphing

- **Crossfade (default)**: trilinear blend of the eight frames around the point,
  per mip level. Cheap, exact on grid points, polyphonic.
- **Spectral (menu)**: blend the eight frames' spectra (magnitude and
  phase-aligned) and resynthesize a frame at control rate (every 64 samples,
  with a crossfade between resyntheses). No phasey middle between unlike
  frames, at about 1-2% of a core per voice, so it is offered for few voices.
  The exact voice limit is set by measurement.

## Conform (static, at load)

Making unlike frames behave alike. These are load options in the menu, applied
once and cached, and skipped when the file's `wt3d` flags say they are done:

- **Normalize each frame** (peak or RMS), so morphing does not jump in level.
- **Remove DC.**
- **Align zero crossings**, so frames do not click against each other.

## Shape (live, with CV)

Bending the frame being read, while playing. This is where the panel goes:

- **WARP**: phase distortion, bending the read position (Casio-style).
- **FOLD**: wavefolding after the read.
- **TILT**: spectral brightness, through the mip level and a tilt filter.
- **SYNC**: internal hard sync at a ratio of the pitch, band-limited.

Each needs its own aliasing answer (WARP and SYNC change the read phase, FOLD
adds harmonics), so each is measured before it ships. What was built:

- **WARP and SYNC: a linear-phase BLEP and BLAMP** (`StrataBl`, windowed sinc,
  16 zero crossings, 0.33 ms of delay). The warp knee, every slave wrap under
  warp, and a sync restart on a smooth frame are changes of SLOPE, not steps, and
  a minBLEP corrects only steps. Rack's minBLEP could not simply be integrated:
  it is minimum phase, so its step lags the ideal one and its integral ends 3.9
  samples of slope short of zero; truncating that leaves a step. A symmetric
  kernel's residuals end at zero. Every event goes in at its sub-sample position.
- **FOLD: twice the rate, plus the antiderivative.** A triangle folder puts
  corners in the wave, and a bright frame's edge crosses more than a whole fold
  period inside two samples, so the alias is genuinely near Nyquist. The
  antiderivative alone (first-order ADAA, exact for the straight line between
  samples) bought 7 dB; folding at 2x through a 47-tap Kaiser halfband pair
  bought the rest. The pair runs even at FOLD 0, where it is flat, so the
  latency never switches; the ADAA's gentle lowpass fades in over the first
  tenth of the knob.
- **The mips hold at least 8 samples per cycle of their top harmonic.** At 4,
  linear interpolation's images sat 19 dB under that harmonic and folded back.
- **A 2 Hz DC blocker** on the output: WARP spends unequal time either side of
  zero and FOLD is not symmetric about it.

Measured on the brightest factory frame (99 harmonics), energy off the
harmonics and below 20 kHz (above it is the halfband's transition folding
back, inaudible):

| | 110 Hz | 440 Hz | 1760 Hz | 3911 Hz |
|---|---|---|---|---|
| plain | -67 | -59 | -67 | -75 |
| WARP 0.8 | -60 | -59 | -64 | -38 |
| SYNC 4.5x | -58 | -62 | -56 | -32 |
| FOLD 0.5 | -44 | -38 | -43 | -44 |

The weak corner is heavy sync or warp at a high pitch: 4.5x sync on 3911 Hz puts
the slave at 17.6 kHz, where every derivative of a restart jumps about as far as
the one before it and no finite series of corrections converges. FOLD is the
next thing to improve if it needs it (4x).

## Voice and panel

- **Polyphonic** V/OCT (16 voices), linear through-zero FM in with an amount,
  and a SYNC input.
- **Outputs**: poly audio, X/Y/Z position, step gate.
- About 22 HP. The panel goes to Figma through the template, with a reference
  layer of the screen as Carillon's had.

## The screen: WaveStack's 3D view

Ported from WaveStack's `Waterfall.svelte` and `camera.svelte.ts`:

- **The camera**: yaw and pitch, the same `rotate()` (yaw about the vertical,
  then pitch down), drag to orbit, double-click to reset, angle saved with the
  patch. World axes as WaveStack has them: x = phase along each frame, y =
  level, z = depth.
- **A layer is drawn as WaveStack draws a 2D table**: columns side by side
  along x, rows receding in depth, row 1 at the back, painted back to front, and
  each frame hanging a curtain that hides what is behind it.
- **Layers stack vertically**, the current one solid and the others faint, so
  the whole volume is visible at once (the complaint about WaveStack's 3D view
  was not being able to see adjacent rows and columns).
- **The point is a frame drawn in the accent colour** at its exact position, and
  the **sequence is a path** through the volume with a marker on each step.
- Screen colours are the plugin's display palette (blue traces, orange point),
  in Share Tech Mono, and the browser thumbnail is a real warmed-up instance
  (`src/preview.hpp`).

## WaveStack's side

Work in the WaveStack repo, separate from the module:

1. **Write `wt3d`** on every 2D WAV export (layers = 1). Small, and it fixes the
   2D grid being lost on export today.
2. **A 3D layout**: a document of cols x rows x layers, with the 3D view drawing
   the stacked layers as described above. This is the larger change.
3. **A "Strata" export target** that writes the 3D WAV.

The format is defined here first, so both sides are built and tested against
the same files.

## What to measure before believing it

- `wt3d` round-trip: WaveStack writes, Strata reads, every frame bit-exact and
  in the right cell; a malformed chunk falls back to 1D.
- Grid points are exact: at an integer X/Y/Z the output is that frame and
  nothing else.
- Glide: rate-based travel time proportional to distance; a re-clocked glide
  continues without a jump.
- Aliasing per shape macro at high pitch.
- Spectral morph: CPU per voice, and no level dip between unlike frames.
- Cost at 16 voices, crossfade mode.

## Wave

Strata does not replace the hidden Wave module (live shape plus snapshot FIFO):
both stay (decided 2026-10-02). Strata reuses Wave's band-limited mipmap code.

## Second round (2026-10-03)

- **FOLD drives to 12x**, exponentially (`12^fold`); the old `1 + 3·fold` top
  folded each half-cycle about once. The folder now runs at **4x** (a second
  halfband stage) because 12x at 2x measured -27 dB of alias at 440 Hz. What is
  left at full FOLD (-32 dB at 440 Hz on the brightest frame) is the input's
  interpolation floor multiplied by the folder's slope, not aliasing of the fold.
- **Factory tables in 1D, 2D and 3D**, and every factory layer is a named
  source (`factory:harmonics` ...), so a factory table is a list of sources and
  a factory layer stacks on anything with an 8 x 8 grid.
- **Add a layer on top**: appends a source; refused, table kept, when the grids
  differ. To make "on top" literal, **layer 1 is now the bottom** of the cube.
  Stacks keep the order given; only a folder is sorted (by name).
- **CLOCK** steps through waypoints clicked onto the screen. The panel never
  said so; the tooltips, the menu and the foot of the screen now do.
- **L/R mix with two listeners** in the cube. Each voice is `1/(1 + 1.5 d²)` loud
  at each listener (d in world units, the cube 2 wide), slewed 5 ms, summed and
  soft-clipped. A one-layer table is a plane. Measured: the first column leans
  6.2 dB left, the last 6.2 dB right, with the listeners at their defaults.

## Third round (2026-10-03): a second row

- **RATE and START corrupt the read.** The table is read as one stream of
  frames in file order (wrapping at its end); a cycle reads RATE frames' worth
  (4^rate: 0.25x to 4x, 1x correct) starting START frames along (0 to 1). It is
  the classic wavetable error of loading with the wrong frame size, as a
  control: the cycle neither starts nor ends where a frame does, and a long read
  crosses into the next frames. Each seam is a step and a slope change and goes
  through the BLEP/BLAMP corrector at its sub-sample position, so the result is
  measured at -50 to -81 dB off-harmonic (110 to 1760 Hz). At 1x and 0 every
  formula reduces to the old read, and the old measurements are unchanged.
- **ROT X / Y / Z** turn the listener pair about the centre of the cube, applied
  after the dragged positions (vertical, then X, then depth). Half a turn about
  the vertical swaps the image: measured, the first column then leans 6 dB right.
- **Panel**: the screen gives up 9 mm (47 mm) for a second row of trimpot over
  jack pairs. Row 2 uses five of row 1's columns: RATE and START under FREQ and
  FM, the turns on the right over the L/R outputs.

## Fourth round (2026-10-03): CRUSH and SHUFFLE

- **CRUSH**: bit depth, 12 bits down to 1 with the step continuous, after FOLD
  at 4x and anti-aliased by its antiderivative (the staircase integrates to a
  line between edges). Measured -57 to -71 dB off-harmonic.
- **SHUFFLE**: the cycle cut into 2, 4 ... 64 pieces and reordered, crossfading
  between levels. The order is a tree of coin flips seeded from the patch (each
  depth swaps the halves of each piece or not; the first cut always swaps), so a
  finer level only reorders inside the coarser level's pieces and the knob
  deepens one scramble instead of jumping between unrelated ones. "New shuffle
  order" re-rolls the seed. Swapping the halves of a sine returns the inverted
  sine to -82 dB.
- Every cut is a seam through the BLEP/BLAMP corrector, found lazily per sample.
  A cut shorter than a sample cannot be band-limited (64 pieces at 1760 Hz
  measured -28 dB), so **the finest levels give way on high notes**, the way the
  mips thin harmonics: no piece shorter than 6 samples. After that, -58 to -75 dB.

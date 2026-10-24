# Strata Manual

## Overview

Strata is a wavetable voice you travel through. Its table is a volume of
single-cycle frames, arranged in columns, rows and layers, and the sound is one
point in that volume. X picks the column, Y the row and Z the layer. Between
grid points the frames are blended, never stepped, so moving the point is a
continuous change of timbre.

Everything on the panel is about where that point is, how it moves, and what
happens to the frame it reads:

- **Place it** with the X, Y and Z knobs and CVs.
- **Move it** through a list of waypoints, clocked, gliding between them.
- **Shape it** with WARP, FOLD, TILT and SYNC.
- **Corrupt the read** with RATE, START, CRUSH and SHUFFLE.
- **Hear it in space** through two listeners placed inside the volume.

Strata is polyphonic (sixteen voices) and 24HP.

Strata plays tables; it does not edit them. Tables are drawn and arranged in
WaveStack.

## The table

A table is up to 16 columns, 16 rows and 16 layers, and at most 1024 frames in
all. Every frame is 2048 samples; a table with another frame size is resampled
to 2048 when it loads. Each frame is band-limited once per octave when the
table loads, so it plays without aliasing at any pitch. Loading runs in the
background, and the voice stays on the previous table until the new one is
ready.

### Where tables come from

- **A WaveStack export.** WaveStack writes the table's shape into the WAV as a
  `wt3d` chunk (an open format, specified in WaveStack's `docs/wt3d.md`), so a
  3D table opens with its columns, rows and layers in place and its layer
  names shown on the screen. Other synths read the same file as one long row.
- **Any other wavetable WAV** opens as a single row. If its frame count is a
  perfect square and **Open square 1D tables as grids** is on (Conform at
  load), it opens as a square grid instead.
- **A folder, or several files at once,** opens as layers. Layer 1 is at the
  bottom of the cube. Files load in the order given; a folder is sorted by name
  first. The files must share a grid.
- **Factory tables:** a 1D row of sixteen shapes, a 2D table of harmonics
  (8 x 8), and a 3D stack of four 8 x 8 layers (harmonics, formant, sync and
  pulse). A new Strata opens on the 3D stack.

**Add a layer on top** (menu) stacks a WAV or a factory layer onto the current
table. A layer with a different grid is refused and the table is kept.
**Remove the top layer** removes the last one.

**Conform at load** can normalize each frame, remove DC and align zero
crossings. It applies to the next table you load.

## Playing

**FREQ** sets the pitch, four octaves either side of middle C. **V/OCT** is
polyphonic: each channel is a voice.

**FM** is linear and through zero, so deep modulation runs the cycle backwards
rather than stalling it. The knob sets the depth, and the FM input is
polyphonic.

### X, Y and Z

The knobs span the table. The CVs add **1 V per cell**, so a sequencer stepping
in whole volts steps cell by cell. All three CVs are polyphonic, so each voice
of a chord can read its own part of the table.

At an edge, each axis either clamps or wraps (menu, **Edges**). Wrapping X on a
table whose last and first columns are related lets the point circle the table
without a jump.

The **X, Y and Z outputs** report where each voice is reading, 1 V per cell,
polyphonic. Patch them to another module to make it follow the point.

## Waypoints

Click a frame on the screen to add it as a waypoint, up to sixteen. **CLOCK**
moves to the next waypoint and **RESET** goes back to the first. **STEP** fires
a trigger as each waypoint begins.

**GLIDE** sets how long the point takes to travel from one waypoint to the
next. Two ways to measure it (menu, **Glide**):

- **Rate** (default): the point moves at a steady speed, so a long move takes
  longer than a short one, like a hand crossing a keyboard.
- **Time**: every move takes the same time, however far it goes.

**Path** (menu) sets the route: straight through the volume, or along the grid
lines, one axis at a time.

Remove the last waypoint or clear them all from the menu, or by right-clicking
the screen.

## Shaping the frame

| Control | What it does |
|---|---|
| **WARP** | Bends the cycle's phase. Positive runs through the first half of the frame quickly and the second half slowly; negative the reverse. |
| **FOLD** | Folds the wave back on itself, driving it into the folder at up to twelve times its level. |
| **TILT** | Darker to the left, brighter to the right. Down, it reads the frame with fewer harmonics (up to three octaves fewer); up, it adds a high shelf. |
| **SYNC** | Runs the frame 1x to 8x against the pitch, restarting it every cycle, for the sound of a synced oscillator. |

**HARD** is a hard sync input, polyphonic: a trigger restarts that voice's
cycle.

## Corrupting the read

The second row breaks the assumption that one cycle reads exactly one frame.

| Control | What it does |
|---|---|
| **RATE** | How many frames one cycle reads, from a quarter to four. At 1x (centre) the read is correct; away from it a cycle starts in one frame and ends in another. |
| **START** | Where the read starts along the table, up to one frame. |
| **CRUSH** | Reduces the bit depth, from 12 bits down to 1. |
| **SHUFFLE** | Cuts the cycle into 2 to 64 pieces and reorders them. Turning it up cuts finer; the pieces of a finer cut stay inside the pieces of the coarser one. |

The table is read as one stream of frames in file order, wrapping at the end,
so RATE and START carry the read across frame boundaries. **New shuffle order**
(menu) chooses another reordering; the order is saved with the patch.

On high notes SHUFFLE's finest cuts give way, as the band-limited frames do: a
piece shorter than about six samples cannot be played cleanly.

## The screen

The screen is split in two.

**The left side** draws the wave each voice is playing, large: the blended
frame faintly, and in orange what WARP, SYNC and FOLD make of it. One voice
gets the whole pane; more voices share it, up to a grid of sixteen.

**The right side** is the table as a cube. Columns run across, rows run into
the screen and layers stack upward, and the axes are labelled X, Y and Z with
their first and last cell. The layer the first voice is reading is drawn
brightest, fading into the layers either side. Each voice's point is drawn in
orange, the waypoints as a path of dots, and the two listeners as rings marked
L and R.

- **Drag** an empty part of the cube to turn it. **Double-click** resets the
  view.
- **Click** a frame to add it as a waypoint.
- **Drag** a listener to move it across the floor; **shift-drag** moves it up
  and down (3D tables only).

## Stereo

The **OUT** jack is the polyphonic voice signal, one channel per voice.

**L** and **R** are every voice mixed, heard from two listeners inside the
cube. A voice is louder at a listener the nearer its point is: about 3.5 dB
down a third of the way across the cube, 10 dB down at the far side. As the
point moves, the voice moves between the two outputs.

**ROT X, ROT Y and ROT Z** turn the pair of listeners around the centre of the
cube, about the vertical first, then the X axis, then the depth. The CVs cover
a full turn either way. A dragged listener lands under the pointer whatever the
turn.

On a one-layer table the cube is a plane, and the listeners' height is ignored.

## Inputs and outputs

**Inputs:** V/OCT, FM, X, Y, Z (all polyphonic), HARD (polyphonic), CLOCK,
RESET, and a CV for GLIDE, WARP, FOLD, TILT, SYNC, RATE, START, CRUSH,
SHUFFLE and ROT X, Y and Z.

**Outputs:** OUT (polyphonic), X, Y, Z (polyphonic, 1 V per cell), STEP, L, R.

## Context menu

- Load table, load a folder of tables as layers
- Factory tables (1D, 2D, 3D)
- Add a layer on top (from a WAV or a factory layer), remove the top layer
- Conform at load: normalize, remove DC, align zero crossings, open square 1D
  tables as grids
- Waypoints: remove the last, clear
- Glide: rate or time
- Path: straight or along the grid
- Edges: clamp or wrap, per axis
- New shuffle order
- Reset the listeners

## Patch ideas

**A chord across the table.** A poly V/OCT into Strata, and a poly CV into X
with a different voltage per channel. Each note of the chord reads its own
column.

**A clocked tour.** Click six or eight frames on the screen, clock CLOCK from
Count or Meter, and set GLIDE to about half. In Rate mode the far jumps take
noticeably longer than the near ones.

**Stereo from position.** Drag the listeners to opposite corners and sweep X
slowly with an LFO. The voice moves from one output to the other as it crosses
the table.

**Broken tape.** RATE a little off centre, START slowly modulated, and a touch
of SHUFFLE. The cycle no longer lines up with any frame, and the table smears.

**Follow the point.** The X, Y and Z outputs into another module's CVs, so a
filter or a second oscillator moves with the timbre.

## CPU

About 0.3% of a core for one voice and 2.5% for sixteen on an Apple M4. FOLD and
CRUSH run at four times the sample rate, so with every shaping control turned up
sixteen voices use about 15%. With FOLD and CRUSH at zero that stage is
replaced by an equivalent filter at the normal rate.

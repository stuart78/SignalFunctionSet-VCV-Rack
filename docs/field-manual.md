# Field Manual

## Overview

Field is nineteen random voltages that are one thing.

The nineteen jacks sit on a hexagonal grid: one at the centre, a ring of six
and a ring of twelve. Under the grid is one smooth random field, a surface in
x, y and time, and each jack simply reads the field at its own position. Every
control is a statement about the field, and the relation between the jacks
falls out of geometry: two neighbours agree because they are sampling nearby
points of the same surface, and how nearby is what COHERENCE means.

There is no screen. The plate behind the jacks is tiled with hexagonal cells,
each coloured by the field at its centre, from the same functions the outputs
read, so what you see under a jack is what comes out of it.

Field is 16HP.

## Controls

Each control has a trimpot and a CV input beneath it; plus or minus 5 V covers
the whole range of the knob.

- **AMP** is the range of the outputs, 0 to 10 V peak. The field is bipolar,
  so at 5 V the jacks swing between -5 V and +5 V.
- **OFFSET** shifts every output, -5 V to +5 V. AMP 5 V with OFFSET 5 V gives
  0 to 10 V.
- **COHER** (coherence) is how far across the grid the field agrees with
  itself. At zero, neighbouring jacks are unrelated and Field is nineteen
  sample-and-holds. Turned up, a pattern spans several jacks; at the top the
  whole plate rises and falls nearly together. The range was measured on the
  grid's own distances, so the bottom of the knob is genuinely uncorrelated
  and the top is genuinely one shape.
- **RATE** is how fast the field changes in time, 0.002 Hz (a cycle every
  eight minutes) to 20 Hz, exponential. The tooltip reads the period as well
  as the frequency.
- **FLOW** moves the field across the grid, 0 to 4 jacks per second, and
  **DIR** says which way, 0 to 360 degrees. With FLOW up, a pattern appears at
  one edge and travels to the other, so the jacks fire in a spatial order
  rather than as unrelated sources.
- **BIAS** tilts the amplitude by radius. Positive is centre hot and rim
  quiet; negative is the reverse. It is what the rings are for.
- **SMOOTH** slews every output, 0 to 2 s.
- **ANIM** picks the animation, which is what FLOW and DIR move. The CV input
  adds one mode per volt to the knob.

## TRIG: sample and hold

With nothing in TRIG the outputs follow the field continuously. Patch a
trigger and every jack holds its value until the next one. A polyphonic TRIG
holds jack N from channel N, so each output can be clocked on its own; a mono
trigger holds all nineteen at once.

## Animations

FLOW is always the speed and DIR always the direction, but what they move
depends on the animation.

- **Flow** slides the window across the field.
- **Zoom** scales the field: DIR at 0 radiates outward from the centre, at
  180 it draws inward.
- **Spin** rotates it: 0 clockwise, 180 anticlockwise.
- **Random** gives each jack its own window, so they drift independently.
- **Life** is a cellular automaton on the hexagonal tiling, stepping on RATE.
  A dead cell with two live neighbours is born and a live one with three to
  five survives, with a trickle of random births so it never freezes.
- **Rain** and **Wave** are one machine, a damped wave equation on the cell
  graph, driven two ways: drops that dent the surface and ring outward, or
  crests launched from the edge facing DIR that cross the plate and reflect.
  RATE is the wave speed, FLOW how often, and COHERENCE how far a wave travels
  before it dies.
- **React** is Gray-Scott reaction-diffusion: two chemicals, spots that split
  and stripes that grow. COHERENCE walks along the line of feed and kill rates
  that makes patterns, from spots to stripes to worms.
- **Cyclic** is the rock-paper-scissors automaton, which organises itself into
  spirals. COHERENCE sets how many states are in the cycle.
- **Sand** is the Bak-Tang-Wiesenfeld sandpile: grains dropped until a cell
  topples onto its neighbours, with avalanches of every size.
- **Worley** is a set of drifting seed points, each cell reading its distance
  to the nearest, so the plate is a moving mosaic with ridges between the
  cells.

Whatever the animation, the outputs are the picture: a jack reads the cell it
sits on.

## Outputs

- **Nineteen jacks** on the hexagonal grid, each the field at that point.
- **POLY** carries jacks 1 to 16 on one cable, numbered from the centre
  outward.

## Context menu

- **Colour**: the palette the plate is drawn in. Seven two-colour pairs and
  Rainbow. The choice is saved with the patch and changes nothing about the
  outputs.

## Tips

- **A drum kit that breathes.** Nineteen slow, related modulations into a
  kit's per-instrument CVs, COHER high and FLOW low, so the whole kit leans
  one way and then the other rather than each drum wandering alone.
- **Spatial sequencing.** FLOW up, DIR set, TRIG from a clock: the held values
  sweep across the grid in order, so jacks patched to a row of voices fire
  as a wave.
- **A sample-and-hold bank.** COHER at zero, TRIG polyphonic from several
  clocks, and every jack is its own S&H.
- **Weather on a voice.** Rain into filter cutoff, resonance and level of one
  voice from three neighbouring jacks: each drop hits all three, a little
  apart in time.

# Cistern Manual

## Overview

Cistern is a reverb modelled on the shapes of real places. You choose a space,
place two sources and a listener in it on the screen, and Cistern works out how
sound would reflect around that shape, at that size, from where everything
stands.

It is a model, not a recording of the room. The geometry is simplified, the
surface materials are estimates, only the first part of the reverb is traced,
and the decay times are calculated rather than measured in the buildings. Treat
the rooms as large spaces of the right shape and size with a plausible
character, not as reproductions of what it sounds like to stand in them.

It is named for the Dan Harpole Cistern at Fort Worden, Port Townsend: an empty
concrete water tank 57 m across and 4.3 m high, its roof on a grid of concrete
columns, where Pauline Oliveros, Stuart Dempster and Panaiotis recorded *Deep
Listening* in 1988.

Cistern is stereo in, stereo out, and 22HP.

## How it works

Three parts, each worked out from the room:

1. **Early reflections, traced.** On a background thread, rays leave each
   source, bounce off the room's surfaces, lose energy at each surface and in
   the air, and are caught at the listener. What arrives in the first part of
   the reverb becomes a set of delays, one per arrival time, in three bands.
   The listener hears through a pair of cardioid microphones, so the direction
   a reflection arrives from places it in the stereo image.
2. **The tail, from the physics.** A 16-line feedback delay network produces
   the later part of the reverb. Its decay at every octave is set to the room's own, worked out from
   the room's volume, its surfaces and the air in it.
3. **The handover.** The tail starts where the traced reflections end, at a
   level matched to them, so the join is not heard.

Retracing takes from a fraction of a second to about a second. While it runs
the screen says **TRACING** and the previous version keeps playing.

## The spaces

| Space | Kind | Decay at 500 Hz |
|---|---|---|
| Dan Harpole Cistern, Fort Worden | built | about 21 s |
| Buffalo Bayou Cistern, Houston | built | about 20 s |
| Basilica Cistern, Istanbul | built | about 18 s |
| The Pantheon, Rome | scan | about 13 s |
| Hintze Hall, Natural History Museum, London | scan | about 5 s |
| Leake Street Tunnel, London | scan | about 8 s |
| Old Church | scan | about 4 s |
| Lincoln's Inn Chapel Undercroft, London | scan | about 2 s |
| St Bartholomew-the-Less, London | scan | about 4 s |
| St Peter, Stourton | scan | about 2 s |
| Mosque of the Tree, Jordan | scan | about 1 s |
| Salt Tower, Lower Room, Tower of London | scan | about 2 s |

Decay times are at the default SIZE and DECAY. Where the sources and the
listener stand changes the early reflections, not the decay.

**Built** spaces are made from published dimensions. The Dan Harpole Cistern
is a cylinder on a grid of square columns with flared capitals under a beam
grid; the Buffalo Bayou Cistern is 221 columns over a water floor; the Basilica
Cistern is 336 columns under brick cross vaults, also over water. Where a
detail (column spacing, sizes) could not be found, it is an estimate.

**Scans** are photogrammetry models of the real rooms, reduced to the air
inside them, with openings (an oculus, a tunnel's open end, an arcade) kept
open so sound leaves through them. Surface materials are assigned by the kind
of surface (floor, wall, ceiling) and the kind of building, and are estimates
rather than measurements.

Water stays a mirror: a water floor reflects without scattering, whatever
SCATTER says.

## Placing the sources and the listener

The screen draws the space at its true proportions: the built cisterns as
their edges, the scans as architectural sections (vertical cuts across the
room, one along it, and its footprint).

- The two **orange** dots marked **L** and **R** are the sources: the L input
  is played from one and the R input from the other. The **white** dot is the
  listener, a pair of microphones angled toward the sources, and the L and R
  outputs are what those microphones pick up. (In Strata, L and R are the
  listeners; here they are the sources.)
- **Drag** a source or the listener to move it across the floor.
  **Shift-drag** lifts it. Nothing can be moved into a wall.
- **Drag** an empty part of the screen to turn the view. **Double-click**
  resets the view.
- Positions are kept separately for every space and saved with the patch.
  **Reset the positions** (menu, or right-click the screen) restores the
  space's defaults.

The **L** input is the L source and the **R** input is the R source. R is
normalled to L, so a mono signal comes from both sources at once.

### Distance

By default distance is **musical**: past 25 m, extra distance counts for less,
so a source at the far end of a 300 m tunnel arrives late enough to sound far
away without arriving too late to play with (150 m is heard as about 65 m).
Choose **True** in the menu for the delay and level as the model calculates
them.

## Controls

| Control | What it does |
|---|---|
| **SPACE** | Chooses the room. The CV is 1 V per space. |
| **SIZE** | Scales the room, from a quarter to twice its real size. The tooltip reads the result in metres. |
| **DECAY** | Sets the decay from a tenth to four times the room's own. The tooltip reads the result in seconds at 500 Hz. |
| **SCATTER** | How much of each reflection goes diffuse instead of bouncing like a mirror. Low, a curved wall focuses sound back at you (listen for it in the round cistern); high, the room is smooth. Default 50%. |
| **HUMID** | The humidity of the air, 10% to 90%. Drier air absorbs more high frequencies, and over the 15 km a sound travels in a 45-second tail that is most of what darkens it. |
| **MOVE** | Slow movement in the tail's delays, so a long tail does not settle into fixed resonances that sound metallic. |
| **MIX** | Dry to wet. |
| **FREEZE** | Holds the tail indefinitely and shuts the input out of it. The gate input does the same while it is high. |

## Changing space

A new space starts a new reverb, and **the old room keeps ringing in its own
acoustic** until it falls silent. Nothing already sounding changes pitch or
character; the old tail simply dies away under the new room. A long room rings
for a long time: after the Dan Harpole Cistern the old tail lasts over a
minute before it is dropped.

Moving a source or the listener, or changing SIZE, SCATTER or HUMID, retraces
the same room. The early reflections crossfade over 40 ms and the tail carries
on, so nothing is cut off. A DECAY change reuses the trace and is quicker.

## The screen's readouts

- **Top left:** the space's name. **Top right:** its size and volume, and the
  **CREDITS** button, which lists every scanned space with its author, link and
  licence. Any click closes the list.
- **Bottom left:** the decay time at each octave from 125 Hz to 4 kHz.
- **Bottom right:** **TRACING** while a new trace runs, **FROZEN** while
  FREEZE is on.

## Inputs and outputs

**Inputs:** L, R (normalled to L), FREEZE gate, and a CV for SPACE, SIZE,
DECAY, SCATTER, HUMID, MOVE and MIX.

**Outputs:** L, R.

## Context menu

- Space
- Distance: musical or true
- Reset the positions
- Reset the view
- Scan credits

## Patch ideas

**Deep Listening.** The Dan Harpole Cistern, MIX fully wet, a long sustained
tone into L. Move the listener toward the wall and back; the curved wall
focuses sound at certain spots, more so with SCATTER low.

**Two players in one room.** Two voices into L and R, the sources dragged to
opposite ends of Hintze Hall. Each keeps its own place in the stereo image, and
the room joins them.

**Down the tunnel.** Leake Street, the source near one end and the listener
near the other. Switch Distance to True to hear how late and quiet a source
300 m away really is.

**A room that grows.** A slow LFO on SIZE. The room swells and shrinks under
the sound without cutting the tail.

**Freeze and play.** Feed a chord, hit FREEZE, and play over the held room.

## CPU

On an Apple M4, about 3.6% of a core in the Dan Harpole Cistern with sound
playing continuously, and 2.2% to 2.7% in every space but one when the input
has gaps (the early reflections are skipped once the input has been silent
long enough). Leake Street, whose length gives it far more reflections, uses
up to about 9%. After a SPACE change the old room adds the cost of its tail
until it falls silent. Tracing runs on separate threads and is not counted in
Rack's meter.

## Scan credits

The scanned spaces are photogrammetry models licensed under Creative Commons
Attribution 4.0 (https://creativecommons.org/licenses/by/4.0/), converted to
room geometry for Cistern (reduced to the air inside, smoothed, materials
assigned; the Pantheon scaled to its real 43.3 m and its oculus re-opened,
Leake Street scaled to its real length).

- "The Pantheon Interior" (https://skfb.ly/pJOwI) by artfletch
- "Hintze Hall" (https://skfb.ly/oF9N6) by artfletch
- "Leake Street Graffiti Tunnel" (https://skfb.ly/6TFJV) by artfletch
- "Old church modeling - Interior Scene" (https://skfb.ly/STIp) by Aurélien Martel
- "Lincoln's Inn Chapel Undercroft" (https://skfb.ly/oItqQ) by artfletch
- "St Bartholomew-the-Less Interior" (https://skfb.ly/o6rPA) by artfletch
- "Church Of St Peter Stourton" (https://skfb.ly/oGNzU) by artfletch
- "Mosque of Tree of Al Buqayawiyya, Jordan" (https://skfb.ly/oZYxr) by SJoudeh
- "Salt Tower Lower Room" (https://skfb.ly/oNoqN) by artfletch

The Dan Harpole Cistern, the Buffalo Bayou Cistern and the Basilica Cistern are
built from published dimensions, not scans.

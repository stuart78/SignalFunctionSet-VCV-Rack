# Cistern: real spaces, modelled from their geometry

Design written 2026-10-15. Hidden from its first commit.

Named for the Dan Harpole Cistern at Fort Worden, Port Townsend: the empty
concrete water tank, 186 ft (56.7 m) across and about 14 ft (4.3 m) high, its
roof on a grid of concrete columns, where Pauline Oliveros, Stuart Dempster and
Panaiotis recorded *Deep Listening* in 1988. Its reverb is quoted at about 45 s.

Cistern is a reverb whose rooms are real places at their real dimensions. The
user chooses a space, places a source and a listener in it, and the sound is
what the geometry and its materials do to it. Crystal is the plugin's echo
chamber (geometric habits, audible discrete echoes, a delay mode); Cistern is
the opposite goal: a believable large space, where a discrete echo is a fault.

## What a space is made of

- **Geometry**: planes, vertical cylinders (room walls and columns) and
  triangles (simplified scans), each surface carrying a material.
- **Materials**: absorption in six octave bands (125 Hz to 4 kHz) and a
  scattering coefficient, from standard tables.
- **Air**: absorption per band from ISO 9613-1 at a temperature and humidity.
  In a large, hard room the air absorbs more high frequencies than the walls do: at
  45 s a sound has travelled more than 15 km. A tail that keeps its highs for
  45 s is the commonest way a long reverb sounds fake.
- **Derived**: volume, surface area per material, mean free path (4V/S), and the
  decay time per band (Eyring, with the air term: `T = 0.161 V / (-S ln(1-a) + 4mV)`).

## The engine, in three layers

1. **Early reflections, traced.** On a worker thread, rays leave the source
   (specular, with a scattering fraction going diffuse), lose energy per band to
   each surface and to the air, and are caught by a receiver sphere at the
   listener. Arrivals up to the mixing time (about 2 sqrt(V) ms, capped) become
   a sparse tap set: one tap per quarter-millisecond bin per ear, with gains in
   three render bands (low, mid, high). Stereo comes from the arrival direction
   through a pair of cardioids, so moving the listener or turning it moves the
   image. The direct sound is added exactly.
2. **Late tail, from the physics.** A 16-line feedback delay network, Hadamard
   mixed, delay lengths spread from the room's size, each line filtered by a
   complementary three-band split whose gains give the Eyring decay in that
   band, with slow cubic-interpolated modulation so a 45 s tail does not ring
   metallic, and allpass diffusion ahead of it.
3. **Handover.** The tail is injected so it starts at the mixing time, and its
   level is calibrated on the worker by running the network offline and
   matching its energy to the traced echogram there, so the join is not heard.

## Spaces

| Space | Source | Notes |
|---|---|---|
| Dan Harpole Cistern | built from its dimensions | cylinder 56.7 m x 4.3 m; the column grid's spacing and diameter are ESTIMATES (4.9 m, 0.46 m) until plans are found |
| Hintze Hall | scan, CC-BY 4.0, artfletch | 65 x 32 x 23 m, vaulted, arcaded, galleries |
| St Peter, Stourton | scan, CC-BY 4.0, artfletch | parish church, nave and aisles, pews |
| St Bartholomew-the-Less | scan, CC-BY 4.0, artfletch | octagonal, domed vault |
| Lincoln's Inn Chapel Undercroft | scan, CC-BY 4.0, artfletch | low vault on a pier grid, open-sided |
| Salt Tower lower room | scan, CC-BY 4.0, artfletch | small vaulted stone room |

Credits for the scans (required by CC-BY) are in `handoff/cistern/credits.txt`
and go into the manual and the README. The raw scans (542 MB) stay out of git
(`handoff/` is ignored); an offline converter reduces each to a few hundred
surfaces with materials assigned by surface type, and only that ships.
Photogrammetry leaves gaps where the scan stopped; a gap is treated as an
opening (absorbs everything), which is right for a doorway and wrong for
missing scan, so each preset gets a pass to close the latter.

### The cistern's numbers

The quoted 45 s needs smooth, sealed concrete (absorption about 0.005 at 125 Hz)
and is the LOW-frequency decay: above 500 Hz air absorption sets the decay. With the estimated
column grid the model predicts 47.9 s at 125 Hz, 33.8 s at 250, 22.2 s at 500,
15.7 s at 1 kHz, 9.9 s at 2 kHz and 4.4 s at 4 kHz at 20 C and 50% humidity
(V 10,719 m3, S 6,324 m2, mean free path 6.8 m, 88 columns). The harness checks
the rendered tail against these.

## Controls (draft)

SPACE (preset), SIZE (scale, from the real size), DECAY (scale around the
physical value, up to freeze), HUMIDITY, **SCATTER** (decided 2026-10-16:
default 0.5, each space carrying its own default), MOD, MIX, source and listener dragged
on the screen (as Carillon's microphones), a FREEZE gate; stereo in, stereo out
(quad to follow, as Crystal). The screen draws the space with its columns, both
positions and a per-band decay readout.

## What to measure before believing it

- Decay per band from the rendered impulse (Schroeder integration, T30) against
  the Eyring prediction.
- The join: the energy envelope across the mixing time, within a few dB.
- No ringing: the late tail's spectrum smooth, no modes standing out.
- The cistern's low-frequency decay near 45 s.
- CPU at 48 kHz.

`src/cistern-engine.hpp` is the engine, free of Rack, and
`tools/cistern-harness.cpp` measures it on its own.

## Engine round 1 (2026-10-15)

`src/cistern-engine.hpp` and `tools/cistern-harness.cpp`, all checks passing:

| Octave | Rendered T30 | Predicted over the octave | Eyring at the centre |
|---|---|---|---|
| 125 Hz | 48.5 s | 47.1 s (+3%) | 47.9 s |
| 250 Hz | 35.3 s | 35.1 s (+1%) | 33.8 s |
| 500 Hz | 23.2 s | 23.3 s (0%) | 22.2 s |
| 1 kHz | 16.4 s | 16.3 s (0%) | 15.7 s |
| 2 kHz | 10.5 s | 11.0 s (-4%) | 9.9 s |
| 4 kHz | 6.3 s | 5.8 s (+9%) | 4.4 s |

Join within 2.1 dB, largest late spectral peak 3.6 dB over its third-octave
neighbourhood, late L/R correlation 0.00, 2.0% of a core at 48 kHz with 290
taps; 100k rays trace in 0.3 s.

Three things learned getting there:

- **The tail's loss per pass is a steep curve and needs a real filter.** In the
  cistern a line loses 0.1 dB a pass at 125 Hz and about 3 dB at 8 kHz. A
  three-band one-pole split inside the loop measured +40% at 500 Hz and +96% at
  4 kHz. Each line now carries a flat gain and six high shelves between the
  octaves (Jot's attenuation filter), solved as a linear system in dB (losses
  this small add) and refined four times against the real response, with an
  8 kHz target so the top octave does not hold 4 kHz's decay to Nyquist.
- **An FDN does not start at full level.** Switched on at the mixing time it
  left a 7 dB hole while its lines filled. `calibrate()` runs the network
  offline, measures its build-up with the decay undone, starts it early enough
  to be at 90% by the mixing time, fades the early taps by the complement, and
  sets the gain against the echogram's level at the join.
- **Measuring a decay per octave has two traps of its own.** A 4th-order
  bandpass leaks the slower octave below, and once the band has fallen 30 dB the
  leak is the measurement; the harness filters brick-wall in the FFT domain.
  And an octave's decay is dominated late by its slowest frequencies, so the
  centre's Eyring value is not the target: the harness integrates the physics
  across the same window (the 4 kHz octave runs from about 7 s down to 2.5 s,
  which is why it reads 5.8 s rather than 4.4).

Not yet: the module, the screen, the scanned spaces, and listening. The early
field is about 700 ray detections binned to 290 taps, which matches a real
room's echo density near the mixing time (about 19 per 10 ms here) but is
sparse earlier; whether that reads as grain needs ears.

## Round 2: the transient at 170 ms (2026-10-16)

First listen: "the initial noise decays and then an echo-like transient
appears; the dispersion from this sounds really good." Rendering the two layers
apart showed two things in that spot:

- **Most of it is the room.** The cistern is round, and a concave wall focuses:
  reflections from the whole 28 m wall return to near the centre together at
  about 2R/c = 165 ms. The early field alone peaks at 170 ms, 5.7 dB over its own
  mean. The columns scatter part of it, but a ray crossing the room and back
  misses every column about a third of the time, so the focus survives. It is
  kept: it is what a round room does, and it moves with the listener (strongest
  near the centre). The harness reports it rather than failing on it.
- **A click on top was the engine.** Every line was fed the same input at once,
  so one trip later all sixteen sent back a bare copy of it: crest factor 16 at
  183 ms (start of the tail plus the shortest line). Each line now receives its own
  velvet-noise burst of the input (24 signed pulses over 50 ms), and the tail's
  first half second runs at crest 2.5 to 4.7, which is diffuse. 12 pulses was
  not enough (6.9). Cost 2.9% of a core.

The harness checks crest factor on the tail ALONE, from where it reaches a tenth
of its settled energy: a level check in 10 ms windows cannot tell one click from
a burst of noise with the same energy, and a burst just arriving scores a high
crest at any level.

## Round 3: scatter (2026-10-16)

How hard the round wall focuses depends on how much of each reflection
scatters. The material tables' coefficients are for flat panels; at room scale
what scatters is the build (form joints, pitting, a wall that is not quite a
cylinder), so it is a property of the SPACE (`Space::rough`), overridable from
`BuildParams::scatter`. Measured on the early field, the focus's energy over the
reflections before it:

| Scatter | Focus bump |
|---|---|
| 0.10 | +7.9 dB |
| 0.30 | +4.0 dB |
| 0.50 | +1.8 dB |
| 0.70 | +1.6 dB |

Heard: 0.5 and 0.7 much smoother; the full renders and a kalimba through the
engine (`tools/cistern-render.cpp`) "all sound great". Decided: SCATTER is a
panel control, default 0.5, each space's default its own. It does not move the
decay times, only how the early sound arrives.

`tools/cistern-render.cpp` runs any WAV through the engine offline (mix, wet
only, scatter, decay scale, tail length). The engine still sums its input to
mono; stereo in needs two source positions, which is module work.

## The module, round 1 (2026-10-16)

`src/cistern.cpp`, hidden, 22HP, code-drawn panel to the measured designer
style until the Figma pass. Controls: SIZE (0.25x to 2x the real room, 1 V =
0.3 octave), DECAY (0.1x to 4x the room's own, the tooltip reading seconds at
500 Hz), SCATTER (default 0.5), HUMID (10 to 90%), MOVE (the tail's
modulation, 0 to 200%), MIX, each with a CV; FREEZE latch and gate; IN L / IN R
(R normalled to L), OUT L / OUT R. One SPACE so far, so no SPACE control yet
(it will be appended when the scans arrive; params serialise by index).

**Two sources and a listener.** Each input channel is its own source in the
room, carrying half the energy (a pair of speakers sharing a signal), so a
stereo input is placed rather than summed. The tail is fed (L + R) / 2. The
listener's two cardioids face the sources' midpoint.

**The screen is the room in 3D at its true proportions** (x and z across the
half-extents, height on the same scale): floor, lid, wall, the 88 columns
back to front, both sources (orange, with a ring that swells with the input)
and the listener (white, its two ears drawn in blue), drop lines to the floor
for height. Drag a marker across the floor, shift-drag to lift it, drag empty
space to orbit, double-click to reset the view. Header: the room's size and
volume; footer: T60 per octave and TRACING / FROZEN.

**Retracing without cutting the tail.** A persistent worker thread builds a
program whenever the panel asks for something new, coalescing (a knob turned
during a trace is picked up at its latest value). A DECAY change reuses the
last trace. The audio thread adopts a program by crossfading the early taps and
the tail's feed over 40 ms (equal power between two traces, linear for a decay
change that keeps the same taps), and RETARGETS the one tail rather than
replacing it: new decay filters at once (coefficients only, the state carries
over), new line lengths glided to at 0.005 samples a sample (a 9-cent Doppler
while the room changes size). The program's gains go on what ENTERS the
reverb, not on what leaves it: applied at the output, a smaller room's
calibration turned up the tail already ringing. Nothing is allocated or freed
in process(): programs and whole reverbs (the first one, and after a
sample-rate change) arrive through atomics and go back the same way to be freed
on the worker.

Measured (`tools/cistern-module-harness.cpp`, the real module against libRack):
first program 0.5 s after load; a moved source adopted and the old program
freed; a DECAY change reuses the trace and halves the 500 Hz decay exactly;
FREEZE holds within 0.7 dB over 2 s (the tail's interpolated reads soften the
top slowly, so a frozen tail fades over minutes); 96 kHz rebuilds and plays;
**4.9% of a core** with two sources (825 taps). The three render bands are
stored side by side per sample, so a tap is one cache line: 8.6% before. The
engine harness adds a retrace while playing (no dip or bump across the
crossfade, the wet level holding through a change of room).

The perf bench (`tools/perf`) cannot see Cistern: it runs faster than real time
and finishes before the worker's first trace arrives, so it reports an idle
module.

The C++11 trap: the plugin builds as C++11, where a `static constexpr` class
member bound to a reference (`std::min(MAX_BASE, x)`) needs an out-of-class
definition, and a missing one links (plugins resolve symbols at load) and fails
in Rack. They are copied into locals first.

First play in Rack (2026-10-16): "sounds amazing".

## Round 4: more spaces (2026-10-16)

Ten interiors arrived in `handoff/cistern/` (CC BY 4.0; credits in
`res/spaces/CREDITS.txt`, which ships, and in the module's menu and on its
screen). Nine are in.

### The converter (`tools/cistern-convert.cpp`)

A scan is millions of tiny triangles with holes where the camera could not
see, clutter, sometimes the street outside, and openings that really are
there. The engine needs the AIR: its volume and a closed surface round it that
knows which parts are walls and which are openings.

1. rasterise the scan into voxels (side about cbrt(bounding volume) / 60);
2. thicken the walls by 0.4 m, which seals scan holes under 0.8 m;
3. score each free voxel for ENCLOSURE: of 26 directions, how many reach a
   wall before leaving the scan. Marched through the full-resolution walls,
   sampled per half-metre block: on a coarse grid where a cell is solid if any
   of it is, the free space under a low-springing vault was swallowed (the Salt
   Tower kept 36 of its 102 m3);
4. the air is the enclosed (>= 0.7) free space connected to the deepest point,
   grown back out to the scanned surface;
5. its boundary by surface nets, Taubin-smoothed, each face floor / wall /
   ceiling by its normal, or OPENING where air meets unenclosed space rather
   than a wall (the Pantheon's oculus, the undercroft's arcades, the tunnel's
   ends). An opening absorbs everything;
6. default positions along the room's long axis, ears 1.6 m above whatever
   floor is under them, the source no more than 14 m off (in a 300 m tunnel the
   first default put it 117 m away, past the end of the early window).

`--cut x z r y` removes scan triangles inside a vertical cylinder above a
height: photogrammetry fills an opening with what the camera saw through it,
and the Pantheon's oculus came back capped. Commands as run (from the repo,
`$C` the built converter, `$H` = handoff/cistern):

```
$C $H/the_pantheon_interior res/spaces/pantheon.bin --scale 1.72 --cut -0.2 -0.15 4.3 37
$C $H/hintze_hall res/spaces/hintze.bin
$C $H/leake_street_graffiti_tunnel res/spaces/leake.bin --scale 1.64
$C $H/old_church_modeling_-_interior_scene res/spaces/oldchurch.bin
$C $H/lincolns_inn_chapel_undercroft res/spaces/undercroft.bin
$C $H/st_bartholomew-the-less_interior res/spaces/bartless.bin
$C $H/church_of_st_peter_stourton res/spaces/stourton.bin
$C $H/mosque_of_tree_of_al_buqayawiyya_jordan res/spaces/mosque.bin
$C $H/salt_tower_lower_room res/spaces/salt.bin
```

**Scale is not consistent across the scans**, so each was checked against a
published dimension: the Pantheon came in at 1/1.72 of its real 43.3 m
rotunda, Leake Street at 183 of its ~300 m; Hintze Hall matches its 1881 /
1927 figures (150-170 ft x 97 ft x 60-72 ft) at the scan's own scale. The
others have no published interior dimensions to hand and are as scanned.
**Left out:** St Bartholomew the Great (the air came back as a sliver of the
interior at every setting tried) and the Montroi castle (exterior only).

### The engine

- **Triangle meshes** behind a median-split BVH; normals face the air.
- **A scanned room's decay is measured from rays**, not Eyring: per band, the
  energy-weighted fraction a reflection keeps (an opening keeps none) and the
  mean free path, with the air on top. Eyring knows neither an opening nor a
  shape. Validated in a shoebox with fully diffuse walls, where rays and Eyring
  must agree: they match to 0.0% in every band and on the mean free path.
- **That validation found a bug**: the diffuse reflection was not Lambertian
  (a random direction flipped into the hemisphere, then the normal added,
  crowds reflections towards the normal; it put the shoebox's mean free path
  14% past 4V/S). Now the normal plus a point on the whole sphere. The
  cistern's early field changed with it: SCATTER 0.5 now spreads the round
  wall's focus further than the version listened to in round 3.
- The early window stretches to cover the direct sound (in the tunnel the
  source could be beyond the mixing time and the window held nothing at all),
  and the level the tail is calibrated against falls back on the diffuse field
  (4 pi c / V, decaying, a third through each cardioid) when few rays arrive
  there. Where rays are plentiful the two agree within 2 dB in every room.
- The diffusers ahead of the tail shorten in a room whose fastest band is
  under five times their own ring (0.45 s): the carpeted mosque rendered 38%
  long at 2 kHz because the tail could not decay faster than its input.
- The two sources trace on two threads, and a moved source reuses the room's
  decay measurement: 0.33 s to retrace a moved source in the Pantheon.

### Materials, and what they give

A scan smoothed to half a metre loses what a room is furnished with, so the
materials put it back as aggregates on the surface it sits on: floor with
pews, furnished floor (cases, exhibits, visitors), carpet on stone, coffered
concrete, rough stone, plaster on lath (a vault that flexes absorbs low frequencies).
**Estimates for each building type, unoccupied, not measurements**; no
published reverberation times were found for any of these rooms (St Peter's
in Rome, far larger than the Pantheon, measures 7.1 s mid-band, its authors
crediting the decoration). T60 at 500 Hz:

| Space | Floor / walls / ceiling | Volume | T60 125 / 500 / 2k |
|---|---|---|---|
| Dan Harpole Cistern | sealed concrete | 10,719 m3 | 48 / 22 / 9.9 s |
| The Pantheon | pews / marble / coffered concrete | 40,928 m3 | 23 / 13 / 7.4 s |
| Hintze Hall | furnished / stone / plaster | 17,620 m3 | 9.0 / 4.7 / 2.9 s |
| Leake Street Tunnel | rough concrete / brick / brick, ends open | 21,297 m3 | 12 / 7.9 / 3.9 s |
| Old Church (a model) | pews / stone / wood | 7,444 m3 | 5.3 / 4.4 / 3.4 s |
| Lincoln's Inn Undercroft | stone / rough stone, 15% open | 4,768 m3 | 2.8 / 2.3 / 1.7 s |
| St Bartholomew-the-Less | pews / plaster / plaster on lath | 1,919 m3 | 4.2 / 3.9 / 2.8 s |
| St Peter, Stourton | pews / plaster / wood | 802 m3 | 2.3 / 2.1 / 1.8 s |
| Mosque of the Tree | carpet / rough stone | 282 m3 | 3.5 / 1.1 / 0.55 s |
| Salt Tower, Lower Room | stone / rough stone | 102 m3 | 3.7 / 2.2 / 1.4 s |

`tools/cistern-spaces-harness.cpp` loads, traces and renders every one: the
tail's decay within 5% of what it asks for at 500 Hz and 2 kHz in all ten
(checked on the tail alone, with brick-wall octaves and the target integrated
over each octave), default positions in the air, traces under 2 s.

### The module

SPACE (trimpot + CV, 1 V per space, also a menu), positions kept PER SPACE
(saved by slug), a shared plugin-wide cache so a room loads once. The screen
draws a scan as ten contour slices of its air (marching squares on the air
grid), the floor slice in blue and the vault fading upward; the cistern keeps
its columns. The header names the room and its size, with the scan's credit
under it. SCATTER is one knob for every space (default 0.5); a space's own
roughness only applies to the tools when no scatter is given. 4.7% of a core.
The space files are 5.6 MB (4.1 MB in the package).

## Round 5: tails that stay in their room, and a model you can read (2026-10-16)

Heard in Rack: "a lot of pitch shifting when switching space". The one tail
was retargeted to the new room, its delay lines gliding to new lengths at 0.005
samples a sample: a Doppler bend lasting seconds. **A SPACE change now starts a
new reverb and leaves the old one ringing out in its own room**, fed nothing,
its decay untouched, until it falls below -94 dBV; a second switch while one is
ringing fades the older out in 100 ms to make room. Two reverbs for a while
(about 9% of a core), one again once the old room has died away. SIZE still
glides: there the room really is changing size under the sound.

The screen drew a scan as ten contour slices of its air, which read as a
diagram ("had imagined more traditional 3D models"). **It is now a shaded
cutaway**: each room's surface reduced by vertex clustering to 1,700-3,900
faces, a face drawn only when its inner side faces the camera (so the near
walls and the ceiling fall away and the room is seen from inside, the way an
architectural cutaway is), painted back to front with flat shading under a
fixed light, the floor in the display blue. The reduced faces use the
area-weighted normals of the surface they replace, or their jagged facets
flickered between floor and wall. Drawn into a framebuffer, redrawn only when
the space or the view changes. The cistern is built the same way from its own
geometry (floor, lid, wall, an eight-sided prism per column). Holding or
hovering a marker shows its height above the floor under it, in metres at the
current SIZE: height moves the floor and ceiling reflections, and in a domed or
galleried room it moves a lot. Renders of every room's cutaway (from the module
harness, `CISTERN_SVG=dir`) are in `handoff/cistern/listening/screens/`.

## Round 6: drawn as Crystal draws (2026-10-16)

The shaded cutaway "loses the shape"; asked for something close to Crystal.
Crystal draws a low-poly habit: every facet edge in the display blue, nearer
brighter and heavier, over faint glass. So now does Cistern. Each room is
reduced (vertex clustering, the clusters carrying the smooth surface's
normals) to about 900 facets ON PURPOSE: at 3,000 the folds of a noisy scan
broke into scribble, and the Pantheon's dome, having no sharp folds, was only
an outline. At 900 a curved surface reads as facets, the way a habit does. A
line is drawn between two facets that are not one plane (more than 12°),
weighted by how sharply they meet; the surface's end (an opening's rim) and
the silhouette for the current view at full weight. Glass on the faces seen
from inside, the floor a little bluer. The cistern is built the same way from
its geometry (glass drum, columns as six-sided prisms). Previews from the
module harness, drawn by the same code through a sink interface, are in
`handoff/cistern/listening/screens/`.

## Round 7: thinner, solid, and drawn as sections (2026-10-16)

From screenshots in Rack: the lines too heavy, overlaps blending wrongly, and
"I want these to evoke the actual space ... simpler and more evocative".

- **Solid colours.** Every line and glass face was translucent, so wherever
  they crossed they added into bright knots (Hintze Hall's tangle). Lines are
  now opaque, dimmed towards the background with distance, stroked far to near
  in depth bins so a nearer line lies on a farther one; the glass is gone (the
  cistern keeps a dim floor). Widths 0.4 to 0.75 px, from 0.5 to 1.3.
- **A scan is drawn as an architectural drawing**, not as its mesh: nine
  vertical sections across the long axis (a nave becomes its arches, a tunnel
  its rings, a dome its circles), one section along it through the room's
  centre of air, and the footprint (the outline of every column holding air,
  at the floor under it, so it follows a sloping floor or a bending tunnel,
  where a flat cut a metre up missed most of Leake Street). Cut from the full
  surface; openings left out, so a doorway or the oculus is a gap. Each cut's
  pieces chained into lines, lines under a tenth of the room dropped (columns,
  balusters, gallery fronts), the rest smoothed (Chaikin, twice). The reduced
  mesh's own edges had described the scan's noise, not the building.
- The cistern keeps its model (it is clean geometry), thinner and solid.

The honest limit: a photogrammetry scan is a noisy skin with clutter in it,
and simplifying it only goes so far. The cleanest rooms, to look at and to
trace, would be built from published plans and dimensions as the cistern is
(the Pantheon is a cylinder, a hemisphere and an oculus).

## Round 8: two cisterns, built (2026-10-16)

Photos from the user turned out to be two different cisterns. Centrum's own
photographs of Fort Worden (credited in a thespaces.com article on Jherek
Bischoff's album *Cistern*) show square columns with flared capitals and
pyramid footings, and a grid of beams under the slab; the plan is round (every
source gives a 186 ft diameter). The photos with water are the Buffalo Bayou
Park Cistern in Houston. Both are now BUILT from published dimensions
(`Builder` in the engine), drawn as their own edges:

- **Dan Harpole Cistern** (rebuilt): 56.7 m across, 4.27 m deep; square
  columns 0.4 m with 1.2 m footings and 1.0 m capitals, beams 0.35 x 0.4 m
  between every pair of neighbouring columns, on a 4.9 m grid. Spacing and
  member sizes are ESTIMATES from the photographs. The beam grid acts as a
  coffered ceiling. T60 44 / 21 / 9.5 s at 125 / 500 / 2k (was 48 / 22 / 9.9).
- **Buffalo Bayou Cistern** (new): 87,500 sq ft, 221 columns 25 ft (7.62 m),
  round shafts with belled capitals and square bases, flat slab, a few inches
  of water on the floor, reverberation measured above 15 s and quoted at 17.
  221 = 13 x 17, and at 20 ft bays that grid covers 88,400 sq ft: the grid is
  that INFERENCE. The sloped skirt along the walls (2.4 m up over 2.4 m in)
  and member sizes are ESTIMATES from photographs. 60,700 m3. The
  board-formed concrete is set so it lands the measurement: T60 19.6 s at
  500 Hz, 15.3 s at 1 kHz.

The big surfaces of a built room (floor, lid, walls) are traced as planes and
cylinders and only drawn as triangles: as triangles, the cistern's floor fan
of long slivers gave every ray a dozen huge bounding boxes to test and a trace
went from 0.2 s to 2.4. Columns, beams and the skirt are triangles behind the
BVH: 0.75 s for the rebuilt cistern, 1.1 s for Houston, per source.

For a built room the decay comes from rays, like a scan's; the engine harness
now checks the render against the decay the program asked for (Eyring on the
triangle areas counts faces hidden inside overlapping solids). The top octave
gets 15% there, where the decay falls steepest.

The scans' wobble ("I kind of like the hand-drawn style ... feels very human")
came free from their noisy surfaces; the two built cisterns drew with a ruler.
Their edges are now drawn by hand too: split into short pieces and nudged by a
smooth two-octave noise of their position in the room (0.005 display units),
so the wobble holds still as the view turns and lines meeting at a corner
still meet.

## Round 9: the Basilica Cistern, water, humidity (2026-10-16)

**Basilica Cistern, Istanbul** (built): 138 x 65 m, 336 marble columns 9 m
high in 12 rows of 28 about 5 m apart, a 4 m firebrick wall coated in
waterproofing mortar, the original brick floor with a little water on it
(Wikipedia), and brick CROSS VAULTS from the columns: each bay two barrels
crossing, the soffit the higher of the two, built as a height field split along
the groins so the crease is exact. Vault rise (half a bay) and member sizes
ESTIMATED. 94,800 m3, 79,000 triangles, 1.45 s a trace per source. T60
27 / 18 / 7.8 s at 125 / 500 / 2k: no published measurement to check it by.
Drawn with `Space::foldDeg` 65, so a six-sided shaft draws as its silhouette and
336 of them read as a forest rather than a hatch.

**Water stays a mirror.** A material with zero scatter (water) is now never
scattered by the SCATTER knob, which used to roughen every surface alike. A
water floor sends a sharp, specular reflection back: the floor-to-lid bounce
in a low cistern stays coherent instead of smearing.

**Humidity, measured with the engine** (ISO 9613-1, 20 C): air absorption at
4 kHz is 11 dB per 100 m at 10% RH, 3.0 at 50%, 2.0 at 90%; at 500 Hz it is
0.3 to 0.4 whatever the humidity. Fort Worden's 4 kHz decay goes 1.5 s, 2.9,
4.3, 5.2, 5.6 s at 10, 30, 50, 70, 90%; 500 Hz stays near 21 s. In a room this
large the air, not the walls, decides the treble.

## Round 10: distance (2026-10-17)

From a screenshot: sources dragged ~150 m down Leake Street, and the readout
said T60 0.1 s in every band. Two things.

**A bug: the decay was measured from the first source.** Dragged to the
tunnel's open end, its rays left through the opening on the first trip.
Decay is a property of the room, now always measured from the room's own
reference point (its default source, which scales with SIZE): 7.9 s at
500 Hz from there, 1.7 s near the end, 0.1 s at the opening.

**Distance, made musical by default** (menu: Distance, Musical / True). With
true distance a source 150 m away is a faint, dull arrival 437 ms late
under the reverb: physically right, not much to play. Out to 25 m nothing
changes; beyond, extra distance counts logarithmically
(`musicalDistance()`: 25 + 20 ln(1 + (d - 25)/20)), so 150 m is heard as 65 m
and 300 m as 79 m, still later, softer and duller the further it goes. It is
applied by pulling a source's whole arrival pattern in by the same excess
(every path's delay, its 1/r^2 and its air loss), so the response keeps its
shape and no source is moved through a wall. Measured (spaces harness): 150 m
arrives at 188 ms musical against 437 ms true (d / c exactly), 7.6 dB louder.

What remains unrealistic about distance: the late tail is one even field (a
real 300 m tunnel is not evenly reverberant along its length), sound does not
bend round corners beyond what scattering gives, and the early window stops at
0.9 s. Listening: `handoff/cistern/listening/distance/` (near, far musical,
far true).

## Round 11: credits on the screen (2026-10-19)

The scan credits used to sit as a line under the room name, for scanned rooms only. They are now behind a **CREDITS** button under the room size. Clicking it opens a list of all nine scans, each with its author, its link and its licence (CC BY asks for all three), with the room on screen drawn brighter. A last line names the three rooms built from published dimensions and points to this document for the estimated details. Any click closes the list, and while it is open a click neither drags a source nor turns the view. The links come from `res/spaces/CREDITS.txt` and are now part of each `SPACES[]` credit string, so the right-click "Scan credits" menu shows them too.

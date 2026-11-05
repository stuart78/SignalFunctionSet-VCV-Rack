# Canopy

A steel tongue drum played by drops falling through leaves you place, or directly
from a gate.

## What it is

Drops appear anywhere in a column as wide as the drum and fall under gravity.
Leaves hang in the column, bend when hit and move around where they were placed. A drop
that lands on the drum makes a sound and may bounce and land again. Leaf hits make no
sound.

The screen is drawn from the physics state and adds nothing of its own, except a
camera, so what you see is what is heard.

## The world

Everything is in drum radii. The top is a dome over the disc of radius 1, 0.11 high
in the middle. Drops start at y = 2.6 and take about 0.8 s to land. The physics
steps at a fixed 2 kHz.

## How many tongues

| Scale | Tongues |
|---|---|
| Up to 8 notes | two octaves, so pentatonic = 10, major = 14 |
| More than 8 | one octave, so chromatic = 12 |
| Always | at most 16, at least 3 |

The lowest note is the big tongue in the middle. The others go round it, starting at
the front and rising up alternate sides to the back. Ten is the count on the
reference drum; that a pentatonic scale over two octaves gives ten is my inference,
not something I found stated.

## Every landing sounds

All of the top is one piece of steel, so there is no silent place on it.

- **On a tongue:** that tongue receives the whole hit, and the others 8% falling away
  over 0.3 of a radius.
- **Off a tongue:** the nearest ones receive 26% falling away over 0.14 of a radius, and
  the shell sounds louder.

A tongue rung through the body gets a quarter of its overtones, because their
inharmonic partials otherwise pile up under a busy rain into something like noise. A
drop landing between tongues therefore sounds like a muted version of what is nearby.
The numbers are chosen, not measured.

## Level and tail

Weight is mass^0.35, compressed from sqrt so a steel marble is not 9 dB over the rest,
and the outs are gained so one glass marble on the big tongue peaks at 5.5 V (one Loom
pluck is 7.7 V). The default settings average about 1 V rms at under one drop a second
(they were 0.48 V; Loom in Auto at 6 notes a second is 1.55 V).

A single hit's tail measured as clean decaying sines, so there was no noise in the
tail as such. What was removed: an unfiltered white noise burst on every contact, and
most of the overtones on tongues rung through the body. A tongue also now fades out
over about 50 ms once it is 47 dB down, so at the default DECAY it stops after about
5 s rather than 8, and the upper modes ring about half as long.

## Material

| Material | Density | Bounce | Friction | Contact (ms) |
|---|---|---|---|---|
| Water | 1.0 | 0.05 | 0.06 | 4.0 |
| Rubber | 1.1 | 0.78 | 0.16 | 3.0 |
| Wood | 0.7 | 0.55 | 0.13 | 1.2 |
| Glass | 2.5 | 0.80 | 0.08 | 0.45 |
| Steel | 7.8 | 0.85 | 0.08 | 0.3 |

These are estimates, chosen so the five sound different. The contact times are
longer than a real ball's: a steel marble on steel is in contact for tens of
microseconds, which would put the first null of its spectrum far above every mode
here, for every material.

- **Weight:** level is mass^0.35 x speed, with mass = density x (r / r0)^3.
- **Contact time:** Hertz gives (m^2 / (r v))^(1/5). With m proportional to r^3
  that reduces to r x v^(-1/5), so material and size matter far more than speed.
  The force is a raised cosine of that length and of constant area, so a longer
  contact keeps the fundamental and loses overtones.
- **Water** does not bounce. On the drum it strikes, throws 2 to 4 pieces about half its
  radius, and is gone. A piece of 0.024 radii or more breaks again when it lands, so a big
  drop goes to medium and then small, and a small drop stays whole. On a leaf what is left
  is one of the pieces.
- **Mix** gives each drop one of the five when it appears, and it keeps it.

## Leaves

The leaves are modelled as surfaces, and the physics and the drawing read the same
numbers. Over its outline the surface arches down toward both ends, folds up from the
midrib to the edges, and twists along its length, and the midrib sweeps to one side.
Each of the twelve has its own values (`canopyLeafShape()`), fixed per slot so a patch
keeps its leaves.

**Size** comes from the form, in real proportion. A drum of 45 cm is assumed, so a drum
radius is 22.5 cm. Tip to stem: monstera 30 cm, oak 15, maple 12, beech 10, willow 8,
ginkgo 6, with each slot within about 12% of that. Beech (6-14 cm) and monstera (mature
indoor leaves 30-60 cm, and only mature leaves have cuts) come from published ranges. The
other four are estimates that I could not source. On the commonest 30 cm drum, one
monstera is as wide as the drum, and five hid it and let only 74% of drops reach it, which
is why the drum is drawn larger and DENSITY defaults to 3. Dragging an end of a leaf steers
it and does not change its size. A bigger leaf is also harder to bend: inertia rises with
the cube of its length.

**Forms**, from the menu: Monstera (the default), Beech, Maple, Ginkgo, Oak, Willow, or
All forms in turn. A form is a distance from a centre point at each angle, built from an
outline by casting a ray per angle (`formFromOutline()`). A width along the midrib,
which is what the first version used, cannot draw a lobe pointing anywhere but sideways:
its maple came out a thin stack and its ginkgo a kite. Because the physics reads the same
table, a small drop falls straight through a monstera's cut. `CP_FORM_FLAT` reduces the
fold on broad forms, which would otherwise close up.

A contact on the face uses the surface's own slope, so a drop runs along the fold and
off an end. **DENSITY** (knob and CV, 0 to 12, default 3) sets how many leaves are in
play. The twelve places follow a low-discrepancy sequence in height and the golden angle
in bearing, so the first N are spread through the column whatever N is. A leaf you moved
keeps its place while it is out of play.

On screen a leaf is small flat facets shaded by which way each faces, darker on the
underside, with the outline, midrib and veins over them. A maple's and a ginkgo's veins
spread from the centre; the others leave the midrib.

**A leaf bends.** It is a damped spring about its stem. A hit pushes it down in
proportion to the drop's mass, speed and distance from the stem, up to 0.5 radians,
and it springs back. Surface velocity counts in the bounce, so a drop lying on a leaf
that is coming back up gets thrown.

| Leaf | Give | Friction | Splash | Bend (Hz) | Damping |
|---|---|---|---|---|---|
| Leaf | 0.40 | 0.05 | 0.5 | 6 | 0.22 |
| Felt | 0.10 | 0.16 | 0 | 3 | 0.70 |
| Rubber | 1.00 | 0.16 | 0.6 | 4 | 0.10 |
| Wood | 0.50 | 0.10 | 0.7 | 8 | 0.18 |
| Glass | 0.70 | 0.04 | 1.0 | 40 | 0.40 |
| Steel | 0.95 | 0.05 | 1.0 | 11 | 0.04 |

A drop's bounce on a leaf is its own bounce times the leaf's give, friction is the
average of the two, and water breaks up when speed x splash is over 2. A glass marble
rebounds 0.08 off felt, 0.22 off leaf, 0.31 off wood, 0.54 off glass, 0.93 off steel and
1.03 off rubber, measured over a level monstera. Leaf, the default, is waxy, thin and
springy. A leaf is silent, so the material
reaches the sound only through where and how hard drops then land, and with the surface
curved that difference in arrival time is small. Water soaks into felt but shatters on
steel. **Mix** assigns a different material per leaf.

Friction is Coulomb: a contact removes tangential speed in proportion to how hard the
drop presses. The first version removed a fixed fraction per contact, and a drop lying
on a surface is in contact on every physics step, so that fraction applied 2000 times
a second and stopped it dead. A drop too slow to leave the surface is rolling, and keeps
85% of what sliding friction would remove. One held in a dip or between two leaves for
a second gets a random push.

## Drops leaving

A drop is not removed where it sits. One that has settled on the drum for 1.5 s, or has
been in play for 15 s, is released: the hoop and the draught stop acting on it, so it rolls
down the dome and off the rim, and it is removed once it has fallen below the view. One
falling behind the drum is drawn behind it.

## Drift

Each leaf moves around where it was placed. Five axes (across, along, height, swing,
pitch), each two sines, at rates drawn per leaf, so no two move in step and the
pattern does not quite repeat.

- **DRIFT:** up to 0.42 across and along, 0.22 in height.
- **TILT:** swing up to 1.1 radians, pitch up to 0.22 of the length, so a leaf can
  change which way it drains.
- **SPEED:** 0.02 to 1.5 Hz.

The bounds are hard, which is the "controlled" half. At zero a leaf stays exactly
where it was put.

## Hit rate

A thrown drop would otherwise miss the drum. Two things keep it in:

- **A draught** pulls toward the middle past radius 0.86, growing with distance and
  four times stronger in the last stretch above the drum. It bends a drop back over the
  drum in a curve. A wall would have done the same job
  with a visible bounce.
- **A hoop** round the rim turns back a drop rolling off.

**X and Z** move the column, and it narrows to stay over the drum: at full SCATTER it is
already as wide as the drum, so moving it used to push drops against the edge and change
little, and the drawn column did not move at all. Now +-5 V is edge to edge.

With default settings 97-100% of drops reach the drum, the rest being mostly still in
the air when the count stops, and 61-85% land on a tongue at some point (water 45%,
since it lands only once). Every other landing still sounds, through the body.

## The tongues

Each is six decaying sinusoids (`TG_RATIO`, `TG_DB`, `TG_T60`). **These are
estimates.** I found little published measurement of tongue drums: the ratios start
from one set on a commercial synth's page that cites no study, so they are a place
to tune by ear from, not data.

The strike position is real, though: a hit reads the first six mode shapes of a
clamped-free beam at the point it lands, with a floor of 0.15 so a hit near the clamp
is quiet rather than silent. Makers lower a tongue's note by cutting it longer, so a
higher tongue is drawn shorter, though more gently than a bar's law, since they also
tune with weights.

## Playing it directly

GATE (polyphonic) strikes a tongue with no drop, three quarters of the way to its
free end. V/OCT picks the nearest tongue, folded by octaves into the drum. Unpatched,
gate channel N plays tongue N. VEL is how hard, unpatched a firm 70%. MATERIAL and
SIZE are the mallet.

## Screen

A perspective camera at 8 units on an orbit, drawn on the faceplate. After a real
drum, and kept plain, in the house style: flat fills, thin ink lines, no gradients or
highlights. Two shells welded at the rim, a hoop, and tongues cut as U-shaped lines
with three slits. A ringing tongue dips and fills with a gradient, grey where it joins the
drum and orange at its free end. A leaf turns green for about a quarter of a second when
a drop hits it, by as much as the hit was hard. A drop is a circle, darker
for heavier, with water only an outline. A leaf is described above, a shade per
material. Faint lines show the column. A drop's shadow falls
straight down on the dome, because a sphere in perspective does not tell you its height.

The panel keeps a tight grid so the screen gets the room: a row of jacks along the foot,
inputs at the left as on the other modules, a gap, then the outputs under the last three
columns of the three groups above, headed DROPS, LEAVES and DRUM.

- Drag a leaf: moves it level.
- Shift-drag a leaf: changes its height.
- Drag an end (dots appear on hover): steers it. Its size stays the same.
- Drag empty space: orbits the camera.

## Not done

- The values above were reached by measurement and a few rounds of listening. The tongue
  modes have not had a dedicated tuning pass by ear.
- The panel is code-drawn until the Figma pass.
- Drops pass through each other.
- Drift does not push a drop, though a leaf bending back up does.

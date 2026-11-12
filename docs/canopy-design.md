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

## The zither

TYPE, the trimpot at the end of the INSTRUMENT row (with a CV, 1 V per
instrument), swaps the tongue drum for a zither. A change fades the output out
over 5 ms, switches, and fades back in, so a CV can change it without a click. Everything
above the instrument stays as it was: the drops, the leaves, the draught.

- **The board.** Strings run toward the viewer, as many as the drum would have
  tongues and 0.1 drum radii apart, the low ones at the left and the longest.
  The board leans toward the viewer (0.12 per unit), so whatever settles on it
  rolls off the front.
- **The strings** are modal: twelve harmonics, stretched a little by stiffness
  (`B` = 0.00015), the higher ones dying sooner. A hit excites harmonic n by
  sin(n pi xi) at the point it lands, so where along a string a drop lands sets
  its tone, as the pluck position does on a real one. TONE leans the harmonics
  from their natural 1/n, DECAY sets the ring time.
- **Rolling.** A drop wider than the spacing rests on two strings like a ball on
  rails and rolls down them. While it touches a string it damps the harmonics
  that move most where it sits, as a finger does, and sustains the others: each
  harmonic is pushed along in step with its own motion, the way an EBow
  sustains a string, so the drop makes the string sing quietly and the
  harmonics it favours move as it rolls. Rubbing the string with noise was the
  first version; it was accurate, and it hissed.
- **The draught** acts only on drops in the air. Acting on drops rolling down
  the strings, it pulled them back up near the front, and they swung to and
  fro on two strings until they were released at 15 s: about 8 s of rolling per
  drop. Now it is 0.1 to 0.3 s (the drum's is 1.4 to 2 s), with about 8 hits a
  drop without leaves (the drum's is 10 to 15).
- **Measured:** one large glass drop set on two strings rests on them for 1.3 s
  and rolls 1.5 drum radii; its sustain alone is 0.18 V rms; a drop resting at a
  string's middle takes the fundamental down 13 dB in half a second.
- **Between the strings.** A drop that falls through knocks the board, and the
  nearest strings ring faintly through it.
- **The column** is scaled onto the string bed, which is narrower than the drum.
  Laid out on the drum, 3 of 27 drops at the default settings struck a string;
  scaled, 12 of 15.
- **GATE** plucks a string near its front end.

The drum is unchanged: its voices use the first six of the twelve mode slots,
and every drum render is bit-identical to before.

## The steel pan

The third TYPE. A low-C tenor pan, C4 to E6, chosen as the most recognisable pan.

- **The layout is the real one.** The lowest octave is the outer ring, the next
  the middle ring, the last five notes sit in the centre. Round each ring the
  notes go in fifths, so every note's neighbours are a fifth and a fourth from it,
  and each note's octave lies straight inward from it.
- **The pads** are tuned so their first three partials are the fundamental, the
  octave and the twelfth; the partials above are not tuned. A hit at a pad's
  middle is mostly fundamental, one near its edge brings the upper partials out:
  measured, the octave is 0.17 of the fundamental at the middle and 0.98 near the
  edge. Pads whose partials meet a struck one (a fifth, an octave, a twelfth away)
  ring with it, which is the pan's shimmer. Ratios, levels and ring times are
  estimates, not measurements of one instrument.
- **Sticks.** A pan stick is light, rubber-tipped and bounces straight off, so
  contact on the pan is 0.3 of the table's. At the drum's contact times rubber
  lost every pad's octave. Rubber is the authentic material.
- **Rolling sounds the pads.** Every pad is a separately tuned patch with a
  groove round it, so a drop rolling over a groove knocks the pad it enters or
  leaves, and the hammered steel of a pad is a run of small bumps, each a soft
  tap of that pad (on average one every 0.015 radii). A rolling contact lasts
  half a cycle of the pad it is on, so it passes the note and rounds off what is
  above it: with a strike's short contact every bump was a tick, and the roll
  sounded like a mechanism. Measured on a glass drop set down on the outer ring:
  8 pads sound at 0.2 V rms as it rolls down, against 0.05 V without it.
- **The skirt hides the near part of the bowl.** It is drawn after the pads and
  the drops in the bowl, so from a low camera the front of the bowl and anything
  in it is behind the drum's wall.
- **The bowl** is concave and hung tilted toward the player, so drops roll down
  across the pads to the low point. A drop that settles there fades out over
  0.4 s instead of piling up: measured, a glass drop reaches the low point and is
  gone after 1.7 s.
- **Tuning** (menu): Chromatic; Tuned to the scale, where each ring holds the
  scale's notes in order (12 pads for Pentatonic Minor over the same range); or
  Chromatic with the notes outside the scale damped, the default, where a damped
  pad thuds and is gone. A real pan is chromatic; the third keeps the real layout
  and still answers ROOT and SCALE.
- **29 notes** is more than POLY's 16 channels, so it carries the 16 loudest,
  each keeping its channel while it stays among them.

## The singing bowls

The fourth TYPE. Tuned bowls on cushions on a low table under the leaves.

- **One bowl per note of the scale**, over one octave from "Lowest note": five
  for a pentatonic, seven for a major scale, at least three and at most eight
  (a longer scale is thinned evenly to eight). The lowest bowl is the largest
  and sits in the middle; the others go round it, smaller as they rise, since a
  bowl's note falls as its mouth widens. Their cushions are different thicknesses.
- **Each drop picks a bowl with equal chance** and appears over it, so the
  largest bowl does not get most of the hits because it is the largest target.
  Measured over 2000 drops: 281 to 320 per bowl, with the rest landing on rims or
  between bowls.
- **The partials** are a bowl's: 1, 2.72, 5.12, 8.21 and 11.92 times the
  fundamental, ringing from 22 s for the fundamental down to 3 s. Ratios, levels
  and ring times are estimates. Each partial is a **pair** a fraction of a
  percent apart, because a real bowl is never perfectly round, and the pair is
  what makes a bowl beat. A hit on the line the pair is split along rings one of
  them and holds steady; a hit between rings both and beats. Measured on the
  fundamental over 12 s, the quietest point against the loudest is 0.99 struck
  on the line and 0.06 struck between. Every bowl has its line at a different
  angle.
- **The shape is a hammered Himalayan bowl** (`cpBowlOuter`): a flat foot, a
  wall that swells to its widest (1.1 of the mouth) a little over half way up,
  and a mouth that turns back in, with a rounded lip. Height 0.95 of the mouth
  radius. The physics and the drawing read the same profile. A drop meets the
  wall's real cross-section (`cpBowlSection`, a polygon swept round the axis),
  so it can strike the outside of the belly, the lip, or the inside, and run
  round inside.
- **Nothing holds a drop on the lip.** The lip is round and frictionless, and a
  drop on top of it is pushed off whichever side it is on: one set down on it
  leaves within 0.3 s. The draught that steers thrown drops back acts only in
  the air above the rims, so it cannot press a drop against one. (The first
  version had a thin rim ring with friction, and the draught pushing drops into
  it, which held them there.)
- **The rim and the inside sound differently.** A hit near the lip brings out
  the upper partials; a hit low in the bowl is mostly the fundamental.
- **A drop rolling round inside a bowl sustains it**, as a ball circling a bowl
  does, using the zither's sustain: what the drop touches is damped a little and
  the rest is pushed along in step with itself, at a quarter of the zither's
  strength because a bowl rings so much longer. A glass drop rolls for about 2 s
  and holds the bowl at about 0.19 V rms.
- **Water stays in the bowl it lands in.** The level rises with each drop, and a
  fuller bowl sounds lower and rings shorter: 90% full is 1.9 semitones down.
  It dries out over three minutes. A drop adds far more than its real volume,
  or the change would take an hour to hear; 20 s of water rain fills the fullest
  bowl to about 30%. Water that hits a rim splashes.
- **Drops do not pile up.** A drop that has settled fades out over 0.4 s.
  Drops that miss land on the table, which is silent, and water soaks into it.
- Default settings average 1.1 V rms, about the drum's level.



- The values above were reached by measurement and a few rounds of listening. The tongue
  modes have not had a dedicated tuning pass by ear.
- The panel is code-drawn until the Figma pass.
- Drops pass through each other.
- Drift does not push a drop, though a leaf bending back up does.

## The wind chimes

The fifth TYPE. Tubes hanging from a ring under the leaves, one per note of the
scale over one octave (3 to 8), from "Lowest note" an octave up, with a wooden
clapper on a cord in the middle and a sail under it.

- **Everything is a pendulum** (`CpPend`): a tube on its cord, and the clapper and
  sail on theirs. A hit anywhere turns into swing through the arm from the pivot,
  so a drop on the edge of the sail swings the clapper, which strikes the tubes,
  and the tubes strike each other. Measured: one glass drop on the edge of the sail
  sounds 8 notes; the longest tube swings once every 1.92 s, the period of a rod
  on its cord.
- **A tube is a free-free beam**: partials 1 : 2.757 : 5.404 : 8.933 : 13.34, each
  struck by its own mode shape where the hit lands. Ring times are estimates for
  aluminium. A note goes as 1/length², so an octave is a tube 0.71 as long.
- **The clapper is the chimes' own voice**: a heavy wooden puck close to the tubes,
  so a swing of 0.2 rad reaches one.
- **A pendulum at the end of its travel stops dead.** Holding the angle at its
  limit while keeping its speed reported a velocity it did not have, and flung
  whatever touched it.
- **The draught** over the chimes checks a drop flying outward and draws one beyond
  the ring back in, but only in flight (`CanopyDrop::air`, the time since it last
  touched a leaf): acting on a drop lying on a leaf, it held it there, and a
  hundred piled up.

## The handpan

The sixth TYPE. A convex steel shell: a raised ding in the middle and eight tone
fields round it, each a shallow dent, so drops run across the fields and off the
edge rather than collecting. The fields rise in a zigzag, alternating left and
right from the one nearest the player, as most makers lay them out; the first is
the first scale note at least a fifth above the ding, so D Minor gives a D Kurd
(D3 | A3 Bb3 C4 D4 E4 F4 G4 A4). The notes use the steel pan's machinery
(fundamental, octave and the fifth above that, fields whose partials meet ringing
with each other, rolling sounding the fields) with a ring 2.6 times as long.

## The bonang

The seventh TYPE. Javanese kettle gongs in two rows on cords across a low wooden
frame, following SCALE: Slendro gives 10 kettles and Pelog 14, the far row an
octave over the near one, both rising left to right. On a real bonang the kettles
are arranged by player and piece, so this is one arrangement of several. A kettle
has a raised boss, a flat top and a rounded shoulder: the boss gives the note,
the flat top a muted, brighter knock (measured, the fundamental is 0.96 of the
sound struck on the boss and 0.73 on the top). No measured partial ratios for
bonang kettles have been published, so the partials are estimates for a small
tuned kettle gong, with the fundamental a near pair. Drops fall over a kettle,
each as likely as any other, and the draught follows the frame's long oval.

## The marimba and the balafon

The eighth and ninth TYPEs, one bar engine.

- **Marimba**: a chromatic keyboard over two octaves from "Lowest note", laid in
  a circle so it fills the round view: the naturals as spokes round an outer ring
  rising clockwise from the front, one place left empty between the highest and
  the lowest, and the sharps raised on an inner ring between the naturals they lie
  between. A straight keyboard was twice as wide as deep and left most of the view
  empty. Each bar is undercut so its overtones are 1 : 4 : 9.9 (measured 3.99 and
  9.90). Notes outside SCALE are damped, as the steel pan's are by default. A
  resonator tube a quarter wavelength long hangs under each bar's outer half.
- **Balafon**: one row of the scale's own notes on gourds. When the bar moves
  past a threshold, the gourd's membrane slaps the rim of its hole on the outward
  half of each cycle, which flattens that half of the note: a buzz made of the
  note's own harmonics, as a kazoo's is, with only a trace of rattle. (The first
  version clipped both halves and added band-passed noise, and sounded like
  noise.) A soft touch stays clean and a hard hit buzzes: the 2nd and 3rd
  harmonics, which a balafon bar does not otherwise have, come to 0.14 of the note
  struck hard and 0.01 soft. The threshold reads the bar's motion, not its level
  in the mix. Its overtones (1 : 2.9 : 6.2) are estimates; balafon bars are tuned
  less exactly than a marimba's.
- **Where a drop lands along a bar** sets the partials by the free bar's mode
  shapes, so the middle has no second mode. Bars are flat, so a drop that
  settles fades out.
- **GATE and V/OCT**: every instrument after the zither keeps its notes from C4,
  and a V/OCT cable picks the bar, kettle or tube by absolute pitch. Looked up as
  the drum's are, relative to ROOT, a Pelog note landed 60 cents out.

## Levels, and drops that would miss

- **The new instruments ring long and their tails stack**, which drove the output
  limiter most of the time and was heard as distortion. Measured as the share of
  time the limiter is past -1 dB over 30 s at the default rate and at 0.75: the
  balafon went from 30% and 74% to 0% and 10%, the chimes from 7% and 46% to 0% and
  8%, the handpan from 12% and 46% to 2% and 9%.
- **The bonang and the bars have gaps a thrown drop falls through**, where the drum
  is solid. In the last 0.7 of its fall, a drop in flight is drawn sideways toward
  the nearest kettle or bar (`steer()`), the nearer the stronger. The bonang went
  from 0.5 to 2.1 notes a second at the default rate. A drop that reaches the
  bonang's cords fades at once instead of lying there.
- **The handpan is drawn as a convex shell is seen**: the dome as one outline (the
  hull of its projected surface), each note only where its surface faces the
  camera, and the bottom shell down to where it turns away. From a low camera the
  notes on the far slope used to show through the dome.

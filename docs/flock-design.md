# Flock: design

*Status: first pass built 2026-09 (`src/flock.cpp`, hidden), revised three
times by ear the same day; the sections at the end record what each listen
changed. Measured with `tools/flock-harness.cpp`, which compiles the real
module against libRack: at LATITUDE 3 semitones the flock sits on the roost
with an rms spread of 0.35 to 1.0 semitones from 11 to 256 birds; a
12-semitone leap is taken by every call within one physics tick at LAG zero,
and at LAG half the slowest tenth of the flock is still within 1.8 s; a
startle takes the spread from 0.6 to 4.9 and back to 2.8 within 1.5 s, the
hawk inside for 1.3 s; 256 birds with 64 calls cost about 4% of one core.
Ships hidden until it is discussed, like every new module.*

## What the first listen changed

Three faults, all found by ear on the first build and then confirmed by
measurement, and all worth remembering for any module built on steering
agents:

- **A flock that chases never arrives.** The textbook Boids steer asks for
  full speed toward every target at any distance, so the flock crossed the
  roost and oscillated about it, and the panel showed the birds bouncing.
  The tether, cohesion and centroid pulls now use an *arrive* rule: within a
  slowing radius the wanted speed falls with the distance left. Alignment
  likewise matches the neighbours' actual velocity rather than normalising
  their heading to top speed, which had every bird asking for full speed
  whenever the flock moved at all (measured: three semitones of overshoot).
- **The picture was centred on the roost**, so each step of the CV threw
  the whole flock the other way across the panel while the birds themselves
  were moving smoothly. The view now follows the flock's centre slowly, and
  the roost line is the thing that jumps.
- **The croak was the modulator ratio.** Phase modulation from a modulator
  at twice the carrier put the sidebands where a frog keeps them. The
  modulator now sits on the carrier, which opens a harmonic series, and the
  index came down. Birds also sing high, and 0 V is C4, so a REGISTER menu
  puts the roost an octave above the CV by default.

Two more from the numbers: the leaders-only tether left the flock hovering
two semitones off the roost, because the leaders sat on it with no force
left and the rest were only held to the leaders, so every bird now hears the
composer faintly (0.45 against the leaders' 1.2); and disposition scales the
acceleration and the top speed rather than only a ceiling, or LAG did
nothing once the arrive rule made the steering small. The hawk throws birds
sideways off its path rather than radially, which is what opens a hole.

A microtonal granular voice after a murmuration. Lots of very short notes,
moving quickly, always taking a new shape. The pitches are structured but
never locked to a scale. The composer sends in a pitch and the flock leans
toward it, and does some of what it wants.

## The one idea

**The birds are persistent. The grains are their calls.**

A granular engine draws stateless grains from a distribution, and a
distribution never takes a new shape. A murmuration is a set of agents that
persist between calls, each with a position that moves continuously, and
the shape you see changing is the flock's state. So Flock keeps N birds alive
the whole time and a grain is one bird calling: a short window onto where
that bird is right now. "Always a new shape" is free, because the shape is
the state.

## The flock

Boids, after Craig Reynolds (1986, published at SIGGRAPH 1987). Each bird
steers by three rules evaluated against its local flockmates:

- **Separation**: steer away from crowding neighbours.
- **Alignment**: steer toward the neighbours' average heading.
- **Cohesion**: steer toward the neighbours' centre of mass.

Two choices on top of the textbook model, both load-bearing:

1. **Neighbours are topological, not metric.** Each bird uses its seven
   nearest birds, whatever the distance, rather than every bird within a
   radius. That is what the Rome starling studies found real flocks do
   (Ballerini et al., 2008, six to seven nearest), and it is why a real
   flock stays one object at any density where a radius model either
   fragments when sparse or gridlocks when dense. It also fixes the cost per
   bird regardless of WEIGHT.
2. **Information travels at a finite speed.** Nothing hears the composer
   at once. The tether force (below) acts on the birds nearest the roost;
   every other bird only follows its neighbours. A pitch change therefore
   sweeps through the flock from the near side to the far side, which is
   the wave you see cross a murmuration, and is the built-in delay.

Each bird also has a **disposition**: a personal response time constant,
drawn once from a distribution whose width is the LAG control. Tight, and
the flock moves as one. Wide, and stragglers turn up seconds after the turn,
still singing the old note over the new one. That is where a flock's
harmonies come from, and it is a delay with no delay line.

Physics runs at control rate (every 128 samples at 48 kHz, about 375 Hz),
neighbours found through a coarse 3D grid rather than every pair. WEIGHT is
the bird count. The ceiling is to be **measured**, not promised: the aim is
a few hundred birds for the picture and the dynamics, with the sound capped
separately (below).

## Space

Three axes, and each means something:

- **Height is pitch**, in semitones (log frequency). A high bird is a high
  note and the flock's vertical extent is its voicing. Rotation never
  touches height, so turning the listener never moves a pitch.
- **The horizontal plane is the stage.** Where a bird is on it decides pan
  and depth, from the listener's viewpoint.

**The tether.** The composer's V/OCT sets the height of a roost point. The
flock has two centres of cohesion, its own centroid and the roost, and it
leans toward the roost without landing on it. Moving the CV moves the flock
the way a hawk moves starlings: it follows with a lag and an overshoot, and
the shape changes as it turns.

**LATITUDE** is the leash, in semitones. Near zero the flock is a cluster a
few cents wide, which is a chorus. Wide open it spans octaves and the CV is
a suggestion.

**STRUCTURE** is a potential landscape on the pitch axis: soft valleys at
simple ratios of the roost frequency, harmonic above and subharmonic below.
Shallow, and the birds sit anywhere and it is a cloud. Deep, and they pool
in the valleys and the flock rings like a chord of just intervals, but a
bird pushed by its neighbours still climbs out and drifts between them.
Separation keeps them from ever collapsing to unison, which is where the
beating and shimmer come from. Nothing in it is a scale; the intervals it
prefers are the ones the ear hears as structure. Open question whether this
is in v1 or the flock rules alone are enough to start.

**AGILITY** is the birds' maximum speed and turn rate, which is how fast the
flock can change shape.

## Flight, roost and the hawk

**GATE is flight.** High and the birds fly and call. Released and they
settle: calls thin out and the cloud sinks toward the roost over a release
time, so a held gate is a texture and a short one is a burst. (Alternative
considered: gate as a trigger for a volley of calls. Flight is the
instrument; a volley is a patch you can make with a short gate.)

**STARTLE is the hawk.** A trigger spawns a predator that dives through the
flock on a path through its centroid. Birds flee it locally, and because
they only see their neighbours the hole and the surge propagate outward at
the flock's own information speed, which is how the comma and the
hourglass shapes form in the sky. The flock re-forms once it passes. A
**HAWK** gate output is high while the predator is inside the flock, so a
startle can fire something else in the patch.

## The call

Birdsong is a chirp, so a grain has a pitch trajectory of its own added to
the bird's height: **CHIRP** sets its direction and depth, an up or a down
glide over the grain's life. The amplitude envelope is asymmetric, quick in
and tapered out. **MORPH** is the timbre opening across the grain: a sine at
the edges and something richer in the middle (a fold or a brief FM index,
both cheap and both sound like a throat opening), so the call has a body.
**LENGTH** runs 10 to 300 ms. **RATE** is how often each bird calls, with
jitter so calls scatter across the flock rather than pulsing.

A global cap of about **64 sounding grains** holds the cost whatever the
flock size: below it, call density scales with WEIGHT; above it, each bird
calls less often. A quiet flock costs nothing, as a quiet Kit drum does.

## The listener, and the display

The listener stands on the stage plane at a fixed distance from the flock's
centroid, and **ROTATE** (knob plus CV) walks them round it. Pan is a
bird's azimuth from the listening axis (equal power); level falls with
distance with a floor, and far birds get a gentle high cut, so depth is in
the mix. Rotating a comma-shaped flock sweeps its dense head across the
field while the thin tail trails behind.

**The display is the listener's view**, drawn on the faceplate in Wheel's
screenless style: the panel's greys, depth by size and alpha, perspective
projection, and orange for a bird at the moment it calls. A faint line at
the roost height, the hawk as one dark dot when it is in. The picture and
the stereo image are the same viewpoint, so turning ROTATE turns both. The
browser thumbnail is a real instance warmed up through `src/preview.hpp`,
flying, with a hawk mid-pass.

## Outputs beyond L and R

- **PITCH** (`CENTRE_OUTPUT`): the flock's actual centroid height as 1V/oct. The premise is
  that the module does some of what it wants, and telling the composer
  where it went closes that loop. Patch it back to a quantizer or to
  another Flock.
- **DENSITY**: calls per second as a CV.
- **HAWK**: gate, above.

## Panel, first draft

Around 18HP, screenless, the flock across the top.

- Row 1: WEIGHT, LATITUDE, STRUCTURE, AGILITY, LAG, ROTATE.
- Row 2: LENGTH, CHIRP, QUANT, RATE, RELEASE.
- CV in for every knob in the two rows (plus or minus 5 V, full range).
- V/OCT, GATE, STARTLE (jack and button).
- Out: L, R, PITCH, DENS, HAWK.

## The designer's panel (2026-09)

Both panels are now the designer's Figma exports (`res/flock.svg`,
`res/flockin.svg`), adopted with the `figma-panel` skill: normalised,
positions read out of the guide circles (30 and 8, every one matched to a
control within 0.13 mm), runtime labels removed because the art outlines
its own, guides stripped after the Rack render was checked.

- Row A keeps the enum's order. **Row B is in the art's order, not the
  enum's**: LENGTH, RELEASE, CHIRP, VARIETY, RATE, QUANT — the envelope
  pair beside the length, the pitch pair at the right.
- Foot: GATE, V/OCT, STARTLE (button, then its jack), LEFT and RIGHT on a
  plate. **PITCH, DENS and HAWK came off the panel** for simplicity; their
  enum slots stay, retired in place, since outputs serialise by index.
- The display stops at 57 mm so the birds fly clear of row A's labels.
- Flock In is one column: ENV, REACH, FREEZE each over its jack, the audio
  pair at the foot.

## After the panel: the box, the mics, the width

- **The box is drawn without its near face.** The four nearest corners are
  found by depth each frame (so ROTATE keeps it right) and the edges among
  them are skipped: the picture is a stage seen from the front, open toward
  the listener, rather than birds behind a pane of glass. It is 1.3 leash
  units tall, since birds fly up to 1.5 above and below the view and a unit
  box had them through its lid; the vertical scale and the picture's centre
  moved to fit it in the shorter display.
- **No bar between the mics.** Two circles and their aim lines.
- **The floor's front edge is always drawn**, near face or not: it is the
  ground, and a floor with no front edge reads as a rug.
- **Drag the display to move the camera.** Horizontal drag orbits (a yaw
  added to the listener's bearing for the view only, so the pair stays
  where ROTATE put it), vertical drag tilts between 3 and 75 degrees
  above the stage. The near face is re-found by depth every frame, so the
  open side of the box is always the one facing you. Saved with the patch;
  double-click resets to behind the listener, 22 degrees down.
- **Width is normalised by the flock's own spread.** A fixed pan sharpness
  panned a tight flock narrow and a loose one wide, so the image was a
  picture of the flock's looseness. The bearing is now divided by the rms
  angular spread seen from the pair (the horizontal rms, 1/√2 of `spreadH`,
  over the distance to the centroid), and WIDTH sets where the rms bird
  lands: 0.7 of the way to the speaker, past it (default), or well past.
  Measured L/R correlation at rest, five flocks each, at agility 0.1 / 0.5:
  Normal 0.58 / 0.60, Wide 0.38 / 0.42, Extreme 0.30 / 0.25 — against 0.48
  for the old fixed ×8, and now the same for a knot as for a cloud. Two
  cautions: correlation of a field filled *evenly* bottoms out at 0.64, so
  anything lower means birds parked against the speakers, and that is what
  Wide and Extreme do by design; and a startled bird still goes wherever
  its bearing takes it.

## The pair moves in, and the startle comes back to the field

The listener pair stood at the front face of the cube (1.1 from the roost)
because width used to come from distance. Once width was normalised by the
flock's spread, distance only set how hard a bird passing the pair swings
and how much level it gains on the way, and both are stronger close, so the
pair now stands **half a unit** out (`FL_LISTEN_D`), at the edge of a
settled flock rather than inside it.

Moving it in exposed two faults in the normalised law, both fixed:

- **The startle had been normalised away.** The flock scattered, the live
  spread grew with it, and every bird stayed where it was in the image.
  The pan is now divided by `spreadRef`, the settled horizontal spread
  slewed over two seconds and **held while the hawk is in the flock**, so a
  fleeing bird goes past the speaker.
- **Sine saturated before the bearing did.** Close in, the flock subtends a
  wide angle, and `sin(bearing)` flattened the outer birds. The pan is now
  the wrapped angle over `atan(spread / distance)`, linear all the way round.

Measured per call (the latched L/R gains of every grain started in the
window, five flocks each; "hard" = within 10% of a speaker), at Wide:
rest about 40% hard, then 45–50% in the first half second after the hawk
and 55–85% in the second. The spread between runs is the hawk's bearing:
one flying along the pair's axis scatters birds front to back, which a pan
cannot show. Width tiers are now {0.8, 1.3, 2.2}; L/R correlation at rest
0.65 / 0.34–0.44 / 0.26–0.31.

## A lead bird calls on the gate (menu, default on)

The gate is flight, not a trigger, and every call is a Poisson draw, so
with one bird a note had a random onset, a random count and often no call
at all: measured over twenty 200 ms gates, the first call came 44–177 ms
after the gate and fifteen gates were silent. With **Lead bird calls on the
gate** one bird calls the instant the gate rises, unconditionally (it skips
the settling-flock probability too), and the rest fan in as before: onset
1.3 ms on every gate, none silent. In a thousand birds it is one call among
the fan-in and cannot be heard. Off restores the pure flock behaviour.

## What to measure before believing it

A harness that extracts the flock step verbatim (as the Kit, Wheel and Sigma
harnesses do) and reports:

- **Cohesion**: mean distance to the centroid over time, at several WEIGHT
  values, so the flock is neither a point nor a gas.
- **Propagation**: after a step in V/OCT, the time for the nearest and the
  farthest birds to arrive. The requirement is a spread, never a jump.
- **Startle**: that the wave crosses the flock and the flock re-forms.
- **Call density** against WEIGHT and RATE, and that the cap holds.
- **Cost** per bird at control rate, to set the WEIGHT ceiling honestly.

## Open decisions

- STRUCTURE is in v1 and untuned by ear.
- The bird ceiling, once measured.
- Whether a bird calls more while turning (it would make the shape audible
  as rhythm), or at a rate independent of its motion.
- A poly out of the sixteen nearest birds, later, if a patch wants them.

## Name

Flock. Alternatives considered: Murmur, Skein.

## Second listen: sines, and QUANT

"Too screechy." The phase-modulation morph is gone: a call is a sine with its
envelope and its chirp, and the timbre is the crowd. MORPH's slot on the
panel became **QUANT**, a control over the pitch each call is derived from:
at zero a bird sings exactly where it is, at full it sings the nearest
semitone, and between the two the call is pulled part of the way. The flock
stays microtonal underneath and only the calls are snapped, so LATITUDE and
the shape are unchanged and what changes is the harmony. The grid is
semitones for now; a scale from the plugin's SCALE bus is the obvious next
grid.

## Third listen: the flock does not fly to the pitch

"Followers never get close." They did not: birds chasing the roost through
pitch space are a flock that is always arriving, and the ones at the back
arrive last or not at all, which is a bad portamento rather than a
murmuration. So the birds now live in **offset space** around the roost.
The shape is the shape, and a change of pitch moves every call instantly.

What is late is what each bird **hears**. Every bird carries its own copy of
the roost, slewed toward the real one with a time constant of its own: zero
for the keenest, LAG seconds for the most sluggish (`tau = LAG * disp²`,
LAG up to four seconds). At LAG zero the whole flock takes a new pitch on
the same tick (measured: 0.02 s). At LAG half the slowest tenth is within
1.5 semitones of a 12-semitone leap by 1.8 s; at LAG full some of them
are still gliding six seconds later, singing the old note over the new
one. The leaders, the propagation through neighbours and the
disposition-scaled speed are gone; disposition is now a mild factor on how
a bird turns, so the flock is not one rigid thing.

The propagation idea (a pitch change sweeping across the flock from the
near side) is therefore retired. It was the more beautiful mechanism and
the worse instrument.

## Fourth listen: timing, width, grids

- **Calls arrived together.** A gate opened in 20 ms onto whichever birds
  were already due and they all called on the first tick, and each bird's
  interval was a near-metronome (0.5 to 1.5 of the period). Now take-off
  fans in over about 150 ms with every bird's next call re-drawn at the
  gate; intervals are exponential (Poisson) with a 30 ms floor, so calls
  bunch and thin the way a flock's do; and each bird has a fixed chattiness
  between 0.4 and 2.5 times the RATE. Call length and chirp depth vary per
  call too, and the rise is a quarter of the call rather than 15%, which
  clicked at 60 ms.
- **The image was mono.** The listener stood two leashes from a flock a
  semitone wide, an eight-degree spread. The listening distance now follows
  the flock's horizontal spread so the edge birds sit near forty-five
  degrees whatever LATITUDE is. Measured L/R correlation went from about
  1.0 to 0.55 to 0.60, and a bird crossing the flock now crosses the field.
- **QUANT grids**, in the context menu, as sets of offsets from the roost
  repeating each octave so the grid moves with the composer's pitch:
  semitones, whole tones, root and fifth, octaves, root-third-fifth, root
  minor third fifth, root-third-fifth-seventh, pentatonic, fourths and
  fifths. The grid hangs off what each bird hears, so a lagging bird snaps
  to its old pitch's grid until it arrives.

## Fifth listen: the space, the listeners, the width

- **The roost line is gone**: it drew the eye and said little once the
  flock stopped flying to it.
- **The leash is a wireframe box**, twelve edges faded by depth, centred
  on the flock and turning with ROTATE, so the birds have somewhere to be
  in front of or behind. The camera stands behind and 22 degrees above the
  listener on the listener's bearing.
- **The listeners are drawn**: an L and R pair on the stage plane where
  the image is actually taken, walking round the box as ROTATE turns.
- **Width is physical distance and nothing else.** The listener stands a
  fixed 0.3 leash from the flock's centre (it followed the flock's spread
  for one build, which held the angular width constant however far the
  birds flew, so a startle sounded no wider than rest). Only the stage
  plane counts: x is the stereo axis, z is depth and level, height is
  pitch alone. Measured L/R correlation: 0.8 at rest, 0.54 in the half
  second after a startle, with birds passing the ears at up to twice the
  level.

## Sixth listen: a cube, solid birds, the pair outside, and variety

- **The cube** is a cube, lighter, with a grid on its floor after the storm
  cell on the cover of Tufte's *Visual Explanations*; the birds are solid,
  depth being size and a shade toward grey and never transparency.
- **The listeners stood inside the flock**, which is why the image was
  narrow: a point inside a cloud hears birds from every side and the sum is
  a diffuse centre. Three models were measured. A spaced pair of cardioids
  a leash out could not get below an L/R correlation of 0.8 at rest at any
  spacing, since a cardioid is a gentle pattern. The pair now stands a
  quarter leash outside the flock's centre and the bearing is panned
  sharply (the sine of the bearing times 1.6, clamped, into an equal-power
  law), with level and brightness falling with distance. Measured
  correlation: 0.53 at rest, 0.43 in the half second after a startle,
  against 0.95 and 0.51 for the pair inside. The L and R marks on the
  display are that pair, with their aim.
- **VARIETY**, a menu slider (a hidden parameter, so it saves with the
  patch and is MIDI-mappable): each bird is born with a *voice*, its own
  second and third partial, and a *song*, one to three syllables each a
  step away with its own sweep direction and length, and VARIETY is how far
  those are allowed to show. At zero the flock is a crowd of one plain
  sine. Syllables chain with a breath between; a song over, the bird waits
  for the Poisson clock like anyone else (it did not, once, and a
  two-syllable bird sang without pause: 136 calls a second from eleven
  birds).

Considered and banked: a stereo input pair in place of some of the
non-audio outs, so the flock could carry a signal rather than sine calls.
That needs input controls the panel has no room for, so it is a left-hand
expander if it happens.

## Seventh listen: the pair stands still

"The listeners keep moving into the middle." They were placed relative to
the flock's centroid, so they followed the birds wherever they went, and a
quarter leash is inside the cube by construction. The pair is now fixed in
the world at the roost, on the front face of the leash cube (a whole leash
from the centre), and the cube and the floor grid are centred on the roost
too, so the flock is what moves. To keep the width from that distance the
bearing is panned sharply, times six, chosen by measuring five flocks per
setting: times 2.5 left a correlation of 0.91 at rest, times 6 gives 0.48
at rest and 0.25 after a startle, times 8 gains nothing. Level and
brightness still fall with distance, so a startled bird that reaches the
face of the cube passes the pair.

## Eighth listen: LATITUDE is pitch only

"Low, they become a single column, which I never want." LATITUDE was the
leash on all three axes, so at a tenth of a semitone the birds were pinned
into a point and the only spread left was the pitch each still heard,
which drew as a column. The birds now live in a **unit cube** round the
roost, in cube units, and the flock always has the same physical size,
shape and motion. LATITUDE is only how many semitones the cube is tall: a
bird's pitch is what it hears plus its height times LATITUDE, so the knob
flattens the flock in pitch (a chorus at the bottom, a cloud three octaves
tall at the top) without touching what you see or the stereo image. The
listener pair stands on the front face of the unit cube; a lagging bird's
pitch difference is drawn in cube units and held to the cube's reach.
Speeds are in cube units per second, so AGILITY means the same thing at
every LATITUDE. Measured at LATITUDE 3: pitch spread 0.2 to 0.6 semitones
rms from 11 to 256 birds; correlation 0.65 at rest, 0.40 after a startle.

## Ninth listen: a thousand birds, VARIETY on the panel, the pair follows

- **WEIGHT runs to 1024.** The neighbour search is a grid whose cell
  follows the flock's spread (every pair was a million distances), and
  the physics tick dropped to 187 Hz. That changed almost nothing, which
  is how it came out that **the cost is the voices, not the birds**: 64
  grains each taking an exp2 and two raised cosines per sample were most
  of the bill. The chirp's frequency is now refreshed every 16 samples,
  the envelope is a smoothstep, and a voice's partials come from the
  fundamental's own sine and cosine. 1024 birds went from 15% of a core
  to 8%.
- **Shape.** Global cohesion is weak (0.15), alignment stronger (1.2), and
  the whole flock shares a slowly wandering *drift* it leans toward, so
  it streams round inside the cube and folds on itself rather than
  sitting as a ball; the leash bends it back. Elongation, the ratio of
  the longest to the shortest axis, measures 1.4 to 1.7 against 1.0 for a
  ball. The dots are smaller (0.2 mm) and a call lightens a bird to grey;
  orange over a thousand birds read as sparks.
- **The pair turns its head.** Once the flock could drift, a pair staring
  straight ahead heard it off to one side and panned the whole flock
  there (measured: L 0.08 V, R 2.03 V). The image is now centred on the
  flock's centre and its width is the flock's spread about that, which is
  what the picture shows. A **Stereo width** menu (Normal, Wide, Extreme:
  pan sharpness 4, 8, 16) is there to test; correlation at Wide is 0.58 at
  rest and 0.23 after a startle.
- **VARIETY is a panel control with a jack**, sixth in the second row.
- **LAG defaults to 0.3** (the slowest bird 0.36 s behind): a whole flock
  snapping to a new pitch on one tick was a jump, not a flock.

## Flock In: live audio, as a 4HP expander on the left

Patched to Flock's left, the birds stop singing sines and sing grains of
whatever comes in. The expander is deliberately dumb: L and R in (R
normalled from L), three controls, and one message a sample over the
expander bus. The ring buffer lives in Flock (2^19 frames, 10.9 s at 48k),
because Flock's birds are what read it and reading another module's memory
across the bus is the race the message protocol exists to avoid.

- **REACH** (pot and CV, 50 ms to 10 s) is how far back the birds may
  read. A bird's **depth** is where in that window it reads, near birds
  recent and far ones old, so the flock's shape along z is a smear in time
  and a startle scatters the flock across the last few seconds.
- **ENV** (pot and CV) is the grain's attack as a fraction of the call, from
  5 to 95 percent; the sine calls keep their fixed quarter.
- **FREEZE** is a latch with a gate input: the write head stops and the
  birds keep singing the buffer as it was.
- **Pitch is playback rate.** Unity when the call sits on 0 V, so the input
  plays at its own speed and V/OCT is varispeed; LATITUDE spreads the rates
  round it, CHIRP glides them within a call, QUANT snaps them. The register
  menu is taken back out, since a sample has no frog problem.

Measured with the real pair wired as the engine would wire them (Flock In's
message flipped by hand): a 440 Hz input comes out at 444 Hz with LATITUDE
at its bottom, an octave on V/OCT doubles it, and FREEZE stops the head
dead while the birds go on singing. The first build subtracted V/OCT from
the rate twice and every octave played at the same speed.


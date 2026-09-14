# Flock: design

*Status: design only, nothing built. When it is, it ships hidden until it is
discussed, like every new module.*

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

- **CENTRE**: the flock's actual centroid height as 1V/oct. The premise is
  that the module does some of what it wants, and telling the composer
  where it went closes that loop. Patch it back to a quantizer or to
  another Flock.
- **DENSITY**: calls per second as a CV.
- **HAWK**: gate, above.

## Panel, first draft

Around 18HP, screenless, the flock across the top.

- Row 1: WEIGHT, LATITUDE, STRUCTURE, AGILITY, LAG, ROTATE.
- Row 2: LENGTH, CHIRP, MORPH, RATE, RELEASE.
- CV in for every knob in the two rows (plus or minus 5 V, full range).
- V/OCT, GATE, STARTLE (jack and button).
- Out: L, R, CENTRE, DENSITY, HAWK.

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

- STRUCTURE in v1, or flock rules only to start.
- The bird ceiling, once measured.
- Whether a bird calls more while turning (it would make the shape audible
  as rhythm), or at a rate independent of its motion.
- A poly out of the sixteen nearest birds, later, if a patch wants them.

## Name

Flock. Alternatives considered: Murmur, Skein.

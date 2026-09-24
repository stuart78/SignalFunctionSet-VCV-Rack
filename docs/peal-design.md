# Peal: a grid of bells joined by rods

Design written 2026-09-22 and built hidden the same day; every item under "What to measure" passes in `tools/peal-harness.cpp`. A peal is a set of bells rung in
sequence, which is what a ripple through the grid is.

## The idea

Twenty-five bells of different sizes on a 5 by 5 grid, covering two octaves.
Neighbouring bells are joined by rods that the player lays out on the screen.
A strike puts energy into one bell, and the energy travels down the rods to
its neighbours, and theirs, losing some at every crossing and stopping when
there is too little left to pass. Velocity is how much energy a strike
delivers, so a soft strike rings one bell and a hard one ripples across the
grid. The rods carry the striking bell's partials into bells tuned elsewhere,
which answer on whatever overtones they share, so the tuning of the grid
decides what every rod sounds like.

## The bells

Each bell is a modal resonator, as Kit's membranes are, with five partials in
bell ratios rather than Bessel ones: hum, prime, tierce, quint and nominal,
the minor-third stack that reads as a bell at any size. Size sets the
fundamental. A smaller bell also decays faster and sits brighter, which is
what makes a grid of sizes sound like a set rather than one bell transposed.

**SIZE** (pot, CV) scales every bell together, a set of hand bells at one end
and a carillon at the other. **DAMP** (pot, CV) is a hand on every bell, and
it is also what tames a network that would otherwise sustain itself, since
rods conduct both ways and energy can come back to the bell that sent it.

## The grid and the key

The grid is chromatic across its two octaves, and its layout is a menu
choice. The default is the Tonnetz: a step right is a fifth and a step down
a major third, so every triangle of neighbours is a triad and a ripple through
the grid is a chord moving. Chromatic rows (a semitone right, a fourth down)
and whole-tone rows are the alternatives.

The bells are fixed, as a set of bells is. **ROOT and SCALE** in the plugin's
convention do not retune them; they **mask** them. A bell outside the key has
a felt on it: it is struck quietly, rings briefly, and still passes energy
along its rods, so a ripple crosses an out-of-key bell without stopping and
without sounding it. Change the key and different bells are muffled, on the
same grid. A menu option lifts the felts for a chromatic instrument.

## The rods

A rod is a lossy delay with a floor. Energy arriving at one end is partly
reflected back into its own bell, partly passed along after the rod's
crossing time, and some is lost. Below the floor nothing passes, and that
floor is what makes velocity decide reach. The loss per crossing is tuned by
measurement so that a full mesh at maximum velocity rings out in a few
seconds at DAMP's default and can be held longer, not so that it runs away.

**REACH** (pot, CV) scales the floor, so a gentle player can still send a
ripple across the grid and a heavy one can be confined to neighbours.

**SPEED** (pot, CV, and a CLOCK input) is how long energy takes to cross a
rod, 5 ms to 2 s free-running. **When CLOCK is patched, a crossing is always
one pulse.** The ripple then steps on the clock, bell by bell, and the grid
becomes a sequencer whose pattern is the network you drew and whose length is
how hard you hit it: a hard strike is a long run, a soft one a short one, and
where the run goes is the rods.

## The grid, drawn on the faceplate

No screen. The grid is drawn straight onto the panel in the screenless style
Wheel and Flock use, in the panel's own greys with the plugin's orange for
energy, seen from a slightly foreshortened angle: the camera stands a little
above the front edge and looks down across the grid, so the back row is
smaller and closer together than the front, the bells read as objects of
different sizes standing on a surface, and a ripple has depth as well as
width. The mics stand on the same surface, so where they are is visible in
the same picture as what they hear.

Drag between two neighbouring bells to lay a rod, drag again to remove it.
Click a bell to strike it, with the vertical position of the click as
velocity. Each rod draws with the energy in it, so you watch the ripple
travel, and each bell rings visibly by its level. The hit-test is the same
projection run backwards, as Kit's is, and a harness will hold the round
trip exact before the grid is trusted with a mouse.

Presets in the menu for the networks worth having to hand: full mesh, rows,
columns, a spiral from the centre, a single chain snaking through all
twenty-five, and clear.

## Playing it

Polyphonic V/OCT, GATE and VEL. Each note is quantised to the nearest bell and
struck with its velocity; a mono VEL cable sets every voice. A STRIKE trigger
input with a BELL CV (1 V per octave, the same quantisation) is the
sequencer's way in.

## Listening to it

**Stereo out**, from two microphones the player places on the grid by
dragging them on the screen, the way Kit's are dragged on its head. Each bell
reaches each mic with its own distance, so a ripple across the grid is a
ripple across the field, and moving a mic moves the image. Positions are
saved with the patch, and a menu item resets them to a wide pair.

**POLY out**, the sixteen loudest bells on sixteen channels, each bell
keeping its channel while it rings, so a poly cable to a VCA or a filter bank
treats the grid as sixteen voices.

## Panel, first draft

About 26 HP. The grid across the top, drawn on the faceplate, tall enough to
read at the foreshortened angle.
Under it a row: SIZE, DAMP, REACH, SPEED as pots over CVs, then ROOT and SCALE
with their CVs. At the foot: V/OCT, GATE, VEL (poly), STRIKE, BELL, CLOCK,
and the outputs on a plate: L, R, POLY.

## What to measure before believing it

- **Reach follows velocity.** Over a single chain of 25 bells with REACH at
  its default, count the bells that ring above a floor at velocity 1, 4, 8
  and 16 V, and require the count to rise with every step and to reach at
  least ten at full velocity.
- **Nothing runs away.** A full mesh struck everywhere at full velocity, DAMP
  at its default, decays below the floor within a stated number of seconds
  and never grows; at DAMP minimum it still decays.
- **The clock is the crossing.** With CLOCK patched at 4 Hz, the bells along
  a chain start ringing 250 ms apart to within a millisecond, whatever SPEED
  says.
- **The mask conducts.** A ripple through a muffled bell reaches the bell
  beyond it, and the muffled bell's own level stays under a stated fraction
  of an unmasked bell's.
- **Complementary resonance is real.** A bell a fifth away answers louder on
  the shared partial than a bell a semitone away, measured from the receiving
  bell's spectrum.
- **The mics move the image.** A strike at the left of the grid lands left,
  and swapping the pair over lands it right.
- **The projection round-trips.** Every bell's drawn position, run back
  through the hit-test, returns that bell, at both ends of the foreshortening.
- **Cost** at 25 bells ringing with a full mesh, in percent of a core.

## What the first build found

- **The rod fraction is 0.85 of what arrived, split by degree.** The first
  figure was 0.55, and it reached six bells at every velocity: a chain
  interior bell splits two ways, so each hop kept 0.275 and the floor arrived
  in six hops however hard the strike. At 0.85 a hop keeps 0.425, the mesh
  still decays (0.85 in, at most 0.85 out), and with a cubic velocity curve
  the chain rings 3, 6, 8 and 10 bells at 1, 2.5, 5 and 10 V.
- **Velocity is cubed** into energy, so a soft strike stays local. Squared,
  1 V already reached three bells and 10 V only six.
- **A bell left of both mics is closer to the left one**, so moving both
  mics right does not move it right; swapping the pair does. The measurement
  now says so.

## What the first listen changed

- **More bell.** Five partials sounded like a tuned sine bank. A bell now
  has eleven: hum, prime, tierce, quint, nominal, deciem, duodeciem and a
  double octave, with the prime, tierce and nominal as **doublets**, a twin
  a few cents off, because a cast bell's modes come in near-degenerate pairs
  and their slow beating is most of what the ear calls a bell. The upper
  partials are loud at the strike and gone within a second; the hum outlasts
  everything. A **clapper** burst, a few milliseconds of noise through a
  bandpass round the upper partials, is the clang that says something struck
  it. A rod arrival gets a quarter of the clapper.
- **Rods are dragged.** Press on a bell and release on another to lay a
  rod (this first allowed neighbours only, and painted paths; see the second
  listen). A press and release on one bell strikes it.
- **Bells are drawn as bells**, hanging from their crowns, and swing when
  struck, a rocking that dies over a second or so. Felted bells are hollow.
- **Ant trails.** Every impulse in flight is a run of dots along its rod,
  the head brightest, so you watch the energy travel and see where it lands
  next. Free-running the head is at its share of the crossing time; clocked,
  at its share of the last clock period.
- **Speed was wrong twice.** A strike that arrived on the same sample as a
  clock pulse crossed on that pulse, so a sequencer clocked from the same
  source struck and crossed at once and the first hop was invisible. A
  crossing now always waits for the next pulse; measured, a gate on the
  pulse's sample lands a full period later. Free-running, the range was 5 ms
  to 2 s with a 74 ms default, which read as instant; it is now 10 ms to 4 s
  with 360 ms at the default, and clockwise is faster, which is what a knob
  called SPEED should do.
- **ROOT reads as a note name**, as the rest of the plugin's roots do.

## The second listen

- **Any two bells.** Rods are no longer limited to neighbours: press on a
  bell and release on any other and a rod joins them, again and it goes.
  The matrix is symmetric over the 25 bells and is saved as pairs. A longer
  rod takes proportionally longer to cross free-running (measured, a corner
  to corner rod at 5.66 times a neighbour's time); clocked, every rod is one
  pulse whatever its length. Path painting went with this, since a straight
  drag across the grid now means the two ends, not the bells between.
- **The bell was a plink because DAMP was strangling it.** The decay table
  topped out at five seconds and the DAMP default multiplied the loss by
  eight, so the prime at C4 rang for under a second. The table now runs to
  fourteen seconds for the hum and nine for the prime, DAMP is squared with a
  gentler ceiling and a lower default, and the strike is a **mallet
  contact**, a raised-cosine force over a few milliseconds that scales with
  the bell's size, instead of a one-sample impulse. The impulse gave every
  partial the same kick and the strike read as a switch; the contact gives
  the high partials less and the strike reads as something hitting metal.
  The clapper noise stays, quieter.
- **The drawing** is a profile now: crown knob, a rounded shoulder, a waist,
  a sound bow flaring to the lip, and the lip's rim as an ellipse because
  the view is from above, with a highlight down the shoulder so the body
  reads as round.

## The third listen

- **SHAPE and BRIGHT.** SHAPE morphs the partial set from a cast bell to a
  free metal bar, geometrically in frequency, with the bar's own amplitudes
  and much shorter decays: a glockenspiel or vibraphone key rather than a
  bell. BRIGHT is the mallet's hardness and a tilt of the partials.
- **Why it was muffled**, three causes found by measuring the spectral
  centroid of one struck bell: the mallet contact was 1.5 ms at C4, and a
  Hann contact of T delivers almost nothing above 2/T, so nothing above
  700 Hz was being struck; a bar's partials at 5 and 9 times the fundamental
  need a contact under 0.3 ms, so SHAPE shortens it as well as BRIGHT; and the
  tables were displacement amplitudes, where a struck body radiates its
  higher modes more efficiently, roughly with the square root of frequency.
  With all three, BRIGHT moves the centroid from 223 Hz to 454 Hz and a bar
  sits above a bell.
- **Hover names the note**, with its octave at the size in force, and says
  when the bell is felted, because a Tonnetz cannot be read off a grid of
  identical bells.
- **The grid fills the display**: the front row spans most of the width and
  the plane starts closer to the front edge, the display is 56 mm tall, and
  the pot row is eight across.

## Open questions

- Whether a rod should carry pitch as well as energy, so a bell struck hard
  bends its neighbours a few cents on arrival, as a real coupled mechanism
  does. Probably later.
- Whether the POLY out should be the sixteen loudest or the sixteen most
  recently struck. Loudest first; recently struck as a menu option if it is
  missed.

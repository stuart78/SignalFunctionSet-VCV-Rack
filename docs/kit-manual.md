# Kit Manual

## Overview

Kit is a struck membrane, modelled mode by mode. A drum head is a 2D wave
equation on a disc, and its solutions are the Bessel modes. Rather than run a
mesh over the disc, Kit runs one resonator per mode, which is cheaper and puts
every parameter you would want to play in plain sight: strike position becomes a
per-mode gain, and muffling becomes the same calculation used subtractively.

One instrument covers tom, timpani, kick, snare, gong, steel pan, frame drum and
tabla, because those differ in shell depth, head stiffness and air cavity rather
than in kind.

Kit holds **eight instruments** at once, and channel N of every cable is
instrument N. The panel's knobs edit whichever instrument is selected on the
screen; the other seven keep their own settings and play on their own channels.

Kit is 22HP.

## Eight instruments, one panel

The screen has a row of eight tabs along its foot, one per instrument, each a
small drawing of that drum which moves when it is struck. Click a tab and the
panel's knobs show that instrument; turn a knob and only that instrument
changes. A level bar under each tab shows what each drum just did, on one
baseline, so the balance of the kit is visible at a glance.

A new Kit comes loaded with a default kit that is Fill's eight channels, in
Fill's order: Kick, Snare, Closed hat, Open hat, Low (floor tom), High (tom),
Clap, Bell. A poly cable from Fill plays the right drum on every channel with
nothing to map. Right-click a tab to load any preset into that instrument, or
to reset its mics; the module's own menu does the same for the selected one,
and can reload the default kit into all eight.

**Channel N is instrument N.** A polyphonic GATE fires instrument N from channel
N; a mono gate fires instrument 1 only. VEL, V/OCT, X, Y and the eight CV
inputs read channel N for instrument N, and a mono cable applies to all eight,
which is the usual convention for modulation.

A drum that is not ringing costs nothing. The cost of the module follows how
many drums are sounding, not how many are loaded.

### Striking a drum that is still ringing

A new trigger neither stops the old hit nor starts a second voice. It is one
drum, and the mallet lands on a head that is still moving: the strike re-arms
the mallet and re-latches the mic delays, and never clears the mode bank. The
new hit's energy adds to whatever is still ringing, exactly as on a real drum,
which is why a fast roll on a ringing tom sounds different from the first hit
and why two hits close together can sum louder than one.

That is per instrument. Hitting instrument 3 does nothing to instrument 2's
ring. There are no choke groups yet (a closed hat cutting an open one); that
is planned alongside the voice work below.

### Mics per instrument

Every instrument has its own mic pair, saved with the patch. The screen shows
the mics of the selected instrument, and dragging them moves only that
instrument's pair. *Reset mic positions* on a tab's right-click menu restores
that instrument's measured pair; the module menu does the same for the
selected one.

## PolyKit In

If your sources are eight separate modules rather than one polyphonic one,
place **PolyKit In** immediately to the left of Kit. It is a matrix of jacks:
thirteen columns, one per Kit input (TRIG, V/OCT, VEL, X, Y and the eight CVs),
by eight rows, one per instrument. Every cell is a mono jack for that
instrument's input. A patched jack takes over that channel from Kit's own poly
cable; an unpatched one leaves the poly cable in charge, so a poly trigger
from Fill and a single hand-patched tension can coexist. PolyKit In is 33HP.

## The one thing to know about the parameters

For an ideal membrane, `f = (c / 2πR) · j_mn` with `c = sqrt(T/σ)`. Radius and
tension scale every mode by the same factor and leave the *ratios* untouched.
Acoustically, SIZE and TENSION are therefore the same control, and shipping both
as separate knobs would be shipping a duplicate.

What breaks that scale invariance is bending stiffness, the air cavity, and
damping that depends on size. That is why MATERIAL and AIR exist, and it is why
SIZE is allowed to exist alongside TENSION: a small tight drum and a large slack
one at the same pitch are different instruments, and those three controls are
where the difference lives.

## Controls

The panel is eight columns by four rows. Every control is a trimpot, on one
pitch, so sixteen of them read as one instrument rather than as four ideas
competing for the same panel.

### Row 1, what the drum is

| Control | Range | Notes |
|---|---|---|
| SIZE | 6 to 22 inches | The tooltip reads inches and centimetres, not a percentage. |
| TENSION | ±8.4 semitones | Head tension, read as a pitch offset. |
| MATERIAL | drum head to gong | Bending stiffness. Pushes the modes from a membrane's inharmonic ratios toward a plate's. |
| AIR | open shell to sealed kettle | Closes the bottom. Wound up, it pulls the modes into a kettledrum's harmonic series. |
| DECAY | 80 ms to 7 s | Scaled by SIZE as well, and the tooltip says so. |
| TONE | | How much faster the high modes die than the low ones. |
| EXCITER | soft felt to hard stick | Reads as a beater name and a contact time in milliseconds. |
| MUFFLE | 0 to 100% | A hand on the head. Subtracts the modes that are live at that spot. |

### Row 2, how it is played and dressed

| Control | Notes |
|---|---|
| COUPLE | How much the batter head drives the resonant head. |
| RESO | Resonant head tuning, as a ratio to the batter head. |
| BEND | Pitch drop after the hit, from the head being driven hard. |
| WEIGHT | Beater mass, 10 to 70 g. |
| WIRES | Snare wire amount. |
| TIGHT | Wire tightness, loose buzz to tight snap. |
| LEVEL | Output trim. |
| PEDAL | The hi-hat's pedal, for an instrument that is a hi-hat (below). Closed hard at the bottom, the plates just touching and rattling through most of the travel, apart at the top. Does nothing on a drum head. (This was HIT, a strike button; click the head on the screen instead.) |

### Row 3, CV

One jack per row-1 control, in the same order, directly underneath. A cable
hangs under the thing it modulates, so the pairing needs no label.

## Inputs

Row 4 is the transport row: performance data in and out, nothing else.

All inputs are polyphonic, channel N for instrument N.

| Input | Notes |
|---|---|
| TRIG | Strikes on a rising edge. Channel N fires instrument N; a mono cable fires instrument 1. |
| V/OCT | 1V/oct. A mono cable pitches all eight. By default 0V is the drum as its knobs tune it; see *V/OCT reference* below for 0V = C4. |
| VEL | Velocity, 0–10V. |
| X, Y | Strike position, ±5V, summed with the STRIKE X/Y parameters. |
| CV row | One CV per voice control, channel N for instrument N. |

## Outputs

| Output | Notes |
|---|---|
| POLY | Sixteen channels: instrument N on channels 2N-1 (left) and 2N (right). With Stereo pairs off, left and right carry the same mono signal. |
| MIX L, MIX R | All eight summed, in stereo. |

Head and wires are summed per instrument. Eight instruments in stereo is
exactly sixteen channels, which is all a cable holds, so the wires no longer
have a jack of their own; a snare that wants its own compressor takes its own
pair off the poly out.

A patch saved before Kit had eight instruments still works unchanged: its drum
becomes instrument 1, a mono gate still fires it, and channel 1 of the poly out
is still its left.

## The display

The head is drawn as it moves. Rings and spokes trace the modal displacement,
excited areas warm toward orange, and the strike mark sits where X and Y put it.
Click or drag on the head to strike it, and the position you click becomes the
strike position.

Three details are load-bearing rather than decorative:

- **The grid redistributes with TENSION.** A taut head pulls its rings out
  toward the rim; a slack one lets them gather in the middle. That is what
  tension does to a real head's response.
- **AIR closes the bottom.** At zero it is an open shell with a resonant head
  across it, which is a tom or a kick. Wound up it bellies into a sealed bowl,
  which is a kettledrum. A kettledrum is what the harmonic series AIR
  imposes belongs to.
- **The snare wires appear one at a time** as WIRES comes up, and they buzz
  while the drum sounds. They are drawn as straight lines rather than as coils.
  A coil is the more accurate picture, and at this size it read as a braid
  wrapped round the shell, so the accurate drawing was the wrong one.

A small spectrum in the corner answers the one question the head cannot: which
modes are actually sounding.

## Context menu

- **Head view**: Flat or 3D.
- **Stereo pairs** (on by default): two mics in the air over each head rather
  than one on it, so the strike position moves the drum in the image. Per
  instrument, and the mics are dragged on the head of the selected instrument.
  With it off the mics are not drawn, since they do nothing.
- **Reset mic positions**: the measured pair, for the selected instrument.
- **V/OCT reference**, per instrument, off by default. Off, 0V is the drum's
  own pitch from SIZE and TENSION, so a kick and a hat on one mono cable stay a
  kick and a hat. On, 0V is C4 (261.63 Hz) as in the rest of Rack: SIZE stops
  moving the pitch and keeps the diameter, the decay and the stereo image, and
  TENSION becomes the fine tune, a fifth either way and exactly on the note at
  its centre. It applies only while that instrument's V/OCT is fed, so an
  unpatched jack does not pull the kit to C4. The readout names the note. The
  default kit at 0V, for reference: Kick 48 Hz (about G1), Floor tom 78 Hz,
  Tom 110 Hz (A2), Snare 190 Hz, Clap 205 Hz, Bell 281 Hz, the hats 309 Hz.
- **Load into instrument N**: Tom, Floor tom, Timpani, Kick, Snare, Brush snare,
  Gong, Steel pan, Frame drum, Tabla, Closed hat, Open hat, Clap, Bell. The
  first ten are the instruments the engine was measured against while it was
  being built, so they are also the shortest route to hearing whether something
  has broken. The last four are provisional; see below.
- **Load the default kit into all eight.**

Right-clicking a tab gives the same menu for that tab's instrument: every
preset, *V/OCT: 0V = C4* and *Reset mic positions*.

## The hi-hat: two plates

Closed hat and Open hat are not membranes. An instrument can be a **hi-hat**
instead (the Closed hat and Open hat presets make it one, and so does the tab's
right-click menu or the module menu): two modal plates on a rod that collide
round a tilted rim, with the dense top of their spectrum above 5 kHz modelled
as an energy field, because a real plate has thousands of modes there and they
are heard as noise. The model and how it was arrived at are in
`docs/kit-hat-design.md`.

The two presets are the same pair of plates, and differ only in PEDAL: closed
is pressed hard, open is apart. Turn PEDAL on either and it becomes the other,
through touching and half open, where the plates rattle against each other.
The screen draws the pair, the gap between them following the pedal.

On a hi-hat the knobs mean: SIZE the diameter (14 inches is the preset),
TENSION and V/OCT the pitch, DECAY how long the clutch lets it ring, TONE its
brightness, EXCITER the stick, WEIGHT the stick's weight, MUFFLE a hand on the
top plate, STRIKE X/Y bow or edge, LEVEL, and PEDAL. AIR, COUPLE, RESO, BEND
and the wires do nothing to a hat. The mics are fixed, so they are not drawn.

A hi-hat costs about 2% of a core while it rings, and nothing when quiet.

## Known gap: Clap, Bell

These two presets exist so that the default kit lines up with Fill's eight
channels, and they do not yet sound right. Measured through the real engine
(`tools/kit-voice-harness.py`), the reasons are not preset settings:

- **Everything is trapped below 2 kHz, and the mallet is the wall.** The
  hardest EXCITER still gives a 1 ms contact, which cannot excite a plate's
  high modes; a stick tip on a cymbal is a tenth of that. Raising the contact
  stiffness a hundredfold takes the closed hat's centroid from 1.7 to 10 kHz.
- **The output tilt is a membrane's.** The pickup rolls off high partials
  twice over, which is right for a drum and puts every partial of a plate
  15 dB under its fundamental, which is why the bell reads as one tone.
- (Closed and open hat were in this list; they are now the hi-hat above.)
- **A clap is not a drum**: it is three or four hits a few milliseconds apart
  through a hand-cavity resonance, and nothing here makes bursts.
- **The bell cannot get high enough**: the fundamental tops out near 600 Hz,
  and the plate's partial ratios are a cowbell's rather than a bell's.

The plan, not yet built: contact stiffness and output tilt keyed to MATERIAL
so every membrane stays bit-identical, a shorter decay floor for plates, and
per-instrument strike count, noise band and octave so a clap and a bell can be
described at all. Until then, treat those two as placeholders.

## Patch ideas

**A kit from one module.** Take three instances, load Kick, Snare and Floor tom,
and clock them from Beat. The three share nothing, so they detune and decay
independently the way a real kit does.

**Timpani that follow a line.** Load Timpani, patch V/OCT from Note or Chance,
and put a slow LFO on TENSION CV. The pedal glide comes from tension, not from
pitch, so it bends the way the instrument does.

**Playing the head.** Patch an LFO into X and another, slower, into Y. The
strike position walks around the head and the timbre moves with it, because
position is a per-mode gain rather than a filter.

**Gongs.** Load Gong, wind MATERIAL up, DECAY long, and strike hard with
velocity. MATERIAL is bending stiffness, so a high setting is genuinely a plate
rather than a membrane with a longer tail.

## Technical notes

The mode table is `j_mn`, the zeros of the Bessel functions, sorted by
frequency and computed offline by bisection. It reproduces the textbook series
1, 1.594, 2.136, 2.296, 2.653, 2.918, 3.156, 3.501 to three decimals.

The first attempt at generating it scanned upward from zero and "found" roots at
x = 0.00002, because `J_m(0) = 0` for m ≥ 1 and rounding noise flips the sign
down there. `J_m` has no zeros below x = m.

The DSP lives in `src/membrane.hpp` so it can be measured without Rack in the
way. That is how the mallet's 38 ms contact times and its 296-bounce chatter
were found.

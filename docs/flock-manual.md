# Flock Manual

## Overview

Flock is a murmuration as a voice. Up to a thousand birds fly as a real
flock, each steering by its seven nearest neighbours, and every call a bird
makes is a grain. Height in the flock is pitch: the V/OCT input is the roost
the flock leans toward, LATITUDE is how far above and below it the birds may
range, and a pitch change travels through the flock the way information
does, bird to bird, so the flock arrives at a new note rather than jumping
to it. The stereo image is the flock heard from a pair of listeners standing
at its edge, so a startled flock scattering past them is a startled stereo
field.

The display is the flock drawn in perspective on the faceplate, with the
listeners, the floor and the walls of the box the birds are leashed to. Drag
it to move the camera; double-click to put the camera back.

Flock is 18HP. Flock In, its 4HP expander, sits immediately to its left and
turns the calls into grains of live audio.

## Controls

Each control has a trimpot and a CV input beneath it; plus or minus 5 V
covers the whole range of the knob.

- **WEIGHT** is how many birds, 1 to 1024. The cost is in voices, not birds:
  a thousand birds and one bird sound the same number of grains at once.
- **LATITUDE** is how far the flock may stray from the pitch, in semitones:
  a tenth of a semitone at the bottom, three octaves at the top. The birds
  keep their shape whatever it is set to; LATITUDE only says how much pitch
  that shape is worth.
- **STRUCT** pulls the flock into wells at just intervals of the pitch, so
  the birds gather at the octave, the fifth and the third rather than
  spreading evenly.
- **AGILITY** is how fast the flock changes shape.
- **LAG** is how late the slowest birds hear a new pitch, up to four
  seconds. At zero the flock moves as one; turned up, a new note spreads
  through it from the birds that heard first.
- **ROTATE** walks the listener pair round the flock, a full turn across the
  knob or 10 V.
- **LENGTH** is the call, 10 ms to 300 ms.
- **CHIRP** is the pitch glide inside each call, up to seven semitones
  either way.
- **QUANT** snaps each call toward the grid chosen in the menu. At zero the
  calls are wherever the birds are, microtonally; at full every call lands
  on the grid.
- **RATE** is how often each bird calls, 0.2 to 20 a second. Calls are not a
  metronome: each bird draws its next call from an exponential distribution,
  and some birds talk more than others, so calls bunch and thin the way a
  flock's do.
- **RELEASE** is how long the flock takes to settle after the gate falls.
- **VARIETY** is how far the birds differ from one another: their voices,
  and the syllables of their calls.
- **STARTLE** sends a hawk through the flock. The button and the jack do the
  same thing.

## Inputs and outputs

- **GATE** high and the flock flies and calls. It is flight, not a trigger:
  the calls come from each bird's own clock. With the menu option **Lead bird
  calls on the gate** (on by default) one bird calls the instant the gate
  rises, which is what makes a small flock playable from a keyboard; with a
  thousand birds it is one call among the fan-in.
- **V/OCT** is the roost, 1 V per octave.
- **STARTLE** is a trigger.
- **LEFT** and **RIGHT** are the flock as heard from the listener pair.

## The stereo field

The pair stands half a leash from the roost, at the edge of a settled flock,
and turns its head to follow the flock's centre. Each call is panned by its
bearing from the pair, divided by the flock's own spread, so a tight knot of
birds fills the field as much as a loose cloud does. The spread it is
divided by is the settled one, and it is held while the hawk is in the
flock, so scattering birds fly past the speakers rather than being
re-centred as they go. Level and brightness fall with distance.

**Stereo width** (context menu) sets where the typical bird lands: Normal
puts it most of the way to the speaker, Wide (the default) just past it, and
Extreme well past, so most of the flock sits hard left or right.

## Context menu

- **Register**: the roost is V/OCT plus 0, 1, 2 or 3 octaves. Default +1.
- **Quantize grid**: the grid QUANT snaps toward, as offsets from the pitch:
  Semitones, Whole tones, Root and fifth, Octaves, Root third fifth, Root
  minor third fifth, Root third fifth seventh, Pentatonic, Fourths and
  fifths.
- **Stereo width**: Normal, Wide, Extreme.
- **Lead bird calls on the gate**: on by default.

## Flock In

Flock In is the 4HP expander that sits immediately to Flock's left. Patch
audio into LEFT and RIGHT (RIGHT is normalled from LEFT) and the birds sing
grains of it instead of sines: each call reads a short piece of the last few
seconds of input, pitched by where the bird is in the flock and by V/OCT.

- **ENV** is the shape of each grain as the position of its peak: near zero
  a struck grain, quick in and long taper; a quarter of the way is the bird
  call; halfway is symmetric; near the top a reversed swell. Both halves are
  smooth, so there are no corners whatever the split.
- **REACH** is how far back into the buffer the birds may read, 50 ms to
  10 s. Birds deep in the flock read further back than birds at the front,
  so the flock is spread through time as well as space.
- **FREEZE** holds the buffer, from the latch or a gate above 1 V, so the
  flock keeps singing what it last heard.

Each control has a CV input beneath it; plus or minus 5 V covers the knob.
Flock In has no outputs of its own; it speaks to the Flock on its right.

## Tips

- **A note that arrives.** LAG up, WEIGHT a few hundred, and play slow
  changes: each new note spreads through the flock over a second or two.
- **Chords from one voice.** STRUCT up and QUANT toward a triad grid: the
  flock gathers at the chord tones and the calls land on them.
- **The hawk as a performance gesture.** A trigger into STARTLE on a
  downbeat scatters the field and lets it settle over RELEASE.
- **A single bird.** WEIGHT at the bottom with the lead bird on is a
  microtonal chirping monosynth that follows a keyboard.

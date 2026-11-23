# Garamut design

Garamut is a slit drum: a hollow column with a long slit cut into one side. The
wall on either side of the slit is a lip, and the two lips are thinned by
different amounts, so each one has its own note. The mallet strikes the left or
the right lip, at a height Y up the slit. Every polyphonic channel plays the same
drum, so a strike can land on a lip that is already ringing.

The engine is `src/garamut-engine.hpp`, free of Rack. `tools/garamut-proto.cpp`
measures it and renders listening WAVs into `renders/garamut/`. The module is
`src/garamut.cpp` (20HP, hidden), and `tools/garamut-harness.cpp` compiles it
against libRack and checks the gates, the roll, Y and VEL per channel, the
screen's strike, the level with every channel rolling, and the cost.

## Controls

| Control | What it changes |
|---|---|
| SIZE | Column height, 0.3 m to 2.5 m. Everything scales with it, including the mallet. |
| SHAPE | Square column (0), round column (0.5), pear (1). |
| MATERIAL | Cedar, hardwood, aluminium, steel, blended in between. |
| SLIT | The slit's length (30% to 75% of the height) and width (5% to 30% of the radius) together. |
| MALLET | Felt, rubber, wood, brass, blended in between. |
| INTERVAL | Semitones from the left lip's note up to the right lip's. |
| Y | Height of the strike up the slit, per channel. |
| DECAY | Ring time, 0.25x to 4x. |
| ROLL RATE | Strokes per second while ROLL is high, 4 to 30 (1 V per doubling on its CV). |
| TUNE, V/OCT | Transpose the whole drum. |

## Inputs

GATE L and GATE R strike the left and right lip on each rising edge, one strike
per channel. Y and VEL are read from the same channel as the gate (a mono cable
sets every channel, and VEL unpatched is 70%). Every trigger runs every sample,
with 0 V where a cable has no channel, because a Rack trigger whose first
reading is already high does not fire.

ROLL is a polyphonic gate. While channel N is high, channel N strikes at ROLL
RATE: the first stroke on the rising edge, then two hands taking turns. Each
stroke lands 1 / rate after the last, give or take 6%; the second hand strikes
8% lighter and a little off the first's spot, and each stroke's velocity varies
by 5%. Those numbers are estimates of a steady hand roll. VEL is read at every
stroke, so an envelope into VEL makes a crescendo. The roll stays on the lip
that channel last struck through a gate (the left one if it has struck
neither), or alternates between the lips with the menu's "A roll alternates the
two lips".

## The drawing

There is no screen. The column is drawn on the faceplate in the panel's own
inks, as Wheel, Flock and Canopy are, with a light and a dark palette, from the
same geometry the engine uses: square, round or pear, the slit at its real
height and width. Each lip fills orange as it rings.

Every channel arriving on GATE L, GATE R or ROLL has its own mallet, drawn
whether or not it is striking. It rests just outside the column's outline at
that channel's Y, beside the lip it last struck (before its first strike, the
side of the input it arrives on). On each side the first channel's mallet rests
closest; each further one rests a step further out and alternately above and
below it (the second a step above, the third a step below, the fourth two steps
above), the steps shrinking so sixteen still fit beside the drum. A stroke carries the head in to the strike point and
back out, and the mallet turns orange for a moment, brighter for a harder strike.
Click a lip to strike it there. The lips' notes and the interval between them
are at the top, the height and the Helmholtz frequency at the foot.

## The model

Three parts make the sound.

**The lips.** Each lip is a plate held along three edges (the two ends of the
slit and the line where it joins the wall) and free along the slit. It has 18
modes: 6 along the slit, shaped as a beam clamped at both ends, times 3 across
the lip, shaped as a beam clamped at one edge and free at the other. The
frequency of each mode is the plate's bending stiffness, with wood ten times
stiffer along the grain than across it, plus a term for curvature. A closed
cylinder is much stiffer than a flat plate of the same thickness, but a slit
cylinder can bend without stretching, so only 5% of a closed shell's membrane
stiffness is kept. That factor is an estimate.

**The air through the slit.** The cavity and the air in the slit form a
Helmholtz resonator, `f = c/2pi sqrt(A / (V L))`, with L the wall thickness plus
an end correction for a slot. The lips push air in and out of the cavity, and
the slit's flow follows that through the resonance. Below the resonance the
slit's flow cancels the lips' flow, so the column radiates little there, as in a
bass-reflex speaker.

**The air up the column.** The column is closed at both ends. Its higher air
modes come from Webster's equation on the cross-section's area along the height,
solved once per shape at startup (129 shapes, 32 cells each). A round or square
column gives harmonic modes. The pear gives inharmonic ones. Each air mode is
driven by the lips in proportion to its pressure where the slit is, so a slit
centred on the column does not excite the first mode, whose pressure node is
there.

The air is driven by the lips and does not push back on them. The cavity's
stiffness, as a lip feels it, is about 0.5% of the lip's own stiffness at the
default settings, so the back-reaction would move the lip's note by about 0.25%.
A study of a Nigerian slit log gong reached the same conclusion: two mechanical
modes and one Helmholtz mode with little energy passing between them, and the
beating heard from interference between them (APS March Meeting 2006, B42.2).

## The strike

The mallet is a mass meeting the lip through a Hertzian contact, `F = k c^alpha`,
as in Kit. For rubber, wood and brass, `k` comes from the two surfaces'
stiffness and the tip radius, so a brass mallet on a wooden lip is still limited
by the wood. Felt uses its own `k`, with alpha 2.5, as in piano hammer felt. The
contact runs at 8 substeps per sample. The mallet reads the lip's displacement
at the strike point, so a strike on a moving lip is a different collision from
a strike on a still one, and a mallet can bounce back and strike again.

A force impulse changes a mode's velocity. In the complex state used here, that
is the real part. Adding it to the imaginary part (the displacement) moves the
lip instantly. That was the first build, and every strike created energy: a
roll of 30 strokes reached 113 V. The harness now checks the balance. The lip
receives 16% of the mallet's kinetic energy, and the mallet leaves at 2.7 m/s
from 3.2.

The mallet also radiates as it is stopped. A small rigid sphere whose
acceleration changes radiates `p ~ V (da/dt) / c`. At its physical size this
sits about 50 dB under the strike on wood and about 20 dB under it with brass
on steel. It is low-passed at 12 kHz, because the derivative is taken at the
substep rate. At 80 times its physical size it put 33 V spikes on a steel
strike.

The other lip receives 6% of the force through the ends of the slit, so its
note sounds faintly, about 20 dB down.

## Level

Without correction, level ran 33 dB across SIZE. A 50 g mallet on a small
drum's light lip stays on it for three of its periods and damps it. A player
chooses a mallet to suit the drum, so the mallet's mass scales with the drum's
volume and its tip radius with the drum's height, both from SIZE 0.5. With
that, the strike stands in the same proportion to the lip at every size, and
the remaining level change is divided out. Measured peaks for a strike at
velocity 0.7:

| Control | Peak range |
|---|---|
| SIZE | 4.3 to 4.5 V |
| SHAPE | 3.7 to 5.1 V |
| SLIT | 2.5 to 6.8 V |
| MATERIAL | 2.6 to 5.6 V |
| MALLET | 3.2 to 5.6 V |

SIZE is then a transposition that also changes ring time, because wood's loss
rises with frequency and a lower drum rings longer. That is also what a
larger slit drum does.

V/OCT and TUNE transpose the drum without changing its size: the lips keep their
mass and become stiffer. The mallet therefore keeps its mass and its contact
stiffness rises by the same factor, pitch squared, so the collision keeps its
proportions in time. Without that, a felt mallet on a drum an octave up stayed
on the lip for several of its periods and the drum was 30 dB quieter. Measured:
a strike from -1 V to +1 V stays within 0.1 dB.

The output gain glides to a new value while the drum rings, so a knob turned
mid-note does not step the level. While the drum is asleep it takes the new
value at once. It used to stay where it was, so the first strike after a change
made in silence played at the old level.

Two faults found by sweeping every control during a roll
(`tools/garamut-harness.cpp`): the mallet's tip radius was never set, and the
size scale multiplied that uninitialised value on every update, so turning SIZE
during a roll overflowed the click term within half a second. The click's
filter then held the overflow and the output stayed NaN. The radius is now set
from the mallet table, and an output that is not finite clears the drum.

## Pitch

Each lip's note is its first mode. The right lip's thickness is solved so that
its note sits exactly INTERVAL above the left's: the mode's `omega^2 = a h^2 + c`,
with `c` the curvature term, solved for `h`. Measured error: 0.00 cents across
every shape and material.

MATERIAL would move the pitch by about two octaves at a fixed thickness, since
steel is far stiffer across a lip than wood is. Metal slit drums are made from
thinner wall, so each metal node also thins the lip (aluminium 0.28, steel 0.20).
The pitch then drifts by up to 6 semitones across MATERIAL, as the table shows.

The default drum (0.87 m tall, round, hardwood) sounds B3 on the left lip and
E4 on the right, with the Helmholtz resonance at 97 Hz.

## Measured

| | |
|---|---|
| Lowest and highest notes, SIZE | E2 to F5 |
| SLIT, small to large | C5 to F3 (the lips lengthen), and air 57 Hz to 124 Hz (the slit widens) |
| Contact time, felt to brass, on hardwood | 6.7 ms to 2.0 ms (under any hard mallet, the wooden lip deflects more than the mallet tip) |
| Contact time, felt to brass, on steel | 5.0 ms to 0.26 ms |
| Ring time of the left lip's note, cedar to steel | 0.8 s to 3.4 s (1.2 s at the default) |
| CPU | 0.7% of an M4 core, 20 strikes a second, SLIT moving; in the module, 1.9% with 16 channels rolling at 30 strokes a second, 0.15% at rest |

## Estimates

Every material constant, the lip proportions (width 0.9 of the radius, thickness
0.045 of it), the curvature factor, the slit end correction, the air modes' Q
and the mounting loss are estimates. They were chosen so the drum plays in the
range real slit drums do, not measured from one instrument.

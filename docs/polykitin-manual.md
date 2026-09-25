# PolyKit In Manual

## Overview

PolyKit In is an expander for Kit. It sits immediately to the **left** of a
Kit and gives every one of Kit's eight instruments its own set of input jacks:
fourteen columns, one for each input Kit has plus the hi-hat PEDAL, by eight
rows, one per instrument. Where a polyphonic cable into Kit says "channel N is instrument
N", PolyKit In says it with a patch cable per drum.

It exists for racks whose sources are eight modules rather than one poly
cable: a trigger from one sequencer into the kick, another into the snare, a
hand-patched envelope into one tom's tension.

PolyKit In is 35HP.

## The matrix

The columns, left to right, are Kit's inputs in Kit's order: TRIG, V/OCT, VEL,
X, Y, SIZE, TENSION, MATERIAL, AIR, DECAY, TONE, EXCITER, MUFFLE, then PEDAL.
PEDAL has no jack on Kit itself, so this is the only way to play the hi-hat
pedal from the patch: plus or minus 5 V covers the whole knob, added to it,
and it does nothing on an instrument that is a drum head. The rows are
instruments 1 to 8, which in the default kit are Kick, Snare, Closed hat, Open
hat, Low tom, High tom, Clap and Bell.

Every jack is mono, and every jack is optional. In Kit each per-instrument
read goes through one rule: **the expander's jack wins when it is patched**,
per jack; otherwise that instrument reads channel N of Kit's own poly cable.
So a poly trigger from Fill into Kit's GATE and a single hand-patched tension
on PolyKit In coexist, and patching one jack changes nothing else.

The columns follow the same conventions as Kit's own inputs: a trigger fires
on a rising edge, V/OCT is 1 V per octave, velocity is 0 to 10 V, and the
CV columns are plus or minus 5 V around the knob.

## Placement

PolyKit In talks to the Kit directly to its right and nothing else. Moved
away from a Kit it does nothing; put back, it resumes. Kit's own tooltip on
GATE says the same.

See [docs/kit-manual.md](kit-manual.md) for Kit itself.

# Count (was Sieve): a narrow clock, six sieves over one stream of steps

Design written 2026-09-20 and built hidden the same day; masks reworked after the first listen on 2026-09-21. 8HP.

## The idea

One tempo and one cycle length, shared. Six gate outputs, each with two
knobs: **DIV**, its own rate against the beat, and **MASK**, which of its
STEPS pulses pass. Each output counts its own pulses modulo STEPS, so a x2
output runs a STEPS-long cycle at twice the speed and a /4 output at a
quarter: polyrhythm and polymeter from two knobs per output, sharing only the
tempo and the cycle. Every output is a sieve; the module counts, so it was renamed Count on 2026-09-21 when the designer's panel arrived with six outputs instead of four.

## Controls

- **BPM** trimpot, 30 to 300, with a CV at 27 BPM per volt (Meter's scale).
- **STEPS** trimpot, 1 to 32, snapped, with a CV at plus or minus 16 steps
  over plus or minus 5 V.
- **CLOCK** in, one pulse per beat, overrides BPM. The rate between pulses is
  measured, and every pulse lands the phase exactly on its beat, so the
  outputs cannot drift from the source. The phase is also held back from
  reaching the next beat before the pulse arrives.
- **RESET** in.
- Per output: **DIV**, snapped over /64 /32 /24 /16 /12 /8 /6 /4 /3 /2 x1 x2
  x3 x4 x6 x8 x12 x16 x24 x32 x64 with x1 at the centre; **MASK**, snapped
  over twelve positions; and the **gate** out on a plate.

## The masks

Fifteen, in knob order, every one a shape over the cycle stated so it scales
with STEPS:

- **All**, **Odd**, **Even**.
- **Pairs** (xxoo), **Threes** (xxxo), **Fours** (xxxxoooo): repeating cells.
- **Front half**, **Back half**.
- **Euclid 1/3**, **Euclid 7/16**, **Euclid 5/8**: densities as ratios of
  STEPS, so 1/3 of 8 is the tresillo and of 16 is E(5,16), 7/16 is E(7,16) or
  E(7,15), 5/8 of 8 is the cinquillo. Bresenham's rule, step i hits when
  (i times k) mod n is under k, spreads the hits as evenly as n allows and
  always hits step 0.
- **Burst out**: runs of R, R minus 1, down to 1 hits with one rest between,
  then silence, R the largest that fits (xxxx o xxx o xx o x ooo over
  sixteen; xxx o xx o x over eight, which fills it exactly). **Burst in** is
  the same shape reversed, gathering toward the next downbeat.
- **Downbeat**, and **None** at the end, where a knob turned all the way is
  a mute.

The first list had Euclid 1/2 (the same rows as Odd), Euclid 1/4 (which DIV
already gives) and a Burst that was really a quarter-cycle cluster; all three
went in the first listen, and the repeating cells and the bursts came in
because they are the patterns a drummer actually plays.

DIV gained the odd rates **1.5, 3 and 5** each way, because a x3 against a
x2 is a polyrhythm where x4 against x2 is only a subdivision.

## What a single selector cannot give, and where it went

- **Rotate each cycle**, per output in the context menu: the mask turns by
  that many steps every cycle, cumulatively, so Threes on rotate-by-1 walks
  its rest through the bar over four cycles and comes home. This is what
  "rotate" was expected to mean on the first listen, and it is the one
  that makes a pattern evolve rather than sit.
- **Offset**, per output, a fixed rotation so two outputs on one mask need
  not fire together. A repeating mask offset by its own period is unchanged
  (Threes by 4, Odd by 2), so each menu entry shows the row it would give
  and a no-op reads as one. Both are saved with the patch.
- **Gate length**: a menu choice per module between a 5 ms trigger and a
  gate for half the pulse.

## The designer's panel (2026-09-21)

`res/count.svg` is the designer's export: 24 guides, all matched, runtime
labels removed, guides stripped. CLOCK, BPM and STEPS across the top row
with RESET and the two CVs under them, then six rows of DIV, MASK and OUT,
the outs on one tall plate. The strip grew to 15 mm for six rows of dots.

## The strip

A 9 mm screen under the title: tempo and steps as numbers on the left, and on
the right one row of STEPS dots per output, lit where its mask passes and
bright on the step it is on. It is the mask made visible, which a
twelve-position knob cannot say on its own.

## What to measure before believing it

`tools/count-harness.cpp` compiles the module against libRack and checks: every
mask over 16 steps against a hand-written row; Euclid 1/3 of 8 is the tresillo;
each DIV position fires the right pulses per beat; a 6-step cycle on x4 fires
its downbeat every 0.75 s at 120 BPM; rotate-each-cycle walks Threes
through four rows and home; a fixed offset moves a mask without changing
its count; and a 100 BPM external clock is measured to within half a BPM with
the x4 output landing on every edge.

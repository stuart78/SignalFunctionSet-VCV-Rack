# Kit hi-hat: two plates that touch

Prototype in `tools/hat-plate-harness.cpp` (standalone, no Rack). Settled on
2026-09-23 after seven listening rounds; round **Q1** is the harness default and
the target for moving it into Kit.

```bash
clang++ -std=c++11 -O2 tools/hat-plate-harness.cpp -o /tmp/hat && /tmp/hat /tmp/hatout
```

It prints, per pedal position: level at fixed times, how noise-like the
spectrum is (flatness 2-16 kHz and 0.5-5 kHz), when each field band peaks, a
single partial's glide tracked ±3%, the centroid early against late, L/R
correlation and cost. It writes a WAV per case plus a groove, a pedal sweep and
a velocity ramp. Every knob can be overridden from the environment (names in
`main()`), which is how the rounds below were run.

## Why the membrane cannot do it

Kit's Closed hat and Open hat presets put 92-96% of their energy below 500 Hz
(`tools/kit-voice-harness.py`). A real hat's is mostly 3-15 kHz. The membrane
has three things wrong for a hat, and no preset fixes any of them: a 1 ms
beater contact (nothing above ~2 kHz is struck), 40 modes (a plate has
thousands), and it is linear (a cymbal's highs are CARRIED up by
nonlinearity, not struck).

## The model

- **Two modal plates**, top and bottom (bottom heavier, +9% in pitch). Modes on
  an (m, n) grid, f ~ (m + 2n)^1.5 from 220 Hz, degenerate pairs split a
  fraction of a percent (a hammered cymbal is not a perfect disc), 3% random
  detune. Explicit only up to 5 kHz: ~150 modes a plate.
- **A stick**: raised-cosine force, 0.35 ms.
- **The plates collide**: Hertz contact at 8 points round a warped, tilted rim,
  expressed about the pressed rest state so a closed hat at rest has no net
  force. Repulsive (the first version had the sign backwards and the plates
  attracted, which is an exponential blow-up, not a sound).
- **Pressing** is modal damping in proportion to preload x shape² at the rim,
  integrated all round it: exact at any strength. As a dashpot in the contact
  it is an explicit stiff term across 300 modes and it explodes.
- **The pedal** is mostly contact: firm to light over the bottom 15%, touching
  and rattling to 85%, apart above. The user found the hat "just touching".
- **Above 5 kHz the plate is a field** (statistical energy analysis): nine
  quarter-octave bands of energy, each heard as band-passed noise. Thousands of
  overlapping modes at random phase ARE band-limited noise, and 3000 explicit
  modes a plate is not affordable. Fed by (a) the stick, through its own
  contact spectrum, (b) every collision, high-passed at 5 kHz so the pedal's
  steady pressure is not a sound, (c) the top octave of explicit modes leaking
  up. Drained by loss and by the pressing.
- **Loss grows with amplitude** (`gNLD`): in a real plate the cascade ends in
  dissipation and runs far harder when the plate is loud, so a hit falls fast
  and settles into a long tail. That is the shape of a real open hat.
- **The clutch**: no mode rings longer than 2.5 s, and every mode rings about
  equally long.
- **Radiation**: the field tilted f^0.75, the explicit modes f^3 (a thin plate
  radiates its low modes very poorly), meeting at 5 kHz.
- **The pair** shares 60% of the field's noise: a close pair hears one small
  object.

## What each round taught (all measured, then confirmed by ear)

1. **Bell, not hat.** A 0.1 ms stick into a linear bank: crisp click, clean
   partials. Flatness 0.000-0.004 (white noise reads ~0.56).
2. **Density and nonlinearity alone cannot get there.** Twice the modes, eight
   warped contacts, a cubic rim spring: flatness to 0.04 at 7.5 us/sample. A
   plate has ~0.18 modes/Hz; at 25 Hz spacing every mode is still resolved.
   This is why the field exists.
3. **The field works; feed it only what changes fast.** Fed the TOTAL contact
   force, a pressed hat pumped energy in every sample and ran away.
4. **Pitch it down and the rim spring becomes a gong.** The cubic spring tuned
   at 420 Hz stiffened the 220 Hz body four times as hard: one partial fell 178
   cents in 300 ms. Cut tenfold, 5 cents.
5. **A long low tail is a falling pitch and a big room.** Loss rising with
   frequency left the body ringing 6 s after the top died: the centroid sank
   2.7 octaves by 600 ms. Equal ring times keep it within 0.1 octave.
6. **A band of noise climbing for 80 ms is heard as a bend.** The leak entered
   5 kHz and walked up band by band (peaks at 8, 12 ... 79 ms). Landing it
   across every band at once, and giving the top band an exit, puts every band
   within 10-16 ms.
7. **Decay shape, noise, and no squelch.** A real open hat's envelope falls
   -12 dB fast and then tails; ours fell in a straight line. Amplitude-dependent
   loss: -7/-12/-17/-23/-34 dB at 25/100/200/400/800 ms. The "squelch" was the
   body's partials and their beating pairs, not a glide (every glide the ±10%
   tracker reported was it jumping to a neighbour; at ±3% there is none), so the
   explicit modes were rolled off harder and the field raised.

The ear was right every round, and every round the number that proved it was a
measurement that did not exist yet: flatness, then glide, then centroid drift,
then band peak times, then decay shape. Add the metric first.

## Q1, the settled numbers

| | closed | half open | open |
|---|---|---|---|
| dB at 25 / 100 / 400 ms | -17 / -67 / - | -9 / -16 / -28 | -7 / -12 / -23 |
| flatness 2-16 kHz | 0.29 | 0.32 | 0.35 |
| flatness 0.5-5 kHz | 0.21 | 0.18 | 0.11 |

Cost 3.1 us/sample, ~15% of a core per ringing hat, before any optimisation.

## Known costs of Q1

- Amplitude-dependent loss calms the plates, so half open collides ~65 times
  where round 6 managed ~2400. The user approved Q1 as heard.
- One partial of the open hit still reads +35 cents at ±3%; not heard as a bend.

## In Kit (2026-09-23)

`src/kit-hat.hpp` is this model as an engine beside `sfs::Drum`, chosen per
instrument (`Kit::Inst::engine`). `tools/kit-hat-check.cpp` runs it against the
harness case by case: every decay checkpoint within 3 dB, flatness within 0.05
(they agree to about 2 dB and 0.01). 0.37-0.48 us/sample per ringing hat, from
flat arrays the compiler vectorises; since 2026-09-24 the rim step is one
gather/decide/scatter over blocked contact shapes with a Gram matrix keeping the
sequential semantics, 0.19 us open and 0.31 us closed. PEDAL took HIT's place on the panel.
Output gain: a closed hit peaks ~4.8 V, an open one ~2 V, because a closed hit
is nearly all transient.

## Originally listed as still to do

- **Cost**: SIMD over modes (structure of arrays), skip the contact projections
  when every point is clearly apart, run the field at control rate with
  interpolated gains. Target under 1 us/sample.
- **Where it lives**: a second engine beside `sfs::Drum` in `src/membrane.hpp`
  (or its own header), chosen per instrument; the membrane presets stay
  bit-identical.
- **The panel**: Kit has no free knob. PEDAL is the obvious new control and
  wants CV (a hat pedal is performed); candidates are reusing a knob whose
  meaning does not apply to a hat (AIR, COUPLE, RESO) while the hat engine is
  selected, or PolyKit In.
- **Foot chick**: the pedal closing on its own (no stick) should clash the
  plates; the contact model already does it if the pedal moves fast enough.

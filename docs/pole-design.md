# Pole design

A stereo filter of four pole pairs (20HP, slug `Pole`, built hidden
2026-11-14). Prototype: `tools/filter-proto.cpp` (free of Rack, renders the
listening WAVs). Module: `src/pole.cpp`. Checks: `tools/pole-harness.cpp`.

## Why it should exist

The plugin is almost all sources and sequencers (roadmap: "a gap in the set").
A new filter belongs in the set only if it does something the Library's
existing filters do not. Two of the obvious candidates fail that test:

- **Controlling one pole pair is the same as controlling cutoff and
  resonance.** A pole pair's angle is its frequency and its radius sets its Q.
  Dragging one pair round a z-plane is a resonant sweep.
- **Every state-variable filter already produces several modes.** Lowpass,
  bandpass, highpass and notch share their poles and differ only in their
  zeros, so a continuous MODE is a mix of the filter's own outputs.

Few filters have **four pole pairs that act as one filter or as four**. The
same poles form a steep resonant filter when gathered and a set of formant
peaks when spread, applied one after another (series) or summed as bands
(parallel). STEREO sets how the right side's poles relate to the left's.

Distinct from **Band**, which locks four bandpasses to harmonics of a detected
fundamental and works as an analyser as well as a filter. Pole filters any
input, and its controls move four pairs between one steep filter and four
separate peaks. If SPREAD were mostly used at harmonic ratios, Pole would
duplicate Band.

## The model

Four pole pairs. Each is a TPT state-variable filter (as Phase's `DJFilter`),
because direct-form biquads go unstable under fast coefficient changes and LAG
produces exactly those. Per pair: frequency, width (Q), mode (the zeros), level
(parallel only).

Pair *i* sits at `f_i = CUTOFF · 2^(SPREAD · pos_i) + SHIFT`:

- **CUTOFF** (20 Hz to 17 kHz, 1V/oct; audio-rate FM is fine, it is read every
  sample) moves every pair by the same ratio (1V/oct). The layout's
  intervals are kept.
- **SHIFT** (±2 kHz, cubic so the middle is fine; 200 Hz/V) moves every pair
  by the same number of Hz. Ratios are NOT kept, so a harmonic layout becomes
  inharmonic. This is the difference between a frequency shifter and a pitch
  shifter, applied to filter frequencies. The two sound different (`07a` vs
  `07b`).
- **SPREAD** scales the distances in octaves between the pairs, 0 = gathered.
  `pos_i` defaults to −1.5, −0.5, 0.5, 1.5 and can be edited per pair on the
  screen.
- **WIDTH** is every pair's bandwidth, 0.02 to 3 octaves (Q 72 to 0.4); right
  is wider. The tooltip shows both.
- **MODE** slides the zeros LP → BP → HP → notch, continuously.
- **TOPOLOGY** crossfades series ↔ parallel. Both chains run while it is
  between the ends; at an end the other chain is cleared and skipped. The
  parallel end is four bands side by side or two chains of two (1-2 beside
  3-4), from the menu.
- **ORDER** of the series chain, a menu item (lowest first, highest first,
  outside in, inside out). Only audible with DRIVE:
  linear stages commute, measured at 104 dB below the signal with no drive.
  With saturation between stages, a resonant pair saturating before or after
  the others is audible (`08`).
- **LAG** applies a separate slew to each pair's CUTOFF, staggered. While
  CUTOFF holds still the pairs sit together; a fast move fans them out into
  separate peaks, which gather again when the move stops. How fast CUTOFF
  moves therefore changes the timbre as well as the frequency (the idea of
  Flock's LAG).
- **DRIVE** saturates before each series stage.

## Stereo

A STEREO selector (six positions, 1 V per relation) and an AMOUNT. Every CV
input also reads a 2-channel cable as L on channel 0 and R on channel 1, in any
relation, so R can be modulated on its own without Independent.

- **Linked**: R is L.
- **Offset in octaves**: R's set moved ±2 octaves (ratios kept, `06a`).
- **Offset in Hz**: R's set moved ±1 kHz (ratios changed, `06b`).
- **Mirror**: R moves opposite to L in log frequency about a fixed axis, 1 kHz,
  which AMOUNT moves ±2 octaves. Knob and CV both mirror. (The first build
  mirrored about the CUTOFF knob, so only CV split them and turning CUTOFF by
  hand moved both sides together: the mirror could not be found from the
  panel.) The layout and SHIFT mirror too. A lowpass mirrored in log
  frequency is a highpass, so by default the mode mirrors as well (`06c`;
  menu). The notch end of MODE is left alone, since a mix of highpass and
  notch has no mirror on the same axis.
- **Spread**: R's layout is L's times (1 − 2·AMOUNT): linked at 0, gathered at
  0.5, reversed at 1, three times as wide at −1. This replaced "Reverse",
  which was the same as Linked on the symmetric default layout.
- **Independent**: R has its own eight knob values and its own pair edits.
  The panel's knobs are a view onto one side, switched by L/R tabs on the
  screen (the same scheme as Kit's instruments). The first time Independent
  is chosen R starts as a copy of L; the menu copies either way.

## Findings

1. **Coincident resonances multiply in series.** Four pairs of Q 8 at one
   frequency peak at 8⁴ (the first renders were 47 dB hot). Fix: a pair shares
   its Q with the pairs it overlaps, weighted by distance in units of its own
   bandwidth, so the gathered set makes one peak of the Q asked for and pairs
   pulled apart get their whole Q back. Gathered sweep now swings 7.3 dB.
2. **Series bandpasses lose level as they spread**: 45.6 dB of level
   swing across one SPREAD sweep (`02b`). Fix: compute the analytic magnitude
   response at control rate (96 points, every 32 samples) and divide by its
   peak. Swing drops to 5.4 dB (`02d`). The screen draws the same curve, so
   the correction needs no extra computation.
3. **Order makes no audible difference without DRIVE** (104 dB below the signal), so ORDER
   is a menu item rather than a panel control. Strictly that holds while the
   coefficients stand still: a time-varying chain does not commute. The
   module's first build showed it, at −35 dB, because for its first 16
   samples every pair sat at the 8 Hz floor, gathered, and its Q was shared
   out accordingly. The pairs are now placed before the first share-out.
4. **The drawn curve is exact.** The TPT SVF is the bilinear transform of the
   analog prototype with the cutoff prewarped, so evaluating the analog
   response at `tan(πw/sr)/tan(πfc/sr)` equals the digital response. Measured with
   sines against the curve: within 0.0002 dB in every topology.
5. **How much the level is corrected.** Divided all the way to the peak, a
   resonant lowpass would lose its passband as Q rises. The peak is instead
   allowed to rise by half as many dB as the highest pair's Q (target
   `√(Q/0.707)`), so a gathered Butterworth set passes at unity and a
   resonant peak stays above the passband. The menu can switch the correction
   off ("Uncorrected (no gain after the filter)"). Output is soft-clipped,
   linear to ±6 V.

6. **Clicks on big changes** (2026-11-15, heard in Rack). Every setting
   reached the filter in one step: a knob turned fast, a CV that steps, a
   switch of MODE, TOPOLOGY or the stereo relation. Measured as the output
   above 8 kHz in the 60 ms after a one-step change, against three sines
   below 1.3 kHz: ten of fifteen changes clicked, 10 to 45 dB over their
   settled state. Three fixes:
   - **Settings glide** through a 6 ms one-pole (`PO_GLIDE`): CUTOFF's
     knob part with the relation's offsets, the layout, SHIFT, each pair's
     Q, the mode weights, TOPOLOGY and DRIVE. CUTOFF's CV does not, since it
     can be audio-rate FM.
   - **Discrete changes are made under a fade**: series order, the parallel end and the
     stereo relation change with the output faded out over 5 ms (a
     smoothstep, so there is no corner at either end) and back in. A new
     relation can put R octaves away in another mode, and gliding there swept
     its resonances across the input (+17 dB for a moment), so R restarts at
     its new settings with empty filters, at the bottom of the fade.
   - **The level correction follows in two poles.** One pole that fell fast
     left the corners of its 16-sample target steps in the output (3 kHz and
     harmonics). The correction is computed from the filter as it is now,
     mid-glide, falls with a 0.5 ms time constant per stage and rises with a
     10 ms one. Each stage chooses its rate from its own direction of change:
     with one shared choice, the second stage jumped when the first stage
     changed direction.

   All fifteen now sit within 10 dB of their settled state, or below −60 dB
   of the signal. A fast CUTOFF move with narrow pairs still sweeps the
   resonance across the input, as a fast sweep of any resonant filter does.
   The click test uses wide pairs for frequency moves, so this sweep is
   excluded from it.

7. **A zero-attack envelope on CUTOFF clicks** (2026-11-15, heard in Rack:
   an ADSR with no attack opening a closed series bandpass, with the pairs
   below 20 Hz.) The output does not step. The filter's states hold the
   signal it was passing, and when the pairs move several octaves in one
   sample that stored signal rings at the new frequency for about 10
   samples. With a 110 Hz input and the band opened to 4 kHz, the ring
   above 8 kHz reached 43 dB below the input. An analog state-variable
   filter stores its signal in its capacitors and rings the same way, and a
   VCA opened with no attack clicks for a related reason. Limiting how fast
   the CV can move made it worse at 3 to 8 octaves per ms, because a 1 to
   2 ms sweep through the spectrum is itself a click. A one-pole on the
   CUTOFF CV works: 1 ms lowers the ring by 15 dB, 3 ms by 41 dB. The
   default is 3 ms, and the menu offers Off, 1 ms and 10 ms. The 3 ms
   one-pole is 3 dB down at 53 Hz and 12.6 dB down at 220 Hz, so
   audio-rate FM needs Off.

## Starting points

The listening renders were each a *movement* (SPREAD swept 0 to 1.5, TOPOLOGY
crossfaded, CUTOFF swept under Mirror), and found by hand from a panel of ten
controls they were hard to reach. "Starting points" in the menu sets the panel
to each render's settings and shows the name of the control to move on the
screen, in orange, for 15 s: gathered lowpass, series lowpass and bandpass pulled apart,
parallel bands, series into parallel, LAG on an envelope, the three stereo
relations, the harmonic ladder (SHIFT on noise) and order under DRIVE. Values
are written in the units they mean (Hz, Q, octaves, seconds, ×) and converted
to knob positions, and the harness checks that each one sets the Q and
frequency it names.

## Measured (`tools/pole-harness.cpp`)

- Gathered series lowpass at its widest: 100 Hz at −1.4 dB, 148 dB down three
  octaves above cutoff.
- Four pairs of Q 8 gathered: the resonance rises 18.5 dB over the passband
  (Q 8 is 18 dB; 8⁴ would be 72).
- SPREAD swept 0 to 1.5 octaves on a saw chord, series: lowpass swings 6.3 dB
  (8.8 uncorrected), bandpass 3.7 dB (44.2 uncorrected).
- LAG at 0.6: 20 ms after a jump from 200 Hz to 3.2 kHz the pairs sit at 3200,
  835, 465, 363 Hz, and gather once the move stops.
- 10 s of random CV on every control plus audio-rate FM on CUTOFF: finite,
  peak 9.9 V.
- Cost: 1.0% of an M4 core static with both chains running, 1.2% with CUTOFF
  modulated (the glide sets every pair's coefficients every sample).
- Fifteen one-step changes (above): none clicks. Every starting point loads at
  the Q and frequency it names and plays a saw chord between −8 and +6 dB.

## Listening (2026-11-14: "these sound amazing")

Renders are in `renders/filter-proto/` (not committed):

- Does SPREAD in series (`02c`, `02d`) and in parallel (`03`) sound like vowels
  or a resonant body, or like a phaser? The answer decided whether to build the
  module.
- Does LAG (`05a` without, `05b` with) make an audible difference on an
  envelope?
- Which stereo relations (`06a` to `06c`) should have a switch position?
- Is SHIFT in Hz (`07b`) musical enough to be a panel control, or a menu item?

## Screen

The magnitude response over 20 Hz to 20 kHz, −42 to +18 dB, after the level
correction: L filled blue (what leaves the jack), R as an orange line when it
differs. Each pair is a mark on its side's curve with a bar across its
bandwidth, drawn where the pair actually is, so LAG is visible as the marks
fanning out during a move. Drag a mark sideways to move that pair (its own
offset, kept apart from SPREAD's layout), up to narrow it, down to widen it;
Ctrl for fine, double-click to reset it. In Independent the marks are drawn on
the side being edited. A pair below 20 Hz or above 20 kHz is drawn as a
triangle at the edge of the plot, pointing past it. The header shows the
topology and mode, and the stereo relation with AMOUNT's value in it.

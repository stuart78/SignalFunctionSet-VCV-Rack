# Canon: four arpeggiators reading one progression

Design written 2026-09-19; first pass built the same day, hidden, and every item under "What to measure" passes in `tools/canon-harness.cpp`. A relative of Fugue: where Fugue is
three voices reading one melody and wandering from it, Canon is four
arpeggiator channels reading one chord and wandering from that. A canon is a
canon in which every voice sings the same material, entering at its own
moment; four channels arpeggiating one chord on their own clocks is exactly
that.

## The idea in one paragraph

Four **sets**, each a chord of up to six notes, shared by everything. A SET
gate advances through them, or a SET CV picks one directly. Four **channels**
each have a GATE input that steps them through the current set in their own
PATTERN, at their own OCTAVE, straying from the chord by their own WANDER,
and each puts out a V/OCT and a GATE pair. Change the set and every channel
follows, from wherever it was.

## Sets

- Four sets, each a chord of one to six notes. Stored as **scale degrees plus
  an accidental**, against the plugin's ROOT and SCALE (`src/scales.hpp`, the
  canonical list, read in the same convention as Fugue, Note and Key), so a
  key change from Key or Arrange moves all four sets together. A note chosen
  outside the scale is kept as a semitone offset from the root, the way Key
  keeps a free sub-scale pick: it transposes with ROOT and does not follow
  SCALE.
- **Entered on screen.** Each set is a small keyboard, C at the left whatever
  the root is (you read a keyboard by its shape, as Key does), click a key to
  add or remove it. A menu of stock voicings (triads, sevenths, sus, add9,
  quartal) fills a set to start from.
- **LEARN.** A poly V/OCT input and a LEARN button: hold a chord and press,
  and the held notes are quantised to degrees and written into the selected
  set. One jack and one button; the fastest way to enter four chords.
- **SET** advances 1 to 2 to 3 to 4 and wraps, on a gate. A **SET CV** at one
  volt per set selects directly and wins when patched, so Arrange's phrase
  index or a sequencer chooses the chord. RESET returns to set 1.
- An empty set is skipped, so a two-chord progression is sets 1 and 2 with 3
  and 4 empty.

## Channels

Four identical columns. Per channel:

- **GATE** in. Each rising edge steps the channel to its next note. The
  inputs **normal left to right** (as Key's IN and TRIG do): one clock into
  channel 1 arpeggiates all four, and a cable into channel 3 breaks the
  chain there, so a division into channel 3 gives 3 and 4 a different rate.
- **PATTERN** (snap knob, one volt per step on its CV): up, down, up-down,
  converge, diverge, pedal (root alternating with each other note), random,
  as-played (the order the notes were entered or learned). Each pattern is
  listed twice, for one octave and for two, because a stock arpeggiator's
  RANGE is worth having and not worth a fifth control per channel.
- **WANDER** (0 to 100%, CV): Fugue's tiered harmonic deviation, reused
  rather than reinvented. At zero the channel plays the set's notes exactly.
  Turned up, a step substitutes with rising probability: first another chord
  tone, then an extension (7th, 9th, 11th in the key), then colour. The
  tiers are Fugue's `DIATONIC_TIERS` / `PENTATONIC_TIERS` / `CHROM_TIERS`
  selected by the scale's size, which is why this should share a header
  with Fugue rather than copy the tables.
- **OCTAVE** (snap, minus two to plus two, CV): an offset applied to the
  whole channel.
- **V/OCT** and **GATE** out. Not a poly pair: poly is annoying to patch.
  The gate out follows the gate in (a held input is a held output), so a
  clock's duty cycle is the note length, and the menu offers a fixed
  trigger instead.

### Harmonic lock

Fugue's Harmonic Lock, across four channels. When a channel wanders, it draws
three candidates and takes the one most consonant with what the other three
are currently sounding. This is the thing that makes four wandering channels
a voicing rather than four soloists, and it is on by default, with the menu
switch to turn it off for the atonal case.

### A channel keeps its place

When the set changes, a channel on the third note of set 1 plays the third
note of set 2 next: the patterns flow through the chord change instead of
restarting on it. Sets of different sizes wrap the index. A menu option,
**Restart channels on set change**, gives the stricter reading.

## Panel, first draft

About 26HP. The screen across the top: four small keyboards, one per set,
with the current set framed and each channel's current note marked on it in
that channel's colour, so the chord and who is playing what are one
picture. Below the screen the global row: ROOT, SCALE, SET (jack and
button), SET CV, RESET, LEARN with its poly jack. Then four channel columns,
each PATTERN, WANDER and OCTAVE as trimpots over their CVs, GATE in above
the column's V/OCT and GATE outs on a plate at the foot.

## After the first listen

- **The dots that flashed were the channel markers**, drawn only while a
  gate was high, so under a clock they blinked. Each channel now keeps a
  marker on the note it last played until its next step, in the plugin's
  four channel colours (blue, green, orange, purple, as Trace's lanes and
  Spool's tapes), each channel on its own row down the key, with a legend
  in the header.
- **Twenty chord progressions** in the context menu fill all four sets at
  once. Fourteen are built on the scale in force as degrees (a triad is d,
  d+2, d+4, the sevenths add d+6), so they stay right in any mode. Three
  borrow chords (bVII, bVI, iv) and are stated in semitones, landing as
  degrees where the scale has one and as accidentals where it does not.
  Three are minor progressions and set SCALE to Natural minor when applied,
  because "i VII VI V" read against a major scale is a different
  progression.

- **Right-click a keyboard for that set's menu**: play it now, fill it
  with a voicing, copy the playing set into it, or clear it. The set under
  the mouse is the one you mean, not the one the playhead is on; the module
  menu keeps the same voicings for the current set.

- **Step offset, per channel** (context menu, 0 to +11, saved): how many
  steps ahead of its own count a channel reads. It moves where the channel
  reads, not where it counts, so the channels stay in step with each other.
  Four channels on one clock with the same pattern and offsets 0 1 2 3 sound
  the chord's notes together and rotate through it as a block, which turns
  the arpeggiator into a chord player; 0 0 1 1 walks in dyads.

## The designer's panel (2026-09-21)

`res/canon.svg` is now the designer's Figma export, adopted with the
figma-panel skill: 48 guides, every one matched within 0.04 mm, runtime
labels removed, guides stripped. Three things the art decided:

- **A SET knob** beside the SET CV jack. The set in force is now an advance
  count (NEXT) plus the knob plus the CV, modulo four, so the three compose:
  the knob picks a set when nothing advances, NEXT walks on from wherever
  the knob points, and a CV sequences it on top of both. `SETSEL_PARAM` was
  appended to the enum.
- **The foot plate reads GATE then V/OCT**, the reverse of the first
  layout; the outputs kept their enum slots and only moved.
- The channel letters A to D sit in the art in the plugin's channel colours,
  matching the screen's markers.

Renamed Canon on 2026-09-21, and the art retitled with it.

## What to measure before believing it

- **Lock does what it says**: over a thousand steps at WANDER 50% on a major
  triad, the interval between simultaneous channels should be consonant
  more often with the lock on than off, by a margin worth having; if the
  margin is small the candidate count or the consonance ranking is wrong.
- **Patterns are right by construction**: each of the sixteen patterns over
  a four-note set produces exactly the sequence its name says, checked
  against a hand-written table.
- **Set changes never skip or double a step**: a channel stepping through a
  set change lands on the index it should, for every pair of set sizes.
- **LEARN quantises to the degree the note is nearest**, including notes off
  the scale landing as accidentals.

## Open questions

- Whether WANDER should also be able to substitute a **rest** at the top of
  its range, as Chance can. Probably yes, as the last tier.
- Whether the four channels want a shared SLEW as Fugue has. Left off the
  first panel; the outputs are CV and a slew module exists.

# Diapason: a pipe organ

A three-division pipe organ (40HP, screenless, code-drawn panel with
`sfs::PanelLabels` until the Figma pass). Great, Swell and Pedal each have their
own polyphonic V/OCT + GATE, their own stops, and are joined by three couplers.
The room is left to Cistern: the organ has outputs placed across its case, and
Cistern's two sources put them in a church.

"Diapason" is the English name for the organ's principal tone, the open metal
pipe the whole chorus is built on. It is a working name.

## What it has to get right

In order of how much each one decides whether it sounds like an organ:

1. **Pipe speech.** A pipe does not start cleanly, and every family starts in
   its own way: a principal speaks its octave before its fundamental, a flute
   has a noisy chiff, a string is slow and builds its upper harmonics last, a
   reed speaks fast with its upper harmonics first. Large pipes take longer to
   speak than small ones, in proportion to their period. Once speaking, the
   tone is steady for as long as the key is held: no decay.
2. **Ranks, not drawbars.** Each stop is a rank of pipes, one per note, each
   with its own spectrum, its own slight mistuning and its own attack. A
   registration is the sum of several separately sounding sources, which is
   what an additive drawbar organ cannot imitate.
3. **One wind supply per division.** A big chord draws enough air to lower the
   pressure, so every pipe of that division sags in level and (slightly) in
   pitch together, and the reservoir bounces back. The tremulant shakes the
   Swell's wind, so every Swell pipe wavers in step.
4. **Pipes stand in places.** Pipes alternate between the C side and the C♯
   side of the case, so a scale moves back and forth and a chord comes from
   several places.
5. **The swell box.** The Swell division is enclosed. Its shutters, opened by
   the swell pedal, raise its level and its brightness together.
6. **The room.** Not modelled here: see "Reverb" below.

## Divisions, stops and couplers

| Great (open) | Swell (enclosed) | Pedal (open) |
|---|---|---|
| Bourdon 16′ (stopped) | Gedackt 8′ (stopped) | Contra Bourdon 32′ (stopped) |
| Principal 8′ | Viola da Gamba 8′ (string) | Subbass 16′ (stopped) |
| Rohrflöte 8′ (chimney flute) | Voix céleste 8′ (string, tuned sharp, from tenor C) | Octave 8′ (principal) |
| Octave 4′ | Principal 4′ | Posaune 16′ (reed) |
| Fifteenth 2′ | Oboe 8′ (reed) | |
| Mixture IV | Trompette 8′ (reed) | |
| Trumpet 8′ (reed) | Tremulant | |

Couplers: **Swell to Great**, **Swell to Pedal**, **Great to Pedal**. A coupler
opens the pallets of another division for the keys held on this keyboard. It
plays the same pipes, not a second set: if the Swell keyboard and the Great
keyboard (through Swell to Great) both hold middle C, the Swell's middle C
pipes sound once. The engine therefore counts **pipes** (rank × key), not
voices, and a pipe speaks while its key is held through any route and its stop
is drawn. Drawing a stop under a held key makes that pipe speak; putting it in
silences it, as on an organ with slider chests.

Compass: manuals C2-C7 (61 notes), pedal C2-G4 (32 notes), in 8′ terms with
0 V = C4. A note outside the compass is folded into it by octaves. Pitch is the
nearest key: an organ cannot bend, so a V/OCT that moves while its gate is held
releases the old key and presses the new one.

The Voix céleste starts at tenor C (C3), as most do, and is tuned 5 cents
sharp, so it beats against the Gamba at about 1.3 Hz at A4.

The Mixture IV has four ranks (19.22.26.29 at the bottom: 1⅓′ 1′ ⅔′ ½′) and
breaks back at every C from C3, ending as 8′ 5⅓′ 4′ 2⅔′ at the top, so it never
reaches the top of hearing.

**Split** (menu): GREAT notes below a chosen key play the Pedal keyboard, so a
single keyboard can play bass and manual.

## Pipe model

Each rank family has a harmonic spectrum (estimates from the general shape of
published pipe spectra, not measurements of one organ):

- **Principal**: every harmonic, falling about 9 dB per octave, the 2nd strong.
- **Stopped flute** (Bourdon, Gedackt, Subbass): odd harmonics; the even ones
  30 dB or more down.
- **Chimney flute** (Rohrflöte): stopped, with the chimney lifting the 5th.
- **String** (Gamba, Céleste): harmonics 2-6 near the fundamental's level and a
  long, slowly falling tail.
- **Reeds**: a formant (Trumpet about 1.6 kHz, Oboe a narrower one near
  1.4 kHz over a weak fundamental, Posaune near 700 Hz) on a rich series.

Treble pipes are voiced with fewer upper harmonics (a rolloff above 6 kHz).

A pipe is three **harmonic groups** read from band-limited tables (the
fundamental; harmonics 2-3; harmonics 4 and up), each with its own onset delay,
rise and overshoot, plus a resonator-filtered noise burst for the chiff and a
little sustained breath on the flutes. The speech time scales with the period:
`T = 12 ms + 10 periods`, clamped to 15-250 ms; reeds speak in half of that.
Release takes `8 ms + 12 periods` (see "Release (third pass)"), the upper groups faster. When all three
groups have settled, the pipe reads one summed table instead of three, which
is what makes a held full-organ chord affordable.

| Family | Fundamental | 2-3 | 4+ | Noise |
|---|---|---|---|---|
| Principal | delay 0.25T | first, overshoot 1.3 | 0.1T | small, at the 2nd |
| Flute | rise T | 0.7T | overshoot 2 (chiff) | chiff at the 3rd, sustained breath |
| String | delay 0.3T, rise 1.5T | 1.2T | 1.2T | small, at the 4th |
| Reed | rise 0.5T | 0.4T | first, overshoot 1.4 | none |

Every pipe has a fixed mistuning of up to ±1 cent (hashed from rank and key)
and a fixed starting phase per harmonic, so two ranks at unison beat slowly and
a chord does not peak where every pipe starts together.

## Pipes that are not quite steady

The first build sounded like a synth emulation because a settled pipe read one
fixed table: perfectly periodic, and identical to every other pipe of its rank
in the same half octave. The **Pipes** menu (saved; Natural by default) now
adds, scaled 0 / 1 / 2 for Steady / Natural / Old organ:

- a fixed **voicing** per pipe: each group's level offset by up to 1 dB
  (fundamental) and 2 dB (upper groups), hashed from rank and key;
- a slow **wavering** of each group's level (1.5%, 3.5% and 6%) and of the
  pitch (0.6 cents), each from its own lowpassed noise at about 3 Hz, so the
  pipes of a chord do not move together;
- **turbulence** in each division's wind (0.4% at about 2 Hz), shared by its
  pipes;
- mistuning up to ±2 cents (±1 at Steady);
- a flue pipe **flattens as its wind dies** (up to 14 cents by the end of its
  release) and gives a puff of noise as the pallet closes;
- more breath noise on the principals and strings.

Measured on a held Principal 8' C4, in windows of exactly 25 periods: 0.12 dB
of level movement at Natural, 0.01 dB at Steady. Every pipe now reads three
tables all the time (the summed table is used only at Steady), which takes a
full organ from 1.7% to 2.3% of an M4 core.

### Speech (second pass)

Old organ still sounded fake in its attacks: every pipe of a family spoke with
the same smooth curve, scaled only by its period, and every pipe of a chord
started on the same sample. Now, scaled by the Pipes setting (none at Steady):

- **The pipes of a chord start apart**: the Swell 3 ms after the Great and the
  Pedal 5 ms (their places in the case), each rank's chest up to 3 ms, and each
  pallet up to 4 ms. A coupled chord's pipes start over about 10 ms at Natural.
  Releases are spread the same way, by less.
- **Speech time varies** from pipe to pipe (±15%) and is 15% longer at Natural.
- **While a pipe speaks** it is flat by up to 20 cents, its 2nd-3rd harmonics
  10 cents and its upper harmonics 25 cents flatter still (an open pipe's
  higher resonances lie below the harmonic series until the jet locks them),
  and the upper groups are roughened by 60 Hz noise. All three fade over half
  the speech time. Each group therefore has its own phase while it speaks.
  Reeds lock faster (0.4 of the amount).

**Stops of one family are no longer the same spectrum.** In the Full organ the
Trumpet and the coupled Trompette, or the Great Principal and the Pedal Octave,
sounded at unison with near-identical waveforms two cents apart, which is a
synth's chorus. Each stop now has its own tilt (dB per octave of harmonic
number) and, for a reed, its own formant: the Trompette brighter, its formant
35% higher; the Gedackt, Pedal Octave, Subbass and 32' darker; the Octave 4'
and Fifteenth brighter.

**Release (third pass).** Releases were far too fast: `6 ms + 3 periods`, so
a full chord fell 10 dB in 20 ms. A pipe whose wind is cut rings down as a
resonator, tau = Q / (pi f); with Q about 40 that is `8 ms + 12 periods`
(56 ms at C4, 190 ms at C2, 0.76 s on the 32'). A reed stops when its tongue
does, so reeds keep a short release, `6 ms + 3 periods`: with the flue rule
the Posaune 16' buzzed on for most of a second after a pedal note. A Principal 8' is now 20 dB
down 114 ms after the key at C4 and 417 ms at C2. The pitch bend as a pipe
speaks was also cut from 20 cents to 2-8, a different amount for each pipe:
the same bend on a hundred pipes at once is heard as one scoop, which is a
synth gesture.

The first renders were also written without the module's output stage, so the
full-organ file peaked past full scale and clipped on playback. Renders now go
through the module's gain and soft clip, and the Full organ preset is at -6 dB.

## Presets

Ten, in `presets/Diapason/`, written from the real module by
`tools/diapason-presets.cpp` so they cannot fall out of step with the
parameters. A preset carries the stops, couplers, tremulant, swell, wind,
level, the Pipes setting, the output mode and the factory pistons. A patch or
preset just loaded never has a piston drawn over it: the first sample adopts
whatever piston resolves. `build.sh dev` renames the plugin inside its copy of
the presets, since Rack refuses a preset whose plugin is not the one loading
it.

## Wind

Each division has a reservoir. Its demand is the sum over speaking pipes of an
airflow weight (larger pipes and reeds draw more) times the pipe's speech
envelope. The pressure follows `1 / (1 + k·WIND·demand)` through a damped
second-order response (about 2.5 Hz, ζ 0.35), so a big chord sags and the
reservoir bounces. Pressure scales level directly and flue pitch as `p^0.04`
(10% low is 7 cents flat); reeds hold pitch (`p^0.01`). WIND sets k, from a
perfectly regulated supply to a generous sag.

The tremulant modulates the Swell's pressure: RATE 4-8 Hz, DEPTH up to 8%.

## Swell box

A level from -20 dB (closed) to 0 dB (open) and a 12 dB/octave lowpass from
700 Hz to 20 kHz, both following the swell pedal (knob + 0-10 V CV).

## Output and the room

**The room is part of the module (2026-11-26, reversing the first decision).**
The first build left the room to Cistern, on the reasoning that a reverb in
the organ would fix one room and duplicate the plugin's real ones. Listening
said otherwise: there is no such thing as a large organ heard without its
building, and every dry render read as an emulation however the pipes were
voiced. So Diapason runs **Cistern's engine** (`src/cistern-engine.hpp`, shared,
not copied) on Cistern's own scans:

- **ROOM** (trimpot): Off, Old Church (the default), St Bartholomew-the-Less,
  St Peter Stourton, Hintze Hall, the Pantheon, the Basilica Cistern. Picked by
  Cistern's slugs, so the list cannot drift from Cistern's.
- **MIX** (trimpot): Cistern's direct-sound mix. Below the centre the
  reverberation fades out under the organ's direct sound as the room delivers
  it, above the centre the direct sound fades under the reverberation; the
  centre is the room as traced.
- **Where the organ stands**: at the space's own source position, raised up to
  3 m toward a gallery, its C side and C# side up to 6 m apart across the
  listener's view (narrower or lower where a wall is in the way). The listener
  is the space's. In a room the two sides of the case feed the room's two
  sources, so a scale walks across the case in the building.
- **The worker** traces a room only when ROOM or the sample rate changes, on
  its own thread, and hands over a whole Reverb through atomics; nothing is
  allocated or freed in `process()`. A room change leaves the last room
  ringing out, as Cistern does. Until the first room is ready (a fraction of a
  second) the organ plays dry.
- **Cost**: a room adds about 2% of an M4 core, since the tail was optimised
  for Cistern and Diapason both (2026-11-27; it was 3-4.5%).
- **How soon it comes in**: a trace took 2-4 s, and the organ played dry until
  it was done. Most of that was the room's decay measurement, which does not
  depend on where the organ stands, so it now ships as a table
  (`DP_ROOM_DECAY`, written by `tools/diapason-rooms.cpp` and checked against
  the engine by the harness). The first trace is 3000 rays, about 0.1 s; the
  full 60000-ray trace follows about 1.5 s later and is cross-faded in through
  Cistern's own program handover, as Cistern does when it traces a room again.

With ROOM off, the output modes still apply: **Left and right of the case**,
or **C side and C# side** for a separate Cistern's two sources.

## Registration

Eight general pistons. PISTON (knob + 1 V per piston CV) and the NEXT trigger
compose, as Canon's sets do: `(NEXT count + knob + CV) mod 8`. When the result
changes, its stored registration is drawn (stops, couplers, tremulant). SET
stores what is drawn into the current piston. A new module comes with eight
registrations, soft to full; piston 1 is drawn at the start.

## Faceplate drawing

The case front, drawn on the faceplate with no screen: two Pedal towers at the
ends, the Swell's two enclosed boxes with shutters that open with the pedal,
and the Great flats in the middle, each side's pipes rising toward the centre.
A pipe fills orange while it speaks. Click a pipe to play that key on its
division.

## Cost

A pipe costs one table read when settled and three during speech and release,
plus a resonator while its noise is audible. Pipes that have died away are
freed. Target: a held eight-note full organ with every coupler drawn under 10%
of an M4 core.

## Measured (2026-11-26)

`tools/diapason-proto.cpp` (engine, no Rack) and `tools/diapason-harness.cpp`
(the module against libRack) both pass. Every stop sounds at its pitch, the
Contra Bourdon's bottom C is 16.36 Hz, a pipe held through two routes sounds
once, the Céleste is silent below tenor C and beats against the Gamba, a
Principal 8' takes 87 ms to speak at C2 and 14 ms at C6, and a full Great
chord at WIND 1 sags to 93% while a regulated one holds. A full organ (every
stop and coupler, four notes on each manual and a pedal note) is 101 pipes,
2.2 V rms with 7.5 V peaks, at 1.7% of an M4 core; one Principal 8' note is
0.35 V rms.

**Cistern's low end.** Its impulse responses, measured in octave bands by the
level over time (not a T60 fitted from a band-filtered Schroeder curve: a
brick-wall FFT band spreads the filter's own ringing over the whole file and
reads as a floor 50 dB down that never decays), show no build-up at the
bottom: in the Old Church the 31.5 Hz band is 19 dB down after one second
against 11 dB at 500 Hz, and St Peter Stourton's 63 Hz band falls with its
mids. A real church usually holds its low end at least as long as its mids, so
a 32' in Cistern is a little drier than in the building, but it does not boom.
The Pantheon and the Basilica Cistern are the large spaces; a cathedral nave
built from published dimensions would be a Cistern addition, not a need.

## Open questions

- The spectra and speech times are estimates and should be listened to rank by
  rank (renders in `renders/diapason/`).

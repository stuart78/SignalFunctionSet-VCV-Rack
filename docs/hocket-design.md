# Hocket

One line dealt out across eight channels (22HP). A hocket splits a melody
between voices, each playing some of its notes; Hocket decides which channel
takes each note, and the pitch, the gate and an audio input all follow that
decision.

## Why one module and not two

Note hocketing (CV and gate to eight voices) and audio hocketing (one signal to
eight destinations) share everything that decides *who is next*: the pattern,
which channels are in play, GROUP, STAY, the advance and RESET. Splitting them
would mean two modules carrying the same controls, kept in step by hand, for the
patch that wants both (a sequencer's notes over eight voices, and the same
line's audio through eight effects). So it is one engine and three lanes.

## The advance

A note is the GATE's rising edge, or TRIG's when TRIG is patched. With TRIG
patched the selection moves on a clock whatever the notes are doing, and a gate
held across a trigger moves to the new channel. GATE is normalled from TRIG, so
a clock alone into TRIG gives each channel the clock pulse while it is selected.

A legato line never lets its gate fall, so it never advances. The menu option
"A new pitch under a held gate is a new note" counts a change of more than half
a semitone under a held gate as a note. It is off by default because a glide or
vibrato under a held gate would deal notes too.

## The lanes

- **CV** is sampled when a channel takes a note and HELD after it ends. A voice
  on channel 3 is still releasing when channel 4 takes the next note, and its
  tail has to stay at its own pitch. While the channel holds the gate it follows
  the input, so a glide or vibrato still arrives (menu: off samples once).
- **Gate** is the input gate, on the selected channel only. A note that stays
  on the channel already holding a high gate (GROUP, STAY, legato) gets a 1 ms
  gap first, or the voice behind it would never hear it start.
- **Audio** is stereo: IN L and IN R (R normalled from L), and an L and R out
  per channel. Both sides go to the selected channel together, with a
  smoothstep crossfade, FADE 0.5 ms to 1 s (default 2 ms, a declick).

**The audio lane is a switch, not a hocket of voices.** A single source has one
tail: when the switch happens, the last note's release and the next note's
attack are already mixed together in it, so switching either cuts the tail or
sends the new attack to the old channel too. Eight voices fed from the CV and
gate lanes ring independently; eight effects fed from the audio lane do not.
FADE runs from a hard switch to a smear.

## Which channels are in play

The eight ON buttons, of which COUNT keeps the first N that are on (COUNT CV is
1 V per channel). A channel switched off while selected lets go at once; a
channel dropped by COUNT finishes its note. The button lights orange while
selected, blue while in play, dim blue when on but beyond COUNT.

## The patterns

Over the channels in play, in channel order:

| Pattern | Order |
|---|---|
| Up, Down | 1 2 3 … / 8 7 6 … |
| Pendulum | 1 … 8 … 1, the ends not repeated |
| Random | any channel, repeats included |
| Random, no repeat | never the same channel twice running |
| Shuffle | every channel once per cycle, a new order each cycle, a cycle never starting where the last ended |
| Stride 3, Stride 5 | k steps at a time. Where k shares a factor with the number in play the stride closes on a subset (3 over 6 is 1 4 1 4), so each closed orbit moves on by one: 1 4 2 5 3 6 |
| Converge | outside in: 1 8 2 7 3 6 4 5 |
| Register | low notes to low channels, scaled to the range of the last 16 notes |
| Drunk | a random step to a neighbour |

**GROUP** (1-8) is how many notes a channel takes before handing on. **STAY** is
the chance that a note stays on the current channel instead. Both apply to every
pattern. **RESET** (a jack; there is no button) makes the next note start the
pattern again; the note sounding is left alone.

## The screen

The pattern and how many channels are in play, then one lane per channel, in
the order of the output rows, with the last four seconds scrolling in from the
right: a band for how long a channel was selected and a bar for each note it
played, orange while it sounds.

## Checks

`tools/hocket-harness.cpp` compiles the module against libRack: every pattern's
order, the random patterns' rules, COUNT with a channel off, the stride over a
shared factor, GROUP and STAY, CV hold and follow, legato and the retrigger gap,
TRIG and its normalling, the crossfade and the stereo pair, RESET and switching
off.

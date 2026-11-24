# Battery

A polyphonic drum sequencer: eight lanes, up to 64 steps each, eight patterns.
A battery is the percussion section of a marching band.

A new module starts blank. Grooves come from the Templates menu.

## Templates

The context menu's Templates submenu holds 27 grooves from Fill's bank
(`res/patterns/drum-patterns-v2.json`), grouped as rock; funk, soul and New
Orleans; electronic; hip-hop; and Latin, Caribbean and African. Fill's grids
use the same lane order as Battery (kick, snare, closed hat, open hat, low
tom, high tom, clap, bell) and describe the same things a step holds:

| Fill | Battery |
|------|---------|
| `v` 1 to 9 | velocity n/9 |
| `a` accent | velocity 1 |
| `p` 1 to 9 | chance n/9 |
| `r` 2 to 8 | ratchet, capped at 4 |

Each template offers its main groove into the pattern on screen, and, where
the set has more than one, every variation (sparse, main, lift, fill, as Fill
names them) into patterns 1 onward. The lane length is the grid's length: 16
for most, 12 for the 12/8 shuffle.

Fill swings its grooves and Battery has no swing of its own, so the menu shows
each groove's swing next to its name. Swing the clock to get it: Meter's
sixteenth output has its own swing, and Battery's x1 lanes play on the clock
edges.

The bank is read once, the first time the menu or the browser thumbnail needs
it. The list of templates is `btTplGroups()`; a set that is not in the bank is
left out with a log line.

## What a step holds

| Field  | Range       | Default | What it does |
|--------|-------------|---------|--------------|
| on     | on/off      | off     | Whether the step plays at all |
| VEL    | 0 to 1      | 0.8     | Held on the VEL out, 0 to 10 V |
| CHANCE | 0 to 1      | 1       | The chance the step plays when the lane reaches it |
| ROUTE  | 0 to 1      | 0       | The chance a played step goes to the lane's B channel instead of its A channel |
| RATCH  | 1 to 4      | 1       | Hits inside the step, evenly spaced |

CHANCE is drawn first. If the step plays, ROUTE is drawn once for the whole
step, so every ratchet of one step lands on the same channel.

## Channels

Lane N plays channel N (A) or channel N + 8 (B) of the 16-channel GATE, VEL
and V/OCT outs. The A and B outs carry the same signals split into two
8-channel cables, because Kit reads channel N as instrument N and has eight
instruments: a 16-channel cable into one Kit would lose the B side. With the
split outs, A drives one Kit and B drives another.

V/OCT carries a note per lane and side (lane menu, Note A / Note B). It exists
for samplers that pick a sample by pitch, such as Play. The defaults are
channel N at N semitones above C4, so the 16 channels are 16 adjacent notes.
Notes and mutes belong to the lane, so they are the same in every pattern.

## Lanes

Each lane has, per pattern, a length (1 to 64), a play order and a rate.

**Play orders.** Forward, backward, pendulum (the end steps play once),
ping-pong (the end steps play twice), random, drunk (one step either way,
wrapping), and serialist: every step of the lane once, in a shuffled order,
then a new shuffle. A new shuffle never starts on the step the last one ended
on, so serialist never plays one step twice in a row. Serialist shuffles every
step within the length, including steps that are off, so the rests move as
well.

**Rates.** /4, /3, /2, x2/3, x3/4, x1, x4/3, x3/2, x2, x3, x4 steps per CLOCK
pulse. Lanes that go faster than the clock, and ratchets, are placed from the
measured clock period, and held short of the next pulse, so a fast lane never
plays ahead of the clock. Until the second pulse has been measured, steps
between pulses do not play.

## Randomness: seeds and LOCK

No random number is taken from a running generator. Each draw is a hash of
the seed, the lane, what is being decided (chance, route, random position,
drunk step, shuffle), the pass through the lane, and the step within the pass.

- Unlocked, the pass number counts up, so every pass draws afresh.
- LOCK freezes each lane's pass number at the pass playing when LOCK was
  pressed. That pass then repeats exactly: the same steps fire, on the same
  channels, in the same order. Locking at the end of a pass keeps the pass you
  just heard.
- NEW changes the seed. Locked, NEW gives a different pass that then repeats.

The seed and each lane's locked pass are saved, so a locked pattern comes back
the same when the patch is opened again. A restart (RESET or a pattern change)
moves an unlocked lane's pass number on, so the draws after a reset are new.

BIAS (knob, plus a polyphonic CV where channel N is lane N) is added to every
step's ROUTE, from -100% to +100%. At +100% every step goes to B.

## Patterns

The pattern asked for is (NEXT count + PATTERN knob + PATTERN CV) mod 8, as on
Canon. NEXT skips patterns with no steps.

A new pattern does not start immediately. With BAR patched, it waits for the next BAR.
Without BAR, it waits for the end of the longest lane: the lane with steps
whose length divided by its rate is largest. Either way every lane restarts at
its first step. The menu item "When a new pattern starts" can instead start it on the
next step, with every lane keeping its position.

A BAR that arrives with its clock pulse, or up to 5 ms after it, belongs to
that pulse's step. A BAR up to 5 ms before its pulse waits for it. Without
this, a BAR a millisecond early played the first step of the new pattern and
then step 2 at the pulse.

EOC fires each time the longest lane goes round, and on a pattern change.

## Transport

CLOCK is one pulse per step. RUN (latch, or the RUN gate when patched) stops
the lanes where they are; the first pulse after a stop is not used as a
period. RESET puts every lane back so that the next CLOCK pulse plays the
first step. MUTE is polyphonic (channel N mutes lane N; a mono cable mutes
every lane), alongside a mute per lane on the screen.

A hit on a gate that is still high goes low for 1 ms first, or the voice
behind it would not see a new note. This happens when the clock speeds up and
a half-step gate from the old tempo is still open.

## The screen

Five tabs choose what a click edits: STEP (on/off, drag to paint), VEL,
CHANCE, ROUTE and RATCH (click or drag up and down in a cell). In the STEP
tab a step is drawn blue (A) below and green (B) above in proportion to its
ROUTE, dimmer the lower its CHANCE, with a tick per ratchet. Shift-click a
step to end the lane there.

To the right of each lane: its length (drag up and down), its play order and
its rate (click for the next one, shift-click for the previous one).
Right-click a lane for its menu: order, rate, length, mute, Euclidean fill,
rotate, randomize, copy, paste, clear, and the two notes.

Along the foot: the eight patterns. Click one to edit it, double-click to
play it next (right-click for copy, paste and clear). The page tabs choose
which 16 steps are shown. Right-click a page tab to copy the page (all eight
lanes, every field of every step), paste a copied page onto it, clear it, or
duplicate it onto the next page or onto every later page. Pasting leaves the
lane lengths alone. Duplicating lengthens each lane that ends at the last step
of the page being duplicated, so that it plays the copies: duplicating page 1
of a 16-step groove onto page 2 makes it 32 steps. A lane of any other length
keeps its length, so a 12-step lane stays 12 steps. By default the screen follows the pattern and page
being played, and holds where a click put it for 5 seconds.

## Checks

`tools/battery-harness.cpp` compiles the module against libRack and checks
that a new module is blank, that every template reads with its grid's length and Fill's ratchets, every play order, serialist's shuffles, CHANCE and ROUTE rates, BIAS, LOCK
and NEW, saving a locked pass, ratchets, lane rates, copying and duplicating pages, pattern changes on the
longest lane and on BAR (early and on time), NEXT, EOC, the channel layout,
MUTE, RUN, RESET, the retrigger gap, saving, and cost: 0.29% of an M4 core
with eight lanes playing.

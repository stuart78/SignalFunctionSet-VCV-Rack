# Spool Manual

## Overview

Spool is four short tapes, one transport, and no rewind.

It is after the Mellotron, and departs from it in the one place that matters.
A Mellotron pulls a tape across the head while a key is held and springs it
back when the key is released, which is why a note can only last eight seconds
and why every press starts from the same instant. Here the tape stays where it
stopped. Press again and it carries on from there, so a repeated stab walks
through the loop instead of replaying the same moment, and the phrase you get
out is a function of how you played rather than of where the file happens to
begin.

Each tape holds up to five seconds of audio, loaded from a WAV file or recorded
in through its own jack. Four tapes, lettered A to D and colour coded on the
panel and the screen, share one envelope, one transport and one set of speed
errors, because they are one machine.

Spool is 22HP.

## The tapes

Each of the four tapes has its own column: a LEVEL knob, a REC button with an
LED, a GATE input, a V/OCT input, a REC IN input and an OUT.

**Loading.** Right-click the module and choose *Load tape N…* to load a WAV
file. Stereo files are summed to mono, any sample rate is resampled, and only
the first five seconds are kept. The menu entry shows the name of the file in
each tape, or *empty*.

**Recording.** Press REC and the tape records whatever is at its REC IN from
the start of the tape. Press REC again to stop, or let it run: it stops on its
own at five seconds. The LED is lit while recording. A recording has no file
behind it, so the audio itself is saved into the patch's own storage folder as
a 16-bit mono WAV, and Rack carries that folder with the patch, copies it on
Save As and removes it when the module is deleted. Nothing you play into Spool
is lost when Rack closes.

**Playing.** A gate at GATE starts the tape rolling from where it stopped, and
the note sounds for as long as the shared envelope says. Each tape has eight
playheads, so a polyphonic GATE plays chords on one tape: channel N of the gate
is a head, and channel N of V/OCT is its pitch. When every head is busy the
quietest one is taken.

**Where a note starts.** The tape keeps a bookmark, and by default every new
note starts there. The bookmark follows the most recent head while its gate is
held and stops where that head was when the gate fell, so what stops the tape
is the gate and what stops the sound is the envelope. The menu option *New
note starts* switches this to *At the beginning*, which is what a Mellotron
does. RESET sends a tape's bookmark to the start: a polyphonic RESET rewinds
one tape per channel, a mono RESET rewinds all four, and the menu's *Rewind
all tapes* does the same by hand.

**V/OCT is varispeed, not transposition.** It changes how fast the tape runs,
so the pitch and the loop's period move together, as they do on any machine
with a capstan. An octave down is twice the loop length. Nothing here
time-stretches, because a Mellotron cannot. Each head reads at the pitch it
was struck at, and while its gate is held it follows the CV.

## The shared row

One envelope and one transport serve all four tapes.

- **ATTACK** and **RELEASE** shape every note. Attack runs from 1 ms to 4 s,
  release from 5 ms to 8 s, and both tooltips read as times. The release
  defaults to about 290 ms, which is long enough that a retriggered gate has
  two or three heads sounding at once: shorter than that and the module sounds
  monophonic however many heads it has.
- **RAMP** is the transport getting up to speed, 0 to 60 ms. It scales the
  playback *rate*, so a note glides up into pitch on the way in and sags on
  the way out. It is also what keeps a tape resumed mid-waveform from
  clicking.
- **SAT** is tape saturation.
- **WOW** and **FLUTTER** are the motor's speed errors, slow and fast. They are
  shared rather than per tape because there is one capstan: four independent
  wobbles sound like four machines. Wow is two slow components rather than one
  sine, because a single sine is a vibrato.
- **ROOT** names the note the tapes are considered to be in, C to B, in the
  plugin's convention, so a ROOT CV from Key or Arrange lands where it means
  to.
- **ABS** decides what V/OCT means. *Relative* is varispeed from the tape's
  natural speed: 0 V plays it as recorded. *Absolute* makes V/OCT a note: the
  tape is measured for pitch and sped up or slowed until what it sounds
  matches what was asked for, relative to ROOT.

ATTACK, RELEASE and RAMP each have a CV input on the bottom row; plus or minus
5 V covers the whole knob.

**Motor sag** lives in the context menu as a slider. On a Mellotron every held
key presses another pinch roller onto the same capstan, so more notes means
more drag and the motor slows: a chord goes flat, by an amount that depends on
how many keys are down. One note is the reference.

**Pitch detection** (context menu) is *Once, when the tape is loaded* by
default. A tape of a sung or bowed note has vibrato and drift in it, and that
is its character; correcting the pitch continuously flattens exactly that out.
*Continuous* tracks the pitch instead, for tapes whose pitch genuinely moves.

## Density

Four tapes with eight heads each is thirty-two voices, and a chord on one tape
would otherwise be louder than a note on it. The mix is normalised by the
summed envelopes of the heads that are sounding, so a chord and a single note
sit at about the same level and nothing steps when a head starts or stops.

## Outputs

- **A, B, C, D**: each tape on its own, mono.
- **MIX L / MIX R**: all four, panned across the stereo field with A at the
  left and D at the right.

## The screen

Four lanes, one per tape, in the tape's own colour. Each lane carries the
tape's letter and the name of what is in it, the waveform as a filled
envelope, a white mark for every head that is sounding, the bookmark where the
next note will start, drawn dim and always, and the detected pitch as a note
and a frequency, which is how you can tell whether the tape is in tune with
the patch. A tape that is recording shows its record position instead.

## Context menu

- **Load tape 1…4**: choose a WAV file for that tape.
- **Rewind all tapes**: every bookmark to the start.
- **Motor sag**: how flat a chord goes.
- **New note starts**: *Where the tape stopped* (default) or *At the
  beginning*.
- **Pitch detection**: *Once* (default) or *Continuous*.

## Tips

- **A walking stab.** Load a long chord, set a short release, and retrigger
  from a clock. Every hit continues from where the last one stopped, so the
  rhythm walks through the recording.
- **The Mellotron.** *New note starts: At the beginning*, RAMP at a few
  milliseconds, WOW and FLUTTER up a little, and play it from a keyboard.
- **A sampler that stays in key.** ABS on, ROOT from Key, V/OCT from a
  sequencer: each tape is retuned to the note asked for whatever it was
  recorded at.
- **Live loops.** Patch a voice into REC IN, press REC for a phrase, then play
  the phrase from a gate. The recording is in the patch when you save.

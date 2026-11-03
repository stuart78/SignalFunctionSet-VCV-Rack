# Concrète

Four lanes of samples on one tape loop (32HP), with each lane's stereo output on
the panel (L over R) beside the mix, and one expander: **Concrète In** (10HP, on
the left) records into the lanes. Its rows sit level with the lanes on the
screen. (Concrete Out, a 10HP expander for the lane outputs, was folded into the
panel on 2026-11-01. The slugs stay ASCII: `Concrete`, `ConcreteIn`.)

## The tape

The loop is 1–8 bars (BARS) with one playhead. A lane holds clips. A clip is
a window into a sample, placed at a bar position and played at its own speed.

**What a lane plays at the playhead is the clip that started most recently at
or before it.** Everything else follows from that rule:

- A clip runs until its audio ends, the next clip on the lane starts, or the
  loop ends.
- **The loop end cuts a clip off.** Nothing carries round into bar 1, as on a
  real tape loop.
- Lengthening a clip (dragging its right edge, slowing it down) is never
  destructive. It runs into the next clip and is cut there.

**Putting a clip down is a splice.** Dropping a file, moving a clip, or
committing a recording replaces what the clip covers. Anything it landed in the
middle of carries on after it, from where that audio had got to. A clip that
covers another entirely replaces it.

## Speed

**Speed is varispeed**: pitch and length change together, so a clip at x0.5 is
an octave down and twice as long on the timeline. The range is x1/8 to x8, and
a clip can be reversed.

A clip can instead **follow the tempo** (clip menu). It then keeps the same
number of bars, and its speed is worked out from the bar length every sample.
A loop recorded at another tempo lands on the grid this way. Its pitch follows
the tempo, as a tape slowed down would.

## Tempo

| Patched | The bar length comes from |
|---|---|
| CLOCK and BAR | the clock period × clocks per bar, counted between BARs |
| CLOCK | the clock period × "Clocks per bar with BAR unpatched" (menu, default 16) |
| BAR | the time between BARs |
| neither | the BPM knob, four beats to the bar |

A clock **drives** the playhead:

- Each clock edge puts the playhead exactly on its grid position.
- Between edges the playhead moves at the measured rate, but never past the
  next clock.
- **A clock that stops stops the tape**: after 1.5 periods with no edge, every
  lane fades out.
- The first clock after a reset is the downbeat of bar 1.
- **BAR moves the playhead to the nearest bar line.** A BAR that arrives a
  sample before or after its clock lands on the same bar.

Playing a clip reads its audio at its own rate from the moment it starts. A
late clock moves the playhead, not the audio.

PLAY is a latch, and the PLAY gate overrides it when patched. Stopping holds
the playhead where it is. RESET (button or jack) returns to bar 1, and "Reset
when PLAY starts" is a menu option. Clicking the ruler moves the playhead.

## Edges

Every edge is faded over 3 ms: a clip starting, being cut, reaching its end,
the loop end, a reset and a seek. When what a lane should be playing changes,
the old voice fades out while the new one fades in. A natural end is faded
*before* the last sample, because the remaining length is known. Measured by
the harness, the largest sample-to-sample step at a switch between ±4 V clips
is 0.03 V.

## The curves

Each lane has four curves across the loop (256 samples a bar, over all 8 bars,
so shortening the loop does not lose them). They are drawn on the VOL, FILTER,
PAN and PITCH tabs.

- **VOL** runs from silence to unity, square law.
- **FILTER** is Phase's DJ filter: lowpass below the centre, highpass above,
  and straight through within 5% of the centre. The user's LPF and HPF became
  one control here, which left three curves.
- **PAN** is a balance: the centre leaves both sides alone, and full right
  silences the left.

A curve is **points joined by straight lines**, edited like vector art:

- Click a lane to add a point, and drag a point anywhere. It stays between its
  neighbours, and its bar follows the grid when snap is on.
- Double-click a point, right-click it, or press Delete over it to remove it.
- Before the first point and after the last, the curve holds that point's
  value. With no points, it is the default.
- Two points on one bar make a step.
- **PITCH** is varispeed on the whole lane, an octave up or down: the audio
  reads faster or slower, so pitch and duration change together, as on a tape.
  A clip under a raised curve runs out before its drawn end, and one under a
  lowered curve is cut off by the next clip or the loop end. The clip is still
  drawn at its speed without the curve. A voice started part way through a clip
  (a seek) integrates the curve to find where playing through would have got to.
  Points land on semitones while the snap is on, and Ctrl (Cmd) frees them.
  The read speed updates every 8 samples and moves the read position smoothly,
  so a step in the curve is a fast glide (slewed 5 ms) and does not click.
- There are detents at unity (VOL) and at the centre (FILTER, PAN, PITCH).
- A dragged point stays on its bar until the drag has gone 2.5 mm sideways, so
  moving it up or down does not also move it along. Shift holds it on its bar.
- A bar holds two points at most: the value arriving and the value leaving. Any
  between them would have no effect, so a click that would add a third picks up
  the nearer of the two, a point dragged onto a full bar removes the one it lands
  beside on release, and a patch that has stacks loses them on load.

The points are rendered into 64 values a bar for the audio thread, and the
curves are slewed by 5 ms, so a step does not click.

PITCH acts where the clip is read. The chain per lane is **clip → VOL → FILTER → PAN**. That is what each lane's
output carries, unclipped, and the four lanes sum to Concrete's L/R mix. The mix is soft-clipped:
linear to 6 V, asymptotic to 10 V.

## Editing on the screen (CLIPS tab)

- Drag a clip's body to move it, including to another lane. When it is dropped,
  it splices.
- Ctrl (Cmd) drag a clip's body to drag a copy, within the lane or to another
  one. The original stays where it was, and the copy splices where it is
  dropped.
- Drag the left edge to trim the start (the clip's start on the tape moves with
  it). Drag the right edge to trim the end. The time in the sample is shown
  while dragging.
- Hold Ctrl (Cmd) while trimming to move the edge at 1/16 of the mouse's speed,
  or Ctrl+Shift for 1/256, as on a Rack knob. Either leaves the grid, and shows
  the audio round the edge at the scale the mouse is moving, with the trimmed
  side dim. Across a 4-bar loop at 100% zoom, 1/256 is about three frames per pixel.
- Shift-drag the right edge to change the speed: the window stays and its
  length on the tape changes.
- Snap to the clock grid is on by default (menu). Ctrl (Cmd) while dragging
  inverts it, except on a copy, where Ctrl is what asks for the copy.
- Drop WAV or FLAC files on a lane to place them at that bar. Several files go
  one after another.
- Right-click a clip for speed (presets, octave, semitone), Reverse, Follow the
  tempo, Restore the whole sample, Duplicate after it and Delete.
- Click a clip to select it. The Delete or Backspace key removes the selected
  clip, wherever the pointer is. With nothing selected it removes the clip or
  curve point under the pointer. Over the screen those keys never reach Rack,
  which would remove the module: `ModuleWidget::onHoverKey` acts on them, press
  and repeat, before offering them to its children, so `ConcreteWidget` catches
  them first, and the screen, as the selected widget, hears them before that.
- Right-click an empty part of a lane to load a sample there.

## The view

The header picks how much of the loop the lanes show: **1, 2 or 4 bars, or
ALL**. The view is always a whole number of bars and starts on a bar line, so
editing happens on a fixed page rather than a free zoom. A button opens the
page holding the playhead. Ctrl (Cmd) + scroll steps through the same sizes
about the bar under the pointer, and scrolling sideways moves the view a bar.
Plain scroll is left to Rack, so moving the pointer over the screen while
scrolling the rack does not catch it. Option/Alt is not used, because Rack
already uses it to pan the rack.

The strip under the header is the whole loop at all times: every lane's
clips, the playhead, and, when zoomed, a frame round what the lanes show. Drag
in the strip to move the view, a bar at a time.

A zoomed view turns the page when the playhead leaves it ("Zoomed view follows
the playhead", menu, on by default). Touching the view holds it where you put
it for three seconds. Waveforms are drawn from the screen peaks (256 frames
each) until a column covers fewer than 512 frames, and from the samples
themselves past that, so the detail grows with the zoom. The view is saved
with the patch.

**The beat grid:** every second quarter note is shaded across the lanes. Every
quarter has a line and every bar a brighter one. The ruler ticks each clock,
taller on the beat, and numbers the beats (`2.3`) once there is room. Beats per
bar are 4 when the clock count per bar divides by 4, and 3 when it divides by
3 and not 4.

## Recording (Concrete In)

Per lane: IN L and IN R (R normalled from L), a REC latch and a REC gate.
Either one arms the lane. A lane records while it is armed, has a cable in L
or R, and the tape is moving.

- **While recording, the lane plays its input** through its curves, as the
  monitor.
- **When recording stops, the take becomes a clip** where it was made, and it
  is spliced in. A punch-in over the middle of a clip leaves the rest of that
  clip playing after the take.
- **Held across the loop end, recording splits.** One take runs to the end and
  the next starts at bar 1, as recording over a tape loop would.
- A reset or seek also ends a take.
- The REC light blinks while armed and stays lit while recording.

Concrete holds two buffers per lane, sized to the loop (×1.25, plus 2 s; at most
120 s). One is kept ready while Concrete In is attached. The second is allocated
as soon as the first starts recording, so a take that crosses the loop end has
somewhere to go. Takes are committed on the UI thread. Nothing is allocated in
`process()`.

## Saving

- **A take has no file behind it.** It is written into the patch's storage
  directory as a 16-bit stereo WAV (`concrete-take-<id>.wav`).
- **Loaded samples are kept by path.** "Keep copies of loaded samples in the
  patch" (menu) also copies each file into the storage directory, and a patch
  then loads the copy first.
- Turning the option off removes the copies at the next save, except where the
  original file has gone.
- Storage files that nothing refers to are removed at save.
- A sample that cannot be found stays on its lane, marked "missing", with its
  place and length kept.

## Threads

The UI thread writes the clips and curves; the audio thread only reads them. A
clip carries a generation number that changes whenever its mapping does, which
is how a lane notices that it should switch. Samples are published by
pointer, and freed a second after nothing refers to them, so a voice fading
out never reads freed memory. Takes move FREE → READY (UI) → REC → SEALED
(audio) → committed and FREE (UI).

## Checks

`tools/concrete-harness.cpp` compiles the module against libRack and checks:

- the transport, a clip on its bar, the cut-off by the next clip and by the
  loop end;
- varispeed, reverse, and following the tempo at two tempos;
- the splice, and the tail resuming where the tape had got to;
- VOL, PAN, and both halves of FILTER;
- following CLOCK and BAR, the first clock landing on the downbeat, and the
  stopped clock;
- recording, playing back in stereo, recording across the loop end, and a
  punch-in over a clip;
- the largest step at every kind of edge;
- the cost: 0.5% of an M4 core with four lanes playing and every curve moving.

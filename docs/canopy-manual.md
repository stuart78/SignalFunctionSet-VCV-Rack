# Canopy Manual

## Overview

Canopy is a set of instruments played by drops falling past leaves you place. TYPE
selects the instrument: a steel tongue drum (the default), a zither, a steel pan,
singing bowls, wind chimes, a handpan, a bonang, a marimba or a balafon. Drops
appear above the instrument and fall under gravity, bounce off the leaves, and sound
it where they land. You can also strike the notes directly from a GATE input, with
or without any drops falling.

Most of this manual describes the tongue drum. [Instruments](#instruments) covers
what changes on the other eight.

The drum and the drops are modelled, and the screen is drawn from that model, so a
drop you see landing on a tongue is the sound you hear. The tongue overtones and the
material values are estimates chosen by ear and by measurement of the module, not
data from a real instrument.

## How it works

Drops fall through a column as wide as the drum. A leaf hit makes no sound: it
changes where the drop goes next and how fast it is moving. Every landing on the drum
makes a sound, because the whole top is one piece of steel.

- **On a tongue:** that tongue rings at full strength and its neighbours ring faintly.
  Where along the tongue the drop lands sets which overtones sound.
- **Between tongues:** the nearest tongues ring quietly and the shell is louder, so
  the note sounds muted.

A drop that bounces lands again, so one drop can play several notes. A drop that has
settled for 1.5 s, or has been in play for 15 s, rolls off the rim.

## The notes

ROOT and SCALE set the notes, and SCALE also sets how many tongues the drum has:

| Scale | Tongues |
|---|---|
| Up to 8 notes | two octaves, so a pentatonic scale has 10 and a major scale has 14 |
| More than 8 notes | one octave, so chromatic has 12 |
| Any | at least 3, at most 16 |

The lowest note is the big tongue in the middle. The others go round it, rising up
alternate sides from the front to the back.

A new module opens on Pentatonic Minor, because a tongue drum a semitone apart does
not sound like the instrument. A SCALE cable therefore adds to a knob that is not at
the bottom of the list. Turn SCALE to the bottom if you want the voltage to be the
index. Key's scale bus replaces the knob whatever it is set to.

## Instruments

TYPE changes the instrument under the leaves. The drops, the leaves and every other
control stay as they are. A change fades the output out over 5 ms and back in, so a
CV can switch instruments without a click. The ratios and ring times of every
instrument's overtones are estimates.

| TYPE | What the drops play |
|---|---|
| **Tongue drum** | The default, described in the rest of this manual. |
| **Zither** | Strings running toward you, low and long at the left, one per tongue the drum would have. Where along a string a drop lands sets its tone: a hit at the middle has no even harmonics. A drop wider than the gap between strings rests on two and rolls down them, damping the harmonics that move most where it sits and sustaining the rest, so the strings sound quietly as it rolls. A drop that falls between strings knocks the board. |
| **Steel pan** | A low-C tenor pan, C4 to E6, 29 pads in a bowl tilted toward you. The outer ring is the lowest octave, the middle ring the next, and five notes sit in the centre. Round each ring the notes go in fifths. A hit near a pad's edge brings out its upper partials, and pads a fifth, an octave or a twelfth away ring with it. A drop rolling down the bowl knocks each pad it crosses. **Steel pan tuning** in the menu sets which notes the pads play. |
| **Singing bowls** | One bowl per note of the scale over one octave, 3 to 8 bowls, the lowest and largest in the middle. Each drop is equally likely to fall over any bowl. A hit on one line across a bowl rings steady and a hit between beats. A drop rolling round inside sustains the bowl. Water stays in the bowl it lands in, which lowers the note and shortens the ring, and dries out over three minutes. |
| **Wind chimes** | Tubes hanging from a ring, one per note of the scale over one octave, with a clapper and sail in the middle. Every tube, the clapper and the sail swing on their cords, so one drop can set off a run of notes. |
| **Handpan** | A convex steel shell with a raised centre note and eight notes round it, rising left and right alternately from the front. Drops run across the notes and off the edge. |
| **Bonang** | Javanese kettle gongs in two rows on a frame, the far row an octave above the near one, following SCALE (Slendro gives 10 kettles, Pelog 14). A hit on a kettle's raised centre gives the note; a hit on the flat top round it gives a muted knock. |
| **Marimba** | A chromatic keyboard over two octaves, laid in a circle: the naturals on an outer ring, the sharps raised on an inner ring. Notes outside SCALE are damped. |
| **Balafon** | One row of the scale's notes on gourds. A hard hit makes the gourd's membrane buzz; a soft one stays clean. |

On the bonang, the marimba and the balafon, a falling drop is drawn toward the
nearest kettle or bar in the last part of its fall, since a thrown drop would
otherwise land in the gaps between them.

Every instrument after the zither starts its notes at C4 (set by **Lowest note** in
the menu), and V/OCT picks the note nearest the pitch it sends.

## Controls

### Drops

| Control | What it does |
|---|---|
| **RATE** | Drops per second, 0.05 to 12. Fully left, no drops fall, so only CLOCK and GATE play. Each volt of CV doubles the rate, and a positive CV opens a knob that is fully left. A patched CLOCK replaces it. |
| **MATERIAL** | Water, rubber, wood, glass, steel, or Mix, where each drop picks one and keeps it. |
| **SIZE** | Drop diameter, 16 to 49 mm on a 45 cm drum. |
| **SCATTER** | Width of the column, from a single point to the full width of the drum. |

Material sets weight, contact time and bounce. A heavier drop is louder. A longer
contact loses more of the overtones, so rubber sounds round and steel sounds bright.
Water does not bounce: it strikes once and breaks into pieces that strike again, and
a large drop breaks twice.

### Leaves

| Control | What it does |
|---|---|
| **DENSITY** | How many leaves are in play, 0 to 12. Default 3. |
| **LEAF** | Leaf, felt, rubber, wood, glass, steel, or Mix (a different material per leaf). |
| **DRIFT** | How far each leaf moves from where you placed it. At zero it does not move. |
| **SPEED** | How fast the leaves drift, 0.02 to 1.5 Hz. |
| **TILT** | How far each leaf turns and rocks as it drifts. |

The leaf's material sets how much bounce a drop keeps, how much it slides and how the
leaf bends. A glass marble rebounds at 0.08 of its speed off felt, 0.22 off leaf and
1.03 off rubber. Because a leaf is silent, its material reaches the sound only through
where and how hard drops land next.

Leaves are in real proportion to each other and to a 45 cm drum: monstera 30 cm, oak
15, maple 12, beech 10, willow 8, ginkgo 6. A small form meets far fewer drops than a
large one, so raise DENSITY when you use one. With 8 leaves, about a third of drops
touch a ginkgo or willow leaf, and all of them touch a monstera.

### Instrument

| Control | What it does |
|---|---|
| **TYPE** | The instrument the drops play. See [Instruments](#instruments). |
| **DECAY** | Ring time, 0.25x to 4x. |
| **TONE** | Level of the overtones. |
| **ROOT** | The root note. |
| **SCALE** | The scale, from the plugin's shared list. |

## The screen

The screen is a 3D view with no background panel behind it.

- **Drag a leaf** to move it level.
- **Shift-drag a leaf** to change its height.
- **Drag an end of a leaf** (dots appear on hover) to steer it. Its size stays the same.
- **Drag empty space** to orbit the camera.

A ringing tongue dips and fills from grey where it joins the drum to orange at its
free end. A leaf turns green briefly when a drop hits it, more strongly for a harder
hit. A drop is a circle, darker for heavier, and water is an outline. The shadow under
a drop shows its height.

## Inputs and outputs

| Jack | What it does |
|---|---|
| **V/OCT** | Polyphonic. Selects the tongue a gate strikes: the nearest to the note, moved by octaves into range. |
| **GATE** | Polyphonic. Strikes a tongue with no drop. With V/OCT unpatched, channel N strikes tongue N. MATERIAL and SIZE act as the mallet. |
| **VEL** | Polyphonic, 0-10 V. How hard a gate strikes. 70% when unpatched. |
| **CLOCK** | Each pulse releases one drop. RATE has no effect while this is patched. |
| **X**, **Z** | Move the column across and front to back, ±5 V from edge to edge. The column narrows to stay over the drum. |
| **POLY** | One channel per note, lowest first. The steel pan has 29 pads, so POLY carries the 16 loudest. |
| **LEFT**, **RIGHT** | The stereo mix. |
| **CV** | The pitch of each note struck, by a drop or by GATE, 1V/oct, held until that channel's next note. The pitch is the note as tuned, so water in a bowl does not change it. |
| **TRIG** | A 1 ms trigger for each note struck. A hit between notes, the neighbours that ring with a note, and a very light touch send nothing. |

Every control in the three groups also has a CV jack under it. DENSITY reads 1 V per
leaf, MATERIAL and LEAF read 1 V per material, and TYPE reads 1 V per instrument. SIZE, SCATTER, DRIFT, SPEED, TILT,
DECAY and TONE cover their whole range over ±5 V.

## Context menu

- **ROOT CV:** 1V/oct, 1 V per semitone, or 0-10 V across the 12 notes.
- **Steel pan tuning:** Chromatic; Tuned to the scale, where each ring holds the
  scale's notes in order; or Chromatic with the notes outside the scale damped (the
  default), where a damped pad thuds and stops.
- **Lowest note:** C2, C3 (the default) or C4 for the lowest tongue's root. The
  instruments after the zither start an octave higher.
- **Leaf form:** Monstera, Beech, Maple, Ginkgo, Oak, Willow, or All forms in turn.
- **Evenly spaced drops:** off, the gaps between drops are random.
- **CV/TRIG channels:** 1 (the default) to 16.
- **CV/TRIG note to channel:** Rotate, where each note goes to the next channel, or
  Reuse, where a note struck again goes back to the channel that carried it.
- **Reset leaf positions** and **Reset view.**

## Patch ideas

**Rain on a pentatonic.** Leave the defaults and patch LEFT and RIGHT to a reverb. Turn
SIZE down and RATE up for a light shower, or set MATERIAL to water and SIZE up for
drops that break apart.

**Clocked marbles.** Patch a slow clock to CLOCK and set MATERIAL to glass. Each pulse
releases one marble, and the bounces fall between the clock pulses at times the clock
does not set.

**A melody over the rain.** Patch V/OCT and GATE from a sequencer. The sequencer plays
the tongues directly while the drops keep landing on the same drum.

**Moving the rain.** Patch slow LFOs to X and Z with SCATTER low. The drops walk across
the tongues, so the LFOs choose the notes.

**One voice per tongue.** Patch POLY through a split into separate effects, or into a
polyphonic filter, to treat each tongue on its own.

**Rain playing another voice.** Set CV/TRIG channels to 4 in the menu and patch CV
and TRIG to a polyphonic voice and envelope. Every note the drops play is sent on,
in key, since the CV is the note as tuned.

**Played from GATE alone.** Turn RATE fully left and patch V/OCT and GATE. No drops
fall, and the instrument is a polyphonic voice you play directly.

## CPU

Under 1% of an M4 core in heavy rain on the drum, and 1.6% on the zither.

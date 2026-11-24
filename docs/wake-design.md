# Wake

Up to four cycles drive on a grid. Each moves one cell per step, leaves a
trail behind it, and never turns back onto its own trail. Each cycle's turns
set its pitch, and a crash fires its CRASH gate.

The idea comes from the light-cycle game in the film TRON. The module uses a
different name because "light cycle" belongs to TRON, and because the plugin
already has a module called Cycle.

## The arena

The grid is 64 x 26 square cells by default. The menu offers 32, 48 and 96
columns as well; changing the grid clears the arena. The grid has the screen's
proportions, so the row count follows from the column count and the shape.

### Grid shapes

The menu's Grid shape item chooses the cells:

| Shape    | Cells | Headings | Default grid |
|----------|-------|----------|--------------|
| Square   | squares | 4, turns of 90 degrees | 64 x 26 |
| Hex      | hexagons, in rows with every other row shifted half a cell | 6, turns of 60 degrees | 64 x 30 |
| Triangle | triangles, pointing down and up in turn along each row | 6 between them, but each cell has only 3 neighbours | 64 x 16 |

On every shape a cycle still has three moves at most: straight on, left or
right. On Hex a turn is 60 degrees, so a cycle needs three turns to reverse
its heading, and it cannot follow a wall round a corner the way it can on the
square grid.

A triangle has no cell straight ahead of it: the way out of a triangle is
across one of its two other edges, so every move is a left or a right turn.
Turning left and right in turn moves the cycle along a straight line, which
zigzags along the edges of the cells. On Triangle, "straight on" means the
other way from the last turn, and the line turns 60 degrees when two moves in
a row turn the same way. The styles, the keys and TURN work in lines on this
shape: a key press or a TURN trigger turns the line, which takes one move when
the last turn was the same way and two when it was not. In 3D, a Triangle
cycle is drawn pointing along its line rather than along each zigzag.

Hex and Triangle need an even number of rows, so that a cycle crossing an open
edge arrives on a cell of the same kind.

On Hex and Triangle the arena's edge follows the cells, so it is a jagged
line. A cycle cannot turn sharply on these shapes, so a cell can be a dead end
even with free cells round it: at the end of a shifted Hex row, a cycle
heading east has no cell ahead, up-right or down-right of it. On these two
shapes Careful, Hunter, Hugger, Zigzag and Spiral therefore check, along with
the free cells beyond each move, that the move leaves four more moves the cycle
is allowed to make. Straight still goes straight until the cell ahead is
blocked, so on Hex and Triangle it meets the walls at an angle, turns 60
degrees, and lives about a tenth as long as it does on the square grid.

Each step, every cycle that is due to move chooses a heading by looking at the
board as it is. Then the tails that are due are removed, and then every cycle
moves at once. Because tails go first, a head may enter the cell that a tail
leaves on the same step, as in the game Snake.

A cycle crashes when it:

- reaches an edge (with the menu's Edges set to Solid, the default);
- enters a cell that is part of any trail, its own included;
- enters the same cell as another cycle on the same step. Both cycles crash.

When a cycle crashes, its CRASH out fires, its trail is removed, and after the
restart delay (menu, 2 steps by default) it starts again from a random free
cell. The new cell has at least four free cells ahead of it and is at least
four cells from any other cycle's head, while such a cell exists. On screen,
the removed trail fades out over half a second and a ring opens where the
cycle crashed.

With the menu's Edges set to Open, a cycle that leaves one edge comes in at the
opposite edge.

## Pitch

Each cycle's V/OCT is a note of the key set by ROOT and SCALE, inside RANGE
octaves above the root. ROOT is a note name (trimpot, plus 1V/oct from its CV,
quantized to semitones; the menu's ROOT CV item offers 1 V per semitone and
0-10 V across the 12 notes), and SCALE is one of the plugin's nineteen scales
(trimpot, plus 1 V per scale from its CV, or Key's polyphonic scale bus, which
carries any scale Key holds). SCALE starts on Chromatic, the bottom of the
list, so a new module plays every semitone.

NOTES chooses how a cycle's movement becomes a note (trimpot, plus 1 V per
mode from its CV). It applies to all four cycles.

| Notes      | What sets the note | When GATE fires |
|------------|--------------------|-----------------|
| Turns      | Each turn adds the interval of the new heading to a running total in semitones | On each turn |
| Every cell | Each move adds the interval of the heading | On each move |
| Steps      | A right turn moves up one degree of the scale, a left turn down one | On each turn |
| Column     | How far across the arena the cycle is: the root at the left edge, RANGE octaves up at the right | When the note changes |
| Row        | How far up the arena the cycle is: the root at the bottom edge, RANGE octaves up at the top | When the note changes |
| Room       | The free cells the cycle can reach from where it is, against the whole arena: high in open space, low when it is enclosed | When the note changes |
| Chase      | The distance to the nearest other cycle (in steps on Square, in a straight line on Hex and Triangle): high when close, the root when it is alone | When the note changes |

GATE also fires each time a cycle starts a new life. A gate that is still high
goes low for 1 ms first, so a voice sees a new note. Gate and crash length is
half a step by default, or a 5 ms trigger (menu).

**Turns and Every cell** count semitones. The interval for each heading is set
in the menu; the default is the one Wake was designed with, north +12, south
-12, east +7, west -7. The running total is moved to the nearest note of the
scale, then folded into RANGE octaves. So with the default intervals a cycle
that keeps turning between east and north walks up the circle of fifths, and
north and south move it between octaves of the same note. Going straight holds
the note. The menu has five other interval sets (fifth and fourth, fifth and
major third, fifth and minor third, fourth and whole tone, whole tone and
semitone). In Every cell, a cycle going east plays a new note on every step.

On Hex and Triangle there are six headings. The heading 30 degrees above east
on Triangle, or east on Hex, takes the set's E interval; the heading 60
degrees anticlockwise from it takes N; the heading 120 degrees anticlockwise
takes N minus E; and the three opposite headings take the negatives. So in
Every cell a cell's total is the same whichever path reached it, as on the
square grid. With the default set on Hex, east is +7, north-east +12 and
north-west +5. With the fifth and major third set on Hex (E +4, NE +7, NW +3),
the grid is the Tonnetz, the lattice of thirds and fifths used in music
theory. On Triangle every move is a turn, so Turns adds an interval and fires
GATE on every move, as Every cell does, and Steps moves a degree on every
move: a cycle going straight alternates between two neighbouring notes.

**Steps** counts degrees of the scale and wraps at RANGE octaves. A Spiral
cycle always turns right, so it plays a rising scale; a Zigzag cycle turns left
and right in turn, so it alternates two neighbouring notes.

**Column, Row, Room and Chase** divide their range into the degrees of RANGE
octaves of the scale, plus the root at the top. On the default 64-column grid
in C major over one octave, Column plays a new note every 8 columns. Room uses
the count the cycles already make to choose their moves, so it costs nothing
more. As the arena fills, every cycle's Room note falls.

A new life starts on the root in Turns, Every cell and Steps, and on the note
of its position in the other modes.

With RANGE at 1, the octave intervals of the default set have no effect on the
pitch in Turns, but the turns still fire GATE.

## Score

A cycle scores one point for every cell it moves, and 50 points each time
another cycle crashes into its trail. A crash into a wall, into its own trail,
or head-on scores nothing. SCORE is 0.01 V per point. By default it goes back
to 0 V after 10 V and counts up again (menu: or stays at 10 V). The screen
shows each score as a number. RESET, or the menu's "Clear the arena and the
scores", sets every score to 0. Scores are saved with the patch.

## Styles

Each cycle has a STYLE (trimpot, plus 1 V per style from its CV), and the
style decides where it goes. Every style except Keys and Random looks at how
many free cells each of its three possible moves leads to (a flood fill); a
move is called safe below when it leads to at least half as many free cells as
the best move.

| Style    | What it does |
|----------|--------------|
| Straight | Goes straight, and turns only when the cell ahead is blocked, toward the side with more free cells |
| Careful  | Takes the move with the most free cells beyond it, and avoids cells next to another cycle's head |
| Hunter   | Heads for the cell two ahead of the nearest other cycle, using safe moves only |
| Random   | Turns at random, about one move in five, and looks only one cell ahead, so it crashes more often |
| Hugger   | Takes the safe move whose cell touches the most walls and trails, so it fills the space it is in |
| Zigzag   | Turns every 3 cells, left and right in turn, so it moves diagonally |
| Spiral   | Turns right after runs of 1, 1, 2, 2, 3, 3... cells, drawing a square spiral outward |
| Keys     | Steered from the keyboard |

When the move a style wants is not safe, it takes Careful's move instead.

The styles give different melodies. Straight turns rarely, so it holds long
notes. Spiral turns right every time, so with the default intervals in Turns it
repeats an arpeggio (+7, -12, -7, +12). Zigzag alternates two headings, so its pitch
climbs or falls by the same two intervals in turn.

Alone on the default grid with no trail limit, the automatic styles fill
1000 to 1650 of the 1664 cells before they crash, except Random, which
averages about 80.

## Keys

Click the screen, then **Z** turns the first cycle whose style is Keys to the
left and **X** turns it to the right. **,** and **.** (the keys marked < and >)
turn the second Keys cycle. Turns are relative to where the cycle is heading,
so the keys steer the same way whichever way the 3D camera faces. With no key
pressed, a Keys cycle goes straight.

Rack uses none of these keys without Ctrl or Cmd, so they do not compete with
Rack's own shortcuts. The arrow keys are left to Rack, which scrolls the rack
with them. Wake takes Z, X, comma and period only while a cycle is on Keys;
otherwise they go to Rack as usual. Rack sends keys to the widget that was
clicked last, so the keys keep working wherever the pointer goes, until
something else is clicked. Hovering over the screen also works.

Each key press is queued and used on one move, so two quick presses of Z make
a U-turn over two steps.

## Other controls

- **RATE**: 0.5 to 32 steps per second, plus 1 V per doubling from its CV. Not
  used while CLOCK is patched.
- **CLOCK**: one step per pulse.
- **TRAIL**: how many cells a trail keeps, from 4 to 1024, or no limit at the
  top of the knob (the default). With a limit, the oldest cell is removed as
  each new one is added. TRAIL CV adds a tenth of the range per volt.
- **TURN** (per cycle): a trigger makes the cycle turn at its next move, toward
  the side with more free cells, whatever its style.
- **ON** (per cycle, lit in the cycle's colour): turning a cycle off removes
  its trail; turning it on starts it on the next step. The light is dim while
  the cycle waits to start again.
- **RESET** (button and input): clears the arena and the scores. The cycles
  start again on the next step.
- Right-click a cycle's name on the screen to choose its style, turn it on or
  off, and set how often it moves: on every step, or every 2nd, 3rd or 4th.
  A slower cycle's gate and crash lengths are half of its own step.

## The screen

The screen shows each cycle's letter, style, current note and score along the
top, over the arena. The menu's Screen item chooses how the arena is drawn:

- **3D** (the default): the arena in perspective. A trail is a semi-transparent
  wall in the cycle's colour, lit along its top edge, and each cycle is drawn
  as a motorcycle at the end of its wall, black with edges and wheel rims in
  its colour. The point where the wall comes out of the cycle glows. Between
  steps, each cycle is drawn part of the way to the cell it is entering, so it
  moves smoothly. When a trail is removed after a crash, its wall sinks into
  the floor over half a second. With solid edges the arena has a low wall
  round it. Drag on the screen to turn the camera round the arena and to tilt
  it; double-click to put it back. The camera's position is saved with the
  patch.
- **Flat**: the arena seen from straight above, with trails as lines.

The walls are drawn as light: each one adds its colour to what is behind it,
so where two walls overlap on screen they are brighter, and the walls never
have to be drawn in a particular order among themselves. Each straight stretch
of a trail is one panel. Only the motorcycles are solid: each is drawn after
the walls behind it and before the walls in front of it.

## Cost

A step runs a flood fill for each possible move. Moves that lead into an area
another move's fill has already covered reuse that count, so on an open board
each cycle fills once. Four cycles on the largest grid at 32 steps per second use about 0.6% of an M4
core on Square (0.9% with the machine busy). Hex at 96 x 44 costs about a
quarter more, since it has more cells with six neighbours each; Triangle costs
about the same as Square. The longest single step measured 75 to 180 us.

## Checks

`tools/wake-harness.cpp` compiles the module against libRack and checks the
pitch total and its fold into RANGE, one gate per turn and per start, the
every-cell mode, ROOT and SCALE (a total snapped to the scale, ROOT CV), each
NOTES mode (Steps up and down and its wrap, Column and Row across the arena
with a gate per new note, Room in the open and in an enclosed pocket, Chase
near and far, NOTES CV), Keys ignoring a turn back, crashes at an edge and into a
trail (with the score for the trail's owner), two cycles into one cell, the
restart delay, TRAIL's limit and a tail leaving the cell a head enters, open
edges, TURN, CLOCK, RATE, RESET, the keys through Rack's own event routing (Z, X and
period steer, their repeats and releases stop at the screen, the arrow keys and
Ctrl+Z go to Rack, and X goes to Rack while no cycle is on Keys), saving, how long each style lives alone, four
cycles together (every cell on the board belongs to the trail that holds it),
and on Hex and Triangle: an even number of rows, neighbours 1 apart and the
opposite heading leading back (across open edges too), three or six
neighbours, Straight holding a line, one key press turning the line 60
degrees, heading intervals that add up the same along any path, each style's
life length, and four cycles together. Then the cost on every shape.

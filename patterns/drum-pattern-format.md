# SFS Drum Pattern Library — Import Format v1

A JSON interchange format for built-in drum patterns in SignalFunctionSet modules.
Designed to be hand-authorable (string grids, same spirit as `fillRow()` in
`fill.cpp`), trivially parseable in C++ with no dependencies beyond Rack's
bundled jansson, and expressive enough for global material: variable meters,
triplet grids, multi-bar clave cycles, and *linked* patterns that form the
sections of a piece.

---

## 1. Top level

```json
{
  "formatVersion": 1,
  "lanes": ["kick", "snare", "chh", "ohh", "lo", "hi", "cp", "bell"],
  "patterns": [ ... ],
  "sets": [ ... ]
}
```

* `formatVersion` — integer; bump on breaking changes.
* `lanes` — the canonical lane order for this file. v1 fixes the 8 lanes below.
* `patterns` — flat list of single patterns (one section of one groove).
* `sets` — linked-pattern groups that tie sections together into a playable piece.

## 2. Lanes

Lanes are **roles**, not fixed instruments. A module maps each role onto
whatever voices it has; a 5-voice module folds lanes down (suggested fold in
the table). Patterns omit lanes they don't use.

| Key    | Role                | Typical instruments                          | Fold to 5 voices |
|--------|---------------------|----------------------------------------------|------------------|
| `kick` | Low anchor          | Bass drum, surdo, low doum                   | kick             |
| `snare`| Backbeat / caixa    | Snare, caixa, timbale strokes                | snare            |
| `chh`  | Closed timekeeper   | Closed hat, hihat foot                       | closed hat       |
| `ohh`  | Open timekeeper     | Open hat, sizzled hat                        | open hat         |
| `lo`   | Low percussion      | Low tom, low conga, floor tom, low surdo     | perc             |
| `hi`   | High percussion     | High conga/bongo, shaker, ganza, tambourine, tek/ka | perc      |
| `cp`   | Crack / offbeat     | Clap, rimshot, cross-stick, clave, tamborim  | snare (soft) or perc |
| `bell` | Metallic pattern    | Cowbell, agogô, ride, cáscara, 12/8 bell     | perc or open hat |

## 3. Pattern object

```json
{
  "id": "latin.son.main",
  "name": "Son montuno — main",
  "family": "latin",
  "tags": ["salsa", "clave-3-2"],
  "meter": "4/4",
  "beats": 4,
  "stepsPerBeat": 4,
  "bars": 1,
  "swing": 0.0,
  "bpm": [88, 108],
  "grid": {
    "kick": { "v": "......7.....9..." },
    "cp":   { "v": "9..9..9...9.9...", "a": "A..A..A........." }
  },
  "notes": "cp carries 3-2 son clave; congas stylized on hi."
}
```

* `id` — unique, dot-namespaced: `family.groove.role`. Sections of the same
  groove share the `family.groove` prefix.
* `meter` — display string only ("4/4", "12/8"). The engine uses the numeric
  fields.
* `beats` — pulses per bar the clock counts (4 for 4/4, 4 for 12/8 felt in
  dotted quarters, 7 for 7/8...).
* `stepsPerBeat` — grid subdivision per pulse: 4 = sixteenths, 3 = triplet /
  12/8, 2 = eighths.
* `bars` — how many bars one cycle of the grid spans.
* **Total steps = `beats` × `stepsPerBeat` × `bars`.** Every row string in
  `grid` must be exactly this length. This is the primary validity invariant.
* `swing` — 0..1. 0 = straight; 1 = the off-step of each pair (2nd sixteenth
  of each eighth) delayed to the triplet position. Only meaningful when
  `stepsPerBeat` is even. Triplet grids use 0.
* `bpm` — suggested tempo range `[min, max]`, advisory only.

### 3.1 Grid rows

Each lane has up to four parallel strings, one character per step:

| Row | Meaning       | Characters                                             | If omitted |
|-----|---------------|--------------------------------------------------------|------------|
| `v` | Velocity      | `.` = rest, `1`–`9` = velocity n/9                     | required   |
| `a` | Accent        | `A` = accent, `.` = none                               | no accents |
| `p` | Probability   | `1`–`9` = n/9 chance the step fires, `.` = always      | always     |
| `r` | Ratchet       | `2`–`8` = retrigger the step n times, `.` = single hit | single     |

This is a superset of the `fillRow(v, a)` convention already in `fill.cpp` —
the same two-string parser extends directly. Ratchets cover trap hat rolls and
drag/ruff ornaments without needing a finer global grid; probability covers
humanized ghost notes and generative variation.

Suggested velocity vocabulary (matching how the library is authored):
`9` = full/accented, `7`–`8` = normal, `5`–`6` = light, `3`–`4` = ghost,
`1`–`2` = barely-there.

## 4. Sets — linked patterns

A set is one groove's sections plus instructions for joining them. Roles map
naturally onto Fill's sparse→dense tier idea:

```json
{
  "id": "latin.son",
  "name": "Son montuno",
  "family": "latin",
  "bpm": 96,
  "roles": {
    "sparse": "latin.son.sparse",
    "main":   "latin.son.main",
    "lift":   "latin.son.lift",
    "fill":   ["latin.son.fill"]
  },
  "arrangement": [
    { "role": "sparse", "bars": 4 },
    { "role": "main",   "bars": 8, "fillEvery": 4 },
    { "role": "lift",   "bars": 8, "fillEvery": 4 },
    { "role": "main",   "bars": 8 },
    { "role": "sparse", "bars": 4 }
  ]
}
```

* `vary` — 0..1 **identity strength** (default 0.4): how far a module's variation
  layer may bend this set. 0 = never (timelines — key patterns regulate the
  ensemble and do not vary); 0.1 = nearly incorruptible (motorik, gamelan);
  0.7 = thrives on ghost notes (funk).
* `axis` — `"genre"` (default) or `"region"`: which browser tab the set's family
  appears under in Fill (Genre = style-based banks, Region = place-based banks).

### 4.1 Roles

* `sparse` — intro / breakdown / outro density. Lowest tier.
* `main` — the verse groove. The section a module should default to.
* `lift` — chorus / peak density. Highest tier.
* `fill` — a **list** of one-cycle transition patterns. When several are
  given, the module picks (round-robin, random, or pressure-driven à la Fill).

All roles except `main` are optional. A missing `sparse`/`lift` falls back to
`main`; missing `fill` means sections butt-join with no transition.

### 4.2 Join semantics

* A `fill` pattern **replaces the final cycle** of the outgoing section —
  it never adds time. If the fill's cycle is shorter than the section
  pattern's cycle (e.g. a 1-bar fill against a 2-bar clave pattern), it
  replaces only the final fill-length tail.
* `fillEvery: N` — additionally play a fill on every Nth bar *within* the
  section (the classic "fill every 4 bars"). Omitted = fills only at section
  boundaries.
* Optional per-step override: `"fill": "some.other.id"` inside an arrangement
  step forces a specific fill for that transition.
* Downbeat rule: the first step of the incoming section always lands on the
  next bar (or clave-cycle) boundary, matching Beat's BAR-aligned advance.
* Clave integrity: patterns whose cycle is >1 bar (`bars: 2`) should only be
  joined on cycle boundaries; the module should advance sections on
  `bars`-sized boundaries of the *outgoing* pattern.

### 4.3 Arrangement

`arrangement` is an advisory demo sequence — a module may expose it as a
one-knob "song mode" or ignore it entirely and just expose the roles as
selectable tiers. `bars` counts bars (not cycles) so 2-bar patterns simply
repeat bars/2 times.

## 5. C++ import notes

* Parse with jansson (already linked by Rack): iterate `patterns`, build a
  `LibraryPattern { id, meta, float vel[8][MAX]; bool acc[8][MAX]; ... }`.
* Row parser is ~6 lines and already exists in spirit as `fillRow()`; extend
  with `p`/`r` rows.
* `MAX` steps in v1 content is 16 (nothing currently exceeds one 16-step or
  one 12-step cycle; 2-bar material is declared `bars: 2` with 32-char rows —
  if the engine caps at 16, split on the bar boundary at import and chain).
* Unknown JSON keys must be ignored (forward compatibility); unknown lanes
  skipped with a log line.
* The JSON can be embedded in the plugin binary (e.g. `xxd -i` into a header,
  or committed as `res/patterns/*.json` and loaded with
  `asset::plugin(pluginInstance, ...)`).

## 6. Copyright note

Short rhythmic patterns — a kick/snare/hat grid for a bar or two — are
generally treated as unprotectable building blocks of music rather than
copyrightable works, and the traditional patterns here (claves, bembé bell,
maqsum, samba figures, etc.) are folkloric material that predates any
recording. Everything in this library is authored as a *stylization of a
genre convention*, not a transcription of any specific recording, and pattern
names reference genres and techniques, never songs, artists, or trademarked
drum machines' preset names. That's the conservative posture; note this is
engineering judgment, not legal advice.

## 6.5 v1.1 additions (used by the v2 library)

* `kind` — `"groove"` (default) or `"timeline"`. Timelines are cyclic key
  patterns (claves, bell patterns, talas, colotomies): standalone, usually
  one or two lanes, not referenced by any set. A module can offer them as a
  bell/aux lane to layer under any groove set.
* `source` — free-text citation for verified placements (see
  `global-drum-patterns-report.md` for the research behind them).
* Aksak / odd meters encode with `stepsPerBeat: 1` and `beats` = pulse count
  (e.g. račenica: `beats: 7`, meter `"7/16"`), accents marking the 2/3 group
  starts, grouping documented in `notes`.
* Sets may omit `fill` and even consist of `main` alone (e.g. gamelan
  colotomy, bulerías — traditions without drum fills).
* Microtiming beyond `swing` (samba's pushed 16ths, candombe's contraction,
  second-line laid-back feel) is documented in `notes` pending a signed
  per-step timing row.

## 7. Extension ideas (not in v1)

* `flam` row; per-lane `swing` override; per-step microtiming offsets
  (`t` row, signed).
* Per-set `humanize` defaults for the module's variation layer.
* A `choke` group declaration (ohh choked by chh).
* Song-level key/section labels for display ("Verse", "Chorus") — v1 roles
  are density-based on purpose so they compose with Fill-style pressure.

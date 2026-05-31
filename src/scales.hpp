#pragma once
// =============================================================================
// Canonical scale table — shared by Note, Fugue, and Muse.
//
// One source of truth so a SCALE CV value (1V per scale step) selects the SAME
// scale on every module — you can patch one SCALE sequence to all three and
// they stay in agreement.
//
// Ordering is fixed and append-only: never reorder or insert, only append, or
// you break cross-module CV compatibility and saved patches.
//
// Each scale provides:
//   longName   — full name for tooltips / context menus / configSwitch labels
//   shortName  — compact name for tight on-screen displays (Note's matrix cell)
//   museName   — name shown on Muse (usually == longName; index 0 differs
//                because Muse can only reach 8 of 12 chromatic steps)
//   intervals  — semitone offsets from root, `size` of them (variable length;
//                used by Note + Fugue, which wrap octaves in DSP)
//   size       — number of intervals
//   museSemis  — EXACTLY 8 semitone offsets, the flattened scale Muse reads via
//                its 3-bit pitch index (bit D adds a further +12 octave on top)
//
// museSemis derivation:
//   • 7-note scales → degrees 1..7 then 13 (the 6th up an octave), so the 8th
//     slot is distinct from "slot 0 + octave bit": {d0..d6, scale[5]+12}.
//   • other sizes  → first 8 consecutive degrees, wrapping with +12 per octave:
//     museSemis[i] = intervals[i % size] + 12*(i / size).
//   This keeps the existing pentatonic Muse tables unchanged and gives every
//   scale a clean octave on bit D.
// =============================================================================

namespace sfs {

struct Scale {
	const char* longName;
	const char* shortName;
	const char* museName;
	const float* intervals;
	int size;
	float museSemis[8];
};

// --- Interval tables (semitones from root) -----------------------------------
static const float SCL_CHROMATIC[]   = {0,1,2,3,4,5,6,7,8,9,10,11};
static const float SCL_MAJOR[]       = {0,2,4,5,7,9,11};
static const float SCL_MINOR[]       = {0,2,3,5,7,8,10};
static const float SCL_PENTA_MAJ[]   = {0,2,4,7,9};
static const float SCL_PENTA_MIN[]   = {0,3,5,7,10};
static const float SCL_BLUES[]       = {0,3,5,6,7,10};
static const float SCL_WHOLE[]       = {0,2,4,6,8,10};
// Just-intonation harmonics 1..12 (non-12-TET, log2(n)*12)
static const float SCL_HARMONIC[]    = {
	0.f, 12.f, 19.0196f, 24.f, 27.8631f, 31.0196f,
	33.6883f, 36.f, 38.0392f, 39.8632f, 41.5126f, 43.0196f
};
static const float SCL_DORIAN[]      = {0,2,3,5,7,9,10};
static const float SCL_PHRYGIAN[]    = {0,1,3,5,7,8,10};
static const float SCL_LYDIAN[]      = {0,2,4,6,7,9,11};
static const float SCL_MIXOLYDIAN[]  = {0,2,4,5,7,9,10};
static const float SCL_HARM_MINOR[]  = {0,2,3,5,7,8,11};
static const float SCL_HIJAZ[]       = {0,1,4,5,7,8,10};
static const float SCL_HIRAJOSHI[]   = {0,2,3,7,8};
// Pelog: Surakarta-style approximation in cents/12 (non-12-TET)
static const float SCL_PELOG[]       = {0.f, 1.2f, 2.7f, 5.4f, 7.0f, 8.0f, 10.4f};
// Slendro: 5 equal divisions of the octave (non-12-TET)
static const float SCL_SLENDRO[]     = {0.f, 2.4f, 4.8f, 7.2f, 9.6f};
static const float SCL_MELO_MINOR[]  = {0,2,3,5,7,9,11};
static const float SCL_LOCRIAN[]     = {0,1,3,5,6,8,10};

// --- Canonical list (NEVER reorder; append only) -----------------------------
static const Scale SCALES[] = {
	// longName                     shortName    museName                       intervals       size  museSemis[8]
	{"Chromatic",                   "Chromatic", "Chromatic-ish", SCL_CHROMATIC,  12, {0,1,2,3,4,5,6,7}},
	{"Major",                       "Major",     "Major",         SCL_MAJOR,       7, {0,2,4,5,7,9,11,21}},
	{"Minor",                       "Minor",     "Minor",         SCL_MINOR,       7, {0,2,3,5,7,8,10,20}},
	{"Pentatonic Major",            "Penta+",    "Penta Major",   SCL_PENTA_MAJ,   5, {0,2,4,7,9,12,14,16}},
	{"Pentatonic Minor",            "Penta-",    "Penta Minor",   SCL_PENTA_MIN,   5, {0,3,5,7,10,12,15,17}},
	{"Blues",                       "Blues",     "Blues",         SCL_BLUES,       6, {0,3,5,6,7,10,12,15}},
	{"Whole tone",                  "Whole",     "Whole tone",    SCL_WHOLE,       6, {0,2,4,6,8,10,12,14}},
	{"Harmonic series",             "Harmonic",  "Harmonic",      SCL_HARMONIC,   12, {0.f,12.f,19.0196f,24.f,27.8631f,31.0196f,33.6883f,36.f}},
	{"Dorian",                      "Dorian",    "Dorian",        SCL_DORIAN,      7, {0,2,3,5,7,9,10,21}},
	{"Phrygian",                    "Phrygian",  "Phrygian",      SCL_PHRYGIAN,    7, {0,1,3,5,7,8,10,20}},
	{"Lydian",                      "Lydian",    "Lydian",        SCL_LYDIAN,      7, {0,2,4,6,7,9,11,21}},
	{"Mixolydian",                  "Mixolyd",   "Mixolydian",    SCL_MIXOLYDIAN,  7, {0,2,4,5,7,9,10,21}},
	{"Harmonic Minor",              "HarmMin",   "Harm Minor",    SCL_HARM_MINOR,  7, {0,2,3,5,7,8,11,20}},
	{"Hijaz (Arabic)",              "Hijaz",     "Hijaz",         SCL_HIJAZ,       7, {0,1,4,5,7,8,10,20}},
	{"Hirajoshi (Japanese)",        "Hirajoshi", "Hirajoshi",     SCL_HIRAJOSHI,   5, {0,2,3,7,8,12,14,15}},
	{"Pelog (Gamelan, 7-tone)",     "Pelog",     "Pelog",         SCL_PELOG,       7, {0.f,1.2f,2.7f,5.4f,7.0f,8.0f,10.4f,20.0f}},
	{"Slendro (Gamelan, 5-equal)",  "Slendro",   "Slendro",       SCL_SLENDRO,     5, {0.f,2.4f,4.8f,7.2f,9.6f,12.f,14.4f,16.8f}},
	{"Melodic Minor",               "MeloMin",   "Melodic Minor", SCL_MELO_MINOR,  7, {0,2,3,5,7,9,11,21}},
	{"Locrian",                     "Locrian",   "Locrian",       SCL_LOCRIAN,     7, {0,1,3,5,6,8,10,20}},
};

static const int NUM_SCALES = (int)(sizeof(SCALES) / sizeof(SCALES[0]));

} // namespace sfs

#pragma once
// =============================================================================
// Push-style isomorphic pad grid — shared by Play and Record.
//
// 12 wide × 8 tall. row 0 = bottom, col 0 = left. Three layouts:
//   0 Chromatic 4ths — right = +1 semitone, up a row = +5 (a perfect fourth).
//   1 In-Key         — only scale notes; right = next scale degree, up = a fourth.
//   2 Chromatic grid — 12 pitch classes per row (C..B) from C0, up a row = octave.
// =============================================================================
#include "scales.hpp"
#include <cmath>

static const int GRID_COLS = 12, GRID_ROWS = 8;
static const int GRID_UI_CHAN = 100;   // reserved voice channel for mouse audition

static inline int gridClampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// row/col → MIDI note, or -1 if out of range.
static inline int gridNoteAt(int layout, int root, int scaleIdx, int base, int row, int col) {
	if (layout == 0) {                                   // chromatic 4ths
		int n = base + row * 5 + col;
		return (n >= 0 && n < 128) ? n : -1;
	}
	if (layout == 2) {                                   // chromatic grid, C0 origin
		if (col >= 12) return -1;
		int n = 12 + row * 12 + col;                     // C0 = MIDI 12
		return (n >= 0 && n < 128) ? n : -1;
	}
	const sfs::Scale& sc = sfs::SCALES[gridClampi(scaleIdx, 0, sfs::NUM_SCALES - 1)];
	int S = sc.size < 1 ? 1 : sc.size;
	int rowStep = (int)std::lround(S * 5.0 / 12.0); if (rowStep < 1) rowStep = 1;  // ≈ a fourth
	int deg = row * rowStep + col;
	int origin = (base - ((base % 12) + 12) % 12) + root;         // root, in base's octave
	int n = origin + (deg / S) * 12 + (int)std::lround(sc.intervals[deg % S]);
	return (n >= 0 && n < 128) ? n : -1;
}
// black key (accidental) pitch class — for the piano-roll shading in the chromatic grid
static inline bool gridIsAccidental(int n) {
	int pc = ((n % 12) + 12) % 12;
	return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
static inline bool gridNoteInScale(int n, int root, int scaleIdx) {
	const sfs::Scale& sc = sfs::SCALES[gridClampi(scaleIdx, 0, sfs::NUM_SCALES - 1)];
	int pc = ((n - root) % 12 + 12) % 12;
	for (int i = 0; i < sc.size; i++) if ((((int)std::lround(sc.intervals[i])) % 12 + 12) % 12 == pc) return true;
	return false;
}

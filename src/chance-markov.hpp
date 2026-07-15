#pragma once
#include "scales.hpp"
#include <cmath>

// Hand-authored first-order Markov transition tables for Chance. Row = current
// scale degree, col = candidate next degree; values are RELATIVE weights encoding
// tonal tendency only (step size / register / gravity are applied by the walk).
namespace chancemk {

// Diatonic tendency — shared by all 7-note scales, keyed by scale-degree position:
// chord-tone gravity on 1/3/5 (cols 0/2/4), leading-tone→tonic, subdominant→3rd/5th,
// dominant→tonic, plus stepwise motion everywhere.
static const float DIATONIC7[49] = {
	//to 1   2   3   4   5   6   7
	2,   6,  5,  2,  6,  4,  3,   // from 1 (tonic) — free
	7,   1,  6,  2,  5,  1,  1,   // from 2 (supertonic) → 1 / 3
	4,   6,  1,  6,  4,  2,  1,   // from 3 (mediant, stable)
	2,   2,  7,  1,  7,  2,  1,   // from 4 (subdominant) → 3 or 5
	9,   2,  4,  3,  1,  5,  2,   // from 5 (dominant) → tonic (deceptive → 6)
	5,   2,  2,  2,  7,  1,  4,   // from 6 (submediant) → 5 / 1
	10,  2,  3,  1,  2,  3,  1,   // from 7 (leading) → tonic
};
static const float PENTA5[25] = {
	1,   6,  3,  4,  5,
	6,   1,  6,  3,  3,
	4,   5,  1,  6,  3,
	5,   3,  5,  1,  6,
	6,   3,  3,  5,  1,
};
static const float BLUES6[36] = {
	1,   5,  3,  1,  5,  3,
	5,   1,  5,  2,  2,  2,
	3,   4,  1,  5,  4,  2,
	1,   2,  6,  1,  6,  1,   // ♭5 passing → 4 or 5
	5,   2,  3,  3,  1,  5,
	6,   2,  2,  1,  5,  1,   // ♭7 → root or 5
};

// scale index (parallel to sfs::SCALES) → hand table, or nullptr = formula default.
static inline const float* tableFor(int scaleIdx, int size) {
	if (scaleIdx == 5) return BLUES6;    // Blues
	if (size == 7) return DIATONIC7;     // Major, modes, harm/melo minor, Hijaz, Pelog
	if (size == 5) return PENTA5;        // pentatonics, Hirajoshi, Slendro
	return nullptr;                       // Whole / Chromatic / Harmonic → default
}

// Weight of a transition from degree `from` to degree `to` in a `size`-note scale.
static inline float weight(int scaleIdx, int from, int to, int size) {
	const float* T = tableFor(scaleIdx, size);
	if (T) return T[from * size + to];
	// default (symmetric/non-tonal scales): circular stepwise + tonic/~5th pull.
	int dd = to - from; if (dd < 0) dd = -dd; if (dd > size - dd) dd = size - dd;
	float step = 1.f / (1.f + dd);
	int fifth = (int)std::lround(size * 4.f / 7.f);
	float pull = (to == 0) ? 1.6f : (to == fifth ? 1.2f : 1.f);
	return step * pull + 0.15f;
}

} // namespace chancemk

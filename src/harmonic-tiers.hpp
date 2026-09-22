#pragma once
// =============================================================================
// Harmonic deviation, shared. These tables and helpers were Fugue's, and Round
// wanders by the same music theory: chord tones before extensions before
// colour, and a consonance score for choosing between candidates. Moved here
// verbatim so the two modules cannot drift apart; nothing in Fugue changed.
// =============================================================================
#include <cstdint>

// ─── Harmonic Deviation Tier Tables ──────────────────────────────────────────

// Diatonic tiers: offsets in scale degrees from the base note
struct DeviationTier {
	int offsets[6];
	int count;
};

static const DeviationTier DIATONIC_TIERS[] = {
	{{0},          1},  // Tier 0: Unison
	{{2, 4},       2},  // Tier 1: 3rd, 5th (chord tones)
	{{6, 1, 3},    3},  // Tier 2: 7th, 9th, 11th (extensions)
	{{5},          1},  // Tier 3: 6th (remaining)
};
static const int NUM_DIATONIC_TIERS = 4;

static const DeviationTier PENTATONIC_TIERS[] = {
	{{0},          1},  // Tier 0: Unison
	{{1, 2},       2},  // Tier 1: 2nd, 3rd degree
	{{3, 4},       2},  // Tier 2: 4th, 5th degree
};
static const int NUM_PENTATONIC_TIERS = 3;

// Chromatic tiers: intervals in semitones
static const int CHROM_TIER_0[] = {0};              // Unison
static const int CHROM_TIER_1[] = {7, 5};           // Perfect consonance (P5, P4)
static const int CHROM_TIER_2[] = {4, 3, 9, 8};     // Imperfect consonance (M3, m3, M6, m6)
static const int CHROM_TIER_3[] = {2, 10};           // Mild dissonance (M2, m7)
static const int CHROM_TIER_4[] = {1, 11, 6};        // Sharp dissonance (m2, M7, tritone)

struct ChromTierInfo {
	const int* intervals;
	int count;
};

static const ChromTierInfo CHROM_TIERS[] = {
	{CHROM_TIER_0, 1},
	{CHROM_TIER_1, 2},
	{CHROM_TIER_2, 4},
	{CHROM_TIER_3, 2},
	{CHROM_TIER_4, 3},
};

// ─── RNG Helper ──────────────────────────────────────────────────────────────

static inline uint32_t xorshift32(uint32_t& state) {
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

static inline float randFloat(uint32_t& state) {
	return (float)(xorshift32(state) & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

// ─── Interval Consonance Scoring ─────────────────────────────────────────────

static float intervalConsonance(int semitones) {
	semitones = ((semitones % 12) + 12) % 12;
	static const float scores[] = {
		1.0f,   // 0: Unison
		0.1f,   // 1: m2
		0.3f,   // 2: M2
		0.65f,  // 3: m3
		0.7f,   // 4: M3
		0.85f,  // 5: P4
		0.15f,  // 6: tritone
		0.9f,   // 7: P5
		0.55f,  // 8: m6
		0.6f,   // 9: M6
		0.25f,  // 10: m7
		0.1f,   // 11: M7
	};
	return scores[semitones];
}


// =============================================================================
// Diapason engine: a three-division pipe organ, free of Rack.
//
// The engine counts PIPES (one per rank per key), not voices. A pipe speaks
// while its key is held on its division by any route (its own keyboard or a
// coupler) and its stop is drawn. Each pipe is three harmonic groups read from
// band-limited tables, each with its own onset, plus a noise resonator for the
// chiff and the flutes' breath; once its groups have settled it reads one
// summed table. Each division has a reservoir whose pressure sags under the
// air its speaking pipes draw. The Swell is enclosed in a box.
//
// Measured and rendered by tools/diapason-proto.cpp. Design:
// docs/diapason-design.md.
// =============================================================================
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace diapason {

enum Div { GREAT, SWELL, PEDAL, NDIV };
enum Family { PRINCIPAL, STOPPED, CHIMNEY, STRING, TRUMPET, OBOE, POSAUNE, NFAM };
enum Stop {
	GT_BOURDON16, GT_PRINCIPAL8, GT_ROHR8, GT_OCTAVE4, GT_FIFTEENTH2, GT_MIXTURE, GT_TRUMPET8,
	SW_GEDACKT8, SW_GAMBA8, SW_CELESTE8, SW_PRINCIPAL4, SW_OBOE8, SW_TROMPETTE8,
	PD_BOURDON32, PD_SUBBASS16, PD_OCTAVE8, PD_POSAUNE16,
	NSTOP
};
enum Coupler { SW_GT, SW_PD, GT_PD, NCOUP };

static const int NRANK = NSTOP + 3;     // the mixture is four ranks
static const int MAXP = 512;            // pipes sounding at once
static const int BLOCK = 16;            // samples per control step
static const int TN = 1024;             // table length (one guard sample after it)
static const int NMIP = 26;             // half-octave tables from 8 Hz
static const int MAN_LO = 36, MAN_HI = 96;   // manual compass C2-C7, 8' terms
static const int PED_LO = 36, PED_HI = 67;   // pedal compass C2-G4

struct StopInfo {
	const char* name;     // as on the stop knob
	const char* foot;     // footage, as printed under it
	int div;
	int fam;
	float ratio;          // pitch against the key at 8'
	float levelDb;
	float cents;          // the céleste's sharpness
	int minKey;           // the céleste starts at tenor C
	float air;            // airflow weight against a Principal 8'
	float tilt;           // dB per octave of harmonic number against its family
	float fmul;           // a reed's formant, as a multiple of its family's
};

static const StopInfo STOPS[NSTOP] = {
	{"BOURDON",   "16'", GREAT, STOPPED,   0.5f,  -4.f, 0.f, 0, 1.1f, 0.f, 1.f},
	{"PRINCIPAL", "8'",  GREAT, PRINCIPAL, 1.f,    0.f, 0.f, 0, 1.0f, 0.f, 1.f},
	{"ROHRFLÖTE", "8'",  GREAT, CHIMNEY,   1.f,   -6.f, 0.f, 0, 0.7f, 0.f, 1.f},
	{"OCTAVE",    "4'",  GREAT, PRINCIPAL, 2.f,   -3.f, 0.f, 0, 0.6f, 0.5f, 1.f},
	{"FIFTEENTH", "2'",  GREAT, PRINCIPAL, 4.f,   -5.f, 0.f, 0, 0.4f, 0.5f, 1.f},
	{"MIXTURE",   "IV",  GREAT, PRINCIPAL, 0.f,  -10.f, 0.f, 0, 0.25f, 0.f, 1.f},
	{"TRUMPET",   "8'",  GREAT, TRUMPET,   1.f,    1.f, 0.f, 0, 1.5f, 0.f, 0.95f},
	{"GEDACKT",   "8'",  SWELL, STOPPED,   1.f,   -7.f, 0.f, 0, 0.6f, -1.5f, 1.f},
	{"GAMBA",     "8'",  SWELL, STRING,    1.f,   -8.f, 0.f, 0, 0.5f, 0.f, 1.f},
	{"CÉLESTE",   "8'",  SWELL, STRING,    1.f,   -8.f, 5.f, 48, 0.5f, -0.7f, 1.f},
	{"PRINCIPAL", "4'",  SWELL, PRINCIPAL, 2.f,   -5.f, 0.f, 0, 0.6f, -1.f, 1.f},
	{"OBOE",      "8'",  SWELL, OBOE,      1.f,   -6.f, 0.f, 0, 1.0f, 0.f, 1.f},
	{"TROMPETTE", "8'",  SWELL, TRUMPET,   1.f,    0.f, 0.f, 0, 1.5f, 1.f, 1.35f},
	{"BOURDON",   "32'", PEDAL, STOPPED,   0.25f, -2.f, 0.f, 0, 2.5f, -2.f, 1.f},
	{"SUBBASS",   "16'", PEDAL, STOPPED,   0.5f,  -1.f, 0.f, 0, 2.0f, -1.f, 1.f},
	{"OCTAVE",    "8'",  PEDAL, PRINCIPAL, 1.f,   -2.f, 0.f, 0, 1.2f, -1.5f, 1.f},
	{"POSAUNE",   "16'", PEDAL, POSAUNE,   0.5f,   2.f, 0.f, 0, 2.5f, 0.f, 1.f},
};
static const char* const DIV_NAME[NDIV] = {"GREAT", "SWELL", "PEDAL"};
static const char* const COUPLER_NAME[NCOUP] = {"SWELL TO GREAT", "SWELL TO PEDAL", "GREAT TO PEDAL"};

// The mixture's four ranks for a key: 19.22.26.29 at the bottom, breaking back
// at every C from C3 until it is 8' 5 1/3' 4' 2 2/3' at the top.
inline float mixRatio(int key, int i) {
	static const float R[5][4] = {
		{6.f, 8.f, 12.f, 16.f}, {4.f, 6.f, 8.f, 12.f}, {3.f, 4.f, 6.f, 8.f},
		{2.f, 3.f, 4.f, 6.f}, {1.f, 1.5f, 2.f, 3.f}};
	int b = key < 48 ? 0 : key < 60 ? 1 : key < 72 ? 2 : key < 84 ? 3 : 4;
	return R[b][i];
}

// How each family speaks. Times are in units of the pipe's speech time T.
struct Speech {
	float d[3], r[3], o[3];   // per group: onset delay, rise, overshoot
	float tScale;             // speech time against a flue pipe's
	float noiseH;             // harmonic the noise resonator sits on (0: none)
	float chiff, breath;      // noise levels against the steady tone
	float chiffDecay;         // in T
	int maxPartials;
	bool reed;
};
static const Speech SPEECH[NFAM] = {
	// principal: the octave first, the fundamental after
	{{0.25f, 0.f, 0.1f}, {1.f, 0.6f, 0.8f}, {1.f, 1.3f, 1.1f}, 1.f, 2.f, 0.10f, 0.014f, 0.3f, 24, false},
	// stopped flute: a chiff of upper harmonics and noise
	{{0.f, 0.f, 0.f}, {1.f, 0.7f, 0.5f}, {1.f, 1.f, 2.0f}, 1.f, 3.f, 0.16f, 0.02f, 0.4f, 15, false},
	// chimney flute: the same, more chiff
	{{0.f, 0.f, 0.f}, {1.f, 0.7f, 0.5f}, {1.f, 1.1f, 2.0f}, 1.f, 3.f, 0.22f, 0.02f, 0.4f, 15, false},
	// string: slow, the fundamental last
	{{0.3f, 0.1f, 0.f}, {1.5f, 1.2f, 1.2f}, {1.f, 1.1f, 1.25f}, 1.f, 4.f, 0.06f, 0.012f, 0.5f, 40, false},
	// reeds: fast, the upper harmonics first
	{{0.1f, 0.05f, 0.f}, {0.6f, 0.4f, 0.3f}, {1.f, 1.1f, 1.4f}, 0.5f, 0.f, 0.f, 0.f, 0.f, 48, true},
	{{0.1f, 0.05f, 0.f}, {0.6f, 0.4f, 0.3f}, {1.f, 1.2f, 1.3f}, 0.5f, 0.f, 0.f, 0.f, 0.f, 48, true},
	{{0.1f, 0.05f, 0.f}, {0.7f, 0.5f, 0.4f}, {1.f, 1.1f, 1.4f}, 0.6f, 0.f, 0.f, 0.f, 0.f, 48, true},
};

inline float formantDb(float f, float fc, float wOct, float g) {
	float x = std::log2(f / fc) / wOct;
	return g * std::exp(-0.5f * x * x);
}

// The level of harmonic n (frequency fn) of a family, in dB. Estimates of the
// general shape of published spectra, not measurements of one organ.
inline float harmonicDb(int fam, int n, float fn, float tilt = 0.f, float fmul = 1.f) {
	float l2 = std::log2((float)n), db;
	switch (fam) {
		case PRINCIPAL: db = -9.f * l2 + (n == 2 ? 3.f : 0.f); break;
		case STOPPED:   db = (n & 1) ? -10.f * l2 - (n > 1 ? 2.f : 0.f) : -35.f - 6.f * l2; break;
		case CHIMNEY:   db = (n & 1) ? -10.f * l2 - (n > 1 ? 2.f : 0.f) + (n == 5 ? 12.f : 0.f)
		                             : (n == 2 ? -24.f : -36.f - 6.f * l2); break;
		case STRING:    db = (n == 1 ? -3.f : -4.f * l2) - (n > 6 ? 1.2f * (n - 6) : 0.f); break;
		case TRUMPET:   db = -3.f * l2 + formantDb(fn, 1600.f * fmul, 1.6f, 8.f); break;
		case OBOE:      db = (n == 1 ? -8.f : -5.f * l2) + formantDb(fn, 1400.f * fmul, 0.9f, 14.f); break;
		default:        db = -3.f * l2 + formantDb(fn, 700.f * fmul, 1.5f, 8.f); break;   // posaune
	}
	db += tilt * l2;
	if (fn > 6000.f) db -= 12.f * std::log2(fn / 6000.f);     // treble voicing
	if (SPEECH[fam].reed && fn > 5000.f) db -= 10.f * std::log2(fn / 5000.f);
	return db;
}

inline uint32_t hash32(uint32_t x) {
	x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
	return x;
}
inline float hashUnit(uint32_t x) { return (hash32(x) >> 8) * (1.f / 16777216.f); }

inline float keyHz(int key) { return 440.f * std::pow(2.f, (key - 69) / 12.f); }

struct Rank {
	int stop, div, fam, mix = -1;        // mix: which mixture rank (or -1)
	float levelLin = 1.f, cents = 0.f, air = 1.f;
	int minKey = 0, maxPartials = 24;
	int mip0 = 0, mipN = 0;              // the tables this rank needs
	std::vector<float> tab;              // [mip][group 0..2, sum][TN + 1]
	float ratio(int key) const { return mix >= 0 ? mixRatio(key, mix) : STOPS[stop].ratio; }
	const float* table(int mip, int g) const {
		mip = std::max(mip0, std::min(mip0 + mipN - 1, mip));
		return &tab[((size_t)(mip - mip0) * 4 + g) * (TN + 1)];
	}
};

inline int mipFor(float f) {
	int m = (int)std::floor(2.f * std::log2(std::max(8.f, f) / 8.f));
	return std::max(0, std::min(NMIP - 1, m));
}

struct Pipe {
	int rank = -1, key = 0, div = 0;
	bool on = false, settled = false, reed = false;
	float t = 0.f, tRel = 0.f;           // seconds since speech began, since release
	float T = 0.05f, relT = 0.02f;       // speech and release times
	float carry[3] = {};                 // the level a re-speaking pipe still had
	float gRel[3] = {};                  // group levels at release
	float g[3] = {};                     // group speech envelopes at the end of the last block
	float ga[3] = {};                    // the same with the voicing and the wavering, as played
	float level = 1.f;
	float panL = 0.7f, panR = 0.7f;
	int side = 0;                        // 0 C side, 1 C# side
	float ph[3] = {};                    // each group's phase (they part while a pipe speaks)
	float baseInc = 0.f;
	float delay = 0.f, relDelay = 0.f;   // action and distance: when it starts, and stops, after the key
	float uf[2] = {};                    // the roughness of the upper groups while it speaks
	float scoop = 1.f;                   // how far this pipe starts off its pitch (0..1)
	const float* tb[4] = {};
	// noise: a resonator on one harmonic
	float a1 = 0.f, a2 = 0.f, b0 = 0.f, y1 = 0.f, y2 = 0.f, nLvl = 0.f;
	uint32_t rng = 1;
	float env = 0.f;                     // mean group level, for the drawing and the wind
	uint32_t stamp = 0;
	// what makes one pipe not quite like its neighbour: a fixed voicing of each
	// group, and slow wavering of each group's level and of the pitch
	float vo[3] = {1.f, 1.f, 1.f};
	float u[3] = {}, uj = 0.f;
	uint32_t wr = 1;                     // its own generator for the wavering
};

struct Engine {
	float sr = 48000.f;
	Rank ranks[NRANK];
	int stopRank0[NSTOP], stopRanks[NSTOP];
	int16_t map[NRANK][128];
	Pipe pipes[MAXP];
	int active[MAXP];
	int nActive = 0;
	int freeList[MAXP];
	int nFree = 0;
	uint32_t blockN = 0;

	// controls (set before each block)
	bool stop[NSTOP] = {};
	bool coupler[NCOUP] = {};
	bool tremulant = false;
	float tremRate = 5.5f, tremDepth = 0.04f;   // Hz, fraction of pressure
	float wind = 0.5f;                          // 0 a perfect regulator .. 1 a generous sag
	float swell = 1.f;                          // swell pedal, 0 closed .. 1 open
	float width = 1.f;                          // how far the case spreads across the stereo field
	bool sidesOut = false;                      // LEFT = C side, RIGHT = C# side
	float master = 1.f;
	float irreg = 1.f;                          // 0 steady pipes, 1 natural, 2 an old organ
	// keyboards: which keys are held on each (bits)
	uint64_t kb[NDIV][2] = {};

	// state the drawing reads
	float keyLevel[NDIV][128] = {};
	float pressure[NDIV] = {1.f, 1.f, 1.f};
	int pipesSounding = 0;

	// wind
	float p[NDIV] = {1.f, 1.f, 1.f}, pv[NDIV] = {};
	float tremPh = 0.f;
	float wj[NDIV] = {};                        // turbulence in each division's wind
	uint32_t wjr = 0x2545F491u;
	float gainPrev[NDIV] = {1.f, 1.f, 1.f};
	// the swell box: a gain and a two-pole lowpass per side
	float boxG = 1.f, boxGPrev = 1.f, boxF = 20000.f;
	float boxS[2][2] = {};

	float outBuf[2][BLOCK] = {};
	int outPos = BLOCK;

	Engine() {
		int r = 0;
		for (int s = 0; s < NSTOP; s++) {
			int n = s == GT_MIXTURE ? 4 : 1;
			stopRank0[s] = r; stopRanks[s] = n;
			for (int i = 0; i < n; i++, r++) {
				Rank& R = ranks[r];
				R.stop = s; R.div = STOPS[s].div; R.fam = STOPS[s].fam;
				R.mix = s == GT_MIXTURE ? i : -1;
				R.levelLin = std::pow(10.f, STOPS[s].levelDb / 20.f);
				R.cents = STOPS[s].cents; R.minKey = STOPS[s].minKey; R.air = STOPS[s].air;
				R.maxPartials = s == GT_MIXTURE ? 6 : SPEECH[R.fam].maxPartials;
			}
		}
		std::memset(map, 0xff, sizeof(map));
		for (int i = 0; i < MAXP; i++) freeList[i] = MAXP - 1 - i;
		nFree = MAXP;
		setSampleRate(48000.f);
	}

	static int compassLo(int div) { return div == PEDAL ? PED_LO : MAN_LO; }
	static int compassHi(int div) { return div == PEDAL ? PED_HI : MAN_HI; }
	// a key folded by octaves into a division's compass
	static int fold(int div, int key) {
		int lo = compassLo(div), hi = compassHi(div);
		while (key < lo) key += 12;
		while (key > hi) key -= 12;
		return key;
	}

	// ---------------------------------------------------------------- tables
	void setSampleRate(float s) {
		sr = s;
		static float sinTab[TN];
		static bool sinInit = false;
		if (!sinInit) { for (int i = 0; i < TN; i++) sinTab[i] = std::sin(2.f * (float)M_PI * i / TN); sinInit = true; }
		std::vector<float> acc((size_t)4 * TN);
		for (int r = 0; r < NRANK; r++) {
			Rank& R = ranks[r];
			int lo = compassLo(R.div), hi = compassHi(R.div);
			float fmin = 1e9f, fmax = 0.f;
			for (int k = lo; k <= hi; k++) {
				float f = keyHz(k) * R.ratio(k);
				fmin = std::min(fmin, f); fmax = std::max(fmax, f);
			}
			R.mip0 = mipFor(fmin);
			R.mipN = mipFor(fmax) - R.mip0 + 1;
			R.tab.assign((size_t)R.mipN * 4 * (TN + 1), 0.f);
			for (int m = 0; m < R.mipN; m++) {
				int mip = R.mip0 + m;
				float fBot = 8.f * std::pow(2.f, mip * 0.5f);
				float fMid = fBot * std::pow(2.f, 0.25f), fTop = fBot * std::sqrt(2.f);
				std::fill(acc.begin(), acc.end(), 0.f);
				for (int n = 1; n <= R.maxPartials; n++) {
					if (n * fTop > 0.45f * sr) break;
					float db = harmonicDb(R.fam, n, n * fMid, STOPS[R.stop].tilt, STOPS[R.stop].fmul);
					if (db < -70.f) continue;
					float a = std::pow(10.f, db / 20.f);
					int g = n == 1 ? 0 : n <= 3 ? 1 : 2;
					uint32_t ph = hash32((uint32_t)(r * 977 + n * 31)) & (TN - 1);
					float* dst = &acc[(size_t)g * TN];
					for (int i = 0; i < TN; i++) dst[i] += a * sinTab[(ph + (uint32_t)n * i) & (TN - 1)];
				}
				for (int i = 0; i < TN; i++) acc[(size_t)3 * TN + i] = acc[i] + acc[TN + i] + acc[2 * TN + i];
				double ss = 0.0;
				for (int i = 0; i < TN; i++) ss += (double)acc[3 * TN + i] * acc[3 * TN + i];
				float norm = 1.f / (float)std::sqrt(std::max(1e-12, ss / TN));
				for (int g = 0; g < 4; g++) {
					float* d = &R.tab[((size_t)m * 4 + g) * (TN + 1)];
					for (int i = 0; i < TN; i++) d[i] = acc[(size_t)g * TN + i] * norm;
					d[TN] = d[0];
				}
			}
		}
		// pipes in flight point into the old tables
		for (int i = 0; i < nActive; i++) bindTables(pipes[active[i]]);
	}

	void bindTables(Pipe& P) {
		const Rank& R = ranks[P.rank];
		int mip = mipFor(P.baseInc * sr);
		for (int g = 0; g < 4; g++) P.tb[g] = R.table(mip, g);
	}

	// ---------------------------------------------------------------- pipes
	float placeX(int div, int key, int& side) const {
		side = (key - 36) & 1;     // C, D, E, F#, G#, A# on the C side
		float lo = (float)compassLo(div), hi = (float)compassHi(div);
		float rel = (key - lo) / (hi - lo);   // 0 the largest pipe
		float a = div == GREAT ? 0.12f + 0.43f * (1.f - rel)
		        : div == SWELL ? 0.60f + 0.14f * (1.f - rel)
		        :                0.82f + 0.18f * (1.f - rel);
		return side == 0 ? -a : a;
	}

	int newPipe(int r, int key) {
		if (nFree == 0) {
			// take the quietest pipe that is no longer held
			int best = -1; float bl = 1e9f;
			for (int i = 0; i < nActive; i++) {
				Pipe& P = pipes[active[i]];
				if (!P.on && P.env < bl) { bl = P.env; best = i; }
			}
			if (best < 0) return -1;
			freePipe(best);
		}
		int id = freeList[--nFree];
		Pipe& P = pipes[id];
		const Rank& R = ranks[r];
		const Speech& S = SPEECH[R.fam];
		P = Pipe();
		P.rank = r; P.key = key; P.div = R.div; P.reed = S.reed;
		float cents = R.cents + (hashUnit((uint32_t)(r * 131 + key * 7 + 1)) * 2.f - 1.f) * (1.f + irreg);
		float f = keyHz(key) * R.ratio(key) * std::pow(2.f, cents / 1200.f);
		P.baseInc = f / sr;
		P.T = std::min(0.25f, std::max(0.015f, 0.012f + 10.f / f)) * S.tScale;
		// A flue pipe whose wind is cut rings down as a resonator, tau = Q / (pi f),
		// with Q about 40. A reed stops when its tongue does: only the
		// resonator rings on, for a few cycles.
		P.relT = S.reed ? 0.006f + 3.f / f : 0.008f + 12.f / f;
		P.level = R.levelLin;
		float x = placeX(R.div, key, P.side);
		float th = (std::max(-1.f, std::min(1.f, x * width)) + 1.f) * 0.25f * (float)M_PI;
		P.panL = std::cos(th); P.panR = std::sin(th);
		float ph0 = hashUnit((uint32_t)(r * 53 + key));
		for (int g = 0; g < 3; g++) P.ph[g] = ph0;
		// The pipes of a chord do not start together. The Swell stands behind
		// the Great and the Pedal at the far ends, each rank has its own chest,
		// and the action opens each pallet a little differently.
		static const float DIV_MS[NDIV] = {0.f, 3.f, 5.f};
		float sc = std::min(1.f, irreg) + 0.5f * std::max(0.f, irreg - 1.f);
		P.delay = sc * 0.001f * (DIV_MS[R.div] + 3.f * hashUnit((uint32_t)(r * 17 + 3))
		                         + 4.f * hashUnit((uint32_t)(r * 29 + key * 13 + 11)));
		P.scoop = 0.3f + 0.7f * hashUnit((uint32_t)(r * 59 + key * 23 + 13));
		P.relDelay = sc * 0.001f * (0.5f * DIV_MS[R.div] + 3.f * hashUnit((uint32_t)(r * 41 + key * 19 + 7)));
		// and each speaks a little slower or faster than the next
		P.T *= (1.f + 0.15f * irreg) * (1.f + 0.15f * irreg * (2.f * hashUnit((uint32_t)(r * 71 + key * 5 + 9)) - 1.f));
		P.rng = hash32((uint32_t)(r * 7919 + key * 104729 + 17)) | 1u;
		P.wr = hash32((uint32_t)(r * 3571 + key * 613 + 5)) | 1u;
		// each pipe voiced a little differently: up to about 1 dB on the
		// fundamental and 2 dB on the upper harmonics, at Natural
		for (int g = 0; g < 3; g++) {
			float h = hashUnit((uint32_t)(r * 389 + key * 97 + g * 7 + 3)) * 2.f - 1.f;
			P.vo[g] = std::pow(10.f, h * irreg * (g == 0 ? 1.f : 2.f) / 20.f);
		}
		if (S.noiseH > 0.f) {
			float fr = std::min(0.4f * sr, f * S.noiseH);
			float bw = std::max(30.f, fr / 8.f);
			float rr = std::exp(-(float)M_PI * bw / sr), w = 2.f * (float)M_PI * fr / sr;
			P.a1 = 2.f * rr * std::cos(w); P.a2 = rr * rr;
			// output variance of the resonator for unit white input, so b0 makes it unity
			float r2 = rr * rr, c2 = std::cos(2.f * w);
			float var = (1.f + r2) / ((1.f - r2) * ((1.f + r2) * (1.f + r2) - 4.f * r2 * (0.5f + 0.5f * c2)));
			P.b0 = 1.f / std::sqrt(std::max(1e-9f, var));
		}
		bindTables(P);
		map[r][key] = (int16_t)id;
		active[nActive++] = id;
		return id;
	}
	void freePipe(int ai) {
		int id = active[ai];
		Pipe& P = pipes[id];
		map[P.rank][P.key] = -1;
		P.rank = -1;
		active[ai] = active[--nActive];
		freeList[nFree++] = id;
	}

	// a group's level while speaking, t in seconds
	static float speakEnv(const Speech& S, int g, float t, float T) {
		float x = (t - S.d[g] * T) / (S.r[g] * T);
		if (x <= 0.f) return 0.f;
		float o = S.o[g];
		if (x < 1.f) return o * (0.5f - 0.5f * std::cos((float)M_PI * x));
		return 1.f + (o - 1.f) * std::exp(-2.5f * (x - 1.f));
	}

	// Which keys each division hears, through its own keyboard and the couplers.
	void divisionKeys(uint64_t out[NDIV][2]) const {
		for (int w = 0; w < 2; w++) {
			out[GREAT][w] = kb[GREAT][w] | (coupler[GT_PD] ? kb[PEDAL][w] : 0);
			out[SWELL][w] = kb[SWELL][w] | (coupler[SW_GT] ? kb[GREAT][w] : 0) | (coupler[SW_PD] ? kb[PEDAL][w] : 0);
			out[PEDAL][w] = kb[PEDAL][w];
		}
	}

	void control() {
		blockN++;
		uint64_t dk[NDIV][2];
		divisionKeys(dk);
		for (int s = 0; s < NSTOP; s++) {
			if (!stop[s]) continue;
			int d = STOPS[s].div;
			for (int w = 0; w < 2; w++) {
				uint64_t bits = dk[d][w];
				while (bits) {
					int b = __builtin_ctzll(bits);
					bits &= bits - 1;
					int key = w * 64 + b;
					for (int i = 0; i < stopRanks[s]; i++) {
						int r = stopRank0[s] + i;
						if (key < ranks[r].minKey) continue;
						int id = map[r][key];
						if (id < 0) id = newPipe(r, key);
						if (id < 0) continue;
						pipes[id].stamp = blockN;
					}
				}
			}
		}
		for (int i = 0; i < nActive; i++) {
			Pipe& P = pipes[active[i]];
			bool want = P.stamp == blockN;
			if (want && !P.on) {
				// speaks (again): from silence, or over what a release left
				for (int g = 0; g < 3; g++) P.carry[g] = P.g[g];
				P.on = true; P.settled = false; P.t = -P.delay;
			} else if (!want && P.on) {
				for (int g = 0; g < 3; g++) P.gRel[g] = P.g[g];
				P.on = false; P.settled = false; P.tRel = -P.relDelay;
			}
		}

		// wind: each reservoir sags under what its pipes draw, and bounces back
		float demand[NDIV] = {};
		for (int i = 0; i < nActive; i++) {
			const Pipe& P = pipes[active[i]];
			demand[P.div] += ranks[P.rank].air * P.env;
		}
		float dt = BLOCK / sr;
		const float W = 2.f * (float)M_PI * 2.5f, Z = 0.35f;
		for (int d = 0; d < NDIV; d++) {
			float target = 1.f / (1.f + 0.0016f * wind * demand[d]);
			pv[d] += (W * W * (target - p[d]) - 2.f * Z * W * pv[d]) * dt;
			p[d] += pv[d] * dt;
		}
		// the wind is never quite steady: a slow random wavering of each
		// division's pressure, about 0.4% at Natural
		const float aw = 2.f * (float)M_PI * 2.f * dt;
		for (int d = 0; d < NDIV; d++) {
			wjr ^= wjr << 13; wjr ^= wjr >> 17; wjr ^= wjr << 5;
			float w = ((float)(wjr >> 8) * (1.f / 8388608.f) - 1.f) * 1.732f;
			wj[d] += aw * (w - wj[d]);
			pressure[d] = p[d] * (1.f + 0.004f * irreg * wj[d] / std::sqrt(aw * 0.5f));
		}
		tremPh += tremRate * dt;
		tremPh -= std::floor(tremPh);
		if (tremulant) pressure[SWELL] *= 1.f + tremDepth * std::sin(2.f * (float)M_PI * tremPh);

		// the swell box
		float s = std::max(0.f, std::min(1.f, swell));
		boxG = std::pow(10.f, -20.f * (1.f - s) / 20.f);
		boxF = 700.f * std::pow(20000.f / 700.f, s);
	}

	// Renders one block into outBuf.
	void render() {
		control();
		float buf[NDIV][2][BLOCK];
		std::memset(buf, 0, sizeof(buf));
		float dt = BLOCK / sr, invB = 1.f / BLOCK;
		float flueMul[NDIV], reedMul[NDIV], gainNow[NDIV];
		for (int d = 0; d < NDIV; d++) {
			flueMul[d] = std::pow(pressure[d], 0.04f);
			reedMul[d] = std::pow(pressure[d], 0.01f);
			gainNow[d] = pressure[d];
		}
		std::memset(keyLevel, 0, sizeof(keyLevel));
		for (int ai = 0; ai < nActive; ai++) {
			Pipe& P = pipes[active[ai]];
			const Rank& R = ranks[P.rank];
			const Speech& S = SPEECH[R.fam];
			float g1[3];
			bool done = false;
			if (P.on) {
				P.t += dt;
				bool settled = true;
				for (int g = 0; g < 3; g++) {
					float e = speakEnv(S, g, P.t, P.T);
					float x = (P.t - S.d[g] * P.T) / (S.r[g] * P.T);
					if (x < 1.f || (S.o[g] != 1.f && x < 4.f)) settled = false;
					// a pipe speaking again starts from what it still had
					if (P.carry[g] > 0.f) {
						float c = P.carry[g] * std::exp(-std::max(0.f, P.t) / P.relT);
						if (c > e) { e = c; settled = false; }
					}
					g1[g] = e;
				}
				P.settled = settled;
			} else {
				P.tRel += dt;
				float mx = 0.f;
				for (int g = 0; g < 3; g++) {
					float tau = P.relT * (g == 0 ? 1.f : g == 1 ? 0.75f : 0.5f);
					g1[g] = P.gRel[g] * std::exp(-std::max(0.f, P.tRel) / tau);
					mx = std::max(mx, g1[g]);
				}
				done = mx < 1e-4f;
			}
			if (done) { freePipe(ai); ai--; continue; }

			// what is played: the speech envelope, the pipe's own voicing, and a
			// slow wavering of each group's level and of the pitch (about 3 Hz)
			float ga1[3], pm = 1.f, det[3] = {0.f, 0.f, 0.f};
			if (irreg > 0.f) {
				const float AU = 2.f * (float)M_PI * 3.f * dt, SU = 1.f / std::sqrt(AU * 0.5f);
				static const float DEPTH[3] = {0.015f, 0.035f, 0.06f};
				uint32_t w = P.wr;
				for (int g = 0; g < 3; g++) {
					w ^= w << 13; w ^= w >> 17; w ^= w << 5;
					P.u[g] += AU * (((float)(w >> 8) * (1.f / 8388608.f) - 1.f) * 1.732f - P.u[g]);
					ga1[g] = g1[g] * P.vo[g] * (1.f + DEPTH[g] * irreg * P.u[g] * SU);
				}
				w ^= w << 13; w ^= w >> 17; w ^= w << 5;
				P.uj += AU * (((float)(w >> 8) * (1.f / 8388608.f) - 1.f) * 1.732f - P.uj);
				P.wr = w;
				pm = 1.f + 0.000578f * 0.6f * irreg * P.uj * SU;    // about 0.6 cents
				// a flue pipe flattens as its wind dies away
				if (!P.on && !P.reed) pm *= 1.f - 0.008f * irreg * (1.f - std::exp(-std::max(0.f, P.tRel) / P.relT));
				// While a pipe speaks, before the jet has locked its resonances to
				// one another: it sounds flat, its upper harmonics sit at the
				// pipe's own (flatter, inharmonic) resonances, and they are rough.
				if (P.on) {
					float tt = std::max(0.f, P.t);
					float sp = std::exp(-tt / (0.5f * P.T)) * irreg * (P.reed ? 0.4f : 1.f);
					// flat by 2-8 cents at Natural, a different amount for every
					// pipe: the same bend on a hundred pipes at once is a scoop
					pm *= 1.f - 0.000578f * 8.f * P.scoop * std::exp(-tt / (0.3f * P.T)) * irreg * (P.reed ? 0.4f : 1.f);
					det[1] = -0.000578f * 10.f * sp * P.scoop;
					det[2] = -0.000578f * 25.f * sp * (1.3f - P.scoop);
					const float AR = 2.f * (float)M_PI * 60.f * dt, SR_ = 1.f / std::sqrt(AR * 0.5f);
					for (int k = 0; k < 2; k++) {
						w ^= w << 13; w ^= w >> 17; w ^= w << 5;
						P.uf[k] += AR * (((float)(w >> 8) * (1.f / 8388608.f) - 1.f) * 1.732f - P.uf[k]);
						ga1[k + 1] *= std::max(0.f, 1.f + 0.35f * sp * P.uf[k] * SR_);
					}
					P.wr = w;
				}
			} else {
				for (int g = 0; g < 3; g++) ga1[g] = g1[g];
			}

			float lvl0 = P.level * gainPrev[P.div], lvl1 = P.level * gainNow[P.div];
			float inc = P.baseInc * (P.reed ? reedMul[P.div] : flueMul[P.div]) * pm * TN;
			float ph = P.ph[0] * TN, phB = P.ph[1] * TN, phC = P.ph[2] * TN;
			float incB = inc * (1.f + det[1]), incC = inc * (1.f + det[2]);
			float* L = buf[P.div][0];
			float* Rr = buf[P.div][1];
			float pl = P.panL, pr = P.panR;
			if (sidesOut) { pl = P.side == 0 ? 1.f : 0.f; pr = 1.f - pl; }
			if (P.settled && P.on && irreg <= 0.f) {
				const float* tb = P.tb[3];
				float a = lvl0 * P.ga[0], b = (lvl1 * ga1[0] - a) * invB;
				for (int i = 0; i < BLOCK; i++) {
					int k = (int)ph; float f = ph - k;
					float v = tb[k] + f * (tb[k + 1] - tb[k]);
					v *= a + b * i;
					L[i] += v * pl; Rr[i] += v * pr;
					ph += inc; if (ph >= TN) ph -= TN;
				}
			} else {
				const float *t0 = P.tb[0], *t1 = P.tb[1], *t2 = P.tb[2];
				float a0 = lvl0 * P.ga[0], b0 = (lvl1 * ga1[0] - a0) * invB;
				float a1 = lvl0 * P.ga[1], b1 = (lvl1 * ga1[1] - a1) * invB;
				float a2 = lvl0 * P.ga[2], b2 = (lvl1 * ga1[2] - a2) * invB;
				for (int i = 0; i < BLOCK; i++) {
					int k = (int)ph; float f = ph - k;
					int kb = (int)phB; float fb = phB - kb;
					int kc = (int)phC; float fc = phC - kc;
					float v = (a0 + b0 * i) * (t0[k] + f * (t0[k + 1] - t0[k]))
					        + (a1 + b1 * i) * (t1[kb] + fb * (t1[kb + 1] - t1[kb]))
					        + (a2 + b2 * i) * (t2[kc] + fc * (t2[kc + 1] - t2[kc]));
					L[i] += v * pl; Rr[i] += v * pr;
					ph += inc; if (ph >= TN) ph -= TN;
					phB += incB; if (phB >= TN) phB -= TN;
					phC += incC; if (phC >= TN) phC -= TN;
				}
			}
			if (P.settled && P.on && irreg <= 0.f) phB = phC = ph;   // the summed table carries all three
			P.ph[0] = ph / TN; P.ph[1] = phB / TN; P.ph[2] = phC / TN;
			// the noise: a chiff at the start, and breath on the flutes while they speak
			if (P.b0 > 0.f) {
				float n1;
				float tt = std::max(0.f, P.t);
				if (P.on) n1 = S.chiff * std::exp(-tt / (S.chiffDecay * P.T)) * std::min(1.f, tt / (0.1f * P.T))
				             + S.breath * g1[0];
				else n1 = S.breath * g1[0]
				        // the puff as the pallet closes
				        + (P.tRel >= 0.f ? 0.3f * irreg * S.chiff * std::exp(-P.tRel / std::min(0.02f, 0.6f * P.relT)) : 0.f);
				n1 *= lvl1;
				if (n1 > 1e-5f || P.nLvl > 1e-5f) {
					float n0 = P.nLvl, nb = (n1 - n0) * invB;
					float y1 = P.y1, y2 = P.y2;
					uint32_t s = P.rng;
					for (int i = 0; i < BLOCK; i++) {
						s ^= s << 13; s ^= s >> 17; s ^= s << 5;
						float x = ((float)(s >> 8) * (1.f / 8388608.f) - 1.f) * 1.732f;   // unit variance
						float y = P.b0 * x + P.a1 * y1 - P.a2 * y2;
						y2 = y1; y1 = y;
						float v = y * (n0 + nb * i);
						L[i] += v * pl; Rr[i] += v * pr;
					}
					P.rng = s; P.y1 = y1; P.y2 = y2;
				}
				P.nLvl = n1;
			}
			for (int g = 0; g < 3; g++) { P.g[g] = g1[g]; P.ga[g] = ga1[g]; }
			P.env = (g1[0] + g1[1] + g1[2]) * (1.f / 3.f);
			float& kl = keyLevel[P.div][P.key];
			kl = std::max(kl, P.env);
		}
		for (int d = 0; d < NDIV; d++) gainPrev[d] = gainNow[d];
		pipesSounding = nActive;

		// the swell box, then the mix
		float g = 2.f * (float)M_PI * std::min(boxF, 0.45f * sr) / sr;
		float G = std::tan(0.5f * g), k = 1.4142f;   // TPT state-variable lowpass, Q 0.707
		float a1 = 1.f / (1.f + G * (G + k)), a2 = G * a1, a3 = G * a2;
		for (int c = 0; c < 2; c++) {
			float ic1 = boxS[c][0], ic2 = boxS[c][1];
			float* x = buf[SWELL][c];
			for (int i = 0; i < BLOCK; i++) {
				float v3 = x[i] - ic2;
				float v1 = a1 * ic1 + a2 * v3;
				float v2 = ic2 + a2 * ic1 + a3 * v3;
				ic1 = 2.f * v1 - ic1; ic2 = 2.f * v2 - ic2;
				float gg = boxGPrev + (boxG - boxGPrev) * (i + 1) * invB;
				x[i] = v2 * gg;
			}
			boxS[c][0] = ic1; boxS[c][1] = ic2;
		}
		boxGPrev = boxG;
		for (int c = 0; c < 2; c++)
			for (int i = 0; i < BLOCK; i++)
				outBuf[c][i] = master * (buf[GREAT][c][i] + buf[SWELL][c][i] + buf[PEDAL][c][i]);
	}

	// One sample out, rendering a block when the last one is used up.
	void next(float& l, float& r) {
		if (outPos >= BLOCK) { render(); outPos = 0; }
		l = outBuf[0][outPos]; r = outBuf[1][outPos];
		outPos++;
	}

	void setKey(int div, int key, bool down) {
		uint64_t m = 1ull << (key & 63);
		if (down) kb[div][key >> 6] |= m; else kb[div][key >> 6] &= ~m;
	}
	void allOff() { std::memset(kb, 0, sizeof(kb)); }
};

} // namespace diapason

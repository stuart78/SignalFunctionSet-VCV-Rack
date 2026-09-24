// =============================================================================
// Peal: a grid of bells joined by rods.
//
// Twenty-five modal bells on a 5 by 5 grid, two octaves across. The player
// lays rods between neighbours on the faceplate. A strike puts energy into
// one bell and the energy travels down the rods, losing some at every
// crossing and stopping below a floor, so velocity decides how far a strike
// ripples. A rod carries the striking bell's partials into a bell tuned
// elsewhere, which answers on whatever overtones it shares: the tuning of the
// grid decides what every rod sounds like. ROOT and SCALE do not retune the
// bells, they put a felt on the ones outside the key. SPEED is how long a
// crossing takes, and with CLOCK patched a crossing is exactly one pulse, so
// the grid is a sequencer whose pattern is the network and whose length is
// how hard you hit it. Design: docs/peal-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "scale-bus.hpp"
#include "scales.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

static const int PL_SIDE = 5;
static const int PL_N = PL_SIDE * PL_SIDE;       // bells
static const int PL_MODES = 11;                  // partials per bell, doublets included
static const int PL_PAIRS = PL_N * PL_N;         // a rod may join ANY two bells; stored as a symmetric matrix
static const int PL_POLY = 16;
static const int PL_MAXEVENTS = 512;
static const int PL_MICDELAY = 1024;             // samples of arrival delay a mic can hold

// Bell partials, relative to the prime: hum, prime, tierce, quint, nominal,
// then the upper partials a cast bell carries (deciem, duodeciem, double
// octave). The minor-third tierce is what reads as a bell at any size. Three
// of them are DOUBLETS, a twin a few cents off: a real bell's modes come in
// near-degenerate pairs that beat against each other, and that slow warble is
// most of what separates a bell from a sine bank. The upper partials are loud
// at the strike and gone in under a second; the hum outlasts everything.
static const float PL_RATIO[PL_MODES] = {0.5f, 1.f, 1.0032f, 1.2f, 1.2041f, 1.5f, 2.f, 2.0028f, 2.51f, 3.01f, 4.07f};
static const float PL_AMP[PL_MODES]   = {0.35f, 0.7f, 0.7f, 0.55f, 0.55f, 0.35f, 0.6f, 0.6f, 0.4f, 0.3f, 0.2f};
// Seconds to fall 60 dB, prime at C4, before DAMP. A bell rings for a long
// time: the first table topped out at five seconds and DAMP's default then
// cut the prime to under one, which is a plink, not a bell.
static const float PL_T60[PL_MODES]   = {14.f, 9.f, 9.f, 6.f, 6.f, 4.f, 3.5f, 3.5f, 1.4f, 0.9f, 0.5f};
// A BAR, for SHAPE to morph toward: a free metal bar's partials (1, 2.76,
// 5.40, 8.93, 13.3 ...) are what a glockenspiel or a vibraphone key has,
// stretched far wider than a bell's and gone far sooner. The hum slot is a
// second copy of the fundamental at low level, since a bar has no hum. The
// morph is geometric in frequency, so halfway between a bell and a bar is a
// set of partials halfway between in pitch, not a pair of sets overlaid.
static const float PL_BAR_RATIO[PL_MODES] = {1.f, 1.f, 1.0025f, 2.756f, 2.762f, 5.404f, 8.933f, 8.95f, 13.34f, 18.64f, 24.8f};
static const float PL_BAR_AMP[PL_MODES]   = {0.1f, 1.f, 0.6f, 0.8f, 0.45f, 0.5f, 0.35f, 0.2f, 0.18f, 0.1f, 0.06f};
static const float PL_BAR_T60[PL_MODES]   = {2.5f, 4.f, 4.f, 2.2f, 2.2f, 1.3f, 0.8f, 0.8f, 0.45f, 0.3f, 0.2f};

// The grid's layouts: what a step right and a step down are, in semitones.
struct PealLayout { const char* name; int dx, dy; };
static const PealLayout PL_LAYOUTS[] = {
	{"Tonnetz (fifths across, thirds down)", 7, 4},
	{"Chromatic (semitones across, fourths down)", 1, 5},
	{"Whole tones (across), semitones down", 2, 1},
};
static const int PL_NLAYOUT = (int)(sizeof(PL_LAYOUTS) / sizeof(PL_LAYOUTS[0]));

// the distance between two bells on the grid, in grid units: a rod's length
static inline float pealDist(int a, int b) {
	float du = (float)(a % PL_SIDE) - (float)(b % PL_SIDE), dv = (float)(a / PL_SIDE) - (float)(b / PL_SIDE);
	return std::sqrt(du * du + dv * dv);
}

// SPEED: clockwise is faster. 4 s per rod at the bottom, 10 ms at the top,
// 360 ms at the default. (It ran 5 ms to 2 s the other way round, and a
// 74 ms default ripple read as instant.)
static inline float pealSpeedSec(float k) { return 4.f * std::pow(0.0025f, clamp(k, 0.f, 1.f)); }
static inline float pealFloor(float k)    { return 0.02f * std::pow(0.001f, clamp(k, 0.f, 1.f)); }   // REACH: 2e-2 .. 2e-5

static const char* PL_NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
struct PealRootQ : ParamQuantity {
	std::string getDisplayValueString() override { return PL_NOTE[clamp((int)std::round(getValue()), 0, 11)]; }
};
struct PealSpeedQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float s = pealSpeedSec(getValue());
		return s < 1.f ? string::f("%.0f ms per rod", s * 1000.f) : string::f("%.2f s per rod", s);
	}
};

struct Peal : Module {
	enum ParamId { SIZE_PARAM, DAMP_PARAM, REACH_PARAM, SPEED_PARAM, ROOT_PARAM, SCALE_PARAM,
	               SHAPE_PARAM, BRIGHT_PARAM,                              // appended
	               PARAMS_LEN };
	enum InputId {
		VOCT_INPUT, GATE_INPUT, VEL_INPUT, STRIKE_INPUT, BELL_INPUT, CLOCK_INPUT,
		SIZE_INPUT, DAMP_INPUT, REACH_INPUT, SPEED_INPUT, ROOT_INPUT, SCALE_INPUT,
		SHAPE_INPUT, BRIGHT_INPUT,                                          // appended
		INPUTS_LEN
	};
	enum OutputId { L_OUTPUT, R_OUTPUT, POLY_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	// ── a bell ──────────────────────────────────────────────────────────
	struct Bell {
		float re[PL_MODES] = {}, im[PL_MODES] = {};   // complex resonator state per partial
		float cosw[PL_MODES] = {}, sinw[PL_MODES] = {}, decay[PL_MODES] = {};
		float hz[PL_MODES] = {};
		float semis = 0.f;                            // from the root
		bool  masked = false;                         // a felt on it
		float energy = 0.f;                           // follower, for the rods and the picture
		float out = 0.f;                              // this sample's output
		float glow = 0.f;                             // the picture: struck recently
		float clap = 0.f;                             // the clapper: a bright burst that decays in a few ms
		float clapLp = 0.f;
		float mallet = 0.f, malletT = 0.f, malletDur = 0.003f;   // the contact: force spread over a few ms, not a click
		float malletW[PL_MODES] = {};
		float swing = 0.f;                            // the picture: how hard it is swinging
		float swingT = 0.f;                           // seconds since the strike
	};
	Bell bell[PL_N];
	bool  rodM[PL_PAIRS] = {};                        // rodM[a*25+b], kept symmetric
	float rodGlow[PL_PAIRS] = {};                     // the picture: energy that crossed recently
	bool hasRod(int a, int b) const { return a != b && rodM[a * PL_N + b]; }
	void setRod(int a, int b, bool on) { if (a == b) return; rodM[a * PL_N + b] = rodM[b * PL_N + a] = on; }
	int  degree(int i) const { int d = 0; for (int j = 0; j < PL_N; j++) if (hasRod(i, j)) d++; return d; }
	void clearRods() { for (int k = 0; k < PL_PAIRS; k++) rodM[k] = false; }
	int   layout = 0;
	bool  unmask = false;                             // lift the felts
	int   polyMode = 0;                               // 0 loudest, 1 most recent

	// energy in flight: an arrival at a bell, due at a time (free) or on the next pulse (clocked)
	struct Event { int bell; int from; float energy; double due; double start; long pulseAt; };
	Event ev[PL_MAXEVENTS]; int nev = 0;
	double now = 0.0;
	bool clocked = false;
	long   pulseCount = 0;                            // pulses seen, so a crossing waits for the NEXT one
	double lastPulse = -1.0, clockPeriod = 0.5;       // for the picture: how far along a rod an impulse is
	dsp::SchmittTrigger clockTrig, strikeTrig;
	bool gateWas[PL_POLY] = {};

	// mics on the grid plane, in grid units: u across (column - 2), v back (row)
	float micU[2] = {-1.5f, 1.5f}, micV[2] = {-0.85f, -0.85f};
	float micDelay[2][PL_N][PL_MICDELAY] = {};        // per bell per mic, an arrival delay line
	int   micHead = 0;
	int   micLag[2][PL_N] = {};
	float micGain[2][PL_N] = {};

	// poly out: a channel holds its bell while it rings
	int chanBell[PL_POLY];
	int ctlDiv = 0;
	float sr = 48000.f;
	int root = 0;
	sfs::BusScale sc;
	float sizeK = 1.f, dampK = 0.f, floorE = 1e-3f, shapeK = 0.f, brightK = 0.5f;
	float modeRatio[PL_MODES] = {}, modeAmp[PL_MODES] = {}, modeT60[PL_MODES] = {};   // the set in force, bell morphed toward bar
	void morph() {
		float tilt = (brightK - 0.5f) * 2.5f;            // BRIGHT tilts the partials: +2.5 dB per doubling at the top
		for (int m = 0; m < PL_MODES; m++) {
			modeRatio[m] = PL_RATIO[m] * std::pow(PL_BAR_RATIO[m] / PL_RATIO[m], shapeK);
			float amp = PL_AMP[m] + (PL_BAR_AMP[m] - PL_AMP[m]) * shapeK;
			// RADIATION: a struck body radiates its higher modes more
			// efficiently than its low ones, roughly with the square root of
			// frequency, and the tables above are displacement amplitudes.
			// Without this tilt every voice, bell or bar, sat on its
			// fundamental and read as muffled.
			modeAmp[m] = amp * std::pow(std::max(modeRatio[m], 0.5f), 0.5f + tilt);
			modeT60[m] = PL_T60[m] * std::pow(PL_BAR_T60[m] / PL_T60[m], shapeK);
		}
	}

	Peal() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SIZE_PARAM, 0.f, 1.f, 0.5f, "Size (hand bells to a carillon)", "%", 0.f, 100.f);
		configParam(DAMP_PARAM, 0.f, 1.f, 0.12f, "Damp (a hand on every bell)", "%", 0.f, 100.f);
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.f, "Shape (a cast bell to a metal bar)", "%", 0.f, 100.f);
		configParam(BRIGHT_PARAM, 0.f, 1.f, 0.5f, "Bright (a soft mallet to a hard one, and the tilt of the partials)", "%", 0.f, 100.f);
		configInput(SHAPE_INPUT, "Shape CV");
		configInput(BRIGHT_INPUT, "Bright CV");
		configParam(REACH_PARAM, 0.f, 1.f, 0.5f, "Reach (how little energy still crosses a rod)", "%", 0.f, 100.f);
		configParam<PealSpeedQ>(SPEED_PARAM, 0.f, 1.f, 0.4f, "Speed (how fast energy crosses a rod; one pulse when clocked)");
		configParam<PealRootQ>(ROOT_PARAM, 0.f, 11.f, 0.f, "Root");
		paramQuantities[ROOT_PARAM]->snapEnabled = true;
		std::vector<std::string> names;
		for (int i = 0; i < sfs::NUM_SCALES; i++) names.push_back(sfs::SCALES[i].longName);
		configSwitch(SCALE_PARAM, 0.f, (float)(sfs::NUM_SCALES - 1), 1.f, "Scale (bells outside it are felted)", names);
		configInput(VOCT_INPUT, "V/OCT (poly): the nearest bell is struck");
		configInput(GATE_INPUT, "Gate (poly)");
		configInput(VEL_INPUT, "Velocity (poly, 0-10V): how much energy the strike delivers");
		configInput(STRIKE_INPUT, "Strike trigger");
		configInput(BELL_INPUT, "Bell (1V/oct) for the Strike trigger");
		configInput(CLOCK_INPUT, "Clock: with it patched, a rod crossing is one pulse");
		configInput(SIZE_INPUT, "Size CV");
		configInput(DAMP_INPUT, "Damp CV");
		configInput(REACH_INPUT, "Reach CV");
		configInput(SPEED_INPUT, "Speed CV");
		configInput(ROOT_INPUT, "Root CV (1V/oct, semitone-quantized)");
		configInput(SCALE_INPUT, "Scale CV (1V per scale, or Key's scale bus)");
		configOutput(L_OUTPUT, "Left mic");
		configOutput(R_OUTPUT, "Right mic");
		configOutput(POLY_OUTPUT, "The sixteen loudest bells");
		for (int c = 0; c < PL_POLY; c++) chanBell[c] = -1;
		// something to start from: a chain snaking through every bell
		presetRods(4);
		morph();
		retune();
	}

	// ── the grid's tuning ────────────────────────────────────────────────
	void retune() {
		const PealLayout& L = PL_LAYOUTS[clamp(layout, 0, PL_NLAYOUT - 1)];
		for (int i = 0; i < PL_N; i++) {
			int r = i / PL_SIDE, c = i % PL_SIDE;
			// centred on the grid, folded into the two octaves round the root
			int s = (c - 2) * L.dx + (2 - r) * L.dy;
			s = ((s + 12) % 24 + 24) % 24 - 12;
			bell[i].semis = (float)s;
		}
	}
	// a bell's prime, in Hz, at the SIZE in force: C4 at the root, halved per octave of SIZE range
	float primeHz(int i) const {
		return dsp::FREQ_C4 * std::pow(2.f, ((float)root + bell[i].semis) / 12.f) * std::pow(2.f, 1.f - 2.f * sizeK);
	}
	void updateBell(int i) {
		Bell& b = bell[i];
		float f0 = primeHz(i);
		// smaller bells decay faster: T60 scales with the prime's period, up to a point
		float sizeT = clamp(261.6f / f0, 0.25f, 4.f);
		// DAMP: nothing at the bottom, a hand on the bell at the top; squared
		// so the first third of the knob is a light touch
		float damp = 1.f + dampK * dampK * 40.f;
		if (b.masked) damp *= 8.f;
		for (int m = 0; m < PL_MODES; m++) {
			float f = f0 * modeRatio[m];
			b.hz[m] = f;
			float w = 2.f * (float)M_PI * f / sr;
			b.cosw[m] = std::cos(w); b.sinw[m] = std::sin(w);
			float t60 = modeT60[m] * sizeT / damp;
			b.decay[m] = (f < sr * 0.45f) ? std::exp(-6.908f / (t60 * sr)) : 0.f;
		}
	}
	// energy into a bell: a direct strike excites every partial at its own
	// weight; a rod excites the partials the SENDER shares, because that is
	// what a rod carries, plus a little of everything, because an impulse is broad
	void excite(int i, float energy, int from) {
		Bell& b = bell[i];
		float amp = std::sqrt(std::max(energy, 0.f));
		if (b.masked) amp *= 0.2f;
		for (int m = 0; m < PL_MODES; m++) {
			float w = modeAmp[m];
			if (from >= 0) {
				float best = 0.f;
				for (int p = 0; p < PL_MODES; p++) {
					float cents = 1200.f * std::log2(b.hz[m] / bell[from].hz[p]);
					best = std::max(best, std::exp(-(cents * cents) / (2.f * 45.f * 45.f)));
				}
				w *= 0.15f + 0.85f * best;
			}
			b.malletW[m] = w;
		}
		// THE CONTACT, NOT A CLICK. A clapper stays on the bell for a few
		// milliseconds, longer on a big bell, and the force it delivers is a
		// raised cosine over that time: the high partials get less of it than
		// an impulse would give and the strike stops sounding like a switch.
		b.mallet = amp;
		b.malletT = 0.f;
		// A harder mallet is a shorter contact, which is more of the high
		// partials: a contact of T seconds delivers almost nothing above 1/T,
		// and the first figure, 1.5 ms at C4, cut everything over 700 Hz off
		// the strike -- which is why a bar came out DULLER than a bell. It now
		// runs 2 ms (soft) to 0.15 ms (hard) at C4, scaled by the bell's size.
		// A bar is struck with a much harder head than a bell's clapper, so
		// SHAPE shortens the contact as well: at a Hann contact of T the first
		// null is at 2/T, and a glockenspiel's partials at 5 and 9 times the
		// fundamental need T under 0.3 ms to be struck at all.
		b.malletDur = clamp(0.0008f * 261.6f / std::max(b.hz[1], 40.f) * (2.4f - 2.2f * brightK) * (1.f - 0.85f * shapeK), 0.0001f, 0.006f);
		b.clap = std::max(b.clap, amp * (from >= 0 ? 0.15f : 0.5f) * (0.3f + 0.9f * brightK));   // a rod arrives softer than a clapper
		b.glow = 1.f;
		b.swing = std::min(1.f, b.swing + amp * 0.5f);
		b.swingT = 0.f;
	}
	// a strike, and what it sends down the rods
	void strike(int i, float energy, int from) {
		if (i < 0 || i >= PL_N || energy <= 0.f) return;
		excite(i, energy, from);
		int deg = degree(i);
		if (deg == 0) return;
		// LOSSY, and CONSERVED: what leaves is a fraction of what arrived,
		// split between the rods, so a mesh cannot multiply energy (the first
		// figure was 0.55, which reached six bells at any velocity: a chain
		// halves twice per hop and the floor arrives too soon). Below the
		// floor a rod passes nothing, and that is where a ripple ends.
		float send = energy * 0.85f / (float)deg;
		if (send < floorE) return;
		float speedK = clamp(params[SPEED_PARAM].getValue() + inputs[SPEED_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		for (int other = 0; other < PL_N; other++) {
			if (!hasRod(i, other) || nev >= PL_MAXEVENTS) continue;
			// a longer rod takes longer to cross, free-running; clocked it is one pulse whatever its length
			ev[nev++] = {other, i, send, clocked ? 0.0 : now + pealSpeedSec(speedK) * pealDist(i, other), now, pulseCount};
			rodGlow[i * PL_N + other] = rodGlow[other * PL_N + i] = 1.f;
		}
	}
	void presetRods(int which) {
		clearRods();
		switch (which) {
			case 0:                                                                         // mesh: every neighbouring pair
				for (int i = 0; i < PL_N; i++) { if (i % PL_SIDE < 4) setRod(i, i + 1, true); if (i + PL_SIDE < PL_N) setRod(i, i + PL_SIDE, true); }
				break;
			case 1: for (int i = 0; i < PL_N; i++) if (i % PL_SIDE < 4) setRod(i, i + 1, true); break;          // rows
			case 2: for (int i = 0; i + PL_SIDE < PL_N; i++) setRod(i, i + PL_SIDE, true); break;               // columns
			case 3: {                                                                       // spiral from the centre
				static const int SP[] = {12, 13, 8, 7, 6, 11, 16, 17, 18, 19, 14, 9, 4, 3, 2, 1, 0, 5, 10, 15, 20, 21, 22, 23, 24};
				for (int k = 0; k + 1 < 25; k++) setRod(SP[k], SP[k + 1], true);
				break;
			}
			case 4:                                                                         // a chain snaking through all 25
				for (int row = 0; row < PL_SIDE; row++) {
					for (int c = 0; c < 4; c++) setRod(row * PL_SIDE + c, row * PL_SIDE + c + 1, true);
					if (row + 1 < PL_SIDE) { int col = (row % 2 == 0) ? 4 : 0; setRod(row * PL_SIDE + col, (row + 1) * PL_SIDE + col, true); }
				}
				break;
			case 5: {                                                                       // a star: the centre to every corner and edge middle
				static const int OUT[] = {0, 2, 4, 10, 14, 20, 22, 24};
				for (int k = 0; k < 8; k++) setRod(12, OUT[k], true);
				break;
			}
			default: break;                                                                 // clear
		}
	}
	// the bell nearest a pitch (semitones from the root), unfelted first
	int nearestBell(float semis) const {
		int best = -1; float bd = 1e9f;
		for (int pass = 0; pass < 2 && best < 0; pass++)
			for (int i = 0; i < PL_N; i++) {
				if (pass == 0 && bell[i].masked) continue;
				float d = std::fabs(bell[i].semis - semis);
				if (d < bd) { bd = d; best = i; }
			}
		return best;
	}

	// ── the mics ────────────────────────────────────────────────────────
	void placeMics() {
		for (int k = 0; k < 2; k++)
			for (int i = 0; i < PL_N; i++) {
				float u = (float)(i % PL_SIDE) - 2.f, v = (float)(i / PL_SIDE);
				float d = std::sqrt((u - micU[k]) * (u - micU[k]) + (v - micV[k]) * (v - micV[k]));
				// a grid unit is about 30 cm: 0.9 ms of air per unit
				micLag[k][i] = clamp((int)(d * 0.0009f * sr), 0, PL_MICDELAY - 1);
				micGain[k][i] = 1.f / (0.6f + d);
			}
	}

	void process(const ProcessArgs& args) override {
		sr = args.sampleRate;
		now += args.sampleTime;
		// ── control rate ──────────────────────────────────────────────────
		if ((ctlDiv++ & 63) == 0) {
			int r = (int)std::round(params[ROOT_PARAM].getValue());
			if (inputs[ROOT_INPUT].isConnected()) r += (int)std::round(inputs[ROOT_INPUT].getVoltage() * 12.f);
			root = ((r % 12) + 12) % 12;
			sc = sfs::busResolve(inputs[SCALE_INPUT], (int)std::round(params[SCALE_PARAM].getValue()));
			sizeK = clamp(params[SIZE_PARAM].getValue() + inputs[SIZE_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
			dampK = clamp(params[DAMP_PARAM].getValue() + inputs[DAMP_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
			floorE = pealFloor(clamp(params[REACH_PARAM].getValue() + inputs[REACH_INPUT].getVoltage() * 0.2f, 0.f, 1.f));
			shapeK = clamp(params[SHAPE_PARAM].getValue() + inputs[SHAPE_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
			brightK = clamp(params[BRIGHT_PARAM].getValue() + inputs[BRIGHT_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
			morph();
			for (int i = 0; i < PL_N; i++) {
				// in the key? within a quarter tone of a degree, any octave
				float s = bell[i].semis;
				float within = s - std::floor(s / sc.period) * sc.period;
				bool inKey = sc.size <= 0;
				for (int d = 0; d < sc.size && !inKey; d++)
					if (std::fabs(sc.intervals[d] - within) < 0.5f || std::fabs(sc.intervals[d] - within + sc.period) < 0.5f) inKey = true;
				bell[i].masked = !unmask && !inKey;
				updateBell(i);
			}
			placeMics();
			// poly out: a channel keeps its bell while it rings; the loudest
			// unassigned bells take the free channels
			for (int c = 0; c < PL_POLY; c++)
				if (chanBell[c] >= 0 && bell[chanBell[c]].energy < 1e-5f) chanBell[c] = -1;
			for (int c = 0; c < PL_POLY; c++) {
				if (chanBell[c] >= 0) continue;
				int best = -1; float be = 1e-4f;
				for (int i = 0; i < PL_N; i++) {
					bool held = false;
					for (int k = 0; k < PL_POLY; k++) if (chanBell[k] == i) held = true;
					if (!held && bell[i].energy > be) { be = bell[i].energy; best = i; }
				}
				chanBell[c] = best;
				if (best < 0) break;
			}
		}
		clocked = inputs[CLOCK_INPUT].isConnected();
		bool pulse = clocked && clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		if (pulse) {
			pulseCount++;
			if (lastPulse >= 0.0 && now - lastPulse > 0.005) clockPeriod = now - lastPulse;
			lastPulse = now;
		}

		// ── strikes in ────────────────────────────────────────────────────
		int nch = std::max(inputs[GATE_INPUT].getChannels(), 1);
		for (int c = 0; c < nch && c < PL_POLY; c++) {
			bool g = inputs[GATE_INPUT].getVoltage(c) >= 1.f;
			if (g && !gateWas[c]) {
				float semis = inputs[VOCT_INPUT].getPolyVoltage(c) * 12.f - (float)root;
				float vel = inputs[VEL_INPUT].isConnected() ? clamp(inputs[VEL_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f) : 0.8f;
				strike(nearestBell(semis), vel * vel * vel * 4.f, -1);
			}
			gateWas[c] = g;
		}
		if (strikeTrig.process(inputs[STRIKE_INPUT].getVoltage(), 0.1f, 1.f)) {
			float semis = inputs[BELL_INPUT].getVoltage() * 12.f - (float)root;
			float vel = inputs[VEL_INPUT].isConnected() ? clamp(inputs[VEL_INPUT].getVoltage() * 0.1f, 0.f, 1.f) : 0.8f;
			strike(nearestBell(semis), vel * vel * vel * 4.f, -1);
		}

		// ── energy arriving ───────────────────────────────────────────────
		// Free-running, an event lands when its time comes. Clocked, every
		// event waiting lands on the pulse, so a crossing is one pulse -- the
		// NEXT pulse, not one that fired on the same sample as the strike: a
		// sequencer clocked from the same source as CLOCK struck and crossed
		// at once, and the first hop was invisible.
		if (nev) {
			Event pending[PL_MAXEVENTS]; int np = 0;
			Event land[PL_MAXEVENTS]; int nl = 0;
			for (int k = 0; k < nev; k++) {
				bool due = clocked ? (pulse && pulseCount > ev[k].pulseAt) : (ev[k].due <= now);
				if (due) land[nl++] = ev[k]; else pending[np++] = ev[k];
			}
			nev = np; for (int k = 0; k < np; k++) ev[k] = pending[k];
			for (int k = 0; k < nl; k++) strike(land[k].bell, land[k].energy, land[k].from);
		}

		// ── the bells ring ────────────────────────────────────────────────
		float outL = 0.f, outR = 0.f;
		for (int i = 0; i < PL_N; i++) {
			Bell& b = bell[i];
			float s = 0.f, e = 0.f;
			float force = 0.f;
			if (b.mallet > 0.f) {
				float u = b.malletT / b.malletDur;
				if (u >= 1.f) b.mallet = 0.f;
				else force = b.mallet * (0.5f - 0.5f * std::cos((float)M_PI * 2.f * u)) / (b.malletDur * sr) * 2.f;
				b.malletT += args.sampleTime;
			}
			for (int m = 0; m < PL_MODES; m++) {
				if (force != 0.f) b.im[m] += force * b.malletW[m];
				float re = b.re[m] * b.cosw[m] - b.im[m] * b.sinw[m];
				float im = b.re[m] * b.sinw[m] + b.im[m] * b.cosw[m];
				b.re[m] = re * b.decay[m]; b.im[m] = im * b.decay[m];
				s += b.re[m];
				e += re * re + im * im;
			}
			// the clapper: a burst of noise through a bandpass round the upper
			// partials, gone in a few milliseconds, which is the "clang" that
			// says something struck the bell
			if (b.clap > 1e-4f) {
				float nz = random::uniform() * 2.f - 1.f;
				b.clapLp += (nz - b.clapLp) * 0.35f;
				s += (nz - b.clapLp) * b.clap * 0.6f;
				b.clap *= 0.9985f;
			}
			b.out = s * 0.45f;
			b.energy += (e - b.energy) * 0.002f;
			b.glow *= 0.9995f;
			b.swingT += args.sampleTime;
			for (int k = 0; k < 2; k++) {
				micDelay[k][i][micHead] = b.out * micGain[k][i];
				int at = (micHead - micLag[k][i] + PL_MICDELAY) % PL_MICDELAY;
				if (k == 0) outL += micDelay[k][i][at]; else outR += micDelay[k][i][at];
			}
		}
		micHead = (micHead + 1) % PL_MICDELAY;
		for (int k = 0; k < PL_PAIRS; k++) rodGlow[k] *= 0.9993f;

		outputs[L_OUTPUT].setVoltage(clamp(outL * 3.f, -10.f, 10.f));
		outputs[R_OUTPUT].setVoltage(clamp(outR * 3.f, -10.f, 10.f));
		outputs[POLY_OUTPUT].setChannels(PL_POLY);
		for (int c = 0; c < PL_POLY; c++)
			outputs[POLY_OUTPUT].setVoltage(chanBell[c] >= 0 ? clamp(bell[chanBell[c]].out * 4.f, -10.f, 10.f) : 0.f, c);
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_t* rods = json_array();                  // pairs, a < b
		for (int a = 0; a < PL_N; a++) for (int b = a + 1; b < PL_N; b++)
			if (hasRod(a, b)) { json_t* pr = json_array(); json_array_append_new(pr, json_integer(a)); json_array_append_new(pr, json_integer(b)); json_array_append_new(rods, pr); }
		json_object_set_new(r, "rodPairs", rods);
		json_object_set_new(r, "layout", json_integer(layout));
		json_object_set_new(r, "unmask", json_boolean(unmask));
		json_object_set_new(r, "polyMode", json_integer(polyMode));
		json_t* mics = json_array();
		for (int k = 0; k < 2; k++) { json_array_append_new(mics, json_real(micU[k])); json_array_append_new(mics, json_real(micV[k])); }
		json_object_set_new(r, "mics", mics);
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* rods = json_object_get(r, "rodPairs")) {
			clearRods();
			for (int k = 0; k < (int)json_array_size(rods); k++) {
				json_t* pr = json_array_get(rods, k);
				if (json_array_size(pr) == 2) setRod(clamp((int)json_integer_value(json_array_get(pr, 0)), 0, PL_N - 1), clamp((int)json_integer_value(json_array_get(pr, 1)), 0, PL_N - 1), true);
			}
		}
		if (json_t* j = json_object_get(r, "layout")) layout = clamp((int)json_integer_value(j), 0, PL_NLAYOUT - 1);
		if (json_t* j = json_object_get(r, "unmask")) unmask = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "polyMode")) polyMode = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* mics = json_object_get(r, "mics"))
			if (json_array_size(mics) == 4) for (int k = 0; k < 2; k++) {
				micU[k] = clamp((float)json_real_value(json_array_get(mics, 2 * k)), -3.5f, 3.5f);
				micV[k] = clamp((float)json_real_value(json_array_get(mics, 2 * k + 1)), -2.5f, 5.5f);
			}
		retune();
	}
};

// =============================================================================
// The grid, drawn on the faceplate. No screen: the panel's greys, orange for
// energy, as Wheel and Flock do. The camera stands a little above the front
// edge and looks down across the grid, so the back row is smaller and closer
// together than the front and the bells read as objects of different sizes
// standing on a surface. The mics stand on the same surface.
//
// The projection is a plane in perspective with a closed-form inverse, so
// the hit-test is the same projection run backwards: a point on the panel
// returns a point on the grid plane, and from that a bell or a mic.
// =============================================================================
static const float PL_K = 0.16f;                 // foreshortening per row of depth
static const float PL_VBACK = 4.f, PL_VFRONT = -1.15f;  // the plane's visible depth range, in rows

struct PealView {
	float w, h, cx, yFront, yBack, S;
	PealView(float w_, float h_) : w(w_), h(h_) {
		cx = w * 0.5f;
		yFront = h * 0.97f; yBack = h * 0.09f;
		S = w * 0.205f;                              // px per grid unit at v = 0: the front row spans most of the width
	}
	float depthScale(float v) const { return 1.f / (1.f + PL_K * (v - PL_VFRONT)); }
	// plane (u across, v back) -> panel
	void project(float u, float v, float& x, float& y) const {
		float ws = depthScale(v), wb = depthScale(PL_VBACK);
		x = cx + u * S * ws / depthScale(PL_VFRONT);
		y = yFront - (yFront - yBack) * (1.f - ws / depthScale(PL_VFRONT)) / (1.f - wb / depthScale(PL_VFRONT));
	}
	// panel -> plane
	void unproject(float x, float y, float& u, float& v) const {
		float w0 = depthScale(PL_VFRONT), wb = depthScale(PL_VBACK);
		float ws = w0 * (1.f - (yFront - y) / (yFront - yBack) * (1.f - wb / w0));
		ws = clamp(ws, wb * 0.5f, w0 * 1.5f);
		v = (1.f / ws - 1.f) / PL_K + PL_VFRONT;
		u = (x - cx) / (S * ws / w0);
	}
};

static Peal* pealPreview() {
	static Peal* m = nullptr;
	if (m) return m;
	m = new Peal();
	m->presetRods(3);
	rack::engine::Module::ProcessArgs a = sfs::previewArgs();
	int64_t frame = 0;
	for (int i = 0; i < 64; i++) { a.frame = frame++; m->process(a); }
	m->strike(12, 4.f, -1);
	for (int i = 0; i < (int)(0.55f * sfs::PREVIEW_SR); i++) { a.frame = frame++; m->process(a); }
	return m;
}

static const NVGcolor PL_INK  = nvgRGB(0x23, 0x1F, 0x20);
static const NVGcolor PL_SOFT = nvgRGB(0x8A, 0x8A, 0x96);
static const NVGcolor PL_LINE = nvgRGB(0xB2, 0xB2, 0xBC);
static const NVGcolor PL_HOT  = nvgRGB(0xEC, 0x65, 0x2E);

struct PealDisplay : OpaqueWidget {
	Peal* module = nullptr;
	std::shared_ptr<Font> font;
	int dragBell = -1, dragMic = -1;                 // what the press landed on
	int hoverBell = -1;
	int lastBell = -1;                                // the bell the drag last passed over
	bool dragErase = false, dragPainted = false;
	Vec dragStart, dragPos;

	int bellAt(Vec p) const {
		if (!module) return -1;
		PealView V(box.size.x, box.size.y);
		int best = -1; float bd = 1e9f;
		for (int i = 0; i < PL_N; i++) {
			float x, y; V.project((float)(i % PL_SIDE) - 2.f, (float)(i / PL_SIDE), x, y);
			float d = std::hypot(p.x - x, p.y - y);
			float r = mm2px(4.4f) * V.depthScale((float)(i / PL_SIDE)) / V.depthScale(PL_VFRONT);
			if (d < r * 1.3f && d < bd) { bd = d; best = i; }
		}
		return best;
	}
	int micAt(Vec p) const {
		if (!module) return -1;
		PealView V(box.size.x, box.size.y);
		for (int k = 0; k < 2; k++) {
			float x, y; V.project(module->micU[k], module->micV[k], x, y);
			if (std::hypot(p.x - x, p.y - y) < mm2px(2.2f)) return k;
		}
		return -1;
	}
	// DRAW THE RODS BY DRAGGING. Press on a bell and release on ANY other,
	// neighbour or not, and a rod joins them; do it again and the rod goes.
	// While the drag is out, the rod being laid is drawn from the bell to the
	// pointer. A press and release on one bell strikes it.
	void onButton(const ButtonEvent& e) override {
		OpaqueWidget::onButton(e);
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT) return;
		if (e.action == GLFW_PRESS) {
			dragMic = micAt(e.pos);                   // a mic first: it stands on the same surface as the bells
			dragBell = dragMic < 0 ? bellAt(e.pos) : -1;
			lastBell = dragBell; dragPainted = false;
			dragStart = dragPos = e.pos;
			if (dragMic >= 0 || dragBell >= 0) e.consume(this);
		}
	}
	void onHover(const HoverEvent& e) override {
		OpaqueWidget::onHover(e);
		hoverBell = module ? bellAt(e.pos) : -1;
		e.consume(this);
	}
	void onLeave(const LeaveEvent& e) override { hoverBell = -1; }
	void onDragHover(const DragHoverEvent& e) override {
		if (!module) return;
		dragPos = e.pos;
		if (dragMic >= 0) {
			PealView V(box.size.x, box.size.y);
			float u, v; V.unproject(e.pos.x, e.pos.y, u, v);
			module->micU[dragMic] = clamp(u, -3.5f, 3.5f);
			module->micV[dragMic] = clamp(v, -2.5f, 5.5f);
			return;
		}
		if (dragBell < 0) return;
		lastBell = bellAt(e.pos);
		if (lastBell >= 0 && lastBell != dragBell) dragPainted = true;   // the pointer has been on another bell: this is a rod, not a strike
	}
	void onDragEnd(const DragEndEvent& e) override {
		if (!module) return;
		if (dragMic >= 0) { dragMic = -1; return; }
		if (dragBell < 0) return;
		int end = bellAt(dragPos);
		if (end >= 0 && end != dragBell) {
			module->setRod(dragBell, end, !module->hasRod(dragBell, end));
		} else if (!dragPainted) {
			// a click strikes it, harder toward the top of the bell
			PealView V(box.size.x, box.size.y);
			float x, y; V.project((float)(dragBell % PL_SIDE) - 2.f, (float)(dragBell / PL_SIDE), x, y);
			float vel = clamp(0.5f + (y - dragStart.y) / mm2px(6.f), 0.2f, 1.f);
			module->strike(dragBell, vel * vel * vel * 4.f, -1);
		}
		dragBell = -1; lastBell = -1; dragPainted = false;
	}

	// a bell, drawn: crown, shoulders, a flared lip and a clapper, hanging
	// from its crown so it swings about that point when struck
	// A bell in profile, seen from a little above: a crown knob, a rounded
	// shoulder, a waist that runs nearly straight, a sound bow that flares
	// out to the lip, and the lip's rim as an ellipse because we look down
	// on it. Hangs from the crown, so it swings about that point.
	static void drawBell(NVGcontext* vg, float x, float y, float h, float angle, NVGcolor fill, bool felted) {
		nvgSave(vg);
		nvgTranslate(vg, x, y - h * 0.6f);
		nvgRotate(vg, angle);
		float W = h * 0.62f;                         // half-width at the lip
		float ry = W * 0.28f;                        // the lip's ellipse, foreshortened
		NVGcolor edge = felted ? PL_SOFT : PL_INK;
		// the body
		nvgBeginPath(vg);
		nvgMoveTo(vg, -W * 0.16f, 0.f);
		nvgBezierTo(vg, -W * 0.42f, h * 0.02f, -W * 0.52f, h * 0.16f, -W * 0.54f, h * 0.30f);   // shoulder
		nvgBezierTo(vg, -W * 0.56f, h * 0.55f, -W * 0.58f, h * 0.72f, -W * 0.66f, h * 0.84f);  // waist
		nvgBezierTo(vg, -W * 0.78f, h * 0.94f, -W * 0.95f, h * 0.98f, -W, h);                  // sound bow
		nvgLineTo(vg, W, h);
		nvgBezierTo(vg, W * 0.95f, h * 0.98f, W * 0.78f, h * 0.94f, W * 0.66f, h * 0.84f);
		nvgBezierTo(vg, W * 0.58f, h * 0.72f, W * 0.56f, h * 0.55f, W * 0.54f, h * 0.30f);
		nvgBezierTo(vg, W * 0.52f, h * 0.16f, W * 0.42f, h * 0.02f, W * 0.16f, 0.f);
		nvgClosePath(vg);
		nvgFillColor(vg, fill); nvgFill(vg);
		nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		// the lip's rim, an ellipse below the body's foot
		nvgBeginPath(vg); nvgEllipse(vg, 0.f, h, W, ry);
		nvgFillColor(vg, felted ? nvgRGB(0xE4, 0xE4, 0xE8) : nvgRGB(0x3A, 0x36, 0x38)); nvgFill(vg);
		nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		// a highlight down the shoulder, so the body reads as round
		if (!felted) {
			nvgBeginPath(vg);
			nvgMoveTo(vg, -W * 0.30f, h * 0.14f);
			nvgBezierTo(vg, -W * 0.40f, h * 0.35f, -W * 0.42f, h * 0.6f, -W * 0.48f, h * 0.8f);
			nvgStrokeColor(vg, nvgRGBA(0xF0, 0xF0, 0xF0, 0x70)); nvgStrokeWidth(vg, std::max(0.6f, W * 0.08f)); nvgStroke(vg);
		}
		// the crown knob
		nvgBeginPath(vg); nvgCircle(vg, 0.f, -h * 0.05f, std::max(0.7f, W * 0.13f));
		nvgFillColor(vg, edge); nvgFill(vg);
		nvgRestore(vg);
	}

	void draw(const DrawArgs& args) override {
		if (!module) { module = pealPreview(); draw(args); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		PealView V(box.size.x, box.size.y);
		nvgSave(vg); nvgScissor(vg, 0, 0, box.size.x, box.size.y);

		// the surface: a faint grid of the plane the bells stand on
		nvgStrokeWidth(vg, 0.5f);
		for (int g = -3; g <= 3; g++) {
			float x0, y0, x1, y1;
			V.project((float)g, PL_VFRONT + 0.1f, x0, y0); V.project((float)g, PL_VBACK + 0.6f, x1, y1);
			nvgBeginPath(vg); nvgMoveTo(vg, x0, y0); nvgLineTo(vg, x1, y1);
			nvgStrokeColor(vg, nvgTransRGBA(PL_LINE, 0x50)); nvgStroke(vg);
		}
		for (float v = PL_VFRONT + 0.1f; v <= PL_VBACK + 0.6f; v += 1.f) {
			float x0, y0, x1, y1;
			V.project(-3.f, v, x0, y0); V.project(3.f, v, x1, y1);
			nvgBeginPath(vg); nvgMoveTo(vg, x0, y0); nvgLineTo(vg, x1, y1);
			nvgStrokeColor(vg, nvgTransRGBA(PL_LINE, 0x50)); nvgStroke(vg);
		}
		// rods, back to front, glowing with the energy that crossed
		auto bellXY = [&](int i, float& x, float& y) { V.project((float)(i % PL_SIDE) - 2.f, (float)(i / PL_SIDE), x, y); };
		for (int a = PL_N - 1; a >= 0; a--) for (int b = a + 1; b < PL_N; b++) {
			if (!module->hasRod(a, b)) continue;
			float xa, ya, xb, yb; bellXY(a, xa, ya); bellXY(b, xb, yb);
			float g = module->rodGlow[a * PL_N + b];
			nvgBeginPath(vg); nvgMoveTo(vg, xa, ya); nvgLineTo(vg, xb, yb);
			nvgStrokeColor(vg, nvgRGBAf(PL_INK.r + (PL_HOT.r - PL_INK.r) * g, PL_INK.g + (PL_HOT.g - PL_INK.g) * g, PL_INK.b + (PL_HOT.b - PL_INK.b) * g, 0.85f));
			nvgStrokeWidth(vg, 1.2f + 1.8f * g); nvgStroke(vg);
		}
		// the rod being laid, from the pressed bell to the pointer
		if (dragBell >= 0 && dragPainted) {
			float xa, ya; bellXY(dragBell, xa, ya);
			nvgBeginPath(vg); nvgMoveTo(vg, xa, ya); nvgLineTo(vg, dragPos.x, dragPos.y);
			nvgStrokeColor(vg, nvgTransRGBA(PL_HOT, 0xA0)); nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);
		}
		// ANT TRAILS: every impulse in flight is a run of dots along its rod,
		// the head brightest, so you watch the energy travel and see where
		// it will land next. Free-running, the head is at its share of the
		// crossing time; clocked, at its share of the last clock period.
		{
			int n = std::min(module->nev, PL_MAXEVENTS);
			for (int k = 0; k < n; k++) {
				const Peal::Event& e = module->ev[k];
				if (e.from < 0 || e.bell < 0) continue;
				double dur = module->clocked ? module->clockPeriod : std::max(e.due - e.start, 1e-3);
				float f = clamp((float)((module->now - e.start) / dur), 0.f, 0.97f);
				float xa, ya, xb, yb; bellXY(e.from, xa, ya); bellXY(e.bell, xb, yb);
				float amp = clamp(std::sqrt(e.energy) * 0.8f, 0.25f, 1.f);
				for (int d = 0; d < 4; d++) {
					float t = f - d * 0.05f; if (t < 0.f) break;
					nvgBeginPath(vg); nvgCircle(vg, xa + (xb - xa) * t, ya + (yb - ya) * t, mm2px(0.55f) * (1.f - 0.18f * d));
					nvgFillColor(vg, nvgTransRGBA(PL_HOT, (unsigned char)(230 * amp * (1.f - 0.25f * d)))); nvgFill(vg);
				}
			}
		}
		// bells, back row first so a near one paints over a far one; each
		// hangs from its crown and swings when struck, a rocking that dies
		// away over a second or so
		for (int i = PL_N - 1; i >= 0; i--) {
			const Peal::Bell& b = module->bell[i];
			int row = i / PL_SIDE;
			float x, y; bellXY(i, x, y);
			float persp = V.depthScale((float)row) / V.depthScale(PL_VFRONT);
			// lower bells are bigger: height from the pitch, two octaves across
			float hh = mm2px(5.2f + 4.2f * (12.f - b.semis) / 24.f) * persp;
			float lvl = clamp(std::sqrt(b.energy) * 3.f, 0.f, 1.f);
			float sw = b.swing * std::exp(-b.swingT * 1.4f) * std::sin(b.swingT * 2.f * (float)M_PI * (2.4f + 0.4f * (12.f - b.semis) / 24.f));
			float angle = sw * 0.35f;
			if (lvl > 0.02f) {
				// the lip's ring of sound
				nvgBeginPath(vg); nvgEllipse(vg, x, y + hh * 0.45f, hh * 0.55f * (1.f + 0.6f * lvl), hh * 0.16f * (1.f + 0.6f * lvl));
				nvgStrokeColor(vg, nvgTransRGBA(PL_HOT, (unsigned char)(180 * lvl))); nvgStrokeWidth(vg, 0.8f + 1.4f * lvl); nvgStroke(vg);
			}
			NVGcolor fill;
			if (b.masked) fill = nvgRGB(0xF0, 0xF0, 0xF0);
			else {
				float g = clamp(b.glow, 0.f, 1.f);
				fill = nvgRGBAf(PL_INK.r + (PL_HOT.r - PL_INK.r) * g, PL_INK.g + (PL_HOT.g - PL_INK.g) * g, PL_INK.b + (PL_HOT.b - PL_INK.b) * g, 1.f);
			}
			drawBell(vg, x, y, hh, angle, fill, b.masked);
		}
		// THE NOTE UNDER THE POINTER. The layout is a Tonnetz by default, which
		// nobody can read off a grid of identical bells, so the bell the mouse is
		// over names its note and octave at the size the grid is set to.
		if (hoverBell >= 0 && font && font->handle >= 0) {
			const Peal::Bell& b = module->bell[hoverBell];
			float hz = module->primeHz(hoverBell);
			int midi = (int)std::round(69.f + 12.f * std::log2(hz / 440.f));
			std::string name = string::f("%s%d", PL_NOTE[((midi % 12) + 12) % 12], midi / 12 - 1);
			if (b.masked) name += "  felted";
			float x, y; bellXY(hoverBell, x, y);
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, mm2px(2.6f));
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
			float tw = nvgTextBounds(vg, 0, 0, name.c_str(), NULL, NULL);
			float ty = y - mm2px(6.5f);
			nvgBeginPath(vg); nvgRoundedRect(vg, x - tw * 0.5f - mm2px(1.f), ty - mm2px(3.2f), tw + mm2px(2.f), mm2px(3.8f), mm2px(0.6f));
			nvgFillColor(vg, nvgRGBA(0xF0, 0xF0, 0xF0, 0xE8)); nvgFill(vg);
			nvgStrokeColor(vg, PL_SOFT); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
			nvgFillColor(vg, PL_INK); nvgText(vg, x, ty, name.c_str(), NULL);
		}
		// the mics
		if (font && font->handle >= 0) {
			const char* nm[2] = {"L", "R"};
			for (int k = 0; k < 2; k++) {
				float x, y; V.project(module->micU[k], module->micV[k], x, y);
				float rr = mm2px(1.6f);
				nvgBeginPath(vg); nvgCircle(vg, x, y, rr);
				nvgFillColor(vg, nvgRGB(0xF0, 0xF0, 0xF0)); nvgFill(vg);
				nvgStrokeColor(vg, PL_INK); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, rr * 1.5f);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE); nvgFillColor(vg, PL_INK);
				nvgText(vg, x, y, nm[k], NULL);
			}
		}
		nvgRestore(vg);
		OpaqueWidget::draw(args);
	}
};

// =============================================================================
// Panel: 26HP. The grid on the faceplate, six pots over their CVs, and the
// transport row at the foot with the outputs on a plate.
// =============================================================================
static const float PL_PX[8] = {8.25f, 24.75f, 41.25f, 57.75f, 74.25f, 90.75f, 107.25f, 123.75f};
static const float PL_PY = 76.f, PL_PCV = 88.f;
static const float PL_FX[9] = {8.5f, 22.9f, 37.3f, 51.7f, 66.1f, 80.5f, 94.9f, 109.3f, 123.7f};
static const float PL_FY = 117.f;

struct PealWidget : ModuleWidget {
	PealWidget(Peal* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/peal.svg")));

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(6.f, 8.f, "PEAL");

		PealDisplay* disp = new PealDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(3.0f, 11.0f));   // to 67 mm; the pot labels sit at 71.6
		disp->box.size = mm2px(Vec(126.08f, 56.0f));
		addChild(disp);

		static const char* PN[8] = {"SIZE", "SHAPE", "BRIGHT", "DAMP", "REACH", "SPEED", "ROOT", "SCALE"};
		static const int PP[8] = {Peal::SIZE_PARAM, Peal::SHAPE_PARAM, Peal::BRIGHT_PARAM, Peal::DAMP_PARAM, Peal::REACH_PARAM, Peal::SPEED_PARAM, Peal::ROOT_PARAM, Peal::SCALE_PARAM};
		static const int PI[8] = {Peal::SIZE_INPUT, Peal::SHAPE_INPUT, Peal::BRIGHT_INPUT, Peal::DAMP_INPUT, Peal::REACH_INPUT, Peal::SPEED_INPUT, Peal::ROOT_INPUT, Peal::SCALE_INPUT};
		for (int i = 0; i < 8; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(PL_PX[i], PL_PY)), module, PP[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(PL_PX[i], PL_PCV)), module, PI[i]));
			lbl->pairDown(PL_PX[i], PL_PY, PL_PCV, PN[i]);
		}
		static const char* FN[6] = {"V/OCT", "GATE", "VEL", "STRIKE", "BELL", "CLOCK"};
		static const int FI[6] = {Peal::VOCT_INPUT, Peal::GATE_INPUT, Peal::VEL_INPUT, Peal::STRIKE_INPUT, Peal::BELL_INPUT, Peal::CLOCK_INPUT};
		for (int i = 0; i < 6; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(PL_FX[i], PL_FY)), module, FI[i]));
			lbl->jack(PL_FX[i], PL_FY, FN[i]);
		}
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(PL_FX[6], PL_FY)), module, Peal::L_OUTPUT));
		lbl->jackOnPlate(PL_FX[6], PL_FY, "L");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(PL_FX[7], PL_FY)), module, Peal::R_OUTPUT));
		lbl->jackOnPlate(PL_FX[7], PL_FY, "R");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(PL_FX[8], PL_FY)), module, Peal::POLY_OUTPUT));
		lbl->jackOnPlate(PL_FX[8], PL_FY, "POLY");
	}

	void appendContextMenu(Menu* menu) override {
		Peal* m = dynamic_cast<Peal*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		std::vector<std::string> ln;
		for (int i = 0; i < PL_NLAYOUT; i++) ln.push_back(PL_LAYOUTS[i].name);
		menu->addChild(createIndexSubmenuItem("Grid layout", ln,
			[=]() { return m->layout; }, [=](int v) { m->layout = clamp(v, 0, PL_NLAYOUT - 1); m->retune(); }));
		menu->addChild(createBoolPtrMenuItem("Lift the felts (every bell sounds, whatever the key)", "", &m->unmask));
		menu->addChild(createMenuLabel("Rods"));
		static const char* RN[7] = {"Mesh of neighbours", "Rows", "Columns", "Spiral from the centre", "A chain through every bell", "A star from the centre", "Clear"};
		for (int i = 0; i < 7; i++) { int idx = (i == 6) ? 99 : i; menu->addChild(createMenuItem(RN[i], "", [=]() { m->presetRods(idx); })); }
		menu->addChild(createMenuItem("Reset mic positions", "", [=]() { m->micU[0] = -1.5f; m->micU[1] = 1.5f; m->micV[0] = m->micV[1] = -0.85f; }));
	}
};

Model* modelPeal = createModel<Peal, PealWidget>("Peal");

// =============================================================================
// Carillon: a set of bells joined by rods. (Called Peal until 2026-09-25.)
//
// Twenty-five modal bells, one for every semitone of the two octaves round the
// root: bell n IS the note n - 12, whatever the layout, so V/OCT strikes the
// bell of that note and a rod stays joined to the same two notes when the
// bells are rearranged. The player lays rods between bells on the faceplate. A
// strike puts energy into one bell and the energy travels down the rods, some
// of it lost at every crossing and none below a floor, so velocity and REACH
// decide how far a strike ripples. A rod carries the striking bell's partials
// into the other, which answers on whatever overtones it shares. ROOT and
// SCALE do not retune the bells, they put a felt on the ones outside the key.
// A crossing takes time in proportion to the rod's LENGTH: SPEED seconds per
// neighbour spacing free-running, and with CLOCK patched one pulse per
// neighbour spacing, so the physical layout is part of the rhythm.
// Design: docs/carillon-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "scale-bus.hpp"
#include "scales.hpp"
#include "waveguide.hpp"   // sfs::softClip
#include <algorithm>
#include <cmath>
#include <vector>

static const int PL_N = 25;                      // bells: semitones -12 .. +12 from the root
static const int PL_MODES = 17;                  // partials per bell, doublets included
static const int PL_PAIRS = PL_N * PL_N;         // a rod may join ANY two bells; stored as a symmetric matrix
static const int PL_POLY = 16;
static const int PL_MAXEVENTS = 512;
static const int PL_MICDELAY = 1024;             // samples of arrival delay a mic can hold

// ── the bell ─────────────────────────────────────────────────────────────────
// A tuned carillon bell, relative to its prime (the strike note, which is the
// note the bell is named for): hum an octave below, prime, the minor-third
// tierce that makes a bell a bell, quint, nominal an octave up, then the upper
// partials a cast bell carries, which are slightly stretched and get less
// tuned the higher they go. Levels are the STRIKE levels as heard, in dB
// against the nominal (so no radiation tilt is applied to them), and T60s are
// for a bell at C4 -- a carillon's large bells ring for half a minute on the
// hum, while the partials above the nominal are gone in a few seconds, and
// that difference IS the sound: a bright clang that settles onto a long, dark,
// beating hum and tierce. The hum, prime, tierce and nominal are DOUBLETS, two
// modes a few cents apart that beat: a real bell is never quite round.
//
// IN TUNE WITH THE NOTE IT IS NAMED FOR. The ear hears a bell's strike note as
// the pitch its nominal, twelfth and upper octave (2 : 3 : 4 ...) imply, an
// octave below the nominal. The first table had those partials 34-74 cents
// sharp (4.10, 5.10, 6.12, 8.35) and every doublet's second member sharp too,
// and a melody on it read as out of tune although every prime was exact. Now
// the loud member of each doublet is exact and the weak one sits either side
// (prime flat, nominal sharp, so they cancel), and the pitch-defining upper
// partials are within 10 cents of their harmonic, stretching only at the top
// where they shimmer rather than decide the pitch. The tenth (2.515), the
// eleventh (2.667) and 5.45 are not harmonics of anything and are left alone:
// they are colour, and they are a good part of why it is a bell.
static const float PL_RATIO[PL_MODES] = {0.500f, 0.4991f, 1.000f, 0.9985f, 1.200f, 1.2033f, 1.498f,
                                         2.000f, 2.0035f, 2.515f, 2.667f, 3.000f, 4.02f, 5.02f, 5.45f, 6.03f, 8.15f};
static const float PL_DB[PL_MODES]    = {-10.f, -13.f, -8.f, -10.f, -4.f, -6.f, -18.f,
                                         0.f, -3.f, -14.f, -15.f, -6.f, -9.f, -14.f, -16.f, -15.f, -20.f};
static const float PL_T60[PL_MODES]   = {30.f, 30.f, 20.f, 20.f, 15.f, 15.f, 8.f,
                                         10.f, 10.f, 5.f, 5.f, 5.f, 3.f, 2.f, 1.6f, 1.3f, 0.8f};
// A BAR, for SHAPE to morph toward: a free metal bar's partials (1, 2.76,
// 5.40, 8.93, 13.3 ...), stretched far wider than a bell's and gone far
// sooner -- a glockenspiel or a vibraphone key. Slot for slot against the
// bell, geometric in frequency, so halfway is a set of partials halfway
// between in pitch, not two sets overlaid.
static const float PL_BAR_RATIO[PL_MODES] = {1.f, 1.0015f, 1.f, 1.002f, 2.756f, 2.760f, 5.404f,
                                             5.404f, 5.41f, 8.933f, 8.94f, 13.34f, 13.35f, 18.64f, 19.f, 24.8f, 31.f};
static const float PL_BAR_DB[PL_MODES]    = {-20.f, -24.f, 0.f, -6.f, -4.f, -8.f, -8.f,
                                             -12.f, -14.f, -12.f, -16.f, -16.f, -20.f, -22.f, -24.f, -26.f, -30.f};
static const float PL_BAR_T60[PL_MODES]   = {2.5f, 2.5f, 4.f, 4.f, 2.2f, 2.2f, 1.3f,
                                             1.3f, 1.3f, 0.8f, 0.8f, 0.45f, 0.45f, 0.3f, 0.3f, 0.2f, 0.15f};

// ── layouts ──────────────────────────────────────────────────────────────────
// Where the bells stand, as a PATH of 25 positions on the plane (u across,
// v back, in the grid's units), and a tuning ORDER that says which bell stands
// at each step of the path. Rod presets are built on the same two things, so
// "along the path" is a chain of neighbours in every layout.
enum { PL_GRID, PL_KEYS, PL_RING, PL_SPIRAL, PL_SCATTER, PL_NLAYOUT };
static const char* PL_LAYOUT_NAME[PL_NLAYOUT] = {
	"Grid, five by five", "Keyboard (naturals in front, sharps behind)", "Ring",
	"Spiral (the low bells in the middle)", "Scatter"};
enum { PL_ORD_PITCH, PL_ORD_FIFTHS, PL_ORD_SHUFFLE, PL_NORDER };
static const char* PL_ORDER_NAME[PL_NORDER] = {"By pitch", "By fifths (every step a fifth or a fourth)", "Shuffled"};
// every one of the 25 semitones once, each step a fifth or a fourth, never
// leaving the two octaves (a Hamiltonian path, found once and written down)
static const int PL_FIFTHS[PL_N] = {0, 7, 14, 21, 16, 23, 18, 13, 20, 15, 22, 17, 24, 19, 12, 5, 10, 3, 8, 1, 6, 11, 4, 9, 2};

// SPEED: clockwise is faster. 4 s per neighbour spacing at the bottom, 10 ms
// at the top, 360 ms at the default. A longer rod takes proportionally longer.
static inline float carillonSpeedSec(float k) { return 4.f * std::pow(0.0025f, clamp(k, 0.f, 1.f)); }
// REACH is how much of a bell's energy crosses its rods: none at the bottom
// (the struck bell alone), 95% at the top, SPLIT between the rods so a mesh
// cannot multiply energy. It was a floor below which nothing crossed, and
// that knob spent its travel on tails too quiet to hear: at its bottom a
// strike still went five bells. Squared, so the lower half is the part where
// a ripple of two or three bells lives.
static inline float carillonTrans(float k)    { float c = clamp(k, 0.f, 1.f); return 0.95f * c * c; }
static const float PL_FLOOR = 2e-4f;
static const float PL_OUT_GAIN = 1.6f;           // a middle bell struck at full velocity peaks near 3.5 V             // an arrival below this is silent (-37 dB on a full strike) and stops

static const char* PL_NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
struct CarillonRootQ : ParamQuantity {
	std::string getDisplayValueString() override { return PL_NOTE[clamp((int)std::round(getValue()), 0, 11)]; }
};
struct CarillonSpeedQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float s = carillonSpeedSec(getValue());
		return s < 1.f ? string::f("%.0f ms per neighbour spacing", s * 1000.f) : string::f("%.2f s per neighbour spacing", s);
	}
};
// SIZE moves the bells by WHOLE OCTAVES, and everything else about size
// continuously. It used to transpose continuously too, so anywhere but 12
// o'clock every bell was out of tune with the rest of the patch (120 cents
// at 45%). A set of bells is cast to a pitch; what a bigger set changes, at
// the same notes, is how long they ring and how heavy the clapper is.
static inline int carillonSizeOct(float k) { return k < 1.f / 3.f ? 1 : (k < 2.f / 3.f ? 0 : -1); }
struct CarillonSizeQ : ParamQuantity {
	std::string getDisplayValueString() override {
		static const char* reg[3] = {"large bells, C2 to C4", "bells as cast, C3 to C5", "hand bells, C4 to C6"};
		return string::f("%.0f%%, %s", getValue() * 100.f, reg[carillonSizeOct(getValue()) + 1]);
	}
};
// DAMP is a RING-TIME MULTIPLIER, log-linear across the knob: 4x the bell's
// natural ring at the bottom (orchestral bell plates and tubular bells hang
// free and ring for most of a minute; "no hand on it" was the old floor and
// it was still too short for them), exactly as cast at 27%, the old default's
// ring time at the default 36%, and the old maximum, a hand flat on every
// bell, at the top.
static inline float carillonRing(float k) { return 4.f * std::pow(1.f / 164.f, clamp(k, 0.f, 1.f)); }
struct CarillonDampQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float r = carillonRing(getValue());
		if (std::fabs(r - 1.f) < 0.04f) return "rings as cast";
		return r > 1.f ? string::f("rings %.1fx as long as cast", r) : string::f("rings %.2fx as long as cast (a hand on it)", r);
	}
};
struct CarillonReachQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float t = carillonTrans(getValue());
		return t <= 0.f ? "nothing crosses: the struck bell alone" : string::f("%.0f%% of the energy crosses, split between the rods", t * 100.f);
	}
};

struct Carillon : Module {
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
		float semis = 0.f;                            // from the root: n - 12, always
		bool  masked = false;                         // a felt on it
		bool  quiet = true;                           // rung out: skipped until struck
		float energy = 0.f;                           // follower, for the rods and the picture
		float out = 0.f;                              // this sample's output
		float glow = 0.f;                             // the picture: struck recently
		float clap = 0.f;                             // the clapper: a short, bright contact noise
		float clapLp = 0.f;
		float mallet = 0.f, malletT = 0.f, malletDur = 0.003f;   // the contact: force spread over a few ms, not a click
		float malletW[PL_MODES] = {};
		float u = 0.f, v = 0.f;                       // where it stands on the plane
	};
	Bell bell[PL_N];
	bool  rodM[PL_PAIRS] = {};                        // rodM[a*25+b], kept symmetric
	float rodGlow[PL_PAIRS] = {};                     // the picture: energy that crossed recently
	bool hasRod(int a, int b) const { return a != b && rodM[a * PL_N + b]; }
	void setRod(int a, int b, bool on) { if (a == b || a < 0 || b < 0) return; rodM[a * PL_N + b] = rodM[b * PL_N + a] = on; }
	int  degree(int i) const { int d = 0; for (int j = 0; j < PL_N; j++) if (hasRod(i, j)) d++; return d; }
	void clearRods() { for (int k = 0; k < PL_PAIRS; k++) rodM[k] = false; }
	int   layout = PL_GRID, order = PL_ORD_PITCH;
	int   path[PL_N];                                 // the bell at each step of the layout's path
	float spacing = 1.f;                              // the layout's typical neighbour distance
	int   layoutRoot = -1;                            // the root the keyboard was laid out for
	bool  unmask = false;                             // lift the felts

	// energy in flight: an arrival at a bell, due at a time (free) or after a number of pulses (clocked)
	struct Event { int bell; int from; float energy; double due; double start; long pulseAt; int pulses; };
	Event ev[PL_MAXEVENTS]; int nev = 0;
	double now = 0.0;
	bool clocked = false;
	long   pulseCount = 0;                            // pulses seen, so a crossing waits for the NEXT one
	double lastPulse = -1.0, clockPeriod = 0.5;       // for the picture: how far along a rod an impulse is
	dsp::SchmittTrigger clockTrig, strikeTrig;
	bool gateWas[PL_POLY] = {};

	// mics on the plane, in grid units
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
	float sizeK = 1.f, dampK = 0.f, transK = 0.2f, shapeK = 0.f, brightK = 0.5f;
	float modeRatio[PL_MODES] = {}, modeAmp[PL_MODES] = {}, modeT60[PL_MODES] = {};   // the set in force, bell morphed toward bar
	void morph() {
		// BRIGHT tilts the partials about the nominal: a soft mallet to a hard one
		float tilt = (brightK - 0.5f) * 2.f;
		for (int m = 0; m < PL_MODES; m++) {
			modeRatio[m] = PL_RATIO[m] * std::pow(PL_BAR_RATIO[m] / PL_RATIO[m], shapeK);
			float db = PL_DB[m] + (PL_BAR_DB[m] - PL_DB[m]) * shapeK;
			// The bell's table is as HEARD; the bar's is displacement, so the
			// bar alone takes the radiation tilt (a struck body radiates its
			// high modes more efficiently, roughly as the square root of
			// frequency) -- without it a bar sat on its fundamental and was muffled.
			float rad = 0.5f * shapeK + tilt;
			modeAmp[m] = std::pow(10.f, db / 20.f) * std::pow(std::max(modeRatio[m], 0.5f) / 2.f, rad);
			modeT60[m] = PL_T60[m] * std::pow(PL_BAR_T60[m] / PL_T60[m], shapeK);
		}
	}

	Carillon() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<CarillonSizeQ>(SIZE_PARAM, 0.f, 1.f, 0.5f, "Size (hand bells to a carillon: octaves for pitch, ring time and clapper weight in between)");
		configParam<CarillonDampQ>(DAMP_PARAM, 0.f, 1.f, 0.36f, "Damp (free-hanging plates at the bottom, a hand on every bell at the top)");
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.f, "Shape (a cast bell to a metal bar)", "%", 0.f, 100.f);
		configParam(BRIGHT_PARAM, 0.f, 1.f, 0.5f, "Bright (a soft clapper to a hard one, and the tilt of the partials)", "%", 0.f, 100.f);
		configInput(SHAPE_INPUT, "Shape CV");
		configInput(BRIGHT_INPUT, "Bright CV");
		configParam<CarillonReachQ>(REACH_PARAM, 0.f, 1.f, 0.5f, "Reach (how much of the energy crosses a rod)");
		configParam<CarillonSpeedQ>(SPEED_PARAM, 0.f, 1.f, 0.4f, "Speed (how fast energy crosses a rod; with CLOCK, one pulse per neighbour spacing)");
		configParam<CarillonRootQ>(ROOT_PARAM, 0.f, 11.f, 0.f, "Root");
		paramQuantities[ROOT_PARAM]->snapEnabled = true;
		std::vector<std::string> names;
		for (int i = 0; i < sfs::NUM_SCALES; i++) names.push_back(sfs::SCALES[i].longName);
		configSwitch(SCALE_PARAM, 0.f, (float)(sfs::NUM_SCALES - 1), 1.f, "Scale (bells outside it are felted)", names);
		configInput(VOCT_INPUT, "V/OCT (poly): strikes the bell of that note, folded by octaves into the two octaves round the root");
		configInput(GATE_INPUT, "Gate (poly)");
		configInput(VEL_INPUT, "Velocity (poly, 0-10V): how much energy the strike delivers");
		configInput(STRIKE_INPUT, "Strike trigger");
		configInput(BELL_INPUT, "Bell (1V/oct) for the Strike trigger");
		configInput(CLOCK_INPUT, "Clock: with it patched, a crossing is one pulse per neighbour spacing of rod");
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
		for (int i = 0; i < PL_N; i++) bell[i].semis = (float)(i - 12);
		morph();
		relayout();
		presetRods(0);       // something to start from: a chain along the path
	}

	// ── where the bells stand ─────────────────────────────────────────────
	// Positions for the 25 steps of the layout's path, then the tuning order
	// puts a bell at each step. The keyboard is the exception: a key's place
	// IS its note, so it has no tuning order and follows the root.
	void relayout() {
		float pu[PL_N], pv[PL_N];
		int  keyOrder[PL_N];
		bool keys = layout == PL_KEYS;
		switch (layout) {
			default:
			case PL_GRID:
				// front row first (the low, large bells nearest), snaking, so
				// consecutive steps are always neighbours
				for (int k = 0; k < PL_N; k++) {
					int r = k / 5, c = (r % 2 == 0) ? k % 5 : 4 - k % 5;
					pu[k] = (float)c - 2.f; pv[k] = (float)r;
				}
				break;
			case PL_KEYS: {
				// a carillon's clavier: naturals along the front, sharps behind
				// between their neighbours, low on the left
				int nNat = 0;
				bool nat[PL_N];
				for (int b = 0; b < PL_N; b++) {
					int pc = ((root + b - 12) % 12 + 12) % 12;
					nat[b] = pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11;
					if (nat[b]) nNat++;
				}
				float step = 4.8f / (float)std::max(nNat - 1, 1);
				float natU[PL_N]; int j = 0;
				for (int b = 0; b < PL_N; b++) if (nat[b]) natU[b] = -2.4f + step * (float)j++;
				for (int b = 0; b < PL_N; b++) {
					keyOrder[b] = b;
					if (nat[b]) { pu[b] = natU[b]; pv[b] = 0.3f; continue; }
					// between the naturals either side (a sharp at an end sits half a step out)
					float lo = b > 0 && nat[b - 1] ? natU[b - 1] : -2.4f - step;
					float hi = b + 1 < PL_N && nat[b + 1] ? natU[b + 1] : lo + 2.f * step;
					if (b == 0) lo = hi - 2.f * step;
					pu[b] = 0.5f * (lo + hi); pv[b] = 2.1f;
				}
				break;
			}
			case PL_RING:
				for (int k = 0; k < PL_N; k++) {
					float a = -(float)M_PI * 0.5f - 2.f * (float)M_PI * (float)k / (float)PL_N;
					pu[k] = 2.0f * std::cos(a); pv[k] = 2.f + 1.9f * std::sin(a);
				}
				break;
			case PL_SPIRAL:
				// an Archimedean spiral walked at equal arc steps, with the arms
				// as far apart as the steps, so consecutive bells are neighbours
				// and the path winds out from the middle (a sunflower spiral packs
				// as evenly, but its consecutive points are a golden angle apart
				// and a chain along it was a tangle)
				for (int k = 0; k < PL_N; k++) {
					float th = std::sqrt(4.f * (float)M_PI * ((float)k + 0.5f));
					float r = 0.7f / (2.f * (float)M_PI) * th;
					pu[k] = r * std::cos(th) * 1.15f; pv[k] = 2.f + r * std::sin(th);
				}
				break;
			case PL_SCATTER: {
				// seeded dart-throwing, so it is the same scatter every time,
				// then the path runs left to right
				uint32_t s = 0x2545F491u;
				auto rnd = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float)(s >> 8) / 16777216.f; };
				int n = 0;
				for (int tries = 0; n < PL_N && tries < 20000; tries++) {
					float u = -2.4f + 4.8f * rnd(), v = 0.15f + 3.7f * rnd();
					bool ok = true;
					for (int q = 0; q < n && ok; q++) ok = std::hypot(u - pu[q], v - pv[q]) > 0.72f;
					if (ok) { pu[n] = u; pv[n] = v; n++; }
				}
				for (; n < PL_N; n++) { pu[n] = -2.4f + 0.2f * n; pv[n] = 2.f; }
				for (int a = 1; a < PL_N; a++)                 // left to right
					for (int b = a; b > 0 && pu[b] < pu[b - 1]; b--) { std::swap(pu[b], pu[b - 1]); std::swap(pv[b], pv[b - 1]); }
				break;
			}
		}
		// the tuning order: which bell stands at each step of the path
		if (keys) for (int k = 0; k < PL_N; k++) path[k] = keyOrder[k];
		else if (order == PL_ORD_FIFTHS) for (int k = 0; k < PL_N; k++) path[k] = PL_FIFTHS[k];
		else if (order == PL_ORD_SHUFFLE) {
			for (int k = 0; k < PL_N; k++) path[k] = k;
			uint32_t s = 0x9E3779B9u;
			for (int k = PL_N - 1; k > 0; k--) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; std::swap(path[k], path[(int)(s % (uint32_t)(k + 1))]); }
		} else for (int k = 0; k < PL_N; k++) path[k] = k;
		for (int k = 0; k < PL_N; k++) { bell[path[k]].u = pu[k]; bell[path[k]].v = pv[k]; }
		// the typical neighbour distance: the median of every bell's nearest
		float nn[PL_N];
		for (int a = 0; a < PL_N; a++) {
			nn[a] = 1e9f;
			for (int b = 0; b < PL_N; b++) if (a != b) nn[a] = std::min(nn[a], std::hypot(bell[a].u - bell[b].u, bell[a].v - bell[b].v));
		}
		std::sort(nn, nn + PL_N);
		spacing = std::max(nn[PL_N / 2], 0.05f);
		layoutRoot = root;
		placeMics();
	}
	// a rod's length, in neighbour spacings: the unit a crossing is timed in
	float rodLen(int a, int b) const { return std::hypot(bell[a].u - bell[b].u, bell[a].v - bell[b].v) / spacing; }
	int   rodPulses(int a, int b) const { return std::max(1, (int)std::lround(rodLen(a, b))); }

	// a bell's prime, in Hz: the root's C4 octave, moved by SIZE in whole octaves only
	float primeHz(int i) const {
		return dsp::FREQ_C4 * std::pow(2.f, ((float)root + bell[i].semis) / 12.f + (float)carillonSizeOct(sizeK));
	}
	void updateBell(int i) {
		Bell& b = bell[i];
		float f0 = primeHz(i);
		// smaller bells decay faster: T60 scales with the prime's period, up to
		// a point, and a bigger set rings longer at the same notes
		float sizeT = clamp(261.6f / f0, 0.25f, 4.f) * std::pow(2.f, 1.4f * (sizeK - 0.5f));
		// DAMP: free-hanging at the bottom, a hand on the bell at the top (carillonRing)
		float ring = carillonRing(dampK);
		float damp = 1.f / ring;
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
		// THE CONTACT, NOT A CLICK. An iron clapper stays on the bronze for a
		// millisecond or so, longer on a big bell, and the force it delivers
		// is a raised cosine over that time: a contact of T strikes almost
		// nothing above 2/T. 1 ms at C4 at the default BRIGHT, scaled by the
		// bell's size; a bar is struck with a much harder head, so SHAPE
		// shortens the contact as well.
		b.mallet = amp;
		b.malletT = 0.f;
		b.malletDur = clamp(0.0008f * 261.6f / std::max(b.hz[2], 40.f) * (2.4f - 2.2f * brightK) * (1.f - 0.85f * shapeK), 0.0001f, 0.006f);
		// the clapper's own contact noise: small, a few milliseconds, and a
		// rod arrives with far less of it than a clapper does
		b.clap = std::max(b.clap, amp * (from >= 0 ? 0.04f : 0.14f) * (0.3f + 0.9f * brightK));
		b.glow = 1.f;
		b.quiet = false;
	}
	// a strike, and what it sends down the rods
	void strike(int i, float energy, int from) {
		if (i < 0 || i >= PL_N || energy <= 0.f) return;
		excite(i, energy, from);
		// OUTWARD: energy leaves by every rod but the one it came in on. Sent
		// back as well, half of every crossing on a chain returned to where it
		// had been and the ripple read as an echo between two bells rather than
		// as distance travelled.
		int outs = degree(i) - (from >= 0 && hasRod(i, from) ? 1 : 0);
		if (outs <= 0) return;
		// LOSSY, and CONSERVED: what leaves is REACH's fraction of what
		// arrived, split between the rods, so a mesh cannot multiply energy.
		// Below the floor an arrival would be inaudible, and there a ripple ends.
		float send = energy * transK / (float)outs;
		if (send < PL_FLOOR) return;
		float speedK = clamp(params[SPEED_PARAM].getValue() + inputs[SPEED_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		for (int other = 0; other < PL_N; other++) {
			if (other == from || !hasRod(i, other) || nev >= PL_MAXEVENTS) continue;
			// A LONGER ROD TAKES LONGER, clocked as well: a crossing is one pulse
			// per neighbour spacing of rod, at least one. A rod laid across the
			// whole layout is a long wait, which makes the layout part of the rhythm.
			int pulses = rodPulses(i, other);
			ev[nev++] = {other, i, send, clocked ? 0.0 : now + carillonSpeedSec(speedK) * rodLen(i, other),
			             now, pulseCount + pulses - 1, pulses};
			rodGlow[i * PL_N + other] = rodGlow[other * PL_N + i] = 1.f;
		}
	}
	// Rod presets. Built on the layout's path and on the bells' pitches, so
	// every one of them means the same thing in every layout.
	enum { ROD_PATH, ROD_MESH, ROD_STAR, ROD_OCTAVES, ROD_FIFTHS, ROD_CLEAR };
	void presetRods(int which) {
		clearRods();
		switch (which) {
			case ROD_PATH: for (int k = 0; k + 1 < PL_N; k++) setRod(path[k], path[k + 1], true); break;
			case ROD_MESH:                                                              // every bell to its near neighbours
				for (int a = 0; a < PL_N; a++) for (int b = a + 1; b < PL_N; b++)
					if (rodLen(a, b) <= 1.45f) setRod(a, b, true);
				break;
			case ROD_STAR: {                                                            // the bell nearest the middle to eight spread round it
				float cu = 0.f, cv = 0.f;
				for (int i = 0; i < PL_N; i++) { cu += bell[i].u; cv += bell[i].v; }
				cu /= PL_N; cv /= PL_N;
				int c = 0; float cd = 1e9f;
				for (int i = 0; i < PL_N; i++) { float d = std::hypot(bell[i].u - cu, bell[i].v - cv); if (d < cd) { cd = d; c = i; } }
				for (int k = 0; k < 8; k++) {
					float a = 2.f * (float)M_PI * (float)k / 8.f;
					int best = -1; float bd = 1e9f;
					for (int i = 0; i < PL_N; i++) {
						if (i == c || hasRod(c, i)) continue;
						float d = std::hypot(bell[i].u - (cu + 2.2f * std::cos(a)), bell[i].v - (cv + 2.2f * std::sin(a)));
						if (d < bd) { bd = d; best = i; }
					}
					setRod(c, best, true);
				}
				break;
			}
			case ROD_OCTAVES: for (int b = 0; b + 12 < PL_N; b++) setRod(b, b + 12, true); break;
			case ROD_FIFTHS:  for (int b = 0; b + 7 < PL_N; b++) setRod(b, b + 7, true); break;
			default: break;                                                             // clear
		}
	}
	// V/OCT to a bell: the note, folded by octaves into the two octaves round
	// the root, and THAT bell, felted or not. It used to take the nearest
	// bell, skipping felted ones -- and the old layouts left some notes with
	// no bell at all, so two notes of a melody could strike the same bell.
	int bellForSemis(float semis) const {
		int s = (int)std::lround(semis);
		while (s > 12) s -= 12;
		while (s < -12) s += 12;
		return s + 12;
	}

	// ── the mics ────────────────────────────────────────────────────────
	void placeMics() {
		for (int k = 0; k < 2; k++)
			for (int i = 0; i < PL_N; i++) {
				float d = std::hypot(bell[i].u - micU[k], bell[i].v - micV[k]);
				// a grid unit is about 30 cm: 0.9 ms of air per unit
				micLag[k][i] = clamp((int)(d * 0.0009f * sr), 0, PL_MICDELAY - 1);
				// Half the inverse-distance law in dB. The full law made a
				// front-row bell 2.4x louder than one in the middle, so where a
				// bell stood decided its level more than how hard it was struck,
				// and a front-row triad drove the output into its clamp. The
				// arrival time still carries the image.
				micGain[k][i] = 1.f / std::sqrt(1.f + d);
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
			if (layout == PL_KEYS && root != layoutRoot) relayout();   // the keyboard follows the root
			sc = sfs::busResolve(inputs[SCALE_INPUT], (int)std::round(params[SCALE_PARAM].getValue()));
			sizeK = clamp(params[SIZE_PARAM].getValue() + inputs[SIZE_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
			dampK = clamp(params[DAMP_PARAM].getValue() + inputs[DAMP_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
			transK = carillonTrans(params[REACH_PARAM].getValue() + inputs[REACH_INPUT].getVoltage() * 0.2f);
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
				strike(bellForSemis(semis), vel * vel * vel * 4.f, -1);
			}
			gateWas[c] = g;
		}
		if (strikeTrig.process(inputs[STRIKE_INPUT].getVoltage(), 0.1f, 1.f)) {
			float semis = inputs[BELL_INPUT].getVoltage() * 12.f - (float)root;
			float vel = inputs[VEL_INPUT].isConnected() ? clamp(inputs[VEL_INPUT].getVoltage() * 0.1f, 0.f, 1.f) : 0.8f;
			strike(bellForSemis(semis), vel * vel * vel * 4.f, -1);
		}

		// ── energy arriving ───────────────────────────────────────────────
		// Free-running, an event lands when its time comes. Clocked, it lands
		// on its pulse: the NEXT pulse for a neighbour -- not one on the same
		// sample as the strike, or a sequencer clocked from the same source
		// struck and crossed at once -- and one more per spacing of rod.
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
			if (b.quiet) {                                // rung out: nothing to compute, only the delay lines to feed
				for (int k = 0; k < 2; k++) {
					micDelay[k][i][micHead] = 0.f;
					int at = (micHead - micLag[k][i] + PL_MICDELAY) % PL_MICDELAY;
					if (k == 0) outL += micDelay[k][i][at]; else outR += micDelay[k][i][at];
				}
				b.glow *= 0.9995f;
				continue;
			}
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
			// the clapper: a little contact noise above the partials, gone in a
			// couple of milliseconds
			if (b.clap > 1e-4f) {
				float nz = random::uniform() * 2.f - 1.f;
				b.clapLp += (nz - b.clapLp) * 0.55f;
				s += (nz - b.clapLp) * b.clap * 0.6f;
				b.clap *= 0.997f;
			}
			b.out = s * 0.45f;
			b.energy += (e - b.energy) * 0.002f;
			b.glow *= 0.9995f;
			// rung out: -90 dB on a full strike, and the contact long over
			if (b.energy < 1e-9f && b.mallet <= 0.f && b.clap <= 1e-4f) {
				b.quiet = true;
				for (int m = 0; m < PL_MODES; m++) b.re[m] = b.im[m] = 0.f;
				b.out = 0.f;
			}
			for (int k = 0; k < 2; k++) {
				micDelay[k][i][micHead] = b.out * micGain[k][i];
				int at = (micHead - micLag[k][i] + PL_MICDELAY) % PL_MICDELAY;
				if (k == 0) outL += micDelay[k][i][at]; else outR += micDelay[k][i][at];
			}
		}
		micHead = (micHead + 1) % PL_MICDELAY;
		for (int k = 0; k < PL_PAIRS; k++) rodGlow[k] *= 0.9993f;

		// Soft-clipped, linear to 6 V and asymptotic to 10, not clamped: a
		// struck chord's crest is far above its body, and a clamp there is the
		// distortion you hear.
		outputs[L_OUTPUT].setVoltage(sfs::softClip(outL * PL_OUT_GAIN));
		outputs[R_OUTPUT].setVoltage(sfs::softClip(outR * PL_OUT_GAIN));
		outputs[POLY_OUTPUT].setChannels(PL_POLY);
		for (int c = 0; c < PL_POLY; c++)
			outputs[POLY_OUTPUT].setVoltage(chanBell[c] >= 0 ? sfs::softClip(bell[chanBell[c]].out * 4.f) : 0.f, c);
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_t* rods = json_array();                  // pairs of BELLS (notes), a < b
		for (int a = 0; a < PL_N; a++) for (int b = a + 1; b < PL_N; b++)
			if (hasRod(a, b)) { json_t* pr = json_array(); json_array_append_new(pr, json_integer(a)); json_array_append_new(pr, json_integer(b)); json_array_append_new(rods, pr); }
		json_object_set_new(r, "rods", rods);
		json_object_set_new(r, "layout", json_integer(layout));
		json_object_set_new(r, "order", json_integer(order));
		json_object_set_new(r, "unmask", json_boolean(unmask));
		json_t* mics = json_array();
		for (int k = 0; k < 2; k++) { json_array_append_new(mics, json_real(micU[k])); json_array_append_new(mics, json_real(micV[k])); }
		json_object_set_new(r, "mics", mics);
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "layout")) layout = clamp((int)json_integer_value(j), 0, PL_NLAYOUT - 1);
		if (json_t* j = json_object_get(r, "order")) order = clamp((int)json_integer_value(j), 0, PL_NORDER - 1);
		if (json_t* j = json_object_get(r, "unmask")) unmask = json_boolean_value(j);
		if (json_t* mics = json_object_get(r, "mics"))
			if (json_array_size(mics) == 4) for (int k = 0; k < 2; k++) {
				micU[k] = clamp((float)json_real_value(json_array_get(mics, 2 * k)), -3.5f, 3.5f);
				micV[k] = clamp((float)json_real_value(json_array_get(mics, 2 * k + 1)), -2.5f, 5.5f);
			}
		relayout();
		// rods are stored by note; a patch from before the bells were notes
		// ("rodPairs", grid positions) cannot be translated, so it keeps the
		// default chain
		if (json_t* rods = json_object_get(r, "rods")) {
			clearRods();
			for (int k = 0; k < (int)json_array_size(rods); k++) {
				json_t* pr = json_array_get(rods, k);
				if (json_array_size(pr) == 2) setRod(clamp((int)json_integer_value(json_array_get(pr, 0)), 0, PL_N - 1), clamp((int)json_integer_value(json_array_get(pr, 1)), 0, PL_N - 1), true);
			}
		}
	}
};

// =============================================================================
// The bells, drawn on the faceplate. No screen: the panel's greys, orange for
// energy, as Wheel and Flock do. The camera stands a little above the front
// edge and looks down across the plane, so the back is smaller and closer
// together than the front. Each bell is a circle, larger for a lower note,
// that fills with orange when struck and throws a ring while it sounds; a
// felted bell is hollow.
//
// The projection is a plane in perspective with a closed-form inverse, so
// the hit-test is the same projection run backwards.
// =============================================================================
static const float PL_K = 0.16f;                 // foreshortening per unit of depth
static const float PL_VBACK = 4.f, PL_VFRONT = -1.15f;  // the plane's visible depth range

struct CarillonView {
	float w, h, cx, yFront, yBack, S;
	CarillonView(float w_, float h_) : w(w_), h(h_) {
		cx = w * 0.5f;
		yFront = h * 0.97f; yBack = h * 0.09f;
		S = w * 0.205f;                              // px per grid unit at the front
	}
	float depthScale(float v) const { return 1.f / (1.f + PL_K * (v - PL_VFRONT)); }
	// how big something at depth v is drawn, against the front
	float persp(float v) const { return depthScale(v) / depthScale(PL_VFRONT); }
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
// a bell's radius on the panel: larger for a lower note, smaller with distance,
// and a little smaller in a crowded layout
static inline float carillonRadius(const CarillonView& V, const Carillon& m, int i) {
	float base = mm2px(1.9f + 1.7f * (12.f - m.bell[i].semis) / 24.f);
	float crowd = clamp(m.spacing * V.S * 0.42f / mm2px(3.6f), 0.55f, 1.f);
	return base * crowd * V.persp(m.bell[i].v);
}

static Carillon* carillonPreview() {
	static Carillon* m = nullptr;
	if (m) return m;
	m = new Carillon();
	m->presetRods(Carillon::ROD_MESH);
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

struct CarillonDisplay : OpaqueWidget {
	Carillon* module = nullptr;
	std::shared_ptr<Font> font;
	int dragBell = -1, dragMic = -1;                 // what the press landed on
	int hoverBell = -1;
	int lastBell = -1;                                // the bell the drag last passed over
	bool dragPainted = false;
	Vec dragStart, dragPos;

	// THE BELL UNDER THE POINTER: of the bells whose circle (a little enlarged)
	// holds the point, the one it is most nearly central in. The hanging bells
	// were tested at their FEET, below where they were drawn, so the body of a
	// back-row bell sat over the front bell's hit point and could not be hovered.
	int bellAt(Vec p) const {
		if (!module) return -1;
		CarillonView V(box.size.x, box.size.y);
		int best = -1; float bd = 1e9f;
		for (int i = 0; i < PL_N; i++) {
			float x, y; V.project(module->bell[i].u, module->bell[i].v, x, y);
			float r = std::max(carillonRadius(V, *module, i) * 1.25f, mm2px(2.f));
			float d = std::hypot(p.x - x, p.y - y) / r;
			if (d < 1.f && d < bd) { bd = d; best = i; }
		}
		return best;
	}
	int micAt(Vec p) const {
		if (!module) return -1;
		CarillonView V(box.size.x, box.size.y);
		for (int k = 0; k < 2; k++) {
			float x, y; V.project(module->micU[k], module->micV[k], x, y);
			if (std::hypot(p.x - x, p.y - y) < mm2px(2.2f)) return k;
		}
		return -1;
	}
	// DRAW THE RODS BY DRAGGING. Press on a bell and release on ANY other and a
	// rod joins them; do it again and the rod goes. A click strikes.
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
			CarillonView V(box.size.x, box.size.y);
			float u, v; V.unproject(e.pos.x, e.pos.y, u, v);
			module->micU[dragMic] = clamp(u, -3.5f, 3.5f);
			module->micV[dragMic] = clamp(v, -2.5f, 5.5f);
			return;
		}
		if (dragBell < 0) return;
		lastBell = bellAt(e.pos);
		if (lastBell >= 0 && lastBell != dragBell) dragPainted = true;   // the pointer has been on another bell: a rod, not a strike
	}
	void onDragEnd(const DragEndEvent& e) override {
		if (!module) return;
		if (dragMic >= 0) { dragMic = -1; return; }
		if (dragBell < 0) return;
		int end = bellAt(dragPos);
		if (end >= 0 && end != dragBell) {
			module->setRod(dragBell, end, !module->hasRod(dragBell, end));
		} else if (!dragPainted) {
			// a click strikes it, harder toward the top of the circle
			CarillonView V(box.size.x, box.size.y);
			float x, y; V.project(module->bell[dragBell].u, module->bell[dragBell].v, x, y);
			float r = carillonRadius(V, *module, dragBell);
			float vel = clamp(0.65f + 0.35f * (y - dragStart.y) / std::max(r, 1.f), 0.25f, 1.f);
			module->strike(dragBell, vel * vel * vel * 4.f, -1);
		}
		dragBell = -1; lastBell = -1; dragPainted = false;
	}

	void draw(const DrawArgs& args) override {
		if (!module) { module = carillonPreview(); draw(args); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		CarillonView V(box.size.x, box.size.y);
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
		auto bellXY = [&](int i, float& x, float& y) { V.project(module->bell[i].u, module->bell[i].v, x, y); };
		// rods, glowing with the energy that crossed
		for (int a = 0; a < PL_N; a++) for (int b = a + 1; b < PL_N; b++) {
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
		// the head brightest, at its share of the crossing: free-running of its
		// time, clocked of its pulses at the last clock period
		{
			int n = std::min(module->nev, PL_MAXEVENTS);
			for (int k = 0; k < n; k++) {
				const Carillon::Event& e = module->ev[k];
				if (e.from < 0 || e.bell < 0) continue;
				double dur = module->clocked ? module->clockPeriod * e.pulses : std::max(e.due - e.start, 1e-3);
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
		// bells, far first so a near one paints over a far one
		int ord[PL_N];
		for (int i = 0; i < PL_N; i++) ord[i] = i;
		std::sort(ord, ord + PL_N, [&](int a, int b) { return module->bell[a].v > module->bell[b].v; });
		for (int oi = 0; oi < PL_N; oi++) {
			int i = ord[oi];
			const Carillon::Bell& b = module->bell[i];
			float x, y; bellXY(i, x, y);
			float r = carillonRadius(V, *module, i);
			float lvl = clamp(std::sqrt(b.energy) * 3.f, 0.f, 1.f);
			if (lvl > 0.02f) {
				// the ring of sound, wider the louder it sounds
				nvgBeginPath(vg); nvgCircle(vg, x, y, r * (1.25f + 0.9f * lvl));
				nvgStrokeColor(vg, nvgTransRGBA(PL_HOT, (unsigned char)(170 * lvl))); nvgStrokeWidth(vg, 0.7f + 1.3f * lvl); nvgStroke(vg);
			}
			nvgBeginPath(vg); nvgCircle(vg, x, y, r);
			if (b.masked) {
				nvgFillColor(vg, nvgRGB(0xF0, 0xF0, 0xF0)); nvgFill(vg);
				nvgStrokeColor(vg, PL_SOFT); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			} else {
				float g = clamp(b.glow, 0.f, 1.f);
				nvgFillColor(vg, nvgRGBAf(PL_INK.r + (PL_HOT.r - PL_INK.r) * g, PL_INK.g + (PL_HOT.g - PL_INK.g) * g, PL_INK.b + (PL_HOT.b - PL_INK.b) * g, 1.f));
				nvgFill(vg);
			}
			if (i == hoverBell) {
				nvgBeginPath(vg); nvgCircle(vg, x, y, r + mm2px(0.5f));
				nvgStrokeColor(vg, PL_HOT); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			}
		}
		// THE NOTE UNDER THE POINTER: above the bell, or below it where above
		// would leave the display (the back row's labels were drawn off the top)
		if (hoverBell >= 0 && font && font->handle >= 0) {
			const Carillon::Bell& b = module->bell[hoverBell];
			float hz = module->primeHz(hoverBell);
			int midi = (int)std::round(69.f + 12.f * std::log2(hz / 440.f));
			std::string name = string::f("%s%d", PL_NOTE[((midi % 12) + 12) % 12], midi / 12 - 1);
			if (b.masked) name += "  felted";
			float x, y; bellXY(hoverBell, x, y);
			float r = carillonRadius(V, *module, hoverBell);
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, mm2px(2.6f));
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
			float tw = nvgTextBounds(vg, 0, 0, name.c_str(), NULL, NULL);
			float ty = y - r - mm2px(1.2f);
			if (ty - mm2px(3.4f) < 0.f) ty = y + r + mm2px(4.4f);
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
// Panel: 26HP. The bells on the faceplate, eight pots over their CVs, and the
// transport row at the foot with the outputs on a plate.
// =============================================================================
static const float PL_PX[8] = {8.25f, 24.75f, 41.25f, 57.75f, 74.25f, 90.75f, 107.25f, 123.75f};
static const float PL_PY = 76.f, PL_PCV = 88.f;
static const float PL_FX[9] = {8.5f, 22.9f, 37.3f, 51.7f, 66.1f, 80.5f, 94.9f, 109.3f, 123.7f};
static const float PL_FY = 117.f;

struct CarillonWidget : ModuleWidget {
	CarillonWidget(Carillon* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/carillon.svg")));

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(6.f, 8.f, "CARILLON");

		CarillonDisplay* disp = new CarillonDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(3.0f, 11.0f));   // to 67 mm; the pot labels sit at 71.6
		disp->box.size = mm2px(Vec(126.08f, 56.0f));
		addChild(disp);

		static const char* PN[8] = {"SIZE", "SHAPE", "BRIGHT", "DAMP", "REACH", "SPEED", "ROOT", "SCALE"};
		static const int PP[8] = {Carillon::SIZE_PARAM, Carillon::SHAPE_PARAM, Carillon::BRIGHT_PARAM, Carillon::DAMP_PARAM, Carillon::REACH_PARAM, Carillon::SPEED_PARAM, Carillon::ROOT_PARAM, Carillon::SCALE_PARAM};
		static const int PI[8] = {Carillon::SIZE_INPUT, Carillon::SHAPE_INPUT, Carillon::BRIGHT_INPUT, Carillon::DAMP_INPUT, Carillon::REACH_INPUT, Carillon::SPEED_INPUT, Carillon::ROOT_INPUT, Carillon::SCALE_INPUT};
		for (int i = 0; i < 8; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(PL_PX[i], PL_PY)), module, PP[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(PL_PX[i], PL_PCV)), module, PI[i]));
			lbl->pairDown(PL_PX[i], PL_PY, PL_PCV, PN[i]);
		}
		static const char* FN[6] = {"V/OCT", "GATE", "VEL", "STRIKE", "BELL", "CLOCK"};
		static const int FI[6] = {Carillon::VOCT_INPUT, Carillon::GATE_INPUT, Carillon::VEL_INPUT, Carillon::STRIKE_INPUT, Carillon::BELL_INPUT, Carillon::CLOCK_INPUT};
		for (int i = 0; i < 6; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(PL_FX[i], PL_FY)), module, FI[i]));
			lbl->jack(PL_FX[i], PL_FY, FN[i]);
		}
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(PL_FX[6], PL_FY)), module, Carillon::L_OUTPUT));
		lbl->jackOnPlate(PL_FX[6], PL_FY, "L");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(PL_FX[7], PL_FY)), module, Carillon::R_OUTPUT));
		lbl->jackOnPlate(PL_FX[7], PL_FY, "R");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(PL_FX[8], PL_FY)), module, Carillon::POLY_OUTPUT));
		lbl->jackOnPlate(PL_FX[8], PL_FY, "POLY");
	}

	void appendContextMenu(Menu* menu) override {
		Carillon* m = dynamic_cast<Carillon*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		std::vector<std::string> ln(PL_LAYOUT_NAME, PL_LAYOUT_NAME + PL_NLAYOUT);
		menu->addChild(createIndexSubmenuItem("Layout", ln,
			[=]() { return m->layout; }, [=](int v) { m->layout = clamp(v, 0, PL_NLAYOUT - 1); m->relayout(); }));
		std::vector<std::string> on(PL_ORDER_NAME, PL_ORDER_NAME + PL_NORDER);
		if (m->layout != PL_KEYS)
			menu->addChild(createIndexSubmenuItem("Tuning order along the layout", on,
				[=]() { return m->order; }, [=](int v) { m->order = clamp(v, 0, PL_NORDER - 1); m->relayout(); }));
		menu->addChild(createBoolPtrMenuItem("Lift the felts (every bell sounds, whatever the key)", "", &m->unmask));
		menu->addChild(createMenuLabel("Rods"));
		static const char* RN[6] = {"A chain along the layout", "Every bell to its neighbours", "A star from the middle",
		                            "Octaves", "Fifths", "Clear"};
		for (int i = 0; i < 6; i++) { int idx = i; menu->addChild(createMenuItem(RN[i], "", [=]() { m->presetRods(idx); })); }
		menu->addChild(createMenuItem("Reset mic positions", "", [=]() { m->micU[0] = -1.5f; m->micU[1] = 1.5f; m->micV[0] = m->micV[1] = -0.85f; m->placeMics(); }));
	}
};

Model* modelCarillon = createModel<Carillon, CarillonWidget>("Carillon");

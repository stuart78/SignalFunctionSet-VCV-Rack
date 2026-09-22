// =============================================================================
// Canon: four arpeggiators reading one progression.
//
// A relative of Fugue. Fugue is three voices reading one melody and wandering
// from it; Canon is four channels reading one CHORD and wandering from that.
// A round is a canon in which every voice sings the same material, entering
// at its own moment, which is what four channels arpeggiating one chord on
// their own clocks are.
//
// Four SETS, each a chord of up to six notes, shared by everything. A SET gate
// advances through them, a SET CV picks one directly. Four CHANNELS each have
// a GATE input that steps them through the current set in their own PATTERN,
// at their own OCTAVE, straying from the chord by their own WANDER, and each
// puts out a V/OCT and a GATE pair. Change the set and every channel follows,
// from wherever it was. Design: docs/canon-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "scale-bus.hpp"
#include "scales.hpp"
#include "harmonic-tiers.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

static const int CN_SETS = 4;
static const int CN_CH = 4;
static const int CN_MAXN = 6;       // notes per set
static const int CN_NFAM = 8;       // pattern families; each comes in a 1- and a 2-octave form
static const int CN_NPAT = CN_NFAM * 2;
static const float CN_REST = -1000.f;   // a wander that chose silence

static const char* CN_FAMILY[CN_NFAM] = {
	"Up", "Down", "Up-down", "Converge", "Diverge", "Pedal", "Random", "As played"};
static const char* CN_NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// A note of a set. Stored as a SCALE DEGREE, so a key change from Key or
// Arrange moves every set with it -- or, when it was chosen outside the scale,
// as a semitone offset from the root, the way Key keeps a free sub-scale pick:
// it transposes with ROOT and does not follow SCALE.
struct CanonNote {
	int8_t deg = 0;
	int8_t chrom = 0;
	bool free = false;
};
struct CanonSet {
	CanonNote n[CN_MAXN];
	int count = 0;
};

struct CanonPatternQ : ParamQuantity {
	std::string getDisplayValueString() override {
		int p = clamp((int)std::round(getValue()), 0, CN_NPAT - 1);
		return std::string(CN_FAMILY[p % CN_NFAM]) + ((p >= CN_NFAM) ? ", 2 octaves" : "");
	}
};
struct CanonRootQ : ParamQuantity {
	std::string getDisplayValueString() override {
		return CN_NOTE_NAMES[clamp((int)std::round(getValue()), 0, 11)];
	}
};

struct Canon : Module {
	enum ParamId {
		ROOT_PARAM, SCALE_PARAM, SET_PARAM, RESET_PARAM, LEARN_PARAM,
		ENUMS(PATTERN_PARAM, CN_CH),
		ENUMS(WANDER_PARAM, CN_CH),
		ENUMS(OCTAVE_PARAM, CN_CH),
		SETSEL_PARAM,                      // appended with the 2026-09 art: the SET knob
		PARAMS_LEN
	};
	enum InputId {
		ROOT_INPUT, SCALE_INPUT, SET_INPUT, SET_CV_INPUT, RESET_INPUT, LEARN_INPUT,
		ENUMS(GATE_INPUT, CN_CH),
		ENUMS(PATTERN_INPUT, CN_CH),
		ENUMS(WANDER_INPUT, CN_CH),
		ENUMS(OCTAVE_INPUT, CN_CH),
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(CV_OUTPUT, CN_CH),
		ENUMS(GATE_OUTPUT, CN_CH),
		OUTPUTS_LEN
	};
	enum LightId { LIGHTS_LEN };

	CanonSet set[CN_SETS];
	// THE SET IN FORCE is an advance count plus the SET knob plus the SET CV,
	// modulo four, so the three compose: the knob picks a set when nothing
	// advances, NEXT walks on from wherever the knob points, and a CV
	// sequences it on top of both. `cur` is what those add up to this sample.
	int cur = 0;
	int nextCount = 0;                    // NEXT edges since reset
	int curWas = 0;
	int root = 0;                         // semitones, 0 = C
	sfs::BusScale sc;

	struct Ch {
		int   idx = 0;                    // position in the pattern's sequence
		float semi = 0.f;                 // what it is sounding, semitones from the root (any register)
		bool  sounding = false;
		bool  played = false;             // has stepped at least once: the marker stays on the last note
		bool  rest = false;
		bool  gateHi = false;
		int   offset = 0;                 // steps ahead in the pattern: four channels, offsets 0 1 2 3, one clock, is a chord
		float cv = 0.f;
		uint32_t rng = 0x9E3779B9u;
		dsp::SchmittTrigger trig;
		dsp::PulseGenerator pulse;
	};
	Ch ch[CN_CH];

	// options
	bool lock = true;                     // Fugue's harmonic lock, across the four channels
	bool restartOnSet = false;            // a channel keeps its place through a set change
	bool wanderRest = true;               // WANDER's top tier may substitute a rest
	int  gateMode = 0;                    // 0 follows the input, 1 a 10 ms trigger

	dsp::SchmittTrigger setTrig, resetTrig, learnTrig;
	dsp::BooleanTrigger setBtn, resetBtn, learnBtn;
	uint32_t rng = 0xC0FFEEu;

	Canon() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<CanonRootQ>(ROOT_PARAM, 0.f, 11.f, 0.f, "Root");
		paramQuantities[ROOT_PARAM]->snapEnabled = true;
		std::vector<std::string> scaleNames;
		for (int i = 0; i < sfs::NUM_SCALES; i++) scaleNames.push_back(sfs::SCALES[i].longName);
		configSwitch(SCALE_PARAM, 0.f, (float)(sfs::NUM_SCALES - 1), 1.f, "Scale", scaleNames);
		configButton(SET_PARAM, "Next set");
		configParam(SETSEL_PARAM, 0.f, 3.f, 0.f, "Set");
		paramQuantities[SETSEL_PARAM]->snapEnabled = true;
		configButton(RESET_PARAM, "Reset (set 1, every channel to its first step)");
		configButton(LEARN_PARAM, "Learn (write the held poly V/OCT into the current set)");
		for (int c = 0; c < CN_CH; c++) {
			std::string L = std::string(1, (char)('A' + c));
			configParam<CanonPatternQ>(PATTERN_PARAM + c, 0.f, (float)(CN_NPAT - 1), 0.f, "Pattern " + L);
			paramQuantities[PATTERN_PARAM + c]->snapEnabled = true;
			configParam(WANDER_PARAM + c, 0.f, 1.f, 0.f, "Wander " + L + " (how far it strays from the chord)", "%", 0.f, 100.f);
			configParam(OCTAVE_PARAM + c, -2.f, 2.f, 0.f, "Octave " + L);
			paramQuantities[OCTAVE_PARAM + c]->snapEnabled = true;
			configInput(GATE_INPUT + c, "Gate " + L + (c ? " (normalled from the channel to its left)" : ""));
			configInput(PATTERN_INPUT + c, "Pattern " + L + " CV (1V per pattern)");
			configInput(WANDER_INPUT + c, "Wander " + L + " CV (+-5V = full range)");
			configInput(OCTAVE_INPUT + c, "Octave " + L + " CV (1V per octave)");
			configOutput(CV_OUTPUT + c, "V/OCT " + L);
			configOutput(GATE_OUTPUT + c, "Gate " + L);
		}
		configInput(ROOT_INPUT, "Root CV (1V/oct, semitone-quantized)");
		configInput(SCALE_INPUT, "Scale CV (1V per scale, or Key's scale bus)");
		configInput(SET_INPUT, "Set: advance to the next set");
		configInput(SET_CV_INPUT, "Set CV (1V per set; wins when patched)");
		configInput(RESET_INPUT, "Reset");
		configInput(LEARN_INPUT, "Learn: a polyphonic V/OCT chord");
		for (int c = 0; c < CN_CH; c++) ch[c].rng = 0x9E3779B9u * (uint32_t)(c + 1);
		// a progression to start from: I IV V vi in the key
		static const int START[CN_SETS][3] = {{0, 2, 4}, {3, 5, 0}, {4, 6, 1}, {5, 0, 2}};
		for (int s = 0; s < CN_SETS; s++) {
			set[s].count = 3;
			for (int k = 0; k < 3; k++) { set[s].n[k].deg = (int8_t)START[s][k]; set[s].n[k].free = false; }
		}
	}

	// ── the notes ────────────────────────────────────────────────────────
	float noteSemis(const CanonNote& n) const {
		if (n.free) return (float)n.chrom;
		if (sc.size <= 0) return 0.f;
		return sc.degree(((n.deg % sc.size) + sc.size) % sc.size);
	}
	// the pitch class a note lands on, for the screen and for toggling
	int notePc(const CanonNote& n) const {
		return (((int)std::round(noteSemis(n)) + root) % 12 + 12) % 12;
	}
	// A pitch class (0 = C) as a note of the key: a degree when the scale has
	// one there, a free semitone otherwise.
	CanonNote noteFromPc(int pc) const {
		CanonNote n;
		int semis = ((pc - root) % 12 + 12) % 12;
		for (int d = 0; d < sc.size; d++) {
			float iv = sc.intervals[d];
			if (std::fabs(iv - (float)semis) < 0.5f) { n.deg = (int8_t)d; n.free = false; return n; }
		}
		n.free = true; n.chrom = (int8_t)semis;
		return n;
	}
	void toggleNote(int s, int pc) {
		CanonSet& S = set[s];
		for (int k = 0; k < S.count; k++) {
			if (notePc(S.n[k]) == pc) {
				for (int j = k; j + 1 < S.count; j++) S.n[j] = S.n[j + 1];
				S.count--;
				return;
			}
		}
		if (S.count >= CN_MAXN) return;
		S.n[S.count++] = noteFromPc(pc);
	}

	// ── the pattern: the ordered notes a channel walks ────────────────────
	// `semis` receives the set's notes in the family's order, expanded over
	// one or two octaves; `seq` the order they are visited. Random is drawn
	// at step time, so its sequence is the plain list.
	int buildSequence(int pattern, const CanonSet& S, float* semis, int* seq) const {
		int fam = pattern % CN_NFAM, octs = (pattern >= CN_NFAM) ? 2 : 1;
		int n = S.count;
		if (n <= 0) return 0;
		float base[CN_MAXN];
		for (int k = 0; k < n; k++) base[k] = noteSemis(S.n[k]);
		if (fam != 7) std::sort(base, base + n);          // as-played keeps entry order
		int N = 0;
		for (int o = 0; o < octs; o++)
			for (int k = 0; k < n; k++) semis[N++] = base[k] + (float)o * sc.period;
		int L = 0;
		switch (fam) {
			case 0: case 6: case 7:                        // up, random, as played
				for (int i = 0; i < N; i++) seq[L++] = i; break;
			case 1:                                        // down
				for (int i = N - 1; i >= 0; i--) seq[L++] = i; break;
			case 2:                                        // up-down, no repeat at the turns
				for (int i = 0; i < N; i++) seq[L++] = i;
				for (int i = N - 2; i >= 1; i--) seq[L++] = i;
				break;
			case 3: {                                      // converge: outside in
				int lo = 0, hi = N - 1;
				while (lo <= hi) { seq[L++] = lo; if (hi != lo) seq[L++] = hi; lo++; hi--; }
				break;
			}
			case 4: {                                      // diverge: inside out
				int tmp[2 * CN_MAXN]; int M = 0;
				int lo = 0, hi = N - 1;
				while (lo <= hi) { tmp[M++] = lo; if (hi != lo) tmp[M++] = hi; lo++; hi--; }
				for (int i = M - 1; i >= 0; i--) seq[L++] = tmp[i];
				break;
			}
			case 5:                                        // pedal: the lowest between each of the others
				if (N == 1) seq[L++] = 0;
				for (int i = 1; i < N; i++) { seq[L++] = 0; seq[L++] = i; }
				break;
		}
		return L;
	}

	// ── wander: Fugue's idea, for a chord ────────────────────────────────
	// At zero the channel plays the set's note. Strayed, it substitutes in
	// tiers: another chord tone, then an extension (a scale degree the chord
	// does not use), then colour (a semitone either side), then, at the top
	// of the range, a rest.
	float wanderPick(float base, const CanonSet& S, float w, uint32_t& r) const {
		if (w <= 0.001f || randFloat(r) >= w) return base;
		float oct = std::floor(base / sc.period) * sc.period;
		float t = randFloat(r);
		if (t < 0.45f && S.count > 1) {
			int k = (int)(randFloat(r) * S.count) % S.count;
			float s = noteSemis(S.n[k]) + oct;
			if (std::fabs(s - base) < 0.01f) s = noteSemis(S.n[(k + 1) % S.count]) + oct;
			return s;
		}
		if (t < 0.75f && sc.size > 0) {
			int cand[sfs::BUS_MAXDEG]; int nc = 0;
			for (int d = 0; d < sc.size; d++) {
				bool used = false;
				for (int k = 0; k < S.count; k++)
					if (std::fabs(noteSemis(S.n[k]) - sc.intervals[d]) < 0.5f) used = true;
				if (!used) cand[nc++] = d;
			}
			if (nc > 0) return sc.intervals[cand[(int)(randFloat(r) * nc) % nc]] + oct;
		}
		if (t < 0.90f || !wanderRest) return base + ((randFloat(r) < 0.5f) ? -1.f : 1.f);
		return CN_REST;
	}
	// Harmonic lock: three candidates, the one most consonant with what the
	// other channels are sounding wins. Without it the first candidate stands.
	float choose(int c, float base, const CanonSet& S, float w) {
		float best = wanderPick(base, S, w, ch[c].rng);
		if (!lock || w <= 0.001f) return best;
		float bestScore = -1.f;
		for (int i = 0; i < 3; i++) {
			float cand = (i == 0) ? best : wanderPick(base, S, w, ch[c].rng);
			float score = 0.f; int n = 0;
			if (cand != CN_REST) {
				for (int o = 0; o < CN_CH; o++) {
					if (o == c || !ch[o].sounding) continue;
					score += intervalConsonance((int)std::round(cand - ch[o].semi)); n++;
				}
			}
			score = n ? score / n : 0.5f;
			if (score > bestScore) { bestScore = score; best = cand; }
		}
		return best;
	}

	// ── sets ─────────────────────────────────────────────────────────────
	int setOffset() {
		int o = (int)std::round(params[SETSEL_PARAM].getValue());
		if (inputs[SET_CV_INPUT].isConnected()) o += (int)std::round(inputs[SET_CV_INPUT].getVoltage());
		return o;
	}
	int resolveSet() { return (((nextCount + setOffset()) % CN_SETS) + CN_SETS) % CN_SETS; }
	// NEXT skips empty sets, so a two-chord progression is sets 1 and 2
	void advanceSet() {
		for (int k = 1; k <= CN_SETS; k++) {
			nextCount++;
			if (set[resolveSet()].count > 0) return;
		}
	}
	void selectSet(int s) {
		nextCount += s - resolveSet();
	}
	void refreshSet() {
		cur = resolveSet();
		if (cur != curWas) {
			curWas = cur;
			if (restartOnSet) for (int c = 0; c < CN_CH; c++) ch[c].idx = 0;
		}
	}
	void learn() {
		int n = std::min(inputs[LEARN_INPUT].getChannels(), CN_MAXN);
		if (n <= 0) return;
		CanonSet& S = set[cur];
		S.count = 0;
		for (int k = 0; k < n; k++) {
			int semi = (int)std::round(inputs[LEARN_INPUT].getVoltage(k) * 12.f);
			int pc = ((semi % 12) + 12) % 12;
			bool dup = false;
			for (int j = 0; j < S.count; j++) if (notePc(S.n[j]) == pc) dup = true;
			if (!dup) S.n[S.count++] = noteFromPc(pc);
		}
	}

	// ── progressions: twenty ways to fill all four sets ───────────────────
	// Degree progressions are built on the scale in force (a triad is d, d+2,
	// d+4; a seventh adds d+6), so they stay right in Dorian or Lydian too.
	// The borrowed-chord ones are stated in semitones and land as degrees
	// where the scale has one and as accidentals where it does not. The minor
	// ones set SCALE to Natural minor, because "i VII VI V" read against a
	// major scale is a different progression.
	struct Prog { const char* name; int kind; int a[4][4]; int n; };   // kind 0 degrees, 1 degrees+7ths, 2 semitone chords, 3 degrees in minor
	static const Prog* progressions(int& count) {
		static const Prog P[] = {
			{"I  V  vi  IV",              0, {{0}, {4}, {5}, {3}}, 3},
			{"vi  IV  I  V",              0, {{5}, {3}, {0}, {4}}, 3},
			{"I  vi  IV  V  (doo-wop)",   0, {{0}, {5}, {3}, {4}}, 3},
			{"I  IV  V  IV",              0, {{0}, {3}, {4}, {3}}, 3},
			{"I  IV  vi  V",              0, {{0}, {3}, {5}, {4}}, 3},
			{"I  iii  vi  IV",            0, {{0}, {2}, {5}, {3}}, 3},
			{"IV  V  iii  vi  (royal road)", 0, {{3}, {4}, {2}, {5}}, 3},
			{"I  V  vi  iii  (canon)",    0, {{0}, {4}, {5}, {2}}, 3},
			{"I  IV  I  V",               0, {{0}, {3}, {0}, {4}}, 3},
			{"I  IV  ii  V",              0, {{0}, {3}, {1}, {4}}, 3},
			{"vi  ii  V  I  (circle)",    0, {{5}, {1}, {4}, {0}}, 3},
			{"I  vi  ii  V  (sevenths)",  1, {{0}, {5}, {1}, {4}}, 4},
			{"ii  V  I  I  (sevenths)",   1, {{1}, {4}, {0}, {0}}, 4},
			{"iii  vi  ii  V  (sevenths)", 1, {{2}, {5}, {1}, {4}}, 4},
			{"I  bVII  IV  I  (mixolydian)", 2, {{0, 4, 7}, {10, 2, 5}, {5, 9, 0}, {0, 4, 7}}, 3},
			{"I  bVI  bVII  I  (aeolian)", 2, {{0, 4, 7}, {8, 0, 3}, {10, 2, 5}, {0, 4, 7}}, 3},
			{"I  iii  IV  iv  (borrowed iv)", 2, {{0, 4, 7}, {4, 7, 11}, {5, 9, 0}, {5, 8, 0}}, 3},
			{"i  VII  VI  V  (Andalusian, minor)", 3, {{0}, {6}, {5}, {4}}, 3},
			{"i  VI  III  VII  (minor)",  3, {{0}, {5}, {2}, {6}}, 3},
			{"i  iv  VII  III  (minor)",  3, {{0}, {3}, {6}, {2}}, 3},
		};
		count = (int)(sizeof(P) / sizeof(P[0]));
		return P;
	}
	void applyProgression(int which) {
		int count; const Prog* P = progressions(count);
		if (which < 0 || which >= count) return;
		const Prog& p = P[which];
		if (p.kind == 3) {
			int minor = -1;
			for (int i = 0; i < sfs::NUM_SCALES && minor < 0; i++)
				if (std::string(sfs::SCALES[i].longName).find("Natural") != std::string::npos) minor = i;
			if (minor < 0) for (int i = 0; i < sfs::NUM_SCALES && minor < 0; i++)
				if (std::string(sfs::SCALES[i].longName).find("Minor") != std::string::npos) minor = i;
			if (minor >= 0) { params[SCALE_PARAM].setValue((float)minor); sc = sfs::busResolve(inputs[SCALE_INPUT], minor); }
		}
		for (int s = 0; s < CN_SETS; s++) {
			CanonSet& S = set[s];
			S.count = 0;
			if (p.kind == 2) {
				for (int k = 0; k < p.n; k++) S.n[S.count++] = noteFromPc((p.a[s][k] + root) % 12);
			} else {
				int size = std::max(sc.size, 1);
				for (int k = 0; k < p.n; k++) {
					CanonNote N; N.free = false; N.deg = (int8_t)((p.a[s][0] + 2 * k) % size);
					S.n[S.count++] = N;
				}
			}
		}
		cur = 0;
	}

	// ── voicings: fill one set ────────────────────────────────────────────
	struct Voicing { const char* name; int n; int deg[6]; };
	static const Voicing* voicings(int& count) {
		static const Voicing V[] = {
			{"Triad", 3, {0, 2, 4}}, {"Seventh", 4, {0, 2, 4, 6}}, {"Ninth", 5, {0, 2, 4, 6, 1}},
			{"Sus2", 3, {0, 1, 4}}, {"Sus4", 3, {0, 3, 4}}, {"Add9", 4, {0, 2, 4, 1}},
			{"Sixth", 4, {0, 2, 4, 5}}, {"Quartal", 4, {0, 3, 6, 2}}, {"Root and fifth", 2, {0, 4}},
		};
		count = (int)(sizeof(V) / sizeof(V[0]));
		return V;
	}
	void applyVoicing(int setIdx, int which) {
		int count; const Voicing* V = voicings(count);
		if (setIdx < 0 || setIdx >= CN_SETS || which < 0 || which >= count) return;
		CanonSet& S = set[setIdx];
		S.count = 0;
		for (int k = 0; k < V[which].n; k++) { S.n[S.count].deg = (int8_t)V[which].deg[k]; S.n[S.count].free = false; S.count++; }
	}

	float gateVoltage(int c) {
		// normalled left to right: a cable breaks the chain from that channel on
		for (int k = c; k >= 0; k--)
			if (inputs[GATE_INPUT + k].isConnected()) return inputs[GATE_INPUT + k].getVoltage();
		return 0.f;
	}

	void step(int c) {
		Ch& C = ch[c];
		const CanonSet& S = set[cur];
		if (S.count <= 0) { C.rest = true; C.sounding = false; return; }
		int pattern = clamp((int)std::round(params[PATTERN_PARAM + c].getValue()
		                                  + inputs[PATTERN_INPUT + c].getVoltage()), 0, CN_NPAT - 1);
		float w = clamp(params[WANDER_PARAM + c].getValue() + inputs[WANDER_INPUT + c].getVoltage() * 0.2f, 0.f, 1.f);
		int oct = clamp((int)std::round(params[OCTAVE_PARAM + c].getValue() + inputs[OCTAVE_INPUT + c].getVoltage()), -4, 4);
		float semis[2 * CN_MAXN]; int seq[4 * CN_MAXN];
		int L = buildSequence(pattern, S, semis, seq);
		if (L <= 0) { C.rest = true; C.sounding = false; return; }
		int pos = ((C.idx % L) + L) % L;
		int at = ((pos + C.offset) % L + L) % L;          // the offset moves where it reads, not where it counts
		int which = (pattern % CN_NFAM == 6) ? (int)(randFloat(C.rng) * L) % L : seq[at];
		C.idx = pos + 1;
		float base = semis[which] + (float)oct * sc.period;
		float pick = choose(c, base, S, w);
		if (pick == CN_REST) { C.rest = true; C.sounding = false; return; }
		C.rest = false; C.sounding = true; C.played = true; C.semi = pick;
		C.cv = ((float)root + pick) / 12.f;
		C.pulse.trigger(0.01f);
	}

	void process(const ProcessArgs& args) override {
		// the key, in the plugin's convention
		int r = (int)std::round(params[ROOT_PARAM].getValue());
		if (inputs[ROOT_INPUT].isConnected()) r += (int)std::round(inputs[ROOT_INPUT].getVoltage() * 12.f);
		root = ((r % 12) + 12) % 12;
		sc = sfs::busResolve(inputs[SCALE_INPUT], (int)std::round(params[SCALE_PARAM].getValue()));

		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)
		 || resetBtn.process(params[RESET_PARAM].getValue() > 0.5f)) {
			nextCount = 0;
			for (int c = 0; c < CN_CH; c++) ch[c].idx = 0;
		}
		if (setTrig.process(inputs[SET_INPUT].getVoltage(), 0.1f, 1.f)) advanceSet();
		if (setBtn.process(params[SET_PARAM].getValue() > 0.5f)) advanceSet();
		refreshSet();
		if (learnBtn.process(params[LEARN_PARAM].getValue() > 0.5f)) learn();

		for (int c = 0; c < CN_CH; c++) {
			Ch& C = ch[c];
			float g = gateVoltage(c);
			if (C.trig.process(g, 0.1f, 1.f)) step(c);
			C.gateHi = g >= 1.f;
			if (!C.gateHi && gateMode == 0) C.sounding = false;
			bool out = C.rest ? false
			         : (gateMode == 0 ? C.gateHi : C.pulse.process(args.sampleTime));
			if (gateMode == 1) C.pulse.process(0.f);
			outputs[GATE_OUTPUT + c].setVoltage(out ? 10.f : 0.f);
			outputs[CV_OUTPUT + c].setVoltage(C.cv);
		}
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_t* sets = json_array();
		for (int s = 0; s < CN_SETS; s++) {
			json_t* a = json_array();
			for (int k = 0; k < set[s].count; k++) {
				json_t* n = json_object();
				json_object_set_new(n, "deg", json_integer(set[s].n[k].deg));
				json_object_set_new(n, "chrom", json_integer(set[s].n[k].chrom));
				json_object_set_new(n, "free", json_boolean(set[s].n[k].free));
				json_array_append_new(a, n);
			}
			json_array_append_new(sets, a);
		}
		json_object_set_new(r, "sets", sets);
		json_object_set_new(r, "nextCount", json_integer(nextCount));
		json_object_set_new(r, "lock", json_boolean(lock));
		json_object_set_new(r, "restartOnSet", json_boolean(restartOnSet));
		json_object_set_new(r, "wanderRest", json_boolean(wanderRest));
		json_object_set_new(r, "gateMode", json_integer(gateMode));
		json_t* offs = json_array();
		for (int c = 0; c < CN_CH; c++) json_array_append_new(offs, json_integer(ch[c].offset));
		json_object_set_new(r, "offset", offs);
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* sets = json_object_get(r, "sets")) {
			for (int s = 0; s < CN_SETS && s < (int)json_array_size(sets); s++) {
				json_t* a = json_array_get(sets, s);
				set[s].count = 0;
				for (int k = 0; k < (int)json_array_size(a) && k < CN_MAXN; k++) {
					json_t* n = json_array_get(a, k);
					CanonNote& N = set[s].n[set[s].count++];
					N.deg = (int8_t)json_integer_value(json_object_get(n, "deg"));
					N.chrom = (int8_t)json_integer_value(json_object_get(n, "chrom"));
					N.free = json_boolean_value(json_object_get(n, "free"));
				}
			}
		}
		if (json_t* j = json_object_get(r, "nextCount")) nextCount = (int)json_integer_value(j);
		if (json_t* j = json_object_get(r, "lock")) lock = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "restartOnSet")) restartOnSet = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "wanderRest")) wanderRest = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "gateMode")) gateMode = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* offs = json_object_get(r, "offset"))
			for (int c = 0; c < CN_CH && c < (int)json_array_size(offs); c++)
				ch[c].offset = clamp((int)json_integer_value(json_array_get(offs, c)), 0, 11);
	}
};

// =============================================================================
// Preview: a real instance walking I IV V vi with four patterns, for the
// browser thumbnail.
// =============================================================================
static Canon* canonPreview() {
	static Canon* m = nullptr;
	if (m) return m;
	m = new Canon();
	sfs::previewConnect(m->inputs[Canon::GATE_INPUT + 0], 1);
	sfs::previewConnect(m->inputs[Canon::SET_INPUT], 1);
	m->params[Canon::PATTERN_PARAM + 1].setValue(1.f);
	m->params[Canon::PATTERN_PARAM + 2].setValue(2.f + CN_NFAM);
	m->params[Canon::PATTERN_PARAM + 3].setValue(5.f);
	m->params[Canon::OCTAVE_PARAM + 3].setValue(-1.f);
	m->params[Canon::WANDER_PARAM + 2].setValue(0.3f);
	rack::engine::Module::ProcessArgs a = sfs::previewArgs();
	int64_t frame = 0;
	for (int i = 0; i < 48000; i++) {
		bool g = (i % 6000) < 3000;
		m->inputs[Canon::GATE_INPUT + 0].setVoltage(g ? 10.f : 0.f);
		m->inputs[Canon::SET_INPUT].setVoltage((i % 24000) < 1000 ? 10.f : 0.f);
		a.frame = frame++;
		m->process(a);
	}
	// end on a note, so the channels' dots are on the keys
	m->inputs[Canon::GATE_INPUT + 0].setVoltage(10.f);
	for (int i = 0; i < 100; i++) { a.frame = frame++; m->process(a); }
	return m;
}

// =============================================================================
// The screen: four keyboards, one per set, the set in force framed, and each
// channel's sounding note marked on it in the channel's colour. Click a key
// to add or remove that note from the set.
// =============================================================================
// The plugin's four channel colours, as Trace's lanes and Spool's tapes wear them.
static const NVGcolor CN_CH_COL[CN_CH] = {
	nvgRGB(0x00, 0x97, 0xDE),   // A blue
	nvgRGB(0x3F, 0xBF, 0x6F),   // B green
	nvgRGB(0xEC, 0x65, 0x2E),   // C orange
	nvgRGB(0x9B, 0x6B, 0xD6),   // D purple
};
static const int CN_WHITE_PC[7] = {0, 2, 4, 5, 7, 9, 11};
static const int CN_BLACK_AFTER[5] = {0, 1, 3, 4, 5};    // a black key sits after these white keys
static const int CN_BLACK_PC[5] = {1, 3, 6, 8, 10};

struct CanonDisplay : OpaqueWidget {
	Canon* module = nullptr;
	std::shared_ptr<Font> font;

	// keyboard geometry, in px: four keyboards across the screen
	void kbRect(int s, float& x, float& y, float& w, float& h) const {
		float pad = mm2px(2.5f), gap = mm2px(2.5f);
		float top = mm2px(7.5f);
		w = (box.size.x - 2.f * pad - 3.f * gap) / 4.f;
		x = pad + s * (w + gap);
		y = top;
		h = box.size.y - top - mm2px(6.0f);
	}
	// which set and pitch class is under a point, or -1
	int hitKey(Vec p, int& setOut) const {
		for (int s = 0; s < CN_SETS; s++) {
			float x, y, w, h; kbRect(s, x, y, w, h);
			if (p.x < x || p.x >= x + w || p.y < y || p.y >= y + h) continue;
			setOut = s;
			float kw = w / 7.f;
			float rel = p.x - x;
			if (p.y < y + h * 0.6f) {
				for (int b = 0; b < 5; b++) {
					float bx = x + (CN_BLACK_AFTER[b] + 1) * kw;
					if (p.x >= bx - kw * 0.3f && p.x < bx + kw * 0.3f) return CN_BLACK_PC[b];
				}
			}
			int wi = clamp((int)(rel / kw), 0, 6);
			return CN_WHITE_PC[wi];
		}
		return -1;
	}
	// which keyboard is under a point, or -1
	int hitSet(Vec p) const {
		for (int s = 0; s < CN_SETS; s++) {
			float x, y, w, h; kbRect(s, x, y, w, h);
			if (p.x >= x - mm2px(1.f) && p.x < x + w + mm2px(1.f) && p.y >= y - mm2px(1.f) && p.y < y + h + mm2px(4.f)) return s;
		}
		return -1;
	}
	void onButton(const ButtonEvent& e) override {
		OpaqueWidget::onButton(e);
		if (!module || e.action != GLFW_PRESS) return;
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			int s = -1; int pc = hitKey(e.pos, s);
			if (pc >= 0) { module->toggleNote(s, pc); e.consume(this); }
			return;
		}
		// A RIGHT-CLICK IS ABOUT THE SET UNDER THE MOUSE, not the one playing:
		// the keyboard you point at is the one you mean.
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			int s = hitSet(e.pos);
			if (s < 0) return;
			e.consume(this);
			Canon* m = module;
			Menu* menu = createMenu();
			menu->addChild(createMenuLabel(string::f("Set %d", s + 1)));
			if (s != m->cur) menu->addChild(createMenuItem("Play this set now", "", [=]() { m->selectSet(s); }));
			menu->addChild(new MenuSeparator);
			menu->addChild(createMenuLabel("Fill with a voicing"));
			int count; const Canon::Voicing* V = Canon::voicings(count);
			for (int i = 0; i < count; i++) {
				int idx = i;
				menu->addChild(createMenuItem(V[i].name, "", [=]() { m->applyVoicing(s, idx); }));
			}
			menu->addChild(new MenuSeparator);
			menu->addChild(createMenuItem("Copy from the set playing", "", [=]() { if (s != m->cur) m->set[s] = m->set[m->cur]; }));
			menu->addChild(createMenuItem("Clear this set", "", [=]() { m->set[s].count = 0; }));
		}
	}

	void draw(const DrawArgs& args) override {
		if (!module) { module = canonPreview(); draw(args); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		float W = box.size.x, H = box.size.y;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, W, H, mm2px(1.f));
		nvgFillColor(vg, sfs::SCREEN_BG); nvgFill(vg);
		nvgSave(vg); nvgScissor(vg, 0, 0, W, H);

		// header: the key
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font);
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			std::string key = std::string(CN_NOTE_NAMES[module->root]) + " ";
			if (module->sc.index >= 0 && module->sc.index < sfs::NUM_SCALES) key += sfs::SCALES[module->sc.index].longName;
			nvgText(vg, mm2px(2.5f), mm2px(3.8f), key.c_str(), NULL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, W - mm2px(2.5f), mm2px(3.8f), "CLICK A KEY TO ADD OR REMOVE A NOTE", NULL);
			// the channels, in their colours
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			for (int c = 0; c < CN_CH; c++) {
				float lx = W * 0.5f + (c - 1.5f) * mm2px(5.f);
				nvgBeginPath(vg); nvgCircle(vg, lx - mm2px(1.6f), mm2px(3.8f), mm2px(0.9f));
				nvgFillColor(vg, CN_CH_COL[c]); nvgFill(vg);
				nvgFillColor(vg, sfs::SCREEN_TEXT);
				char L[2] = {(char)('A' + c), 0};
				nvgText(vg, lx + mm2px(0.4f), mm2px(3.8f), L, NULL);
			}
		}

		for (int s = 0; s < CN_SETS; s++) {
			float x, y, w, h; kbRect(s, x, y, w, h);
			float kw = w / 7.f;
			const CanonSet& S = module->set[s];
			bool inSet[12] = {};
			for (int k = 0; k < S.count; k++) inSet[module->notePc(S.n[k])] = true;
			// white keys
			for (int i = 0; i < 7; i++) {
				nvgBeginPath(vg);
				nvgRect(vg, x + i * kw + 0.5f, y, kw - 1.f, h);
				nvgFillColor(vg, inSet[CN_WHITE_PC[i]] ? sfs::SCREEN_BLUE : sfs::SCREEN_PURP);
				nvgFill(vg);
			}
			// black keys
			for (int b = 0; b < 5; b++) {
				float bx = x + (CN_BLACK_AFTER[b] + 1) * kw;
				nvgBeginPath(vg);
				nvgRect(vg, bx - kw * 0.3f, y, kw * 0.6f, h * 0.6f);
				nvgFillColor(vg, inSet[CN_BLACK_PC[b]] ? sfs::SCREEN_DEEP : sfs::SCREEN_BG);
				nvgFill(vg);
				nvgStrokeColor(vg, sfs::SCREEN_PMID); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
			}
			// WHERE EACH CHANNEL IS. A marker per channel on the note it last
			// played, and it STAYS there until the next step: drawn only while
			// the gate was high, the markers blinked with the clock and read
			// as stray flashes. Each channel has its own row down the key, so
			// four channels on one note are four marks, not one.
			if (s == module->cur) {
				for (int c = 0; c < CN_CH; c++) {
					const Canon::Ch& C = module->ch[c];
					if (!C.played || C.rest) continue;
					int pc = (((int)std::round(C.semi) + module->root) % 12 + 12) % 12;
					float cx, cy;
					bool black = false; int bi = 0;
					for (int b = 0; b < 5; b++) if (CN_BLACK_PC[b] == pc) { black = true; bi = b; }
					if (black) { cx = x + (CN_BLACK_AFTER[bi] + 1) * kw; cy = y + h * (0.12f + 0.11f * c); }
					else {
						int wi = 0; for (int i = 0; i < 7; i++) if (CN_WHITE_PC[i] == pc) wi = i;
						cx = x + (wi + 0.5f) * kw; cy = y + h * (0.64f + 0.1f * c);
					}
					nvgBeginPath(vg); nvgCircle(vg, cx, cy, mm2px(1.0f));
					nvgFillColor(vg, CN_CH_COL[c]); nvgFill(vg);
					nvgStrokeColor(vg, sfs::SCREEN_BG); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
				}
				nvgBeginPath(vg);
				nvgRect(vg, x - mm2px(0.8f), y - mm2px(0.8f), w + mm2px(1.6f), h + mm2px(1.6f));
				nvgStrokeColor(vg, sfs::SCREEN_HOT); nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
			}
			// the set's number and its notes
			if (font && font->handle >= 0) {
				sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, s == module->cur ? sfs::SCREEN_HOT : sfs::SCREEN_DIM);
				std::string t = string::f("%d ", s + 1);
				for (int k = 0; k < S.count; k++) { t += CN_NOTE_NAMES[module->notePc(S.n[k])]; if (k + 1 < S.count) t += " "; }
				nvgText(vg, x, y + h + mm2px(3.2f), t.c_str(), NULL);
			}
		}
		nvgRestore(vg);
		OpaqueWidget::draw(args);
	}
};

// =============================================================================
// Panel: 26HP, the designer's Figma export (2026-09). The art carries its own
// outlined labels, so there is NO sfs::PanelLabels here -- adding one prints
// every label twice. Screen across the top; one row of six pairs (ROOT,
// SCALE, NEXT, SET, LEARN, RESET, each a control over its jack); then four
// channel blocks of PAT / WANDER / OCT over their CVs, with GATE in and the
// GATE + V/OCT pair on a plate at the foot. Positions are read from the art's
// guides and written out in full for tools/panel_reticules.py.
// =============================================================================
static const float CN_GY = 69.9f;                                        // the global row, control and jack side by side
static const float CN_GX[12] = {6.31f, 16.55f, 27.89f, 38.14f, 49.48f, 59.72f, 71.06f, 81.31f, 92.65f, 102.89f, 114.24f, 124.48f};
static const float CN_CX[12] = {6.31f, 16.46f, 26.62f, 39.41f, 49.56f, 59.72f, 72.42f, 82.58f, 92.74f, 105.43f, 115.59f, 125.75f};
static const float CN_FX[12] = {6.22f, 16.38f, 26.54f, 39.41f, 49.56f, 59.72f, 72.33f, 82.49f, 92.65f, 105.35f, 115.51f, 125.66f};
static const float CN_CY = 91.3f, CN_CCV = 101.6f, CN_FY = 121.9f;

struct CanonWidget : ModuleWidget {
	CanonWidget(Canon* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/canon.svg")));

		CanonDisplay* disp = new CanonDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(5.08f, 10.16f));
		disp->box.size = mm2px(Vec(121.90f, 48.25f));
		addChild(disp);

		// the key and the sets
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CN_GX[0], CN_GY)), module, Canon::ROOT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_GX[1], CN_GY)), module, Canon::ROOT_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CN_GX[2], CN_GY)), module, Canon::SCALE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_GX[3], CN_GY)), module, Canon::SCALE_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(CN_GX[4], CN_GY)), module, Canon::SET_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_GX[5], CN_GY)), module, Canon::SET_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CN_GX[6], CN_GY)), module, Canon::SETSEL_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_GX[7], CN_GY)), module, Canon::SET_CV_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(CN_GX[8], CN_GY)), module, Canon::LEARN_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_GX[9], CN_GY)), module, Canon::LEARN_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(CN_GX[10], CN_GY)), module, Canon::RESET_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_GX[11], CN_GY)), module, Canon::RESET_INPUT));

		// the channels: three columns each, GATE in then GATE out and V/OCT out on the plate
		for (int c = 0; c < CN_CH; c++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(CN_CX[3 * c], CN_CY)), module, Canon::PATTERN_PARAM + c));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_CX[3 * c], CN_CCV)), module, Canon::PATTERN_INPUT + c));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(CN_CX[3 * c + 1], CN_CY)), module, Canon::WANDER_PARAM + c));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_CX[3 * c + 1], CN_CCV)), module, Canon::WANDER_INPUT + c));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(CN_CX[3 * c + 2], CN_CY)), module, Canon::OCTAVE_PARAM + c));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_CX[3 * c + 2], CN_CCV)), module, Canon::OCTAVE_INPUT + c));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CN_FX[3 * c], CN_FY)), module, Canon::GATE_INPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CN_FX[3 * c + 1], CN_FY)), module, Canon::GATE_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CN_FX[3 * c + 2], CN_FY)), module, Canon::CV_OUTPUT + c));
		}
	}

	void appendContextMenu(Menu* menu) override {
		Canon* m = dynamic_cast<Canon*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Harmonic lock (channels choose notes that agree)", "", &m->lock));
		menu->addChild(createBoolPtrMenuItem("Restart channels on set change", "", &m->restartOnSet));
		menu->addChild(createBoolPtrMenuItem("Wander may rest", "", &m->wanderRest));
		menu->addChild(createIndexPtrSubmenuItem("Gate out", {"Follows the gate in", "10 ms trigger"}, &m->gateMode));
		// STEP OFFSET, per channel: how many steps ahead of its count the
		// channel reads. Four channels on one clock with the same pattern and
		// offsets 0 1 2 3 play the chord's notes together -- the arpeggiator
		// becomes a chord player -- and offsets of 0 0 1 1 are two-note dyads
		// walking in pairs.
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Step offset (channels on one clock play a chord)"));
		for (int c = 0; c < CN_CH; c++) {
			std::vector<std::string> names;
			for (int k = 0; k < 12; k++) names.push_back(k ? string::f("+%d", k) : "0");
			menu->addChild(createIndexSubmenuItem(string::f("Channel %c", 'A' + c), names,
				[=]() { return m->ch[c].offset; },
				[=](int v) { m->ch[c].offset = clamp(v, 0, 11); }));
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Chord progressions (fill all four sets)", "", [=](Menu* sub) {
			int count; const Canon::Prog* P = Canon::progressions(count);
			for (int i = 0; i < count; i++) {
				int idx = i;
				sub->addChild(createMenuItem(P[i].name, "", [=]() { m->applyProgression(idx); }));
			}
		}));
		menu->addChild(createMenuLabel("Fill the current set with a voicing (or right-click a keyboard)"));
		int count; const Canon::Voicing* V = Canon::voicings(count);
		for (int i = 0; i < count; i++) {
			int idx = i;
			menu->addChild(createMenuItem(V[i].name, "", [=]() { m->applyVoicing(m->cur, idx); }));
		}
		menu->addChild(createMenuItem("Clear the current set", "", [=]() { m->set[m->cur].count = 0; }));
	}
};

Model* modelCanon = createModel<Canon, CanonWidget>("Canon");

// =============================================================================
// Pole: a stereo filter made of four pole pairs.
//
// A pole pair sets one cutoff and one resonance: the pair's angle is its
// frequency and its distance from the origin sets its Q. Four of them can be gathered at one
// frequency, where in series they are one steep resonant filter, or spread
// apart, where they are four separate peaks. TOPOLOGY sets whether the pairs
// are applied one after another (series: one shape, the peaks multiplied) or
// side by side (parallel: a sum of bands). MODE moves only the zeros; lowpass,
// bandpass, highpass and notch use the same poles.
//
// Pair i sits at CUTOFF * 2^(SPREAD * pos_i + trim_i) + SHIFT. CUTOFF moves
// every pair by the same ratio, so the layout's intervals are kept; SHIFT
// moves every pair by the same number of Hz, so they are not. trim_i is the
// pair's own offset, dragged on the screen.
//
// STEREO sets how the right side's poles relate to the left's: Linked, an offset in
// octaves or in Hz, Mirror (R moves opposite to L in log frequency, about a
// frequency AMOUNT sets), Spread
// (R's spread a multiple of L's, through zero to reversed), or Independent.
//
// Two things the prototype measured (tools/filter-proto.cpp, docs/pole-design.md):
//  * Resonances that coincide MULTIPLY in series, so each series pair shares
//    its Q with the pairs it overlaps, and the gathered set makes one peak of
//    the Q asked for.
//  * Series bandpasses pulled apart lose up to 45 dB. The level is corrected
//    from the analytic response, which is also what the screen draws.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "fastmath.hpp"
#include "waveguide.hpp"
#include <cmath>
#include <string>

static const int PO_N = 4;                               // pole pairs
static const float PO_POS[PO_N] = {-1.5f, -0.5f, 0.5f, 1.5f};  // layout, in units of SPREAD
static const int PO_CTL = 16;                            // samples per control update
static const int PO_GRID = 32;                           // response points for the level
static const float PO_FMIN = 8.f;
static const float PO_IN = 0.2f;                         // volts to internal units (5 V = 1)
// Every setting reaches the filter through a one-pole glide of this time
// constant, CUTOFF's CV excepted (it can be audio-rate FM). A knob turned fast,
// a CV that steps, or a switch of MODE or TOPOLOGY otherwise lands on the
// filter in one sample, and the output steps with it: that was a click.
static const float PO_GLIDE = 0.006f;
// A change that cannot glide (series order, the parallel end, the stereo
// relation) is made with the output faded out for this long, then back in.
static const float PO_DUCK = 0.005f;

enum PoStereo { PO_LINKED, PO_RATIO, PO_HZ, PO_MIRROR, PO_SPREADREL, PO_INDEP, PO_NSTEREO };
static const char* PO_STEREO_NAME[PO_NSTEREO] = {
	"Linked", "Offset in octaves", "Offset in Hz", "Mirror", "Spread", "Independent"};
static const char* PO_STEREO_SHORT[PO_NSTEREO] = {"LINKED", "OCTAVES", "HZ", "MIRROR", "SPREAD", "INDEPENDENT"};
// Mirror reflects R about a fixed frequency, 1 kHz with AMOUNT moving it two
// octaves either way, so turning CUTOFF by hand mirrors. (About the knob
// itself, only CV mirrored and the knob moved both sides together.)
static float poMirrorAxis(float amount) { return 1000.f * std::exp2(2.f * amount); }

static const int PO_ORDERS[4][PO_N] = {{0, 1, 2, 3}, {3, 2, 1, 0}, {0, 3, 1, 2}, {1, 2, 0, 3}};
static const char* PO_ORDER_NAME[4] = {"Lowest pair first", "Highest pair first", "Outside in", "Inside out"};

// ── the knobs' meanings, shared by the module, the tooltips and the screen ──
static float poBwOct(float v) { return 0.02f * std::pow(150.f, clamp(v, 0.f, 1.f)); }
static float poQ(float bwOct) {                           // bandwidth in octaves to Q
	float p = std::exp2(bwOct);
	return std::sqrt(p) / (p - 1.f);
}
static float poShiftHz(float v) { return 2000.f * v * v * v; }
static float poLagS(float v) { v = clamp(v, 0.f, 1.f); return (std::pow(500.f, v) - 1.f) / 499.f; }
static float poDrive(float v) { v = clamp(v, 0.f, 1.f); return 10.f * v * v; }

// One saturating stage: unity for small signals, a 0.5 (2.5 V) signal comes
// through at its own level, and louder ones flatten. Continuous at d = 0.
static inline float poSat(float y, float d, float comp) { return SFS_TANH(y * d) * comp; }

// Mode weights: LP, BP, HP, notch (= LP + HP), continuous between neighbours.
static void poModeW(float mode, float& wl, float& wb, float& wh) {
	mode = clamp(mode, 0.f, 3.f);
	float w[4] = {0, 0, 0, 0};
	int i = std::min((int)mode, 2);
	float t = mode - i;
	w[i] = 1.f - t; w[i + 1] = t;
	wl = w[0] + w[3]; wb = w[1]; wh = w[2] + w[3];
}

// ── the analytic response of one TPT state-variable pair ──
// The TPT SVF is the bilinear transform of the analog prototype with the
// cutoff prewarped, so the digital response at w is the analog one at
// tan(pi w/sr) / tan(pi fc/sr): exact, not an approximation.
struct PoC { float r, i; };
static inline PoC poMul(PoC a, PoC b) { return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r}; }
static inline PoC poPairH(float ratio, float q, float wl, float wb, float wh) {
	// den = 1 - r^2 + j r/q ; LP = 1/den, BP = j r/den, HP = -r^2/den
	float dr = 1.f - ratio * ratio, di = ratio / q;
	float nr = wl - wh * ratio * ratio, ni = wb * ratio;
	float d = dr * dr + di * di;
	return {(nr * dr + ni * di) / d, (ni * dr - nr * di) / d};
}

// ── one pair's filter state ──
struct PoPair {
	float ic1 = 0.f, ic2 = 0.f;
	float a1 = 1.f, a2 = 0.f, a3 = 0.f, k = 1.f;
	void set(float g, float kk) {
		k = kk;
		a1 = 1.f / (1.f + g * (g + k));
		a2 = g * a1;
		a3 = g * a2;
	}
	inline float tick(float x, float wl, float wb, float wh) {
		float v3 = x - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		float hp = x - k * v1 - v2;
		return wl * v2 + wb * v1 + wh * hp;
	}
	void reset() { ic1 = ic2 = 0.f; }
};

// ── the starting points: the settings of the prototype's listening renders ──
// Each sets the panel and shows on the screen which control to move, because
// each listening render was made by moving one control.
// Knob values are given in the units they mean and converted below.
struct PoPreset {
	const char* name;
	const char* move;           // shown on the screen after loading
	float cutoffHz, shiftHz, q, spread, mode, topo, lagS, drive;
	int stereo; float amount;
	float trim[PO_N];
	int order;
};
static const PoPreset PO_PRESETS[] = {
	{"Gathered 8-pole lowpass", "SWEEP CUTOFF", 800, 0, 3, 0, 0, 0, 0, 0, PO_LINKED, 0, {0, 0, 0, 0}, 0},
	{"Series lowpass, pulled apart", "TURN SPREAD 0 TO 1.5", 900, 0, 8, 0, 0, 0, 0, 0, PO_LINKED, 0, {0, 0, 0, 0}, 0},
	{"Series bandpass, pulled apart", "TURN SPREAD 0 TO 1.5", 900, 0, 8, 0, 1, 0, 0, 0, PO_LINKED, 0, {0, 0, 0, 0}, 0},
	{"Parallel bands", "TURN SPREAD 0 TO 1.5", 900, 0, 8, 0, 1, 1, 0, 0, PO_LINKED, 0, {0, 0, 0, 0}, 0},
	{"Series into parallel", "TURN TOPOLOGY", 900, 0, 8, 0.8f, 1, 0, 0, 0, PO_LINKED, 0, {0, 0, 0, 0}, 0},
	{"LAG on an envelope", "PATCH AN ENVELOPE INTO CUTOFF CV", 150, 0, 6, 0, 0, 0, 0.15f, 0, PO_LINKED, 0, {0, 0, 0, 0}, 0},
	{"Stereo: R 0.3 octave up", "SWEEP CUTOFF", 700, 0, 10, 0.7f, 1, 1, 0, 0, PO_RATIO, 0.15f, {0, 0, 0, 0}, 0},
	{"Stereo: R 120 Hz up", "SWEEP CUTOFF", 700, 0, 10, 0.7f, 1, 1, 0, 0, PO_HZ, 0.12f, {0, 0, 0, 0}, 0},
	{"Stereo: mirror", "SWEEP CUTOFF", 1000, 0, 4, 0.3f, 0, 0, 0, 0, PO_MIRROR, 0, {0, 0, 0, 0}, 0},
	{"Harmonic ladder, for a noise input", "TURN SHIFT", 200, 0, 20, 0, 1, 1, 0, 0, PO_LINKED, 0, {0, 1, 1.5849625f, 2}, 0},
	{"Order under DRIVE", "TURN DRIVE; SERIES ORDER IS IN THE MENU", 700, 0, 12, 0.9f, 1, 0, 0, 2.5f, PO_LINKED, 0, {0, 0, 0, 0}, 0},
};
static const int PO_NPRESETS = sizeof(PO_PRESETS) / sizeof(PO_PRESETS[0]);

// What one side is asked to do, resolved from the knobs, CVs and the relation.
struct PoCfg {
	float lfc = 0.f;            // log2 Hz of CUTOFF
	float lay[PO_N] = {0, 0, 0, 0};  // octaves from CUTOFF, per pair
	float shift = 0.f;          // Hz
	float q[PO_N] = {4, 4, 4, 4};
	float mode = 0.f, topo = 0.f, lag = 0.f, drive = 0.f;
};

struct Pole : Module {
	enum ParamId { CUTOFF_PARAM, SHIFT_PARAM, WIDTH_PARAM, SPREAD_PARAM, MODE_PARAM,
	               TOPO_PARAM, LAG_PARAM, DRIVE_PARAM, STEREO_PARAM, AMOUNT_PARAM, PARAMS_LEN };
	enum InputId { CUTOFF_INPUT, SHIFT_INPUT, WIDTH_INPUT, SPREAD_INPUT, MODE_INPUT,
	               TOPO_INPUT, LAG_INPUT, DRIVE_INPUT, STEREO_INPUT, AMOUNT_INPUT,
	               IN_L_INPUT, IN_R_INPUT, INPUTS_LEN };
	enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	// The eight knob values each side has (CUTOFF..DRIVE). In Independent the panel's
	// knobs are a view onto val[edit]; otherwise onto val[0], R following L.
	static const int NK = 8;
	float val[2][NK] = {};
	float trim[2][PO_N] = {};   // octaves, dragged on the screen
	float wtrim[2][PO_N] = {};  // octaves of Q (log2 multiplier), dragged on the screen
	int edit = 0;
	int editReq = -1;           // set by the screen, consumed in process()
	bool rInit = false;         // R has had values of its own

	// options
	int order = 0;
	int parEnd = 0;             // 0 = four bands, 1 = two chains of two
	int lagLead = 0;            // 0 = lowest pair leads, 1 = highest
	bool mirrorMode = true;     // Mirror turns lowpass into highpass
	bool levelHold = true;      // correct the level from the response

	struct Side {
		PoPair ser[PO_N], par[PO_N];
		float lf[PO_N] = {0, 0, 0, 0};
		bool lfInit = false;
		float lagA[PO_N] = {0, 0, 0, 0};
		float lagS[PO_N] = {-1, -1, -1, -1};
		float fCur[PO_N] = {0, 0, 0, 0}, gCur[PO_N] = {0, 0, 0, 0};
		float ks[PO_N] = {1, 1, 1, 1}, kp[PO_N] = {1, 1, 1, 1};
		float wl = 1, wb = 0, wh = 0;
		float topo = 0.f, d = 0.f, comp = 1.f;
		float gain = 1.f, gain1 = 1.f, gainT = 1.f;
		bool serOn = true, parOn = false;
		// targets set at control rate, followed per sample through the glide
		float kT[2][PO_N] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
		float wT[3] = {1, 0, 0}, topoT = 0.f, dT = 0.f, compT = 1.f;
		float lay[PO_N] = {0, 0, 0, 0}, shift = 0.f, knobLf = 0.f;
		bool smInit = false, snapGain = false;
		float cvSm = 0.f; bool cvInit = false;
		// published for the screen
		float pubF[PO_N] = {0, 0, 0, 0}, pubQs[PO_N] = {1, 1, 1, 1}, pubQp[PO_N] = {1, 1, 1, 1};
		float pubBw[PO_N] = {0, 0, 0, 0};
		float pubMode = 0.f, pubTopo = 0.f, pubGain = 1.f;
	};
	Side side[2];
	PoCfg cfg[2];
	int stereoNow = 0;          // the relation asked for (the knobs follow it at once)
	int stDsp = 0;              // the relation the filter is running (changed under the duck)
	int orderDsp = 0, parEndDsp = 0;
	float duck = 1.f;
	int ctl = 0;
	float sr = 48000.f;
	float aGlide = 1.f;
	// CUTOFF CV smoothing, ms (0 = none). A filter moved in one sample keeps
	// the signal stored in its states and that signal rings at the new
	// frequency: an envelope with no attack opening a closed bandpass gives a
	// ring at the top of the sweep, heard as a click. A 3 ms one-pole lowers
	// that ring by about 40 dB (tools/pole-harness.cpp). It also lowers
	// audio-rate FM, so Off is in the menu.
	float cvSlewMs = 3.f;
	float aCv = 1.f, aCvMs = -1.f;

	Pole() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(CUTOFF_PARAM, -4.f, 6.f, std::log2(800.f / dsp::FREQ_C4), "Cutoff", " Hz", 2.f, dsp::FREQ_C4);
		configParam<ShiftQ>(SHIFT_PARAM, -1.f, 1.f, 0.f, "Shift (every pair moved by the same number of Hz)");
		configParam<WidthQ>(WIDTH_PARAM, 0.f, 1.f, std::log(17.5f) / std::log(150.f), "Width");
		configParam(SPREAD_PARAM, 0.f, 2.f, 0.f, "Spread (octaves between neighbouring pairs)", " oct");
		configParam<ModeQ>(MODE_PARAM, 0.f, 3.f, 0.f, "Mode");
		configParam<TopoQ>(TOPO_PARAM, 0.f, 1.f, 0.f, "Topology");
		configParam<LagQ>(LAG_PARAM, 0.f, 1.f, 0.f, "Lag (the pairs follow CUTOFF at staggered speeds)");
		configParam<DriveQ>(DRIVE_PARAM, 0.f, 1.f, 0.f, "Drive (saturation before each series stage)");
		configSwitch(STEREO_PARAM, 0.f, (float)(PO_NSTEREO - 1), 0.f, "Stereo",
			{PO_STEREO_NAME[0], PO_STEREO_NAME[1], PO_STEREO_NAME[2], PO_STEREO_NAME[3], PO_STEREO_NAME[4], PO_STEREO_NAME[5]});
		configParam<AmountQ>(AMOUNT_PARAM, -1.f, 1.f, 0.f, "Amount");
		configInput(CUTOFF_INPUT, "Cutoff (1V/oct; on a 2-channel cable, channel 2 sets R)");
		configInput(SHIFT_INPUT, "Shift (200 Hz/V)");
		configInput(WIDTH_INPUT, "Width (±5 V = the whole range)");
		configInput(SPREAD_INPUT, "Spread (0.4 oct/V)");
		configInput(MODE_INPUT, "Mode (1 V per mode: lowpass, bandpass, highpass, notch)");
		configInput(TOPO_INPUT, "Topology (±5 V = series to parallel)");
		configInput(LAG_INPUT, "Lag (±5 V = the whole range)");
		configInput(DRIVE_INPUT, "Drive (±5 V = the whole range)");
		configInput(STEREO_INPUT, "Stereo relation (1 V per relation)");
		configInput(AMOUNT_INPUT, "Amount (±5 V = the whole range)");
		configInput(IN_L_INPUT, "Left");
		configInput(IN_R_INPUT, "Right (normalled from left)");
		configOutput(OUT_L_OUTPUT, "Left");
		configOutput(OUT_R_OUTPUT, "Right");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		for (int s = 0; s < 2; s++)
			for (int k = 0; k < NK; k++) val[s][k] = paramQuantities[k]->getDefaultValue();
	}

	// ── tooltips that read in the units the knob means ──
	struct ShiftQ : ParamQuantity {
		std::string getDisplayValueString() override { return string::f("%+.0f Hz", poShiftHz(getValue())); }
	};
	struct WidthQ : ParamQuantity {
		std::string getDisplayValueString() override {
			float b = poBwOct(getValue());
			return string::f("%.2f oct (Q %.1f)", b, poQ(b));
		}
	};
	struct ModeQ : ParamQuantity {
		std::string getDisplayValueString() override {
			static const char* N[4] = {"Lowpass", "Bandpass", "Highpass", "Notch"};
			float m = clamp(getValue(), 0.f, 3.f);
			int i = (int)std::lround(m);
			if (std::fabs(m - i) < 0.02f) return N[i];
			int a = std::min((int)m, 2);
			return string::f("%s %.0f%%, %s %.0f%%", N[a], 100.f * (1.f - (m - a)), N[a + 1], 100.f * (m - a));
		}
	};
	struct TopoQ : ParamQuantity {
		std::string getDisplayValueString() override {
			float t = clamp(getValue(), 0.f, 1.f);
			if (t < 0.005f) return "Series";
			if (t > 0.995f) return "Parallel";
			return string::f("Series %.0f%%, parallel %.0f%%", 100.f * (1.f - t), 100.f * t);
		}
	};
	struct LagQ : ParamQuantity {
		std::string getDisplayValueString() override {
			float s = poLagS(getValue());
			if (s < 0.0005f) return "Off";
			return s < 1.f ? string::f("%.0f ms for the slowest pair", s * 1000.f) : string::f("%.2f s for the slowest pair", s);
		}
	};
	struct DriveQ : ParamQuantity {
		std::string getDisplayValueString() override {
			float d = poDrive(getValue());
			if (d < 0.01f) return "Off";
			return string::f("%.1fx", d);
		}
	};
	struct AmountQ : ParamQuantity {
		std::string getDisplayValueString() override {
			Pole* m = dynamic_cast<Pole*>(module);
			float a = getValue();
			int st = m ? m->stereoNow : 0;
			switch (st) {
				case PO_RATIO: return string::f("R %+.2f oct from L", 2.f * a);
				case PO_HZ: return string::f("R %+.0f Hz from L", 1000.f * a);
				case PO_MIRROR: { float ax = poMirrorAxis(a);
					return ax >= 1000.f ? string::f("Mirrored about %.2f kHz", ax / 1000.f) : string::f("Mirrored about %.0f Hz", ax); }
				case PO_SPREADREL: return string::f("R's spread %.2f x L's", 1.f - 2.f * a);
				default: return string::f("%.2f (not used by %s)", a, PO_STEREO_NAME[st]);
			}
		}
	};

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		sr = e.sampleRate;
		aGlide = 1.f;                          // recomputed on the next sample
		for (int s = 0; s < 2; s++) for (int i = 0; i < PO_N; i++) { side[s].lagS[i] = -1.f; side[s].fCur[i] = 0.f; }
	}
	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		for (int s = 0; s < 2; s++) {
			for (int k = 0; k < NK; k++) val[s][k] = paramQuantities[k]->getDefaultValue();
			for (int i = 0; i < PO_N; i++) trim[s][i] = wtrim[s][i] = 0.f;
		}
		edit = 0; rInit = false;
		order = 0; parEnd = 0; lagLead = 0; mirrorMode = true; levelHold = true; cvSlewMs = 3.f;
	}

	// The hint a preset leaves on the screen (UI thread only).
	std::string hint;
	double hintUntil = 0.0;

	void applyPreset(int n) {
		const PoPreset& p = PO_PRESETS[clamp(n, 0, PO_NPRESETS - 1)];
		float k[NK];
		k[0] = std::log2(p.cutoffHz / dsp::FREQ_C4);
		k[1] = std::cbrt(p.shiftHz / 2000.f);
		// Q to bandwidth in octaves (the inverse of poQ), then to the knob
		float bw = 2.f / std::log(2.f) * std::asinh(1.f / (2.f * p.q));
		k[2] = clamp(std::log(bw / 0.02f) / std::log(150.f), 0.f, 1.f);
		k[3] = p.spread; k[4] = p.mode; k[5] = p.topo;
		k[6] = p.lagS > 0.f ? std::log(499.f * p.lagS + 1.f) / std::log(500.f) : 0.f;
		k[7] = std::sqrt(p.drive / 10.f);
		for (int s = 0; s < 2; s++) for (int j = 0; j < NK; j++) val[s][j] = k[j];
		for (int j = 0; j < NK; j++) params[j].setValue(k[j]);
		params[STEREO_PARAM].setValue((float)p.stereo);
		params[AMOUNT_PARAM].setValue(p.amount);
		for (int s = 0; s < 2; s++) for (int i = 0; i < PO_N; i++) { trim[s][i] = p.trim[i]; wtrim[s][i] = 0.f; }
		order = p.order; parEnd = 0; lagLead = 0; mirrorMode = true; levelHold = true;
		hint = p.move;
		hintUntil = system::getTime() + 15.0;
	}

	void resetPairs(int s) { for (int i = 0; i < PO_N; i++) trim[s][i] = wtrim[s][i] = 0.f; }
	void copySide(int from, int to) {
		for (int k = 0; k < NK; k++) val[to][k] = val[from][k];
		for (int i = 0; i < PO_N; i++) { trim[to][i] = trim[from][i]; wtrim[to][i] = wtrim[from][i]; }
		if (edit == to) editReq = to;      // push the copied values onto the knobs
	}

	// ── resolve one side's configuration (control rate, plus CUTOFF per sample) ──
	float cvOf(int in, int s) {
		Input& p = inputs[in];
		if (!p.isConnected()) return 0.f;
		return p.getPolyVoltage(s);         // mono: channel 0 for both; 2+ channels: R reads channel 1
	}
	void resolve(int s, int st, float amount) {
		bool own = st == PO_INDEP && s == 1;
		const float* v = val[own ? 1 : 0];
		const float* tr = trim[own ? 1 : 0];
		const float* wt = wtrim[own ? 1 : 0];
		PoCfg& c = cfg[s];
		float bw = poBwOct(v[2] + 0.1f * cvOf(WIDTH_INPUT, s));
		float q = poQ(bw);
		float spread = clamp(v[3] + 0.4f * cvOf(SPREAD_INPUT, s), -3.f, 3.f);
		c.shift = poShiftHz(v[1]) + 200.f * cvOf(SHIFT_INPUT, s);
		c.mode = clamp(v[4] + cvOf(MODE_INPUT, s), 0.f, 3.f);
		c.topo = clamp(v[5] + 0.1f * cvOf(TOPO_INPUT, s), 0.f, 1.f);
		c.lag = poLagS(v[6] + 0.1f * cvOf(LAG_INPUT, s));
		c.drive = poDrive(v[7] + 0.1f * cvOf(DRIVE_INPUT, s));
		float layMul = 1.f;
		if (s == 1 && st == PO_SPREADREL) layMul = 1.f - 2.f * amount;
		if (s == 1 && st == PO_MIRROR) layMul = -1.f;
		for (int i = 0; i < PO_N; i++) {
			c.lay[i] = (spread * PO_POS[i] + tr[i]) * layMul;
			c.q[i] = clamp(q * std::exp2(wt[i]), 0.5f, 200.f);
		}
		if (s == 1 && st == PO_HZ) c.shift += 1000.f * amount;
		if (s == 1 && st == PO_MIRROR) {
			c.shift = -c.shift;
			if (mirrorMode && c.mode <= 2.f) c.mode = 2.f - c.mode;
		}
	}
	// CUTOFF in log2 Hz is knob + sign * CV. The knob part (with the relation's
	// offsets) glides; the CV is used as it arrives, since it can be audio-rate
	// FM. Mirror reflects R, knob and CV both, about poMirrorAxis().
	float knobLf(int s, int st, float amount) {
		bool own = st == PO_INDEP && s == 1;
		float lf = std::log2(dsp::FREQ_C4) + val[own ? 1 : 0][0];
		if (s == 1) {
			if (st == PO_RATIO) lf += 2.f * amount;
			if (st == PO_MIRROR) lf = 2.f * std::log2(poMirrorAxis(amount)) - lf;
		}
		return lf;
	}
	float cvSign(int s, int st) const { return s == 1 && st == PO_MIRROR ? -1.f : 1.f; }
	float cutoffLf(int s, int st, float amount) { return knobLf(s, st, amount) + cvSign(s, st) * cvOf(CUTOFF_INPUT, s); }

	// Per-pair Q in series, shared with the pairs it overlaps (finding 1).
	static void shareQ(const float* f, const float* q, const int* members, int n, float* out) {
		for (int a = 0; a < n; a++) {
			int i = members[a];
			float bw = 1.f / (q[i] * 0.6931472f);
			float li = std::log2(f[i]), cnt = 0.f;
			for (int b = 0; b < n; b++) {
				float d = (li - std::log2(f[members[b]])) / bw;
				cnt += std::exp(-d * d);
			}
			out[i] = std::max(std::pow(q[i], 1.f / cnt), 0.5f);
		}
	}

	// |H| at a frequency, for the side as it is set now. qsv are the series
	// pairs' Q (shared across overlap), qpv the parallel end's.
	float magAt(const Side& S, float w, const float* qsv, const float* qpv) const {
		float tw = std::tan((float)M_PI * std::min(w, 0.49f * sr) / sr);
		PoC ser = {1, 0}, par = {0, 0};
		PoC h[PO_N];
		for (int i = 0; i < PO_N; i++) {
			float r = tw / std::max(S.gCur[i], 1e-9f);
			ser = poMul(ser, poPairH(r, qsv[i], S.wl, S.wb, S.wh));
			h[i] = poPairH(r, qpv[i], S.wl, S.wb, S.wh);
		}
		if (parEndDsp == 0) for (int i = 0; i < PO_N; i++) { par.r += 0.25f * h[i].r; par.i += 0.25f * h[i].i; }
		else {
			PoC a = poMul(h[0], h[1]), b = poMul(h[2], h[3]);
			par = {0.5f * (a.r + b.r), 0.5f * (a.i + b.i)};
		}
		float re = ser.r + (par.r - ser.r) * S.topo, im = ser.i + (par.i - ser.i) * S.topo;
		return std::sqrt(re * re + im * im);
	}

	// A pair's position in log2 Hz, converted to its frequency in Hz.
	float pairF(int s, float lf) const {
		return clamp(SFS_EXP2(clamp(lf, 2.f, 15.f)) + side[s].shift, PO_FMIN, 0.45f * sr);
	}
	void setPair(Side& S, int i, float f) {
		S.fCur[i] = f;
		float t = 0.5f * f / sr;                 // tan(pi f/sr) as sin/cos of half that many turns
		S.gCur[i] = SFS_SIN2PI(t) / SFS_COS2PI(t);
		S.ser[i].set(S.gCur[i], S.ks[i]);
		S.par[i].set(S.gCur[i], S.kp[i]);
	}

	void control(int s, int st, float amount) {
		Side& S = side[s];
		const PoCfg& c = cfg[s];
		poModeW(c.mode, S.wT[0], S.wT[1], S.wT[2]);
		S.topoT = c.topo;
		S.dT = c.drive;
		S.compT = c.drive > 1e-3f ? 0.5f / std::tanh(0.5f * c.drive) : 1.f;
		// The first update places everything where it is asked to be, with no
		// glide, and the pairs before their Q is shared out (or for one control
		// period every pair would sit at the floor, gathered).
		if (!S.smInit) {
			for (int i = 0; i < PO_N; i++) S.lay[i] = c.lay[i];
			S.shift = c.shift;
			S.knobLf = knobLf(s, st, amount);
			S.wl = S.wT[0]; S.wb = S.wT[1]; S.wh = S.wT[2];
			S.topo = S.topoT; S.d = S.dT; S.comp = S.compT;
			S.smInit = true;
		}
		if (!S.lfInit) {
			float lfc = cutoffLf(s, st, amount);
			for (int i = 0; i < PO_N; i++) { S.lf[i] = lfc + c.lay[i]; setPair(S, i, pairF(s, S.lf[i])); }
			S.lfInit = true;
		}
		float topo = S.topo;
		bool serOn = topo < 0.999f || S.topoT < 0.999f, parOn = topo > 0.001f || S.topoT > 0.001f;
		if (!serOn && S.serOn) for (auto& p : S.ser) p.reset();
		if (!parOn && S.parOn) for (auto& p : S.par) p.reset();
		S.serOn = serOn; S.parOn = parOn;
		// lag, staggered: the leading pair has none, the slowest all of LAG
		for (int i = 0; i < PO_N; i++) {
			int rank = lagLead == 0 ? i : PO_N - 1 - i;
			float t = c.lag * rank / (PO_N - 1);
			if (t != S.lagS[i]) { S.lagS[i] = t; S.lagA[i] = t <= 0.f ? 1.f : 1.f - std::exp(-1.f / (t * sr)); }
		}
		// Q: series shared across overlap (or within each chain at the parallel end)
		float f[PO_N], qs[PO_N], qc[PO_N];
		for (int i = 0; i < PO_N; i++) f[i] = std::max(S.fCur[i], PO_FMIN);
		static const int ALL[4] = {0, 1, 2, 3}, CA[2] = {0, 1}, CB[2] = {2, 3};
		shareQ(f, c.q, ALL, 4, qs);
		shareQ(f, c.q, CA, 2, qc); shareQ(f, c.q, CB, 2, qc);
		bool first = S.kT[0][0] < 0.f;
		for (int i = 0; i < PO_N; i++) {
			S.kT[0][i] = 1.f / qs[i];
			S.kT[1][i] = 1.f / (parEndDsp ? qc[i] : c.q[i]);
			if (first) { S.ks[i] = S.kT[0][i]; S.kp[i] = S.kT[1][i]; S.ser[i].set(S.gCur[i], S.ks[i]); S.par[i].set(S.gCur[i], S.kp[i]); }
		}
		// level, from the response's peak
		float gt = 1.f;
		float qsv[PO_N], qpv[PO_N];
		for (int i = 0; i < PO_N; i++) { qsv[i] = 1.f / S.ks[i]; qpv[i] = 1.f / S.kp[i]; }
		if (levelHold && S.gCur[0] > 0.f) {
			float pk = 1e-6f, qmax = 0.707f;
			for (int g = 0; g < PO_GRID; g++)
				pk = std::max(pk, magAt(S, 20.f * std::pow(1000.f, g / (float)(PO_GRID - 1)), qsv, qpv));
			for (int i = 0; i < PO_N; i++) {
				pk = std::max(pk, magAt(S, f[i], qsv, qpv));
				qmax = std::max(qmax, qpv[i]);
			}
			// The peak is allowed to rise with Q, by half as many dB as the Q
			// itself, so a resonant peak stays above the passband and a
			// gathered Butterworth set (Q 0.707) passes at unity.
			float target = std::sqrt(qmax / 0.707f);
			gt = clamp(target / pk, 1.f / 16.f, 64.f);
		}
		S.gainT = gt;
		if (S.snapGain) { S.gain = S.gain1 = gt; S.snapGain = false; }
		for (int i = 0; i < PO_N; i++) {
			S.pubF[i] = f[i]; S.pubQs[i] = qsv[i]; S.pubQp[i] = qpv[i];
			S.pubBw[i] = 1.f / (c.q[i] * 0.6931472f);
		}
		S.pubMode = c.mode; S.pubTopo = topo; S.pubGain = S.gainT;
	}

	// Put a side where it is asked to be on the next control update, with no
	// glide, its filters empty and its level correction already in place.
	void snapSide(int s) {
		Side& S = side[s];
		S.smInit = false; S.lfInit = false; S.kT[0][0] = -1.f; S.snapGain = true;
		for (int i = 0; i < PO_N; i++) { S.ser[i].reset(); S.par[i].reset(); }
	}

	void process(const ProcessArgs& args) override {
		if (sr != args.sampleRate || aGlide >= 1.f || aCvMs != cvSlewMs) {
			sr = args.sampleRate;
			aGlide = 1.f - std::exp(-1.f / (PO_GLIDE * sr));
			aCvMs = cvSlewMs;
			aCv = cvSlewMs > 0.f ? 1.f - std::exp(-1.f / (0.001f * cvSlewMs * sr)) : 1.f;
		}
		// the knobs are a view onto one side
		int st = clamp((int)std::lround(params[STEREO_PARAM].getValue() + (inputs[STEREO_INPUT].isConnected() ? inputs[STEREO_INPUT].getVoltage() : 0.f)), 0, PO_NSTEREO - 1);
		if (st != stereoNow) {
			if (st == PO_INDEP && !rInit) { copySide(0, 1); rInit = true; }
			stereoNow = st;
		}
		// Changes that cannot glide wait for the output to fade out, are made
		// at the bottom, and the output fades back in.
		bool pending = st != stDsp || clamp(order, 0, 3) != orderDsp || clamp(parEnd, 0, 1) != parEndDsp;
		if (pending && !side[0].smInit) { stDsp = st; orderDsp = clamp(order, 0, 3); parEndDsp = clamp(parEnd, 0, 1); pending = false; }
		float dStep = 1.f / (PO_DUCK * sr);
		if (pending) {
			duck -= dStep;
			if (duck <= 0.f) {
				duck = 0.f;
				// A new relation can move R by octaves and change its mode.
				// Gliding there sweeps its resonances across the input, so R
				// restarts at its new settings with empty filters and fades in.
				if (st != stDsp) snapSide(1);
				stDsp = st; orderDsp = clamp(order, 0, 3); parEndDsp = clamp(parEnd, 0, 1);
				ctl = 0;                       // re-resolve now, under the silence
			}
		} else if (duck < 1.f) duck = std::min(1.f, duck + dStep);
		int want = editReq;
		if (st != PO_INDEP && edit != 0) want = 0;
		if (want >= 0) {
			editReq = -1;
			edit = st == PO_INDEP ? clamp(want, 0, 1) : 0;
			for (int k = 0; k < NK; k++) params[k].setValue(val[edit][k]);
		}
		for (int k = 0; k < NK; k++) val[edit][k] = params[k].getValue();
		float amount = clamp(params[AMOUNT_PARAM].getValue() + 0.2f * (inputs[AMOUNT_INPUT].isConnected() ? inputs[AMOUNT_INPUT].getVoltage() : 0.f), -1.f, 1.f);

		if (ctl-- <= 0) {
			ctl = PO_CTL - 1;
			for (int s = 0; s < 2; s++) resolve(s, stDsp, amount);
			for (int s = 0; s < 2; s++) control(s, stDsp, amount);
		}

		float inL = inputs[IN_L_INPUT].getVoltage();
		float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() : inL;
		float x[2] = {inL * PO_IN, inR * PO_IN};
		float out[2];
		const int* ord = PO_ORDERS[orderDsp];
		const float duckG = duck * duck * (3.f - 2.f * duck);   // smoothstep: no corner at either end
		const float a = aGlide;
		for (int s = 0; s < 2; s++) {
			Side& S = side[s];
			const PoCfg& c = cfg[s];
			// the glide
			S.knobLf += (knobLf(s, stDsp, amount) - S.knobLf) * a;
			S.shift += (c.shift - S.shift) * a;
			S.wl += (S.wT[0] - S.wl) * a; S.wb += (S.wT[1] - S.wb) * a; S.wh += (S.wT[2] - S.wh) * a;
			S.topo += (S.topoT - S.topo) * a;
			S.d += (S.dT - S.d) * a; S.comp += (S.compT - S.comp) * a;
			float cv = cvOf(CUTOFF_INPUT, s);
			S.cvSm = S.cvInit ? S.cvSm + (cv - S.cvSm) * aCv : cv;
			S.cvInit = true;
			float lfc = S.knobLf + cvSign(s, stDsp) * S.cvSm;
			for (int i = 0; i < PO_N; i++) {
				S.lay[i] += (c.lay[i] - S.lay[i]) * a;
				float ks = S.ks[i] + (S.kT[0][i] - S.ks[i]) * a, kp = S.kp[i] + (S.kT[1][i] - S.kp[i]) * a;
				bool kMoved = ks != S.ks[i] || kp != S.kp[i];
				S.ks[i] = ks; S.kp[i] = kp;
				S.lf[i] += (lfc + S.lay[i] - S.lf[i]) * S.lagA[i];
				float f = pairF(s, S.lf[i]);
				if (f != S.fCur[i]) setPair(S, i, f);
				else if (kMoved) { S.ser[i].set(S.gCur[i], ks); S.par[i].set(S.gCur[i], kp); }
			}
			float ys = 0.f, yp = 0.f;
			if (S.serOn) {
				ys = x[s];
				bool sat = S.d > 1e-3f;
				for (int n = 0; n < PO_N; n++) {
					int i = ord[n];
					if (sat) ys = poSat(ys, S.d, S.comp);
					ys = S.ser[i].tick(ys, S.wl, S.wb, S.wh);
				}
			}
			if (S.parOn) {
				if (parEndDsp == 0) {
					for (int i = 0; i < PO_N; i++) yp += S.par[i].tick(x[s], S.wl, S.wb, S.wh);
					yp *= 0.25f;
				} else {
					float a = S.par[1].tick(S.par[0].tick(x[s], S.wl, S.wb, S.wh), S.wl, S.wb, S.wh);
					float b = S.par[3].tick(S.par[2].tick(x[s], S.wl, S.wb, S.wh), S.wl, S.wb, S.wh);
					yp = 0.5f * (a + b);
				}
			}
			float y = ys + (yp - ys) * S.topo;
			// The level correction falls within about a millisecond of the
			// filter getting louder (a slower fall lets a burst through) and
			// rises over about 10 ms. Two poles, because the target moves
			// in steps every control period and one pole leaves the corners of
			// those steps in the output, at 3 kHz and its harmonics.
			S.gain1 += (S.gainT - S.gain1) * (S.gainT < S.gain1 ? 0.04f : 0.002f);
			S.gain += (S.gain1 - S.gain) * (S.gain1 < S.gain ? 0.04f : 0.002f);
			out[s] = sfs::softClip(y * S.gain * duckG / PO_IN);
		}
		outputs[OUT_L_OUTPUT].setVoltage(out[0]);
		outputs[OUT_R_OUTPUT].setVoltage(out[1]);
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_t* v = json_array();
		for (int s = 0; s < 2; s++) for (int k = 0; k < NK; k++) json_array_append_new(v, json_real(val[s][k]));
		json_object_set_new(r, "values", v);
		json_t* t = json_array();
		for (int s = 0; s < 2; s++) for (int i = 0; i < PO_N; i++) {
			json_array_append_new(t, json_real(trim[s][i]));
			json_array_append_new(t, json_real(wtrim[s][i]));
		}
		json_object_set_new(r, "pairs", t);
		json_object_set_new(r, "edit", json_integer(edit));
		json_object_set_new(r, "rInit", json_boolean(rInit));
		json_object_set_new(r, "order", json_integer(order));
		json_object_set_new(r, "parEnd", json_integer(parEnd));
		json_object_set_new(r, "lagLead", json_integer(lagLead));
		json_object_set_new(r, "mirrorMode", json_boolean(mirrorMode));
		json_object_set_new(r, "levelHold", json_boolean(levelHold));
		json_object_set_new(r, "cvSmooth", json_real(cvSlewMs));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* v = json_object_get(r, "values"))
			for (int s = 0; s < 2; s++) for (int k = 0; k < NK; k++)
				if (json_t* e = json_array_get(v, s * NK + k)) val[s][k] = json_number_value(e);
		if (json_t* t = json_object_get(r, "pairs"))
			for (int s = 0; s < 2; s++) for (int i = 0; i < PO_N; i++) {
				if (json_t* e = json_array_get(t, (s * PO_N + i) * 2)) trim[s][i] = json_number_value(e);
				if (json_t* e = json_array_get(t, (s * PO_N + i) * 2 + 1)) wtrim[s][i] = json_number_value(e);
			}
		if (json_t* e = json_object_get(r, "edit")) edit = clamp((int)json_integer_value(e), 0, 1);
		if (json_t* e = json_object_get(r, "rInit")) rInit = json_boolean_value(e);
		if (json_t* e = json_object_get(r, "order")) order = clamp((int)json_integer_value(e), 0, 3);
		if (json_t* e = json_object_get(r, "parEnd")) parEnd = clamp((int)json_integer_value(e), 0, 1);
		if (json_t* e = json_object_get(r, "lagLead")) lagLead = clamp((int)json_integer_value(e), 0, 1);
		if (json_t* e = json_object_get(r, "mirrorMode")) mirrorMode = json_boolean_value(e);
		if (json_t* e = json_object_get(r, "levelHold")) levelHold = json_boolean_value(e);
		if (json_t* e = json_object_get(r, "cvSmooth")) cvSlewMs = clamp((float)json_number_value(e), 0.f, 50.f);
		// the knobs were restored as the side being edited; keep them in agreement
		for (int k = 0; k < NK; k++) val[edit][k] = params[k].getValue();
	}
};

// =============================================================================
// THE SCREEN: the response of each side over 20 Hz - 20 kHz, and the pairs as
// marks on the left side's curve. Drag a mark sideways to move that pair,
// up and down to narrow or widen it; double-click to reset it.
// =============================================================================
static Pole* polePreview() {
	static Pole* m = nullptr;
	if (m) return m;
	m = new Pole();
	m->params[Pole::SPREAD_PARAM].setValue(0.75f);
	m->params[Pole::MODE_PARAM].setValue(1.f);
	m->params[Pole::TOPO_PARAM].setValue(1.f);
	m->params[Pole::WIDTH_PARAM].setValue(0.45f);
	m->params[Pole::STEREO_PARAM].setValue((float)PO_RATIO);
	m->params[Pole::AMOUNT_PARAM].setValue(0.12f);
	int64_t frame = 0;
	sfs::previewRun(*m, 0.1f, frame);
	return m;
}

static const float PO_DB_TOP = 18.f, PO_DB_BOT = -42.f;

struct PoleDisplay : Widget {
	Pole* module = nullptr;
	std::shared_ptr<Font> font;
	int drag = -1;          // pair being dragged
	int hover = -1;
	Vec dragPos;

	// plot rectangle, in px
	float px0() const { return mm2px(3.f); }
	float px1() const { return box.size.x - mm2px(3.f); }
	float py0() const { return mm2px(6.5f); }
	float py1() const { return box.size.y - mm2px(5.f); }
	float xOfF(float f) const { return px0() + (std::log2(f / 20.f) / std::log2(1000.f)) * (px1() - px0()); }
	float yOfDb(float db) const { return py0() + (PO_DB_TOP - clamp(db, PO_DB_BOT, PO_DB_TOP)) / (PO_DB_TOP - PO_DB_BOT) * (py1() - py0()); }
	float octPerPx() const { return std::log2(1000.f) / (px1() - px0()); }

	float dbAt(const Pole::Side& S, float f) const {
		float mag = module->magAt(S, f, S.pubQs, S.pubQp) * (module->levelHold ? S.pubGain : 1.f);
		return 20.f * std::log10(std::max(mag, 1e-6f));
	}
	int markSide() const { return module && module->stereoNow == PO_INDEP ? module->edit : 0; }
	// Where a pair's mark is drawn: on its curve, kept inside the plot. A pair
	// below 20 Hz or above 20 kHz (SPREAD, SHIFT or a drag can put one there)
	// is held at the edge, and `out` is set to that edge (-1 left, 1 right).
	Vec markPos(int s, int i, int* out = nullptr) const {
		const Pole::Side& S = module->side[s];
		float x = xOfF(S.pubF[i]);
		int o = x < px0() ? -1 : x > px1() ? 1 : 0;
		if (out) *out = o;
		return Vec(clamp(x, px0(), px1()), clamp(yOfDb(dbAt(S, S.pubF[i])), py0() + 2.f, py1() - 2.f));
	}
	int hit(Vec p) const {
		if (!module) return -1;
		int s = markSide(), best = -1;
		float bd = mm2px(3.f);
		for (int i = 0; i < PO_N; i++) {
			float d = markPos(s, i).minus(p).norm();
			if (d < bd) { bd = d; best = i; }
		}
		return best;
	}
	bool tabHit(Vec p, int& which) const {
		if (!module || module->stereoNow != PO_INDEP) return false;
		float y0 = mm2px(0.8f), y1 = mm2px(5.2f);
		if (p.y < y0 || p.y > y1) return false;
		float xr = box.size.x - mm2px(3.f);
		if (p.x > xr - mm2px(5.f) && p.x <= xr) { which = 1; return true; }
		if (p.x > xr - mm2px(10.5f) && p.x <= xr - mm2px(5.5f)) { which = 0; return true; }
		return false;
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT) { Widget::onButton(e); return; }
		if (e.action == GLFW_PRESS) {
			int w;
			if (tabHit(e.pos, w)) { module->editReq = w; e.consume(this); return; }
			drag = hit(e.pos);
			if (drag >= 0) e.consume(this);
		}
	}
	void onDoubleClick(const DoubleClickEvent& e) override {
		int i = drag >= 0 ? drag : hover;
		if (!module || i < 0) return;
		int s = markSide();
		module->trim[s][i] = 0.f;
		module->wtrim[s][i] = 0.f;
		e.consume(this);
	}
	void onDragMove(const DragMoveEvent& e) override {
		if (!module || drag < 0) return;
		int s = markSide();
		float z = getAbsoluteZoom();
		Vec d = e.mouseDelta.div(z);
		bool fine = (APP->window->getMods() & RACK_MOD_MASK) == RACK_MOD_CTRL;
		float k = fine ? 0.1f : 1.f;
		// Under Mirror the right side's layout is negated, but the marks are
		// drawn on the left side's curve, so a drag moves the left side's pair.
		module->trim[s][drag] = clamp(module->trim[s][drag] + d.x * octPerPx() * k, -6.f, 6.f);
		module->wtrim[s][drag] = clamp(module->wtrim[s][drag] - d.y / (py1() - py0()) * 6.f * k, -4.f, 4.f);
	}
	void onDragEnd(const DragEndEvent& e) override { drag = -1; }
	void onHover(const HoverEvent& e) override { hover = hit(e.pos); Widget::onHover(e); }
	void onLeave(const LeaveEvent& e) override { hover = -1; }

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, mm2px(1.27f));
		nvgFillColor(args.vg, sfs::SCREEN_BG);
		nvgFill(args.vg);
	}

	void curvePath(NVGcontext* vg, const Pole::Side& S, int n) const {
		for (int k = 0; k <= n; k++) {
			float f = 20.f * std::pow(1000.f, k / (float)n);
			float x = xOfF(f), y = yOfDb(dbAt(S, f));
			if (k == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) return;
		if (!module) { module = polePreview(); drawLayer(args, layer); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		bool hasFont = font && font->handle >= 0;
		Pole* m = module;
		const Pole::Side& L = m->side[0];
		const Pole::Side& R = m->side[1];
		float x0 = px0(), x1 = px1(), y0 = py0(), y1 = py1();

		// grid: 100 Hz, 1 kHz, 10 kHz, and 0 dB
		nvgStrokeWidth(vg, 1.f);
		for (float f : {100.f, 1000.f, 10000.f}) {
			nvgBeginPath(vg); nvgMoveTo(vg, xOfF(f), y0); nvgLineTo(vg, xOfF(f), y1);
			nvgStrokeColor(vg, sfs::SCREEN_PURP); nvgStroke(vg);
		}
		nvgBeginPath(vg); nvgMoveTo(vg, x0, yOfDb(0.f)); nvgLineTo(vg, x1, yOfDb(0.f));
		nvgStrokeColor(vg, sfs::SCREEN_PMID); nvgStroke(vg);

		// does R differ from L enough to draw?
		bool differ = false;
		for (int i = 0; i < PO_N; i++)
			if (std::fabs(L.pubF[i] - R.pubF[i]) > 0.01f * L.pubF[i] || std::fabs(L.pubQp[i] - R.pubQp[i]) > 0.01f) differ = true;
		if (std::fabs(L.pubMode - R.pubMode) > 0.01f || std::fabs(L.pubTopo - R.pubTopo) > 0.01f) differ = true;

		nvgSave(vg);
		nvgScissor(vg, x0, y0, x1 - x0, y1 - y0);
		const int NPT = 180;
		// L: filled, what leaves the left jack
		nvgBeginPath(vg);
		curvePath(vg, L, NPT);
		nvgLineTo(vg, x1, y1); nvgLineTo(vg, x0, y1); nvgClosePath(vg);
		nvgFillColor(vg, nvgTransRGBA(sfs::SCREEN_BLUE, 60));
		nvgFill(vg);
		nvgBeginPath(vg); curvePath(vg, L, NPT);
		nvgStrokeColor(vg, sfs::SCREEN_BLUE); nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);
		if (differ) {
			nvgBeginPath(vg); curvePath(vg, R, NPT);
			nvgStrokeColor(vg, sfs::SCREEN_HOT); nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
		}
		nvgRestore(vg);

		// the pairs: a mark on the curve, a bar across its bandwidth
		auto marks = [&](int s, NVGcolor col, bool active) {
			const Pole::Side& S = m->side[s];
			for (int i = 0; i < PO_N; i++) {
				int out = 0;
				Vec p = markPos(s, i, &out);
				float half = 0.5f * S.pubBw[i];
				float xa = clamp(xOfF(S.pubF[i] * std::exp2(-half)), x0, x1), xb = clamp(xOfF(S.pubF[i] * std::exp2(half)), x0, x1);
				if (xb > xa) {
					nvgBeginPath(vg); nvgMoveTo(vg, xa, p.y); nvgLineTo(vg, xb, p.y);
					nvgStrokeColor(vg, nvgTransRGBA(col, 150)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
				}
				float rad = active && (i == hover || i == drag) ? mm2px(1.1f) : mm2px(0.8f);
				nvgBeginPath(vg);
				if (out) {
					// off the plot: a triangle at the edge, pointing past it
					float tip = p.x + out * rad * 0.6f, back = p.x - out * rad * 1.4f;
					nvgMoveTo(vg, tip, p.y); nvgLineTo(vg, back, p.y - rad); nvgLineTo(vg, back, p.y + rad); nvgClosePath(vg);
				} else nvgCircle(vg, p.x, p.y, rad);
				if (active) { nvgFillColor(vg, col); nvgFill(vg); }
				else { nvgStrokeColor(vg, col); nvgStrokeWidth(vg, 1.f); nvgStroke(vg); }
				if (active && hasFont) {
					sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
					nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
					nvgFillColor(vg, sfs::SCREEN_TEXT);
					nvgText(vg, p.x, p.y - mm2px(1.3f), string::f("%d", i + 1).c_str(), NULL);
				}
			}
		};
		int ms = markSide();
		if (differ) marks(1 - ms, ms == 0 ? sfs::SCREEN_HOT : sfs::SCREEN_BLUE, false);
		marks(ms, ms == 0 ? sfs::SCREEN_BLUE : sfs::SCREEN_HOT, true);

		if (!hasFont) return;
		// header: topology and mode on the left, stereo relation on the right
		sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, sfs::SCREEN_TEXT);
		float t = L.pubTopo;
		std::string topo = t < 0.005f ? "SERIES" : t > 0.995f ? (m->parEnd ? "2 CHAINS" : "PARALLEL")
		                 : string::f("SERIES %.0f%%", 100.f * (1.f - t));
		static const char* MN[4] = {"LP", "BP", "HP", "NOTCH"};
		int mi = clamp((int)std::lround(L.pubMode), 0, 3);
		nvgText(vg, mm2px(3.f), mm2px(3.f), (topo + "  " + MN[mi]).c_str(), NULL);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		float xr = box.size.x - mm2px(3.f);
		if (m->stereoNow == PO_INDEP) {
			for (int s = 0; s < 2; s++) {
				float bx = xr - (s == 0 ? mm2px(10.5f) : mm2px(5.f));
				bool on = m->edit == s;
				nvgBeginPath(vg); nvgRoundedRect(vg, bx, mm2px(1.f), mm2px(5.f), mm2px(4.f), mm2px(0.6f));
				nvgFillColor(vg, on ? (s == 0 ? sfs::SCREEN_DEEP : nvgTransRGBA(sfs::SCREEN_HOT, 120)) : sfs::SCREEN_PURP);
				nvgFill(vg);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, sfs::SCREEN_TEXT);
				nvgText(vg, bx + mm2px(2.5f), mm2px(3.f), s == 0 ? "L" : "R", NULL);
			}
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, xr - mm2px(11.5f), mm2px(3.f), "INDEPENDENT", NULL);
		} else {
			// the relation, with AMOUNT's value in it
			float a = clamp(m->params[Pole::AMOUNT_PARAM].getValue() + 0.2f * (m->inputs[Pole::AMOUNT_INPUT].isConnected() ? m->inputs[Pole::AMOUNT_INPUT].getVoltage() : 0.f), -1.f, 1.f);
			std::string st;
			switch (m->stereoNow) {
				case PO_RATIO: st = string::f("R %+.2f OCT", 2.f * a); break;
				case PO_HZ: st = string::f("R %+.0f HZ", 1000.f * a); break;
				case PO_MIRROR: { float ax = poMirrorAxis(a);
					st = ax >= 1000.f ? string::f("MIRROR ABOUT %.2f KHZ", ax / 1000.f) : string::f("MIRROR ABOUT %.0f HZ", ax); } break;
				case PO_SPREADREL: st = string::f("R SPREAD x%.2f", 1.f - 2.f * a); break;
				default: st = PO_STEREO_SHORT[clamp(m->stereoNow, 0, PO_NSTEREO - 1)];
			}
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, xr, mm2px(3.f), st.c_str(), NULL);
		}
		// foot: frequency axis, or the pair under the pointer
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		float fy = box.size.y - mm2px(2.4f);
		int show = drag >= 0 ? drag : hover;
		if (show >= 0) {
			const Pole::Side& S = m->side[ms];
			float f = S.pubF[show];
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			nvgText(vg, box.size.x * 0.5f, fy, string::f("PAIR %d   %s   Q %.1f   %+.2f oct", show + 1,
				f >= 1000.f ? string::f("%.2f kHz", f / 1000.f).c_str() : string::f("%.0f Hz", f).c_str(),
				m->cfg[ms].q[show], m->trim[ms][show]).c_str(), NULL);
		} else if (!m->hint.empty() && system::getTime() < m->hintUntil) {
			nvgFillColor(vg, sfs::SCREEN_HOT);
			nvgText(vg, box.size.x * 0.5f, fy, m->hint.c_str(), NULL);
		} else {
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, xOfF(100.f), fy, "100", NULL);
			nvgText(vg, xOfF(1000.f), fy, "1k", NULL);
			nvgText(vg, xOfF(10000.f), fy, "10k", NULL);
		}
	}
};

// ── the panel ──
static const float PO_SX = 5.08f, PO_SY = 10.16f, PO_SW = 91.44f, PO_SH = 38.1f;
static const float PO_X[5] = {12.7f, 31.75f, 50.8f, 69.85f, 88.9f};
static const float PO_Y_KNOB = 60.96f, PO_Y_KCV = 73.66f;
static const float PO_Y_TRIM = 90.17f, PO_Y_TCV = 100.33f;
static const float PO_Y_FOOT = 120.65f;
static const float PO_RULE_Y = 80.01f;

struct PoleWidget : ModuleWidget {
	PoleWidget(Pole* module) {
		setModule(module);
		setPanel(sfs::themedPanel("pole"));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("POLE");

		PoleDisplay* disp = new PoleDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(PO_SX, PO_SY));
		disp->box.size = mm2px(Vec(PO_SW, PO_SH));
		addChild(disp);

		// the five knobs, each over its CV
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(PO_X[0], PO_Y_KNOB)), module, Pole::CUTOFF_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[0], PO_Y_KCV)), module, Pole::CUTOFF_INPUT));
		lbl->rogan(PO_X[0], PO_Y_KNOB, "CUTOFF");
		lbl->link(PO_X[0], PO_Y_KNOB, PO_X[0], PO_Y_KCV);
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(PO_X[1], PO_Y_KNOB)), module, Pole::SHIFT_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[1], PO_Y_KCV)), module, Pole::SHIFT_INPUT));
		lbl->rogan(PO_X[1], PO_Y_KNOB, "SHIFT");
		lbl->link(PO_X[1], PO_Y_KNOB, PO_X[1], PO_Y_KCV);
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(PO_X[2], PO_Y_KNOB)), module, Pole::WIDTH_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[2], PO_Y_KCV)), module, Pole::WIDTH_INPUT));
		lbl->rogan(PO_X[2], PO_Y_KNOB, "WIDTH");
		lbl->link(PO_X[2], PO_Y_KNOB, PO_X[2], PO_Y_KCV);
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(PO_X[3], PO_Y_KNOB)), module, Pole::SPREAD_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[3], PO_Y_KCV)), module, Pole::SPREAD_INPUT));
		lbl->rogan(PO_X[3], PO_Y_KNOB, "SPREAD");
		lbl->link(PO_X[3], PO_Y_KNOB, PO_X[3], PO_Y_KCV);
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(PO_X[4], PO_Y_KNOB)), module, Pole::MODE_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[4], PO_Y_KCV)), module, Pole::MODE_INPUT));
		lbl->rogan(PO_X[4], PO_Y_KNOB, "MODE");
		lbl->link(PO_X[4], PO_Y_KNOB, PO_X[4], PO_Y_KCV);
		lbl->divider(PO_SX, PO_RULE_Y, PO_SX + PO_SW, PO_RULE_Y);

		// the five trimpots, each over its CV
		addParam(createParamCentered<Trimpot>(mm2px(Vec(PO_X[0], PO_Y_TRIM)), module, Pole::TOPO_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[0], PO_Y_TCV)), module, Pole::TOPO_INPUT));
		lbl->pairDown(PO_X[0], PO_Y_TRIM, PO_Y_TCV, "TOPOLOGY");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(PO_X[1], PO_Y_TRIM)), module, Pole::LAG_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[1], PO_Y_TCV)), module, Pole::LAG_INPUT));
		lbl->pairDown(PO_X[1], PO_Y_TRIM, PO_Y_TCV, "LAG");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(PO_X[2], PO_Y_TRIM)), module, Pole::DRIVE_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[2], PO_Y_TCV)), module, Pole::DRIVE_INPUT));
		lbl->pairDown(PO_X[2], PO_Y_TRIM, PO_Y_TCV, "DRIVE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(PO_X[3], PO_Y_TRIM)), module, Pole::STEREO_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[3], PO_Y_TCV)), module, Pole::STEREO_INPUT));
		lbl->pairDown(PO_X[3], PO_Y_TRIM, PO_Y_TCV, "STEREO");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(PO_X[4], PO_Y_TRIM)), module, Pole::AMOUNT_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[4], PO_Y_TCV)), module, Pole::AMOUNT_INPUT));
		lbl->pairDown(PO_X[4], PO_Y_TRIM, PO_Y_TCV, "AMOUNT");

		// the foot: inputs left, outputs on the plate
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[0], PO_Y_FOOT)), module, Pole::IN_L_INPUT));
		lbl->jack(PO_X[0], PO_Y_FOOT, "IN L");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(PO_X[1], PO_Y_FOOT)), module, Pole::IN_R_INPUT));
		lbl->jack(PO_X[1], PO_Y_FOOT, "IN R");
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(PO_X[3], PO_Y_FOOT)), module, Pole::OUT_L_OUTPUT));
		lbl->jackOnPlate(PO_X[3], PO_Y_FOOT, "OUT L");
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(PO_X[4], PO_Y_FOOT)), module, Pole::OUT_R_OUTPUT));
		lbl->jackOnPlate(PO_X[4], PO_Y_FOOT, "OUT R");
	}

	void appendContextMenu(Menu* menu) override {
		Pole* m = dynamic_cast<Pole*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Starting points", "", [=](Menu* sub) {
			sub->addChild(createMenuLabel("Each one shows on the screen which control to move"));
			for (int i = 0; i < PO_NPRESETS; i++)
				sub->addChild(createMenuItem(PO_PRESETS[i].name, "", [=]() { m->applyPreset(i); }));
		}));
		menu->addChild(createIndexPtrSubmenuItem("Parallel end of TOPOLOGY",
			{"Four bands side by side", "Two chains of two (pairs 1-2 beside pairs 3-4)"}, &m->parEnd));
		menu->addChild(createIndexPtrSubmenuItem("Series order (audible only with DRIVE up)",
			{PO_ORDER_NAME[0], PO_ORDER_NAME[1], PO_ORDER_NAME[2], PO_ORDER_NAME[3]}, &m->order));
		menu->addChild(createIndexPtrSubmenuItem("Pair that moves first under LAG",
			{"The lowest", "The highest"}, &m->lagLead));
		menu->addChild(createBoolPtrMenuItem("Mirror also swaps lowpass and highpass on R", "", &m->mirrorMode));
		{
			static const float MS[4] = {0.f, 1.f, 3.f, 10.f};
			menu->addChild(createIndexSubmenuItem("CUTOFF CV smoothing",
				{"Off (for audio-rate FM)", "1 ms", "3 ms (removes the click from a zero-attack envelope)", "10 ms"},
				[=]() { for (int i = 0; i < 4; i++) if (std::fabs(m->cvSlewMs - MS[i]) < 0.01f) return (size_t)i; return (size_t)2; },
				[=](size_t i) { m->cvSlewMs = MS[i]; }));
		}
		menu->addChild(createIndexSubmenuItem("Level",
			{"Corrected (gain holds the response's peak near a fixed level)", "Uncorrected (no gain after the filter)"},
			[=]() { return m->levelHold ? 0 : 1; }, [=](size_t i) { m->levelHold = i == 0; }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Drag a pair on the screen sideways to move it, up to narrow it"));
		menu->addChild(createMenuItem("Reset the pairs' positions and widths", "", [=]() {
			m->resetPairs(0); m->resetPairs(1); }));
		menu->addChild(createMenuItem("Copy L to R (Independent)", "", [=]() { m->copySide(0, 1); m->rInit = true; }));
		menu->addChild(createMenuItem("Copy R to L (Independent)", "", [=]() { m->copySide(1, 0); }));
	}
};

Model* modelPole = createModel<Pole, PoleWidget>("Pole");

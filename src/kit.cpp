#include "plugin.hpp"
#include "panel-style.hpp"
#include "membrane.hpp"
#include "polykit-messages.hpp"
#include "waveguide.hpp"   // softClip
#include <cmath>
#include <atomic>
#include <cstring>

// Kit -- a struck membrane, modelled mode by mode.
//
// The DSP lives in membrane.hpp so it can be measured without Rack in the way,
// which is how the mallet's 38 ms contact times and 296-bounce chatter were
// found. This file is the instrument around it: the parameters, the strike, and
// a head you can play with the mouse.

// ── tooltips that say something ─────────────────────────────────────────────
// "Size 45%" tells you nothing you could act on. A drum has a diameter, a head
// has a pitch, a beater has a weight and a contact time -- and every one of
// those is already implied by the knob, so printing the percentage instead is
// throwing information away. Each of these reads the module and reports the
// quantity the control actually sets.
struct KitSizeQ : ParamQuantity {
	std::string getDisplayValueString() override {
		// The range spans roughly a 6-inch splash to a 22-inch kick.
		float in = 6.f * std::pow(22.f / 6.f, clamp(getValue(), 0.f, 1.f));
		return string::f("%.0f in (%.0f cm)", in, in * 2.54f);
	}
};
struct KitTensionQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float st = (clamp(getValue(), 0.f, 1.f) - 0.5f) * 1.4f * 12.f;
		return string::f("%+.1f semitones", st);
	}
};
struct KitDecayQ : ParamQuantity {
	std::string getDisplayValueString() override {
		// Depends on SIZE as well, so read it rather than pretending it does not.
		float sz = 0.45f;
		if (module) sz = clamp(module->params[0].getValue(), 0.f, 1.f);
		float sec = 0.08f * std::pow(90.f, clamp(getValue(), 0.f, 1.f)) * (0.6f + 0.8f * sz);
		return sec < 1.f ? string::f("%.0f ms", sec * 1000.f) : string::f("%.2f s", sec);
	}
};
struct KitExciteQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float h = clamp(getValue(), 0.f, 1.f);
		// tau ~ pi*sqrt(m/k), the same numbers strike() uses
		float m = 0.05f - 0.035f * h, k = 1.0e8f * std::pow(0.1f, h);
		float ms = (float)M_PI * std::sqrt(m / k) * 1000.f * 6.f;   // ~ the measured span
		const char* n = h < 0.2f ? "soft felt" : h < 0.45f ? "felt mallet"
		              : h < 0.7f ? "hard mallet" : h < 0.9f ? "wood stick" : "hard stick";
		return string::f("%s, %.1f ms contact", n, ms);
	}
};
struct KitWeightQ : ParamQuantity {
	std::string getDisplayValueString() override {
		return string::f("%.0f g", clamp(getValue(), 0.3f, 2.f) * 35.f);
	}
};
struct KitMaterialQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = clamp(getValue(), 0.f, 1.f);
		const char* n = v < 0.12f ? "drum head" : v < 0.35f ? "stiff head"
		              : v < 0.6f ? "thin plate" : v < 0.85f ? "plate" : "gong";
		return string::f("%s (%.0f%%)", n, v * 100.f);
	}
};
struct KitAirQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = clamp(getValue(), 0.f, 1.f);
		const char* n = v < 0.1f ? "open, no shell" : v < 0.4f ? "shallow shell"
		              : v < 0.75f ? "deep shell" : "sealed kettle (pitched)";
		return string::f("%s (%.0f%%)", n, v * 100.f);
	}
};
struct KitToneQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = clamp(getValue(), 0.f, 1.f);
		const char* n = v < 0.2f ? "partials ring on" : v < 0.45f ? "bright"
		              : v < 0.7f ? "natural" : v < 0.9f ? "dark" : "partials die at once";
		return string::f("%s (%.0f%%)", n, v * 100.f);
	}
};
struct KitBendQ : ParamQuantity {
	std::string getDisplayValueString() override {
		return string::f("%.1f semitones at full strike", clamp(getValue(), 0.f, 1.f) * 4.2f);
	}
};
struct KitResoQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = clamp(getValue(), 0.6f, 1.6f);
		return string::f("%+.1f semitones vs the batter head", 12.f * std::log2(v));
	}
};

// Presets, written as what the instrument IS -- 110 Hz, 1.1 s -- and converted
// to knob positions by inverting the module's own mappings, so they cannot drift
// from what process() actually does with the numbers.
struct KitPreset {
	const char* name;
	float size, tension, material, air, decay, tone, couple, reso,
	      excite, muffle, bend, snare, snareTune, strikeY;
};
static const KitPreset KIT_PRESETS[] = {
	{"Tom",          0.510f, 0.50f, 0.00f, 0.00f, 0.581f, 0.50f, 0.40f, 1.12f, 0.75f, 0.00f, 0.71f, 0.00f, 0.52f, 0.46f},
	{"Floor tom",    0.654f, 0.50f, 0.00f, 0.00f, 0.640f, 0.45f, 0.45f, 1.08f, 0.70f, 0.05f, 0.86f, 0.00f, 0.52f, 0.41f},
	{"Timpani",      0.699f, 0.50f, 0.00f, 1.00f, 0.773f, 0.35f, 0.30f, 1.00f, 0.30f, 0.00f, 0.43f, 0.00f, 0.52f, 0.74f},
	{"Kick",         0.856f, 0.50f, 0.00f, 0.00f, 0.352f, 0.90f, 0.55f, 0.85f, 0.85f, 0.55f, 1.00f, 0.00f, 0.52f, 0.29f},
	{"Snare",        0.282f, 0.50f, 0.00f, 0.00f, 0.370f, 0.80f, 0.50f, 1.30f, 0.90f, 0.00f, 0.57f, 0.80f, 0.22f, 0.52f},
	{"Brush snare",  0.282f, 0.50f, 0.00f, 0.00f, 0.400f, 0.70f, 0.50f, 1.30f, 0.15f, 0.00f, 0.57f, 0.60f, 0.12f, 0.72f},
	{"Gong",         0.799f, 0.50f, 1.00f, 0.00f, 0.689f, 0.20f, 0.20f, 1.00f, 0.50f, 0.00f, 0.29f, 0.00f, 0.52f, 0.52f},
	{"Steel pan",    0.185f, 0.50f, 0.70f, 0.00f, 0.756f, 0.30f, 0.25f, 1.00f, 0.60f, 0.00f, 0.29f, 0.00f, 0.52f, 0.64f},
	{"Frame drum",   0.441f, 0.50f, 0.00f, 0.20f, 0.523f, 0.60f, 0.25f, 1.00f, 0.45f, 0.20f, 1.00f, 0.00f, 0.52f, 0.82f},
	{"Tabla",        0.221f, 0.50f, 0.15f, 0.50f, 0.658f, 0.55f, 0.30f, 1.00f, 0.80f, 0.35f, 1.00f, 0.00f, 0.52f, 0.70f},
	// The four below exist so the default kit is Fill's eight channels (KICK
	// SNR CHH OHH LOW HIGH CLAP BELL). A hat is a small stiff plate struck hard
	// near its edge and choked (closed) or let ring (open); a clap is a short
	// small drum that is nearly all wires; a bell is the steel pan's material
	// at a bell's size with a long decay and the highs kept.
	{"Closed hat",   0.120f, 0.60f, 1.00f, 0.00f, 0.120f, 0.25f, 0.20f, 1.00f, 1.00f, 0.55f, 0.00f, 0.00f, 0.52f, 0.72f},
	{"Open hat",     0.120f, 0.60f, 1.00f, 0.00f, 0.450f, 0.25f, 0.20f, 1.00f, 1.00f, 0.10f, 0.00f, 0.00f, 0.52f, 0.72f},
	{"Clap",         0.250f, 0.50f, 0.10f, 0.00f, 0.200f, 0.70f, 0.50f, 1.30f, 0.90f, 0.30f, 0.00f, 1.00f, 0.10f, 0.40f},
	{"Bell",         0.200f, 0.70f, 1.00f, 0.00f, 0.800f, 0.15f, 0.20f, 1.00f, 1.00f, 0.00f, 0.00f, 0.00f, 0.52f, 0.55f},
};
static const int KIT_NPRESET = (int)(sizeof(KIT_PRESETS) / sizeof(KIT_PRESETS[0]));
// Eight instruments: two poly channels each is the whole of a sixteen-channel
// cable, which is a good sign the size is right.
static const int KIT_N = 8;

struct Kit : Module {
	enum ParamId {
		SIZE_PARAM, TENSION_PARAM, STIFF_PARAM, AIR_PARAM,
		DECAY_PARAM, TONE_PARAM, COUPLE_PARAM, RESO_PARAM,
		EXCITE_PARAM, WEIGHT_PARAM, BEND_PARAM,
		STRIKEX_PARAM, STRIKEY_PARAM,
		MUFFLE_PARAM, MUFFLEANG_PARAM,
		SNARE_PARAM, SNARETHR_PARAM,
		LEVEL_PARAM, STRIKE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		GATE_INPUT, VEL_INPUT, VOCT_INPUT,
		STRIKEX_INPUT, STRIKEY_INPUT,
		SIZE_INPUT, TENSION_INPUT, STIFF_INPUT, AIR_INPUT, MUFFLE_INPUT,
		// APPENDED for the 2026-08 panel, which gives every voice control a CV
		// in. New entries go on the END: inputs serialise by index, so slotting
		// these in beside their siblings above would repatch every saved Kit.
		DECAY_INPUT, TONE_INPUT, EXCITE_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUT_OUTPUT, HEAD_OUTPUT, SNARE_OUTPUT, OUTPUTS_LEN };
	enum LightId { STRIKE_LIGHT, LIGHTS_LEN };

	// ── EIGHT INSTRUMENTS, ONE PANEL ────────────────────────────────────────
	// The panel's knobs are a VIEW onto the selected instrument, the way Beat's
	// controls edit one pattern: each instrument keeps its own copy of every
	// knob value, selecting a tab pushes that slot into the knobs, and from then
	// on the knobs ARE the slot -- copied back every sample, no change detection
	// to get wrong. Channel N of every cable is instrument N.
	struct Inst {
		sfs::Drum drum;
		float v[PARAMS_LEN] = {0.f};
		dsp::SchmittTrigger gateTrig;
		float lastVel = 0.6f, uiFlash = 0.f;
		float level = 0.f;                   // peak follower, for the meters
		int   ctl = 0;                       // control-rate countdown
		int   hold = 0;                      // samples to keep running after a strike
		// QUIET means "not ringing": the mode bank is skipped entirely, so the
		// cost of the module tracks how many drums are SOUNDING, not eight.
		bool  quiet = true;
		// what the screen draws this instrument from
		float dispR = 0.55f, dispA = 0.f, dispEnergy = 0.f;
		float dispSize = 0.45f, dispTens = 0.5f, dispAir = 0.f;
		float dispExcite = 0.7f, dispWires = 0.f, dispMuffle = 0.f, dispCouple = 0.4f;
		float dispStiff = 0.3f;
		float modeVis[sfs::Drum::NM] = {0.f};
		float micSeen[4] = {0.f, 0.f, 0.f, 0.f};
	};
	Inst inst[KIT_N];
	int edit = 0;                            // the instrument the knobs show
	std::atomic<int> editReq{-1};            // a tab click, consumed in process()
	dsp::SchmittTrigger strikeBtn;
	// A mouse strike is aimed at the instrument being edited.
	float pendVel = 0.f, pendX = 0.f, pendY = 0.f;
	bool  pendHit = false;
	int   headView = 1;                  // 0 = flat rings, 1 = 3D surface
	// STEREO PAIRS. The poly out always carries L and R per instrument; with
	// this off both channels of a pair are the mono tap, and the mono path is
	// bit-identical to what Kit was before stereo existed.
	bool  stereo = true;

	Kit() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// SIZE and TENSION both move the pitch, and for an ideal membrane that
		// is ALL they do -- the mode ratios are scale-invariant, so the two are
		// the same control. They only separate through stiffness, the cavity and
		// damping, which is why those exist. SIZE is the slow one: a big drum is
		// low and dark, and it stays dark when you tighten it.
		configParam<KitSizeQ>(SIZE_PARAM, 0.f, 1.f, 0.45f, "Drum size");
		configParam<KitTensionQ>(TENSION_PARAM, 0.f, 1.f, 0.5f, "Head tension");
		configParam<KitMaterialQ>(STIFF_PARAM, 0.f, 1.f, 0.f, "Material");
		configParam<KitAirQ>(AIR_PARAM, 0.f, 1.f, 0.f, "Air cavity");
		configParam<KitDecayQ>(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay");
		configParam<KitToneQ>(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone (how fast partials die)");
		configParam(COUPLE_PARAM, 0.f, 1.f, 0.4f, "Coupling between the two heads", "%", 0.f, 100.f);
		configParam<KitResoQ>(RESO_PARAM, 0.6f, 1.6f, 1.1f, "Resonant head tuning");
		configParam<KitExciteQ>(EXCITE_PARAM, 0.f, 1.f, 0.7f, "Beater");
		configParam<KitWeightQ>(WEIGHT_PARAM, 0.3f, 2.f, 1.f, "Beater weight");
		configParam<KitBendQ>(BEND_PARAM, 0.f, 1.f, 0.25f, "Bend (pitch drop after the hit)");
		configParam(STRIKEX_PARAM, -1.f, 1.f, 0.f, "Strike X");
		configParam(STRIKEY_PARAM, -1.f, 1.f, 0.42f, "Strike Y");
		configParam(MUFFLE_PARAM, 0.f, 1.f, 0.f, "Muffle (a hand on the head)", "%", 0.f, 100.f);
		configParam(MUFFLEANG_PARAM, -1.f, 1.f, 0.f, "Muffle angle");
		configParam(SNARE_PARAM, 0.f, 1.f, 0.f, "Snare wires", "%", 0.f, 100.f);
		configParam(SNARETHR_PARAM, 0.f, 1.f, 0.3f, "Wire tightness (loose buzz to tight snap)", "%", 0.f, 100.f);
		configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Level", "x");
		configButton(STRIKE_PARAM, "Strike");

		configInput(GATE_INPUT, "Trigger (poly: channel N fires instrument N; a mono cable fires instrument 1; PolyKit In to the left gives each instrument its own jack)");
		configInput(VEL_INPUT, "Velocity (0-10V)");
		configInput(VOCT_INPUT, "1V/oct");
		configInput(STRIKEX_INPUT, "Strike X CV (+/-5V)");
		configInput(STRIKEY_INPUT, "Strike Y CV (+/-5V)");
		configInput(SIZE_INPUT, "Size CV (+/-5V)");
		configInput(TENSION_INPUT, "Tension CV (+/-5V)");
		configInput(STIFF_INPUT, "Material CV (+/-5V)");
		configInput(AIR_INPUT, "Air CV (+/-5V)");
		configInput(MUFFLE_INPUT, "Muffle CV (+/-5V)");
		configInput(DECAY_INPUT, "Decay CV (+/-5V)");
		configInput(TONE_INPUT, "Tone CV (+/-5V)");
		configInput(EXCITE_INPUT, "Beater CV (+/-5V)");
		// OUT_OUTPUT is the POLY out: sixteen channels, instrument N on 2N (L)
		// and 2N+1 (R). HEAD/SNARE keep their ids -- outputs serialise by index
		// -- and carry the stereo MIX of all eight. Per-instrument head and wires
		// are summed, which is the price of eight instruments on one cable.
		configOutput(OUT_OUTPUT, "Poly: instrument N on channels 2N-1 (L) and 2N (R)");
		configOutput(HEAD_OUTPUT, "Mix L");
		configOutput(SNARE_OUTPUT, "Mix R");
		loadDefaultKit();
	}

	// ── PolyKit In, the expander to the left ────────────────────────────────
	// Every per-instrument input has two possible sources: channel c of the
	// poly cable on Kit's own jack, or that instrument's jack on the expander.
	// The expander's jack wins WHEN IT IS PATCHED, per jack, so the two can be
	// mixed: a poly gate from Fill and one instrument's tension from a knob.
	const PolyKitMessage* xp = nullptr;
	static int pkCol(int in) {
		switch (in) {
			case GATE_INPUT:    return PK_TRIG;   case VOCT_INPUT:    return PK_VOCT;
			case VEL_INPUT:     return PK_VEL;    case STRIKEX_INPUT: return PK_X;
			case STRIKEY_INPUT: return PK_Y;      case SIZE_INPUT:    return PK_SIZE;
			case TENSION_INPUT: return PK_TENS;   case STIFF_INPUT:   return PK_MAT;
			case AIR_INPUT:     return PK_AIR;    case DECAY_INPUT:   return PK_DECAY;
			case TONE_INPUT:    return PK_TONE;   case EXCITE_INPUT:  return PK_EXC;
			case MUFFLE_INPUT:  return PK_MUFFLE;
		}
		return -1;
	}
	// The voltage instrument c sees at input `in`, and whether anything is
	// feeding it at all. A mono poly cable reaches every instrument, as
	// getPolyVoltage has it -- the ordinary Rack convention for modulation.
	bool xin(int in, int c, float& out) {
		int col = pkCol(in);
		if (xp && col >= 0 && xp->on[col][c]) { out = xp->v[col][c]; return true; }
		if (inputs[in].isConnected()) { out = inputs[in].getPolyVoltage(c); return true; }
		return false;
	}
	// A knob value for instrument c, with that instrument's CV on top.
	inline float pvc(int c, int p, int in, float lo = 0.f, float hi = 1.f) {
		float v = inst[c].v[p], cv;
		if (xin(in, c, cv)) v += cv * 0.2f * (hi - lo);
		return clamp(v, lo, hi);
	}

	void pushSlotToParams() {
		for (int p = 0; p < PARAMS_LEN; p++)
			if (p != STRIKE_PARAM) params[p].setValue(inst[edit].v[p]);
	}

	// Control rate, per instrument. The mode layout involves a pow and two sqrts
	// per mode, so the eight are STAGGERED (see the ctl seeds in loadDefaultKit)
	// rather than all re-solved in the same sample.
	void control(int c) {
		Inst& I = inst[c];
		sfs::Drum& drum = I.drum;
		float size = pvc(c, SIZE_PARAM, SIZE_INPUT);
		float tens = pvc(c, TENSION_PARAM, TENSION_INPUT);
		// One pitch, from both. Size spans roughly a 22" kick to a 6" splash
		// and tension is a fifth either way on top of it.
		float f0 = 34.f * std::pow(11.f, 1.f - size) * std::pow(2.f, (tens - 0.5f) * 1.4f);
		float vo;
		if (xin(VOCT_INPUT, c, vo)) f0 *= std::pow(2.f, vo);
		drum.f0 = clamp(f0, 12.f, 4000.f);
		// The one place a drum's ABSOLUTE size matters. Everything about the
		// modes is scale-invariant -- that is the whole argument for SIZE and
		// TENSION being one control -- but the speed of sound is not a ratio,
		// so the distance from the strike to each mic is real metres and a
		// 22-inch kick images far wider than a 6-inch splash. Same expression
		// the SIZE tooltip prints, halved to a radius.
		drum.radiusM = 6.f * std::pow(22.f / 6.f, size) * 0.0127f;

		drum.stiff    = pvc(c, STIFF_PARAM, STIFF_INPUT);
		drum.air      = pvc(c, AIR_PARAM, AIR_INPUT);
		drum.couple   = I.v[COUPLE_PARAM];
		drum.resoTune = I.v[RESO_PARAM];
		// A big drum rings longer than a small one at the same tension, so
		// decay leans on size as well as on its own knob.
		float dk = pvc(c, DECAY_PARAM, DECAY_INPUT);
		drum.decay = 0.08f * std::pow(90.f, dk) * (0.6f + 0.8f * size);
		drum.tone  = pvc(c, TONE_PARAM, TONE_INPUT) * 1.3f;
		drum.muffle    = pvc(c, MUFFLE_PARAM, MUFFLE_INPUT);
		drum.muffleAng = I.v[MUFFLEANG_PARAM] * (float)M_PI;
		drum.bend      = I.v[BEND_PARAM] * 0.35f;
		drum.snareAmt  = I.v[SNARE_PARAM];
		drum.snareThr  = 0.04f + I.v[SNARETHR_PARAM] * 0.5f;
		drum.snareTight = 0.06f + (1.f - I.v[SNARETHR_PARAM]) * 0.3f;

		float xcv = 0.f, ycv = 0.f;
		xin(STRIKEX_INPUT, c, xcv); xin(STRIKEY_INPUT, c, ycv);
		float x = clamp(I.v[STRIKEX_PARAM] + xcv * 0.2f, -1.f, 1.f);
		float y = clamp(I.v[STRIKEY_PARAM] + ycv * 0.2f, -1.f, 1.f);
		float r = std::min(1.f, std::sqrt(x * x + y * y));
		drum.strikeR   = r * 0.97f;
		drum.strikeAng = std::atan2(y, x);
		I.dispR = r; I.dispA = drum.strikeAng;
		I.dispSize = size; I.dispTens = tens; I.dispAir = drum.air;
		I.dispMuffle = drum.muffle; I.dispCouple = drum.couple;
		I.dispStiff = drum.stiff;
		I.dispExcite = pvc(c, EXCITE_PARAM, EXCITE_INPUT);
		I.dispWires  = clamp(I.v[SNARE_PARAM], 0.f, 1.f);
		// modes first: updateStrike()'s excitation tilt reads ratio[], which
		// updateModes() computes. The other order used last frame's layout.
		drum.updateModes();
		drum.updateStrike();
		if (I.micSeen[0] != drum.micR[0]   || I.micSeen[1] != drum.micR[1]
		 || I.micSeen[2] != drum.micAng[0] || I.micSeen[3] != drum.micAng[1]) {
			I.micSeen[0] = drum.micR[0];   I.micSeen[1] = drum.micR[1];
			I.micSeen[2] = drum.micAng[0]; I.micSeen[3] = drum.micAng[1];
			drum.updateMics();
		}
		I.dispEnergy = drum.energy;
		for (int k = 0; k < sfs::Drum::NM; k++)
			I.modeVis[k] = I.quiet ? 0.f : std::fabs(drum.lo[k].value()) * drum.outGain;
	}

	void process(const ProcessArgs& args) override {
		// The expander, if one is standing to the left.
		xp = nullptr;
		if (leftExpander.module && leftExpander.module->model == modelPolyKitIn)
			xp = (const PolyKitMessage*)leftExpander.module->rightExpander.consumerMessage;

		// A tab click lands here, on the audio thread, so the knobs and the slot
		// can never disagree about which instrument they are.
		int er = editReq.exchange(-1);
		if (er >= 0 && er < KIT_N && er != edit) {
			edit = er;
			pushSlotToParams();
			inst[edit].ctl = 0;
		}
		// The knobs ARE the edit slot.
		for (int p = 0; p < PARAMS_LEN; p++)
			if (p != STRIKE_PARAM) inst[edit].v[p] = params[p].getValue();

		// ── strikes: the gate per channel, the button and the mouse on the
		//    instrument being edited ─────────────────────────────────────────
		bool fire[KIT_N] = {false};
		float vel[KIT_N];
		// A MONO gate fires instrument 1 only. getPolyVoltage would fire all
		// eight from one trigger, which is never what a mono cable into a kit
		// means; a mono cable into a poly drum module plays the first drum.
		int gch = inputs[GATE_INPUT].getChannels();
		for (int c = 0; c < KIT_N; c++) {
			vel[c] = inst[c].lastVel;
			float g = 0.f;
			if (xp && xp->on[PK_TRIG][c]) g = xp->v[PK_TRIG][c];
			else if (c < gch)              g = inputs[GATE_INPUT].getVoltage(c);
			if (inst[c].gateTrig.process(g, 0.1f, 1.f)) {
				float vv;
				vel[c] = xin(VEL_INPUT, c, vv) ? clamp(vv * 0.1f, 0.02f, 1.f) : 0.7f;
				fire[c] = true;
			}
		}
		if (strikeBtn.process(params[STRIKE_PARAM].getValue() > 0.5f)) { vel[edit] = 0.7f; fire[edit] = true; }
		if (pendHit) {
			pendHit = false; vel[edit] = pendVel;
			// A mouse strike sets its own place on the head, and the knobs
			// follow it, so the panel never disagrees with what you just played.
			params[STRIKEX_PARAM].setValue(pendX);
			params[STRIKEY_PARAM].setValue(pendY);
			inst[edit].v[STRIKEX_PARAM] = pendX;
			inst[edit].v[STRIKEY_PARAM] = pendY;
			inst[edit].ctl = 0;
			fire[edit] = true;
		}

		float mixL = 0.f, mixR = 0.f;
		outputs[OUT_OUTPUT].setChannels(2 * KIT_N);
		for (int c = 0; c < KIT_N; c++) {
			Inst& I = inst[c];
			sfs::Drum& drum = I.drum;
			drum.sr = args.sampleRate;
			if (fire[c]) {
				I.lastVel = vel[c];
				float hard = pvc(c, EXCITE_PARAM, EXCITE_INPUT);
				// Velocity is a real approach speed, so everything downstream --
				// contact time, brightness, how far the pitch bends -- follows
				// from the collision rather than from a curve drawn over the top.
				drum.strike(0.4f + vel[c] * 9.f, hard, I.v[WEIGHT_PARAM]);
				I.uiFlash = 1.f;
				I.quiet = false;
				// The mallet has a pre-contact gap and the energy follower is
				// slow, so a fresh strike is held awake long enough to be heard.
				I.hold = (int)(args.sampleRate * 0.25f);
				I.ctl = 0;
			}
			if (--I.ctl <= 0) { I.ctl = 32; control(c); }

			float lvl = I.v[LEVEL_PARAM];
			float L = 0.f, R = 0.f;
			if (!I.quiet) {
				float head[2] = {0.f, 0.f}, snare[2] = {0.f, 0.f};
				drum.process(head, snare, stereo);
				if (!stereo) { head[1] = head[0]; snare[1] = snare[0]; }
				L = sfs::softClip((head[0] + snare[0]) * lvl * 5.f);
				R = sfs::softClip((head[1] + snare[1]) * lvl * 5.f);
				if (I.hold > 0) I.hold--;
				// -88 dB below a full hit, in the follower's own units (it runs
				// ahead of outGain and the 5 V scaling).
				else if (drum.energy < 2e-8f) { I.quiet = true; drum.clear(); }
			}
			outputs[OUT_OUTPUT].setVoltage(L, 2 * c);
			outputs[OUT_OUTPUT].setVoltage(R, 2 * c + 1);
			mixL += L; mixR += R;

			// A peak follower with a slow fall, so the meters compare what each
			// drum just did rather than flickering with the waveform.
			float a = std::max(std::fabs(L), std::fabs(R)) * 0.2f;
			I.level = (a > I.level) ? a : I.level * (1.f - 6.f * args.sampleTime);
			I.uiFlash -= I.uiFlash * 6.f * args.sampleTime;
		}
		// Eight drums summed can exceed what one could, so the mix is clipped
		// again after the sum.
		outputs[HEAD_OUTPUT].setChannels(1);
		outputs[SNARE_OUTPUT].setChannels(1);
		outputs[HEAD_OUTPUT].setVoltage(sfs::softClip(mixL));
		outputs[SNARE_OUTPUT].setVoltage(sfs::softClip(mixR));
		lights[STRIKE_LIGHT].setBrightness(inst[edit].uiFlash);
	}

	// A preset into a SLOT, not into the knobs: the knobs follow if it is the
	// slot they are showing. Presets carry the voice; WEIGHT, the muffle angle
	// and LEVEL are left as they are.
	void presetToSlot(int s, const KitPreset& p) {
		if (s < 0 || s >= KIT_N) return;
		float* v = inst[s].v;
		v[SIZE_PARAM] = p.size;      v[TENSION_PARAM] = p.tension;
		v[STIFF_PARAM] = p.material; v[AIR_PARAM] = p.air;
		v[DECAY_PARAM] = p.decay;    v[TONE_PARAM] = p.tone;
		v[COUPLE_PARAM] = p.couple;  v[RESO_PARAM] = p.reso;
		v[EXCITE_PARAM] = p.excite;  v[MUFFLE_PARAM] = p.muffle;
		v[BEND_PARAM] = p.bend;      v[SNARE_PARAM] = p.snare;
		v[SNARETHR_PARAM] = p.snareTune;
		// Straight up the head: the radius is the tone control, the angle only
		// matters against the muffle.
		v[STRIKEX_PARAM] = 0.f;      v[STRIKEY_PARAM] = p.strikeY;
		if (s == edit) pushSlotToParams();
		inst[s].ctl = 0;                       // re-solve the layout on the next sample
	}
	void loadPreset(int i) {
		if (i < 0 || i >= KIT_NPRESET) return;
		presetToSlot(edit, KIT_PRESETS[i]);
	}
	static int presetIndex(const char* name) {
		for (int i = 0; i < KIT_NPRESET; i++)
			if (std::strcmp(KIT_PRESETS[i].name, name) == 0) return i;
		return -1;
	}
	// The default kit: a slot for every preset the engine was measured against,
	// in playing order. A new Kit, and an old patch's instruments 2-8.
	void loadDefaultKit() {
		// Fill's eight channels, in Fill's order, so a poly cable from Fill's
		// outs plays the right drum on every channel with nothing to map.
		static const char* NAMES[KIT_N] = {"Kick", "Snare", "Closed hat", "Open hat",
		                                   "Floor tom", "Tom", "Clap", "Bell"};
		for (int s = 0; s < KIT_N; s++) {
			for (int p = 0; p < PARAMS_LEN; p++)
				inst[s].v[p] = paramQuantities[p] ? paramQuantities[p]->getDefaultValue() : 0.f;
			int i = presetIndex(NAMES[s]);
			presetToSlot(s, KIT_PRESETS[i >= 0 ? i : (s % KIT_NPRESET)]);
			inst[s].ctl = 4 * s;                 // stagger the control-rate updates
			resetMics(s);
		}
		pushSlotToParams();
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "headView", json_integer(headView));
		json_object_set_new(r, "stereo", json_boolean(stereo));
		json_object_set_new(r, "edit", json_integer(edit));
		json_t* arr = json_array();
		for (int s = 0; s < KIT_N; s++) {
			json_t* o = json_object();
			json_t* vv = json_array();
			for (int p = 0; p < PARAMS_LEN; p++) json_array_append_new(vv, json_real(inst[s].v[p]));
			json_object_set_new(o, "v", vv);
			// Where the pair is standing is part of the instrument, not a
			// preference: a kick and a splash want their mics in different places.
			json_t* mp = json_array();
			for (int c = 0; c < 2; c++) json_array_append_new(mp, json_real(inst[s].drum.micR[c]));
			for (int c = 0; c < 2; c++) json_array_append_new(mp, json_real(inst[s].drum.micAng[c]));
			json_object_set_new(o, "mics", mp);
			json_array_append_new(arr, o);
		}
		json_object_set_new(r, "inst", arr);
		return r;
	}
	void readMics(json_t* mp, sfs::Drum& d) {
		if (!mp || json_array_size(mp) != 4) return;
		for (int c = 0; c < 2; c++)
			d.micR[c] = clamp((float)json_real_value(json_array_get(mp, c)), 0.10f, 0.92f);
		for (int c = 0; c < 2; c++)
			d.micAng[c] = (float)json_real_value(json_array_get(mp, 2 + c));
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "headView"))
			headView = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(r, "stereo"))
			stereo = json_boolean_value(j);
		json_t* arr = json_object_get(r, "inst");
		if (!arr) {
			// A PRE-2026-09 PATCH: one drum. Its knobs are already in params
			// (paramsFromJson runs first), so they become instrument 1, its mics
			// come with it, and instruments 2-8 stay the default kit. A mono
			// gate still fires instrument 1, channel 0 of the poly out is still
			// instrument 1's left, so the patch sounds exactly as it did.
			edit = 0;
			for (int p = 0; p < PARAMS_LEN; p++)
				if (p != STRIKE_PARAM) inst[0].v[p] = params[p].getValue();
			readMics(json_object_get(r, "mics"), inst[0].drum);
			return;
		}
		for (int s = 0; s < KIT_N && s < (int)json_array_size(arr); s++) {
			json_t* o = json_array_get(arr, s);
			json_t* vv = json_object_get(o, "v");
			if (vv)
				for (int p = 0; p < PARAMS_LEN && p < (int)json_array_size(vv); p++)
					inst[s].v[p] = (float)json_real_value(json_array_get(vv, p));
			readMics(json_object_get(o, "mics"), inst[s].drum);
			inst[s].ctl = 0;
		}
		if (json_t* j = json_object_get(r, "edit"))
			edit = clamp((int)json_integer_value(j), 0, KIT_N - 1);
		pushSlotToParams();
	}

	// The tuned default pair, taken from a fresh engine rather than retyped --
	// the numbers were measured, and a second copy of them here is a second
	// thing to forget to update.
	void resetMics(int s) {
		sfs::Drum d;
		for (int c = 0; c < 2; c++) { inst[s].drum.micR[c] = d.micR[c]; inst[s].drum.micAng[c] = d.micAng[c]; }
	}
	void onReset() override {
		for (int s = 0; s < KIT_N; s++) inst[s].drum.clear();
		loadDefaultKit();
	}
	void onSampleRateChange() override {
		for (int s = 0; s < KIT_N; s++) {
			inst[s].drum.sr = APP->engine->getSampleRate();
			inst[s].drum.clear();
		}
	}
};

// ── the head ────────────────────────────────────────────────────────────────
// A drum seen from above. It is not a picture of the module's settings, it is
// the drum: click it to play it, and where you click is where it is struck.
struct KitDisplay : OpaqueWidget {
	Kit* module = nullptr;
	std::shared_ptr<Font> font;
	Vec dragFrom;
	// THE INSTRUMENT BEING DRAWN. The main view draws the edited one; the tab
	// strip points this at each of the eight in turn. Null in the browser.
	const Kit::Inst* S = nullptr;
	const Kit::Inst* cur() const { return module ? &module->inst[module->edit] : nullptr; }
	sfs::Drum& md() const { return module->inst[module->edit].drum; }
	// The tab strip along the foot, and the main view above it.
	float stripH() const { return mm2px(12.f); }
	float mainH()  const { return box.size.y - stripH(); }

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) return;
		// Load the face here, not in the constructor: the window may not exist
		// yet when the widget is built. Leaving it unloaded is a null deref the
		// first time anything draws text -- f->handle on a null shared_ptr, which
		// faults at address 0x8 and takes Rack down the moment Kit is placed.
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		nvgScissor(args.vg, RECT_ARGS(Rect(Vec(0, 0), box.size)));
		S = cur();
		if (!module) drawPreview(args); else drawLive(args);
		drawTabs(args);
		nvgResetScissor(args.vg);
	}

	// MATERIAL, drawn. Every other macro had a picture and this one did not,
	// which made it the knob you turned without knowing whether anything had
	// happened. A head's material shows in its SURFACE: skin and mylar are matte
	// and warm, and the stiffer the material gets the more it behaves like
	// metal -- colder, and with a tight specular highlight instead of a soft
	// sheen. So stiffness moves the fill from warm to blue-steel and pulls the
	// highlight in. It is a tint and one gradient, deliberately: the mode rings
	// are drawn on top of this and are the thing you are meant to be reading.
	float stiffAmt() const { return module ? clamp(S->dispStiff, 0.f, 1.f) : 0.3f; }

	void head(const DrawArgs& args, float cx, float cy, float rad) {
		float st = stiffAmt();
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, rad);
		nvgFillColor(args.vg, nvgRGBf(0.20f - 0.07f * st,
		                              0.16f - 0.02f * st,
		                              0.17f + 0.09f * st));
		nvgFill(args.vg);
		// the sheen: broad and faint on skin, tight and bright on metal
		{
			float hx = cx - rad * 0.34f, hy = cy - rad * 0.34f;
			NVGpaint g = nvgRadialGradient(args.vg, hx, hy,
			                               rad * (0.02f + 0.30f * (1.f - st)),
			                               rad * (0.55f + 0.45f * (1.f - st)),
			                               nvgRGBAf(0.62f, 0.72f, 0.95f,
			                                        0.05f + 0.20f * st),
			                               nvgRGBAf(0.62f, 0.72f, 0.95f, 0.f));
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, cy, rad);
			nvgFillPaint(args.vg, g);
			nvgFill(args.vg);
		}
		nvgStrokeColor(args.vg, sfs::SCREEN_LINE);
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
		// the hoop
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, rad * 0.93f);
		nvgStrokeColor(args.vg, sfs::SCREEN_PURP);
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStroke(args.vg);
	}

	// The standing wave actually on the head right now: sum over modes of
	// amplitude * J_m(j_mn r) * cos(m theta). Drawn as rings rather than a full
	// raster because the radial part is what the strike controls, and a ring
	// reads as a drum head where a heat map reads as a physics demo.
	// Where the head sits in a wide screen: centred vertically, tucked left, so
	// the remaining width is a usable panel rather than padding.
	float headRad() const { return mainH() * 0.5f - mm2px(1.2f); }
	// CENTRED. It used to be tucked against the left edge to leave a column for
	// the spectrum; with the scope moved into a corner there is nothing to make
	// room for, and a drum head off to one side of a wide screen reads as a
	// mistake rather than as a layout.
	float headCx()  const { return box.size.x * 0.5f; }

	// The head as a surface rather than a plan. The 2D view can only show the
	// RADIAL part of a mode, because a flat ring has one value; the whole point
	// of a drum is that the modes have angular shape too -- (m,1) has m nodal
	// diameters -- and that is invisible until you tilt it and give it height.
	//
	// The head is drawn as wide as the screen allows rather than as wide as the
	// screen is TALL. Tilted at 0.4 the disc's vertical extent is only 0.8 of
	// its width, so fitting it to the height wasted most of the radius it could
	// have had.
	static constexpr float TILT = 0.40f;

	// SIZE is drawn, not just heard: a 22-inch kick should look like one next to
	// a 6-inch splash. The range is kept off zero so a small drum is still a
	// drum rather than a dot.
	float sizeScale() const {
		return 0.52f + 0.48f * (module ? clamp(S->dispSize, 0.f, 1.f) : 0.55f);
	}
	// EVERY drum here has two heads and a shell between them -- COUPLE and RESO
	// only mean anything because it does -- so the shell is always drawn, and its
	// depth simply follows the drum's size the way a real one does.
	//
	// It used to follow AIR, which was wrong twice: AIR is the CAVITY morph, the
	// thing that pulls the modes into a kettledrum's harmonic series, and a kick
	// wants none of that while having the deepest shell of anything here. So the
	// kick rendered with no shell at all. AIR now closes the BOTTOM instead,
	// which is what a kettle actually is: one head over a sealed bowl.
	// DEPTH, AS A FRACTION OF THE RADIUS. It was 0.30, which is a depth of 15%
	// of the diameter -- a tambourine, not a drum. Real shells run far deeper:
	// a 14x5.5 snare is 0.79 of its radius, a 12x9 tom is 1.5. It also had a
	// consequence nobody would predict from the number: the bottom head's plane
	// sat at cy + 0.30r while the TOP head's front rim reaches cy + 0.40r, so
	// the plane the snare wires live in passed INSIDE the head's own silhouette
	// and the wires came out strung across the rim like a bracelet. There was
	// no honest place to put them until the drum had a depth.
	static constexpr float SHELL = 0.52f;
	float shellDepth(float rad) const { return rad * SHELL; }
	float airAmt() const {
		return module ? clamp(S->dispAir, 0.f, 1.f) : 0.35f;
	}
	float head3Rad() const {
		// Centred on the WHOLE screen, so the room either side has to clear the
		// spectrum on the right -- otherwise "centred" and "does not overlap"
		// cannot both be true.
		float wAvail = box.size.x - 2.f * mm2px(3.f);
		// The divisor is the object's own height in radii: the mode rise on top,
		// the near and far halves of the tilted disc, and the shell. The second
		// 0.34 used to stand in for the shell and happened to be close to it;
		// now that the shell is a named constant the sum says what it means, and
		// deepening the shell cannot silently push the drum off the bottom.
		float byH = (mainH() - mm2px(5.f)) / (0.34f + 2.f * TILT + SHELL);
		return std::min(wAvail * 0.5f, byH) * sizeScale();
	}
	// The scope is a CORNER READOUT now, not a column. As a full-height column
	// it took a fifth of the screen to say what the head already says, and it
	// pushed the head off centre to do it. Small and out of the way it still
	// answers the one question the head cannot: which partials are sounding.
	float scopeW() const { return std::min(mm2px(24.f), box.size.x * 0.28f); }
	float scopeH() const { return std::min(mm2px(9.f), mainH() * 0.24f); }
	void drawScope(const DrawArgs& args) {
		float x1 = box.size.x - mm2px(2.f), x0 = x1 - scopeW();
		float y1 = mainH() - mm2px(2.f), y0 = y1 - scopeH();
		drawSpectrum(args, x0, y0, x1, y1);
	}
	// The object is rise + tilted disc + shell, so its centre is not the box's.
	// Both the surface and the strike mark must agree about this or the mark
	// floats off the head.
	float head3Cy() const {
		float rad = head3Rad(), hgt = rad * 0.34f;
		float shell = shellDepth(rad);
		return mainH() * 0.5f - (shell - hgt) * 0.5f;
	}
	float head3Cx() const { return box.size.x * 0.5f; }

	// Parametrised by where and how big, because the same drawing serves the
	// main view and the eight small ones on the tabs: a tab is not an icon of a
	// drum, it IS that instrument drawn the same way, at the same tilt, going
	// through the same animation when it is struck. RINGS/SECT are the detail.
	static const int RMAX = 12, SMAX = 36;
	// Stroke weight for the drawing in hand. The main view is 1; a tab is a
	// third the size and at full weight its rings merged into a solid disc.
	float lineScale = 1.f;
	void drawHead3D(const DrawArgs& args, float cx, float cy, float rad, int RINGS, int SECT) {
		const int ARCS = 6;
		RINGS = std::min(RINGS, RMAX); SECT = std::min(SECT, SMAX);
		const sfs::MembraneShapes& sh = sfs::membraneShapes();
		float hgt = rad * 0.34f;
		// The shell. Tied to AIR, which IS the cavity, so the picture says
		// something rather than being a constant box.
		float shell = shellDepth(rad), airv = airAmt();
		float sa = module ? S->dispA : 0.f;

		static float ang[sfs::Drum::NM][SMAX + 1];
		for (int k = 0; k < sfs::Drum::NM; k++) {
			int mm = sfs::MEMBRANE_MODES[k].m;
			for (int j = 0; j <= SECT; j++)
				ang[k][j] = std::cos(mm * (2.f * (float)M_PI * (float)j / SECT - sa));
		}
		float z[RMAX + 1][SMAX + 1];
		float zmax = 1e-6f;
		for (int i = 0; i <= RINGS; i++) {
			float u = (float)i / RINGS;
			for (int j = 0; j <= SECT; j++) {
				float v = 0.f;
				for (int k = 0; k < sfs::Drum::NM; k++) {
					float a = module ? S->modeVis[k] : 0.55f * std::exp(-k * 0.30f);
					if (a < 1e-4f) continue;
					v += a * sh.at(k, u) * ang[k][j];
				}
				z[i][j] = v;
				zmax = std::max(zmax, std::fabs(v));
			}
		}
		float zs = hgt / std::max(zmax, 0.35f);

		// A taut head pulls its grid out toward the rim; a slack one lets it
		// gather in the middle. Same rings, redistributed -- which is what
		// tension does to a real head's response, and it reads instantly.
		float tens = module ? clamp(S->dispTens, 0.f, 1.f) : 0.5f;
		float warp = 1.f - 0.55f * (tens - 0.5f);        // <1 pushes rings outward
		auto PX = [&](int i, int j, float& X, float& Y) {
			float u = std::pow((float)i / RINGS, warp);
			float th = 2.f * (float)M_PI * (float)j / SECT;
			X = cx + std::cos(th) * u * rad;
			Y = cy + std::sin(th) * u * rad * TILT - z[i][j] * zs;
		};
		// Excited areas warm toward the plugin's orange. Depth cue on top: the
		// far half of the surface sits back a little.
		auto col = [&](float amp, float th) {
			float hot = clamp(amp, 0.f, 1.f);
			float far = 0.72f + 0.28f * (0.5f + 0.5f * std::sin(th));
			return nvgRGBAf(0.00f + 0.92f * hot, 0.59f - 0.19f * hot, 0.87f - 0.69f * hot,
			                (0.30f + 0.60f * hot) * far);
		};

		nvgLineCap(args.vg, NVG_ROUND);

		// The snare goes down FIRST, before the shell and before the head, so
		// every line of the drum crosses over it. Drawn last it sat on top of
		// the shell and read as a band strapped round the outside; underneath
		// everything, it reads as hardware slung below the body.
		if (airv < 0.85f) drawWires(args, cx, cy, rad, shell);

		// ── the shell, drawn over the snare so the head sits on top of it ─────
		if (shell >= 1.f) {
			nvgBeginPath(args.vg);
			for (int j = 0; j <= SECT; j++) {
				float th = 2.f * (float)M_PI * (float)j / SECT;
				float X = cx + std::cos(th) * rad, Y = cy + std::sin(th) * rad * TILT + shell;
				if (j == 0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
			}
			nvgStrokeColor(args.vg, nvgRGBAf(0.21f, 0.21f, 0.30f, 0.95f));
			nvgStrokeWidth(args.vg, (1.2f) * lineScale);
			nvgStroke(args.vg);
			for (int j = 0; j <= SECT; j += 3) {
				if (shell < 1.f) break;
				float th = 2.f * (float)M_PI * (float)j / SECT;
				if (std::sin(th) < -0.15f) continue;          // the back wall is hidden
				float X = cx + std::cos(th) * rad, Y0 = cy + std::sin(th) * rad * TILT;
				nvgBeginPath(args.vg);
				nvgMoveTo(args.vg, X, Y0); nvgLineTo(args.vg, X, Y0 + shell);
				nvgStrokeColor(args.vg, nvgRGBAf(0.21f, 0.21f, 0.30f, 0.85f));
				nvgStrokeWidth(args.vg, (0.9f) * lineScale);
				nvgStroke(args.vg);
			}
		}

		// AIR closes the bottom. At zero it is an open shell with a resonant head
		// across it, which is a tom or a kick; wound up it bellies into a sealed
		// bowl, which is a kettledrum -- and a kettledrum is exactly what the
		// harmonic series AIR imposes belongs to. Same knob, same story, twice.
		if (airv > 0.02f) {
			int NB = 5;
			for (int b = 1; b <= NB; b++) {
				float t = (float)b / NB;                    // 0 at the rim, 1 at the pole
				float rr = rad * std::sqrt(std::max(0.f, 1.f - t * t));
				float yy = cy + shell + airv * rad * 0.55f * t;
				nvgBeginPath(args.vg);
				for (int j = 0; j <= SECT; j++) {
					float th = 2.f * (float)M_PI * (float)j / SECT;
					float X = cx + std::cos(th) * rr, Y = yy + std::sin(th) * rr * TILT;
					if (j == 0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
				}
				nvgStrokeColor(args.vg, nvgRGBAf(0.21f, 0.21f, 0.30f, 0.30f + 0.55f * airv));
				nvgStrokeWidth(args.vg, (0.8f) * lineScale);
				nvgStroke(args.vg);
			}
		}
		// ── the surface: rings in arcs so colour is local, then spokes ────────
		for (int i = RINGS; i >= 1; i--) {
			int per = SECT / ARCS;
			for (int a = 0; a < ARCS; a++) {
				int j0 = a * per, j1 = j0 + per;
				float amp = 0.f;
				for (int j = j0; j <= j1; j++) amp = std::max(amp, std::fabs(z[i][j]) * zs / hgt);
				nvgBeginPath(args.vg);
				for (int j = j0; j <= j1; j++) {
					float X, Y; PX(i, j, X, Y);
					if (j == j0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
				}
				float thm = 2.f * (float)M_PI * (float)(j0 + per / 2) / SECT;
				nvgStrokeColor(args.vg, i == RINGS ? sfs::SCREEN_LINE : col(amp, thm));
				nvgStrokeWidth(args.vg, (i == RINGS ? 1.5f : 0.9f + amp * 1.6f) * lineScale);
				nvgStroke(args.vg);
			}
		}
		for (int j = 0; j < SECT; j += 2) {
			float amp = 0.f;
			for (int i = 0; i <= RINGS; i++) amp = std::max(amp, std::fabs(z[i][j]) * zs / hgt);
			nvgBeginPath(args.vg);
			for (int i = 0; i <= RINGS; i++) {
				float X, Y; PX(i, j, X, Y);
				if (i == 0) nvgMoveTo(args.vg, X, Y); else nvgLineTo(args.vg, X, Y);
			}
			float th = 2.f * (float)M_PI * (float)j / SECT;
			NVGcolor c = col(amp * 0.8f, th);
			c.a *= 0.65f;
			nvgStrokeColor(args.vg, c);
			nvgStrokeWidth(args.vg, (0.7f) * lineScale);
			nvgStroke(args.vg);
		}
	}

	void drawLive(const DrawArgs& args) {
		float cy = mainH() * 0.5f;
		float rad = headRad(), cx = headCx();
		if (module && module->headView == 1) {
			drawHead3D(args, head3Cx(), head3Cy(), head3Rad(), 12, 36);
			drawScope(args);
			drawMics(args);
			drawStrikeMark(args, head3Cx(), head3Cy(), head3Rad());
			drawReadout(args, head3Cx());
			return;
		}
		head(args, cx, cy, rad);
		drawScope(args);

		const sfs::MembraneShapes& sh = sfs::membraneShapes();
		const int RINGS = 22;
		for (int i = RINGS; i >= 1; i--) {
			float u = (float)i / RINGS;
			float amp = 0.f;
			for (int k = 0; k < sfs::Drum::NM; k++)
				amp += S->modeVis[k] * sh.at(k, u);
			amp = clamp(std::fabs(amp) * 0.5f, 0.f, 1.f);
			if (amp < 0.004f) continue;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, cy, rad * u * 0.93f);
			nvgStrokeColor(args.vg, nvgRGBAf(0.0f, 0.59f, 0.87f, amp * 0.85f));
			nvgStrokeWidth(args.vg, 1.f + amp * 2.5f);
			nvgStroke(args.vg);
		}

		// where the muffle sits
		float mu = S->dispMuffle;
		if (mu > 0.01f) {
			float ma = module->params[Kit::MUFFLEANG_PARAM].getValue() * (float)M_PI;
			float mr = rad * 0.82f * 0.93f;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx + std::cos(ma) * mr, cy + std::sin(ma) * mr,
			          mm2px(1.1f + mu * 2.2f));
			nvgFillColor(args.vg, nvgRGBAf(0.55f, 0.55f, 0.62f, 0.25f + mu * 0.5f));
			nvgFill(args.vg);
		}

		drawMics(args);
		drawStrikeMark(args, cx, cy, rad);
		drawReadout(args, cx);
	}

	// Both views mark the strike the same way; in 3D it is flattened onto the
	// tilted plane so it still sits where you clicked.
	// The beater, drawn as what it is. A felt mallet is broad and its edge is
	// soft; a stick is small and hard. Those are the same two things EXCITER
	// changes in the sound -- contact area and contact time -- so the icon and
	// the tone move together instead of the icon being decoration.
	void drawStrikeMark(const DrawArgs& args, float cx, float cy, float rad) {
		if (!module) return;
		bool three = module->headView == 1;
		float tilt = three ? TILT : 0.93f;
		float sr = S->dispR * (three ? 1.f : 0.97f), sa = S->dispA;
		float sx = cx + std::cos(sa) * sr * rad * (three ? 1.f : 0.93f);
		float sy = cy + std::sin(sa) * sr * rad * tilt;
		float f = clamp(S->uiFlash, 0.f, 1.f);
		float hard = clamp(S->dispExcite, 0.f, 1.f);
		float r = mm2px(2.6f - 1.5f * hard) + f * mm2px(1.8f);
		// soft beater: a wide halo and no rim. stick: tight, with a hard edge.
		NVGpaint g = nvgRadialGradient(args.vg, sx, sy, r * (0.15f + 0.70f * hard), r,
		                               nvgRGBAf(0.93f, 0.40f, 0.18f, 0.45f + f * 0.55f),
		                               nvgRGBAf(0.93f, 0.40f, 0.18f, 0.f));
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, sx, sy, r);
		nvgFillPaint(args.vg, g);
		nvgFill(args.vg);
		if (hard > 0.35f) {
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, sx, sy, r * 0.55f);
			nvgStrokeColor(args.vg, nvgRGBAf(0.97f, 0.55f, 0.30f,
			                                 (hard - 0.35f) / 0.65f * (0.5f + f * 0.5f)));
			nvgStrokeWidth(args.vg, 0.8f + hard);
			nvgStroke(args.vg);
		}
	}

	// The two mics, and the two paths from the strike to them. The paths are the
	// point: their LENGTH DIFFERENCE is the whole stereo image -- a few tenths
	// of a millisecond and a decibel or two -- so drawing the mics without them
	// would be a picture of the setup rather than of the mechanism. The nearer
	// path is drawn brighter, which is also which side the drum is on.
	//
	// They are DRAGGABLE, and drawn on the head rather than set from knobs
	// because that is where the answer is: how a pair sounds is a fact about
	// where it is relative to the strike, and the panel has no free control to
	// say that with anyway.
	int micDrag = -1, micHover = -1;

	// Where mic c lands on the screen, in the head's own projection.
	Vec micPos(int c) const {
		float cx, cy, rad, sq;
		headFrame(cx, cy, rad, sq);
		float r = md().micR[c], a2 = md().micAng[c];
		return Vec(cx + std::cos(a2) * r * rad, cy + std::sin(a2) * r * rad * sq);
	}
	// Which mic is under the cursor, or -1. The grab radius is a little wider
	// than the mark is drawn, since the mark is small; the cost is a 2.4 mm
	// patch of head that grabs instead of striking, and only when stereo is on.
	int micAt(Vec p) const {
		if (!module || !module->stereo) return -1;
		int best = -1;
		float bestD = mm2px(2.4f) * mm2px(2.4f);
		for (int c = 0; c < 2; c++) {
			Vec m = micPos(c);
			float dx = p.x - m.x, dy = p.y - m.y, d = dx * dx + dy * dy;
			if (d <= bestD) { bestD = d; best = c; }
		}
		return best;
	}
	void placeMic(int c, Vec p) {
		if (!module) return;
		float cx, cy, rad, sq;
		headFrame(cx, cy, rad, sq);
		float x = (p.x - cx) / rad, y = (p.y - cy) / (rad * sq);
		float r = std::sqrt(x * x + y * y);
		// The travel stops short of both ends, because both are degenerate
		// rather than extreme. Every mode is nodal at the rim, so a mic there
		// hears almost nothing; dead centre only the m = 0 monopoles exist, and
		// they have no angular shape, so a mic at the middle is a mono mic
		// wherever the other one is.
		r = clamp(r, 0.10f, 0.92f);
		md().micR[c] = r;
		md().micAng[c] = (r > 1e-4f) ? std::atan2(y, x) : 0.f;
	}

	void drawMics(const DrawArgs& args) {
		if (!module || !module->stereo) return;
		float cx, cy, rad, sq;
		headFrame(cx, cy, rad, sq);
		float sr = S->dispR, sa = S->dispA;
		float sx = cx + std::cos(sa) * sr * rad, sy = cy + std::sin(sa) * sr * rad * sq;
		// Which is nearer, in the same three dimensions the engine uses -- the
		// mics are above the head, so the flat picture cannot answer it.
		float d[2];
		for (int c = 0; c < 2; c++) {
			float dx = md().micR[c] * std::cos(md().micAng[c])
			         - sr * std::cos(sa);
			float dy = md().micR[c] * std::sin(md().micAng[c])
			         - sr * std::sin(sa);
			d[c] = std::sqrt(dx * dx + dy * dy + md().micH * md().micH);
		}
		for (int c = 0; c < 2; c++) {
			Vec m = micPos(c);
			bool near = d[c] <= d[1 - c];
			bool live = (micDrag == c) || (micDrag < 0 && micHover == c);
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, sx, sy);
			nvgLineTo(args.vg, m.x, m.y);
			nvgStrokeColor(args.vg, nvgRGBAf(0.55f, 0.62f, 0.78f, near ? 0.42f : 0.16f));
			nvgStrokeWidth(args.vg, near ? 1.0f : 0.7f);
			nvgStroke(args.vg);

			float rr = mm2px(live ? 1.9f : 1.5f);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, m.x, m.y, rr);
			nvgFillColor(args.vg, nvgRGBAf(0.10f, 0.10f, 0.20f, 0.75f));
			nvgFill(args.vg);
			float al = live ? 1.f : (near ? 0.95f : 0.55f);
			nvgStrokeColor(args.vg, nvgRGBAf(0.62f, 0.70f, 0.86f, al));
			nvgStrokeWidth(args.vg, live ? 1.4f : 1.f);
			nvgStroke(args.vg);
			if (font && font->handle >= 0) {
				sfs::screenFont(args.vg, font, sfs::TYPE_SCREEN_SMALL);
				nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(args.vg, nvgRGBAf(0.75f, 0.81f, 0.94f, al));
				// Nudged up: ALIGN_MIDDLE centres the ascender-to-descender band,
				// not the cap height, so a bare capital sits low in its circle.
				nvgText(args.vg, m.x, m.y - mm2px(0.12f), c == 0 ? "L" : "R", NULL);
			}
		}
	}

	// The wires, on the bottom head. The first version spread seven of them
	// evenly across the whole diameter, which is not what a snare looks like at
	// all -- it read as a set of unrelated horizontal lines drawn through the
	// drum. Real snare wires are about twenty strands bunched into a BAND a
	// couple of inches wide, running parallel from a strainer on one side to a
	// butt plate on the other, so nearly all of them are close to full length
	// and they sit together rather than dividing the head up.
	float wirePh = 0.f;              // the snare's rattle, advanced per frame
	void drawWires(const DrawArgs& args, float cx, float cy, float rad, float shell) {
		// THREE WIRES, STRAIGHT, SOLID, AND FINE.
		//
		// This was a coil first, and the coil was the more accurate drawing --
		// a snare wire really is a helix, and the loops were geometrically
		// honest right down to why they cross. (An orthographic projection of a
		// helix is a sinusoid and can never cross itself; you only get the
		// crossing by leaning each turn along the axis, which is what the eye
		// sees the moment the view is off perpendicular.) It was still wrong
		// for this picture. At the size these are drawn the loops turned into
		// a scalloped braid wrapped round the shell, and accuracy that reads as
		// decoration is worse than a straight line that reads as a wire.
		//
		// SOLID, at one weight, with no translucency to overlap. The coil
		// needed a dim pass for its far side and a bright pass for its near
		// side, and stacking two alphas is what made the band look woven.
		// Straight wires have no far side, so they need no second pass.
		//
		// FINE, and no end plates. Drawn heavy, three wires plus a strainer and
		// a butt plate stopped being hardware slung under a drum and became a
		// gate across the front of it -- the vertical plates read as posts and
		// the wires as rails. The drum is the subject; the snare is a detail on
		// it, and a detail that outweighs its subject is just an error with
		// good intentions.
		float w = module ? clamp(S->dispWires, 0.f, 1.f) : 0.85f;
		if (w < 0.005f) return;
		// They arrive one at a time so the knob's whole travel does something:
		// the centre wire from the moment WIRES leaves zero, the top at a third,
		// the bottom at two thirds. A step rather than a fade, because a fade is
		// the "different opacity" this drawing is meant to be rid of.
		const float ON[3] = {0.f, 1.f / 3.f, 2.f / 3.f};   // centre, top, bottom
		const float O[3]  = {0.f, -1.f, 1.f};              // across the band
		float band = rad * 0.10f;
		// ON THE BOTTOM HEAD, which is the plane at cy + shell -- where a snare
		// actually is. Pushing them lower to get them clear of the shell was a
		// mistake: below that plane there is no drum for them to be attached
		// to, so they ran past the silhouette on both sides and floated.
		float yb = cy + shell;
		// ATTACHED AT BOTH ENDS. Each wire is a CHORD of the bottom head, so its
		// ends land on the rim ellipse -- which is the one place a snare wire is
		// ever fixed. At this band width the chords are within half a percent of
		// the full diameter, so they still read as spanning the drum edge to
		// edge; they just stop ON the drum instead of crossing it.
		// THEY BUZZ WHEN THE HEAD IS STRUCK, which is the whole reason a snare
		// drum has them: the wires are not stretched tight against the head,
		// they lie ON it loosely and rattle. So the vibration is a rattle and
		// not a swing -- two modes, one of them high enough to read as a blur
		// rather than as a wave, pinned to zero at both ends where the wire is
		// held. It follows the same mode amplitudes the head's own colouring
		// uses, scaled the way the spectrum readout already scales them, so the
		// wires stop moving exactly when the drum stops sounding.
		float e = 0.f;
		if (module)
			for (int k = 0; k < sfs::Drum::NM; k++) e = std::max(e, S->modeVis[k]);
		float vib = clamp(e * 2.2f, 0.f, 1.f) * mm2px(0.55f);
		wirePh += 0.9f;                       // per frame; a rattle, not a sway
		for (int i = 0; i < 3; i++) {
			if (w < ON[i]) continue;
			float o = O[i];
			float rr = o * band / std::max(rad, 1.f);
			float half = std::sqrt(std::max(0.f, 1.f - rr * rr)) * rad;
			float yy = yb + o * band * TILT;
			nvgBeginPath(args.vg);
			if (vib < 0.15f) {                // at rest it is a straight line
				nvgMoveTo(args.vg, cx - half, yy);
				nvgLineTo(args.vg, cx + half, yy);
			} else {
				const int N = 64;
				float ph = wirePh + (float)i * 2.1f;
				for (int k = 0; k <= N; k++) {
					float u = (float)k / N;                  // 0..1 along the wire
					float env = std::sin(u * (float)M_PI);   // held at both ends
					float d = std::sin(u * 9.f * (float)M_PI + ph) * 0.6f
					        + std::sin(u * 23.f * (float)M_PI - ph * 1.7f) * 0.4f;
					float x = cx - half + u * 2.f * half;
					if (k == 0) nvgMoveTo(args.vg, x, yy + d * env * vib);
					else        nvgLineTo(args.vg, x, yy + d * env * vib);
				}
			}
			nvgStrokeColor(args.vg, nvgRGBf(0.42f, 0.44f, 0.52f));
			nvgStrokeWidth(args.vg, (0.6f) * lineScale);
			nvgStroke(args.vg);
		}
	}
	void drawReadout(const DrawArgs& args, float cx) {
		if (!module) return;
		if (!font || font->handle < 0) return;
		sfs::screenFont(args.vg, font, sfs::TYPE_SCREEN);
		nvgFillColor(args.vg, sfs::SCREEN_DIM);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
		nvgText(args.vg, mm2px(1.4f), mm2px(1.2f),
		        string::f("%d   %.0f Hz", module->edit + 1, md().f0).c_str(), NULL);
		nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
		float pct = S->dispR * 100.f;
		nvgText(args.vg, box.size.x - mm2px(1.4f), mm2px(1.2f),
		        string::f("%.0f%% out", pct).c_str(), NULL);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
		// The bottom line is the only place the mics are discoverable: they are
		// two small marks on a surface whose whole job is to be clicked, so
		// nothing about them says "and these move" until it is written down.
		// While one is held it reports itself instead -- an angle you are
		// dragging is worth more than an instruction you have already followed.
		int live = (micDrag >= 0) ? micDrag : (module->stereo ? micHover : -1);
		if (live >= 0) {
			float a2 = md().micAng[live] * 180.f / (float)M_PI;
			while (a2 < 0.f) a2 += 360.f;
			nvgFillColor(args.vg, sfs::SCREEN_DIM);
			nvgText(args.vg, cx, mainH() - mm2px(1.f),
			        string::f("%s MIC   %.0f%% OUT   %.0f DEG", live == 0 ? "LEFT" : "RIGHT",
			                  md().micR[live] * 100.f, a2).c_str(), NULL);
		} else {
			nvgFillColor(args.vg, sfs::SCREEN_PMID);
			nvgText(args.vg, cx, mainH() - mm2px(1.f),
			        module->stereo ? "CLICK THE HEAD TO PLAY   DRAG L AND R TO MOVE THE MICS"
			                       : "CLICK THE HEAD TO PLAY", NULL);
		}
	}

	// The browser thumbnail. Without this the module is a dark slab in the
	// library and says nothing about what it is.
	// One bar per mode, tallest at the fundamental: the 1/omega excitation tilt
	// and the frequency-dependent damping are both visible here and nowhere else.
	void drawSpectrum(const DrawArgs& args, float x0, float y0, float x1, float y1) {
		if (x1 - x0 < mm2px(8.f)) return;
		float h = y1 - y0;
		float w = (x1 - x0) / (float)sfs::Drum::NM;
		for (int k = 0; k < sfs::Drum::NM; k++) {
			float a = module ? clamp(S->modeVis[k] * 2.2f, 0.f, 1.f)
			                 : 0.9f * std::exp(-k * 0.12f);
			float bh = 1.f + a * h;
			nvgBeginPath(args.vg);
			nvgRect(args.vg, x0 + k * w, y1 - bh, std::max(w - 0.8f, 0.8f), bh);
			nvgFillColor(args.vg, a > 0.02f
			             ? nvgRGBAf(0.0f, 0.59f, 0.87f, 0.35f + a * 0.65f)
			             : sfs::SCREEN_PURP);
			nvgFill(args.vg);
		}
	}

	// The browser thumbnail shows the DEFAULT view, which is 3D. drawHead3D
	// already copes with module == NULL by standing in a plausible mode mix, so
	// the preview is the same code rather than a second drawing to keep in step.
	void drawPreview(const DrawArgs& args) {
		drawHead3D(args, head3Cx(), head3Cy(), head3Rad(), 12, 36);
		drawScope(args);
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, head3Cx() + head3Rad() * 0.42f,
		          head3Cy() - head3Rad() * 0.30f * TILT, mm2px(1.6f));
		nvgFillColor(args.vg, nvgRGBAf(0.93f, 0.40f, 0.18f, 0.9f));
		nvgFill(args.vg);
	}

	// ── THE TABS: eight small drums, each the instrument itself ─────────────
	// A row rather than a grid, so the eight level bars share one baseline and
	// read as a comparison; the selected tab is underlined in the panel's
	// orange. Each drum is drawn by the same code as the big one, from that
	// instrument's own mode amplitudes, so striking instrument 5 makes tab 5
	// ring while the main view keeps showing the one you are editing.
	float tabW() const { return box.size.x / (float)KIT_N; }
	void drawTabs(const DrawArgs& args) {
		float y0 = mainH(), h = stripH(), w = tabW();
		// a rule between the main view and the strip
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, y0, box.size.x, 1.f);
		nvgFillColor(args.vg, sfs::SCREEN_PURP);
		nvgFill(args.vg);
		const float footH = mm2px(2.4f);                // number + level bar
		const float drumH = h - footH - mm2px(0.8f);
		// The object is 0.34 + 2*TILT + SHELL radii tall; what fits is the
		// smaller of that and the cell's width.
		float radMax = std::min(drumH / (0.34f + 2.f * TILT + SHELL), w * 0.5f - mm2px(0.8f));
		const Kit::Inst* saved = S;
		for (int c = 0; c < KIT_N; c++) {
			S = module ? &module->inst[c] : nullptr;
			float x0 = c * w, cx = x0 + w * 0.5f;
			bool sel = module && module->edit == c;
			float rad = radMax * sizeScale();
			float hgt = rad * 0.34f, shell = shellDepth(rad);
			// centre the OBJECT (rise + disc + shell) in the drum area
			float cy = y0 + mm2px(0.8f) + drumH * 0.5f - (shell - hgt) * 0.5f;
			lineScale = 0.45f;
			drawHead3D(args, cx, cy, rad, 8, 24);
			lineScale = 1.f;
			// the number, top-left of the cell
			if (font && font->handle >= 0) {
				sfs::screenFont(args.vg, font, sfs::TYPE_SCREEN_SMALL);
				nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
				nvgFillColor(args.vg, sel ? sfs::SCREEN_HOT : sfs::SCREEN_DIM);
				nvgText(args.vg, x0 + mm2px(1.f), y0 + mm2px(0.9f),
				        string::f("%d", c + 1).c_str(), NULL);
			}
			// the level bar along the foot: a dim track, and the level over it.
			// sqrt so a quiet drum still shows; a linear bar is empty at -20 dB.
			float bx0 = x0 + mm2px(1.f), bx1 = x0 + w - mm2px(1.f);
			float by = y0 + h - mm2px(1.4f);
			nvgBeginPath(args.vg);
			nvgRect(args.vg, bx0, by, bx1 - bx0, mm2px(0.7f));
			nvgFillColor(args.vg, sfs::SCREEN_PURP);
			nvgFill(args.vg);
			float lv = module ? std::sqrt(clamp(module->inst[c].level, 0.f, 1.f)) : 0.f;
			if (lv > 0.005f) {
				nvgBeginPath(args.vg);
				nvgRect(args.vg, bx0, by, (bx1 - bx0) * lv, mm2px(0.7f));
				nvgFillColor(args.vg, lv > 0.92f ? sfs::SCREEN_HOT : sfs::SCREEN_BLUE);
				nvgFill(args.vg);
			}
			// the selection: an underline in the plugin's orange
			if (sel) {
				nvgBeginPath(args.vg);
				nvgRect(args.vg, x0 + mm2px(0.6f), y0 + h - mm2px(0.5f), w - mm2px(1.2f), mm2px(0.5f));
				nvgFillColor(args.vg, sfs::SCREEN_HOT);
				nvgFill(args.vg);
			}
			// cell divider
			if (c > 0) {
				nvgBeginPath(args.vg);
				nvgRect(args.vg, x0, y0 + mm2px(1.f), 0.6f, h - mm2px(2.f));
				nvgFillColor(args.vg, nvgTransRGBA(sfs::SCREEN_PURP, 140));
				nvgFill(args.vg);
			}
		}
		S = saved;
	}

	// THE 3D VIEW HAS ITS OWN GEOMETRY, and every part of this widget that maps
	// between the head and the screen has to use the same one -- the mode rings,
	// the mic marks, a click that strikes and a drag that moves a mic. It lived
	// inside hit() and was already wrong there once: that inverted the FLAT
	// projection while the 3D view was showing, so a click right of centre
	// mapped past the rim, clamped, and landed on the edge. The projection is
	// X = cx + x*r and Y = cy + y*r*TILT, so undoing it means dividing the
	// vertical by the tilt as well.
	void headFrame(float& cx, float& cy, float& rad, float& sq) const {
		if (module && module->headView == 1) {
			cx = head3Cx(); cy = head3Cy(); rad = head3Rad(); sq = TILT;
		} else {
			cx = headCx(); cy = mainH() * 0.5f; rad = headRad() * 0.93f; sq = 1.f;
		}
	}

	void hit(Vec p, float vel) {
		if (!module) return;
		float cx, cy, rad, sq;
		headFrame(cx, cy, rad, sq);
		float x = (p.x - cx) / rad, y = (p.y - cy) / (rad * sq);
		float r = std::sqrt(x * x + y * y);
		if (r > 1.f) { x /= r; y /= r; }
		module->pendX = clamp(x, -1.f, 1.f);
		module->pendY = clamp(y, -1.f, 1.f);
		module->pendVel = vel;
		module->pendHit = true;
	}

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			// A press in the tab strip selects that instrument -- on the audio
			// thread, via editReq, so the knobs and the slot switch together.
			if (module && e.pos.y >= mainH()) {
				module->editReq = clamp((int)(e.pos.x / tabW()), 0, KIT_N - 1);
				e.consume(this);
				return;
			}
			// A mic under the cursor is grabbed rather than struck. Tested
			// FIRST, and it has to be: the head is a play surface everywhere,
			// so anything drawn on it that you can also move must claim the
			// press before the strike does.
			int m = micAt(e.pos);
			if (m >= 0) { micDrag = m; e.consume(this); return; }
			// Velocity from where you land: the rim is where you play rimshots,
			// and it saves a modifier key.
			hit(e.pos, 0.85f);
			dragFrom = e.pos;
			e.consume(this);
			return;
		}
		// A RIGHT-CLICK ON A TAB is that tab's menu, whichever tab is selected:
		// load a preset into it, or put its mics back. The module's own menu
		// only ever speaks for the selected instrument.
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT
		    && module && e.pos.y >= mainH()) {
			int tab = clamp((int)(e.pos.x / tabW()), 0, KIT_N - 1);
			Kit* m = module;
			Menu* menu = createMenu();
			menu->addChild(createMenuLabel(string::f("Instrument %d", tab + 1)));
			for (int i = 0; i < KIT_NPRESET; i++) {
				int idx = i;
				menu->addChild(createMenuItem(KIT_PRESETS[i].name, "",
					[=]() { m->presetToSlot(tab, KIT_PRESETS[idx]); }));
			}
			menu->addChild(new MenuSeparator);
			menu->addChild(createMenuItem("Reset mic positions", "",
			                              [=]() { m->resetMics(tab); }));
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
	// Dragging across the head keeps striking, which is how you get a roll out
	// of a mouse -- unless the drag started on a mic, in which case it moves it.
	void onDragHover(const DragHoverEvent& e) override {
		if (e.origin == this) {
			if (micDrag >= 0) {
				placeMic(micDrag, e.pos);
			} else {
				Vec d = e.pos.minus(dragFrom);
				if (e.pos.y < mainH() && std::sqrt(d.x * d.x + d.y * d.y) > mm2px(2.2f)) {
					hit(e.pos, 0.45f);
					dragFrom = e.pos;
				}
			}
		}
		OpaqueWidget::onDragHover(e);
	}
	// Not onButton's release: a drag that ends outside the widget never sends
	// one here, and the mic would stay stuck to the cursor.
	void onDragEnd(const DragEndEvent& e) override {
		micDrag = -1;
		OpaqueWidget::onDragEnd(e);
	}
	void onHover(const HoverEvent& e) override {
		micHover = micAt(e.pos);
		OpaqueWidget::onHover(e);
	}
	void onLeave(const LeaveEvent& e) override {
		micHover = -1;
		OpaqueWidget::onLeave(e);
	}
};

struct KitWidget : ModuleWidget {
	void appendContextMenu(Menu* menu) override {
		Kit* m = dynamic_cast<Kit*>(this->module);
		assert(m);
		menu->addChild(new MenuSeparator);
		// These are the instruments the engine was measured against while it was
		// being built, so they are also the shortest route to hearing whether a
		// change broke something.
		menu->addChild(createIndexPtrSubmenuItem("Head view",
			{"Flat", "3D"}, &m->headView));
		menu->addChild(createBoolPtrMenuItem("Stereo pairs (L/R per instrument on the poly out)", "",
		                                     &m->stereo));
		menu->addChild(createMenuItem("Reset mic positions", "",
		                              [=]() { m->resetMics(m->edit); },
		                              !m->stereo));
		menu->addChild(createSubmenuItem(string::f("Load into instrument %d", m->edit + 1), "",
			[=](Menu* sub) {
				for (int i = 0; i < KIT_NPRESET; i++) {
					int idx = i;
					sub->addChild(createMenuItem(KIT_PRESETS[i].name, "",
					                             [=]() { m->loadPreset(idx); }));
				}
			}));
		menu->addChild(createMenuItem("Load the default kit into all eight", "",
		                              [=]() { m->loadDefaultKit(); }));
	}

	KitWidget(Kit* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/kit.svg")));
		using sfs::hp;

		// NO PanelLabels, and no title. This panel's artwork carries its own text
		// as outlined paths -- which Rack DOES render, unlike <text> -- so
		// drawing them again in Figtree printed every label twice, half a
		// millimetre out. Slice learned this first; see its widget. The grid
		// below is read from the art, which is now the source of the layout.
		KitDisplay* disp = new KitDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(5.08f, 10.16f));
		disp->box.size = mm2px(Vec(101.58f, 45.71f));
		addChild(disp);

		// ── the 2026-08 grid: eight columns, four rows ─────────────────────
		// Transcribed from the panel art, which is authored at 11.813 units/mm
		// (1320 x 1518 for 22HP). Every control is a trimpot: sixteen of them
		// on one pitch reads as one instrument, where four sizes of knob read
		// as four separate ideas competing for the same panel.
		static const float KX[8] = {10.96f, 23.74f, 36.53f, 49.31f,
		                            62.09f, 74.87f, 87.66f, 100.44f};
		static const float Y_VOICE = 70.26f;   // what the drum IS
		static const float Y_CHAR  = 85.50f;   // how it is played and dressed
		static const float Y_CV    = 102.43f;  // one CV in per voice control
		static const float Y_PERF  = 119.36f;  // the transport row

		struct K { int p; const char* t; };
		// Row 1 and row 3 are the SAME EIGHT in the same order, so a cable
		// hangs directly under the control it modulates and the pairing needs
		// no label to explain it.
		const K voice[8] = {
			{Kit::SIZE_PARAM, "SIZE"},   {Kit::TENSION_PARAM, "TENSION"},
			{Kit::STIFF_PARAM, "MATERIAL"}, {Kit::AIR_PARAM, "AIR"},
			{Kit::DECAY_PARAM, "DECAY"}, {Kit::TONE_PARAM, "TONE"},
			{Kit::EXCITE_PARAM, "EXCITER"}, {Kit::MUFFLE_PARAM, "MUFFLE"}};
		for (int i = 0; i < 8; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(KX[i], Y_VOICE)), module, voice[i].p));
		}

		// HIT stays a BUTTON. It is a momentary strike, not a value, and the
		// only reason it sits in a row of trimpots is that VCVButton is 6.10mm
		// against Trimpot's 6.05 -- near enough to hold the grid.
		const K chr[7] = {
			{Kit::COUPLE_PARAM, "COUPLE"}, {Kit::RESO_PARAM, "RESO"},
			{Kit::BEND_PARAM, "BEND"},     {Kit::WEIGHT_PARAM, "WEIGHT"},
			{Kit::SNARE_PARAM, "WIRES"},   {Kit::SNARETHR_PARAM, "TIGHT"},
			{Kit::LEVEL_PARAM, "LEVEL"}};
		for (int i = 0; i < 7; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(KX[i], Y_CHAR)), module, chr[i].p));
		}
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(KX[7], Y_CHAR)), module, Kit::STRIKE_PARAM, Kit::STRIKE_LIGHT));

		struct J { int id; const char* t; };
		const J cv[8] = {
			{Kit::SIZE_INPUT, "SIZE"},   {Kit::TENSION_INPUT, "TENSION"},
			{Kit::STIFF_INPUT, "MAT"},   {Kit::AIR_INPUT, "AIR"},
			{Kit::DECAY_INPUT, "DECAY"}, {Kit::TONE_INPUT, "TONE"},
			{Kit::EXCITE_INPUT, "EXCITER"}, {Kit::MUFFLE_INPUT, "MUFFLE"}};
		for (int i = 0; i < 8; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(KX[i], Y_CV)), module, cv[i].id));
		}

		// ── THE TRANSPORT ROW (house style) ────────────────────────────────
		// The bottom row is performance data in and out, and nothing else:
		// GATE, V/OCT, VEL first and in that order, then whatever else the
		// instrument is played with, then its outputs on a plate at the right.
		// Per-parameter CV belongs above, beside its control. See
		// docs/conventions/panel-design.md.
		const J perf[5] = {{Kit::GATE_INPUT, "GATE"}, {Kit::VOCT_INPUT, "V/OCT"},
		                   {Kit::VEL_INPUT, "VEL"},   {Kit::STRIKEX_INPUT, "X"},
		                   {Kit::STRIKEY_INPUT, "Y"}};
		for (int i = 0; i < 5; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(KX[i], Y_PERF)), module, perf[i].id));
		}
		// POLY, MIX L, MIX R. The ids are the old HEAD/WIRES/MIX slots (outputs
		// serialise by index); the art's labels need redrawing to match.
		const J outs[3] = {{Kit::OUT_OUTPUT, "POLY"}, {Kit::HEAD_OUTPUT, "MIX L"},
		                   {Kit::SNARE_OUTPUT, "MIX R"}};
		for (int i = 0; i < 3; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(KX[5 + i], Y_PERF)),
			                                           module, outs[i].id));
		}
	}
};

Model* modelKit = createModel<Kit, KitWidget>("Kit");

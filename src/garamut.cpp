// =============================================================================
// Garamut: a slit drum.
//
// A hollow column with a long slit cut into one side. The wall on either side
// of the slit is a lip, and each lip has its own note. GATE L strikes the left
// lip and GATE R the right, at the height Y gives, and ROLL keeps striking for
// as long as it is high. Every channel of every polyphonic input plays the
// same drum, so a strike can land on a lip that is already ringing.
//
// The sound is src/garamut-engine.hpp, which runs without Rack and is measured
// and rendered by tools/garamut-proto.cpp. Design: docs/garamut-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "waveguide.hpp"
#include "garamut-engine.hpp"
#include <cmath>
#include <string>

static const int GM_MOUSE = garamut::NCH - 1;   // the engine's channel for a click on the screen
static const int GM_MARKS = 24;

static const char* GM_INTERVAL_NAMES[13] = {
	"unison", "a minor second", "a major second", "a minor third", "a major third", "a fourth",
	"a tritone", "a fifth", "a minor sixth", "a major sixth", "a minor seventh", "a major seventh", "an octave"};

static std::string gmBlendName(const char* const* names, int n, float v) {
	float u = clamp(v, 0.f, 1.f) * (n - 1);
	int i = (int)std::round(u);
	if (std::fabs(u - i) < 0.02f) return names[i];
	int a = std::min((int)u, n - 2);
	return string::f("%s, %.0f%% of the way to %s", names[a], 100.f * (u - a), names[a + 1]);
}

struct Garamut : Module {
	enum ParamId {
		SIZE_PARAM, SHAPE_PARAM, MATERIAL_PARAM, SLIT_PARAM, MALLET_PARAM,
		Y_PARAM, INTERVAL_PARAM, DECAY_PARAM, RATE_PARAM, TUNE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		SIZE_INPUT, SHAPE_INPUT, MATERIAL_INPUT, SLIT_INPUT, MALLET_INPUT,
		Y_INPUT, INTERVAL_INPUT, DECAY_INPUT, RATE_INPUT, VOCT_INPUT,
		GATE_L_INPUT, GATE_R_INPUT, ROLL_INPUT, VEL_INPUT,
		INPUTS_LEN
	};
	enum OutputId { LEFT_OUTPUT, RIGHT_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	garamut::Drum drum;
	garamut::Params P;
	garamut::Roller roller[16];
	dsp::SchmittTrigger trigL[16], trigR[16];
	int lastSide[16] = {};
	int rollCount[16] = {};
	int ctl = 0;

	// a strike for the screen to draw: written by the audio thread
	struct Mark { float y = 0.5f, vel = 0.f, t = -10.f; int side = 0, ch = 0; };
	Mark marks[GM_MARKS];
	int markNext = 0;
	float now = 0.f;
	int strikes = 0;            // every strike so far, for the harness

	// One mallet per channel, for the screen: the channels arriving on GATE L,
	// GATE R and ROLL, each resting on the side it last struck at its own Y.
	int chCount = 0;
	float chY[16] = {}, chHit[16] = {}, chVel[16] = {};
	int chSide[16] = {};
	bool chStruck[16] = {};

	// a click on the screen, handed to the audio thread
	std::atomic<int> uiSide{-1};
	float uiY = 0.5f, uiVel = 0.7f;

	// menu
	bool rollAlternates = false;

	struct NamedQ : ParamQuantity {
		const char* const* names = nullptr;
		int n = 0;
		std::string getDisplayValueString() override { return gmBlendName(names, n, getValue()); }
	};
	struct MaterialQ : NamedQ {
		MaterialQ() { static const char* N[4] = {"Cedar", "Hardwood", "Aluminium", "Steel"}; names = N; n = 4; }
	};
	struct MalletQ : NamedQ {
		MalletQ() { static const char* N[4] = {"Felt", "Rubber", "Wood", "Brass"}; names = N; n = 4; }
	};
	struct SizeQ : ParamQuantity {
		std::string getDisplayValueString() override {
			float h = garamut::heightOf(getValue());
			return h >= 1.f ? string::f("%.2f m tall", h) : string::f("%.0f cm tall", 100.f * h);
		}
	};
	struct ShapeQ : ParamQuantity {
		std::string getDisplayValueString() override {
			float v = getValue();
			if (v < 0.01f) return "Square column";
			if (std::fabs(v - 0.5f) < 0.01f) return "Round column";
			if (v > 0.99f) return "Pear";
			if (v < 0.5f) return string::f("Square to round column, %.0f%% round", 200.f * v);
			return string::f("Round column to pear, %.0f%% pear", 200.f * (v - 0.5f));
		}
	};
	struct SlitQ : ParamQuantity {
		std::string getDisplayValueString() override {
			float v = getValue();
			float size = module ? module->params[SIZE_PARAM].getValue() : 0.5f;
			float H = garamut::heightOf(size), R = 0.17f * H;
			float lf = 0.30f + 0.45f * v;
			float w = R * (0.05f + 0.25f * v);
			return string::f("%.0f%% of the height, %.1f cm wide", 100.f * lf, 100.f * w);
		}
	};
	struct YQ : ParamQuantity {
		std::string getDisplayValueString() override { return string::f("%.0f%% up the slit", 100.f * getValue()); }
	};
	struct IntervalQ : ParamQuantity {
		std::string getDisplayValueString() override {
			int s = (int)std::round(getValue());
			return string::f("%d semitone%s (%s)", s, s == 1 ? "" : "s", GM_INTERVAL_NAMES[clamp(s, 0, 12)]);
		}
	};

	Garamut() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<SizeQ>(SIZE_PARAM, 0.f, 1.f, 0.5f, "Size");
		configParam<ShapeQ>(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Shape (square column, round column, pear)");
		configParam<MaterialQ>(MATERIAL_PARAM, 0.f, 1.f, 1.f / 3.f, "Material");
		configParam<SlitQ>(SLIT_PARAM, 0.f, 1.f, 0.5f, "Slit (length and width)");
		configParam<MalletQ>(MALLET_PARAM, 0.f, 1.f, 0.5f, "Mallet");
		configParam<YQ>(Y_PARAM, 0.f, 1.f, 0.5f, "Strike height");
		configParam<IntervalQ>(INTERVAL_PARAM, 0.f, 12.f, 5.f, "Interval (the right lip's note above the left's)");
		paramQuantities[INTERVAL_PARAM]->snapEnabled = true;
		configParam(DECAY_PARAM, -1.f, 1.f, 0.f, "Decay (ring time)", "x", 4.f, 1.f);
		configParam(RATE_PARAM, 0.f, 1.f, std::log(14.f / 4.f) / std::log(7.5f), "Roll rate", " strokes/s", 7.5f, 4.f);
		configParam(TUNE_PARAM, -12.f, 12.f, 0.f, "Tune (the whole drum)", " semitones");
		configInput(SIZE_INPUT, "Size (±5 V = the whole range)");
		configInput(SHAPE_INPUT, "Shape (±5 V = the whole range)");
		configInput(MATERIAL_INPUT, "Material (±5 V = the whole range)");
		configInput(SLIT_INPUT, "Slit (±5 V = the whole range)");
		configInput(MALLET_INPUT, "Mallet (±5 V = the whole range)");
		configInput(Y_INPUT, "Strike height (polyphonic, ±5 V = bottom to top of the slit)");
		configInput(INTERVAL_INPUT, "Interval (1V/oct)");
		configInput(DECAY_INPUT, "Decay (±5 V = the whole range)");
		configInput(RATE_INPUT, "Roll rate (1 V per doubling)");
		configInput(VOCT_INPUT, "V/OCT (transposes the whole drum)");
		configInput(GATE_L_INPUT, "Strike the left lip (polyphonic)");
		configInput(GATE_R_INPUT, "Strike the right lip (polyphonic)");
		configInput(ROLL_INPUT, "Roll (polyphonic: strikes for as long as it is high)");
		configInput(VEL_INPUT, "Velocity (polyphonic, 0-10 V; 70% when unpatched)");
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
		for (int c = 0; c < 16; c++) roller[c].rng = 0x2545F491u + 0x9E3779B9u * (uint32_t)c;
		drum.setSampleRate(48000.f);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		drum.setSampleRate(e.sampleRate);
		drum.update(P);
	}
	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		rollAlternates = false;
		drum.reset();
		for (int c = 0; c < 16; c++) { roller[c].reset(); lastSide[c] = 0; }
	}

	float cv(int in, float range) {
		return inputs[in].isConnected() ? inputs[in].getVoltage() * range : 0.f;
	}
	float strikeY(int c) {
		float y = params[Y_PARAM].getValue();
		if (inputs[Y_INPUT].isConnected()) y += 0.1f * inputs[Y_INPUT].getPolyVoltage(c);
		return clamp(y, 0.f, 1.f);
	}
	float strikeVel(int c) {
		if (!inputs[VEL_INPUT].isConnected()) return 0.7f;
		return clamp(inputs[VEL_INPUT].getPolyVoltage(c) / 10.f, 0.f, 1.f);
	}
	void strike(int c, int side, float y, float vel) {
		drum.strike(c, side, y, vel);
		Mark& m = marks[markNext];
		m.y = y; m.vel = vel; m.side = side; m.t = now; m.ch = c;
		if (c < 16) { chHit[c] = now; chVel[c] = vel; chStruck[c] = true; chSide[c] = side; }
		markNext = (markNext + 1) % GM_MARKS;
		strikes++;
	}

	void controls() {
		P.size     = clamp(params[SIZE_PARAM].getValue() + cv(SIZE_INPUT, 0.1f), 0.f, 1.f);
		P.shape    = clamp(params[SHAPE_PARAM].getValue() + cv(SHAPE_INPUT, 0.1f), 0.f, 1.f);
		P.material = clamp(params[MATERIAL_PARAM].getValue() + cv(MATERIAL_INPUT, 0.1f), 0.f, 1.f);
		P.slit     = clamp(params[SLIT_PARAM].getValue() + cv(SLIT_INPUT, 0.1f), 0.f, 1.f);
		P.mallet   = clamp(params[MALLET_PARAM].getValue() + cv(MALLET_INPUT, 0.1f), 0.f, 1.f);
		P.interval = clamp(params[INTERVAL_PARAM].getValue() + cv(INTERVAL_INPUT, 12.f), 0.f, 24.f);
		P.decay    = std::pow(4.f, clamp(params[DECAY_PARAM].getValue() + cv(DECAY_INPUT, 0.2f), -1.f, 1.f));
		P.voct     = params[TUNE_PARAM].getValue() / 12.f + cv(VOCT_INPUT, 1.f);
		drum.update(P);

		int nl = inputs[GATE_L_INPUT].isConnected() ? inputs[GATE_L_INPUT].getChannels() : 0;
		int nr = inputs[GATE_R_INPUT].isConnected() ? inputs[GATE_R_INPUT].getChannels() : 0;
		int no = inputs[ROLL_INPUT].isConnected() ? inputs[ROLL_INPUT].getChannels() : 0;
		chCount = std::max(nl, std::max(nr, no));
		for (int c = 0; c < chCount; c++) {
			chY[c] = strikeY(c);
			// before a channel has struck, its mallet waits on the side of the input it arrives on
			if (!chStruck[c]) chSide[c] = (c < nl || c >= nr) ? 0 : 1;
		}
	}

	void process(const ProcessArgs& args) override {
		if (ctl-- <= 0) { ctl = 15; controls(); }
		now += args.sampleTime;

		float rate = 4.f * std::pow(7.5f, params[RATE_PARAM].getValue()) * std::exp2(cv(RATE_INPUT, 1.f));
		rate = clamp(rate, 1.f, 40.f);

		int nl = inputs[GATE_L_INPUT].getChannels(), nr = inputs[GATE_R_INPUT].getChannels();
		int nroll = inputs[ROLL_INPUT].getChannels();
		// Every trigger runs every sample (0 V where a cable has no channel), because a
		// Rack trigger whose first reading is already high does not fire.
		for (int c = 0; c < 16; c++) {
			if (trigL[c].process(c < nl ? inputs[GATE_L_INPUT].getVoltage(c) : 0.f, 0.1f, 1.f)) {
				strike(c, 0, strikeY(c), strikeVel(c)); lastSide[c] = 0;
			}
			if (trigR[c].process(c < nr ? inputs[GATE_R_INPUT].getVoltage(c) : 0.f, 0.1f, 1.f)) {
				strike(c, 1, strikeY(c), strikeVel(c)); lastSide[c] = 1;
			}
			bool high = c < nroll && inputs[ROLL_INPUT].getVoltage(c) >= 1.f;
			float vm, dy;
			if (roller[c].step(high, rate, args.sampleTime, vm, dy)) {
				int side = rollAlternates ? ((lastSide[c] + rollCount[c]) & 1) : lastSide[c];
				rollCount[c]++;
				strike(c, side, clamp(strikeY(c) + dy, 0.f, 1.f), clamp(strikeVel(c) * vm, 0.f, 1.f));
			}
			if (!high) rollCount[c] = 0;
		}
		int s = uiSide.exchange(-1);
		if (s >= 0) strike(GM_MOUSE, s, uiY, uiVel);

		float l, r;
		drum.process(l, r);
		outputs[LEFT_OUTPUT].setVoltage(sfs::softClip(l));
		outputs[RIGHT_OUTPUT].setVoltage(sfs::softClip(r));
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "rollAlternates", json_boolean(rollAlternates));
		return r;
	}
	void dataFromJson(json_t* r) override {
		json_t* j = json_object_get(r, "rollAlternates");
		if (j) rollAlternates = json_boolean_value(j);
	}
};

// ── the browser preview: a real drum, struck on both lips ──
static Garamut* garamutPreview() {
	static Garamut* m = nullptr;
	if (m) return m;
	m = new Garamut();
	// three players on the left lip and one on the right, each at its own height
	sfs::previewConnect(m->inputs[Garamut::GATE_L_INPUT], 3);
	sfs::previewConnect(m->inputs[Garamut::GATE_R_INPUT], 4);
	sfs::previewConnect(m->inputs[Garamut::Y_INPUT], 4);
	const float ys[4] = {-0.8f, 1.8f, -3.2f, 3.0f};
	for (int c = 0; c < 4; c++) m->inputs[Garamut::Y_INPUT].setVoltage(ys[c], c);
	int64_t frame = 0;
	sfs::previewRun(*m, 0.02f, frame);
	m->inputs[Garamut::GATE_L_INPUT].setVoltage(10.f, 0);
	m->inputs[Garamut::GATE_R_INPUT].setVoltage(10.f, 3);
	sfs::previewRun(*m, 0.06f, frame);
	return m;
}

// =============================================================================
// The drawing: the column on the faceplate, with no screen behind it, in the
// panel's own inks and orange for a lip that is ringing. The slit faces you.
// Every channel arriving on GATE L, GATE R or ROLL has its own mallet, resting
// beside the lip it last struck at its own height; mallets on one side fan out
// from the first. A click on a lip strikes it there.
// =============================================================================
struct GaramutView {
	float cx = 0.f, base = 0.f, s = 1.f;  // centre x, the column's base y, px per height
	float yaw = 0.32f, tilt = 0.30f;
	float n = 2.f;              // superellipse exponent: 2 round, larger square
	float pear = 0.f, rad = 0.17f;
	// a point on the column's surface at height eta and angle th (front = pi/2)
	Vec at(float eta, float th, float out = 1.f, float* depth = nullptr) const {
		float c = std::cos(th), si = std::sin(th);
		float se = 1.f / std::pow(std::pow(std::fabs(c), n) + std::pow(std::fabs(si), n), 1.f / n);
		float r = rad * garamut::profileR(eta, pear) * se * out;
		float x = r * c, z = r * si;
		float xr = x * std::cos(yaw) + z * std::sin(yaw);
		float zr = -x * std::sin(yaw) + z * std::cos(yaw);
		if (depth) *depth = zr;
		return Vec(cx + s * xr, base - s * (eta * std::cos(tilt)) + s * zr * std::sin(tilt));
	}
	// whether the surface at angle th faces the viewer
	bool faces(float th) const {
		float c = std::cos(th), si = std::sin(th);
		float nx = (c < 0 ? -1.f : 1.f) * std::pow(std::fabs(c), n - 1.f);
		float nz = (si < 0 ? -1.f : 1.f) * std::pow(std::fabs(si), n - 1.f);
		return -nx * std::sin(yaw) + nz * std::cos(yaw) > 0.f;
	}
};

// Inks for the faceplate, light and dark. The light values sit on #f0f0f0, the
// dark ones on #3a3a3e; the column is a shade off the faceplate either way so it
// reads as an object.
struct GmPal { NVGcolor ink, soft, line, body, top, hollow, mallet; };
static inline GmPal gmPal() {
	const sfs::Theme& T = sfs::theme();
	if (sfs::dark())
		return {T.ink, T.inkSoft, nvgRGB(0x5C, 0x5C, 0x64), nvgRGB(0x4A, 0x4A, 0x50), nvgRGB(0x56, 0x56, 0x5C),
		        nvgRGB(0x1E, 0x1E, 0x22), nvgRGB(0xC8, 0xC8, 0xCE)};
	return {T.ink, T.inkSoft, nvgRGB(0xC4, 0xC4, 0xCC), nvgRGB(0xE0, 0xE0, 0xE5), nvgRGB(0xD2, 0xD2, 0xD8),
	        nvgRGB(0x4A, 0x4A, 0x52), nvgRGB(0x5A, 0x5A, 0x62)};
}
static const NVGcolor GM_HOT = nvgRGB(0xEC, 0x65, 0x2E);

struct GaramutDisplay : OpaqueWidget {
	Garamut* module = nullptr;
	int hoverSide = -1;
	float hoverY = 0.5f;

	GaramutView view() const {
		GaramutView v;
		Garamut* m = module;
		v.n = 2.f + 8.f * m->drum.sq * m->drum.sq;
		v.pear = m->drum.pear;
		v.rad = 0.17f * 1.5f;     // drawn wider than it is, so the lips read
		float top = mm2px(3.6f), bot = box.size.y - mm2px(5.4f);
		v.s = (bot - top) / (std::cos(v.tilt) + 2.f * v.rad * std::sin(v.tilt));
		v.base = bot - v.s * v.rad * std::sin(v.tilt);
		v.cx = box.size.x * 0.5f;
		return v;
	}
	// half the slit's width, and each lip's width, as angles round the column
	void slitAngles(float& half, float& lip) const {
		const garamut::Drum& D = module->drum;
		half = std::max(0.02f, 0.5f * D.slitW / D.R);
		lip = 0.55f;
	}
	// the angle of the strike line on a lip
	static float strikeAngle(int side, float half, float lip) {
		return (float)M_PI / 2.f + (side == 0 ? 1.f : -1.f) * (half + 0.45f * lip);
	}

	bool hitLip(Vec p, int& side, float& y) const {
		if (!module) return false;
		GaramutView v = view();
		const garamut::Drum& D = module->drum;
		float half, lip; slitAngles(half, lip);
		float best = mm2px(2.2f);
		bool found = false;
		for (int sd = 0; sd < 2; sd++) {
			float th = (float)M_PI / 2.f + (sd == 0 ? 1.f : -1.f) * (half + 0.5f * lip);
			for (int i = 0; i <= 24; i++) {
				float yy = i / 24.f;
				float eta = D.slit0 + (D.slit1 - D.slit0) * yy;
				Vec q = v.at(eta, th);
				float d = q.minus(p).norm();
				if (d < best) { best = d; side = sd; y = yy; found = true; }
			}
		}
		return found;
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) { OpaqueWidget::onButton(e); return; }
		int sd; float y;
		if (hitLip(e.pos, sd, y)) {
			module->uiY = y;
			module->uiVel = 0.75f;
			module->uiSide.store(sd);
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
	void onHover(const HoverEvent& e) override {
		int sd; float y;
		if (hitLip(e.pos, sd, y)) { hoverSide = sd; hoverY = y; } else hoverSide = -1;
		OpaqueWidget::onHover(e);
	}
	void onLeave(const LeaveEvent& e) override { hoverSide = -1; }

	static std::string noteName(float f) {
		static const char* N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		if (f <= 0.f) return "-";
		float n = 12.f * std::log2(f / 440.f) + 69.f;
		int r = (int)std::lround(n);
		return string::f("%s%d", N[((r % 12) + 12) % 12], r / 12 - 1);
	}

	// The front half of the ring at height eta, from the outline's right point
	// (x0, y0) to its left point (x1, y1). Seen from above, the outline's outermost
	// points lie a little past where the surface stops facing you, so a ring drawn
	// only over the front-facing angles stopped short of the outline and left a
	// gap at the corners. Pinning both ends to the outline closes it.
	void frontArc(NVGcontext* vg, const GaramutView& v, float eta, float x0, float y0, float x1, float y1, bool move) const {
		const int NT = 72;
		if (move) nvgMoveTo(vg, x0, y0); else nvgLineTo(vg, x0, y0);
		// Turned by the yaw, the front runs from angle yaw (rightmost) to yaw + pi (leftmost).
		for (int k = 0; k <= NT; k++) {
			float th = v.yaw + (float)M_PI * k / NT;
			Vec p = v.at(eta, th);
			if (!v.faces(th)) continue;
			if ((p.x - x0) * (x1 - x0) < 0.f || (p.x - x1) * (x0 - x1) < 0.f) continue;   // outside the two ends
			nvgLineTo(vg, p.x, p.y);
		}
		nvgLineTo(vg, x1, y1);
	}

	// one patch of the surface as a closed path
	void patchPath(NVGcontext* vg, const GaramutView& v, float th0, float th1, float e0, float e1) const {
		const int K = 6, J = 16;
		for (int k = 0; k <= K; k++) { Vec p = v.at(e0, th0 + (th1 - th0) * k / K); if (k == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
		for (int j = 1; j <= J; j++) { Vec p = v.at(e0 + (e1 - e0) * j / J, th1); nvgLineTo(vg, p.x, p.y); }
		for (int k = K - 1; k >= 0; k--) { Vec p = v.at(e1, th0 + (th1 - th0) * k / K); nvgLineTo(vg, p.x, p.y); }
		for (int j = J - 1; j >= 1; j--) { Vec p = v.at(e0 + (e1 - e0) * j / J, th0); nvgLineTo(vg, p.x, p.y); }
		nvgClosePath(vg);
	}

	// A mallet: a stick reaching out to the side and a head. At rest the head
	// waits `gap` outside the column's outline (edgeX) and `lift` above or below
	// its strike height; a stroke carries it in to the strike point and back out.
	void drawMallet(NVGcontext* vg, Vec lipPt, float edgeX, int side, float gap, float lift, float age, float vel, const GmPal& P) const {
		float swing;
		if (age < 0.f || age > 0.3f) swing = 1.f;
		else if (age < 0.025f) swing = 1.f - age / 0.025f;
		else swing = std::min(1.f, (age - 0.025f) / 0.12f);
		float dir = side == 0 ? -1.f : 1.f;
		float headR = mm2px(0.85f);
		float restX = edgeX + dir * (headR + gap);
		float hitX = lipPt.x + dir * headR;
		Vec head = Vec(hitX + (restX - hitX) * swing, lipPt.y + lift * swing);
		Vec tail = Vec(head.x + dir * mm2px(6.5f), head.y + mm2px(1.4f));
		float glow = (age >= 0.f && age < 0.35f) ? (1.f - age / 0.35f) * (0.4f + 0.6f * vel) : 0.f;
		NVGcolor c = nvgLerpRGBA(P.mallet, GM_HOT, glow);
		nvgBeginPath(vg); nvgMoveTo(vg, head.x, head.y); nvgLineTo(vg, tail.x, tail.y);
		nvgStrokeColor(vg, c); nvgStrokeWidth(vg, 1.1f); nvgStroke(vg);
		nvgBeginPath(vg); nvgCircle(vg, head.x, head.y, headR);
		nvgFillColor(vg, c); nvgFill(vg);
	}

	void draw(const DrawArgs& args) override {
		if (!module) { module = garamutPreview(); draw(args); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		GmPal P = gmPal();
		Garamut* m = module;
		const garamut::Drum& D = m->drum;
		GaramutView v = view();
		float half, lip; slitAngles(half, lip);
		const float PI2 = (float)M_PI / 2.f;
		const int NE = 32, NT = 72;

		// the silhouette: extreme left and right at each height
		float lx[NE + 1], ly[NE + 1], rx[NE + 1], ry[NE + 1];
		for (int i = 0; i <= NE; i++) {
			float eta = (float)i / NE;
			float mn = 1e9f, mx = -1e9f; Vec pm, px;
			for (int k = 0; k < NT; k++) {
				Vec p = v.at(eta, 2.f * (float)M_PI * k / NT);
				if (p.x < mn) { mn = p.x; pm = p; }
				if (p.x > mx) { mx = p.x; px = p; }
			}
			lx[i] = pm.x; ly[i] = pm.y; rx[i] = px.x; ry[i] = px.y;
		}
		nvgBeginPath(vg);
		nvgMoveTo(vg, lx[0], ly[0]);
		for (int i = 1; i <= NE; i++) nvgLineTo(vg, lx[i], ly[i]);
		for (int i = NE; i >= 0; i--) nvgLineTo(vg, rx[i], ry[i]);
		frontArc(vg, v, 0.f, rx[0], ry[0], lx[0], ly[0], false);
		nvgClosePath(vg);
		nvgFillColor(vg, P.body);
		nvgFill(vg);

		// the open top: the rim, and the hollow inside it
		nvgBeginPath(vg);
		for (int k = 0; k <= NT; k++) {
			Vec p = v.at(1.f, 2.f * (float)M_PI * k / NT);
			if (k == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgClosePath(vg);
		nvgFillColor(vg, P.top);
		nvgFill(vg);
		nvgBeginPath(vg);
		for (int k = 0; k <= NT; k++) {
			Vec p = v.at(1.f, 2.f * (float)M_PI * k / NT, 1.f - garamut::WALL);
			if (k == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgClosePath(vg);
		nvgFillColor(vg, P.hollow);
		nvgFill(vg);

		// rings round the front, each from the outline's right edge to its left
		nvgStrokeWidth(vg, 0.8f);
		nvgStrokeColor(vg, P.line);
		for (int ri = 1; ri < 8; ri++) {
			int i = ri * NE / 8;
			nvgBeginPath(vg);
			frontArc(vg, v, (float)i / NE, rx[i], ry[i], lx[i], ly[i], true);
			nvgStroke(vg);
		}

		// the lips: they fill orange as they ring
		float e0 = D.slit0, e1 = D.slit1;
		nvgStrokeWidth(vg, 1.f);
		for (int sd = 0; sd < 2; sd++) {
			float a0 = sd == 0 ? PI2 + half : PI2 - half - lip;
			float a1 = sd == 0 ? PI2 + half + lip : PI2 - half;
			float lvl = clamp(D.lipLevel[sd] / 0.8f, 0.f, 1.f);
			nvgBeginPath(vg);
			patchPath(vg, v, a0, a1, e0, e1);
			nvgFillColor(vg, nvgLerpRGBA(P.body, GM_HOT, lvl));
			nvgFill(vg);
			nvgStrokeColor(vg, hoverSide == sd ? GM_HOT : P.ink);
			nvgStroke(vg);
		}
		// the slit
		nvgBeginPath(vg);
		patchPath(vg, v, PI2 - half, PI2 + half, e0, e1);
		nvgFillColor(vg, P.hollow);
		nvgFill(vg);

		// the outline
		nvgStrokeColor(vg, P.ink);
		nvgStrokeWidth(vg, 1.f);
		nvgBeginPath(vg);
		nvgMoveTo(vg, lx[0], ly[0]);
		for (int i = 1; i <= NE; i++) nvgLineTo(vg, lx[i], ly[i]);
		nvgMoveTo(vg, rx[0], ry[0]);
		for (int i = 1; i <= NE; i++) nvgLineTo(vg, rx[i], ry[i]);
		nvgStroke(vg);
		nvgBeginPath(vg);
		frontArc(vg, v, 0.f, rx[0], ry[0], lx[0], ly[0], true);
		nvgStroke(vg);
		nvgBeginPath(vg);
		for (int k = 0; k <= NT; k++) {
			Vec p = v.at(1.f, 2.f * (float)M_PI * k / NT);
			if (k == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgClosePath(vg);
		nvgStroke(vg);

		// a mallet per channel: on each side the first rests closest, the others
		// each a step further out, the step shrinking so sixteen still fit
		auto edgeAt = [&](float eta, int sd) {
			float f = clamp(eta, 0.f, 1.f) * NE;
			int i = std::min((int)f, NE - 1);
			float t = f - i;
			return sd == 0 ? lx[i] + (lx[i + 1] - lx[i]) * t : rx[i] + (rx[i + 1] - rx[i]) * t;
		};
		int count[2] = {0, 0};
		for (int c = 0; c < m->chCount && c < 16; c++) count[m->chSide[c]]++;
		float room[2] = {lx[NE / 2] - mm2px(9.f), box.size.x - rx[NE / 2] - mm2px(9.f)};
		float step[2];
		for (int sd = 0; sd < 2; sd++)
			step[sd] = count[sd] > 1 ? std::min(mm2px(2.4f), std::max(mm2px(0.6f), room[sd] / (count[sd] - 1))) : 0.f;
		float vstep[2];
		for (int sd = 0; sd < 2; sd++)
			vstep[sd] = count[sd] > 1 ? std::min(mm2px(2.0f), mm2px(16.f) / (float)count[sd]) : 0.f;
		int rank[2] = {0, 0};
		for (int c = 0; c < m->chCount && c < 16; c++) {
			int sd = m->chSide[c];
			float eta = e0 + (e1 - e0) * m->chY[c];
			Vec lipPt = v.at(eta, strikeAngle(sd, half, lip));
			float gap = mm2px(0.8f) + step[sd] * rank[sd];
			// the second mallet rests a step above the first, the third a step below,
			// the fourth two steps above, and so on
			int r = rank[sd];
			float lift = r == 0 ? 0.f : (r % 2 ? -1.f : 1.f) * (float)((r + 1) / 2) * vstep[sd];
			drawMallet(vg, lipPt, edgeAt(eta, sd), sd, gap, lift, m->chStruck[c] ? m->now - m->chHit[c] : -1.f, m->chVel[c], P);
			rank[sd]++;
		}
		// a click on the drawing: a stroke that fades
		for (int i = 0; i < GM_MARKS; i++) {
			const Garamut::Mark& k = m->marks[i];
			if (k.ch != GM_MOUSE) continue;
			float age = m->now - k.t;
			if (age < 0.f || age > 0.3f) continue;
			float eta = e0 + (e1 - e0) * k.y;
			Vec lipPt = v.at(eta, strikeAngle(k.side, half, lip));
			drawMallet(vg, lipPt, edgeAt(eta, k.side), k.side, mm2px(0.8f), 0.f, age, k.vel, P);
		}

		// where a click would land
		if (hoverSide >= 0) {
			Vec p = v.at(e0 + (e1 - e0) * hoverY, strikeAngle(hoverSide, half, lip));
			nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, mm2px(0.7f));
			nvgStrokeColor(vg, GM_HOT); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		}

		// the lips' notes either side, the height and the air underneath
		std::shared_ptr<Font> font = sfs::panelFont();
		if (!font || font->handle < 0) return;
		nvgFontFaceId(vg, font->handle);
		nvgTextLetterSpacing(vg, 0.f);
		float xl = mm2px(2.f), xr = box.size.x - mm2px(2.f);
		float y1 = mm2px(4.2f), y2 = mm2px(7.6f), y3 = mm2px(10.6f);
		nvgFontSize(vg, mm2px(sfs::NVG_EM * sfs::TYPE_NOTE));
		nvgFillColor(vg, P.soft);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgText(vg, xl, y1, "LEFT LIP", NULL);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
		nvgText(vg, xr, y1, "RIGHT LIP", NULL);
		float st = 12.f * std::log2(std::max(1e-3f, D.lipF0[1]) / std::max(1e-3f, D.lipF0[0]));
		bool whole = std::fabs(st - std::round(st)) < 0.05f;
		nvgText(vg, xr, y3, (whole ? string::f("%+.0f SEMITONES", st) : string::f("%+.1f SEMITONES", st)).c_str(), NULL);
		float fy = box.size.y - mm2px(1.2f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgText(vg, xl, fy, D.H >= 1.f ? string::f("%.2f M TALL", D.H).c_str() : string::f("%.0f CM TALL", 100.f * D.H).c_str(), NULL);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
		nvgText(vg, xr, fy, string::f("AIR %.0f HZ", D.fH).c_str(), NULL);
		nvgFontSize(vg, mm2px(sfs::NVG_EM * sfs::TYPE_LABEL));
		nvgFillColor(vg, P.ink);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgText(vg, xl, y2, string::f("%s  %.0f Hz", noteName(D.lipF0[0]).c_str(), D.lipF0[0]).c_str(), NULL);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
		nvgText(vg, xr, y2, string::f("%s  %.0f Hz", noteName(D.lipF0[1]).c_str(), D.lipF0[1]).c_str(), NULL);
	}
};

// ── the panel ──
static const float GM_SX = 5.08f, GM_SY = 10.16f, GM_SW = 91.44f, GM_SH = 38.1f;
static const float GM_X[5] = {12.7f, 31.75f, 50.8f, 69.85f, 88.9f};
static const float GM_Y_KNOB = 60.96f, GM_Y_KCV = 73.66f;
static const float GM_Y_TRIM = 90.17f, GM_Y_TCV = 100.33f;
static const float GM_Y_FOOT = 120.65f;
static const float GM_RULE_Y = 80.01f;
static const float GM_FX[4] = {10.16f, 22.86f, 35.56f, 48.26f};

struct GaramutWidget : ModuleWidget {
	GaramutWidget(Garamut* module) {
		setModule(module);
		setPanel(sfs::themedPanel("garamut"));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("GARAMUT");

		GaramutDisplay* disp = new GaramutDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(GM_SX, GM_SY));
		disp->box.size = mm2px(Vec(GM_SW, GM_SH));
		addChild(disp);

		// the drum: five knobs, each over its CV
		static const char* KN[5] = {"SIZE", "SHAPE", "MATERIAL", "SLIT", "MALLET"};
		for (int i = 0; i < 5; i++) {
			addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(GM_X[i], GM_Y_KNOB)), module, Garamut::SIZE_PARAM + i));
			addInput(createInputCentered<sfs::Jack>(mm2px(Vec(GM_X[i], GM_Y_KCV)), module, Garamut::SIZE_INPUT + i));
			lbl->rogan(GM_X[i], GM_Y_KNOB, KN[i]);
			lbl->link(GM_X[i], GM_Y_KNOB, GM_X[i], GM_Y_KCV);
		}
		lbl->divider(GM_SX, GM_RULE_Y, GM_SX + GM_SW, GM_RULE_Y);

		// the playing: four trimpots over their CVs, and TUNE over V/OCT
		static const char* TN[5] = {"Y", "INTERVAL", "DECAY", "ROLL RATE", "TUNE"};
		for (int i = 0; i < 5; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(GM_X[i], GM_Y_TRIM)), module, Garamut::Y_PARAM + i));
			addInput(createInputCentered<sfs::Jack>(mm2px(Vec(GM_X[i], GM_Y_TCV)), module, Garamut::Y_INPUT + i));
			lbl->pairDown(GM_X[i], GM_Y_TRIM, GM_Y_TCV, TN[i]);
		}

		// the foot: the strikes in, the stereo pair out
		static const char* FN[4] = {"GATE L", "GATE R", "ROLL", "VEL"};
		for (int i = 0; i < 4; i++) {
			addInput(createInputCentered<sfs::Jack>(mm2px(Vec(GM_FX[i], GM_Y_FOOT)), module, Garamut::GATE_L_INPUT + i));
			lbl->jack(GM_FX[i], GM_Y_FOOT, FN[i]);
		}
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(GM_X[3], GM_Y_FOOT)), module, Garamut::LEFT_OUTPUT));
		lbl->jackOnPlate(GM_X[3], GM_Y_FOOT, "LEFT");
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(GM_X[4], GM_Y_FOOT)), module, Garamut::RIGHT_OUTPUT));
		lbl->jackOnPlate(GM_X[4], GM_Y_FOOT, "RIGHT");
	}

	void appendContextMenu(Menu* menu) override {
		Garamut* m = dynamic_cast<Garamut*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("A roll alternates the two lips", "", &m->rollAlternates));
		menu->addChild(createMenuLabel("Without it, a roll stays on the lip that channel last struck"));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Click a lip on the drawing to strike it there"));
	}
};

Model* modelGaramut = createModel<Garamut, GaramutWidget>("Garamut");

#include "plugin.hpp"
#include "scales.hpp"
#include <cmath>

// ─── Chance — a garden of forking paths ──────────────────────────────────────
// A generative melodic sequencer with a recognizable CORE you can strain to
// hear through. Set Key / Root and a Start..End window. A fixed, seeded walk
// builds a core skeleton melody (shaped by Gravity = up/down bias and Drift =
// move size). Each cycle, BRANCH is the per-step chance that playback strays
// from the core to a neighbouring note — bounded and NON-cascading, so the core
// always shows through. BRANCH 0 = the core repeats; higher = more wandering.
// HARMONY out carries the complement (core vs stray). Randomize = new core.

static const int NUM_NODES = 8;
static const int NUM_SCALES = sfs::NUM_SCALES;

// Node column centres (mm) — shared by the widget and the display so on-screen
// nodes sit above their per-node controls.
static const float CHANCE_COL_X[NUM_NODES] = {13.f, 28.f, 43.f, 58.f, 73.f, 88.f, 103.f, 118.f};
static const float CHANCE_DISP_X0 = 5.f, CHANCE_DISP_W = 122.f;

// deterministic hash → [0,1) (defines the stable core)
static inline uint32_t hashU(uint32_t seed, int node, int salt) {
	uint32_t h = seed + (uint32_t)node * 0x9E3779B9u + (uint32_t)salt * 0x85EBCA77u;
	h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
	return h;
}
static inline float hashF(uint32_t seed, int node, int salt) {
	return (float)(hashU(seed, node, salt) & 0xFFFFFF) / (float)0x1000000;
}
static inline int ifloordiv(int a, int b) {
	int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q;
}
static inline int reflectPos(int x, int m) {
	if (m <= 0) return 0;
	while (x > m)  x = 2 * m - x;
	while (x < -m) x = -2 * m - x;
	return x;
}
// move magnitude in scale degrees (>=1): drift sets the ceiling, r spreads it.
static int pickOffset(float drift, int scaleSize, float r) {
	int maxStep = 1 + (int)std::lround(clamp(drift, 0.f, 1.f) * (scaleSize - 1));
	int n = 1 + (int)(r * maxStep);
	return n > maxStep ? maxStep : n;
}

struct Chance;

// ─── Display ─────────────────────────────────────────────────────────────────
struct ChanceDisplay : OpaqueWidget {
	Chance* module = nullptr;
	std::shared_ptr<Font> font;
	void drawLayer(const DrawArgs& args, int layer) override;
	void drawScene(const DrawArgs& args, const int* core, const int* play, const bool* rest,
	               const int* seq, int seqLen, int curNode, int maxPos, bool running);
	void drawPreview(const DrawArgs& args);
};

// ─── Module ──────────────────────────────────────────────────────────────────
struct Chance : Module {
	enum ParamId {
		WEIGHT_PARAM,                              // 8 per-node gravity offset
		NDRIFT_PARAM = WEIGHT_PARAM + NUM_NODES,   // 8 per-node drift offset
		KEY_PARAM = NDRIFT_PARAM + NUM_NODES,
		ROOT_PARAM, START_PARAM, END_PARAM, GRAVITY_PARAM, DRIFT_PARAM, BRANCH_PARAM,
		CLOCK_PARAM, GATE_PARAM, RESTS_PARAM, HOLD_PARAM, OCTAVE_PARAM, REPEAT_PARAM, GLIDE_PARAM,
		RANDOMIZE_PARAM, RESET_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT, RESET_INPUT, RANDOMIZE_INPUT, ROOT_CV_INPUT,
		GRAVITY_CV_INPUT, DRIFT_CV_INPUT, BRANCH_CV_INPUT, INPUTS_LEN
	};
	enum OutputId { CV_OUTPUT, GATE_OUTPUT, EOC_OUTPUT, HARMONY_OUTPUT, OUTPUTS_LEN };
	enum LightId { NODE_LIGHT, LIGHTS_LEN = NODE_LIGHT + NUM_NODES };

	uint32_t seed = 1u;
	int rangeOctaves = 1;

	int curScale = 0, curRoot = 0, maxPosDeg = 7;
	float curGateFrac = 0.5f;

	// per node-index (so the display aligns with the columns)
	int  coreNote[NUM_NODES] = {};   // stable seeded skeleton
	int  playNote[NUM_NODES] = {};   // what plays this cycle (core or a stray)
	int  harmNote[NUM_NODES] = {};   // the complement
	bool pathRest[NUM_NODES] = {};
	int  pathDur[NUM_NODES] = {};
	int  seq[NUM_NODES] = {};
	int  seqLen = NUM_NODES;

	int  curStep = 0, curNodeIdx = 0;
	bool started = false;
	int  nodeClocksRemaining = 1;
	float clockPeriod = 0.5f;
	int  samplesSinceClock = 0;
	float internalPhase = 0.f;

	bool gateOpen = false; float gateTimer = 0.f;
	float targetCV = 0.f, targetHarmony = 0.f, currentCV = 0.f, harmonyCV = 0.f;
	bool dispRunning = false;

	dsp::SchmittTrigger clockTrig, resetTrigIn, resetTrigBtn, randTrigIn, randTrigBtn;
	dsp::PulseGenerator eocPulse;

	Chance() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < NUM_NODES; i++) {
			configParam(WEIGHT_PARAM + i, -1.f, 1.f, 0.f, string::f("Node %d gravity", i + 1));
			configParam(NDRIFT_PARAM + i, -1.f, 1.f, 0.f, string::f("Node %d drift", i + 1));
		}
		std::vector<std::string> scaleNames;
		for (int i = 0; i < NUM_SCALES; i++) scaleNames.push_back(sfs::SCALES[i].longName);
		configSwitch(KEY_PARAM, 0.f, (float)(NUM_SCALES - 1), 0.f, "Key / scale", scaleNames);
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root note",
			{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"});
		configSwitch(START_PARAM, 0.f, (float)(NUM_NODES - 1), 0.f, "Start step",
			{"1","2","3","4","5","6","7","8"});
		configSwitch(END_PARAM, 0.f, (float)(NUM_NODES - 1), (float)(NUM_NODES - 1), "End step",
			{"1","2","3","4","5","6","7","8"});
		configParam(GRAVITY_PARAM, -1.f, 1.f, 0.f, "Gravity (down – up)");
		configParam(DRIFT_PARAM, 0.f, 1.f, 0.3f, "Drift (move size)", "%", 0.f, 100.f);
		configParam(BRANCH_PARAM, 0.f, 1.f, 0.25f, "Branch (stray from core)", "%", 0.f, 100.f);
		configParam(CLOCK_PARAM, 0.f, 1.f, 0.5f, "Internal clock rate");
		configParam(GATE_PARAM, 0.f, 1.f, 0.5f, "Gate length", "%", 0.f, 100.f);
		configParam(RESTS_PARAM, 0.f, 1.f, 0.f, "Rests", "%", 0.f, 100.f);
		configParam(HOLD_PARAM, 0.f, 1.f, 0.f, "Held nodes", "%", 0.f, 100.f);
		configParam(OCTAVE_PARAM, 0.f, 1.f, 0.f, "Octave leaps", "%", 0.f, 100.f);
		configParam(REPEAT_PARAM, 0.f, 1.f, 0.f, "Repeat (hold note)", "%", 0.f, 100.f);
		configParam(GLIDE_PARAM, 0.f, 1.f, 0.f, "Glide");
		configButton(RANDOMIZE_PARAM, "Randomize (new core)");
		configButton(RESET_PARAM, "Reset");

		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset");
		configInput(RANDOMIZE_INPUT, "Randomize trigger");
		configInput(ROOT_CV_INPUT, "Root CV (1V/oct)");
		configInput(GRAVITY_CV_INPUT, "Gravity CV");
		configInput(DRIFT_CV_INPUT, "Drift CV");
		configInput(BRANCH_CV_INPUT, "Branch CV");
		configOutput(CV_OUTPUT, "CV (1V/oct)");
		configOutput(GATE_OUTPUT, "Gate");
		configOutput(EOC_OUTPUT, "End of cycle");
		configOutput(HARMONY_OUTPUT, "Harmony — the complement (core vs stray)");

		seed = random::u32();
	}

	float voltsFromPos(int pos) {
		const sfs::Scale& sc = sfs::SCALES[curScale];
		int sz = sc.size;
		int oct = ifloordiv(pos, sz);
		int deg = pos - oct * sz;
		float semis = oct * 12 + (int)std::lround(sc.intervals[deg]);
		return (float)curRoot / 12.f + semis / 12.f;
	}

	// Build the window order + the deterministic core skeleton (stable per seed).
	void computeCore() {
		const sfs::Scale& sc = sfs::SCALES[curScale];
		int sz = sc.size;
		maxPosDeg = sz * rangeOctaves;

		int sIdx = clamp((int)std::round(params[START_PARAM].getValue()), 0, NUM_NODES - 1);
		int eIdx = clamp((int)std::round(params[END_PARAM].getValue()), 0, NUM_NODES - 1);
		int dir = (eIdx >= sIdx) ? 1 : -1;
		seqLen = std::abs(eIdx - sIdx) + 1;
		for (int k = 0; k < seqLen; k++) seq[k] = sIdx + dir * k;

		float gGrav = clamp(params[GRAVITY_PARAM].getValue() + inputs[GRAVITY_CV_INPUT].getVoltage() / 5.f, -1.f, 1.f);
		float gDrift = clamp(params[DRIFT_PARAM].getValue() + inputs[DRIFT_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);

		int pos = 0;
		for (int k = 0; k < seqLen; k++) {
			int ni = seq[k];
			float grav  = clamp(gGrav + params[WEIGHT_PARAM + ni].getValue(), -1.f, 1.f);
			float drift = clamp(gDrift + params[NDRIFT_PARAM + ni].getValue(), 0.f, 1.f);
			int n = pickOffset(drift, sz, hashF(seed, ni, 2));
			int d = (hashF(seed, ni, 3) < (0.5f + 0.5f * grav)) ? 1 : -1;
			pos = reflectPos(pos + d * n, maxPosDeg);
			coreNote[ni] = pos;
		}
	}

	// Each cycle: keep the stable core, then decide live, per-step strays.
	void computeCycle() {
		computeCore();
		const sfs::Scale& sc = sfs::SCALES[curScale];
		int sz = sc.size;
		float gGrav = clamp(params[GRAVITY_PARAM].getValue() + inputs[GRAVITY_CV_INPUT].getVoltage() / 5.f, -1.f, 1.f);
		float gDrift = clamp(params[DRIFT_PARAM].getValue() + inputs[DRIFT_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float gBranch = clamp(params[BRANCH_PARAM].getValue() + inputs[BRANCH_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float gRests = clamp(params[RESTS_PARAM].getValue(), 0.f, 1.f);
		float gHold  = clamp(params[HOLD_PARAM].getValue(), 0.f, 1.f);
		float gOct   = clamp(params[OCTAVE_PARAM].getValue(), 0.f, 1.f);
		float gRep   = clamp(params[REPEAT_PARAM].getValue(), 0.f, 1.f);

		int prev = 0;
		for (int k = 0; k < seqLen; k++) {
			int ni = seq[k];
			int core = coreNote[ni];
			// a bounded stray near the core note (does NOT shift later notes)
			float grav  = clamp(gGrav + params[WEIGHT_PARAM + ni].getValue(), -1.f, 1.f);
			float drift = clamp(gDrift + params[NDRIFT_PARAM + ni].getValue(), 0.f, 1.f);
			int dn = pickOffset(drift, sz, random::uniform());
			int dd = (random::uniform() < (0.5f + 0.5f * grav)) ? 1 : -1;
			int stray = reflectPos(core + dd * dn, maxPosDeg);

			bool deviate = (random::uniform() < gBranch);
			int play = deviate ? stray : core;
			int harm = deviate ? core : stray;

			if (random::uniform() < gRep) play = prev;          // hold the previous note
			if (random::uniform() < gOct)
				play = reflectPos(play + (random::uniform() < 0.5f ? sz : -sz), maxPosDeg);

			playNote[ni] = play;
			harmNote[ni] = harm;
			pathRest[ni] = (random::uniform() < gRests);
			pathDur[ni]  = (random::uniform() < gHold) ? (2 + (int)(random::uniform() * 3.f)) : 1;
			prev = play;
		}
	}

	void fireStep(int k) {
		curNodeIdx = seq[k];
		targetCV = voltsFromPos(playNote[curNodeIdx]);
		targetHarmony = voltsFromPos(harmNote[curNodeIdx]);
		if (!pathRest[curNodeIdx]) { gateOpen = true; gateTimer = std::max(0.001f, curGateFrac * clockPeriod); }
	}

	void onClock() {
		if (!started) { started = true; curStep = 0; computeCycle(); fireStep(0); nodeClocksRemaining = pathDur[seq[0]]; return; }
		if (nodeClocksRemaining > 1) { nodeClocksRemaining--; return; }
		curStep++;
		if (curStep >= seqLen) { curStep = 0; computeCycle(); eocPulse.trigger(1e-3f); }
		fireStep(curStep);
		nodeClocksRemaining = pathDur[seq[curStep]];
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;

		curScale = clamp((int)std::round(params[KEY_PARAM].getValue()), 0, NUM_SCALES - 1);
		int rootKnob = (int)std::round(params[ROOT_PARAM].getValue());
		int rootCv = (int)std::round(inputs[ROOT_CV_INPUT].getVoltage() * 12.f);
		curRoot = ((rootKnob + rootCv) % 12 + 12) % 12;
		curGateFrac = rescale(clamp(params[GATE_PARAM].getValue(), 0.f, 1.f), 0.f, 1.f, 0.05f, 0.95f);

		if (randTrigBtn.process(params[RANDOMIZE_PARAM].getValue())
		    || randTrigIn.process(inputs[RANDOMIZE_INPUT].getVoltage())) {
			seed = random::u32(); computeCycle();
		}
		if (resetTrigBtn.process(params[RESET_PARAM].getValue())
		    || resetTrigIn.process(inputs[RESET_INPUT].getVoltage())) {
			curStep = 0; started = false; nodeClocksRemaining = 1;
		}

		bool clk = false;
		if (inputs[CLOCK_INPUT].isConnected()) {
			samplesSinceClock++;
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage())) {
				clk = true;
				float per = samplesSinceClock * dt;
				if (per > 0.0005f && per < 10.f) clockPeriod = per;
				samplesSinceClock = 0;
			}
		} else {
			float freq = 0.2f * std::pow(100.f, clamp(params[CLOCK_PARAM].getValue(), 0.f, 1.f));
			clockPeriod = 1.f / freq;
			internalPhase += freq * dt;
			if (internalPhase >= 1.f) { internalPhase -= 1.f; clk = true; }
		}
		dispRunning = true;

		if (clk) onClock();
		if (gateOpen) { gateTimer -= dt; if (gateTimer <= 0.f) gateOpen = false; }

		float glide = clamp(params[GLIDE_PARAM].getValue(), 0.f, 1.f);
		if (glide < 1e-4f) { currentCV = targetCV; harmonyCV = targetHarmony; }
		else {
			float tau = glide * glide * 0.5f;
			float c = 1.f - std::exp(-dt / std::max(tau, 1e-4f));
			currentCV += (targetCV - currentCV) * c;
			harmonyCV += (targetHarmony - harmonyCV) * c;
		}

		outputs[CV_OUTPUT].setVoltage(currentCV);
		outputs[HARMONY_OUTPUT].setVoltage(harmonyCV);
		outputs[GATE_OUTPUT].setVoltage(gateOpen ? 10.f : 0.f);
		outputs[EOC_OUTPUT].setVoltage(eocPulse.process(dt) ? 10.f : 0.f);

		for (int i = 0; i < NUM_NODES; i++) {
			float b = (i == curNodeIdx && started) ? (gateOpen ? 1.f : 0.4f) : 0.f;
			lights[NODE_LIGHT + i].setBrightnessSmooth(b, dt);
		}
		if (!started) { static int warm = 0; if (warm++ == 0) computeCycle(); }
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "seed", json_integer((json_int_t)seed));
		json_object_set_new(root, "rangeOctaves", json_integer(rangeOctaves));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "seed")) seed = (uint32_t)json_integer_value(j);
		if (json_t* j = json_object_get(root, "rangeOctaves")) rangeOctaves = clamp((int)json_integer_value(j), 1, 2);
	}
};

// ─── Display impl ────────────────────────────────────────────────────────────
void ChanceDisplay::drawScene(const DrawArgs& args, const int* core, const int* play,
                              const bool* rest, const int* seq, int seqLen, int curNode,
                              int maxPos, bool running) {
	const float w = box.size.x, h = box.size.y;
	nvgBeginPath(args.vg);
	nvgRoundedRect(args.vg, 0, 0, w, h, 3.f);
	nvgFillColor(args.vg, nvgRGB(0x1a, 0x1a, 0x2e));
	nvgFill(args.vg);
	if (maxPos < 1 || seqLen < 1) return;

	const float pad = 6.f;
	auto X = [&](int ni) { return (CHANCE_COL_X[ni] - CHANCE_DISP_X0) / CHANCE_DISP_W * w; };
	auto Y = [&](int p) { return h - pad - (h - 2 * pad) * (0.5f + 0.5f * (float)p / (float)maxPos); };

	bool active[NUM_NODES] = {};
	for (int k = 0; k < seqLen; k++) active[seq[k]] = true;
	for (int i = 0; i < NUM_NODES; i++) {
		if (active[i]) continue;
		nvgBeginPath(args.vg); nvgCircle(args.vg, X(i), Y(0), 1.3f);
		nvgFillColor(args.vg, nvgRGB(0x35, 0x35, 0x4d)); nvgFill(args.vg);
	}

	// core skeleton (faint) — what you strain to hear through
	nvgStrokeColor(args.vg, nvgRGBA(0x6a, 0x6a, 0x8a, 0xb0));
	nvgStrokeWidth(args.vg, 1.f);
	nvgBeginPath(args.vg);
	for (int k = 0; k < seqLen; k++) { float x = X(seq[k]), y = Y(core[seq[k]]); if (k == 0) nvgMoveTo(args.vg, x, y); else nvgLineTo(args.vg, x, y); }
	nvgStroke(args.vg);
	for (int k = 0; k < seqLen; k++) { nvgBeginPath(args.vg); nvgCircle(args.vg, X(seq[k]), Y(core[seq[k]]), 1.4f); nvgFillColor(args.vg, nvgRGBA(0x6a, 0x6a, 0x8a, 0xc0)); nvgFill(args.vg); }

	// played path (bright)
	nvgStrokeColor(args.vg, nvgRGB(0x00, 0x97, 0xde));
	nvgStrokeWidth(args.vg, 1.6f);
	nvgBeginPath(args.vg);
	for (int k = 0; k < seqLen; k++) { float x = X(seq[k]), y = Y(play[seq[k]]); if (k == 0) nvgMoveTo(args.vg, x, y); else nvgLineTo(args.vg, x, y); }
	nvgStroke(args.vg);
	for (int k = 0; k < seqLen; k++) {
		int ni = seq[k];
		bool on = running && (ni == curNode);
		nvgBeginPath(args.vg); nvgCircle(args.vg, X(ni), Y(play[ni]), on ? 3.4f : 2.4f);
		if (rest[ni]) { nvgStrokeColor(args.vg, nvgRGB(0x6a, 0x6a, 0x8a)); nvgStrokeWidth(args.vg, 1.f); nvgStroke(args.vg); }
		else { nvgFillColor(args.vg, on ? nvgRGB(0xec, 0x65, 0x2e) : nvgRGB(0x00, 0x97, 0xde)); nvgFill(args.vg); }
	}
}

void ChanceDisplay::drawPreview(const DrawArgs& args) {
	const int core[8] = {1, 3, 2, 4, 3, 5, 4, 6};
	const int play[8] = {1, 3, 5, 4, 3, 2, 4, 6};   // strays at a couple of steps
	const bool rest[8] = {false, false, false, false, true, false, false, false};
	const int seq[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	drawScene(args, core, play, rest, seq, 8, 2, 8, true);
}

void ChanceDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
	if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	if (!module) { drawPreview(args); return; }
	Chance* m = dynamic_cast<Chance*>(module);
	if (!m) { drawPreview(args); return; }
	drawScene(args, m->coreNote, m->playNote, m->pathRest, m->seq, m->seqLen,
	          m->curNodeIdx, m->maxPosDeg, m->dispRunning);
}

// ─── Widget ──────────────────────────────────────────────────────────────────
struct ChanceWidget : ModuleWidget {
	ChanceWidget(Chance* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/chance.svg")));
		// No virtual screws — see CLAUDE.md (SFS panels omit them by design).

		ChanceDisplay* disp = new ChanceDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(CHANCE_DISP_X0, 12.f));
		disp->box.size = mm2px(Vec(CHANCE_DISP_W, 30.f));
		addChild(disp);

		const float ledY = 47.f, weightY = 57.f, ndriftY = 69.f;
		for (int i = 0; i < NUM_NODES; i++) {
			float x = CHANCE_COL_X[i];
			addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(x, ledY)), module, Chance::NODE_LIGHT + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(x, weightY)), module, Chance::WEIGHT_PARAM + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(x, ndriftY)), module, Chance::NDRIFT_PARAM + i));
		}

		const float r1 = 86.f;
		const struct { float x; int p; } knobs[] = {
			{8, Chance::KEY_PARAM}, {20, Chance::ROOT_PARAM}, {32, Chance::START_PARAM},
			{44, Chance::END_PARAM}, {58, Chance::GRAVITY_PARAM}, {71, Chance::DRIFT_PARAM},
			{84, Chance::BRANCH_PARAM}, {97, Chance::CLOCK_PARAM}, {110, Chance::GATE_PARAM},
			{122, Chance::GLIDE_PARAM},
		};
		for (auto& k : knobs) addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(k.x, r1)), module, k.p));

		const float r2 = 101.f;
		addParam(createParamCentered<Trimpot>(mm2px(Vec(8.f, r2)),  module, Chance::RESTS_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(20.f, r2)), module, Chance::HOLD_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(32.f, r2)), module, Chance::OCTAVE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(44.f, r2)), module, Chance::REPEAT_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(113.f, r2)), module, Chance::RANDOMIZE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(125.f, r2)), module, Chance::RESET_PARAM));

		const float r3 = 117.f;
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.f, r3)),   module, Chance::ROOT_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.f, r3)),  module, Chance::GRAVITY_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.f, r3)),  module, Chance::DRIFT_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(41.f, r3)),  module, Chance::BRANCH_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(53.f, r3)),  module, Chance::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(64.f, r3)),  module, Chance::RANDOMIZE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(75.f, r3)),  module, Chance::RESET_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(90.f, r3)),  module, Chance::CV_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(101.f, r3)), module, Chance::GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(112.f, r3)), module, Chance::EOC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(123.f, r3)), module, Chance::HARMONY_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Chance* m = dynamic_cast<Chance*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Walk range"));
		menu->addChild(createCheckMenuItem("±1 octave", "",
			[m]() { return m->rangeOctaves == 1; }, [m]() { m->rangeOctaves = 1; }));
		menu->addChild(createCheckMenuItem("±2 octaves", "",
			[m]() { return m->rangeOctaves == 2; }, [m]() { m->rangeOctaves = 2; }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("New core (reseed)", "",
			[m]() { m->seed = random::u32(); m->computeCycle(); }));
	}
};

Model* modelChance = createModel<Chance, ChanceWidget>("Chance");

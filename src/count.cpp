// =============================================================================
// Count: a narrow clock, six gate outputs, each a sieve over one stream of
// steps. (Called Sieve until 2026-09-21.)
//
// One tempo (BPM, or a CLOCK in at one pulse per beat) and one cycle length
// (STEPS). Each output has two knobs: DIV sets its own rate against the beat,
// /64 to x64 in the musical steps, and MASK says which of its STEPS pulses
// pass -- all of them, odd, even, a Euclidean share, the front of the cycle,
// the back, a burst. Each output counts its OWN pulses modulo STEPS, so a x2
// output runs a STEPS-long cycle at twice the speed and a /4 output at a
// quarter: polyrhythm and polymeter from two knobs per output, sharing only
// the tempo and the cycle. The Euclidean masks take their density from STEPS
// as a ratio, which is what makes a second selector unnecessary; rotation
// lives in the menu. Design: docs/count-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include <cmath>
#include <string>
#include <vector>

static const int CT_N = 6;                  // outputs
static const int CT_MAXSTEPS = 32;

// DIV positions, x1 in the middle, with the odd ones (1.5, 3, 5) that make a
// polyrhythm against the even ones rather than a subdivision of them.
static const float CT_DIVS[] = {64, 32, 24, 16, 12, 8, 6, 5, 4, 3, 2, 1.5f, 1, 1.5f, 2, 3, 4, 5, 6, 8, 12, 16, 24, 32, 64};
static const int CT_NDIV = (int)(sizeof(CT_DIVS) / sizeof(CT_DIVS[0]));
static const int CT_UNITY = 12;
// the output's pulses per beat
static inline float ctRate(int d) {
	d = clamp(d, 0, CT_NDIV - 1);
	return (d < CT_UNITY) ? 1.f / CT_DIVS[d] : CT_DIVS[d];
}
static inline std::string ctDivName(int d) {
	d = clamp(d, 0, CT_NDIV - 1);
	if (d == CT_UNITY) return "x1";
	float v = CT_DIVS[d];
	std::string num = (v == std::floor(v)) ? string::f("%d", (int)v) : string::f("%.1f", v);
	return ((d < CT_UNITY) ? "/" : "x") + num;
}

// THE MASKS, in the order they sit on the knob. Every one is a shape over the
// cycle, stated so it scales with STEPS: the repeating ones repeat, the
// halves are halves, the Euclidean ones are ratios, and the bursts are a run
// that thins out (or gathers) by one each time. None sits at the end, where
// a knob turned all the way is a mute.
enum CtMask { CT_ALL, CT_ODD, CT_EVEN, CT_PAIRS, CT_THREES, CT_FOURS, CT_FRONT, CT_BACK,
              CT_E3, CT_E716, CT_E58, CT_BURST, CT_BURST_IN, CT_DOWN, CT_NONE, CT_NMASK };
static const char* CT_MASK_NAME[CT_NMASK] = {
	"All", "Odd", "Even", "Pairs (xxoo)", "Threes (xxxo)", "Fours (xxxxoooo)", "Front half", "Back half",
	"Euclid 1/3", "Euclid 7/16", "Euclid 5/8", "Burst out", "Burst in", "Downbeat", "None"};

// A burst: runs of R, R-1, ... 1 hits with a single rest between, then
// silence -- xxxx o xxx o xx o x ooo over sixteen -- with R the largest that
// fits. "Burst in" is the same shape reversed, gathering toward the downbeat
// of the next cycle.
static bool ctBurst(int i, int n, bool in) {
	int R = 1;
	while ((R + 1) * (R + 2) / 2 + R <= n) R++;
	if (in) i = n - 1 - i;
	int pos = 0;
	for (int run = R; run >= 1; run--) {
		if (i < pos + run) return true;
		pos += run;
		if (i == pos) return false;
		pos += 1;
	}
	return false;
}

// Does step `i` of `n` pass mask `m`? Euclidean patterns are Bresenham's:
// step i hits when (i*k) mod n < k, which spreads k hits as evenly as n
// allows and always hits step 0, so every mask starts on the downbeat.
static bool ctPass(int m, int i, int n) {
	if (n <= 0) return false;
	i = ((i % n) + n) % n;
	auto euclid = [&](float ratio) { int k = clamp((int)std::round(n * ratio), 1, n); return ((i * k) % n) < k; };
	switch (m) {
		case CT_ALL:      return true;
		case CT_ODD:      return (i % 2) == 0;           // steps 1, 3, 5 as a player counts them
		case CT_EVEN:     return (i % 2) == 1;
		case CT_PAIRS:    return (i % 4) < 2;
		case CT_THREES:   return (i % 4) < 3;
		case CT_FOURS:    return (i % 8) < 4;
		case CT_FRONT:    return i < (n + 1) / 2;
		case CT_BACK:     return i >= (n + 1) / 2;
		case CT_E3:       return euclid(1.f / 3.f);
		case CT_E716:     return euclid(7.f / 16.f);
		case CT_E58:      return euclid(5.f / 8.f);
		case CT_BURST:    return ctBurst(i, n, false);
		case CT_BURST_IN: return ctBurst(i, n, true);
		case CT_DOWN:     return i == 0;
		case CT_NONE:     return false;
	}
	return false;
}

struct CtDivQ : ParamQuantity {
	std::string getDisplayValueString() override { return ctDivName((int)std::round(getValue())); }
};
struct CtMaskQ : ParamQuantity {
	std::string getDisplayValueString() override { return CT_MASK_NAME[clamp((int)std::round(getValue()), 0, CT_NMASK - 1)]; }
};

struct Count : Module {
	enum ParamId { BPM_PARAM, STEPS_PARAM, ENUMS(DIV_PARAM, CT_N), ENUMS(MASK_PARAM, CT_N), PARAMS_LEN };
	enum InputId { BPM_INPUT, STEPS_INPUT, CLOCK_INPUT, RESET_INPUT, INPUTS_LEN };
	enum OutputId { ENUMS(GATE_OUTPUT, CT_N), OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	double phase = 0.0;                      // beats since reset
	int    steps = 16;
	float  bpmEff = 120.f;
	struct Out {
		long  count = -1;                    // pulses of this output so far
		int   step = 0;                      // the step it is on, 0..steps-1
		bool  fired = false;                 // did the current pulse pass the mask
		int   rot = 0;                       // menu: a fixed offset of the mask
		int   rotStep = 0;                   // menu: how far the mask rotates EACH CYCLE, cumulatively
		int   rotNow = 0;                    // the rotation in force this cycle: rot + rotStep * cycles
		dsp::PulseGenerator trig;
	};
	Out out[CT_N];
	int gateMode = 0;                        // 0 = 5 ms trigger, 1 = half the pulse period
	// external clock, one pulse per beat
	dsp::SchmittTrigger clockTrig, resetTrig;
	double extPeriod = 0.0;                  // seconds between the last two pulses
	double sinceEdge = 0.0;
	long   extBeats = 0;                     // pulses seen since reset
	bool   extValid = false;

	Count() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(BPM_PARAM, 30.f, 300.f, 120.f, "Tempo", " BPM");
		configParam(STEPS_PARAM, 1.f, (float)CT_MAXSTEPS, 16.f, "Steps in the cycle");
		paramQuantities[STEPS_PARAM]->snapEnabled = true;
		for (int i = 0; i < CT_N; i++) {
			configParam<CtDivQ>(DIV_PARAM + i, 0.f, (float)(CT_NDIV - 1), (float)CT_UNITY, string::f("Output %d rate", i + 1));
			paramQuantities[DIV_PARAM + i]->snapEnabled = true;
			configParam<CtMaskQ>(MASK_PARAM + i, 0.f, (float)(CT_NMASK - 1), (float)CT_ALL, string::f("Output %d mask", i + 1));
			paramQuantities[MASK_PARAM + i]->snapEnabled = true;
			configOutput(GATE_OUTPUT + i, string::f("Gate %d", i + 1));
		}
		configInput(BPM_INPUT, "Tempo CV (27 BPM per volt, as Meter)");
		configInput(STEPS_INPUT, "Steps CV (+-5V = +-16 steps)");
		configInput(CLOCK_INPUT, "Clock in, one pulse per beat (overrides BPM)");
		configInput(RESET_INPUT, "Reset");
	}

	void reset() {
		phase = 0.0; extBeats = 0; sinceEdge = 0.0;
		for (int i = 0; i < CT_N; i++) { out[i].count = -1; out[i].step = 0; out[i].fired = false; }
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;
		steps = clamp((int)std::round(params[STEPS_PARAM].getValue() + inputs[STEPS_INPUT].getVoltage() * 3.2f), 1, CT_MAXSTEPS);
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) reset();

		// ── the beat ─────────────────────────────────────────────────────
		// Internal: BPM plus CV. External: the beat is the pulse, measured
		// between edges for the rate in between, and every edge lands the
		// phase exactly on its beat so the outputs never drift from the source.
		bool ext = inputs[CLOCK_INPUT].isConnected();
		if (ext) {
			sinceEdge += dt;
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (sinceEdge > 0.01 && sinceEdge < 10.0) { extPeriod = sinceEdge; extValid = true; }
				sinceEdge = 0.0;
				phase = (double)extBeats;
				extBeats++;
			}
			if (extValid) {
				bpmEff = (float)(60.0 / extPeriod);
				double next = (double)extBeats;              // the beat the next edge will land on
				phase = std::min(phase + dt / extPeriod, next - 1e-6);
			}
		} else {
			bpmEff = clamp(params[BPM_PARAM].getValue() + inputs[BPM_INPUT].getVoltage() * 27.f, 10.f, 999.f);
			phase += (double)bpmEff / 60.0 * dt;
		}

		// ── the outputs ──────────────────────────────────────────────────
		for (int i = 0; i < CT_N; i++) {
			Out& o = out[i];
			int d = clamp((int)std::round(params[DIV_PARAM + i].getValue()), 0, CT_NDIV - 1);
			int m = clamp((int)std::round(params[MASK_PARAM + i].getValue()), 0, CT_NMASK - 1);
			double p = phase * (double)ctRate(d);            // this output's pulses since reset
			long count = (long)std::floor(p);
			if (count != o.count) {
				o.count = count;
				o.step = (int)(((count % steps) + steps) % steps);
				// the mask turns by rotStep every cycle, so a Threes on
				// rotate-by-1 walks its rest through the bar over four cycles
				long cyc = (count >= 0) ? count / steps : 0;
				o.rotNow = (int)(((o.rot + (long)o.rotStep * cyc) % steps + steps) % steps);
				o.fired = ctPass(m, o.step - o.rotNow, steps);
				if (o.fired) o.trig.trigger(0.005f);
			}
			bool high;
			if (gateMode == 0) high = o.trig.process(dt);
			else { o.trig.process(dt); high = o.fired && (p - std::floor(p)) < 0.5; }
			outputs[GATE_OUTPUT + i].setVoltage(high ? 10.f : 0.f);
		}
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "gateMode", json_integer(gateMode));
		json_t* rots = json_array();
		for (int i = 0; i < CT_N; i++) json_array_append_new(rots, json_integer(out[i].rot));
		json_object_set_new(r, "rot", rots);
		json_t* rs = json_array();
		for (int i = 0; i < CT_N; i++) json_array_append_new(rs, json_integer(out[i].rotStep));
		json_object_set_new(r, "rotStep", rs);
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "gateMode")) gateMode = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* rots = json_object_get(r, "rot"))
			for (int i = 0; i < CT_N && i < (int)json_array_size(rots); i++)
				out[i].rot = clamp((int)json_integer_value(json_array_get(rots, i)), 0, CT_MAXSTEPS - 1);
		if (json_t* rs = json_object_get(r, "rotStep"))
			for (int i = 0; i < CT_N && i < (int)json_array_size(rs); i++)
				out[i].rotStep = clamp((int)json_integer_value(json_array_get(rs, i)), 0, CT_MAXSTEPS - 1);
	}
};

// =============================================================================
// The strip: tempo and steps on the left, and on the right one row of dots
// per output, lit where its mask passes and bright on the step it is on.
// =============================================================================
static Count* countPreview() {
	static Count* m = nullptr;
	if (m) return m;
	m = new Count();
	m->params[Count::DIV_PARAM + 1].setValue((float)(CT_UNITY + 2));
	m->params[Count::MASK_PARAM + 1].setValue((float)CT_E3);
	m->params[Count::MASK_PARAM + 2].setValue((float)CT_ODD);
	m->params[Count::DIV_PARAM + 3].setValue((float)(CT_UNITY - 1));
	m->params[Count::MASK_PARAM + 3].setValue((float)CT_BURST);
	m->params[Count::MASK_PARAM + 4].setValue((float)CT_PAIRS);
	m->params[Count::DIV_PARAM + 5].setValue((float)(CT_UNITY + 3));
	m->params[Count::MASK_PARAM + 5].setValue((float)CT_E58);
	int64_t frame = 0;
	sfs::previewRun(*m, 2.3f, frame);
	return m;
}

struct CountDisplay : OpaqueWidget {
	Count* module = nullptr;
	std::shared_ptr<Font> font;
	void draw(const DrawArgs& args) override {
		if (!module) { module = countPreview(); draw(args); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		float W = box.size.x, H = box.size.y;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, W, H, mm2px(0.8f));
		nvgFillColor(vg, sfs::SCREEN_BG); nvgFill(vg);
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			nvgText(vg, mm2px(1.2f), H * 0.3f, string::f("%d", (int)std::round(module->bpmEff)).c_str(), NULL);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, mm2px(1.2f), H * 0.72f, string::f("%d st", module->steps).c_str(), NULL);
		}
		// the masks
		float x0 = mm2px(8.5f), x1 = W - mm2px(1.2f);
		int n = module->steps;
		float pitch = (x1 - x0) / (float)std::max(n, 1);
		float rowH = H / (float)CT_N;
		for (int i = 0; i < CT_N; i++) {
			int m = clamp((int)std::round(module->params[Count::MASK_PARAM + i].getValue()), 0, CT_NMASK - 1);
			float cy = rowH * (i + 0.5f);
			float r = std::min(pitch * 0.32f, rowH * 0.3f);
			for (int s = 0; s < n; s++) {
				bool on = ctPass(m, s - module->out[i].rotNow, n);
				bool here = (s == module->out[i].step);
				nvgBeginPath(vg);
				nvgCircle(vg, x0 + pitch * (s + 0.5f), cy, here ? r * 1.35f : r);
				nvgFillColor(vg, here ? (on ? sfs::SCREEN_HOT : sfs::SCREEN_DIM) : (on ? sfs::SCREEN_BLUE : sfs::SCREEN_PURP));
				nvgFill(vg);
			}
		}
		OpaqueWidget::draw(args);
	}
};

// =============================================================================
// Panel: 8HP, the designer's Figma export (2026-09-21). The art carries its
// own outlined labels, so there is NO sfs::PanelLabels here. The strip under
// the title; CLOCK, BPM and STEPS in one row over RESET and the two CVs; then
// six rows of DIV, MASK and the gate on a plate. Positions are the art's.
// =============================================================================
static const float CT_X_IN = 7.58f, CT_X_BPM = 20.36f, CT_X_STEPS = 32.97f;
static const float CT_Y_TOP = 34.25f, CT_Y_CV = 48.3f;
static const float CT_X_DIV = 11.47f, CT_X_MASK = 21.63f, CT_X_OUT = 32.97f;
static const float CT_Y_OUT[CT_N] = {63.5f, 75.0f, 86.4f, 97.8f, 109.2f, 120.7f};

struct CountWidget : ModuleWidget {
	CountWidget(Count* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/count.svg")));

		CountDisplay* disp = new CountDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(2.62f, 10.16f));
		disp->box.size = mm2px(Vec(35.47f, 15.24f));
		addChild(disp);

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CT_X_IN, CT_Y_TOP)), module, Count::CLOCK_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CT_X_BPM, CT_Y_TOP)), module, Count::BPM_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CT_X_STEPS, CT_Y_TOP)), module, Count::STEPS_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CT_X_IN, CT_Y_CV)), module, Count::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CT_X_BPM, CT_Y_CV)), module, Count::BPM_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CT_X_STEPS, CT_Y_CV)), module, Count::STEPS_INPUT));

		for (int i = 0; i < CT_N; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(CT_X_DIV, CT_Y_OUT[i])), module, Count::DIV_PARAM + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(CT_X_MASK, CT_Y_OUT[i])), module, Count::MASK_PARAM + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CT_X_OUT, CT_Y_OUT[i])), module, Count::GATE_OUTPUT + i));
		}
	}

	void appendContextMenu(Menu* menu) override {
		Count* m = dynamic_cast<Count*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Gate length", {"5 ms trigger", "Half the pulse"}, &m->gateMode));
		// ROTATION SHOWS ITS RESULT. A repeating mask rotated by its own
		// period is unchanged -- Threes by 4, Odd by 2 -- and those are the
		// first amounts anyone tries, so each entry carries the row it would
		// give over the current cycle, and a no-op reads as one.
		menu->addChild(createMenuLabel("Rotate each cycle by (steps): the mask walks round the bar"));
		for (int i = 0; i < CT_N; i++) {
			int n = m->steps;
			std::vector<std::string> names;
			for (int k = 0; k < n; k++) names.push_back(k ? string::f("%d", k) : "0 (still)");
			menu->addChild(createIndexSubmenuItem(string::f("Output %d", i + 1), names,
				[=]() { return std::min(m->out[i].rotStep, n - 1); },
				[=](int v) { m->out[i].rotStep = clamp(v, 0, CT_MAXSTEPS - 1); }));
		}
		menu->addChild(createMenuLabel("Offset a mask (steps; the row it gives)"));
		for (int i = 0; i < CT_N; i++) {
			int n = m->steps;
			int mk = clamp((int)std::round(m->params[Count::MASK_PARAM + i].getValue()), 0, CT_NMASK - 1);
			std::vector<std::string> names;
			for (int k = 0; k < n; k++) {
				std::string row;
				for (int st = 0; st < n; st++) row += ctPass(mk, st - k, n) ? 'x' : '.';
				names.push_back(string::f("%2d   %s", k, row.c_str()));
			}
			menu->addChild(createIndexSubmenuItem(string::f("Output %d  (%s)", i + 1, CT_MASK_NAME[mk]), names,
				[=]() { return std::min(m->out[i].rot, n - 1); },
				[=](int v) { m->out[i].rot = clamp(v, 0, CT_MAXSTEPS - 1); }));
		}
	}
};

Model* modelCount = createModel<Count, CountWidget>("Count");

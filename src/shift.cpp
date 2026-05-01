#include "plugin.hpp"
#include <cmath>


// 4-output CV shift register, per-output controls.
//   Each output has its own N pot (1-16) and parallel/cascade mode switch.
//   - Parallel: this output is an independent divider on the input clock —
//     it ticks every N input clocks and samples the input CV on each tick.
//   - Cascade:  this output ticks every N ticks of the previous output. Use
//     to chain dividers and produce exponentially slower taps.
//     Cascade on output A falls back to parallel (no preceding output).
//
// All four outputs share a single optional N CV input that sums into every
// per-output N pot (so you can sweep the whole register's rate at once).

static const int NUM_OUTS = 4;
static const int MAX_N    = 16;


struct Shift : Module {
	enum ParamId {
		N_PARAM_A,
		N_PARAM_B,
		N_PARAM_C,
		N_PARAM_D,
		MODE_PARAM_A,
		MODE_PARAM_B,
		MODE_PARAM_C,
		MODE_PARAM_D,
		PARAMS_LEN
	};
	enum InputId {
		CV_INPUT,
		CLOCK_INPUT,
		N_CV_INPUT,
		RESET_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_A,
		OUT_B,
		OUT_C,
		OUT_D,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHT_A,
		LIGHT_B,
		LIGHT_C,
		LIGHT_D,
		LIGHTS_LEN
	};

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger resetTrigger;

	int   counter[NUM_OUTS] = {0, 0, 0, 0};
	float held[NUM_OUTS]    = {0.f, 0.f, 0.f, 0.f};
	float ledFlash[NUM_OUTS]= {0.f, 0.f, 0.f, 0.f};

	struct NParamQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			return string::f("%.0f", std::round(getValue()));
		}
	};

	Shift() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		const char* labels[NUM_OUTS] = {"A", "B", "C", "D"};
		for (int i = 0; i < NUM_OUTS; i++) {
			configParam<NParamQuantity>(N_PARAM_A + i, 1.f, (float)MAX_N, 4.f,
				string::f("Step count %s", labels[i]));
			paramQuantities[N_PARAM_A + i]->snapEnabled = true;
			configSwitch(MODE_PARAM_A + i, 0.f, 1.f, 0.f,
				string::f("Mode %s", labels[i]),
				{"Parallel", "Cascade"});
			configOutput(OUT_A + i, string::f("Output %s", labels[i]));
		}
		configInput(CV_INPUT,    "CV (data, sampled on each tap fire)");
		configInput(CLOCK_INPUT, "Clock (advances all taps in parallel mode, "
		                        "and the cascade chain root)");
		configInput(N_CV_INPUT,  "N CV (±5V → ±16, summed into every N pot)");
		configInput(RESET_INPUT, "Reset (clears all counters and held values)");
	}

	void onReset() override {
		for (int i = 0; i < NUM_OUTS; i++) {
			counter[i] = 0;
			held[i] = 0.f;
			ledFlash[i] = 0.f;
		}
	}

	void process(const ProcessArgs& args) override {
		// Global N CV — added to every per-output N pot before clamping.
		float nCv = inputs[N_CV_INPUT].isConnected()
			? inputs[N_CV_INPUT].getVoltage() * (float)MAX_N / 5.f
			: 0.f;

		int targetN[NUM_OUTS];
		for (int i = 0; i < NUM_OUTS; i++) {
			float n = params[N_PARAM_A + i].getValue() + nCv;
			int ni = (int)std::round(n);
			if (ni < 1)     ni = 1;
			if (ni > MAX_N) ni = MAX_N;
			targetN[i] = ni;
		}

		// --- Reset ---
		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			for (int i = 0; i < NUM_OUTS; i++) {
				counter[i] = 0;
				held[i] = 0.f;
			}
		}

		// --- Clock processing ---
		bool clockRose = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		bool tickFired[NUM_OUTS] = {false, false, false, false};
		float inCV = inputs[CV_INPUT].getVoltage();

		if (clockRose) {
			for (int i = 0; i < NUM_OUTS; i++) {
				bool cascade = params[MODE_PARAM_A + i].getValue() > 0.5f;
				bool advance;
				if (cascade && i > 0) {
					// Cascade: advance only when previous output ticked.
					advance = tickFired[i - 1];
				} else {
					// Parallel (or cascade-on-A which has no source).
					advance = true;
				}
				if (!advance) continue;
				counter[i]++;
				if (counter[i] >= targetN[i]) {
					counter[i] = 0;
					held[i] = inCV;
					tickFired[i] = true;
				}
			}
		}

		// --- LED flash (~120ms decay) + outputs ---
		float decay = args.sampleTime / 0.12f;
		for (int i = 0; i < NUM_OUTS; i++) {
			if (tickFired[i]) ledFlash[i] = 1.f;
			else ledFlash[i] = std::max(0.f, ledFlash[i] - decay);
			lights[LIGHT_A + i].setBrightness(ledFlash[i]);
			outputs[OUT_A + i].setVoltage(held[i]);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* heldJ = json_array();
		json_t* cntJ  = json_array();
		for (int i = 0; i < NUM_OUTS; i++) {
			json_array_append_new(heldJ, json_real(held[i]));
			json_array_append_new(cntJ,  json_integer(counter[i]));
		}
		json_object_set_new(root, "held",     heldJ);
		json_object_set_new(root, "counters", cntJ);
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "held")) {
			for (int i = 0; i < NUM_OUTS && i < (int)json_array_size(j); i++) {
				held[i] = (float)json_number_value(json_array_get(j, i));
			}
		}
		if (json_t* j = json_object_get(root, "counters")) {
			for (int i = 0; i < NUM_OUTS && i < (int)json_array_size(j); i++) {
				counter[i] = (int)json_integer_value(json_array_get(j, i));
			}
		}
	}
};


// --- Widget ---

struct ShiftWidget : ModuleWidget {
	ShiftWidget(Shift* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/shift.svg")));

		// --- Inputs row (top) ---
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.f, 20.f)), module, Shift::CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.f, 20.f)), module, Shift::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.f, 20.f)), module, Shift::N_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(52.f, 20.f)), module, Shift::RESET_INPUT));

		// --- Per-output rows ---
		const float rowYs[NUM_OUTS] = { 46.f, 66.f, 86.f, 106.f };
		const float xPot   = 14.f;
		const float xMode  = 22.f;
		const float xLED   = 40.f;
		const float xJack  = 48.f;
		for (int i = 0; i < NUM_OUTS; i++) {
			float y = rowYs[i];
			addParam(createParamCentered<RoundBlackKnob>(
				mm2px(Vec(xPot, y)), module, Shift::N_PARAM_A + i));
			addParam(createParamCentered<CKSS>(
				mm2px(Vec(xMode, y)), module, Shift::MODE_PARAM_A + i));
			addChild(createLightCentered<SmallLight<WhiteLight>>(
				mm2px(Vec(xLED, y)), module, Shift::LIGHT_A + i));
			addOutput(createOutputCentered<PJ301MPort>(
				mm2px(Vec(xJack, y)), module, Shift::OUT_A + i));
		}
	}
};


Model* modelShift = createModel<Shift, ShiftWidget>("Shift");

#include "plugin.hpp"
#include <cmath>

// =============================================================================
// VAC — Semi-stable A/R envelope with vactrol-like timing drift
//
// Classic attack/release envelope shape, but with a per-stage STAB control
// that introduces controlled cycle-to-cycle variation in timing. Each
// rising-edge trigger samples fresh random factors for the rise and fall
// stages, scaled exponentially by their respective STAB knobs:
//
//   factor = exp(STAB * r * ln(2.5))    where r = random_uniform(0..1)
//
// STAB=0 → factor=1 (perfectly stable, like any normal envelope)
// STAB=+1 → factor in [1, 2.5]  (cycles are 1x–2.5x longer)
// STAB=-1 → factor in [0.4, 1]  (cycles are 0.4x–1x shorter)
//
// The exponential form is log-symmetric around 1, so STAB never collapses the
// stage to zero — and it matches how real vactrols actually drift (LED-driven
// photoresistor response is multiplicative, not additive).
//
// CURVE blends linear-rate / exponential-rate stage shaping, Swell-style, and
// has its own CV input. LOOP latches re-triggering when the fall completes.
// END pulses 1ms at the end of each fall stage.
// Context-menu opt-in: CONTINUOUS DRIFT — instead of one random factor per
// stage, the rate wobbles smoothly throughout the stage for an even more
// "thermal" feel.
// =============================================================================

static const float STAB_LOG_K = 0.9162907f;  // ln(2.5)


struct Vac : Module {
	enum ParamId {
		RISE_PARAM,
		RISE_STAB_PARAM,
		FALL_PARAM,
		FALL_STAB_PARAM,
		CURVE_PARAM,
		LOOP_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,
		RISE_CV_INPUT,
		RISE_STAB_CV_INPUT,
		FALL_CV_INPUT,
		FALL_STAB_CV_INPUT,
		CURVE_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		END_OUTPUT,
		ENV_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LOOP_LIGHT,
		LIGHTS_LEN
	};

	enum Stage { IDLE, RISING, FALLING };

	// State
	Stage stage = IDLE;
	float V = 0.f;                   // current envelope voltage (0..10)
	float curRiseLen = 0.5f;          // sampled per trigger
	float curFallLen = 0.5f;
	dsp::SchmittTrigger trigTrigger;
	dsp::PulseGenerator endPulse;

	// Context menu
	bool continuousDrift = false;
	// Continuous-drift state (smoothed random per stage)
	float driftSmoothed[2] = {};      // 0 = rise, 1 = fall

	// --- Param quantities ---
	struct RiseFallQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float t = 0.001f * std::pow(5000.f, getValue());  // 1ms..5s exponential
			if (t < 0.1f) return string::f("%.0f ms", t * 1000.f);
			return string::f("%.2f s", t);
		}
	};
	struct StabQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float v = getValue();
			if (std::fabs(v) < 0.01f) return "Stable";
			float ratio = std::exp(std::fabs(v) * STAB_LOG_K);
			return string::f("%s up to %.2fx", v > 0 ? "Longer" : "Shorter", ratio);
		}
	};
	struct CurveQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			float c = getValue();
			if (c < 0.05f) return "Linear";
			if (c > 0.95f) return "Exponential";
			return string::f("%.0f%% exp", c * 100.f);
		}
	};

	Vac() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<RiseFallQuantity>(RISE_PARAM, 0.f, 1.f, 0.3f, "Rise time");
		configParam<StabQuantity>(RISE_STAB_PARAM, -1.f, 1.f, 0.f, "Rise stability");
		configParam<RiseFallQuantity>(FALL_PARAM, 0.f, 1.f, 0.55f, "Fall time");
		configParam<StabQuantity>(FALL_STAB_PARAM, -1.f, 1.f, 0.f, "Fall stability");
		configParam<CurveQuantity>(CURVE_PARAM, 0.f, 1.f, 0.6f, "Curve (linear → exponential)");
		configButton(LOOP_PARAM, "Loop (auto-retrigger at end of fall)");

		configInput(TRIG_INPUT,         "Trigger (rising edge fires one A/R cycle)");
		configInput(RISE_CV_INPUT,      "Rise CV (±5V → ±50%)");
		configInput(RISE_STAB_CV_INPUT, "Rise stability CV (±5V → ±50%)");
		configInput(FALL_CV_INPUT,      "Fall CV (±5V → ±50%)");
		configInput(FALL_STAB_CV_INPUT, "Fall stability CV (±5V → ±50%)");
		configInput(CURVE_CV_INPUT,     "Curve CV (±5V → ±50%)");
		configOutput(END_OUTPUT,        "End-of-cycle trigger (1ms pulse at end of fall)");
		configOutput(ENV_OUTPUT,        "Envelope (0–10V)");
	}

	void onReset() override {
		stage = IDLE;
		V = 0.f;
		driftSmoothed[0] = 0.f;
		driftSmoothed[1] = 0.f;
		continuousDrift = false;
	}

	// Helper: clamp(param + cv*0.1, [lo, hi]). 50%/V scaling for normalized params.
	static float readParamCV(Param& p, Input& cv, float lo, float hi) {
		float v = p.getValue();
		if (cv.isConnected()) v += cv.getVoltage() * 0.1f * (hi - lo);
		return clamp(v, lo, hi);
	}

	// Sample a fresh stage length given base and stability.
	// factor = exp(stab * r * ln(2.5))
	static float sampleStageLen(float baseLen, float stab) {
		float r = random::uniform();
		return baseLen * std::exp(stab * r * STAB_LOG_K);
	}

	// Convert normalized rise/fall param (0..1) → seconds (1ms..5s exponential).
	static float paramToSeconds(float v) {
		return 0.001f * std::pow(5000.f, clamp(v, 0.f, 1.f));
	}

	void process(const ProcessArgs& args) override {
		// --- Read params + CV ---
		float baseRise = paramToSeconds(readParamCV(params[RISE_PARAM],
			inputs[RISE_CV_INPUT], 0.f, 1.f));
		float baseFall = paramToSeconds(readParamCV(params[FALL_PARAM],
			inputs[FALL_CV_INPUT], 0.f, 1.f));
		float riseStab = readParamCV(params[RISE_STAB_PARAM],
			inputs[RISE_STAB_CV_INPUT], -1.f, 1.f);
		float fallStab = readParamCV(params[FALL_STAB_PARAM],
			inputs[FALL_STAB_CV_INPUT], -1.f, 1.f);
		float curve    = readParamCV(params[CURVE_PARAM], inputs[CURVE_CV_INPUT], 0.f, 1.f);

		// --- LOOP state: latch button ---
		bool looping = params[LOOP_PARAM].getValue() > 0.5f;
		lights[LOOP_LIGHT].setBrightness(looping ? 1.f : 0.f);

		// --- TRIG: rising-edge starts a new cycle. Also auto-start when LOOP
		// turns on from IDLE, so toggling LOOP doesn't need an external trigger
		// to kick the cycle off. ---
		bool trigEdge = trigTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		bool loopKick = (stage == IDLE) && looping;
		if (trigEdge || loopKick) {
			curRiseLen = std::max(0.001f, sampleStageLen(baseRise, riseStab));
			curFallLen = std::max(0.001f, sampleStageLen(baseFall, fallStab));
			driftSmoothed[0] = driftSmoothed[1] = 0.f;
			stage = RISING;
			// Note: don't reset V — retrigger smoothly from wherever we are.
		}

		// --- Continuous drift (opt-in) ---
		// Smoothly wobbles the effective stage rate during the stage. We use
		// a one-pole LPF on white noise, but at higher gain than wAtaWANDER's
		// implementation since here we want noticeable rate variation.
		auto driftFactor = [&](int idx, float stab) -> float {
			if (!continuousDrift || std::fabs(stab) < 1e-4f) return 1.f;
			float target = random::uniform() * 2.f - 1.f;
			float alpha = 1.f - std::exp(-2.f * (float)M_PI * 2.f * args.sampleTime);
			driftSmoothed[idx] += alpha * (target - driftSmoothed[idx]);
			// Amplify since LPF on white noise has tiny RMS; this gets back to
			// roughly ±1 range during a stage.
			float scaled = clamp(driftSmoothed[idx] * 10.f, -1.f, 1.f);
			return std::exp(stab * scaled * STAB_LOG_K);
		};

		// --- State machine ---
		if (stage == RISING) {
			float effLen = curRiseLen * driftFactor(0, riseStab);
			float linRate = 10.f / effLen;
			float expRate = (10.f - V) * 3.f / effLen;
			float rate = (1.f - curve) * linRate + curve * expRate;
			V += rate * args.sampleTime;
			if (V >= 10.f - 0.005f) {
				V = 10.f;
				stage = FALLING;
				driftSmoothed[1] = 0.f;
			}
		} else if (stage == FALLING) {
			float effLen = curFallLen * driftFactor(1, fallStab);
			float linRate = -10.f / effLen;
			float expRate = -V * 3.f / effLen;
			float rate = (1.f - curve) * linRate + curve * expRate;
			V += rate * args.sampleTime;
			if (V <= 0.005f) {
				V = 0.f;
				endPulse.trigger(0.001f);
				if (looping) {
					// Re-sample stage lengths and fire next cycle.
					curRiseLen = std::max(0.001f, sampleStageLen(baseRise, riseStab));
					curFallLen = std::max(0.001f, sampleStageLen(baseFall, fallStab));
					driftSmoothed[0] = 0.f;
					stage = RISING;
				} else {
					stage = IDLE;
				}
			}
		}

		if (V < 0.f) V = 0.f;
		if (V > 10.f) V = 10.f;

		outputs[ENV_OUTPUT].setVoltage(V);
		outputs[END_OUTPUT].setVoltage(endPulse.process(args.sampleTime) ? 10.f : 0.f);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "continuousDrift", json_boolean(continuousDrift));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "continuousDrift"))
			continuousDrift = json_boolean_value(j);
	}
};


// =============================================================================
// Widget
// =============================================================================

struct VacWidget : ModuleWidget {
	VacWidget(Vac* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/vac.svg")));

		// 2-column layout; row centers read from the reticules in res/vac.svg
		// (SVG units ÷ 2.83465 → mm). Columns: xL=7.62, xR=22.86.
		const float xL = 7.62f;
		const float xR = 22.86f;

		// Rise group
		addParam(createParamCentered<Trimpot>(mm2px(Vec(xL, 20.32f)), module, Vac::RISE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xR, 20.32f)), module, Vac::RISE_CV_INPUT));

		addParam(createParamCentered<Trimpot>(mm2px(Vec(xL, 35.56f)), module, Vac::RISE_STAB_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xR, 35.56f)), module, Vac::RISE_STAB_CV_INPUT));

		// Fall / curve group
		addParam(createParamCentered<Trimpot>(mm2px(Vec(xL, 55.88f)), module, Vac::FALL_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xR, 55.88f)), module, Vac::FALL_CV_INPUT));

		addParam(createParamCentered<Trimpot>(mm2px(Vec(xL, 71.12f)), module, Vac::FALL_STAB_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xR, 71.12f)), module, Vac::FALL_STAB_CV_INPUT));

		addParam(createParamCentered<Trimpot>(mm2px(Vec(xL, 86.35f)), module, Vac::CURVE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xR, 86.35f)), module, Vac::CURVE_CV_INPUT));

		// LOOP — light latch with embedded green LED + TRIG in
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(xL, 106.67f)), module, Vac::LOOP_PARAM, Vac::LOOP_LIGHT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xR, 106.67f)), module, Vac::END_OUTPUT));

		// END trig out, ENV audio out
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xL, 121.92f)), module, Vac::TRIG_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xR, 121.92f)), module, Vac::ENV_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Vac* v = dynamic_cast<Vac*>(module);
		if (!v) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Continuous drift (rate wobbles during stage)", "",
			&v->continuousDrift));
	}
};


Model* modelVac = createModel<Vac, VacWidget>("Vac");

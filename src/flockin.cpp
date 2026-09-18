#include "plugin.hpp"
#include "panel-style.hpp"
#include "flock-messages.hpp"
#include <cmath>

// ─── Flock In — live audio into Flock, as an expander on its LEFT ────────────
// Patched, the birds stop singing sines and sing the last REACH seconds of
// whatever comes in here: each call is a grain read from a ring buffer that
// Flock keeps, at the pitch the bird's height gives it, from a point in the
// buffer its depth gives it. ENV is the grain's attack fraction, FREEZE holds
// the buffer. Everything else the flock needs is on Flock.

static inline float fiReachSec(float k) { return 0.05f * std::pow(200.f, clamp(k, 0.f, 1.f)); }   // 50 ms .. 10 s

struct FlockInReachQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float s = fiReachSec(getValue());
		return s < 1.f ? string::f("%.0f ms", s * 1000.f) : string::f("%.2f s", s);
	}
};

struct FlockIn : Module {
	enum ParamId { ENV_PARAM, REACH_PARAM, FREEZE_PARAM, PARAMS_LEN };
	enum InputId { L_INPUT, R_INPUT, ENV_INPUT, REACH_INPUT, FREEZE_INPUT, INPUTS_LEN };
	enum OutputId { OUTPUTS_LEN };
	enum LightId { FREEZE_LIGHT, LIGHTS_LEN };

	FlockIn() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(ENV_PARAM, 0.f, 1.f, 0.25f, "Envelope shape (attack as a fraction of the call)", "%", 0.f, 100.f);
		configParam<FlockInReachQ>(REACH_PARAM, 0.f, 1.f, 0.57f, "Reach (how far back the birds may read)");
		configSwitch(FREEZE_PARAM, 0.f, 1.f, 0.f, "Freeze (hold the buffer)", {"Live", "Frozen"});
		configInput(L_INPUT, "Left audio");
		configInput(R_INPUT, "Right audio (normalled from left)");
		configInput(ENV_INPUT, "Envelope shape CV (+-5V = full range)");
		configInput(REACH_INPUT, "Reach CV (+-5V = full range)");
		configInput(FREEZE_INPUT, "Freeze gate (above 1V holds)");
		rightExpander.producerMessage = new FlockInMessage();
		rightExpander.consumerMessage = new FlockInMessage();
	}
	~FlockIn() override {
		delete (FlockInMessage*)rightExpander.producerMessage;
		delete (FlockInMessage*)rightExpander.consumerMessage;
	}

	inline float pv(int p, int in) {
		float v = params[p].getValue();
		if (inputs[in].isConnected()) v += inputs[in].getVoltage() * 0.1f;
		return clamp(v, 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		bool frozen = params[FREEZE_PARAM].getValue() > 0.5f
		           || (inputs[FREEZE_INPUT].isConnected() && inputs[FREEZE_INPUT].getVoltage() >= 1.f);
		lights[FREEZE_LIGHT].setBrightness(frozen ? 1.f : 0.f);
		if (!(rightExpander.module && rightExpander.module->model == modelFlock)) return;
		auto* m = (FlockInMessage*)rightExpander.producerMessage;
		float l = inputs[L_INPUT].getVoltageSum();
		float r = inputs[R_INPUT].isConnected() ? inputs[R_INPUT].getVoltageSum() : l;
		m->l = l; m->r = r;
		m->env = pv(ENV_PARAM, ENV_INPUT);
		m->reach = fiReachSec(pv(REACH_PARAM, REACH_INPUT));
		m->freeze = frozen;
		m->on = inputs[L_INPUT].isConnected() || inputs[R_INPUT].isConnected();
		rightExpander.requestMessageFlip();
	}
};

// 4HP, the designer's Figma export (2026-09): one column, each control over
// its jack, the audio pair at the foot. The art carries its own outlined
// labels, so there is NO sfs::PanelLabels here -- adding one doubles them.
static const float FI_X = 10.16f, FI_XL = 5.04f, FI_XR = 15.19f;
static const float FI_Y_ENV = 40.59f, FI_Y_ENV_CV = 52.27f;
static const float FI_Y_REACH = 66.83f, FI_Y_REACH_CV = 78.51f;
static const float FI_Y_FREEZE = 93.07f, FI_Y_FREEZE_CV = 104.76f;
static const float FI_Y_IN = 120.67f;

struct FlockInWidget : ModuleWidget {
	FlockInWidget(FlockIn* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/flockin.svg")));

		addParam(createParamCentered<Trimpot>(mm2px(Vec(FI_X, FI_Y_ENV)), module, FlockIn::ENV_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_X, FI_Y_ENV_CV)), module, FlockIn::ENV_INPUT));

		addParam(createParamCentered<Trimpot>(mm2px(Vec(FI_X, FI_Y_REACH)), module, FlockIn::REACH_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_X, FI_Y_REACH_CV)), module, FlockIn::REACH_INPUT));

		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(FI_X, FI_Y_FREEZE)), module, FlockIn::FREEZE_PARAM, FlockIn::FREEZE_LIGHT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_X, FI_Y_FREEZE_CV)), module, FlockIn::FREEZE_INPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_XL, FI_Y_IN)), module, FlockIn::L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_XR, FI_Y_IN)), module, FlockIn::R_INPUT));
	}
};

Model* modelFlockIn = createModel<FlockIn, FlockInWidget>("FlockIn");

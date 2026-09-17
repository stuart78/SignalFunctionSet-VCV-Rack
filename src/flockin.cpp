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

// 4HP: two columns, the audio pair at the top, then each control beside its jack.
static const float FI_XL = 5.08f, FI_XR = 15.24f, FI_XM = 10.16f;
static const float FI_Y_IN = 26.f, FI_Y_ENV = 52.f, FI_Y_REACH = 76.f, FI_Y_FREEZE = 100.f;

struct FlockInWidget : ModuleWidget {
	FlockInWidget(FlockIn* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/flockin.svg")));

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(2.2f, 8.f, "FLOCK");
		lbl->note(FI_XM, 13.f, "IN");

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_XL, FI_Y_IN)), module, FlockIn::L_INPUT));
		lbl->jack(FI_XL, FI_Y_IN, "L");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_XR, FI_Y_IN)), module, FlockIn::R_INPUT));
		lbl->jack(FI_XR, FI_Y_IN, "R");

		// labels sit over the pair, not the pot: centred on the pot they ran
		// off the left edge of a four-HP panel
		addParam(createParamCentered<Trimpot>(mm2px(Vec(FI_XL, FI_Y_ENV)), module, FlockIn::ENV_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_XR, FI_Y_ENV)), module, FlockIn::ENV_INPUT));
		lbl->link(FI_XL, FI_Y_ENV, FI_XR, FI_Y_ENV);
		lbl->add(FI_XM, FI_Y_ENV - 5.4f, "ENV");

		addParam(createParamCentered<Trimpot>(mm2px(Vec(FI_XL, FI_Y_REACH)), module, FlockIn::REACH_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_XR, FI_Y_REACH)), module, FlockIn::REACH_INPUT));
		lbl->link(FI_XL, FI_Y_REACH, FI_XR, FI_Y_REACH);
		lbl->add(FI_XM, FI_Y_REACH - 5.4f, "REACH");

		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(FI_XL, FI_Y_FREEZE)), module, FlockIn::FREEZE_PARAM, FlockIn::FREEZE_LIGHT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FI_XR, FI_Y_FREEZE)), module, FlockIn::FREEZE_INPUT));
		lbl->link(FI_XL, FI_Y_FREEZE, FI_XR, FI_Y_FREEZE);
		lbl->add(FI_XM, FI_Y_FREEZE - 5.4f, "FREEZE");
	}
};

Model* modelFlockIn = createModel<FlockIn, FlockInWidget>("FlockIn");

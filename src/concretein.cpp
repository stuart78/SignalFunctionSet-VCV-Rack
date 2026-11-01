#include "plugin.hpp"
#include "panel-style.hpp"
#include "concrete-messages.hpp"

// ─── Concrete In — recording into Concrete's lanes, as an expander on its LEFT ──────
// Per lane: a stereo input (R normalled from L) and REC, a latch and a gate
// that each arm it. Recording happens only while the tape is moving, and it
// replaces what is on the lane under the playhead for as long as it lasts.
// Concrete holds the buffers; this module only sends the audio and the REC state.
struct ConcreteIn : Module {
	enum ParamId { ENUMS(REC_PARAM, CONCRETE_LANES), PARAMS_LEN };
	enum InputId { ENUMS(L_INPUT, CONCRETE_LANES), ENUMS(R_INPUT, CONCRETE_LANES), ENUMS(REC_INPUT, CONCRETE_LANES), INPUTS_LEN };
	enum OutputId { OUTPUTS_LEN };
	enum LightId { ENUMS(REC_LIGHT, CONCRETE_LANES), LIGHTS_LEN };

	dsp::SchmittTrigger recGate[CONCRETE_LANES];
	dsp::ClockDivider lightDiv;
	float blink = 0.f;

	ConcreteIn() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int l = 0; l < CONCRETE_LANES; l++) {
			configSwitch(REC_PARAM + l, 0.f, 1.f, 0.f, string::f("Lane %d record", l + 1), {"Off", "Armed"});
			configInput(L_INPUT + l, string::f("Lane %d left", l + 1));
			configInput(R_INPUT + l, string::f("Lane %d right (normalled from left)", l + 1));
			configInput(REC_INPUT + l, string::f("Lane %d record gate (high records)", l + 1));
		}
		rightExpander.producerMessage = new ConcreteInMessage();
		rightExpander.consumerMessage = new ConcreteInMessage();
		lightDiv.setDivision(64);
	}
	~ConcreteIn() override {
		delete (ConcreteInMessage*)rightExpander.producerMessage;
		delete (ConcreteInMessage*)rightExpander.consumerMessage;
	}

	void process(const ProcessArgs& args) override {
		bool armed[CONCRETE_LANES];
		for (int l = 0; l < CONCRETE_LANES; l++) {
			recGate[l].process(inputs[REC_INPUT + l].getVoltage(), 0.1f, 1.f);
			armed[l] = params[REC_PARAM + l].getValue() > 0.5f || recGate[l].isHigh();
		}
		const ConcreteBackMessage* back = nullptr;
		if (rightExpander.module && rightExpander.module->model == modelConcrete) {
			auto* m = (ConcreteInMessage*)rightExpander.producerMessage;
			for (int l = 0; l < CONCRETE_LANES; l++) {
				float vl = inputs[L_INPUT + l].getVoltageSum();
				m->l[l] = vl;
				m->r[l] = inputs[R_INPUT + l].isConnected() ? inputs[R_INPUT + l].getVoltageSum() : vl;
				m->rec[l] = armed[l];
				m->on[l] = inputs[L_INPUT + l].isConnected() || inputs[R_INPUT + l].isConnected();
			}
			rightExpander.requestMessageFlip();
			back = (const ConcreteBackMessage*)rightExpander.module->leftExpander.consumerMessage;
		}
		if (lightDiv.process()) {
			// recording: lit; armed but not recording: a slow blink
			blink += args.sampleTime * 64.f;
			if (blink >= 1.f) blink -= 1.f;
			for (int l = 0; l < CONCRETE_LANES; l++) {
				bool rec = back && back->recording[l];
				lights[REC_LIGHT + l].setBrightness(rec ? 1.f : armed[l] ? (blink < 0.5f ? 0.45f : 0.1f) : 0.f);
			}
		}
	}
};

// Panel: 10HP, code-drawn. One row per lane: REC (latch over nothing, its gate
// beside it), then the stereo input.
static const float RI_X_NUM = 3.81f, RI_X_REC = 11.43f, RI_X_GATE = 21.59f, RI_X_L = 31.75f, RI_X_R = 41.91f;
static const float RI_Y0 = 30.48f, RI_PITCH = 25.4f;

struct ConcreteInWidget : ModuleWidget {
	ConcreteInWidget(ConcreteIn* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/concretein.svg")));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("CONCRETE IN");
		lbl->trim(RI_X_REC, RI_Y0, "REC");
		lbl->jack(RI_X_GATE, RI_Y0, "GATE");
		lbl->jack(RI_X_L, RI_Y0, "IN L");
		lbl->jack(RI_X_R, RI_Y0, "IN R");
		for (int l = 0; l < CONCRETE_LANES; l++) {
			float y = RI_Y0 + RI_PITCH * l;
			lbl->add(RI_X_NUM, y + 0.83f, string::f("%d", l + 1));
			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
				mm2px(Vec(RI_X_REC, y)), module, ConcreteIn::REC_PARAM + l, ConcreteIn::REC_LIGHT + l));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(RI_X_GATE, y)), module, ConcreteIn::REC_INPUT + l));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(RI_X_L, y)), module, ConcreteIn::L_INPUT + l));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(RI_X_R, y)), module, ConcreteIn::R_INPUT + l));
			if (l > 0) lbl->divider(2.54f, y - RI_PITCH * 0.5f, 48.26f, y - RI_PITCH * 0.5f);
		}
	}
};

Model* modelConcreteIn = createModel<ConcreteIn, ConcreteInWidget>("ConcreteIn");

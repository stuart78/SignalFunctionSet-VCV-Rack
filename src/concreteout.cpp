#include "plugin.hpp"
#include "panel-style.hpp"
#include "concrete-messages.hpp"

// ─── Concrete Out — each lane's stereo output, as an expander on Concrete's RIGHT ───
// What each lane sends to Concrete's mix: after its VOL, FILTER and PAN curves.
struct ConcreteOut : Module {
	enum ParamId { PARAMS_LEN };
	enum InputId { INPUTS_LEN };
	enum OutputId { ENUMS(L_OUTPUT, CONCRETE_LANES), ENUMS(R_OUTPUT, CONCRETE_LANES), OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	ConcreteOut() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int l = 0; l < CONCRETE_LANES; l++) {
			configOutput(L_OUTPUT + l, string::f("Lane %d left", l + 1));
			configOutput(R_OUTPUT + l, string::f("Lane %d right", l + 1));
		}
	}

	void process(const ProcessArgs& args) override {
		const ConcreteOutMessage* m = nullptr;
		if (leftExpander.module && leftExpander.module->model == modelConcrete)
			m = (const ConcreteOutMessage*)leftExpander.module->rightExpander.consumerMessage;
		for (int l = 0; l < CONCRETE_LANES; l++) {
			outputs[L_OUTPUT + l].setVoltage(m ? m->l[l] : 0.f);
			outputs[R_OUTPUT + l].setVoltage(m ? m->r[l] : 0.f);
		}
	}
};

// Panel: 10HP, code-drawn (narrower put the title under the logo). One row
// per lane, L and R on a plate.
static const float RO_X_NUM = 10.16f, RO_X_L = 20.32f, RO_X_R = 30.48f;
static const float RO_Y0 = 30.48f, RO_PITCH = 25.4f;

struct ConcreteOutWidget : ModuleWidget {
	ConcreteOutWidget(ConcreteOut* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/concreteout.svg")));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("CONCRETE OUT");
		lbl->jackOnPlate(RO_X_L, RO_Y0, "L");
		lbl->jackOnPlate(RO_X_R, RO_Y0, "R");
		for (int l = 0; l < CONCRETE_LANES; l++) {
			float y = RO_Y0 + RO_PITCH * l;
			lbl->add(RO_X_NUM, y + 0.83f, string::f("%d", l + 1));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(RO_X_L, y)), module, ConcreteOut::L_OUTPUT + l));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(RO_X_R, y)), module, ConcreteOut::R_OUTPUT + l));
		}
	}
};

Model* modelConcreteOut = createModel<ConcreteOut, ConcreteOutWidget>("ConcreteOut");

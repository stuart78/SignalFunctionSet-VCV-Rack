#include "plugin.hpp"
#include "polykit-messages.hpp"

// ─── PolyKit In — per-instrument inputs for Kit, as a matrix of jacks ────────
// Sits to the LEFT of a Kit. Thirteen columns by eight rows: every input Kit
// has, for every instrument, as a mono jack. Where a poly cable says
// "channel N is instrument N", this says it with a patch cable per drum.

struct PolyKitIn : Module {
	enum ParamId { PARAMS_LEN };
	enum InputId { ENUMS(IN_INPUT, PK_NCOL * PK_NCH), INPUTS_LEN };   // col * PK_NCH + row
	enum OutputId { OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	static const char* colName(int col) {
		static const char* N[PK_NCOL] = {"trigger", "V/oct", "velocity", "strike X", "strike Y",
		                                 "size CV", "tension CV", "material CV", "air CV",
		                                 "decay CV", "tone CV", "exciter CV", "muffle CV"};
		return N[col];
	}

	PolyKitIn() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int col = 0; col < PK_NCOL; col++)
			for (int ch = 0; ch < PK_NCH; ch++)
				configInput(IN_INPUT + col * PK_NCH + ch,
				            string::f("Instrument %d %s", ch + 1, colName(col)));
		rightExpander.producerMessage = new PolyKitMessage();
		rightExpander.consumerMessage = new PolyKitMessage();
	}
	~PolyKitIn() override {
		delete (PolyKitMessage*)rightExpander.producerMessage;
		delete (PolyKitMessage*)rightExpander.consumerMessage;
	}

	void process(const ProcessArgs& args) override {
		if (!(rightExpander.module && rightExpander.module->model == modelKit)) return;
		auto* m = (PolyKitMessage*)rightExpander.producerMessage;
		for (int col = 0; col < PK_NCOL; col++)
			for (int ch = 0; ch < PK_NCH; ch++) {
				Input& in = inputs[IN_INPUT + col * PK_NCH + ch];
				m->on[col][ch] = in.isConnected();
				m->v[col][ch]  = in.getVoltage();
			}
		rightExpander.requestMessageFlip();
	}
};

// EVERY NUMBER HERE IS READ OUT OF res/polykit-in.svg: the designer's guide
// circles are the jack centres. Thirteen columns on an 11.85mm pitch, eight
// rows on 13.54mm.
static const float PKI_COLX[PK_NCOL] = {15.19f, 27.05f, 38.90f, 50.75f, 62.60f, 74.45f, 86.30f,
                                    98.15f, 110.00f, 121.85f, 133.71f, 145.56f, 157.41f};
static const float PKI_ROWY[PK_NCH]  = {20.36f, 33.90f, 47.45f, 60.99f, 74.54f, 88.08f, 101.62f, 115.17f};

struct PolyKitInWidget : ModuleWidget {
	PolyKitInWidget(PolyKitIn* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/polykit-in.svg")));
		// NO PanelLabels: the art carries its own outlined text, which Rack
		// renders, and the runtime labels would print every one of them twice.
		for (int col = 0; col < PK_NCOL; col++)
			for (int ch = 0; ch < PK_NCH; ch++)
				addInput(createInputCentered<PJ301MPort>(mm2px(Vec(PKI_COLX[col], PKI_ROWY[ch])),
				                                         module, PolyKitIn::IN_INPUT + col * PK_NCH + ch));
	}
};

Model* modelPolyKitIn = createModel<PolyKitIn, PolyKitInWidget>("PolyKitIn");

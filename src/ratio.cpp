#include "plugin.hpp"
#include <cmath>

// ─── Ratio — quad ratio VCA ──────────────────────────────────────────────────
// One signal input and one CV input feed four VCA outputs. Each output scales the
// CV by its own bipolar Ratio (−1…+1) to form a gain, applies that gain to the
// signal, and has per-output slew and mute (button + gate). CV is normalled to
// +10 V when unpatched, so with no CV the Ratio knobs act as a quad attenuverter.

static const int RN = 4;   // outputs

struct Ratio : Module {
	enum ParamId {
		ENUMS(RATIO_PARAM, RN),
		ENUMS(SLEW_PARAM, RN),
		ENUMS(MUTE_PARAM, RN),
		PARAMS_LEN
	};
	enum InputId {
		CHANNEL_INPUT,
		CV_INPUT,
		ENUMS(RATIOCV_INPUT, RN),
		ENUMS(MUTECV_INPUT, RN),
		INPUTS_LEN
	};
	enum OutputId { ENUMS(OUT_OUTPUT, RN), OUTPUTS_LEN };
	enum LightId { ENUMS(MUTE_LIGHT, RN), LIGHTS_LEN };

	dsp::SchmittTrigger muteTrig[RN];
	bool  muted[RN] = {};
	float gain[RN] = {};          // slewed gain per output

	// display mirrors
	float dispGain[RN] = {}, dispRatio[RN] = {};
	bool  dispMute[RN] = {};

	// rolling scope of each output (min/max peak per column, ~1.5 s window)
	static const int SCOPE_W = 130;
	float scMin[RN][SCOPE_W] = {}, scMax[RN][SCOPE_W] = {};
	int   scPos = 0, scAcc = 0;
	float binMin[RN], binMax[RN];
	int   selectedScope = 0;      // which output's signal is highlighted (0..3)

	Ratio() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configInput(CHANNEL_INPUT, "Signal");
		configInput(CV_INPUT, "CV (normalled to +10V)");
		for (int i = 0; i < RN; i++) {
			configParam(RATIO_PARAM + i, -1.f, 1.f, 1.f, string::f("Out %d ratio", i + 1));
			configParam(SLEW_PARAM + i, 0.f, 1.f, 0.f, string::f("Out %d slew", i + 1));
			configButton(MUTE_PARAM + i, string::f("Out %d mute", i + 1));
			configInput(RATIOCV_INPUT + i, string::f("Out %d ratio CV", i + 1));
			configInput(MUTECV_INPUT + i, string::f("Out %d mute gate", i + 1));
			configOutput(OUT_OUTPUT + i, string::f("Out %d", i + 1));
			binMin[i] = 1e30f; binMax[i] = -1e30f;
		}
	}

	void process(const ProcessArgs& args) override {
		float sig = inputs[CHANNEL_INPUT].getVoltage();
		float cv = inputs[CV_INPUT].isConnected() ? inputs[CV_INPUT].getVoltage() : 10.f;

		for (int i = 0; i < RN; i++) {
			if (muteTrig[i].process(params[MUTE_PARAM + i].getValue())) muted[i] = !muted[i];
			bool jackMute = inputs[MUTECV_INPUT + i].isConnected() && inputs[MUTECV_INPUT + i].getVoltage() >= 1.f;
			bool isMuted = muted[i] || jackMute;

			float ratio = params[RATIO_PARAM + i].getValue();
			if (inputs[RATIOCV_INPUT + i].isConnected())
				ratio += inputs[RATIOCV_INPUT + i].getVoltage() / 5.f;   // ±5V → ±1
			ratio = clamp(ratio, -1.f, 1.f);

			float pot = (cv * 0.1f) * ratio;                            // potential gain −1…+1
			float target = isMuted ? 0.f : pot;

			// one-pole slew: 0.8 ms floor (anti-click) up to ~0.6 s
			float s = params[SLEW_PARAM + i].getValue();
			float t = 0.0008f + s * s * 0.6f;
			gain[i] += (target - gain[i]) * (1.f - std::exp(-args.sampleTime / t));

			outputs[OUT_OUTPUT + i].setVoltage(sig * gain[i]);
			lights[MUTE_LIGHT + i].setBrightness(isMuted ? 1.f : 0.f);

			// display shows the live gain, but a muted track shows its would-be
			// level (darkened by the widget) instead of collapsing to zero
			float shown = isMuted ? pot : gain[i];
			dispGain[i] = shown; dispRatio[i] = ratio; dispMute[i] = isMuted;
			float mon = sig * shown;
			binMin[i] = std::min(binMin[i], mon); binMax[i] = std::max(binMax[i], mon);
		}

		// rolling scope: peak-track each output into one column per ~1.5s/SCOPE_W
		int decim = std::max(1, (int)(args.sampleRate * 1.5f / SCOPE_W));
		if (++scAcc >= decim) {
			for (int i = 0; i < RN; i++) {
				bool empty = binMax[i] < binMin[i];
				scMin[i][scPos] = empty ? 0.f : binMin[i];
				scMax[i][scPos] = empty ? 0.f : binMax[i];
				binMin[i] = 1e30f; binMax[i] = -1e30f;
			}
			scPos = (scPos + 1) % SCOPE_W; scAcc = 0;
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* m = json_array();
		for (int i = 0; i < RN; i++) json_array_append_new(m, json_boolean(muted[i]));
		json_object_set_new(root, "muted", m);
		json_object_set_new(root, "selectedScope", json_integer(selectedScope));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* m = json_object_get(root, "muted"))
			for (int i = 0; i < RN; i++) if (json_t* v = json_array_get(m, i)) muted[i] = json_boolean_value(v);
		if (json_t* j = json_object_get(root, "selectedScope")) selectedScope = clamp((int)json_integer_value(j), 0, RN - 1);
	}
};

// ─── Display ──────────────────────────────────────────────────────────────────
// Two halves. Left: four vertical bipolar gain meters + a +1/0/−1 scale. Right: a
// rolling layered scope of every output's amplitude (dark grey = the envelope);
// click 1–4 to highlight one output's signal. Muted outputs still show, darkened.
struct RatioDisplay : OpaqueWidget {
	Ratio* module = nullptr;
	std::shared_ptr<Font> font;

	float splitX() const { return std::round(box.size.x * 0.40f); }
	Rect toggleRect(int i) const {
		float x = splitX() + 4.f, tw = 12.f, th = 10.f, gap = 3.f;
		return Rect(Vec(x + i * (tw + gap), 2.f), Vec(tw, th));
	}

	void drawBars(NVGcontext* vg, const float* g, const bool* mu, float w, float h) {
		if (font) {   // +1 / 0 / −1 scale
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 7.f);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, nvgRGB(0x6a, 0x6a, 0x88));
			nvgText(vg, 14.f, 8.f, "+1", NULL);
			nvgText(vg, 14.f, h * 0.5f, "0", NULL);
			nvgText(vg, 14.f, h - 9.f, "-1", NULL);
		}
		float bx0 = 18.f, avail = w - bx0 - 3.f, slot = avail / RN;
		float top = 5.f, bot = h - 11.f, mid = (top + bot) * 0.5f, half = (bot - top) * 0.5f;
		nvgBeginPath(vg); nvgRect(vg, bx0, mid - 0.5f, avail, 1.f);
		nvgFillColor(vg, nvgRGB(0x3a, 0x3a, 0x50)); nvgFill(vg);
		for (int i = 0; i < RN; i++) {
			float cx = bx0 + slot * (i + 0.5f), bw = std::min(slot * 0.62f, 12.f);
			nvgBeginPath(vg); nvgRoundedRect(vg, cx - bw * 0.5f, top, bw, bot - top, 2.f);
			nvgFillColor(vg, nvgRGB(0x24, 0x24, 0x36)); nvgFill(vg);
			float gg = clamp(g[i], -1.f, 1.f);
			if (std::fabs(gg) > 0.001f) {
				float bh = std::fabs(gg) * half, yy = gg >= 0 ? mid - bh : mid;
				NVGcolor pos = mu[i] ? nvgRGB(0x1c, 0x3c, 0x52) : nvgRGB(0x00, 0x97, 0xde);
				NVGcolor neg = mu[i] ? nvgRGB(0x5a, 0x30, 0x1c) : nvgRGB(0xec, 0x65, 0x2e);
				nvgBeginPath(vg); nvgRoundedRect(vg, cx - bw * 0.5f, yy, bw, bh, 1.5f);
				nvgFillColor(vg, gg >= 0 ? pos : neg); nvgFill(vg);
			}
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 7.f);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, mu[i] ? nvgRGB(0x55, 0x55, 0x6a) : nvgRGB(0xb0, 0xb0, 0xcc));
				nvgText(vg, cx, bot + 5.f, string::f("%d", i + 1).c_str(), NULL);
			}
		}
	}

	void drawScope(NVGcontext* vg, const float* const* mn, const float* const* mx,
	               int scPos, const bool* mu, int sel, float x0, float w, float h) {
		// toggles 1..4
		for (int i = 0; i < RN; i++) {
			Rect t = toggleRect(i); bool active = (i == sel);
			nvgBeginPath(vg); nvgRoundedRect(vg, t.pos.x, t.pos.y, t.size.x, t.size.y, 2.f);
			nvgFillColor(vg, active ? nvgRGB(0x0d, 0x59, 0x86) : nvgRGB(0x2a, 0x2a, 0x3e)); nvgFill(vg);
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 8.f);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, active ? nvgRGB(0xe6, 0xe6, 0xf0) : nvgRGB(0x6a, 0x6a, 0x88));
				nvgText(vg, t.pos.x + t.size.x * 0.5f, t.pos.y + t.size.y * 0.5f + 0.5f, string::f("%d", i + 1).c_str(), NULL);
			}
		}
		float syT = 16.f, syB = h - 3.f, syc = (syT + syB) * 0.5f, half = (syB - syT) * 0.5f;
		float sxL = x0 + 2.f, sxR = x0 + w - 3.f, sw = sxR - sxL;
		nvgBeginPath(vg); nvgRect(vg, sxL, syc - 0.5f, sw, 1.f);
		nvgFillColor(vg, nvgRGB(0x3a, 0x3a, 0x50)); nvgFill(vg);
		const int W = Ratio::SCOPE_W;
		// non-selected first (grey envelopes), then the selected output on top
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < RN; i++) {
				bool isSel = (i == sel);
				if ((pass == 0) == isSel) continue;
				NVGcolor c = isSel ? (mu[i] ? nvgRGBA(0x1c, 0x5a, 0x82, 0xff) : nvgRGBA(0x00, 0x97, 0xde, 0xff))
				                   : nvgRGBA(0x60, 0x60, 0x74, mu[i] ? 0x38 : 0x78);
				nvgBeginPath(vg);
				for (int k = 0; k < W; k++) {
					int idx = (scPos + k) % W;
					float xx = sxL + (W > 1 ? (float)k / (W - 1) : 0.f) * sw;
					float yy = syc - clamp(mx[i][idx] * 0.1f, -1.f, 1.f) * half;
					if (k == 0) nvgMoveTo(vg, xx, yy); else nvgLineTo(vg, xx, yy);
				}
				for (int k = W - 1; k >= 0; k--) {
					int idx = (scPos + k) % W;
					float xx = sxL + (W > 1 ? (float)k / (W - 1) : 0.f) * sw;
					float yy = syc - clamp(mn[i][idx] * 0.1f, -1.f, 1.f) * half;
					nvgLineTo(vg, xx, yy);
				}
				nvgFillColor(vg, c); nvgFill(vg);
			}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		NVGcontext* vg = args.vg; const float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 3.f);
		nvgFillColor(vg, nvgRGB(0x1a, 0x1a, 0x2e)); nvgFill(vg);

		float g[RN]; bool mu[RN]; int sel = 0, scPos = 0;
		const int W = Ratio::SCOPE_W;
		const float* mn[RN]; const float* mx[RN];
		static float demoMin[RN][Ratio::SCOPE_W], demoMax[RN][Ratio::SCOPE_W];
		if (module) {
			for (int i = 0; i < RN; i++) { g[i] = module->dispGain[i]; mu[i] = module->dispMute[i]; mn[i] = module->scMin[i]; mx[i] = module->scMax[i]; }
			sel = module->selectedScope; scPos = module->scPos;
		} else {
			float dg[RN] = {0.8f, -0.5f, 0.35f, 0.f};
			for (int i = 0; i < RN; i++) {
				g[i] = dg[i]; mu[i] = (i == 3);
				for (int k = 0; k < W; k++) {
					float env = std::fabs(std::sin(k * 0.09f + i * 1.7f)) * (0.85f - 0.12f * i);
					demoMax[i][k] = env * 10.f; demoMin[i][k] = -env * 8.f;
				}
				mn[i] = demoMin[i]; mx[i] = demoMax[i];
			}
			sel = 0;
		}

		float sx = splitX();
		drawBars(vg, g, mu, sx, h);
		nvgBeginPath(vg); nvgRect(vg, sx - 0.5f, 4.f, 1.f, h - 8.f);
		nvgFillColor(vg, nvgRGB(0x34, 0x34, 0x48)); nvgFill(vg);
		drawScope(vg, mn, mx, scPos, mu, sel, sx + 2.f, w - sx - 2.f, h);
	}

	void onButton(const ButtonEvent& e) override {
		if (module && e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
			for (int i = 0; i < RN; i++)
				if (toggleRect(i).contains(e.pos)) { module->selectedScope = i; e.consume(this); return; }
		}
		OpaqueWidget::onButton(e);
	}
};

// ─── Widget ───────────────────────────────────────────────────────────────────
struct RatioWidget : ModuleWidget {
	RatioWidget(Ratio* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ratio.svg")));
		// No virtual screws — see CLAUDE.md.

		RatioDisplay* disp = new RatioDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(5.f, 12.f));
		disp->box.size = mm2px(Vec(91.6f, 28.f));
		addChild(disp);

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.f, 48.f)), module, Ratio::CHANNEL_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(24.f, 48.f)), module, Ratio::CV_INPUT));

		float rowY[RN] = {62.f, 78.f, 94.f, 110.f};
		for (int i = 0; i < RN; i++) {
			float y = rowY[i];
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.f, y)), module, Ratio::RATIO_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.f, y)), module, Ratio::RATIOCV_INPUT + i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(37.f, y)), module, Ratio::SLEW_PARAM + i));
			addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedLight>>>(
				mm2px(Vec(48.f, y)), module, Ratio::MUTE_PARAM + i, Ratio::MUTE_LIGHT + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(59.f, y)), module, Ratio::MUTECV_INPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(89.f, y)), module, Ratio::OUT_OUTPUT + i));
		}
	}
};

Model* modelRatio = createModel<Ratio, RatioWidget>("Ratio");

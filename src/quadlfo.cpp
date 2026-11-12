#include "plugin.hpp"
#include "fastmath.hpp"
#include "panel-style.hpp"


struct Drift : Module {
	sfs::Memo mFreq;   // the frequency knob's pow (fastmath.hpp)
	enum ParamId {
		PARAMSHAPE_PARAM,
		PARAMSTABILITY_PARAM,
		PARAMFREQUENCY_PARAM,
		PARAMXSPREAD_PARAM,
		PARAMCENTER_PARAM,
		PARAMYSPREAD_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		INSHAPE_INPUT,
		INSTABILITY_INPUT,
		INFREQUENCY_INPUT,
		INSPREAD_INPUT,
		INCENTER_INPUT,
		INYSPREAD_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTMIN_OUTPUT,
		OUTMAX_OUTPUT,
		OUTA_OUTPUT,
		OUTB_OUTPUT,
		OUTC_OUTPUT,
		OUTD_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	float phases[4] = {0.f, 0.f, 0.f, 0.f};
	float baseFreq = 1.0f;
	dsp::SchmittTrigger clockTrigger;
	float clockFreq = 1.0f;
	int clockSampleCount = 0;
	int lastClockSample = 0;
	bool clockConnected = false;
	
	// Lorenz attractor state for each output (x, y, z coordinates)
	struct LorenzState {
		float x, y, z;
		LorenzState() : x(1.f), y(1.f), z(1.f) {}
	};
	LorenzState lorenz[4];
	
	// Lorenz parameters - classic values with slight variations per output
	float lorenzSigma[4] = {10.0f, 10.2f, 9.8f, 10.1f};
	float lorenzRho[4] = {28.0f, 28.3f, 27.7f, 28.1f};
	float lorenzBeta[4] = {2.667f, 2.7f, 2.6f, 2.65f};
	
	// Brownian motion state for smooth random waveform
	float brownianValue[4] = {0.f, 0.f, 0.f, 0.f};
	float brownianTarget[4] = {0.f, 0.f, 0.f, 0.f};
	float lastBrownianPhase[4] = {0.f, 0.f, 0.f, 0.f};

	// what the panel's scope draws: the controls after their CVs
	float dShape = 0.5f, dCenter = 0.f, dYSpread = 5.f, dXSpread = 0.f;

	Drift() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PARAMSHAPE_PARAM, 0.f, 1.f, 0.5f, "Shape", "", 0.f, 1.f);
		configParam(PARAMSTABILITY_PARAM, 0.f, 1.f, 0.5f, "Stability", "", 0.f, 1.f);
		configParam(PARAMFREQUENCY_PARAM, -4.9f, 3.32f, 0.f, "Frequency", " Hz", 2.f, 1.f);
		configParam(PARAMXSPREAD_PARAM, 0.f, 1.f, 0.f, "X Spread", "", 0.f, 1.f);
		configParam(PARAMCENTER_PARAM, -5.f, 5.f, 0.f, "Center", " V");
		configParam(PARAMYSPREAD_PARAM, 0.f, 5.f, 5.f, "Y Spread", " V");
		
		configInput(INSHAPE_INPUT, "Shape CV");
		configInput(INSTABILITY_INPUT, "Stability CV");
		configInput(INFREQUENCY_INPUT, "Clock");
		configInput(INSPREAD_INPUT, "X Spread CV");
		configInput(INCENTER_INPUT, "Center CV");
		configInput(INYSPREAD_INPUT, "Y Spread CV");
		
		configOutput(OUTMIN_OUTPUT, "Min");
		configOutput(OUTMAX_OUTPUT, "Max");
		configOutput(OUTA_OUTPUT, "Output A");
		configOutput(OUTB_OUTPUT, "Output B");
		configOutput(OUTC_OUTPUT, "Output C");
		configOutput(OUTD_OUTPUT, "Output D");
	}

	void updateBrownianMotion(int outputIndex, float phase, float sampleTime) {
		// Update Brownian motion when phase wraps (once per cycle)
		if (phase < lastBrownianPhase[outputIndex]) {
			// Phase wrapped, take a small Brownian step toward a new target
			float step = 2.f * (random::uniform() - 0.5f) * 0.1f; // Much smaller steps ±0.1
			brownianTarget[outputIndex] += step;
			
			// Keep target bounded with gentle pull toward center
			if (std::abs(brownianTarget[outputIndex]) > 0.8f) {
				brownianTarget[outputIndex] *= 0.9f; // Gentle pull back toward center
			}
			brownianTarget[outputIndex] = clamp(brownianTarget[outputIndex], -1.2f, 1.2f);
		}
		
		// Smooth interpolation toward target (creates inertia and smoothness)
		float smoothing = 0.02f; // Very slow movement for smoothness
		brownianValue[outputIndex] += (brownianTarget[outputIndex] - brownianValue[outputIndex]) * smoothing;
		
		lastBrownianPhase[outputIndex] = phase;
	}

	float generateWave(float phase, float shape, int outputIndex, float sampleTime) {
		// Generate base waveforms - all bipolar ±1, perfectly phase-aligned
		// All waveforms start at 0 when phase=0
		
		float sine = SFS_SIN2PI(phase);   // was double sin, twice (2026-09-25): -108 dB
		
		// Triangle wave: starts at 0, goes to +1 at 0.25, back to 0 at 0.5, to -1 at 0.75, back to 0 at 1.0
		float triangle;
		if (phase < 0.25f) {
			triangle = 4.f * phase; // 0 to +1
		} else if (phase < 0.75f) {
			triangle = 2.f - 4.f * phase; // +1 to -1
		} else {
			triangle = 4.f * phase - 4.f; // -1 to 0
		}
		
		// Sawtooth wave: starts at 0, ramps linearly to +1 at 0.5, jumps to -1, ramps to 0 at 1.0
		float sawtooth;
		if (phase < 0.5f) {
			sawtooth = 2.f * phase; // 0 to +1
		} else {
			sawtooth = 2.f * phase - 2.f; // -1 to 0 (after jump from +1 to -1)
		}
		
		// Square wave: special handling to start at 0 and maintain symmetry
		// Use a sine-based approach for smooth transitions that starts at 0
		float square = (sine >= 0.f) ? 1.f : -1.f;
		// Override the exact phase=0 case to ensure it starts at 0
		if (phase < 0.001f || phase > 0.999f) {
			square = 0.f;
		}
		
		// Brownian motion chaos
		updateBrownianMotion(outputIndex, phase, sampleTime);
		float chaos = brownianValue[outputIndex];
		
		// Morphing between waveforms using smooth interpolation
		if (shape <= 0.25f) {
			// Sine to Triangle (0.0 to 0.25)
			float mix = shape * 4.f;
			return sine * (1.f - mix) + triangle * mix;
		}
		else if (shape <= 0.5f) {
			// Triangle to Sawtooth (0.25 to 0.5)
			float mix = (shape - 0.25f) * 4.f;
			return triangle * (1.f - mix) + sawtooth * mix;
		}
		else if (shape <= 0.75f) {
			// Sawtooth to Square (0.5 to 0.75)
			float mix = (shape - 0.5f) * 4.f;
			return sawtooth * (1.f - mix) + square * mix;
		}
		else {
			// Square to Chaos (0.75 to 1.0)
			float mix = (shape - 0.75f) * 4.f;
			return square * (1.f - mix) + chaos * mix;
		}
	}

	void process(const ProcessArgs& args) override {
		// Get parameters with CV
		float shape = params[PARAMSHAPE_PARAM].getValue();
		if (inputs[INSHAPE_INPUT].isConnected())
			shape += inputs[INSHAPE_INPUT].getVoltage() / 10.f;
		shape = clamp(shape, 0.f, 1.f);

		float stability = params[PARAMSTABILITY_PARAM].getValue();
		if (inputs[INSTABILITY_INPUT].isConnected())
			stability += inputs[INSTABILITY_INPUT].getVoltage() / 10.f;
		stability = clamp(stability, 0.f, 1.f);

		float freq = params[PARAMFREQUENCY_PARAM].getValue();
		float actualFreq = mFreq(freq, [](float f) { return std::pow(2.f, f); });
		
		// Handle clock input
		clockConnected = inputs[INFREQUENCY_INPUT].isConnected();
		if (clockConnected) {
			if (clockTrigger.process(inputs[INFREQUENCY_INPUT].getVoltage())) {
				// Rising edge detected - reset phase A and calculate frequency from clock
				if (lastClockSample > 0) {
					int samplesBetweenClocks = clockSampleCount - lastClockSample;
					if (samplesBetweenClocks > 0) {
						float clockPeriod = samplesBetweenClocks * args.sampleTime;
						clockFreq = 1.f / clockPeriod;
						// Clamp to reasonable range
						clockFreq = clamp(clockFreq, 0.1f, 100.f);
					}
				}
				lastClockSample = clockSampleCount;
				phases[0] = 0.f; // Reset output A phase on each clock
			}
			clockSampleCount++;
			actualFreq = clockFreq;
		}

		float xSpread = params[PARAMXSPREAD_PARAM].getValue();
		if (inputs[INSPREAD_INPUT].isConnected())
			xSpread += inputs[INSPREAD_INPUT].getVoltage() / 10.f;
		xSpread = clamp(xSpread, 0.f, 1.f);

		float center = params[PARAMCENTER_PARAM].getValue();
		if (inputs[INCENTER_INPUT].isConnected())
			center += inputs[INCENTER_INPUT].getVoltage();
		center = clamp(center, -5.f, 5.f);

		float ySpread = params[PARAMYSPREAD_PARAM].getValue();
		if (inputs[INYSPREAD_INPUT].isConnected())
			ySpread += inputs[INYSPREAD_INPUT].getVoltage();
		ySpread = clamp(ySpread, 0.f, 5.f);

		// Calculate phase spreads for the 4 outputs
		dShape = shape; dCenter = center; dYSpread = ySpread; dXSpread = xSpread;

		float phaseOffsets[4] = {0.f, 0.25f, 0.5f, 0.75f};
		
		// Apply x spread to phase offsets
		for (int i = 0; i < 4; i++) {
			phaseOffsets[i] *= xSpread;
		}

		// Update master phase only
		float deltaPhase = actualFreq * args.sampleTime;
		phases[0] += deltaPhase;
		if (phases[0] >= 1.f)
			phases[0] -= 1.f;

		// Update Lorenz attractors for each output
		float lorenzDt = args.sampleTime * (1.f - stability) * 2.0f; // Scale with instability only, not frequency
		for (int i = 0; i < 4; i++) {
			if (stability < 1.f) { // Only update when instability is present
				// Lorenz equations: dx/dt = σ(y-x), dy/dt = x(ρ-z)-y, dz/dt = xy-βz
				float dx = lorenzSigma[i] * (lorenz[i].y - lorenz[i].x);
				float dy = lorenz[i].x * (lorenzRho[i] - lorenz[i].z) - lorenz[i].y;
				float dz = lorenz[i].x * lorenz[i].y - lorenzBeta[i] * lorenz[i].z;
				
				// Integrate using Euler method
				lorenz[i].x += dx * lorenzDt;
				lorenz[i].y += dy * lorenzDt;
				lorenz[i].z += dz * lorenzDt;

				// Safety: if the Euler step ever diverges (huge dt / denormals),
				// reset this attractor to its seed instead of spewing NaNs.
				if (!std::isfinite(lorenz[i].x) || !std::isfinite(lorenz[i].y) ||
				    !std::isfinite(lorenz[i].z) ||
				    std::abs(lorenz[i].x) > 1e4f || std::abs(lorenz[i].y) > 1e4f ||
				    std::abs(lorenz[i].z) > 1e4f) {
					lorenz[i] = LorenzState();
				}
			}
		}

		// Generate outputs
		float outputs[4];
		float minVal = 10.f, maxVal = -10.f;

		for (int i = 0; i < 4; i++) {
			float adjustedPhase = phases[0] + phaseOffsets[i];
			if (adjustedPhase >= 1.f)
				adjustedPhase -= 1.f;

			// Work out the (optionally Lorenz-modulated) phase FIRST, then render
			// the wave exactly once. generateWave() advances the Brownian/chaos
			// state, so it must be called a single time per sample per output
			// against one consistent phase — calling it twice (as before) made the
			// chaos waveform re-roll almost every sample under instability.
			float finalPhase = adjustedPhase;
			float ampMod = 1.f;
			float harmonicContent = 0.f;
			if (stability < 1.f) {
				float instabilityAmount = 1.f - stability;

				// Normalize Lorenz coordinates to usable ranges
				// X and Y typically range ±20, Z ranges 0-50
				float lorenzX = clamp(lorenz[i].x / 20.f, -1.f, 1.f);   // Phase modulation
				float lorenzY = clamp(lorenz[i].y / 20.f, -1.f, 1.f);   // Amplitude modulation
				float lorenzZ = clamp((lorenz[i].z - 25.f) / 25.f, -1.f, 1.f); // Frequency modulation

				// Phase modulation - creates "drift" in timing
				float phaseOffset = lorenzX * instabilityAmount * 0.1f;
				float driftedPhase = adjustedPhase + phaseOffset;
				while (driftedPhase < 0.f) driftedPhase += 1.f;
				while (driftedPhase >= 1.f) driftedPhase -= 1.f;

				// Amplitude modulation - creates "breathing" effect
				ampMod = clamp(1.f + (lorenzY * instabilityAmount * 0.3f), 0.3f, 1.7f);

				// Frequency modulation - creates subtle tempo variations
				float freqMod = 1.f + (lorenzZ * instabilityAmount * 0.05f);
				finalPhase = driftedPhase * freqMod;
				while (finalPhase < 0.f) finalPhase += 1.f;
				while (finalPhase >= 1.f) finalPhase -= 1.f;

				// Subtle harmonic content based on Lorenz Z
				harmonicContent = SFS_SIN2PI(finalPhase * 1.5f) * lorenzZ * instabilityAmount * 0.1f;
			}

			float wave = generateWave(finalPhase, shape, i, args.sampleTime) * ampMod + harmonicContent;

			// Scale and offset
			outputs[i] = center + wave * ySpread;
			
			// Track min/max
			minVal = std::min(minVal, outputs[i]);
			maxVal = std::max(maxVal, outputs[i]);
		}

		// Set outputs
		this->outputs[OUTA_OUTPUT].setVoltage(outputs[0]);
		this->outputs[OUTB_OUTPUT].setVoltage(outputs[1]);
		this->outputs[OUTC_OUTPUT].setVoltage(outputs[2]);
		this->outputs[OUTD_OUTPUT].setVoltage(outputs[3]);
		this->outputs[OUTMIN_OUTPUT].setVoltage(minVal);
		this->outputs[OUTMAX_OUTPUT].setVoltage(maxVal);
	}
};


// The base waveform, as generateWave() makes it before the instability acts on
// it. The random walk at the top of SHAPE is drawn from a fixed table, so the
// drawing holds still.
static float driftBaseWave(float phase, float shape) {
	float sine = std::sin(2.f * (float)M_PI * phase);
	float tri = phase < 0.25f ? 4.f * phase : phase < 0.75f ? 2.f - 4.f * phase : 4.f * phase - 4.f;
	float saw = phase < 0.5f ? 2.f * phase : 2.f * phase - 2.f;
	float sq = sine >= 0.f ? 1.f : -1.f;
	static const float WALK[9] = {0.1f, 0.45f, 0.3f, -0.2f, -0.5f, -0.15f, 0.35f, 0.6f, 0.1f};
	float wp = phase * 8.f;
	int wi = std::min((int)wp, 7);
	float walk = WALK[wi] + (WALK[wi + 1] - WALK[wi]) * (wp - (float)wi);
	if (shape <= 0.25f) { float m = shape * 4.f; return sine + (tri - sine) * m; }
	if (shape <= 0.5f) { float m = (shape - 0.25f) * 4.f; return tri + (saw - tri) * m; }
	if (shape <= 0.75f) { float m = (shape - 0.5f) * 4.f; return saw + (sq - saw) * m; }
	float m = (shape - 0.75f) * 4.f; return sq + (walk - sq) * m;
}

// Drawn on the faceplate, under the controls: the five shapes round the SHAPE
// knob at the positions they occupy on its travel, and a small scope between
// CENTER and Y SPRD showing the waveform SHAPE gives, the range CENTER and
// Y SPRD give it, and where A-D are in it now.
struct DriftArt : Widget {
	Drift* module = nullptr;

	// one glyph, centred at (cx, cy), w x h in px
	static void glyph(NVGcontext* vg, float cx, float cy, float w, float h, int kind) {
		nvgBeginPath(vg);
		const int N = 24;
		for (int i = 0; i <= N; i++) {
			float t = (float)i / N;
			float v;
			switch (kind) {
				case 0: v = std::sin(2.f * (float)M_PI * t); break;                        // sine
				case 1: v = t < 0.25f ? 4.f * t : t < 0.75f ? 2.f - 4.f * t : 4.f * t - 4.f; break;  // triangle
				default: v = 0.f;
			}
			if (kind <= 1) {
				float x = cx - w / 2 + w * t, y = cy - v * h / 2;
				if (i == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
			}
		}
		float x0 = cx - w / 2, x1 = cx + w / 2, y0 = cy - h / 2, y1 = cy + h / 2;
		if (kind == 2) {                                   // saw: up, drop, up
			nvgMoveTo(vg, x0, cy); nvgLineTo(vg, cx, y0); nvgLineTo(vg, cx, y1); nvgLineTo(vg, x1, cy);
		}
		else if (kind == 3) {                              // square
			nvgMoveTo(vg, x0, y1); nvgLineTo(vg, x0, y0); nvgLineTo(vg, cx, y0);
			nvgLineTo(vg, cx, y1); nvgLineTo(vg, x1, y1); nvgLineTo(vg, x1, y0);
		}
		else if (kind == 4) {                              // a random walk
			static const float W[7] = {0.1f, 0.7f, 0.3f, -0.4f, -0.8f, 0.2f, 0.6f};
			for (int i = 0; i < 7; i++) {
				float x = x0 + w * i / 6.f, y = cy - W[i] * h / 2;
				if (i == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
			}
		}
		nvgStroke(vg);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const sfs::Theme& T = sfs::theme();
		nvgLineCap(vg, NVG_ROUND); nvgLineJoin(vg, NVG_ROUND);

		// ── SHAPE glyphs ── a row under the knob, sine at the left end of its
		// travel to the random walk at the right, as the 0% and 100% notes sit
		// under STABIL. Round the knob at their true angles the saw would sit
		// on top, where the label and the title leave no room. The row bends
		// up at its ends, following the knob's lower edge.
		nvgStrokeColor(vg, T.rule);
		nvgStrokeWidth(vg, mm2px(0.18f));
		const float kx = 12.7f;
		for (int k = 0; k < 5; k++) {
			float dx = 3.2f * (float)(k - 2);
			glyph(vg, mm2px(kx + dx), mm2px(27.6f - 0.025f * dx * dx),
			      mm2px(2.2f), mm2px(1.2f), k);
		}

		// ── the scope ──
		const float x0 = 19.2f, x1 = 31.6f, yt = 35.6f, yb = 45.6f;
		auto vy = [&](float v) { return mm2px(yt + (yb - yt) * (0.5f - v / 20.f)); };   // ±10 V
		const float sx0 = x0 + 1.4f, sx1 = x1 - 1.6f;                                   // the plot
		// scale ticks at -10, -5, 0, 5, 10 V
		nvgStrokeColor(vg, T.rule);
		nvgStrokeWidth(vg, mm2px(0.15f));
		for (int i = -2; i <= 2; i++) {
			float y = vy(5.f * (float)i);
			nvgBeginPath(vg);
			nvgMoveTo(vg, mm2px(x0), y); nvgLineTo(vg, mm2px(x0 + (i == 0 ? 0.9f : 0.6f)), y);
			nvgStroke(vg);
		}
		float shape = 0.5f, center = 0.f, ysp = 5.f, xsp = 0.f, ph = 0.f;
		if (module) {
			shape = module->dShape; center = module->dCenter; ysp = module->dYSpread;
			xsp = module->dXSpread; ph = module->phases[0];
		}
		// the range: a bracket from CENTER - Y SPRD to CENTER + Y SPRD
		float lo = vy(clamp(center - ysp, -10.f, 10.f)), hi = vy(clamp(center + ysp, -10.f, 10.f));
		float bx = mm2px(x1);
		nvgBeginPath(vg);
		nvgMoveTo(vg, bx - mm2px(0.6f), hi); nvgLineTo(vg, bx, hi);
		nvgLineTo(vg, bx, lo); nvgLineTo(vg, bx - mm2px(0.6f), lo);
		nvgStrokeColor(vg, T.inkSoft);
		nvgStrokeWidth(vg, mm2px(0.18f));
		nvgStroke(vg);
		// one cycle of the waveform, in that range
		nvgBeginPath(vg);
		const int N = 72;
		for (int i = 0; i <= N; i++) {
			float p = (float)i / N;
			float x = mm2px(sx0 + (sx1 - sx0) * p);
			float y = vy(clamp(center + driftBaseWave(std::min(p, 0.9999f), shape) * ysp, -10.f, 10.f));
			if (i == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
		}
		nvgStrokeColor(vg, T.ink);
		nvgStrokeWidth(vg, mm2px(0.22f));
		nvgStroke(vg);
		// where A-D are now: A in orange, the rest in ink
		for (int i = 3; i >= 0; i--) {
			float p = ph + 0.25f * (float)i * xsp;
			p -= std::floor(p);
			float x = mm2px(sx0 + (sx1 - sx0) * p);
			float y = vy(clamp(center + driftBaseWave(p, shape) * ysp, -10.f, 10.f));
			nvgBeginPath(vg);
			nvgCircle(vg, x, y, mm2px(i == 0 ? 0.45f : 0.35f));
			nvgFillColor(vg, i == 0 ? sfs::SCREEN_HOT : T.ink);
			nvgFill(vg);
		}
	}
};

struct DriftWidget : ModuleWidget {
	DriftWidget(Drift* module) {
		setModule(module);
		// Redrawn 2026-11 in the code-drawn house style, so it follows "Use dark
		// panels if available". Every control is where it was, so patches and
		// cables are unaffected; the art keeps only the diagrams beside the knobs.
		setPanel(sfs::themedPanel("drift"));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("DRIFT");
		DriftArt* art = new DriftArt();
		art->module = module;
		art->box.size = box.size;
		addChild(art);

		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(38.1, 20.32)), module, Drift::PARAMSTABILITY_PARAM));
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(12.7, 20.32)), module, Drift::PARAMSHAPE_PARAM));
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(38.1, 60.96)), module, Drift::PARAMXSPREAD_PARAM));
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(12.7, 40.64)), module, Drift::PARAMCENTER_PARAM));
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(12.7, 60.96)), module, Drift::PARAMFREQUENCY_PARAM));
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(38.1, 40.64)), module, Drift::PARAMYSPREAD_PARAM));
		lbl->rogan(12.7, 20.32, "SHAPE");
		lbl->rogan(38.1, 20.32, "STABIL");
		lbl->rogan(12.7, 40.64, "CENTER");
		lbl->rogan(38.1, 40.64, "Y SPRD");
		lbl->rogan(12.7, 60.96, "FREQ");
		lbl->rogan(38.1, 60.96, "X SPRD");
		// what the ends of three knobs mean
		lbl->note(33.6, 28.3, "0%");
		lbl->note(43.2, 28.3, "100%");
		lbl->note(7.7, 48.6, "-5V");
		lbl->note(17.7, 48.6, "+5V");
		lbl->note(33.6, 48.6, "0V");
		lbl->note(42.8, 48.6, "5V");

		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(6.306, 85.365)), module, Drift::INSHAPE_INPUT));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(18.955, 85.365)), module, Drift::INSTABILITY_INPUT));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(31.568, 85.365)), module, Drift::INFREQUENCY_INPUT));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(44.182, 85.365)), module, Drift::INSPREAD_INPUT));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(18.955, 102.033)), module, Drift::INCENTER_INPUT));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(31.568, 102.033)), module, Drift::INYSPREAD_INPUT));
		lbl->jack(6.306, 85.365, "SHAPE");
		lbl->jack(18.955, 85.365, "STAB");
		lbl->jack(31.568, 85.365, "FREQ");
		lbl->jack(44.182, 85.365, "X SPR");
		lbl->jack(18.955, 102.033, "CNTR");
		lbl->jack(31.568, 102.033, "Y SPR");

		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(44.182, 102.033)), module, Drift::OUTMAX_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(6.255, 102.454)), module, Drift::OUTMIN_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(6.306, 117.363)), module, Drift::OUTA_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(18.955, 117.363)), module, Drift::OUTB_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(31.568, 117.363)), module, Drift::OUTC_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(44.182, 117.363)), module, Drift::OUTD_OUTPUT));
		lbl->jackOnPlate(6.255, 102.454, "MIN");
		lbl->jackOnPlate(44.182, 102.033, "MAX");
		lbl->jackOnPlate(6.306, 117.363, "A");
		lbl->jackOnPlate(18.955, 117.363, "B");
		lbl->jackOnPlate(31.568, 117.363, "C");
		lbl->jackOnPlate(44.182, 117.363, "D");
	}
};


Model* modelDrift = createModel<Drift, DriftWidget>("Drift");

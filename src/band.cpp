#include "plugin.hpp"
#include <cmath>
#include <functional>

// ─── Band: harmonic bandpass bank ────────────────────────────────────────────
//
// Four narrow bandpass "bands", each locked to an integer harmonic of a shared
// fundamental f0. Inspired by Suzanne Ciani's technique of isolating individual
// harmonics from a low, harmonically-rich wave.
//
// Harmonics are LINEARLY spaced (f0, 2f0, 3f0 …), so a normal (1V/oct) filter
// makes them fiddly to find. Here the fundamental tracks 1V/oct (matching your
// oscillator) but each band selects an integer HARMONIC; its center is N·f0 and
// lands dead-on every time. WIDTH is a fraction of f0 → constant absolute
// bandwidth, so every harmonic isolates equally cleanly. Everything is CV-able.

static const int N_BANDS = 4;
static const float C4_HZ = 261.6256f;   // standard 1V/oct reference (0V = C4)

// RBJ biquad bandpass, constant 0 dB peak gain (transposed direct form II).
struct BandpassBiquad {
	float b0 = 0.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
	float z1 = 0.f, z2 = 0.f;
	void setBandpass(float fc, float fs, float Q) {
		float w0 = 2.f * (float) M_PI * fc / fs;
		float cw = std::cos(w0), sw = std::sin(w0);
		float alpha = sw / (2.f * Q);
		float a0 = 1.f + alpha;
		b0 = alpha / a0; b1 = 0.f; b2 = -alpha / a0;
		a1 = (-2.f * cw) / a0; a2 = (1.f - alpha) / a0;
	}
	float process(float x) {
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
	void reset() { z1 = z2 = 0.f; }
};


struct Band : Module {
	enum ParamId {
		TUNE_PARAM,
		WIDTH_PARAM,
		ENUMS(HARM_PARAM, N_BANDS),
		ENUMS(LEVEL_PARAM, N_BANDS),
		ENUMS(ENABLE_PARAM, N_BANDS),
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		WIDTH_INPUT,
		HARM_INPUT,            // global continuous shift
		AUDIO_INPUT,
		ENUMS(HARM_CV_INPUT, N_BANDS),
		ENUMS(LEVEL_CV_INPUT, N_BANDS),
		ENUMS(ENABLE_CV_INPUT, N_BANDS),
		INPUTS_LEN
	};
	enum OutputId {
		MIX_OUTPUT,
		POLY_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(ENABLE_LIGHT, N_BANDS),
		LIGHTS_LEN
	};

	BandpassBiquad filt[N_BANDS][2];   // 2 cascaded = 4-pole
	float ampEnv[N_BANDS] = {};         // anti-click on/off envelope per band
	bool fourPole = true;

	// Auto-follow: detect the source's fundamental from the audio and lock the
	// harmonic grid to it (overrides TUNE/V-OCT when audio is present).
	bool followPitch = true;
	float detF0 = 0.f;
	bool  detValid = false;

	// Display state
	float dispF0 = 110.f;
	float dispSR = 44100.f;
	float dispCenterHarm[N_BANDS] = {};
	float dispLevel[N_BANDS] = {};
	bool  dispOn[N_BANDS] = {};
	float dispWidth = 0.15f;

	// Input spectrum analyzer (overlapped FFT of the source audio).
	static const int FFT_N = 4096;
	static const int FFT_BINS = FFT_N / 2;
	static const int FFT_HOP = FFT_N / 2;
	dsp::RealFFT* fft = nullptr;
	float ring[FFT_N] = {};
	int   ringW = 0, hopCount = 0;
	std::vector<float> fftFrame, fftSpec;     // windowed input / interleaved output
	std::vector<float> acfInput, acf;         // power spectrum / autocorrelation
	float spectrum[FFT_BINS] = {};            // smoothed magnitudes (for display)
	float spectrumMax = 1e-6f;

	~Band() { delete fft; }

	Band() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Standard 1V/oct tuning (octaves around C4) so a same-CV oscillator matches.
		configParam(TUNE_PARAM, -4.f, 4.f, -1.f, "Tune", " Hz", 2.f, C4_HZ);
		configParam(WIDTH_PARAM, 0.02f, 2.f, 0.15f, "Width", "×f0");
		for (int i = 0; i < N_BANDS; i++) {
			configParam(HARM_PARAM + i, 1.f, 32.f, (float)(i + 1),
				string::f("Band %d harmonic", i + 1));
			paramQuantities[HARM_PARAM + i]->snapEnabled = true;
			configParam(LEVEL_PARAM + i, 0.f, 1.f, 0.6f, string::f("Band %d level", i + 1), "%", 0.f, 100.f);
			configSwitch(ENABLE_PARAM + i, 0.f, 1.f, 1.f, string::f("Band %d enable", i + 1), {"Off", "On"});
			configInput(HARM_CV_INPUT + i, string::f("Band %d harmonic CV", i + 1));
			configInput(LEVEL_CV_INPUT + i, string::f("Band %d level CV", i + 1));
			configInput(ENABLE_CV_INPUT + i, string::f("Band %d enable gate", i + 1));
		}
		configInput(VOCT_INPUT, "Fundamental V/oct");
		configInput(WIDTH_INPUT, "Width CV");
		configInput(HARM_INPUT, "Harmonic shift CV (1V/harmonic, all bands)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(MIX_OUTPUT, "Mix");
		configOutput(POLY_OUTPUT, "Per-band (polyphonic)");

		fft = new dsp::RealFFT(FFT_N);
		fftFrame.resize(FFT_N, 0.f);
		fftSpec.resize(2 * FFT_N, 0.f);
		acfInput.resize(2 * FFT_N, 0.f);
		acf.resize(FFT_N, 0.f);
	}

	// Run an FFT over the most recent FFT_N input samples (Hann windowed) and
	// fold the magnitudes into the smoothed display spectrum.
	void computeSpectrum() {
		for (int i = 0; i < FFT_N; i++) {
			int idx = (ringW + i) % FFT_N;
			float win = 0.5f * (1.f - std::cos(2.f * (float) M_PI * i / (FFT_N - 1)));
			fftFrame[i] = ring[idx] * win;
		}
		fft->rfft(fftFrame.data(), fftSpec.data());
		// Canonical order: [0]=DC, [1]=Nyquist, [2k],[2k+1]=re,im of bin k.
		float frameMax = 1e-6f;
		for (int k = 1; k < FFT_BINS; k++) {
			float re = fftSpec[2 * k], im = fftSpec[2 * k + 1];
			float mag = std::sqrt(re * re + im * im);
			spectrum[k] += (mag - spectrum[k]) * 0.5f;   // temporal smoothing
			if (spectrum[k] > frameMax) frameMax = spectrum[k];
		}
		spectrum[0] = std::fabs(fftSpec[0]);
		spectrumMax += (frameMax - spectrumMax) * 0.2f;
		if (spectrumMax < 1e-6f) spectrumMax = 1e-6f;

		// --- Fundamental detection via autocorrelation (robust, no octave hacks) ---
		// Autocorrelation = IFFT(|FFT|^2). The period is the SHORTEST lag with a
		// strong peak, which avoids the sub-octave / DC-leakage traps that plagued
		// the harmonic-product approach.
		float sr = dispSR;
		acfInput[0] = fftSpec[0] * fftSpec[0];               // DC power
		acfInput[1] = fftSpec[1] * fftSpec[1];               // Nyquist power
		for (int k = 1; k < FFT_BINS; k++) {
			float re = fftSpec[2 * k], im = fftSpec[2 * k + 1];
			acfInput[2 * k]     = re * re + im * im;          // power spectrum (real)
			acfInput[2 * k + 1] = 0.f;
		}
		fft->irfft(acfInput.data(), acf.data());

		int minLag = std::max(2, (int) std::floor(sr / 2000.f));   // up to 2 kHz
		int maxLag = std::min(FFT_N - 2, (int) std::ceil(sr / 40.f)); // down to 40 Hz
		float zero = (acf[0] > 1e-6f) ? acf[0] : 1e-6f;
		float gmax = 0.f;
		for (int t = minLag; t <= maxLag; t++) if (acf[t] > gmax) gmax = acf[t];

		int bestLag = -1;
		if (gmax > 0.f) {
			float thr = 0.85f * gmax;                         // shortest strong peak
			for (int t = minLag; t <= maxLag; t++) {
				if (acf[t] >= thr) {
					while (t + 1 <= maxLag && acf[t + 1] > acf[t]) t++;  // climb to peak
					bestLag = t;
					break;
				}
			}
		}
		if (bestLag > minLag && bestLag < maxLag && gmax > 0.05f * zero && spectrumMax > 1e-4f) {
			float a = acf[bestLag - 1], b = acf[bestLag], c = acf[bestLag + 1];
			float denom = a - 2.f * b + c;
			float d = (std::fabs(denom) > 1e-9f) ? 0.5f * (a - c) / denom : 0.f;
			d = clamp(d, -0.5f, 0.5f);
			float raw = sr / (bestLag + d);
			if (!detValid || std::fabs(raw - detF0) > detF0 * 0.06f) detF0 = raw;   // snap on note change
			else detF0 += (raw - detF0) * 0.3f;                                     // smooth small jitter
			detValid = true;
		} else {
			detValid = false;
		}
	}

	void process(const ProcessArgs& args) override {
		float fs = args.sampleRate;
		// Manual fundamental: standard 1V/oct (0V = C4), matches a normal VCO.
		float manualF0 = C4_HZ * std::pow(2.f, params[TUNE_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage());
		// Auto-follow overrides it when enabled and a pitch is detected.
		float f0 = manualF0;
		if (followPitch && inputs[AUDIO_INPUT].isConnected() && detValid && detF0 > 1.f)
			f0 = detF0;
		f0 = clamp(f0, 1.f, fs * 0.45f);
		dispF0 = f0;
		dispSR = fs;

		float width = params[WIDTH_PARAM].getValue();
		if (inputs[WIDTH_INPUT].isConnected())
			width += inputs[WIDTH_INPUT].getVoltage() * 0.2f;
		width = clamp(width, 0.01f, 4.f);
		dispWidth = width;

		float globalShift = inputs[HARM_INPUT].isConnected() ? inputs[HARM_INPUT].getVoltage() : 0.f;

		float in = inputs[AUDIO_INPUT].getVoltage();
		float mix = 0.f;
		int stages = fourPole ? 2 : 1;

		for (int i = 0; i < N_BANDS; i++) {
			// Enable: per-band gate overrides the button when patched.
			bool on = params[ENABLE_PARAM + i].getValue() > 0.5f;
			if (inputs[ENABLE_CV_INPUT + i].isConnected())
				on = inputs[ENABLE_CV_INPUT + i].getVoltage() >= 1.f;
			lights[ENABLE_LIGHT + i].setBrightness(on ? 1.f : 0.f);
			dispOn[i] = on;

			float centerHarm = params[HARM_PARAM + i].getValue() + globalShift
				+ (inputs[HARM_CV_INPUT + i].isConnected() ? inputs[HARM_CV_INPUT + i].getVoltage() : 0.f);
			dispCenterHarm[i] = centerHarm;

			float level = params[LEVEL_PARAM + i].getValue();
			if (inputs[LEVEL_CV_INPUT + i].isConnected())
				level += inputs[LEVEL_CV_INPUT + i].getVoltage() * 0.1f;   // 10V → +1
			level = clamp(level, 0.f, 1.f);
			dispLevel[i] = level;

			// Anti-click VCA: slew an amplitude envelope toward on/off (~2 ms)
			// so enabling/disabling a band fades instead of stepping.
			float target = on ? 1.f : 0.f;
			float step = args.sampleTime / 0.002f;
			if (ampEnv[i] < target) ampEnv[i] = std::min(target, ampEnv[i] + step);
			else                    ampEnv[i] = std::max(target, ampEnv[i] - step);

			float bandOut = 0.f;
			float fc = centerHarm * f0;
			bool audible = ampEnv[i] > 1e-4f;
			if (audible && centerHarm > 0.25f && fc > 1.f && fc < fs * 0.47f) {
				float Q = clamp(centerHarm / width, 0.5f, 250.f);
				for (int s = 0; s < stages; s++) filt[i][s].setBandpass(fc, fs, Q);
				float y = in;
				for (int s = 0; s < stages; s++) y = filt[i][s].process(y);
				bandOut = y * level * ampEnv[i];
				mix += bandOut;
			} else {
				filt[i][0].reset(); filt[i][1].reset();
				ampEnv[i] = target;   // settle fully when silent
			}
			outputs[POLY_OUTPUT].setVoltage(bandOut, i);
		}
		outputs[POLY_OUTPUT].setChannels(N_BANDS);
		outputs[MIX_OUTPUT].setVoltage(clamp(mix, -12.f, 12.f));

		// --- Input spectrum: overlapped FFT of the source audio ---
		ring[ringW] = in;
		ringW = (ringW + 1) % FFT_N;
		if (++hopCount >= FFT_HOP) { hopCount = 0; computeSpectrum(); }
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "fourPole", json_boolean(fourPole));
		json_object_set_new(root, "followPitch", json_boolean(followPitch));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "fourPole")) fourPole = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "followPitch")) followPitch = json_boolean_value(j);
	}
};


// ─── Display: live input scope behind the harmonic bells ─────────────────────
struct BandDisplay : OpaqueWidget {
	Band* module = nullptr;
	std::shared_ptr<Font> font;
	static const int MAX_HARM = 32;

	// Spectrum analyzer of the source, drawn on the SAME harmonic x-axis
	// (frequency / f0) so energy peaks line up under the band markers.
	void drawSpectrum(NVGcontext* vg, float padL, float plotW, float baseY, float plotH,
	                  std::function<float(float)> hx, bool preview) {
		nvgBeginPath(vg);
		nvgMoveTo(vg, padL, baseY);
		bool any = false;
		if (preview) {
			// Fake harmonic spectrum: peaks at integer harmonics with 1/n falloff.
			int steps = 400;
			for (int s = 0; s <= steps; s++) {
				float harm = 0.5f + (float) s / steps * (float)(MAX_HARM);
				float v = 0.f;
				for (int n = 1; n <= 12; n++) {
					float d = (harm - n) / 0.12f;
					v += (1.f / n) * std::exp(-d * d);
				}
				float y = baseY - clamp(std::pow(v, 0.6f), 0.f, 1.f) * plotH;
				nvgLineTo(vg, hx(harm), y); any = true;
			}
		} else {
			int N = Band::FFT_N, bins = Band::FFT_BINS;
			float sr = module->dispSR, f0 = module->dispF0;
			float invMax = 1.f / module->spectrumMax;
			for (int k = 1; k < bins; k++) {
				float freq = (float) k * sr / N;
				float harm = freq / f0;
				if (harm > MAX_HARM + 1) break;
				float norm = clamp(module->spectrum[k] * invMax, 0.f, 1.f);
				float y = baseY - std::pow(norm, 0.6f) * plotH;
				nvgLineTo(vg, hx(harm), y); any = true;
			}
		}
		if (!any) return;
		nvgLineTo(vg, padL + plotW, baseY);
		nvgClosePath(vg);
		nvgFillColor(vg, nvgRGBA(0x6c, 0xb0, 0x90, 0x55));    // dim teal fill
		nvgFill(vg);
	}

	void drawContent(NVGcontext* vg, bool preview) {
		const NVGcolor COL_BG    = nvgRGB(0x1a, 0x1a, 0x2e);
		const NVGcolor COL_GRID  = nvgRGB(0x35, 0x35, 0x4d);
		// Per-band colors — match the A/B/C/D selector labels on the panel.
		const NVGcolor BAND_COL[N_BANDS] = {
			nvgRGB(0x00, 0x97, 0xde),   // A
			nvgRGB(0x1f, 0xbc, 0x17),   // B
			nvgRGB(0xdd, 0x64, 0x00),   // C
			nvgRGB(0x69, 0x2f, 0xbc),   // D
		};
		float w = box.size.x, h = box.size.y;

		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 3.f);
		nvgFillColor(vg, COL_BG); nvgFill(vg);
		nvgStrokeColor(vg, COL_GRID); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);

		// Clip everything that follows to the screen so bells/spectrum can't spill
		// past the panel edge.
		nvgIntersectScissor(vg, 1.f, 1.f, w - 2.f, h - 2.f);

		float padL = 6.f, padR = 6.f, padT = 6.f, padB = 12.f;
		float plotW = w - padL - padR, plotH = h - padT - padB;
		float baseY = h - padB;
		auto hx = [&](float harm) { return padL + harm / (float)(MAX_HARM + 1) * plotW; };

		// Spectrum analyzer sits beneath the harmonic markers.
		drawSpectrum(vg, padL, plotW, baseY, plotH, hx, preview);

		// Harmonic gridlines
		for (int n = 1; n <= MAX_HARM; n++) {
			float x = hx((float) n);
			nvgBeginPath(vg); nvgMoveTo(vg, x, padT); nvgLineTo(vg, x, baseY);
			nvgStrokeColor(vg, nvgRGBA(0x35, 0x35, 0x4d, (n % 4 == 1) ? 0xcc : 0x44));
			nvgStrokeWidth(vg, (n % 4 == 1) ? 1.f : 0.6f); nvgStroke(vg);
		}
		nvgBeginPath(vg); nvgMoveTo(vg, padL, baseY); nvgLineTo(vg, w - padR, baseY);
		nvgStrokeColor(vg, COL_GRID); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);

		// Band bells, colored per band (A/B/C/D) so it's clear which control owns
		// which harmonic — colors match the A–D selector labels on the panel.
		struct B { float harm, level, widthHarm; bool on; };
		B bands[N_BANDS];
		if (preview) {
			B p[N_BANDS] = {{1, 0.9f, 0.15f, true}, {3, 0.7f, 0.15f, true},
			                {5, 0.5f, 0.15f, true}, {8, 0.6f, 0.15f, true}};
			for (int i = 0; i < N_BANDS; i++) bands[i] = p[i];
		} else {
			for (int i = 0; i < N_BANDS; i++)
				bands[i] = {module->dispCenterHarm[i], module->dispLevel[i], module->dispWidth, module->dispOn[i]};
		}

		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		bool haveFont = font && font->handle >= 0;

		for (int i = 0; i < N_BANDS; i++) {
			const B& b = bands[i];
			if (!b.on || b.level < 0.001f) continue;
			NVGcolor col = BAND_COL[i];
			float cx = hx(b.harm);
			float topY = baseY - b.level * plotH;
			float hw = clamp(b.widthHarm, 0.05f, 4.f) * (plotW / (float)(MAX_HARM + 1)) * 1.2f;
			nvgBeginPath(vg);
			nvgMoveTo(vg, cx - hw, baseY);
			nvgBezierTo(vg, cx - hw * 0.4f, baseY, cx - hw * 0.4f, topY, cx, topY);
			nvgBezierTo(vg, cx + hw * 0.4f, topY, cx + hw * 0.4f, baseY, cx + hw, baseY);
			nvgClosePath(vg);
			nvgFillColor(vg, nvgRGBAf(col.r, col.g, col.b, 0.33f)); nvgFill(vg);
			nvgBeginPath(vg); nvgMoveTo(vg, cx, baseY); nvgLineTo(vg, cx, topY);
			nvgStrokeColor(vg, col); nvgStrokeWidth(vg, 1.6f); nvgStroke(vg);
			nvgBeginPath(vg); nvgCircle(vg, cx, topY, 2.4f);
			nvgFillColor(vg, col); nvgFill(vg);
			// Label: band letter + harmonic (e.g. "A5"), in the band's color, so
			// "band A is on harmonic 5" reads at a glance.
			if (haveFont) {
				int hn = (int) std::round(b.harm);
				nvgFontFaceId(vg, font->handle);
				nvgFontSize(vg, 8.5f);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
				nvgFillColor(vg, col);
				nvgText(vg, cx, topY - 4.f, string::f("%c%d", 'A' + i, hn).c_str(), NULL);
			}
		}

		if (haveFont) {
			nvgFontFaceId(vg, font->handle);
			nvgFontSize(vg, 8.f);
			nvgFillColor(vg, nvgRGBA(0xf0, 0xc0, 0x60, 0xc0));
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
			float f0 = preview ? 110.f : module->dispF0;
			bool autoLock = !preview && module->followPitch && module->detValid
				&& module->inputs[Band::AUDIO_INPUT].isConnected();
			nvgText(vg, padL + 2.f, h - 2.f,
				string::f("f0 %.1f Hz%s", f0, autoLock ? "  AUTO" : "").c_str(), NULL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
			nvgFillColor(vg, nvgRGBA(0x80, 0x90, 0xb0, 0xc0));
			nvgText(vg, w - padR - 2.f, h - 2.f, "harmonics →", NULL);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		drawContent(args.vg, module == nullptr);
		OpaqueWidget::drawLayer(args, layer);
	}
};


struct BandWidget : ModuleWidget {
	BandWidget(Band* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/band.svg")));

		// Display screen (matches the dashed reticule in res/band.svg)
		BandDisplay* display = new BandDisplay();
		display->module = module;
		display->box.pos = mm2px(Vec(5.08f, 11.85f));
		display->box.size = mm2px(Vec(111.76f, 30.f));
		addChild(display);

		// Per-band controls — positions read from the reticules in res/band.svg.
		// Each band = a knob column + a CV-jack column; three rows: LVL, HRM, EN.
		const float knobX[N_BANDS] = {10.16f, 38.10f, 66.04f, 93.98f};
		const float jackX[N_BANDS] = {25.40f, 53.34f, 81.28f, 109.22f};
		const float yLvl = 60.95f, yHrm = 81.27f, yEn = 101.59f;
		for (int i = 0; i < N_BANDS; i++) {
			float kx = knobX[i], jx = jackX[i];
			// Row 1: level + CV
			addParam(createParamCentered<Trimpot>(mm2px(Vec(kx, yLvl)), module, Band::LEVEL_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx, yLvl)), module, Band::LEVEL_CV_INPUT + i));
			// Row 2: harmonic selector (A/B/C/D) + CV
			addParam(createParamCentered<Trimpot>(mm2px(Vec(kx, yHrm)), module, Band::HARM_PARAM + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx, yHrm)), module, Band::HARM_CV_INPUT + i));
			// Row 3: enable + gate CV
			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
				mm2px(Vec(kx, yEn)), module, Band::ENABLE_PARAM + i, Band::ENABLE_LIGHT + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx, yEn)), module, Band::ENABLE_CV_INPUT + i));
		}

		// Global bottom row (y = 121.91 mm)
		const float yG = 121.91f;
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, yG)), module, Band::AUDIO_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.40f, yG)), module, Band::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.64f, yG)), module, Band::HARM_INPUT));   // SHIFT
		addParam(createParamCentered<Trimpot>(mm2px(Vec(53.70f, yG)), module, Band::TUNE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(66.04f, yG)), module, Band::WIDTH_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(81.28f, yG)), module, Band::WIDTH_INPUT));  // W-CV
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(95.67f, yG)), module, Band::MIX_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(109.22f, yG)), module, Band::POLY_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Band* module = dynamic_cast<Band*>(this->module);
		assert(module);
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Follow input pitch (auto-lock harmonics)", "",
			&module->followPitch));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Filter slope"));
		menu->addChild(createCheckMenuItem("4-pole (steep, default)", "",
			[=]() { return module->fourPole; }, [=]() { module->fourPole = true; }));
		menu->addChild(createCheckMenuItem("2-pole (gentler)", "",
			[=]() { return !module->fourPole; }, [=]() { module->fourPole = false; }));
	}
};


Model* modelBand = createModel<Band, BandWidget>("Band");

#include "plugin.hpp"
#include <cmath>


// 4-output CV shift register, per-output controls.
//   Each output has its own N pot (1-16) and parallel/cascade mode switch.
//   - Parallel: this output is a delay line of length N. On every divided
//     input clock the lane pushes the current input CV into the delay
//     line and outputs the value from N (divided) clocks ago. The DIV
//     knob controls the input clock divider for the lane (how many input
//     clocks per delay-line step); N controls the depth of the delay
//     tap (how many lane-steps back the output reads). Captures are
//     also mirrored into the full-depth history ring so disconnect
//     playback can keep cycling.
//   - Cascade:  this output is a "tape loop" of length N. Its CV cycles
//     through the buffer continuously at the input-clock rate (so it
//     never sits still). The parent writes its current value into the
//     buffer every time the parent ticks — slower than the read rate —
//     so the buffer evolves gradually as new content drips in.
//     LED + gate fire on the WRITE events (parent's tick rate), not on
//     every read, so they meaningfully indicate "new content arrived".
//     The cascade chain propagates write events down: each cascaded
//     output writes when its parent writes, at the same rate.
//     Cascade on output A falls back to parallel (no preceding output).
//
// All four outputs share a single optional N CV input that sums into every
// per-output N pot (so you can sweep the whole register's rate at once).

static const int NUM_OUTS  = 4;
static const int MAX_N     = 16;   // delay-line / history ring size
static const int MAX_STEPS = 15;   // max selectable delay in steps; 0 = passthrough

// Per-lane clock divider values (selected by a snapped trimpot).
static const int DIV_VALUES[]   = { 1, 2, 3, 4, 5, 8 };
static const int NUM_DIV_VALUES = 6;


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
		DIV_PARAM_A,
		DIV_PARAM_B,
		DIV_PARAM_C,
		DIV_PARAM_D,
		RESET_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CV_INPUT,
		CLOCK_INPUT,
		N_CV_INPUT,
		RESET_INPUT,
		STEP_CV_INPUT_A,
		STEP_CV_INPUT_B,
		STEP_CV_INPUT_C,
		STEP_CV_INPUT_D,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_A,
		OUT_B,
		OUT_C,
		OUT_D,
		GATE_A,
		GATE_B,
		GATE_C,
		GATE_D,
		JUMBLE_CV,
		JUMBLE_CLK,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHT_A,
		LIGHT_B,
		LIGHT_C,
		LIGHT_D,
		LIGHT_JUMBLE,
		LIGHTS_LEN
	};

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger resetTrigger;
	dsp::SchmittTrigger resetButtonTrigger;
	dsp::PulseGenerator gatePulse[NUM_OUTS];
	dsp::PulseGenerator jumblePulse;
	float jumbleHeld = 0.f;
	float jumbleLedFlash = 0.f;

	int   counter[NUM_OUTS]  = {0, 0, 0, 0};
	float held[NUM_OUTS]     = {0.f, 0.f, 0.f, 0.f};
	float ledFlash[NUM_OUTS] = {0.f, 0.f, 0.f, 0.f};
	int   divCounter[NUM_OUTS] = {0, 0, 0, 0};   // per-lane clock divider counter

	// Per-output tape-loop buffer for cascade mode. Length used = current
	// targetN[i]; positions 0..targetN[i]-1 are the live ring.
	//   readIdx  advances on every input clock — drives the CV output.
	//   writeIdx advances when the parent ticks — overwrites buffer slots
	//            with the parent's current value.
	float delayLine[NUM_OUTS][MAX_N] = {};
	int   readIdx[NUM_OUTS]  = {0, 0, 0, 0};
	int   writeIdx[NUM_OUTS] = {0, 0, 0, 0};

	// Separate full-depth history ring (always MAX_N slots) accumulated in
	// parallel with the active buffer. When the CV input is disconnected,
	// the output cycles through this 16-slot history at clock rate so even
	// outputs with small N (e.g. cascade N=1, where the active buffer is a
	// single slot) have a meaningful "play out the remainder" stream.
	float historyLine[NUM_OUTS][MAX_N] = {};
	int   historyWriteIdx[NUM_OUTS] = {0, 0, 0, 0};
	int   historyReadIdx[NUM_OUTS]  = {0, 0, 0, 0};

	struct NParamQuantity : ParamQuantity {
		std::string getDisplayValueString() override {
			return string::f("%.0f", std::round(getValue()));
		}
	};

	Shift() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		const char* labels[NUM_OUTS] = {"A", "B", "C", "D"};
		for (int i = 0; i < NUM_OUTS; i++) {
			configParam<NParamQuantity>(N_PARAM_A + i, 0.f, (float)MAX_STEPS, 0.f,
				string::f("Step count %s (0 = no delay)", labels[i]));
			paramQuantities[N_PARAM_A + i]->snapEnabled = true;
			configSwitch(MODE_PARAM_A + i, 0.f, 1.f, 0.f,
				string::f("Mode %s", labels[i]),
				{"Parallel", "Cascade"});
			configSwitch(DIV_PARAM_A + i, 0.f, (float)(NUM_DIV_VALUES - 1), 0.f,
				string::f("Clock divider %s", labels[i]),
				{"÷1", "÷2", "÷3", "÷4", "÷5", "÷8"});
			paramQuantities[DIV_PARAM_A + i]->snapEnabled = true;
			configInput(STEP_CV_INPUT_A + i,
				string::f("Step CV %s (±5V → ±15, summed with N pot)", labels[i]));
			configOutput(OUT_A + i, string::f("Output %s", labels[i]));
		}
		configInput(CV_INPUT,    "CV (data, sampled on each tap fire)");
		configInput(CLOCK_INPUT, "Clock (advances all taps in parallel mode, "
		                        "and the cascade chain root)");
		configInput(N_CV_INPUT,  "N CV (±5V → ±15, summed into every N pot — "
		                        "global modulator on top of per-lane Step CVs)");
		configInput(RESET_INPUT, "Reset (clears all counters and held values)");
		configButton(RESET_PARAM, "Reset (button)");
		for (int i = 0; i < NUM_OUTS; i++) {
			configOutput(GATE_A + i, string::f("Gate %s (1ms pulse on each tap fire)", labels[i]));
		}
		configOutput(JUMBLE_CV,  "Jumble CV (random pick from A/B/C/D, "
		                        "re-rolled on each input clock)");
		configOutput(JUMBLE_CLK, "Jumble clock (1ms pulse on each re-roll)");
	}

	// Wipes the active delay-line buffers and the full-depth history rings
	// for every lane, plus the held/jumble values and all read/write/divider
	// indices. Equivalent to a full Reset trigger but reachable from the
	// context menu so the user can flush stored content without touching
	// param settings or sending a reset pulse.
	void clearAll() {
		for (int i = 0; i < NUM_OUTS; i++) {
			counter[i] = 0;
			held[i] = 0.f;
			ledFlash[i] = 0.f;
			readIdx[i] = 0;
			writeIdx[i] = 0;
			historyWriteIdx[i] = 0;
			historyReadIdx[i] = 0;
			divCounter[i] = 0;
			for (int k = 0; k < MAX_N; k++) {
				delayLine[i][k] = 0.f;
				historyLine[i][k] = 0.f;
			}
		}
		jumbleHeld = 0.f;
		jumbleLedFlash = 0.f;
	}

	void onReset() override {
		clearAll();
	}

	void process(const ProcessArgs& args) override {
		// Global N CV — added to every per-output N pot before clamping.
		float nCv = inputs[N_CV_INPUT].isConnected()
			? inputs[N_CV_INPUT].getVoltage() * (float)MAX_STEPS / 5.f
			: 0.f;

		int targetN[NUM_OUTS];
		int laneDiv[NUM_OUTS];
		for (int i = 0; i < NUM_OUTS; i++) {
			float n = params[N_PARAM_A + i].getValue() + nCv;
			if (inputs[STEP_CV_INPUT_A + i].isConnected()) {
				n += inputs[STEP_CV_INPUT_A + i].getVoltage() * (float)MAX_STEPS / 5.f;
			}
			int ni = (int)std::round(n);
			if (ni < 0)         ni = 0;
			if (ni > MAX_STEPS) ni = MAX_STEPS;
			targetN[i] = ni;

			int divIdx = (int)std::round(params[DIV_PARAM_A + i].getValue());
			if (divIdx < 0)                  divIdx = 0;
			if (divIdx >= NUM_DIV_VALUES)    divIdx = NUM_DIV_VALUES - 1;
			laneDiv[i] = DIV_VALUES[divIdx];
		}

		// --- Reset (input OR button) ---
		bool resetIn  = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		bool resetBtn = resetButtonTrigger.process(params[RESET_PARAM].getValue());
		if (resetIn || resetBtn) {
			for (int i = 0; i < NUM_OUTS; i++) {
				counter[i] = 0;
				held[i] = 0.f;
				readIdx[i] = 0;
				writeIdx[i] = 0;
				historyWriteIdx[i] = 0;
				historyReadIdx[i] = 0;
				divCounter[i] = 0;
				for (int k = 0; k < MAX_N; k++) {
					delayLine[i][k] = 0.f;
					historyLine[i][k] = 0.f;
				}
			}
			jumbleHeld = 0.f;
		}

		// --- Clock processing ---
		bool clockRose = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		bool tickFired[NUM_OUTS] = {false, false, false, false};
		bool cvConnected = inputs[CV_INPUT].isConnected();
		float inCV = inputs[CV_INPUT].getVoltage();

		// laneTick[i] = true when this lane sees a divided clock pulse this
		// sample. Drives the LED + gate output (so per-lane gates fire on
		// the lane's clock regardless of CV connection or mode). Distinct
		// from tickFired[], which still tracks "this lane received new
		// data" events for cascade chain propagation.
		bool laneTick[NUM_OUTS] = {false, false, false, false};

		if (clockRose) {
			for (int i = 0; i < NUM_OUTS; i++) {
				// --- Per-lane clock divider ---
				// Increment this lane's divider counter on every input
				// clock pulse; only run the lane's processing when the
				// counter wraps. So div=4 means this lane sees one "tick"
				// every 4 input clocks — its cascade reads cycle slower,
				// its parallel divider counter advances slower, and its
				// gate fires at the divided rate.
				divCounter[i]++;
				if (divCounter[i] < laneDiv[i]) continue;
				divCounter[i] = 0;
				laneTick[i] = true;

				bool cascade = params[MODE_PARAM_A + i].getValue() > 0.5f;
				int N = targetN[i];

				if (cvConnected) {
					// --- CONNECTED: normal active-buffer behavior. ---
					if (cascade && i > 0) {
						// Cascade tape loop: read at lane-clock rate, write
						// on parent tick. CV cycles through the active
						// N-slot buffer continuously.
						// N==0 = zero-length loop: pass the parent straight through.
						if (N == 0) {
							held[i] = held[i - 1];
							if (tickFired[i - 1]) {
								historyLine[i][historyWriteIdx[i]] = held[i - 1];
								historyWriteIdx[i] = (historyWriteIdx[i] + 1) % MAX_N;
								tickFired[i] = true;
							}
						} else {
							int rIdx = readIdx[i] % N;
							held[i] = delayLine[i][rIdx];
							readIdx[i] = (rIdx + 1) % N;

							if (tickFired[i - 1]) {
								int wIdx = writeIdx[i] % N;
								delayLine[i][wIdx] = held[i - 1];
								writeIdx[i] = (wIdx + 1) % N;
								historyLine[i][historyWriteIdx[i]] = held[i - 1];
								historyWriteIdx[i] = (historyWriteIdx[i] + 1) % MAX_N;
								tickFired[i] = true;
							}
						}
					} else {
						// Parallel (or cascade-on-A): N-step delay line.
						// Read the value from N lane-steps ago out of the
						// always-written full-depth history ring, then write
						// the current input. Using the continuously-written
						// ring (lookback = N) rather than an N-sized buffer
						// keeps the delay correct even when N is modulated on
						// the fly via the Step CV — otherwise a changing N
						// indexes slots that were never written at that N and
						// the output reads stale/zero values (looks frozen).
						// N==0 = no delay: the input passes straight through.
						if (N == 0) {
							held[i] = inCV;
						} else {
							int rIdx = (historyWriteIdx[i] - N + MAX_N) % MAX_N;
							held[i] = historyLine[i][rIdx];
						}
						historyLine[i][historyWriteIdx[i]] = inCV;
						historyWriteIdx[i] = (historyWriteIdx[i] + 1) % MAX_N;
						tickFired[i] = true;
					}
				} else {
					// --- DISCONNECTED: every lane-clock advances the
					//     history ring read pointer. Output cycles through
					//     up to MAX_N captured values regardless of N, so
					//     even N=1 outputs keep playing when CV drops. ---
					int hIdx = historyReadIdx[i] % MAX_N;
					held[i] = historyLine[i][hIdx];
					historyReadIdx[i] = (hIdx + 1) % MAX_N;
				}
			}
		}

		// --- Per-channel gate pulses (1ms on each lane clock tick) + LED ---
		// Gate + LED follow the divided input clock for that lane, so they
		// keep firing even when the CV input is disconnected. tickFired[]
		// (used for cascade chain propagation) only fires on actual data
		// events — gate output is driven by laneTick[] instead.
		float decay = args.sampleTime / 0.12f;
		for (int i = 0; i < NUM_OUTS; i++) {
			if (laneTick[i]) {
				ledFlash[i] = 1.f;
				gatePulse[i].trigger(0.001f);
			} else {
				ledFlash[i] = std::max(0.f, ledFlash[i] - decay);
			}
			lights[LIGHT_A + i].setBrightness(ledFlash[i]);
			outputs[OUT_A + i].setVoltage(held[i]);
			bool gateHi = gatePulse[i].process(args.sampleTime);
			outputs[GATE_A + i].setVoltage(gateHi ? 10.f : 0.f);
		}

		// --- Jumble: re-roll on each input clock pulse, picking a random
		//     channel's current held value. JUMBLE_CV is the picked value
		//     (S&H), JUMBLE_CLK is a 1ms pulse on each re-roll. The jumble
		//     LED flashes on each re-roll using the same decay envelope as
		//     the per-lane LEDs.
		if (clockRose) {
			int pick = (int)(random::uniform() * (float)NUM_OUTS);
			if (pick < 0)         pick = 0;
			if (pick >= NUM_OUTS) pick = NUM_OUTS - 1;
			jumbleHeld = held[pick];
			jumblePulse.trigger(0.001f);
			jumbleLedFlash = 1.f;
		} else {
			jumbleLedFlash = std::max(0.f, jumbleLedFlash - decay);
		}
		lights[LIGHT_JUMBLE].setBrightness(jumbleLedFlash);
		outputs[JUMBLE_CV].setVoltage(jumbleHeld);
		outputs[JUMBLE_CLK].setVoltage(jumblePulse.process(args.sampleTime) ? 10.f : 0.f);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* heldJ  = json_array();
		json_t* cntJ   = json_array();
		json_t* delayJ = json_array();
		json_t* rIdxJ  = json_array();
		json_t* wIdxJ  = json_array();
		for (int i = 0; i < NUM_OUTS; i++) {
			json_array_append_new(heldJ, json_real(held[i]));
			json_array_append_new(cntJ,  json_integer(counter[i]));
			json_array_append_new(rIdxJ, json_integer(readIdx[i]));
			json_array_append_new(wIdxJ, json_integer(writeIdx[i]));
			json_t* row = json_array();
			for (int k = 0; k < MAX_N; k++) {
				json_array_append_new(row, json_real(delayLine[i][k]));
			}
			json_array_append_new(delayJ, row);
		}
		json_object_set_new(root, "held",      heldJ);
		json_object_set_new(root, "counters",  cntJ);
		json_object_set_new(root, "delayLine", delayJ);
		json_object_set_new(root, "readIdx",   rIdxJ);
		json_object_set_new(root, "writeIdx",  wIdxJ);
		// History ring (full MAX_N depth, used for disconnect playback).
		json_t* histJ = json_array();
		json_t* histWJ = json_array();
		json_t* histRJ = json_array();
		for (int i = 0; i < NUM_OUTS; i++) {
			json_array_append_new(histWJ, json_integer(historyWriteIdx[i]));
			json_array_append_new(histRJ, json_integer(historyReadIdx[i]));
			json_t* row = json_array();
			for (int k = 0; k < MAX_N; k++) {
				json_array_append_new(row, json_real(historyLine[i][k]));
			}
			json_array_append_new(histJ, row);
		}
		json_object_set_new(root, "historyLine",     histJ);
		json_object_set_new(root, "historyWriteIdx", histWJ);
		json_object_set_new(root, "historyReadIdx",  histRJ);
		json_object_set_new(root, "jumble", json_real(jumbleHeld));
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
		if (json_t* j = json_object_get(root, "readIdx")) {
			for (int i = 0; i < NUM_OUTS && i < (int)json_array_size(j); i++) {
				readIdx[i] = (int)json_integer_value(json_array_get(j, i));
			}
		}
		if (json_t* j = json_object_get(root, "writeIdx")) {
			for (int i = 0; i < NUM_OUTS && i < (int)json_array_size(j); i++) {
				writeIdx[i] = (int)json_integer_value(json_array_get(j, i));
			}
		}
		if (json_t* j = json_object_get(root, "delayLine")) {
			for (int i = 0; i < NUM_OUTS && i < (int)json_array_size(j); i++) {
				json_t* row = json_array_get(j, i);
				if (!row) continue;
				for (int k = 0; k < MAX_N && k < (int)json_array_size(row); k++) {
					delayLine[i][k] = (float)json_number_value(json_array_get(row, k));
				}
			}
		}
		if (json_t* j = json_object_get(root, "historyWriteIdx")) {
			for (int i = 0; i < NUM_OUTS && i < (int)json_array_size(j); i++) {
				historyWriteIdx[i] = (int)json_integer_value(json_array_get(j, i));
			}
		}
		if (json_t* j = json_object_get(root, "historyReadIdx")) {
			for (int i = 0; i < NUM_OUTS && i < (int)json_array_size(j); i++) {
				historyReadIdx[i] = (int)json_integer_value(json_array_get(j, i));
			}
		}
		if (json_t* j = json_object_get(root, "historyLine")) {
			for (int i = 0; i < NUM_OUTS && i < (int)json_array_size(j); i++) {
				json_t* row = json_array_get(j, i);
				if (!row) continue;
				for (int k = 0; k < MAX_N && k < (int)json_array_size(row); k++) {
					historyLine[i][k] = (float)json_number_value(json_array_get(row, k));
				}
			}
		}
		if (json_t* j = json_object_get(root, "jumble")) {
			jumbleHeld = (float)json_number_value(j);
		}
	}
};


// --- Widget ---

// Custom 2-position horizontal switch. Frame 0 = handle right (Parallel),
// Frame 1 = handle left (Cascade). Used for the per-lane MODE switch so the
// switch lies flat on the panel grid with cascade on the left.
struct CKSSHoriz : app::SvgSwitch {
	CKSSHoriz() {
		shadow->opacity = 0.f;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/CKSSH_0.svg")));
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/CKSSH_1.svg")));
	}
};


struct ShiftWidget : ModuleWidget {
	ShiftWidget(Shift* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/shift.svg")));

		// All positions taken from the centerpoints of the placeholders in
		// res/shift.svg (1mm = 2.835 viewBox units).

		// --- Top inputs row at y=25.40 ---
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(10.16f, 25.40f)), module, Shift::CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(25.40f, 25.40f)), module, Shift::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(40.64f, 25.40f)), module, Shift::N_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(55.88f, 25.40f)), module, Shift::RESET_INPUT));
		addParam(createParamCentered<VCVButton>(
			mm2px(Vec(73.66f, 25.40f)), module, Shift::RESET_PARAM));

		// --- Per-output rows ---
		const float rowYs[NUM_OUTS] = { 60.95f, 76.19f, 91.43f, 106.67f };
		const float xPot     = 10.16f;
		const float xStepCV  = 25.40f;
		const float xMode    = 35.75f;
		const float xDiv     = 45.72f;
		const float xLED     = 55.88f;
		const float xCV      = 63.49f;
		const float xGate    = 73.66f;
		for (int i = 0; i < NUM_OUTS; i++) {
			float y = rowYs[i];
			addParam(createParamCentered<RoundBlackKnob>(
				mm2px(Vec(xPot, y)), module, Shift::N_PARAM_A + i));
			addInput(createInputCentered<PJ301MPort>(
				mm2px(Vec(xStepCV, y)), module, Shift::STEP_CV_INPUT_A + i));
			// No mode switch on row A — cascade-on-A falls back to
			// parallel anyway, so the panel hides A's switch. The
			// MODE_PARAM_A param still exists in the enum (preserved for
			// JSON compatibility); its value just doesn't affect anything.
			if (i > 0) {
				addParam(createParamCentered<CKSSHoriz>(
					mm2px(Vec(xMode, y)), module, Shift::MODE_PARAM_A + i));
			}
			addParam(createParamCentered<Trimpot>(
				mm2px(Vec(xDiv, y)), module, Shift::DIV_PARAM_A + i));
			addChild(createLightCentered<SmallLight<GreenLight>>(
				mm2px(Vec(xLED, y)), module, Shift::LIGHT_A + i));
			addOutput(createOutputCentered<PJ301MPort>(
				mm2px(Vec(xCV, y)), module, Shift::OUT_A + i));
			addOutput(createOutputCentered<PJ301MPort>(
				mm2px(Vec(xGate, y)), module, Shift::GATE_A + i));
		}

		// --- Jumble row at y=121.91, same output column positions ---
		addChild(createLightCentered<SmallLight<GreenLight>>(
			mm2px(Vec(xLED,  121.91f)), module, Shift::LIGHT_JUMBLE));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(xCV,   121.91f)), module, Shift::JUMBLE_CV));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(xGate, 121.91f)), module, Shift::JUMBLE_CLK));
	}

	void appendContextMenu(Menu* menu) override {
		Shift* m = dynamic_cast<Shift*>(this->module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Clear all", "",
			[m]() { m->clearAll(); }));
	}
};


Model* modelShift = createModel<Shift, ShiftWidget>("Shift");

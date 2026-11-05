// =============================================================================
// Hocket: one line dealt out across eight channels.
//
// A hocket splits a melody between voices, each playing some of its notes. One
// engine decides which channel takes the next note, and three lanes follow
// that decision: the pitch CV (sampled when the note starts, held after it
// ends, so a voice still releasing stays in tune), the gate (passed through to
// the selected channel while it is high) and an audio input (switched to the
// selected channel with a crossfade).
//
// The advance is the GATE's rising edge, or TRIG's when TRIG is patched, so
// the same module deals notes to eight voices or rotates audio round eight
// destinations on a clock. GATE is normalled from TRIG.
//
// The audio lane is a switch, not a hocket of voices: a single source has one
// tail, so a note's release and the next note's attack are already mixed
// together when the switch happens. Eight voices fed from the CV and gate
// lanes ring independently; eight effects fed from the audio lane do not.
//
// Which channels take part: the eight ON buttons, of which COUNT keeps the
// first N that are on. Design: docs/hocket-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

static const int HK_N = 8;

// THE PATTERNS, in knob order. Each picks the next channel from the ones in
// play; the order of a cycle is over those channels, not over all eight.
enum HkPattern { HK_UP, HK_DOWN, HK_PEND, HK_RAND, HK_NOREP, HK_SHUF, HK_STRIDE3, HK_STRIDE5,
                 HK_CONV, HK_REG, HK_DRUNK, HK_NPAT };
static const std::vector<std::string> HK_PAT_NAME = {
	"Up", "Down", "Pendulum", "Random", "Random, no repeat", "Shuffle", "Stride 3", "Stride 5",
	"Converge", "Register", "Drunk"};
static const char* HK_PAT_SHORT[HK_NPAT] = {
	"UP", "DOWN", "PENDULUM", "RANDOM", "NO REPEAT", "SHUFFLE", "STRIDE 3", "STRIDE 5",
	"CONVERGE", "REGISTER", "DRUNK"};

static const int HK_REG_HIST = 16;           // notes the Register pattern measures its range over
static const float HK_LEGATO_V = 1.f / 24.f;  // a pitch change of half a semitone under a held gate
static const int HK_RING = 128;               // spans kept for the screen

static int hkGcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

struct Hocket : Module {
	enum ParamId { PATTERN_PARAM, COUNT_PARAM, GROUP_PARAM, STAY_PARAM, FADE_PARAM,
	               ENUMS(ON_PARAM, HK_N), PARAMS_LEN };
	enum InputId { CV_INPUT, GATE_INPUT, AUDIO_L_INPUT, AUDIO_R_INPUT, TRIG_INPUT, RESET_INPUT,
	               PATTERN_INPUT, COUNT_INPUT, GROUP_INPUT, STAY_INPUT, FADE_INPUT, INPUTS_LEN };
	enum OutputId { ENUMS(CV_OUTPUT, HK_N), ENUMS(GATE_OUTPUT, HK_N), ENUMS(AUDIO_L_OUTPUT, HK_N), ENUMS(AUDIO_R_OUTPUT, HK_N), OUTPUTS_LEN };
	enum LightId { ENUMS(ON_LIGHT, HK_N * 3), LIGHTS_LEN };

	// ── the engine ──────────────────────────────────────────────────────
	int cur = -1;                 // the selected channel, -1 for none
	int groupLeft = 0;            // further notes the selected channel takes before handing on
	int pendDir = 1;              // Pendulum: which way it is travelling
	unsigned bag = 0;             // Shuffle: channels not yet dealt this cycle
	int strideBase = 0, strideStep = 0;   // Stride: which orbit, and how far along it
	int convC = 0;                // Converge: position in 0, N-1, 1, N-2 ...
	float regHist[HK_REG_HIST] = {};
	int regN = 0, regW = 0;       // Register: the last notes' pitches, a ring
	float noteCv = 0.f;           // the pitch the current note started at
	bool fromTop = false;         // RESET: the next note starts the pattern again

	// ── the lanes ───────────────────────────────────────────────────────
	float chCv[HK_N] = {};
	float gain[HK_N] = {};        // audio crossfade position per channel, 0..1
	bool gateWas[HK_N] = {};
	dsp::PulseGenerator retrig[HK_N];   // a 1 ms low before a note that stays on its channel
	dsp::SchmittTrigger gateTrig, trigTrig, resetTrig;
	dsp::ClockDivider lightDiv;

	// menu, saved
	bool cvTrack = true;          // CV follows the input while the gate is held
	bool legatoNotes = false;     // a new pitch under a held gate is a new note

	// counts the screen reads
	int nActive = 0;
	unsigned activeMask = 0, onMask = 0;
	int patNow = 0;

	// ── the screen's history: who was selected, and the notes, as spans ──
	struct Span { int8_t ch = -1; float v = 0.f; int64_t t0 = 0, t1 = -1; };
	Span selRing[HK_RING], noteRing[HK_RING];
	std::atomic<int> selHead{0}, noteHead{0};
	int selOpen = -1;
	int64_t selOpenT0 = 0;
	int noteOpen[HK_N];
	int64_t noteOpenT0[HK_N] = {};
	int64_t now = 0;
	float sr = 48000.f;

	Hocket() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configSwitch(PATTERN_PARAM, 0.f, (float)(HK_NPAT - 1), 0.f, "Pattern", HK_PAT_NAME);
		configParam(COUNT_PARAM, 1.f, (float)HK_N, (float)HK_N, "Channels in play (the first N that are on)");
		paramQuantities[COUNT_PARAM]->snapEnabled = true;
		configParam(GROUP_PARAM, 1.f, 8.f, 1.f, "Notes per channel before moving to the next");
		paramQuantities[GROUP_PARAM]->snapEnabled = true;
		configParam(STAY_PARAM, 0.f, 1.f, 0.f, "Chance the next note stays on the same channel", "%", 0.f, 100.f);
		// 0.5 ms to 1 s, exponential: display = 0.5 * 2000^v ms
		configParam(FADE_PARAM, 0.f, 1.f, std::log(4.f) / std::log(2000.f), "Audio crossfade at each switch", " ms", 2000.f, 0.5f);
		for (int c = 0; c < HK_N; c++) {
			configSwitch(ON_PARAM + c, 0.f, 1.f, 1.f, string::f("Channel %d", c + 1), {"Off", "On"});
			configOutput(CV_OUTPUT + c, string::f("Channel %d pitch", c + 1));
			configOutput(GATE_OUTPUT + c, string::f("Channel %d gate", c + 1));
			configOutput(AUDIO_L_OUTPUT + c, string::f("Channel %d audio left", c + 1));
			configOutput(AUDIO_R_OUTPUT + c, string::f("Channel %d audio right", c + 1));
			noteOpen[c] = -1;
		}
		configInput(CV_INPUT, "Pitch (1V/oct)");
		configInput(GATE_INPUT, "Gate (each rising edge sends a note to the next channel, unless TRIG is patched; normalled from TRIG)");
		configInput(AUDIO_L_INPUT, "Audio left");
		configInput(AUDIO_R_INPUT, "Audio right (normalled from left)");
		configInput(TRIG_INPUT, "Trigger (when patched, each trigger moves to the next channel)");
		configInput(RESET_INPUT, "Reset");
		configInput(PATTERN_INPUT, "Pattern CV (1V per pattern)");
		configInput(COUNT_INPUT, "Count CV (1V per channel)");
		configInput(GROUP_INPUT, "Group CV (1V per note)");
		configInput(STAY_INPUT, "Stay CV (10% per volt)");
		configInput(FADE_INPUT, "Fade CV (+-5V = the whole range)");
		lightDiv.setDivision(64);
	}

	// The pattern starts again at the next note. The note sounding now is left
	// alone: cutting it would be a reset you could hear.
	void doReset() {
		fromTop = true; groupLeft = 0; pendDir = 1; bag = 0;
		strideBase = strideStep = 0; convC = 0; regN = 0; regW = 0;
	}
	void onReset() override { doReset(); cur = -1; }

	// The channels in play, in channel order: those switched on, the first
	// COUNT of them.
	int activeList(int* act) {
		int count = clamp((int)std::round(params[COUNT_PARAM].getValue() + inputs[COUNT_INPUT].getVoltage()), 1, HK_N);
		int n = 0;
		onMask = 0; activeMask = 0;
		for (int c = 0; c < HK_N; c++) {
			if (params[ON_PARAM + c].getValue() < 0.5f) continue;
			onMask |= 1u << c;
			if (n < count) { act[n++] = c; activeMask |= 1u << c; }
		}
		nActive = n;
		return n;
	}

	// The next channel by the pattern, from the channels in play.
	int pick(int pat, const int* act, int N, float v) {
		if (N < 1 || N > HK_N) return cur;
		int idx = -1;
		for (int j = 0; j < N; j++) if (act[j] == cur) idx = j;
		// the first channel in play above `cur`, and the last below it
		auto above = [&]() { for (int j = 0; j < N; j++) if (act[j] > cur) return act[j]; return -1; };
		auto below = [&]() { for (int j = N - 1; j >= 0; j--) if (act[j] < cur) return act[j]; return -1; };
		auto rnd = [&](int n) { return clamp((int)(random::uniform() * n), 0, std::max(n - 1, 0)); };
		switch (pat) {
			case HK_UP:   { int a = above(); return a >= 0 ? a : act[0]; }
			case HK_DOWN: { int b = (cur < 0) ? -1 : below(); return b >= 0 ? b : act[N - 1]; }
			case HK_PEND: {
				// turn at the ends without playing the end twice
				if (N == 1) return act[0];
				int a = above(), b = (cur < 0) ? -1 : below();
				if (pendDir > 0) { if (a >= 0) return a; pendDir = -1; return b >= 0 ? b : act[0]; }
				if (b >= 0) return b;
				pendDir = 1; return a >= 0 ? a : act[N - 1];
			}
			case HK_RAND: return act[rnd(N)];
			case HK_NOREP: {
				if (N == 1) return act[0];
				if (idx < 0) return act[rnd(N)];
				int r = rnd(N - 1);
				return act[r >= idx ? r + 1 : r];
			}
			case HK_SHUF: {
				// every channel once per cycle, in a new order each cycle; the
				// first of a new cycle is never the last of the old
				bag &= activeMask;
				bool fresh = (bag == 0);
				if (fresh) bag = activeMask;
				unsigned cand = bag;
				if (fresh && N > 1 && idx >= 0) cand &= ~(1u << cur);
				int nc = 0;
				for (int j = 0; j < N; j++) if (cand & (1u << act[j])) nc++;
				int k = rnd(nc);
				for (int j = 0; j < N; j++) {
					if (!(cand & (1u << act[j]))) continue;
					if (k-- == 0) { bag &= ~(1u << act[j]); return act[j]; }
				}
				return act[0];
			}
			case HK_STRIDE3: case HK_STRIDE5: {
				// k steps at a time over the channels in play. Where k and N share
				// a factor the stride closes on a subset (3 over 6 is 1 4 1 4), so
				// each time an orbit closes it moves on by one and every channel
				// is reached.
				int k = (pat == HK_STRIDE3) ? 3 : 5;
				int g = hkGcd(k, N), L = N / g;
				strideBase %= g; strideStep %= L;
				int j = (strideBase + strideStep * k) % N;
				if (++strideStep >= L) { strideStep = 0; strideBase = (strideBase + 1) % g; }
				return act[j];
			}
			case HK_CONV: {
				// from the outside in: 1 8 2 7 3 6 4 5
				if (convC >= N) convC = 0;
				int j = (convC % 2 == 0) ? convC / 2 : N - 1 - convC / 2;
				convC = (convC + 1) % N;
				return act[j];
			}
			case HK_REG: {
				// low notes to low channels, over the range of the recent notes
				int n = std::min(regN, HK_REG_HIST);
				float lo = v, hi = v;
				for (int i = 0; i < n; i++) { lo = std::min(lo, regHist[i]); hi = std::max(hi, regHist[i]); }
				if (hi - lo < 1e-3f) return act[(N - 1) / 2];
				return act[clamp((int)((v - lo) / (hi - lo) * N), 0, N - 1)];
			}
			case HK_DRUNK: {
				if (N == 1) return act[0];
				if (idx < 0) return act[rnd(N)];
				int j = idx + (random::uniform() < 0.5f ? -1 : 1);
				if (j < 0) j = 1;
				if (j >= N) j = N - 2;
				return act[j];
			}
		}
		return act[0];
	}

	// A note arrives: decide who takes it.
	void advance(float v) {
		int act[HK_N];
		int N = activeList(act);
		int pat = clamp((int)std::round(params[PATTERN_PARAM].getValue() + inputs[PATTERN_INPUT].getVoltage()), 0, HK_NPAT - 1);
		int group = clamp((int)std::round(params[GROUP_PARAM].getValue() + inputs[GROUP_INPUT].getVoltage()), 1, 8);
		float stay = clamp(params[STAY_PARAM].getValue() + inputs[STAY_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		regHist[regW] = v; regW = (regW + 1) % HK_REG_HIST; regN++;
		if (fromTop) { cur = -1; fromTop = false; }
		if (N == 0) { cur = -1; return; }
		bool curIn = cur >= 0 && (activeMask & (1u << cur));
		if (curIn && groupLeft > 0) { groupLeft--; return; }
		if (curIn && stay > 0.f && random::uniform() < stay) return;
		cur = pick(pat, act, N, v);
		groupLeft = group - 1;
	}

	// ── the screen's spans ──────────────────────────────────────────────
	int openSpan(Span* ring, std::atomic<int>& head, int ch, float v) {
		int h = head.load();
		int i = h % HK_RING;
		ring[i].t1 = -1; ring[i].t0 = now; ring[i].v = v; ring[i].ch = (int8_t)ch;
		head.store(h + 1);
		return i;
	}
	void closeSpan(Span* ring, int i, int64_t t0) {
		if (i >= 0 && ring[i].t0 == t0 && ring[i].t1 < 0) ring[i].t1 = now;
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;
		sr = args.sampleRate;
		now++;
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) doReset();

		bool trigMode = inputs[TRIG_INPUT].isConnected();
		float trigV = inputs[TRIG_INPUT].getVoltage();
		float gIn = inputs[GATE_INPUT].isConnected() ? inputs[GATE_INPUT].getVoltage() : trigV;
		bool gateRise = gateTrig.process(gIn, 0.1f, 1.f);
		bool gateHigh = gateTrig.isHigh();
		bool trigRise = trigTrig.process(trigV, 0.1f, 1.f);
		float cv = inputs[CV_INPUT].getVoltage();

		bool note = trigMode ? trigRise : gateRise;
		if (!note && legatoNotes && !trigMode && gateHigh && std::fabs(cv - noteCv) > HK_LEGATO_V) note = true;
		// a channel switched off lets go at once rather than at the next note
		if (cur >= 0 && params[ON_PARAM + cur].getValue() < 0.5f) cur = -1;
		int prev = cur;
		if (note) {
			advance(cv);
			noteCv = cv;
			if (cur >= 0) {
				chCv[cur] = cv;
				// a new note on the channel already holding a high gate gets a
				// gap, or the voice behind it would never hear it start
				if (cur == prev && gateHigh && !gateRise) retrig[cur].trigger(1e-3f);
			}
		} else {
			int act[HK_N];
			activeList(act);
		}
		patNow = clamp((int)std::round(params[PATTERN_PARAM].getValue() + inputs[PATTERN_INPUT].getVoltage()), 0, HK_NPAT - 1);
		if (cur != prev) {
			closeSpan(selRing, selOpen, selOpenT0);
			selOpen = -1;
			if (cur >= 0) { selOpen = openSpan(selRing, selHead, cur, cv); selOpenT0 = now; }
		}

		float fade = clamp(params[FADE_PARAM].getValue() + inputs[FADE_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float fadeSec = 0.0005f * std::pow(2000.f, fade);
		float step = dt / fadeSec;
		float audioL = inputs[AUDIO_L_INPUT].getVoltage();
		float audioR = inputs[AUDIO_R_INPUT].isConnected() ? inputs[AUDIO_R_INPUT].getVoltage() : audioL;
		for (int c = 0; c < HK_N; c++) {
			bool sel = (c == cur);
			if (sel && gateHigh && cvTrack) chCv[c] = cv;
			bool gap = retrig[c].process(dt);
			bool g = sel && gateHigh && !gap;
			outputs[CV_OUTPUT + c].setVoltage(chCv[c]);
			outputs[GATE_OUTPUT + c].setVoltage(g ? 10.f : 0.f);
			if (g && !gateWas[c]) { noteOpen[c] = openSpan(noteRing, noteHead, c, chCv[c]); noteOpenT0[c] = now; }
			if (!g && gateWas[c]) { closeSpan(noteRing, noteOpen[c], noteOpenT0[c]); noteOpen[c] = -1; }
			gateWas[c] = g;
			float target = sel ? 1.f : 0.f;
			if (gain[c] < target) gain[c] = std::min(target, gain[c] + step);
			else if (gain[c] > target) gain[c] = std::max(target, gain[c] - step);
			float x = gain[c];
			float k = x * x * (3.f - 2.f * x);
			outputs[AUDIO_L_OUTPUT + c].setVoltage(audioL * k);
			outputs[AUDIO_R_OUTPUT + c].setVoltage(audioR * k);
		}

		if (lightDiv.process()) {
			for (int c = 0; c < HK_N; c++) {
				float r = 0.f, gr = 0.f, b = 0.f;
				if (c == cur) { r = 0.93f; gr = 0.40f; b = 0.18f; }                      // selected: orange
				else if (activeMask & (1u << c)) { gr = 0.59f * 0.8f; b = 0.87f * 0.8f; } // in play: blue
				else if (onMask & (1u << c)) { gr = 0.59f * 0.15f; b = 0.87f * 0.15f; }   // on, beyond COUNT
				lights[ON_LIGHT + 3 * c + 0].setBrightness(r);
				lights[ON_LIGHT + 3 * c + 1].setBrightness(gr);
				lights[ON_LIGHT + 3 * c + 2].setBrightness(b);
			}
		}
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "cvTrack", json_boolean(cvTrack));
		json_object_set_new(r, "legatoNotes", json_boolean(legatoNotes));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "cvTrack")) cvTrack = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "legatoNotes")) legatoNotes = json_boolean_value(j);
	}
};

// =============================================================================
// The screen: the pattern and how many channels are in play, then one lane per
// channel in the same order as the rows of outputs, with the last four seconds
// scrolling in from the right. A dim band is how long a channel was selected;
// a bar is a note it played, orange while it sounds.
// =============================================================================
static Hocket* hocketPreview() {
	static Hocket* m = nullptr;
	if (m) return m;
	m = new Hocket();
	sfs::previewConnect(m->inputs[Hocket::CV_INPUT], 1);
	sfs::previewConnect(m->inputs[Hocket::GATE_INPUT], 1);
	m->params[Hocket::PATTERN_PARAM].setValue((float)HK_SHUF);
	m->params[Hocket::COUNT_PARAM].setValue(6.f);
	m->params[Hocket::ON_PARAM + 2].setValue(0.f);
	static const float MEL[8] = {0.f, 3.f / 12, 7.f / 12, 10.f / 12, 1.f, 10.f / 12, 7.f / 12, 5.f / 12};
	rack::engine::Module::ProcessArgs a = sfs::previewArgs();
	int64_t frame = 0;
	int n = (int)(3.9f * sfs::PREVIEW_SR), period = 12000;   // eighth notes at 120 BPM
	for (int i = 0; i < n; i++) {
		int k = i / period;
		m->inputs[Hocket::CV_INPUT].setVoltage(MEL[k % 8]);
		m->inputs[Hocket::GATE_INPUT].setVoltage((i % period) >= period * 4 / 10 ? 10.f : 0.f);
		a.frame = frame++;
		m->process(a);
	}
	return m;
}

static const float HK_DESIGN_W = 154.f;   // the screen's 40.64 mm x 3.78
static const float HK_WINDOW_S = 4.f;     // seconds of history across the screen

struct HocketDisplay : Widget {
	Hocket* module = nullptr;
	std::shared_ptr<Font> font;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, mm2px(1.27f));
		nvgFillColor(args.vg, sfs::SCREEN_BG);
		nvgFill(args.vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) return;
		if (!module) { module = hocketPreview(); drawLayer(args, layer); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		bool hasFont = font && font->handle >= 0;
		float W = box.size.x, H = box.size.y, s = W / HK_DESIGN_W;
		Hocket* m = module;

		// header
		if (hasFont) {
			sfs::screenFont(vg, font);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			nvgText(vg, 7.f * s, 9.f * s, HK_PAT_SHORT[clamp(m->patNow, 0, HK_NPAT - 1)], NULL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, W - 7.f * s, 9.f * s, string::f("%d CH", m->nActive).c_str(), NULL);
		}
		nvgBeginPath(vg);
		nvgRect(vg, 7.f * s, 16.f * s, W - 14.f * s, 1.f * s);
		nvgFillColor(vg, sfs::SCREEN_LINE);
		nvgFill(vg);

		// lanes
		float top = 20.f * s, bot = H - 6.f * s;
		float laneH = (bot - top) / HK_N;
		float x0 = 16.f * s, x1 = W - 7.f * s;
		int64_t now = m->now;
		double win = (double)HK_WINDOW_S * m->sr;
		auto xOf = [&](int64_t t) { return x1 - (float)((double)(now - t) / win) * (x1 - x0); };
		for (int c = 0; c < HK_N; c++) {
			float y = top + laneH * c;
			bool inPlay = m->activeMask & (1u << c);
			nvgBeginPath(vg);
			nvgRect(vg, x0, y + laneH - 0.6f * s, x1 - x0, 0.6f * s);
			nvgFillColor(vg, inPlay ? sfs::SCREEN_PURP : nvgRGB(0x22, 0x22, 0x3A));
			nvgFill(vg);
			if (hasFont) {
				sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, c == m->cur ? sfs::SCREEN_HOT : inPlay ? sfs::SCREEN_DIM : sfs::SCREEN_PURP);
				nvgText(vg, 9.f * s, y + laneH * 0.5f, string::f("%d", c + 1).c_str(), NULL);
			}
		}
		nvgSave(vg);
		nvgScissor(vg, x0, top, x1 - x0, bot - top);
		auto drawRing = [&](const Hocket::Span* ring, bool notes) {
			for (int k = 0; k < HK_RING; k++) {
				const Hocket::Span& sp = ring[k];
				if (sp.ch < 0 || sp.ch >= HK_N) continue;
				int64_t t1 = sp.t1 < 0 ? now : sp.t1;
				if ((double)(now - t1) > win) continue;
				float xa = xOf(sp.t0), xb = std::max(xOf(t1), xa + 1.2f * s);
				float y = top + laneH * sp.ch;
				nvgBeginPath(vg);
				if (notes) nvgRect(vg, xa, y + laneH * 0.2f, xb - xa, laneH * 0.6f);
				else nvgRect(vg, xa, y + laneH * 0.08f, xb - xa, laneH * 0.84f);
				nvgFillColor(vg, !notes ? nvgRGB(0x24, 0x30, 0x5C)
				                 : sp.t1 < 0 ? sfs::SCREEN_HOT : sfs::SCREEN_BLUE);
				nvgFill(vg);
			}
		};
		drawRing(m->selRing, false);
		drawRing(m->noteRing, true);
		nvgRestore(vg);
		Widget::drawLayer(args, layer);
	}
};

// =============================================================================
// Panel: 22HP, code-drawn until the Figma pass. The screen at the top left
// with the controls under it: four rows of four, the pot and CV pairs with
// TRIG and RESET beside FADE, then the signal inputs at the foot. The eight
// channels are rows down the right, each an ON button that lights with its
// state and the four outs (CV, GATE, audio L and R) on a plate.
// =============================================================================
static const float HK_SX = 5.08f, HK_SY = 10.16f, HK_SW = 40.64f, HK_SH = 40.64f;
static const float HK_LX[4] = {10.16f, 20.32f, 30.48f, 40.64f};
static const float HK_Y_P1 = 64.77f, HK_Y_P2 = 78.74f, HK_Y_P3 = 92.71f, HK_Y_IN = 120.65f;
static const float HK_RULE_Y = 99.06f;
static const float HK_X_NUM = 55.88f, HK_X_ON = 60.96f, HK_X_CV = 71.12f, HK_X_GATE = 81.28f;
static const float HK_X_L = 91.44f, HK_X_R = 101.6f;
static const float HK_RY0 = 22.86f, HK_RPITCH = 13.97f;
static const float HK_HEAD_Y = 17.06f;

struct HocketWidget : ModuleWidget {
	HocketWidget(Hocket* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/hocket.svg")));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("HOCKET");

		HocketDisplay* disp = new HocketDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(HK_SX, HK_SY));
		disp->box.size = mm2px(Vec(HK_SW, HK_SH));
		addChild(disp);

		// pot and CV pairs, written out so tools/panel_reticules.py can read them
		addParam(createParamCentered<Trimpot>(mm2px(Vec(HK_LX[0], HK_Y_P1)), module, Hocket::PATTERN_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HK_LX[1], HK_Y_P1)), module, Hocket::PATTERN_INPUT));
		lbl->pair(HK_LX[0], HK_Y_P1, "PATTERN");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(HK_LX[2], HK_Y_P1)), module, Hocket::COUNT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HK_LX[3], HK_Y_P1)), module, Hocket::COUNT_INPUT));
		lbl->pair(HK_LX[2], HK_Y_P1, "COUNT");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(HK_LX[0], HK_Y_P2)), module, Hocket::GROUP_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HK_LX[1], HK_Y_P2)), module, Hocket::GROUP_INPUT));
		lbl->pair(HK_LX[0], HK_Y_P2, "GROUP");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(HK_LX[2], HK_Y_P2)), module, Hocket::STAY_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HK_LX[3], HK_Y_P2)), module, Hocket::STAY_INPUT));
		lbl->pair(HK_LX[2], HK_Y_P2, "STAY");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(HK_LX[0], HK_Y_P3)), module, Hocket::FADE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HK_LX[1], HK_Y_P3)), module, Hocket::FADE_INPUT));
		lbl->pair(HK_LX[0], HK_Y_P3, "FADE");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HK_LX[2], HK_Y_P3)), module, Hocket::TRIG_INPUT));
		lbl->jack(HK_LX[2], HK_Y_P3, "TRIG");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HK_LX[3], HK_Y_P3)), module, Hocket::RESET_INPUT));
		lbl->jack(HK_LX[3], HK_Y_P3, "RESET");
		lbl->divider(HK_SX, HK_RULE_Y, HK_SX + HK_SW, HK_RULE_Y);

		// the inputs
		const int ins[4] = {Hocket::CV_INPUT, Hocket::GATE_INPUT, Hocket::AUDIO_L_INPUT, Hocket::AUDIO_R_INPUT};
		const char* inName[4] = {"CV", "GATE", "IN L", "IN R"};
		for (int i = 0; i < 4; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(HK_LX[i], HK_Y_IN)), module, ins[i]));
			lbl->jack(HK_LX[i], HK_Y_IN, inName[i]);
		}

		// the channels
		lbl->add(HK_X_ON, HK_HEAD_Y, "ON");
		lbl->add(HK_X_CV, HK_HEAD_Y, "CV", sfs::PanelLabels::ON_PLATE);
		lbl->add(HK_X_GATE, HK_HEAD_Y, "GATE", sfs::PanelLabels::ON_PLATE);
		lbl->add(HK_X_L, HK_HEAD_Y, "L", sfs::PanelLabels::ON_PLATE);
		lbl->add(HK_X_R, HK_HEAD_Y, "R", sfs::PanelLabels::ON_PLATE);
		for (int c = 0; c < HK_N; c++) {
			float y = HK_RY0 + HK_RPITCH * c;
			lbl->add(HK_X_NUM, y + 0.83f, string::f("%d", c + 1));
			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedGreenBlueLight>>>(
				mm2px(Vec(HK_X_ON, y)), module, Hocket::ON_PARAM + c, Hocket::ON_LIGHT + 3 * c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(HK_X_CV, y)), module, Hocket::CV_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(HK_X_GATE, y)), module, Hocket::GATE_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(HK_X_L, y)), module, Hocket::AUDIO_L_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(HK_X_R, y)), module, Hocket::AUDIO_R_OUTPUT + c));
		}
	}

	void appendContextMenu(Menu* menu) override {
		Hocket* m = dynamic_cast<Hocket*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("CV follows the input while the gate is held", "", &m->cvTrack));
		menu->addChild(createBoolPtrMenuItem("A new pitch under a held gate is a new note", "", &m->legatoNotes));
	}
};

Model* modelHocket = createModel<Hocket, HocketWidget>("Hocket");

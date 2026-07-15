#include "plugin.hpp"
#include "dr_wav.h"        // implementation lives in phase.cpp; headers only here
#include "scales.hpp"      // canonical sfs::SCALES (shared) — for the In-Key grid
#include <osdialog.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstdio>

// ─── Play — polyphonic multisample (SFZ) player ──────────────────────────────
// Loads SFZ instruments (e.g. those written by Record, or simple third-party
// SFZ) and plays them polyphonically: V/OCT + GATE + VELOCITY (poly) pick a
// region by key & velocity, pitch-shift the sample, and mix to stereo. Several
// instruments can be loaded and switched by CV.

static const int PLAY_MAX_VOICES = 16;
static const int PKB_LO = 21, PKB_HI = 108;
static const char* PLAY_NOTES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static inline bool playIsWhite(int n) {
	int s = ((n % 12) + 12) % 12;
	return s == 0 || s == 2 || s == 4 || s == 5 || s == 7 || s == 9 || s == 11;
}
static inline int playWhiteBefore(int n) { int c = 0; for (int m = PKB_LO; m < n; m++) if (playIsWhite(m)) c++; return c; }

// Push-style isomorphic pad grid — shared helpers (GRID_COLS/ROWS, gridNoteAt, …)
#include "pushgrid.hpp"

struct SfzRegion {
	std::string sample;
	int   lokey = 0, hikey = 127, keycenter = 60, lovel = 0, hivel = 127;
	float tuneCents = 0.f, volumeDb = 0.f;
	int   loopMode = 0;              // 0 = one-shot / no loop, 1 = loop
	long  loopStart = -1, loopEnd = -1;
	int   seqLength = 1, seqPos = 1; // round-robin
	// loaded audio
	std::vector<float> L, R;
	long  frames = 0; float srcRate = 48000.f; bool loaded = false;
	float volGain = 1.f;
};

struct Instrument {
	std::string name, dir, srcPath;
	std::vector<SfzRegion> regions;
};

// ─── SFZ parsing ─────────────────────────────────────────────────────────────
static void sfzStripComments(std::string& s) {
	for (size_t i = 0; i + 1 < s.size(); i++)
		if (s[i] == '/' && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') s[i++] = ' '; }
}
static int sfzKeyNum(const std::string& v) {
	if (v.empty()) return -1;
	if (isdigit((unsigned char)v[0]) || v[0] == '-') return atoi(v.c_str());
	// note name like c4, f#3, db2
	int step[7] = {9, 11, 0, 2, 4, 5, 7};   // a b c d e f g
	int c = tolower(v[0]); if (c < 'a' || c > 'g') return -1;
	int semi = step[c - 'a']; size_t i = 1;
	if (i < v.size() && (v[i] == '#' || v[i] == 's')) { semi++; i++; }
	else if (i < v.size() && v[i] == 'b') { semi--; i++; }
	int oct = (i < v.size()) ? atoi(v.c_str() + i) : 4;
	return clamp(semi + (oct + 1) * 12, 0, 127);   // c4 = 60
}

static bool parseSfz(const std::string& path, Instrument& inst) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	std::string text(n, 0); if (std::fread(&text[0], 1, n, f) != (size_t)n) {} std::fclose(f);
	sfzStripComments(text);

	inst.dir = system::getDirectory(path);
	inst.name = system::getStem(path);
	std::string defaultPath;
	std::map<std::string, std::string> gGlobal, gGroup, gRegion;
	bool inRegion = false;

	auto geti = [](std::map<std::string, std::string>& m, const char* k, int d) {
		auto it = m.find(k); return it == m.end() ? d : atoi(it->second.c_str()); };
	auto getf = [](std::map<std::string, std::string>& m, const char* k, float d) {
		auto it = m.find(k); return it == m.end() ? d : (float)atof(it->second.c_str()); };

	auto flush = [&]() {
		if (!inRegion) return;
		std::map<std::string, std::string> m = gGlobal;
		for (auto& kv : gGroup) m[kv.first] = kv.second;
		for (auto& kv : gRegion) m[kv.first] = kv.second;
		SfzRegion r;
		if (m.count("sample")) r.sample = m["sample"];
		if (m.count("key")) { int k = sfzKeyNum(m["key"]); r.lokey = r.hikey = r.keycenter = k; }
		if (m.count("lokey")) r.lokey = sfzKeyNum(m["lokey"]);
		if (m.count("hikey")) r.hikey = sfzKeyNum(m["hikey"]);
		if (m.count("pitch_keycenter")) r.keycenter = sfzKeyNum(m["pitch_keycenter"]);
		r.lovel = geti(m, "lovel", 0); r.hivel = geti(m, "hivel", 127);
		r.tuneCents = getf(m, "tune", 0.f); r.volumeDb = getf(m, "volume", 0.f);
		r.volGain = std::pow(10.f, r.volumeDb / 20.f);
		std::string lm = m.count("loop_mode") ? m["loop_mode"] : "";
		r.loopMode = (lm == "loop_continuous" || lm == "loop_sustain") ? 1 : 0;
		r.loopStart = m.count("loop_start") ? atol(m["loop_start"].c_str()) : -1;
		r.loopEnd   = m.count("loop_end")   ? atol(m["loop_end"].c_str())   : -1;
		r.seqLength = geti(m, "seq_length", 1); r.seqPos = geti(m, "seq_position", 1);
		if (!r.sample.empty()) inst.regions.push_back(std::move(r));
	};

	// tokenize on whitespace. A token WITHOUT '=' (and not a <header>) is a
	// continuation of the previous opcode's value — this is how SFZ allows spaces
	// in a value, most importantly sample paths (e.g. `sample=My Long Pad.wav`).
	size_t i = 0; int ctx = 0;   // 1 global, 2 group, 3 region, 4 control
	std::string* curVal = nullptr;
	while (i < text.size()) {
		while (i < text.size() && isspace((unsigned char)text[i])) i++;
		if (i >= text.size()) break;
		size_t j = i; while (j < text.size() && !isspace((unsigned char)text[j])) j++;
		std::string tok = text.substr(i, j - i); i = j;
		if (tok[0] == '<') {
			curVal = nullptr;
			if (tok == "<region>") { flush(); gRegion.clear(); inRegion = true; ctx = 3; }
			else if (tok == "<group>") { flush(); inRegion = false; gGroup.clear(); ctx = 2; }
			else if (tok == "<global>") { flush(); inRegion = false; gGlobal.clear(); ctx = 1; }
			else { flush(); inRegion = false; ctx = 4; }
		} else {
			size_t eq = tok.find('=');
			if (eq == std::string::npos) {   // continuation (value contained a space)
				if (curVal) { *curVal += ' '; *curVal += tok; }
				continue;
			}
			std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
			curVal = nullptr;
			if (ctx == 4 && k == "default_path") { defaultPath = v; curVal = &defaultPath; }
			else if (ctx == 1) { gGlobal[k] = v; curVal = &gGlobal[k]; }
			else if (ctx == 2) { gGroup[k] = v; curVal = &gGroup[k]; }
			else if (ctx == 3) { gRegion[k] = v; curVal = &gRegion[k]; }
		}
	}
	flush();
	if (!defaultPath.empty()) for (auto& r : inst.regions) r.sample = defaultPath + r.sample;
	return !inst.regions.empty();
}

// ─── DecentSampler (.dspreset) parsing ───────────────────────────────────────
// XML: <DecentSampler><groups ...><group ...><sample path= rootNote= loNote= .../>.
// Attributes cascade groups→group→sample; tuning and volume accumulate across
// levels (DecentSampler convention), everything else overrides. Mapped onto the
// same SfzRegion so the voice engine is shared.
static std::map<std::string, std::string> dsAttrs(const std::string& s) {
	std::map<std::string, std::string> m;
	size_t i = 0;
	while (i < s.size()) {
		while (i < s.size() && !(isalpha((unsigned char)s[i]) || s[i] == '_')) i++;
		if (i >= s.size()) break;
		size_t j = i;
		while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '-' || s[j] == ':')) j++;
		std::string key = s.substr(i, j - i);
		size_t k = j; while (k < s.size() && isspace((unsigned char)s[k])) k++;
		if (k >= s.size() || s[k] != '=') { i = j + 1; continue; }
		k++; while (k < s.size() && isspace((unsigned char)s[k])) k++;
		if (k >= s.size()) break;
		char q = s[k];
		if (q == '"' || q == '\'') {
			k++; size_t e = k; while (e < s.size() && s[e] != q) e++;
			m[key] = s.substr(k, e - k); i = e + 1;
		} else {
			size_t e = k; while (e < s.size() && !isspace((unsigned char)s[e]) && s[e] != '/' && s[e] != '>') e++;
			m[key] = s.substr(k, e - k); i = e;
		}
	}
	return m;
}
static float dsVolToDb(const std::string& v) {
	if (v.empty()) return 0.f;
	size_t n = v.size();
	if (n >= 2 && (v[n - 1] == 'B' || v[n - 1] == 'b') && (v[n - 2] == 'd' || v[n - 2] == 'D'))
		return (float)atof(v.c_str());           // "-6dB"
	float g = (float)atof(v.c_str());             // linear gain
	return g > 0.f ? 20.f * std::log10(g) : 0.f;
}

static bool parseDecentSampler(const std::string& path, Instrument& inst) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	std::string text(n, 0); if (std::fread(&text[0], 1, n, f) != (size_t)n) {} std::fclose(f);
	for (size_t p = text.find("<!--"); p != std::string::npos; p = text.find("<!--")) {
		size_t e = text.find("-->", p);
		if (e == std::string::npos) { text.erase(p); break; }
		text.erase(p, e + 3 - p);
	}
	inst.dir = system::getDirectory(path);
	inst.name = system::getStem(path);

	std::map<std::string, std::string> gGlobal, gGroup;
	size_t i = 0;
	while ((i = text.find('<', i)) != std::string::npos) {
		if (text.compare(i, 2, "<?") == 0 || text.compare(i, 2, "<!") == 0) {
			size_t e = text.find('>', i); if (e == std::string::npos) break; i = e + 1; continue;
		}
		size_t end = text.find('>', i); if (end == std::string::npos) break;
		std::string tag = text.substr(i + 1, end - i - 1); i = end + 1;
		size_t ns = 0; while (ns < tag.size() && isspace((unsigned char)tag[ns])) ns++;
		bool closing = (ns < tag.size() && tag[ns] == '/'); if (closing) ns++;
		size_t ne = ns; while (ne < tag.size() && !isspace((unsigned char)tag[ne]) && tag[ne] != '/' && tag[ne] != '>') ne++;
		std::string name = tag.substr(ns, ne - ns), body = tag.substr(ne);

		if (closing) { if (name == "group") gGroup.clear(); continue; }
		if (name == "groups") { gGlobal = dsAttrs(body); continue; }
		if (name == "group") { gGroup = dsAttrs(body); continue; }
		if (name != "sample") continue;

		std::map<std::string, std::string> sa = dsAttrs(body);
		auto pick = [&](const char* k) -> std::string {
			auto it = sa.find(k); if (it != sa.end()) return it->second;
			it = gGroup.find(k); if (it != gGroup.end()) return it->second;
			it = gGlobal.find(k); if (it != gGlobal.end()) return it->second;
			return std::string();
		};
		std::string sp = pick("path"); if (sp.empty()) continue;
		SfzRegion r;
		r.sample = sp;
		int root = -1;
		{ std::string s = pick("rootNote"); if (s.empty()) s = pick("rootKey"); if (s.empty()) s = pick("pitchKeyCenter"); if (!s.empty()) root = sfzKeyNum(s); }
		std::string loS = pick("loNote"); if (loS.empty()) loS = pick("loKey");
		std::string hiS = pick("hiNote"); if (hiS.empty()) hiS = pick("hiKey");
		r.lokey = !loS.empty() ? sfzKeyNum(loS) : (root >= 0 ? root : 0);
		r.hikey = !hiS.empty() ? sfzKeyNum(hiS) : (root >= 0 ? root : 127);
		r.keycenter = root >= 0 ? root : r.lokey;
		{ std::string s = pick("loVel"); r.lovel = s.empty() ? 0 : atoi(s.c_str()); }
		{ std::string s = pick("hiVel"); r.hivel = s.empty() ? 127 : atoi(s.c_str()); }
		float tuning = 0.f, volDb = 0.f;
		std::map<std::string, std::string>* levels[3] = { &gGlobal, &gGroup, &sa };
		for (auto* mp : levels) {
			auto t = mp->find("tuning"); if (t != mp->end()) tuning += (float)atof(t->second.c_str());
			auto v = mp->find("volume"); if (v != mp->end()) volDb += dsVolToDb(v->second);
		}
		r.tuneCents = tuning * 100.f;
		r.volumeDb = volDb; r.volGain = std::pow(10.f, volDb / 20.f);
		{ std::string s = pick("loopEnabled"); r.loopMode = (s == "true" || s == "1") ? 1 : 0; }
		{ std::string s = pick("loopStart"); if (!s.empty()) r.loopStart = atol(s.c_str()); }
		{ std::string s = pick("loopEnd"); if (!s.empty()) r.loopEnd = atol(s.c_str()); }
		{ std::string s = pick("seqLength"); r.seqLength = s.empty() ? 1 : atoi(s.c_str()); }
		{ std::string s = pick("seqPosition"); r.seqPos = s.empty() ? 1 : atoi(s.c_str()); }
		inst.regions.push_back(std::move(r));
	}
	return !inst.regions.empty();
}

// A .dsbundle is a folder holding a .dspreset (+ a Samples/ folder). Return the
// path of the first .dspreset inside `dir` (searched one level deep), or "".
static std::string findDspresetIn(const std::string& dir) {
	for (const std::string& e : system::getEntries(dir)) {
		if (string::lowercase(system::getExtension(e)) == ".dspreset") return e;
	}
	for (const std::string& e : system::getEntries(dir)) {   // fall back: one subdir deep
		if (system::isDirectory(e)) {
			std::string p = findDspresetIn(e);
			if (!p.empty()) return p;
		}
	}
	return "";
}

static void loadRegionAudio(SfzRegion& r, const std::string& dir) {
	std::string p = system::join(dir, r.sample);
	unsigned int ch = 0, sr = 0; drwav_uint64 frames = 0;
	float* data = drwav_open_file_and_read_pcm_frames_f32(p.c_str(), &ch, &sr, &frames, NULL);
	if (!data || ch == 0) { if (data) drwav_free(data, NULL); return; }
	r.srcRate = (float)sr; r.frames = (long)frames;
	r.L.resize(frames); r.R.resize(frames);
	for (drwav_uint64 i = 0; i < frames; i++) {
		r.L[i] = data[i * ch];
		r.R[i] = (ch > 1) ? data[i * ch + 1] : data[i * ch];
	}
	drwav_free(data, NULL);
	if (r.loopEnd <= 0 || r.loopEnd > r.frames) r.loopEnd = r.frames;
	if (r.loopStart < 0) r.loopStart = 0;
	r.loaded = true;
}

// ─── Module ──────────────────────────────────────────────────────────────────
struct Play;
struct PlayDisplay : OpaqueWidget {
	Play* module = nullptr;
	std::shared_ptr<Font> font;
	bool dragging = false;
	Vec dragPos;

	void drawLayer(const DrawArgs& args, int layer) override;
	void drawKeys(NVGcontext* vg, float x, float y, float w, float h, const uint8_t* mapped, const uint8_t* root, const uint8_t* playing);
	void drawTabs(NVGcontext* vg, int view);
	void drawGrid(NVGcontext* vg, const uint8_t* mapped, const uint8_t* root, const uint8_t* playing,
	              int layout, int gRoot, int gScale, int gBase);

	// Geometry (px, widget-local) — shared by draw + hit-test
	void tabRects(Rect out[2]) const {
		float tw = (box.size.x - 12.f - 4.f) * 0.5f;
		out[0] = Rect(Vec(6.f, 4.f), Vec(tw, 15.f));
		out[1] = Rect(Vec(6.f + tw + 4.f, 4.f), Vec(tw, 15.f));
	}
	void gridGeom(float& x0, float& y0, float& pad) const {
		float top = 32.f, marg = 6.f;
		float aw = box.size.x - 2.f * marg, ah = box.size.y - 6.f - top;
		pad = std::min(aw / GRID_COLS, ah / GRID_ROWS);
		x0 = (box.size.x - pad * GRID_COLS) * 0.5f;
		y0 = top + std::max(0.f, (ah - pad * GRID_ROWS) * 0.5f);
	}
	int hitTab(Vec p) const { Rect r[2]; tabRects(r); for (int i = 0; i < 2; i++) if (r[i].contains(p)) return i; return -1; }
	int hitPad(Vec p) const;      // grid pad under cursor → MIDI note or -1
	int pianoNoteAt(Vec p) const; // piano key under cursor → MIDI note or -1

	void onButton(const ButtonEvent& e) override;
	void onDragStart(const DragStartEvent& e) override { OpaqueWidget::onDragStart(e); }
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;
};

struct Play : Module {
	enum ParamId { INSTR_PARAM, LEVEL_PARAM, PARAMS_LEN };
	enum InputId { VOCT_INPUT, GATE_INPUT, VEL_INPUT, INSTR_CV_INPUT, LEVEL_CV_INPUT, INPUTS_LEN };
	enum OutputId { L_OUTPUT, R_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	struct Voice {
		bool active = false, held = false;
		int chan = -1, instr = -1, reg = -1, note = 60;
		double pos = 0, ratio = 1; float amp = 1.f, env = 0.f;
	};
	Voice voices[PLAY_MAX_VOICES];
	std::vector<Instrument> instruments;
	int curInstrument = 0;
	int rrCounter = 0;                       // rotates through round-robin takes
	bool oneShot = false;                   // true = play samples through, ignoring gate-off (drums)
	bool suspended = false;                 // true while (re)loading on the GUI thread
	bool gateWas[PLAY_MAX_VOICES] = {};

	// Keyboard surface view (tab-switched on the display) + Push grid config
	int kbView = 1;        // 0 = piano, 1 = grid (grid is the default)
	int gridLayout = 0;    // 0 = chromatic 4ths, 1 = in-key
	int gridRoot = 0;      // 0..11 (C..B) — key root / highlight
	int gridScale = 1;     // sfs::SCALES index (default Major)
	int gridBase = 36;     // MIDI note of the bottom-left pad (C2)
	// Mouse audition: widget writes uiNote (-1 = none), audio thread edge-detects
	int uiNote = -1, uiNotePrev = -1;

	// display mirrors
	char dispName[48] = {0}, dispInfo[48] = {0};
	uint8_t dispMapped[128] = {0}, dispRoot[128] = {0}, dispPlaying[128] = {0};
	int dispCount = 0, statusDiv = 0;

	Play() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(INSTR_PARAM, 0.f, 15.f, 0.f, "Instrument");
		getParamQuantity(INSTR_PARAM)->snapEnabled = true;
		configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Level");
		configInput(VOCT_INPUT, "V/oct (poly)");
		configInput(GATE_INPUT, "Gate (poly)");
		configInput(VEL_INPUT, "Velocity (poly)");
		configInput(INSTR_CV_INPUT, "Instrument select CV");
		configInput(LEVEL_CV_INPUT, "Level CV (VCA)");
		configOutput(L_OUTPUT, "Left");
		configOutput(R_OUTPUT, "Right");
		refreshDisplay();
	}

	SfzRegion* findRegion(int inst, int note, int vel) {
		if (inst < 0 || inst >= (int)instruments.size()) return nullptr;
		for (auto& r : instruments[inst].regions)
			if (r.loaded && note >= r.lokey && note <= r.hikey && vel >= r.lovel && vel <= r.hivel) return &r;
		return nullptr;
	}

	void noteOn(int chan, int note, int vel) {
		if (curInstrument < 0 || curInstrument >= (int)instruments.size()) return;
		auto& regs = instruments[curInstrument].regions;
		// collect every region matching this key+velocity (>1 = round-robin takes)
		std::vector<int> matches;
		for (int i = 0; i < (int)regs.size(); i++) {
			SfzRegion& r = regs[i];
			if (r.loaded && note >= r.lokey && note <= r.hikey && vel >= r.lovel && vel <= r.hivel) matches.push_back(i);
		}
		if (matches.empty()) return;
		std::sort(matches.begin(), matches.end(), [&](int a, int b) { return regs[a].seqPos < regs[b].seqPos; });
		int regIdx = matches[(matches.size() > 1) ? (rrCounter++ % (int)matches.size()) : 0];
		SfzRegion* r = &regs[regIdx];
		int slot = -1;
		for (int i = 0; i < PLAY_MAX_VOICES; i++) if (!voices[i].active) { slot = i; break; }
		if (slot < 0) { double best = -1; for (int i = 0; i < PLAY_MAX_VOICES; i++) if (voices[i].pos > best) { best = voices[i].pos; slot = i; } }
		Voice& v = voices[slot];
		v.active = true; v.held = true; v.chan = chan; v.instr = curInstrument; v.reg = regIdx; v.note = note;
		v.pos = 0; v.amp = clamp(vel / 127.f, 0.f, 1.f); v.env = 0.f;
		v.ratio = std::pow(2.0, (note - r->keycenter) / 12.0 + r->tuneCents / 1200.0) * (r->srcRate / APP->engine->getSampleRate());
	}

	void process(const ProcessArgs& args) override {
		float sel = params[INSTR_PARAM].getValue();
		if (inputs[INSTR_CV_INPUT].isConnected()) sel += inputs[INSTR_CV_INPUT].getVoltage() / 10.f * 15.f;
		curInstrument = clamp((int)std::round(sel), 0, std::max(0, (int)instruments.size() - 1));

		if (suspended || instruments.empty()) {
			outputs[L_OUTPUT].setVoltage(0.f); outputs[R_OUTPUT].setVoltage(0.f);
			return;
		}

		int nch = std::max(1, std::max(inputs[VOCT_INPUT].getChannels(), inputs[GATE_INPUT].getChannels()));
		for (int c = 0; c < nch; c++) {
			bool g = inputs[GATE_INPUT].getVoltage(c) >= 1.f;
			if (g && !gateWas[c]) {
				// getPolyVoltage → a mono V/oct or velocity cable applies to every voice
				int note = clamp((int)std::round(inputs[VOCT_INPUT].getPolyVoltage(c) * 12.f + 60.f), 0, 127);
				int vel  = inputs[VEL_INPUT].isConnected()
					? clamp((int)std::round(inputs[VEL_INPUT].getPolyVoltage(c) * 12.7f), 1, 127) : 100;
				noteOn(c, note, vel);
			} else if (!g && gateWas[c]) {
				for (auto& v : voices) if (v.active && v.chan == c) v.held = false;
			}
			gateWas[c] = g;
		}

		// Mouse audition from the on-screen keyboard / grid (reserved channel)
		if (uiNote != uiNotePrev) {
			for (auto& v : voices) if (v.active && v.chan == GRID_UI_CHAN) v.held = false;
			if (uiNote >= 0) noteOn(GRID_UI_CHAN, uiNote, 100);
			uiNotePrev = uiNote;
		}

		const float atkC = 1.f - std::exp(-1.f / (0.002f * args.sampleRate));
		const float relC = 1.f - std::exp(-1.f / (0.030f * args.sampleRate));
		float outL = 0.f, outR = 0.f;
		for (auto& v : voices) {
			if (!v.active) continue;
			Instrument& in = instruments[v.instr];
			if (v.reg < 0 || v.reg >= (int)in.regions.size()) { v.active = false; continue; }
			SfzRegion& r = in.regions[v.reg];
			long i0 = (long)v.pos;
			if (i0 < 0 || i0 >= r.frames - 1) { v.active = false; continue; }
			double f = v.pos - i0;
			float sl = r.L[i0] * (1 - f) + r.L[i0 + 1] * f;
			float sr = r.R[i0] * (1 - f) + r.R[i0 + 1] * f;
			// gate-controlled: note releases on gate-off. "One-shot" mode plays
			// the sample through, ignoring gate-off (for percussion / triggers).
			float targ = (v.held || oneShot) ? 1.f : 0.f;
			v.env += ((targ) - v.env) * (targ > v.env ? atkC : relC);
			if (targ == 0.f && v.env < 0.0008f) { v.active = false; continue; }
			float ggain = v.amp * v.env * r.volGain;
			outL += sl * ggain; outR += sr * ggain;
			v.pos += v.ratio;
			if (r.loopMode == 1 && v.held && r.loopEnd > r.loopStart && v.pos >= r.loopEnd)
				v.pos -= (r.loopEnd - r.loopStart);
		}
		float lvl = params[LEVEL_PARAM].getValue();
		if (inputs[LEVEL_CV_INPUT].isConnected())          // CV acts as an output VCA
			lvl *= std::max(0.f, inputs[LEVEL_CV_INPUT].getVoltage() * 0.1f);
		outputs[L_OUTPUT].setVoltage(clamp(outL * lvl * 5.f, -10.f, 10.f));
		outputs[R_OUTPUT].setVoltage(clamp(outR * lvl * 5.f, -10.f, 10.f));

		if (++statusDiv >= 256) { statusDiv = 0; refreshDisplay(); }
	}

	void refreshDisplay() {
		std::memset(dispMapped, 0, sizeof(dispMapped));
		std::memset(dispRoot, 0, sizeof(dispRoot));
		std::memset(dispPlaying, 0, sizeof(dispPlaying));
		if (curInstrument >= 0 && curInstrument < (int)instruments.size()) {
			Instrument& in = instruments[curInstrument];
			for (auto& r : in.regions) {
				for (int k = r.lokey; k <= r.hikey && k < 128; k++) if (k >= 0) dispMapped[k] = 1;
				if (r.keycenter >= 0 && r.keycenter < 128) dispRoot[r.keycenter] = 1;
			}
			snprintf(dispName, sizeof(dispName), "%d/%d %s", curInstrument + 1, (int)instruments.size(), in.name.c_str());
			int loaded = 0, tot = (int)in.regions.size();
			for (auto& r : in.regions) if (r.loaded) loaded++;
			if (loaded < tot) snprintf(dispInfo, sizeof(dispInfo), "%d/%d loaded", loaded, tot);
			else snprintf(dispInfo, sizeof(dispInfo), "%d regions", tot);
		} else { snprintf(dispName, sizeof(dispName), "no instrument"); snprintf(dispInfo, sizeof(dispInfo), "load .sfz"); }
		int cnt = 0;
		for (auto& v : voices) if (v.active) { cnt++; if (v.note >= 0 && v.note < 128) dispPlaying[v.note] = 1; }
		dispCount = cnt;
	}

	void loadInstrument(const std::string& path) {
		suspended = true;
		for (auto& v : voices) v.active = false;
		Instrument in;
		std::string ext = string::lowercase(system::getExtension(path));
		// A .dsbundle is a folder holding a .dspreset + samples — find the preset
		// inside and load that; sample paths resolve relative to the bundle.
		std::string presetPath = path;
		bool bundle = (ext == ".dsbundle") || system::isDirectory(path);
		if (bundle) presetPath = findDspresetIn(path);
		std::string pext = string::lowercase(system::getExtension(presetPath));
		bool ok = !presetPath.empty()
			&& ((pext == ".dspreset") ? parseDecentSampler(presetPath, in) : parseSfz(presetPath, in));
		if (ok) {
			in.srcPath = path;                    // persist what the user picked (bundle or file)
			if (bundle) in.name = system::getStem(path);
			int failed = 0;
			for (auto& r : in.regions) {
				loadRegionAudio(r, in.dir);
				if (!r.loaded) { failed++; WARN("Play: could not load sample \"%s\"", system::join(in.dir, r.sample).c_str()); }
			}
			if (failed) WARN("Play: %d/%d samples failed to load in \"%s\"", failed, (int)in.regions.size(), in.name.c_str());
			instruments.push_back(std::move(in));
		} else WARN("Play: failed to parse \"%s\"", path.c_str());
		suspended = false;
		refreshDisplay();
	}
	void removeInstrument(int idx) {
		if (idx < 0 || idx >= (int)instruments.size()) return;
		suspended = true;
		for (auto& v : voices) v.active = false;
		instruments.erase(instruments.begin() + idx);
		suspended = false;
		refreshDisplay();
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* arr = json_array();
		for (auto& in : instruments) json_array_append_new(arr, json_string(in.srcPath.c_str()));
		json_object_set_new(root, "sfzPaths", arr);
		json_object_set_new(root, "oneShot", json_boolean(oneShot));
		json_object_set_new(root, "kbView", json_integer(kbView));
		json_object_set_new(root, "gridLayout", json_integer(gridLayout));
		json_object_set_new(root, "gridRoot", json_integer(gridRoot));
		json_object_set_new(root, "gridScale", json_integer(gridScale));
		json_object_set_new(root, "gridBase", json_integer(gridBase));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "oneShot")) oneShot = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "kbView")) kbView = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(root, "gridLayout")) gridLayout = clamp((int)json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(root, "gridRoot")) gridRoot = clamp((int)json_integer_value(j), 0, 11);
		if (json_t* j = json_object_get(root, "gridScale")) gridScale = clamp((int)json_integer_value(j), 0, sfs::NUM_SCALES - 1);
		if (json_t* j = json_object_get(root, "gridBase")) gridBase = clamp((int)json_integer_value(j), 0, 115);
		json_t* arr = json_object_get(root, "sfzPaths");
		if (!arr) return;
		instruments.clear();
		size_t n = json_array_size(arr);
		for (size_t i = 0; i < n; i++) { json_t* p = json_array_get(arr, i); if (p) loadInstrument(json_string_value(p)); }
	}
};

// ─── Display ─────────────────────────────────────────────────────────────────
void PlayDisplay::drawKeys(NVGcontext* vg, float x, float y, float w, float h, const uint8_t* mapped, const uint8_t* root, const uint8_t* playing) {
	int NW = playWhiteBefore(PKB_HI + 1); float ww = w / NW;
	// bright blue = a directly-sampled root; dim blue = mapped but pitch-shifted; grey = unmapped
	NVGcolor cyan = nvgRGB(0x00, 0xc8, 0xff), blue = nvgRGB(0x00, 0x97, 0xde), gap = nvgRGB(0x1a, 0x1a, 0x2e);
	int k = 0;
	for (int m = PKB_LO; m <= PKB_HI; m++) {
		if (!playIsWhite(m)) continue;
		float x0 = x + k * ww;
		NVGcolor col = playing[m] ? cyan : (root[m] ? blue : (mapped[m] ? nvgRGB(0x25, 0x4c, 0x66) : nvgRGB(0x3a, 0x3a, 0x4a)));
		nvgBeginPath(vg); nvgRoundedRectVarying(vg, x0 + 0.6f, y, ww - 1.2f, h, 0, 0, 1.4f, 1.4f);
		nvgFillColor(vg, col); nvgFill(vg); k++;
	}
	float bh = h * 0.62f, bw = ww * 0.6f;
	for (int m = PKB_LO; m <= PKB_HI; m++) {
		if (playIsWhite(m)) continue;
		float bx = x + playWhiteBefore(m) * ww - bw * 0.5f;
		nvgBeginPath(vg); nvgRoundedRectVarying(vg, bx - 0.9f, y, bw + 1.8f, bh + 0.9f, 0, 0, 1.6f, 1.6f);
		nvgFillColor(vg, gap); nvgFill(vg);
		NVGcolor col = playing[m] ? cyan : (root[m] ? blue : (mapped[m] ? nvgRGB(0x1b, 0x3a, 0x4e) : nvgRGB(0x20, 0x20, 0x2c)));
		nvgBeginPath(vg); nvgRoundedRectVarying(vg, bx, y, bw, bh, 0, 0, 1.2f, 1.2f);
		nvgFillColor(vg, col); nvgFill(vg);
	}
	if (font) {
		nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 6.5f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP); nvgFillColor(vg, nvgRGB(0x8a, 0x8a, 0xb0));
		for (int m = PKB_LO; m <= PKB_HI; m++) if (m % 12 == 0)
			nvgText(vg, x + playWhiteBefore(m) * ww + ww * 0.5f, y + h + 1, string::f("C%d", m / 12 - 1).c_str(), NULL);
	}
}

// Tabs, left→right: GRID (view 1) then PIANO (view 0).
static const int PLAY_TAB_VIEW[2] = { 1, 0 };
static const char* PLAY_TAB_LABEL[2] = { "GRID", "PIANO" };
void PlayDisplay::drawTabs(NVGcontext* vg, int view) {
	Rect r[2]; tabRects(r);
	for (int i = 0; i < 2; i++) {
		bool active = (view == PLAY_TAB_VIEW[i]);
		nvgBeginPath(vg); nvgRoundedRect(vg, r[i].pos.x, r[i].pos.y, r[i].size.x, r[i].size.y, 2.f);
		nvgFillColor(vg, active ? nvgRGB(0x0d, 0x59, 0x86) : nvgRGB(0x2a, 0x2a, 0x3e)); nvgFill(vg);
		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, active ? nvgRGB(0xe6, 0xe6, 0xf0) : nvgRGB(0x6a, 0x6a, 0x88));
			nvgText(vg, r[i].pos.x + r[i].size.x * 0.5f, r[i].pos.y + r[i].size.y * 0.5f + 0.5f, PLAY_TAB_LABEL[i], NULL);
		}
	}
}

void PlayDisplay::drawGrid(NVGcontext* vg, const uint8_t* mapped, const uint8_t* root, const uint8_t* playing,
                           int layout, int gRoot, int gScale, int gBase) {
	float x0, y0, pad; gridGeom(x0, y0, pad);
	float ps = pad * 0.88f, off = (pad - ps) * 0.5f;
	for (int rr = 0; rr < GRID_ROWS; rr++) {           // rr = display row (0 = top)
		int row = GRID_ROWS - 1 - rr;                  // musical row (0 = bottom)
		for (int col = 0; col < GRID_COLS; col++) {
			int n = gridNoteAt(layout, gRoot, gScale, gBase, row, col);
			float px = x0 + col * pad + off, py = y0 + rr * pad + off;
			bool inScale = (n >= 0) && gridNoteInScale(n, gRoot, gScale);
			bool isKeyRoot = (n >= 0) && (((n - gRoot) % 12 + 12) % 12 == 0);
			NVGcolor c;
			if (n < 0)              c = nvgRGB(0x14, 0x14, 0x22);
			else if (playing[n])    c = nvgRGB(0x00, 0xc8, 0xff);
			else if (root[n])       c = nvgRGB(0x00, 0x97, 0xde);
			else if (mapped[n])     c = nvgRGB(0x25, 0x4c, 0x66);
			else if (layout == 2)   c = nvgRGB(0x40, 0x40, 0x54);
			else                    c = inScale ? nvgRGB(0x3a, 0x3a, 0x4a) : nvgRGB(0x20, 0x20, 0x2c);
			// chromatic grid: darken accidental (black-key) columns across every tier
			// so they read as a piano-roll overlay even inside the lit/mapped region
			if (layout == 2 && n >= 0 && !playing[n] && gridIsAccidental(n))
				c = nvgRGBf(c.r * 0.55f, c.g * 0.55f, c.b * 0.55f);
			nvgBeginPath(vg); nvgRoundedRect(vg, px, py, ps, ps, 2.f);
			nvgFillColor(vg, c); nvgFill(vg);
			if (isKeyRoot) {   // key-root pads: bright ring + label
				nvgBeginPath(vg); nvgRoundedRect(vg, px + 0.5f, py + 0.5f, ps - 1.f, ps - 1.f, 2.f);
				nvgStrokeColor(vg, nvgRGBA(0xff, 0xd0, 0x60, 0xcc)); nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
				if (font && ps > 10.f) {
					nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 6.5f);
					nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
					nvgFillColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0xdd));
					nvgText(vg, px + ps * 0.5f, py + ps * 0.5f, string::f("%s%d", PLAY_NOTES[((n % 12) + 12) % 12], n / 12 - 1).c_str(), NULL);
				}
			}
		}
	}
}

void PlayDisplay::drawLayer(const DrawArgs& args, int layer) {
	if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
	if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	NVGcontext* vg = args.vg; const float w = box.size.x, h = box.size.y;
	nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, 3.f);
	nvgFillColor(vg, nvgRGB(0x1a, 0x1a, 0x2e)); nvgFill(vg);

	const char* name = "Piano"; const char* info = "12 regions";
	uint8_t mapped[128] = {0}, root[128] = {0}, playing[128] = {0}; int cnt = 0;
	int view = 1, layout = 0, gRoot = 0, gScale = 1, gBase = 36;
	if (module) { name = module->dispName; info = module->dispInfo;
		std::memcpy(mapped, module->dispMapped, 128); std::memcpy(root, module->dispRoot, 128);
		std::memcpy(playing, module->dispPlaying, 128); cnt = module->dispCount;
		view = module->kbView; layout = module->gridLayout; gRoot = module->gridRoot;
		gScale = module->gridScale; gBase = module->gridBase; }
	else { for (int n = 48; n <= 72; n++) mapped[n] = 1; for (int n = 48; n <= 72; n += 3) root[n] = 1; layout = 2; }

	drawTabs(vg, view);
	if (font) {
		nvgFontFaceId(vg, font->handle);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgFontSize(vg, 10.f); nvgFillColor(vg, nvgRGB(0xc8, 0xc8, 0xe0)); nvgText(vg, 6, 30, name, NULL);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
		nvgFillColor(vg, nvgRGB(0x00, 0xc8, 0xff)); nvgText(vg, w - 6, 30, string::f("%d voices", cnt).c_str(), NULL);
	}

	if (view == 1) {
		drawGrid(vg, mapped, root, playing, layout, gRoot, gScale, gBase);
	} else {
		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE); nvgFillColor(vg, nvgRGB(0x8a, 0x8a, 0xb0));
			nvgText(vg, 6, 43, info, NULL);
		}
		drawKeys(vg, 6, h - 46, w - 12, 28, mapped, root, playing);
	}
}

int PlayDisplay::hitPad(Vec p) const {
	if (!module) return -1;
	float x0, y0, pad; gridGeom(x0, y0, pad);
	if (p.x < x0 || p.y < y0) return -1;
	int col = (int)((p.x - x0) / pad), rr = (int)((p.y - y0) / pad);
	if (col < 0 || col >= GRID_COLS || rr < 0 || rr >= GRID_ROWS) return -1;
	return gridNoteAt(module->gridLayout, module->gridRoot, module->gridScale, module->gridBase, GRID_ROWS - 1 - rr, col);
}

int PlayDisplay::pianoNoteAt(Vec p) const {
	float x = 6, y = box.size.y - 46, w = box.size.x - 12, h = 28;
	if (p.y < y || p.y > y + h) return -1;
	int NW = playWhiteBefore(PKB_HI + 1); float ww = w / NW;
	float bh = h * 0.62f, bw = ww * 0.6f;
	if (p.y <= y + bh) {                              // black keys sit on top
		for (int m = PKB_LO; m <= PKB_HI; m++) {
			if (playIsWhite(m)) continue;
			float bx = x + playWhiteBefore(m) * ww - bw * 0.5f;
			if (p.x >= bx && p.x <= bx + bw) return m;
		}
	}
	int k = (int)((p.x - x) / ww); if (k < 0) return -1;
	int cnt = 0;
	for (int m = PKB_LO; m <= PKB_HI; m++) { if (!playIsWhite(m)) continue; if (cnt == k) return m; cnt++; }
	return -1;
}

void PlayDisplay::onButton(const ButtonEvent& e) {
	if (!module) { OpaqueWidget::onButton(e); return; }
	if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
		Vec p = e.pos;
		int t = hitTab(p);
		if (t >= 0) { module->kbView = PLAY_TAB_VIEW[t]; e.consume(this); return; }
		int n = (module->kbView == 1) ? hitPad(p) : pianoNoteAt(p);
		if (n >= 0) { module->uiNote = n; dragging = true; dragPos = p; e.consume(this); return; }
	}
	if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_RELEASE && dragging) {
		module->uiNote = -1; dragging = false; e.consume(this); return;
	}
	OpaqueWidget::onButton(e);
}
void PlayDisplay::onDragMove(const DragMoveEvent& e) {
	if (!module || !dragging) { OpaqueWidget::onDragMove(e); return; }
	float zoom = getAbsoluteZoom(); if (zoom <= 0.f) zoom = 1.f;
	dragPos = dragPos.plus(e.mouseDelta.div(zoom));
	int n = (module->kbView == 1) ? hitPad(dragPos) : pianoNoteAt(dragPos);
	if (n >= 0) module->uiNote = n;
}
void PlayDisplay::onDragEnd(const DragEndEvent& e) {
	if (module && dragging) { module->uiNote = -1; dragging = false; }
	OpaqueWidget::onDragEnd(e);
}

// ─── Widget ──────────────────────────────────────────────────────────────────
struct PlayWidget : ModuleWidget {
	PlayWidget(Play* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/play.svg")));
		// No virtual screws — see CLAUDE.md.

		PlayDisplay* disp = new PlayDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(4.f, 12.f));
		disp->box.size = mm2px(Vec(73.f, 57.f));
		addChild(disp);

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 81.18f)), module, Play::INSTR_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.16f, 101.50f)), module, Play::LEVEL_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(26.25f, 81.73f)), module, Play::INSTR_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.40f, 101.50f)), module, Play::LEVEL_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 121.82f)), module, Play::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.40f, 121.82f)), module, Play::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.64f, 121.82f)), module, Play::VEL_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(55.88f, 122.13f)), module, Play::L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(71.12f, 121.82f)), module, Play::R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Play* m = dynamic_cast<Play*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Load instrument (.sfz / .dspreset / .dsbundle)…", "", [m]() {
			char* p = osdialog_file(OSDIALOG_OPEN, NULL, NULL, osdialog_filters_parse("Instrument:sfz,dspreset,dsbundle"));
			if (p) { m->loadInstrument(p); std::free(p); }
		}));
		// Fallback for when .dsbundle is a plain folder (not a registered package):
		// pick the bundle folder directly.
		menu->addChild(createMenuItem("Load DecentSampler bundle (folder)…", "", [m]() {
			char* p = osdialog_file(OSDIALOG_OPEN_DIR, NULL, NULL, NULL);
			if (p) { m->loadInstrument(p); std::free(p); }
		}));
		for (int i = 0; i < (int)m->instruments.size(); i++) {
			std::string label = string::f("%d: %s", i + 1, m->instruments[i].name.c_str());
			menu->addChild(createCheckMenuItem(label, "",
				[m, i]() { return m->curInstrument == i; },
				[m, i]() { m->params[Play::INSTR_PARAM].setValue((float)i); }));
		}
		if (!m->instruments.empty())
			menu->addChild(createMenuItem("Remove current instrument", "", [m]() { m->removeInstrument(m->curInstrument); }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("One-shot (play through, ignore gate-off)", "", &m->oneShot));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Grid view"));
		menu->addChild(createIndexSubmenuItem("Layout", {"Chromatic (4ths)", "In-Key (scale)", "Chromatic grid (C0)"},
			[m]() { return m->gridLayout; }, [m](int i) { m->gridLayout = i; }));
		static const char* NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
		std::vector<std::string> roots(NOTE_NAMES, NOTE_NAMES + 12);
		menu->addChild(createIndexSubmenuItem("Root", roots,
			[m]() { return m->gridRoot; }, [m](int i) { m->gridRoot = i; }));
		std::vector<std::string> scales;
		for (int i = 0; i < sfs::NUM_SCALES; i++) scales.push_back(sfs::SCALES[i].longName);
		menu->addChild(createIndexSubmenuItem("Scale (In-Key)", scales,
			[m]() { return m->gridScale; }, [m](int i) { m->gridScale = i; }));
		menu->addChild(createMenuItem("Octave up", "", [m]() { if (m->gridBase <= 103) m->gridBase += 12; }));
		menu->addChild(createMenuItem("Octave down", "", [m]() { if (m->gridBase >= 12) m->gridBase -= 12; }));
	}
};

Model* modelPlay = createModel<Play, PlayWidget>("Play");

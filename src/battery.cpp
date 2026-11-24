// =============================================================================
// Battery: a polyphonic drum sequencer, eight lanes by up to 64 steps.
//
// Each lane is one instrument. Each step has on/off, velocity, a chance that it
// plays, a ratchet (1 to 4 hits inside the step) and a ROUTE: the chance the
// hit goes to the lane's B channel instead of its A channel. A lane's A channel
// is its index and its B channel is its index + 8, so lane 1 plays channel 1 or
// channel 9 on the 16-channel GATE/VEL/V/OCT outs. The A and B outs carry the
// same thing split in two 8-channel cables, so each side can drive its own Kit
// without a splitter.
//
// Every lane has its own length (1 to 64), play order and clock rate. The play
// orders: forward, backward, pendulum (ends once), ping-pong (ends twice),
// random, drunk, and serialist, which plays every step of the lane once in a
// shuffled order before it shuffles again.
//
// Random draws are not taken from a running generator. Each is a hash of the
// seed, the lane, what is being decided, the pass through the lane and the
// step within it. Unlocked, the pass counts up, so every pass draws afresh.
// LOCK freezes the pass at the one playing when it was pressed, so that pass
// repeats exactly. NEW changes the seed.
//
// Eight patterns. PATTERN (knob + 1 V per pattern) and NEXT compose, as on
// Canon; a change waits for the BAR input when it is patched, otherwise for
// the end of the longest lane, and restarts every lane from its first step.
//
// Design: docs/battery-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

static const int BT_L = 8;          // lanes
static const int BT_S = 64;         // steps per lane
static const int BT_PAGE = 16;      // steps on one page of the screen
static const int BT_NPAGE = BT_S / BT_PAGE;
static const int BT_P = 8;          // patterns
static const int BT_MAXR = 4;       // ratchets per step

enum BtMode { BT_FWD, BT_BACK, BT_PEND, BT_PING, BT_RAND, BT_DRUNK, BT_SERIAL, BT_NMODE };
static const char* BT_MODE_NAME[BT_NMODE] = {
	"Forward", "Backward", "Pendulum (ends once)", "Ping-pong (ends twice)", "Random", "Drunk",
	"Serialist (every step once, then shuffle again)"};
static const char* BT_MODE_SHORT[BT_NMODE] = {"FWD", "BACK", "PEND", "PING", "RAND", "DRNK", "SER"};

// Lane clock rates, in steps per CLOCK pulse.
static const int BT_NDIV = 11;
static const int BT_UNITY = 5;
static const float BT_RATE[BT_NDIV] = {0.25f, 1.f / 3.f, 0.5f, 2.f / 3.f, 0.75f, 1.f,
                                       4.f / 3.f, 1.5f, 2.f, 3.f, 4.f};
static const char* BT_RATE_NAME[BT_NDIV] = {"/4", "/3", "/2", "x2/3", "x3/4", "x1",
                                            "x4/3", "x3/2", "x2", "x3", "x4"};

static const float BT_VEL0 = 0.8f;   // a new step's velocity

// Draws, so each decision has its own stream.
enum BtKind { BK_CHANCE = 1, BK_ROUTE, BK_RAND, BK_DRUNK, BK_PERM };

static inline uint32_t btMix(uint32_t h) {
	h ^= h >> 16; h *= 0x7feb352du;
	h ^= h >> 15; h *= 0x846ca68bu;
	h ^= h >> 16;
	return h;
}
static inline uint32_t btHash(uint32_t seed, uint32_t lane, uint32_t kind, uint32_t pass, uint32_t i) {
	uint32_t h = btMix(seed ^ 0x9E3779B9u);
	h = btMix(h ^ (lane * 0x85EBCA6Bu + kind * 0xC2B2AE35u));
	h = btMix(h ^ pass * 0x27D4EB2Fu);
	h = btMix(h ^ i * 0x165667B1u);
	return h;
}
static inline float btUnit(uint32_t h) { return (float)(h >> 8) / 16777216.f; }   // 0..1

struct BtStep {
	bool on = false;
	float vel = BT_VEL0;
	float chance = 1.f;
	float route = 0.f;      // chance of the B channel
	int ratch = 1;
};

struct BtLane {
	BtStep st[BT_S];
	int len = 16;
	int mode = BT_FWD;
	int div = BT_UNITY;
	bool used() const {
		for (int i = 0; i < BT_S; i++) if (st[i].on) return true;
		return false;
	}
};

struct BtPattern {
	BtLane lane[BT_L];
	bool used() const {
		for (int l = 0; l < BT_L; l++) if (lane[l].used()) return true;
		return false;
	}
};

// The play order's period in steps, and the position at index i of it.
static inline int btPeriod(int mode, int len) {
	if (mode == BT_PEND) return std::max(1, 2 * len - 2);
	if (mode == BT_PING) return 2 * len;
	return len;
}

// ── templates, from Fill's bank ─────────────────────────────────────────────
// Fill's grids are in the same lane order (kick, snare, closed hat, open hat,
// low tom, high tom, clap, bell) and carry what a Battery step does: `v` is
// velocity n/9, `a` an accent (taken as full velocity), `p` a chance n/9 and
// `r` a ratchet (capped at 4 here). A set's variations are sparse, main, lift
// and fill, which can be loaded into patterns 1 to 4. Fill's swing has no
// counterpart here: swing the clock instead (Meter swings each output).
static const char* BT_LANE_KEY[BT_L] = {"kick", "snare", "chh", "ohh", "lo", "hi", "cp", "bell"};
static const char* BT_ROLE_KEY[4] = {"sparse", "main", "lift", "fill"};

struct BtTplGroup { const char* heading; std::vector<const char*> ids; };
static const std::vector<BtTplGroup>& btTplGroups() {
	static const std::vector<BtTplGroup> G = {
		{"Rock", {"rock.motorik", "rock.basic", "rock.shuffle"}},
		{"Funk, soul and New Orleans", {"funk.sixteen", "pop.motown", "usa.secondline"}},
		{"Electronic", {"edm.house", "electronic.deephouse", "electronic.techno", "electronic.electro",
		                "electronic.breakbeat", "electronic.jungle", "electronic.dnb", "electronic.garage",
		                "electronic.dubstep", "electronic.triphop"}},
		{"Hip-hop", {"hiphop.boombap", "hiphop.trap", "hiphop.lofi", "hiphop.gfunk"}},
		{"Latin, Caribbean and African", {"latin.son", "latin.samba", "latin.bossa", "latin.dembow",
		                                   "carib.onedrop", "africa.afrobeat", "africa.amapiano"}},
	};
	return G;
}

struct BtTemplate {
	std::string id, name;
	float swing = 0.f;
	std::vector<std::string> roleName;   // in sparse, main, lift, fill order, those the set has
	std::vector<BtPattern> roles;
	int mainIdx = -1;
};

// One grid from the bank into a pattern. Returns false if it has no steps.
static bool btParseGrid(json_t* p, BtPattern& out) {
	out = BtPattern();
	int beats = (int)json_integer_value(json_object_get(p, "beats"));
	int spb = (int)json_integer_value(json_object_get(p, "stepsPerBeat"));
	int bars = std::max(1, (int)json_integer_value(json_object_get(p, "bars")));
	int L = clamp(beats * spb * bars, 1, BT_S);
	json_t* grid = json_object_get(p, "grid");
	if (!grid) return false;
	bool any = false;
	for (int l = 0; l < BT_L; l++) {
		BtLane& ln = out.lane[l];
		ln.len = L;
		json_t* g = json_object_get(grid, BT_LANE_KEY[l]);
		if (!g) continue;
		const char* v = json_string_value(json_object_get(g, "v"));
		const char* a = json_string_value(json_object_get(g, "a"));
		const char* pr = json_string_value(json_object_get(g, "p"));
		const char* r = json_string_value(json_object_get(g, "r"));
		for (int i = 0; v && i < L && v[i]; i++) {
			if (v[i] < '1' || v[i] > '9') continue;
			BtStep& st = ln.st[i];
			st.on = true; any = true;
			st.vel = (v[i] - '0') / 9.f;
			if (a && (int)std::strlen(a) > i && a[i] == 'A') st.vel = 1.f;
			if (pr && (int)std::strlen(pr) > i && pr[i] >= '1' && pr[i] <= '9') st.chance = (pr[i] - '0') / 9.f;
			if (r && (int)std::strlen(r) > i && r[i] >= '2' && r[i] <= '9') st.ratch = std::min(BT_MAXR, r[i] - '0');
		}
	}
	return any;
}

// Reads the bank once. A path, so a harness can read it without a plugin.
static std::vector<BtTemplate> btLoadTemplates(const std::string& path) {
	std::vector<BtTemplate> out;
	json_error_t err;
	json_t* root = json_load_file(path.c_str(), 0, &err);
	if (!root) { WARN("Battery: cannot read %s", path.c_str()); return out; }
	json_t* pats = json_object_get(root, "patterns");
	json_t* sets = json_object_get(root, "sets");
	auto findById = [](json_t* arr, const char* id) -> json_t* {
		size_t i; json_t* e;
		json_array_foreach(arr, i, e) {
			const char* x = json_string_value(json_object_get(e, "id"));
			if (x && std::string(x) == id) return e;
		}
		return nullptr;
	};
	for (const BtTplGroup& g : btTplGroups()) {
		for (const char* id : g.ids) {
			json_t* set = findById(sets, id);
			if (!set) { WARN("Battery: template %s is not in the bank", id); continue; }
			BtTemplate t;
			t.id = id;
			t.name = json_string_value(json_object_get(set, "name")) ? json_string_value(json_object_get(set, "name")) : id;
			json_t* roles = json_object_get(set, "roles");
			for (int k = 0; k < 4; k++) {
				json_t* rv = json_object_get(roles, BT_ROLE_KEY[k]);
				if (json_is_array(rv)) rv = json_array_get(rv, 0);
				const char* pid = json_string_value(rv);
				json_t* p = pid ? findById(pats, pid) : nullptr;
				BtPattern bp;
				if (!p || !btParseGrid(p, bp)) continue;
				if (k == 1) { t.mainIdx = (int)t.roles.size(); t.swing = (float)json_number_value(json_object_get(p, "swing")); }
				t.roleName.push_back(BT_ROLE_KEY[k]);
				t.roles.push_back(bp);
			}
			if (t.mainIdx >= 0) out.push_back(t);
		}
	}
	json_decref(root);
	return out;
}

static const std::vector<BtTemplate>& btTemplates() {
	static std::vector<BtTemplate> T;
	static bool loaded = false;
	if (!loaded) { loaded = true; T = btLoadTemplates(asset::plugin(pluginInstance, "res/patterns/drum-patterns-v2.json")); }
	return T;
}

struct Battery : Module {
	enum ParamId { RUN_PARAM, RESET_PARAM, PATTERN_PARAM, NEXT_PARAM, BIAS_PARAM,
	               LOCK_PARAM, NEW_PARAM, PARAMS_LEN };
	enum InputId { CLOCK_INPUT, BAR_INPUT, RUN_INPUT, RESET_INPUT, PATTERN_INPUT, NEXT_INPUT,
	               BIAS_INPUT, LOCK_INPUT, NEW_INPUT, MUTE_INPUT, INPUTS_LEN };
	enum OutputId { GATE_OUTPUT, VEL_OUTPUT, VOCT_OUTPUT, A_GATE_OUTPUT, A_VEL_OUTPUT, A_VOCT_OUTPUT,
	                B_GATE_OUTPUT, B_VEL_OUTPUT, B_VOCT_OUTPUT, EOC_OUTPUT, OUTPUTS_LEN };
	enum LightId { RUN_LIGHT, LOCK_LIGHT, LIGHTS_LEN };

	BtPattern pat[BT_P];

	// per lane, not per pattern: what the lane is
	bool mute[BT_L] = {};
	float noteA[BT_L], noteB[BT_L];     // semitones from C4

	// ── the clock ───────────────────────────────────────────────────────
	bool started = false;        // a clock edge has arrived since reset
	long edges = 0;              // clock edges since reset
	double phase = 0.0;          // in clock pulses since reset
	double origin = 0.0;         // where the lanes' first step is
	double sinceEdge = 0.0, period = 0.125;
	bool periodValid = false, skipMeasure = false;
	bool wasRunning = true;

	// ── patterns ────────────────────────────────────────────────────────
	int cur = 0;                 // the pattern playing
	int nextCount = 0;           // NEXT edges since reset, as on Canon
	int target = 0;              // the pattern PATTERN, CV and NEXT ask for
	int master = -1;             // the lane that sets the cycle, refreshed every 64 samples

	// ── randomness ──────────────────────────────────────────────────────
	uint32_t seed = 1u;          // set at random for a new module, saved with the patch
	bool locked = false;

	// ── each lane as it runs ────────────────────────────────────────────
	struct Run {
		long count = -1;         // steps since the origin
		int pos = -1;            // the step being played
		long passBase = 0;       // passes before the last restart (unlocked draws stay fresh)
		long lockPass = 0;
		int walk = 0, walkStart = 0, lockWalkStart = 0;
		int perm[BT_S];
		int permLen = 0;
		long permPass = -1;
		// the step's hits
		bool live = false;
		int side = 0, ratch = 1, sub = 0;
		float vel = 0.f;
		// for the screen
		int64_t fireT = -100000;
		int fireSide = 0;
	};
	Run run[BT_L];

	dsp::SchmittTrigger clockTrig, barTrig, resetTrig, resetBtnTrig, nextTrig, nextBtnTrig,
	                    newTrig, newBtnTrig;
	dsp::PulseGenerator gatePulse[16], eocPulse;
	float gapT[16] = {}, gapLen[16] = {};   // a retrigger's low gap, and the gate after it
	float velOut[16] = {};
	float barWait = 0.f;         // a BAR that came before its clock waits this long for it

	// menu, saved
	int gateMode = 1;            // 0: 5 ms trigger, 1: half the step
	bool resetOnRun = false;
	bool switchNow = false;      // a new pattern takes over on the next step, lanes keep their positions

	// screen state, saved
	int editPattern = 0;
	int tab = 0;
	int page = 0;
	bool viewFollow = true;

	int64_t now = 0;
	float sr = 48000.f;

	Battery() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configSwitch(RUN_PARAM, 0.f, 1.f, 1.f, "Run", {"Stopped", "Running"});
		configButton(RESET_PARAM, "Reset");
		configParam(PATTERN_PARAM, 0.f, (float)(BT_P - 1), 0.f, "Pattern", "", 0.f, 1.f, 1.f);
		paramQuantities[PATTERN_PARAM]->snapEnabled = true;
		configButton(NEXT_PARAM, "Next pattern");
		configParam(BIAS_PARAM, -1.f, 1.f, 0.f, "Route bias (added to every step's chance of B)", "%", 0.f, 100.f);
		configSwitch(LOCK_PARAM, 0.f, 1.f, 0.f, "Lock (repeat the pass playing when it was locked)", {"Off", "On"});
		configButton(NEW_PARAM, "New seed");
		configInput(CLOCK_INPUT, "Clock (one pulse per step)");
		configInput(BAR_INPUT, "Bar (a pattern change waits for this when patched)");
		configInput(RUN_INPUT, "Run gate (overrides the RUN button when patched)");
		configInput(RESET_INPUT, "Reset");
		configInput(PATTERN_INPUT, "Pattern CV (1V per pattern)");
		configInput(NEXT_INPUT, "Next pattern");
		configInput(BIAS_INPUT, "Route bias CV (polyphonic: channel N is lane N; +-5V = +-100%)");
		configInput(LOCK_INPUT, "Lock gate (locked while high)");
		configInput(NEW_INPUT, "New seed trigger");
		configInput(MUTE_INPUT, "Mute (polyphonic: channel N mutes lane N; a mono cable mutes every lane)");
		configOutput(GATE_OUTPUT, "Gate (16 channels: lane N on N for A, N + 8 for B)");
		configOutput(VEL_OUTPUT, "Velocity (16 channels, 0-10V)");
		configOutput(VOCT_OUTPUT, "Pitch (16 channels, each lane's A and B note)");
		configOutput(A_GATE_OUTPUT, "A gate (8 channels)");
		configOutput(A_VEL_OUTPUT, "A velocity (8 channels)");
		configOutput(A_VOCT_OUTPUT, "A pitch (8 channels)");
		configOutput(B_GATE_OUTPUT, "B gate (8 channels)");
		configOutput(B_VEL_OUTPUT, "B velocity (8 channels)");
		configOutput(B_VOCT_OUTPUT, "B pitch (8 channels)");
		configOutput(EOC_OUTPUT, "End of cycle (the longest lane wraps, or the pattern changes)");
		for (int l = 0; l < BT_L; l++) { noteA[l] = (float)l; noteB[l] = (float)(l + 8); }
		seed = random::u32();
	}

	// A groove in pattern 1, in Fill's and Kit's channel order (kick, snare,
	// closed hat, open hat, low tom, high tom, clap, bell). Only the browser
	// thumbnail uses it, if the template bank cannot be read: a new module
	// starts blank.
	void defaultPattern() {
		BtPattern& p = pat[0];
		auto set = [&](int l, int s, float v, float ch = 1.f, float rt = 0.f) {
			BtStep& st = p.lane[l].st[s];
			st.on = true; st.vel = v; st.chance = ch; st.route = rt;
		};
		set(0, 0, 1.f); set(0, 8, 0.95f); set(0, 10, 0.7f, 0.6f);
		set(1, 4, 0.9f); set(1, 12, 0.9f); set(1, 15, 0.4f, 0.4f);
		for (int s = 0; s < 16; s += 2) set(2, s, (s % 4) ? 0.55f : 0.85f, 1.f, (s == 6 || s == 14) ? 0.5f : 0.f);
		set(3, 14, 0.7f, 0.5f);
		set(6, 12, 0.8f, 0.35f);
		p.lane[2].st[14].ratch = 2;
	}

	void onReset() override {
		for (int i = 0; i < BT_P; i++) pat[i] = BtPattern();
		for (int l = 0; l < BT_L; l++) { mute[l] = false; noteA[l] = (float)l; noteB[l] = (float)(l + 8); }
		cur = 0; nextCount = 0; editPattern = 0; page = 0; tab = 0;
		locked = false;
		doReset();
	}

	// Every lane starts again at its first step, at the given origin. Unlocked
	// lanes move their pass base on, so the next run draws afresh.
	void restart(double at) {
		origin = at;
		for (int l = 0; l < BT_L; l++) {
			Run& r = run[l];
			if (r.count >= 0) {
				const BtLane& ln = pat[cur].lane[l];
				r.passBase += r.count / btPeriod(ln.mode, ln.len) + 1;
			}
			r.count = -1; r.pos = -1; r.live = false;
			r.walk = 0; r.walkStart = 0;
			r.permPass = -1;
		}
	}

	// The pending pattern takes over, every lane at its first step.
	void switchTo(double at) {
		int t = target;
		restart(at);
		cur = t;
		master = -1;
		eocPulse.trigger(1e-3f);
	}

	void doReset() {
		started = false; edges = 0; phase = 0.0;
		restart(0.0);
		cur = target = resolve();
		for (int c = 0; c < 16; c++) { gatePulse[c].reset(); gapT[c] = 0.f; }
		barWait = 0.f;
	}

	// ── patterns ────────────────────────────────────────────────────────
	int offset() {
		int o = (int)std::round(params[PATTERN_PARAM].getValue());
		if (inputs[PATTERN_INPUT].isConnected()) o += (int)std::round(inputs[PATTERN_INPUT].getVoltage());
		return o;
	}
	int resolve() { return (((nextCount + offset()) % BT_P) + BT_P) % BT_P; }
	// NEXT skips empty patterns, so a two-pattern song is patterns 1 and 2
	void advancePattern() {
		for (int k = 1; k <= BT_P; k++) {
			nextCount++;
			if (pat[resolve()].used()) return;
		}
	}
	void selectPattern(int p) { nextCount += p - resolve(); }

	// The lane that takes longest to go round decides the cycle. Lanes with no
	// steps do not count, or an untouched 16-step lane would set every cycle.
	int masterLane() {
		int best = 0; double longest = -1.0;
		for (int l = 0; l < BT_L; l++) {
			const BtLane& ln = pat[cur].lane[l];
			if (!ln.used()) continue;
			double d = (double)ln.len / BT_RATE[clamp(ln.div, 0, BT_NDIV - 1)];
			if (d > longest + 1e-9) { longest = d; best = l; }
		}
		return best;
	}

	void setLock(bool on) {
		if (on == locked) return;
		locked = on;
		if (on) {
			for (int l = 0; l < BT_L; l++) {
				Run& r = run[l];
				const BtLane& ln = pat[cur].lane[l];
				long k = std::max(0L, r.count);
				r.lockPass = r.passBase + k / btPeriod(ln.mode, ln.len);
				r.lockWalkStart = r.walkStart;
			}
		}
	}
	void newSeed() {
		seed = random::u32();
		for (int l = 0; l < BT_L; l++) run[l].permPass = -1;
	}

	// ── a lane lands on step count k ────────────────────────────────────
	void land(int l, long k) {
		Run& r = run[l];
		const BtLane& ln = pat[cur].lane[l];
		int L = clamp(ln.len, 1, BT_S);
		int P = btPeriod(ln.mode, L);
		int i = (int)(k % P);
		long pass = locked ? r.lockPass : r.passBase + k / P;
		uint32_t lu = (uint32_t)l, pu = (uint32_t)pass, iu = (uint32_t)i;
		int pos = 0;
		switch (ln.mode) {
			case BT_FWD:  pos = i; break;
			case BT_BACK: pos = L - 1 - i; break;
			case BT_PEND: pos = (i < L) ? i : P - i; break;
			case BT_PING: pos = (i < L) ? i : 2 * L - 1 - i; break;
			case BT_RAND: pos = std::min(L - 1, (int)(btUnit(btHash(seed, lu, BK_RAND, pu, iu)) * L)); break;
			case BT_DRUNK: {
				// a step either way, wrapping; a locked pass starts where it started
				int d = btUnit(btHash(seed, lu, BK_DRUNK, pu, iu)) < 0.5f ? -1 : 1;
				if (i == 0 && locked) r.walk = r.lockWalkStart;
				else if (k > 0) r.walk += d;
				r.walk = ((r.walk % L) + L) % L;
				if (i == 0) r.walkStart = r.walk;
				pos = r.walk;
				break;
			}
			case BT_SERIAL: {
				if (r.permPass != pass || r.permLen != L) {
					int prevLast = (r.permLen > 0 && r.permPass >= 0) ? r.perm[r.permLen - 1] : -1;
					for (int j = 0; j < L; j++) r.perm[j] = j;
					for (int j = L - 1; j > 0; j--) {
						int q = (int)(btUnit(btHash(seed, lu, BK_PERM, pu, (uint32_t)j)) * (j + 1));
						q = std::min(q, j);
						std::swap(r.perm[j], r.perm[q]);
					}
					// a new shuffle never starts on the step the last one ended on
					if (L > 1 && r.perm[0] == prevLast) {
						int q = 1 + (int)(btUnit(btHash(seed, lu, BK_PERM, pu, 999u)) * (L - 1));
						std::swap(r.perm[0], r.perm[std::min(q, L - 1)]);
					}
					r.permLen = L; r.permPass = pass;
				}
				pos = r.perm[std::min(i, L - 1)];
				break;
			}
		}
		pos = clamp(pos, 0, L - 1);
		r.pos = pos;
		r.live = false;
		const BtStep& st = ln.st[pos];
		if (!st.on) return;
		if (btUnit(btHash(seed, lu, BK_CHANCE, pu, iu)) >= st.chance) return;
		float bias = params[BIAS_PARAM].getValue();
		if (inputs[BIAS_INPUT].isConnected()) bias += 0.2f * inputs[BIAS_INPUT].getPolyVoltage(l);
		float route = clamp(st.route + bias, 0.f, 1.f);
		r.side = btUnit(btHash(seed, lu, BK_ROUTE, pu, iu)) < route ? 1 : 0;
		r.vel = st.vel;
		r.ratch = clamp(st.ratch, 1, BT_MAXR);
		r.sub = 0;
		r.live = true;
		hit(l);
	}

	bool muted(int l) {
		if (mute[l]) return true;
		return inputs[MUTE_INPUT].isConnected() && inputs[MUTE_INPUT].getPolyVoltage(l) >= 1.f;
	}

	void hit(int l) {
		Run& r = run[l];
		if (muted(l)) return;
		int c = l + 8 * r.side;
		float g = 0.005f;
		if (gateMode == 1) {
			const BtLane& ln = pat[cur].lane[l];
			float stepSec = periodValid ? (float)period / BT_RATE[clamp(ln.div, 0, BT_NDIV - 1)] : 0.02f;
			g = std::max(0.002f, 0.5f * stepSec / r.ratch);
		}
		// a hit on a channel still high goes low for a millisecond first
		if (gatePulse[c].remaining > 0.f) { gatePulse[c].reset(); gapT[c] = 0.001f; gapLen[c] = g; }
		else gatePulse[c].trigger(g);
		velOut[c] = 10.f * r.vel;
		r.fireT = now; r.fireSide = r.side;
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;
		sr = args.sampleRate;
		now++;

		bool resetIn = resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		bool resetBtn = resetBtnTrig.process(params[RESET_PARAM].getValue() > 0.5f ? 10.f : 0.f, 0.1f, 1.f);
		if (resetIn || resetBtn) { nextCount = 0; doReset(); }

		if (nextTrig.process(inputs[NEXT_INPUT].getVoltage(), 0.1f, 1.f)) advancePattern();
		if (nextBtnTrig.process(params[NEXT_PARAM].getValue() > 0.5f ? 10.f : 0.f, 0.1f, 1.f)) advancePattern();
		if (newTrig.process(inputs[NEW_INPUT].getVoltage(), 0.1f, 1.f)) newSeed();
		if (newBtnTrig.process(params[NEW_PARAM].getValue() > 0.5f ? 10.f : 0.f, 0.1f, 1.f)) newSeed();
		bool lockNow = params[LOCK_PARAM].getValue() > 0.5f
		            || (inputs[LOCK_INPUT].isConnected() && inputs[LOCK_INPUT].getVoltage() >= 1.f);
		setLock(lockNow);
		target = resolve();
		// before anything has played, or with the menu set to "on the next step",
		// the new pattern takes over now and the lanes keep their positions
		if (target != cur && (switchNow || !started)) cur = target;

		bool running = inputs[RUN_INPUT].isConnected() ? inputs[RUN_INPUT].getVoltage() >= 1.f
		                                               : params[RUN_PARAM].getValue() > 0.5f;
		if (running && !wasRunning) {
			if (resetOnRun) doReset();
			skipMeasure = true;
		}
		wasRunning = running;

		bool clockEdge = false;
		if (inputs[CLOCK_INPUT].isConnected()) {
			sinceEdge += dt;
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f) && running) {
				// (the gap across a stop is not a period: keep the last one)
				if (!skipMeasure && sinceEdge > 0.002 && sinceEdge < 10.0) { period = sinceEdge; periodValid = true; }
				skipMeasure = false;
				sinceEdge = 0.0;
				phase = (double)edges;
				edges++;
				started = true;
				clockEdge = true;
			}
		}

		if (!running) {
			for (int c = 0; c < 16; c++) { gatePulse[c].reset(); gapT[c] = 0.f; }
		}
		else if (started) {
			// between edges the phase runs at the measured rate, held short of the
			// next edge so a fast lane never plays ahead of the clock
			if (!clockEdge && periodValid) phase = std::min(phase + dt / period, (double)edges - 1e-6);

			// the bar: a pending pattern lands on it, with every lane at its first step
			bool barPatched = inputs[BAR_INPUT].isConnected();
			bool barEdge = barTrig.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 1.f);
			if (barEdge && target != cur) {
				// a bar with its clock, or a moment after it, lands on that clock's
				// step; a bar a moment before its clock waits for it
				if (clockEdge) switchTo(phase);
				else if (sinceEdge < 0.005) switchTo((double)(edges - 1));
				else barWait = 0.005f;
			}
			else if (barWait > 0.f) {
				barWait -= dt;
				if (clockEdge || barWait <= 0.f) { barWait = 0.f; if (target != cur) switchTo(phase); }
			}

			// the end of the cycle: the longest lane goes round
			{
				if ((now & 63) == 0 || master < 0) master = masterLane();
				int m = master;
				const BtLane& ln = pat[cur].lane[m];
				double rate = BT_RATE[clamp(ln.div, 0, BT_NDIV - 1)];
				long km = (long)std::floor((phase - origin) * rate + 1e-9);
				if (km != run[m].count && km > 0 && km % std::max(1, ln.len) == 0) {
					eocPulse.trigger(1e-3f);
					if (!barPatched && target != cur) switchTo(phase);
				}
			}

			// the lanes
			for (int l = 0; l < BT_L; l++) {
				Run& r = run[l];
				const BtLane& ln = pat[cur].lane[l];
				double rate = BT_RATE[clamp(ln.div, 0, BT_NDIV - 1)];
				double p = (phase - origin) * rate;
				if (p < -1e-9) continue;
				long k = (long)std::floor(p + 1e-9);
				if (k != r.count) { r.count = k; land(l, k); }
				else if (r.live && r.ratch > 1) {
					int sub = std::min(r.ratch - 1, (int)std::floor((p - (double)k) * r.ratch));
					if (sub > r.sub) { r.sub = sub; hit(l); }
				}
			}
		}

		// ── outputs ─────────────────────────────────────────────────────
		outputs[GATE_OUTPUT].setChannels(16);
		outputs[VEL_OUTPUT].setChannels(16);
		outputs[VOCT_OUTPUT].setChannels(16);
		for (int o = A_GATE_OUTPUT; o <= B_VOCT_OUTPUT; o++) outputs[o].setChannels(8);
		for (int c = 0; c < 16; c++) {
			if (gapT[c] > 0.f) { gapT[c] -= dt; if (gapT[c] <= 0.f) gatePulse[c].trigger(gapLen[c]); }
			float g = gatePulse[c].process(dt) ? 10.f : 0.f;
			float v = velOut[c];
			float n = (c < 8 ? noteA[c] : noteB[c - 8]) / 12.f;
			outputs[GATE_OUTPUT].setVoltage(g, c);
			outputs[VEL_OUTPUT].setVoltage(v, c);
			outputs[VOCT_OUTPUT].setVoltage(n, c);
			int side = c < 8 ? 0 : 3, ch = c & 7;
			outputs[A_GATE_OUTPUT + side].setVoltage(g, ch);
			outputs[A_VEL_OUTPUT + side].setVoltage(v, ch);
			outputs[A_VOCT_OUTPUT + side].setVoltage(n, ch);
		}
		outputs[EOC_OUTPUT].setVoltage(eocPulse.process(dt) ? 10.f : 0.f);
		lights[RUN_LIGHT].setBrightness(running ? 1.f : 0.f);
		lights[LOCK_LIGHT].setBrightness(locked ? 1.f : 0.f);
	}

	// ── edits the screen and menu make ──────────────────────────────────
	void rotateLane(int p, int l, int by) {
		BtLane& ln = pat[p].lane[l];
		int L = clamp(ln.len, 1, BT_S);
		BtStep tmp[BT_S];
		for (int i = 0; i < L; i++) tmp[((i + by) % L + L) % L] = ln.st[i];
		for (int i = 0; i < L; i++) ln.st[i] = tmp[i];
	}
	// Pages: sixteen steps of all eight lanes. Copying a page copies every field
	// of every step and leaves the lane lengths alone.
	struct BtPage { BtStep st[BT_L][BT_PAGE]; };
	BtPage readPage(int p, int g) const {
		BtPage pg;
		for (int l = 0; l < BT_L; l++)
			for (int c = 0; c < BT_PAGE; c++) pg.st[l][c] = pat[p].lane[l].st[g * BT_PAGE + c];
		return pg;
	}
	void writePage(int p, int g, const BtPage& pg) {
		for (int l = 0; l < BT_L; l++)
			for (int c = 0; c < BT_PAGE; c++) pat[p].lane[l].st[g * BT_PAGE + c] = pg.st[l][c];
	}
	void clearPage(int p, int g) { writePage(p, g, BtPage()); }
	// Copies page g onto pages g+1 .. last. A lane that ends at the end of page g
	// is lengthened to the end of page last, so it plays the copies; a lane of any
	// other length keeps its length (a 12-step lane stays 12 steps).
	void duplicatePage(int p, int g, int last) {
		if (g < 0 || g >= last || last >= BT_NPAGE) return;
		BtPage pg = readPage(p, g);
		for (int t = g + 1; t <= last; t++) writePage(p, t, pg);
		for (int l = 0; l < BT_L; l++)
			if (pat[p].lane[l].len == (g + 1) * BT_PAGE) pat[p].lane[l].len = (last + 1) * BT_PAGE;
	}
	void euclid(int p, int l, int hits) {
		BtLane& ln = pat[p].lane[l];
		int L = clamp(ln.len, 1, BT_S);
		for (int i = 0; i < L; i++) ln.st[i].on = ((i * hits) % L) < hits;
	}
	void randomizeLane(int p, int l) {
		BtLane& ln = pat[p].lane[l];
		int L = clamp(ln.len, 1, BT_S);
		for (int i = 0; i < L; i++) {
			ln.st[i].on = random::uniform() < 0.4f;
			ln.st[i].vel = 0.5f + 0.5f * random::uniform();
		}
	}

	// ── saving ──────────────────────────────────────────────────────────
	json_t* dataToJson() override {
		json_t* r = json_object();
		json_t* ps = json_array();
		for (int p = 0; p < BT_P; p++) {
			json_t* jp = json_array();
			for (int l = 0; l < BT_L; l++) {
				const BtLane& ln = pat[p].lane[l];
				json_t* jl = json_object();
				json_object_set_new(jl, "len", json_integer(ln.len));
				json_object_set_new(jl, "mode", json_integer(ln.mode));
				json_object_set_new(jl, "div", json_integer(ln.div));
				std::string on, ra;
				bool anyV = false, anyC = false, anyR = false, anyX = false;
				for (int i = 0; i < BT_S; i++) {
					const BtStep& s = ln.st[i];
					on += s.on ? '1' : '0';
					ra += (char)('0' + clamp(s.ratch, 1, BT_MAXR));
					anyV |= s.vel != BT_VEL0; anyC |= s.chance != 1.f; anyR |= s.route != 0.f; anyX |= s.ratch != 1;
				}
				json_object_set_new(jl, "on", json_string(on.c_str()));
				if (anyX) json_object_set_new(jl, "ratch", json_string(ra.c_str()));
				auto arr = [&](const char* key, float BtStep::*f) {
					json_t* a = json_array();
					for (int i = 0; i < BT_S; i++) json_array_append_new(a, json_real(ln.st[i].*f));
					json_object_set_new(jl, key, a);
				};
				if (anyV) arr("vel", &BtStep::vel);
				if (anyC) arr("chance", &BtStep::chance);
				if (anyR) arr("route", &BtStep::route);
				json_array_append_new(jp, jl);
			}
			json_array_append_new(ps, jp);
		}
		json_object_set_new(r, "patterns", ps);
		json_t* lanes = json_array();
		for (int l = 0; l < BT_L; l++) {
			json_t* jl = json_object();
			json_object_set_new(jl, "mute", json_boolean(mute[l]));
			json_object_set_new(jl, "noteA", json_real(noteA[l]));
			json_object_set_new(jl, "noteB", json_real(noteB[l]));
			json_object_set_new(jl, "lockPass", json_integer(run[l].lockPass));
			json_object_set_new(jl, "lockWalk", json_integer(run[l].lockWalkStart));
			json_array_append_new(lanes, jl);
		}
		json_object_set_new(r, "lanes", lanes);
		json_object_set_new(r, "seed", json_integer((json_int_t)seed));
		json_object_set_new(r, "locked", json_boolean(locked));
		json_object_set_new(r, "cur", json_integer(cur));
		json_object_set_new(r, "nextCount", json_integer(nextCount));
		json_object_set_new(r, "gateMode", json_integer(gateMode));
		json_object_set_new(r, "resetOnRun", json_boolean(resetOnRun));
		json_object_set_new(r, "switchNow", json_boolean(switchNow));
		json_object_set_new(r, "editPattern", json_integer(editPattern));
		json_object_set_new(r, "tab", json_integer(tab));
		json_object_set_new(r, "page", json_integer(page));
		json_object_set_new(r, "viewFollow", json_boolean(viewFollow));
		return r;
	}

	void dataFromJson(json_t* r) override {
		if (json_t* ps = json_object_get(r, "patterns")) {
			for (int p = 0; p < BT_P; p++) {
				pat[p] = BtPattern();
				json_t* jp = json_array_get(ps, p);
				if (!jp) continue;
				for (int l = 0; l < BT_L; l++) {
					json_t* jl = json_array_get(jp, l);
					if (!jl) continue;
					BtLane& ln = pat[p].lane[l];
					if (json_t* j = json_object_get(jl, "len")) ln.len = clamp((int)json_integer_value(j), 1, BT_S);
					if (json_t* j = json_object_get(jl, "mode")) ln.mode = clamp((int)json_integer_value(j), 0, BT_NMODE - 1);
					if (json_t* j = json_object_get(jl, "div")) ln.div = clamp((int)json_integer_value(j), 0, BT_NDIV - 1);
					if (json_t* j = json_object_get(jl, "on")) {
						const char* s = json_string_value(j);
						for (int i = 0; s && i < BT_S && s[i]; i++) ln.st[i].on = s[i] == '1';
					}
					if (json_t* j = json_object_get(jl, "ratch")) {
						const char* s = json_string_value(j);
						for (int i = 0; s && i < BT_S && s[i]; i++) ln.st[i].ratch = clamp(s[i] - '0', 1, BT_MAXR);
					}
					auto arr = [&](const char* key, float BtStep::*f, float lo, float hi) {
						json_t* a = json_object_get(jl, key);
						if (!a) return;
						for (int i = 0; i < BT_S; i++)
							if (json_t* v = json_array_get(a, i)) ln.st[i].*f = clamp((float)json_number_value(v), lo, hi);
					};
					arr("vel", &BtStep::vel, 0.f, 1.f);
					arr("chance", &BtStep::chance, 0.f, 1.f);
					arr("route", &BtStep::route, 0.f, 1.f);
				}
			}
		}
		if (json_t* lanes = json_object_get(r, "lanes")) {
			for (int l = 0; l < BT_L; l++) {
				json_t* jl = json_array_get(lanes, l);
				if (!jl) continue;
				if (json_t* j = json_object_get(jl, "mute")) mute[l] = json_boolean_value(j);
				if (json_t* j = json_object_get(jl, "noteA")) noteA[l] = (float)json_number_value(j);
				if (json_t* j = json_object_get(jl, "noteB")) noteB[l] = (float)json_number_value(j);
				if (json_t* j = json_object_get(jl, "lockPass")) run[l].lockPass = (long)json_integer_value(j);
				if (json_t* j = json_object_get(jl, "lockWalk")) run[l].lockWalkStart = (int)json_integer_value(j);
			}
		}
		if (json_t* j = json_object_get(r, "seed")) seed = (uint32_t)json_integer_value(j);
		// the lock passes are restored above, so a locked pattern comes back as it was
		if (json_t* j = json_object_get(r, "locked")) locked = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "cur")) cur = clamp((int)json_integer_value(j), 0, BT_P - 1);
		if (json_t* j = json_object_get(r, "nextCount")) nextCount = (int)json_integer_value(j);
		if (json_t* j = json_object_get(r, "gateMode")) gateMode = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(r, "resetOnRun")) resetOnRun = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "switchNow")) switchNow = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "editPattern")) editPattern = clamp((int)json_integer_value(j), 0, BT_P - 1);
		if (json_t* j = json_object_get(r, "tab")) tab = clamp((int)json_integer_value(j), 0, 4);
		if (json_t* j = json_object_get(r, "page")) page = clamp((int)json_integer_value(j), 0, BT_NPAGE - 1);
		if (json_t* j = json_object_get(r, "viewFollow")) viewFollow = json_boolean_value(j);
	}
};

// =============================================================================
// The screen. Everything is laid out in the screen's own millimetres and scaled
// once. Tabs and page tabs across the top, then the eight lanes (number, sixteen
// cells, length / order / rate), then the patterns along the foot.
// =============================================================================
static const float BT_SW = 162.56f, BT_SH = 68.58f;   // the screen, mm
enum BtTab { BT_TAB_STEP, BT_TAB_VEL, BT_TAB_CHANCE, BT_TAB_ROUTE, BT_TAB_RATCH, BT_NTAB };
static const char* BT_TAB_NAME[BT_NTAB] = {"STEP", "VEL", "CHANCE", "ROUTE", "RATCH"};

static const float BT_TAB_X = 2.5f, BT_TAB_W = 14.f, BT_TAB_GAP = 1.f, BT_TAB_Y = 1.6f, BT_TAB_H = 5.f;
static const float BT_PG_X = 84.f, BT_PG_W = 6.f, BT_PG_P = 7.f;
static const float BT_GRID_Y = 9.0f, BT_LANE_P = 6.3f, BT_CELL_H = 5.3f;
static const float BT_NUM_X = 2.5f, BT_NUM_W = 6.5f;
static const float BT_CELL_X = 10.5f, BT_CELL_P = 7.55f, BT_CELL_W = 6.6f;
static const float BT_LEN_X = 132.f, BT_LEN_W = 8.f, BT_MODE_X = 140.5f, BT_MODE_W = 10.5f,
                   BT_DIV_X = 151.5f, BT_DIV_W = 8.5f;
static const float BT_FOOT_Y = 61.6f, BT_FOOT_H = 5.3f;

static const NVGcolor BT_A_COL = nvgRGB(0x00, 0x97, 0xDE);   // the A channel: blue
static const NVGcolor BT_B_COL = nvgRGB(0x3F, 0xBF, 0x6F);   // the B channel: green

static Battery* batteryPreview() {
	static Battery* m = nullptr;
	if (m) return m;
	m = new Battery();
	const std::vector<BtTemplate>& T = btTemplates();
	const BtTemplate* funk = nullptr;
	for (const BtTemplate& t : T) if (t.id == "funk.sixteen") funk = &t;
	if (funk) m->pat[0] = funk->roles[funk->mainIdx];
	else m->defaultPattern();
	sfs::previewConnect(m->inputs[Battery::CLOCK_INPUT], 1);
	rack::engine::Module::ProcessArgs a = sfs::previewArgs();
	int period = 6000;   // sixteenths at 120 BPM
	int n = 13 * period + period / 4;
	for (int i = 0; i < n; i++) {
		m->inputs[Battery::CLOCK_INPUT].setVoltage((i % period) < 240 ? 10.f : 0.f);
		a.frame = i;
		m->process(a);
	}
	return m;
}

struct BatteryDisplay : OpaqueWidget {
	Battery* module = nullptr;
	std::shared_ptr<Font> font;
	Vec dragPos;
	enum Drag { D_NONE, D_PAINT, D_VALUE, D_LEN, D_PATTERN };
	Drag drag = D_NONE;
	int dragLane = -1, dragStep = -1;
	bool paintState = false;
	float dragVal = 0.f;
	double holdUntil = 0.0;      // the view stays where a click put it until then

	float sc() const { return box.size.x / BT_SW; }

	// hit tests, in screen mm
	int hitTab(Vec p) {
		if (p.y < BT_TAB_Y || p.y > BT_TAB_Y + BT_TAB_H) return -1;
		for (int t = 0; t < BT_NTAB; t++) {
			float x = BT_TAB_X + t * (BT_TAB_W + BT_TAB_GAP);
			if (p.x >= x && p.x <= x + BT_TAB_W) return t;
		}
		return -1;
	}
	int hitPage(Vec p) {
		if (p.y < BT_TAB_Y || p.y > BT_TAB_Y + BT_TAB_H) return -1;
		for (int g = 0; g < BT_NPAGE; g++) {
			float x = BT_PG_X + g * BT_PG_P;
			if (p.x >= x && p.x <= x + BT_PG_W) return g;
		}
		return -1;
	}
	int hitLane(Vec p) {
		if (p.y < BT_GRID_Y || p.y >= BT_GRID_Y + BT_L * BT_LANE_P) return -1;
		return clamp((int)((p.y - BT_GRID_Y) / BT_LANE_P), 0, BT_L - 1);
	}
	int hitCol(Vec p) {
		if (p.x < BT_CELL_X || p.x >= BT_CELL_X + BT_PAGE * BT_CELL_P) return -1;
		return clamp((int)((p.x - BT_CELL_X) / BT_CELL_P), 0, BT_PAGE - 1);
	}
	int hitPattern(Vec p) {
		if (p.y < BT_FOOT_Y || p.y > BT_FOOT_Y + BT_FOOT_H) return -1;
		if (p.x < BT_CELL_X || p.x >= BT_CELL_X + BT_P * BT_CELL_P) return -1;
		return clamp((int)((p.x - BT_CELL_X) / BT_CELL_P), 0, BT_P - 1);
	}
	// where in a lane row: 0 number, 1 cells, 2 length, 3 order, 4 rate
	int laneZone(Vec p) {
		if (p.x >= BT_NUM_X && p.x < BT_NUM_X + BT_NUM_W) return 0;
		if (hitCol(p) >= 0) return 1;
		if (p.x >= BT_LEN_X && p.x < BT_LEN_X + BT_LEN_W) return 2;
		if (p.x >= BT_MODE_X && p.x < BT_MODE_X + BT_MODE_W) return 3;
		if (p.x >= BT_DIV_X && p.x < BT_DIV_X + BT_DIV_W) return 4;
		return -1;
	}
	float cellFrac(Vec p, int lane) {
		float y0 = BT_GRID_Y + lane * BT_LANE_P;
		return clamp(1.f - (p.y - y0) / BT_CELL_H, 0.f, 1.f);
	}

	void hold() { holdUntil = system::getTime() + 5.0; }

	BtLane& laneOf(int l) { return module->pat[module->editPattern].lane[l]; }

	void setValue(BtStep& s, float v) {
		switch (module->tab) {
			case BT_TAB_VEL: s.vel = v; break;
			case BT_TAB_CHANCE: s.chance = v; break;
			case BT_TAB_ROUTE: s.route = v; break;
			case BT_TAB_RATCH: s.ratch = clamp(1 + (int)(v * BT_MAXR), 1, BT_MAXR); break;
		}
		s.on = true;
	}
	float getValue(const BtStep& s) {
		switch (module->tab) {
			case BT_TAB_VEL: return s.vel;
			case BT_TAB_CHANCE: return s.chance;
			case BT_TAB_ROUTE: return s.route;
			case BT_TAB_RATCH: return (s.ratch - 0.5f) / BT_MAXR;
		}
		return 0.f;
	}

	void laneMenu(int l) {
		Battery* m = module;
		int p = m->editPattern;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("Lane %d, pattern %d", l + 1, p + 1)));
		menu->addChild(createIndexSubmenuItem("Order",
			std::vector<std::string>(BT_MODE_NAME, BT_MODE_NAME + BT_NMODE),
			[=]() { return m->pat[p].lane[l].mode; },
			[=](size_t i) { m->pat[p].lane[l].mode = (int)i; }));
		menu->addChild(createIndexSubmenuItem("Rate",
			std::vector<std::string>(BT_RATE_NAME, BT_RATE_NAME + BT_NDIV),
			[=]() { return m->pat[p].lane[l].div; },
			[=](size_t i) { m->pat[p].lane[l].div = (int)i; }));
		menu->addChild(createSubmenuItem("Length", string::f("%d", m->pat[p].lane[l].len), [=](ui::Menu* sm) {
			static const int LENS[] = {4, 8, 12, 16, 24, 32, 48, 64};
			for (int v : LENS)
				sm->addChild(createCheckMenuItem(string::f("%d steps", v), "",
					[=]() { return m->pat[p].lane[l].len == v; },
					[=]() { m->pat[p].lane[l].len = v; }));
			sm->addChild(createMenuLabel("Shift-click a step to end the lane there"));
		}));
		menu->addChild(createBoolPtrMenuItem("Mute", "", &m->mute[l]));
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Euclidean fill", "", [=](ui::Menu* sm) {
			int L = m->pat[p].lane[l].len;
			for (int k = 1; k <= std::min(L, 16); k++)
				sm->addChild(createMenuItem(string::f("%d hits over %d steps", k, L), "",
					[=]() { m->euclid(p, l, k); }));
		}));
		menu->addChild(createMenuItem("Rotate one step later", "", [=]() { m->rotateLane(p, l, 1); }));
		menu->addChild(createMenuItem("Rotate one step earlier", "", [=]() { m->rotateLane(p, l, -1); }));
		menu->addChild(createMenuItem("Randomize steps", "", [=]() { m->randomizeLane(p, l); }));
		menu->addChild(createMenuItem("Copy lane", "", [=]() { laneClip() = m->pat[p].lane[l]; laneClipSet() = true; }));
		if (laneClipSet())
			menu->addChild(createMenuItem("Paste lane", "", [=]() { m->pat[p].lane[l] = laneClip(); }));
		menu->addChild(createMenuItem("Clear lane", "", [=]() { m->pat[p].lane[l] = BtLane(); }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Notes on the V/OCT outs (the same in every pattern)"));
		menu->addChild(noteSlider(m, l, false));
		menu->addChild(noteSlider(m, l, true));
	}

	static BtLane& laneClip() { static BtLane c; return c; }
	static bool& laneClipSet() { static bool s = false; return s; }

	struct NoteQ : Quantity {
		Battery* m; int l; bool b;
		float* ref() { return b ? &m->noteB[l] : &m->noteA[l]; }
		void setValue(float v) override { *ref() = clamp(std::round(v), getMinValue(), getMaxValue()); }
		float getValue() override { return *ref(); }
		float getMinValue() override { return -36.f; }
		float getMaxValue() override { return 48.f; }
		float getDefaultValue() override { return b ? (float)(l + 8) : (float)l; }
		std::string getLabel() override { return b ? "Note B" : "Note A"; }
		std::string getDisplayValueString() override {
			static const char* N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
			int s = (int)std::round(getValue()) + 48;
			return string::f("%s%d", N[s % 12], s / 12);
		}
	};
	ui::Slider* noteSlider(Battery* m, int l, bool b) {
		ui::Slider* s = new ui::Slider;
		NoteQ* q = new NoteQ;
		q->m = m; q->l = l; q->b = b;
		s->quantity = q;
		s->box.size.x = 200.f;
		return s;
	}

	void patternMenu(int p) {
		Battery* m = module;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("Pattern %d", p + 1)));
		menu->addChild(createMenuItem("Play next", "", [=]() { m->selectPattern(p); }));
		menu->addChild(createMenuItem("Copy pattern", "", [=]() { patClip() = m->pat[p]; patClipSet() = true; }));
		if (patClipSet())
			menu->addChild(createMenuItem("Paste pattern", "", [=]() { m->pat[p] = patClip(); }));
		menu->addChild(createMenuItem("Clear pattern", "", [=]() { m->pat[p] = BtPattern(); }));
	}
	void pageMenu(int g) {
		Battery* m = module;
		int p = m->editPattern;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("Page %d of pattern %d (steps %d-%d, all eight lanes)",
			g + 1, p + 1, g * BT_PAGE + 1, (g + 1) * BT_PAGE)));
		menu->addChild(createMenuItem("Copy page", "", [=]() { pageClip() = m->readPage(p, g); pageClipSet() = true; }));
		if (pageClipSet())
			menu->addChild(createMenuItem("Paste onto this page", "", [=]() { m->writePage(p, g, pageClip()); }));
		if (g < BT_NPAGE - 1) {
			menu->addChild(createMenuItem(string::f("Duplicate onto page %d", g + 2), "",
				[=]() { m->duplicatePage(p, g, g + 1); }));
			if (g < BT_NPAGE - 2)
				menu->addChild(createMenuItem(string::f("Duplicate onto pages %d-%d", g + 2, BT_NPAGE), "",
					[=]() { m->duplicatePage(p, g, BT_NPAGE - 1); }));
			menu->addChild(createMenuLabel(string::f("Duplicating lengthens lanes that end at step %d to include the copies",
				(g + 1) * BT_PAGE)));
		}
		menu->addChild(createMenuItem("Clear page", "", [=]() { m->clearPage(p, g); }));
	}
	static Battery::BtPage& pageClip() { static Battery::BtPage c; return c; }
	static bool& pageClipSet() { static bool s = false; return s; }

	static BtPattern& patClip() { static BtPattern c; return c; }
	static bool& patClipSet() { static bool s = false; return s; }

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS) { OpaqueWidget::onButton(e); return; }
		Vec p = e.pos.div(sc());
		dragPos = e.pos;
		Battery* m = module;
		drag = D_NONE;
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			int l = hitLane(p);
			if (l >= 0 && laneZone(p) >= 0) { laneMenu(l); e.consume(this); return; }
			int pt = hitPattern(p);
			if (pt >= 0) { patternMenu(pt); e.consume(this); return; }
			int g = hitPage(p);
			if (g >= 0) { m->page = g; hold(); pageMenu(g); e.consume(this); return; }
			OpaqueWidget::onButton(e);
			return;
		}
		if (e.button != GLFW_MOUSE_BUTTON_LEFT) { OpaqueWidget::onButton(e); return; }
		e.consume(this);
		int t = hitTab(p);
		if (t >= 0) { m->tab = t; return; }
		int g = hitPage(p);
		if (g >= 0) { m->page = g; hold(); return; }
		int pt = hitPattern(p);
		if (pt >= 0) { m->editPattern = pt; hold(); drag = D_PATTERN; return; }
		int l = hitLane(p);
		if (l < 0) return;
		BtLane& ln = laneOf(l);
		switch (laneZone(p)) {
			case 0: m->mute[l] = !m->mute[l]; return;
			case 2: drag = D_LEN; dragLane = l; dragVal = (float)ln.len; hold(); return;
			case 3: ln.mode = (ln.mode + ((e.mods & GLFW_MOD_SHIFT) ? BT_NMODE - 1 : 1)) % BT_NMODE; return;
			case 4: ln.div = (ln.div + ((e.mods & GLFW_MOD_SHIFT) ? BT_NDIV - 1 : 1)) % BT_NDIV; return;
			case 1: break;
			default: return;
		}
		int s = m->page * BT_PAGE + hitCol(p);
		hold();
		if (e.mods & GLFW_MOD_SHIFT) { ln.len = s + 1; return; }
		BtStep& st = ln.st[s];
		dragLane = l; dragStep = s;
		if (m->tab == BT_TAB_STEP) {
			paintState = !st.on;
			st.on = paintState;
			drag = D_PAINT;
		}
		else {
			setValue(st, cellFrac(p, l));
			dragVal = getValue(st);
			drag = D_VALUE;
		}
	}

	void onDoubleClick(const DoubleClickEvent& e) override {
		if (module) {
			int pt = hitPattern(dragPos.div(sc()));
			if (pt >= 0) { module->selectPattern(pt); e.consume(this); return; }
		}
		OpaqueWidget::onDoubleClick(e);
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (!module) return;
		float zoom = getAbsoluteZoom();
		if (zoom <= 0.f) zoom = 1.f;
		Vec delta = e.mouseDelta.div(zoom);
		dragPos = dragPos.plus(delta);
		Vec p = dragPos.div(sc());
		Vec d = delta.div(sc());
		Battery* m = module;
		switch (drag) {
			case D_PAINT: {
				int l = hitLane(p), c = hitCol(p);
				if (l >= 0 && c >= 0) {
					int s = m->page * BT_PAGE + c;
					if (s < laneOf(l).len) laneOf(l).st[s].on = paintState;
				}
				break;
			}
			case D_VALUE: {
				if (dragLane < 0 || dragStep < 0) break;
				dragVal = clamp(dragVal - d.y / (BT_CELL_H * 2.f), 0.f, 1.f);
				setValue(laneOf(dragLane).st[dragStep], dragVal);
				break;
			}
			case D_LEN: {
				dragVal = clamp(dragVal - d.y * 0.5f, 1.f, (float)BT_S);
				laneOf(dragLane).len = (int)std::round(dragVal);
				break;
			}
			case D_PATTERN: {
				int pt = hitPattern(p);
				if (pt >= 0) m->editPattern = pt;
				break;
			}
			default: break;
		}
		hold();
		OpaqueWidget::onDragMove(e);
	}
	void onDragEnd(const DragEndEvent& e) override {
		drag = D_NONE; dragLane = dragStep = -1;
		OpaqueWidget::onDragEnd(e);
	}

	void step() override {
		Battery* m = module;
		if (m && m->viewFollow && system::getTime() > holdUntil) {
			m->editPattern = m->cur;
			const Battery::Run& r = m->run[m->masterLane()];
			if (r.pos >= 0) m->page = clamp(r.pos / BT_PAGE, 0, BT_NPAGE - 1);
		}
		OpaqueWidget::step();
	}

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, mm2px(1.27f));
		nvgFillColor(args.vg, sfs::SCREEN_BG);
		nvgFill(args.vg);
	}

	void rect(NVGcontext* vg, float x, float y, float w, float h, NVGcolor c, float r = 0.4f) {
		float s = sc();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, x * s, y * s, w * s, h * s, r * s);
		nvgFillColor(vg, c);
		nvgFill(vg);
	}
	void text(NVGcontext* vg, float x, float y, const std::string& t, NVGcolor c, int align,
	          float size = sfs::TYPE_SCREEN) {
		float s = sc();
		sfs::screenFont(vg, font, size);
		nvgTextAlign(vg, align);
		nvgFillColor(vg, c);
		nvgText(vg, x * s, y * s, t.c_str(), NULL);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!module) { module = batteryPreview(); drawLayer(args, layer); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		bool hasFont = font && font->handle >= 0;
		float s = sc();
		Battery* m = module;
		const BtPattern& P = m->pat[clamp(m->editPattern, 0, BT_P - 1)];
		NVGcolor off = sfs::SCREEN_PURP, offBeat = sfs::SCREEN_PMID;
		NVGcolor outside = sfs::screenTone(nvgRGB(0x22, 0x22, 0x3A));
		int middle = NVG_ALIGN_MIDDLE;

		// tabs and pages
		for (int t = 0; t < BT_NTAB; t++) {
			float x = BT_TAB_X + t * (BT_TAB_W + BT_TAB_GAP);
			bool sel = t == m->tab;
			rect(vg, x, BT_TAB_Y, BT_TAB_W, BT_TAB_H, sel ? sfs::SCREEN_DEEP : off);
			if (hasFont) text(vg, x + BT_TAB_W / 2, BT_TAB_Y + BT_TAB_H / 2, BT_TAB_NAME[t],
			                  sel ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM, NVG_ALIGN_CENTER | middle, sfs::TYPE_SCREEN_SMALL);
		}
		for (int g = 0; g < BT_NPAGE; g++) {
			float x = BT_PG_X + g * BT_PG_P;
			bool sel = g == m->page;
			// a page some lane is playing on is marked
			bool playing = false;
			if (m->editPattern == m->cur)
				for (int l = 0; l < BT_L; l++) if (m->run[l].pos >= 0 && m->run[l].pos / BT_PAGE == g) playing = true;
			bool reach = false;
			for (int l = 0; l < BT_L; l++) if (P.lane[l].len > g * BT_PAGE) reach = true;
			rect(vg, x, BT_TAB_Y, BT_PG_W, BT_TAB_H, sel ? sfs::SCREEN_DEEP : off);
			if (hasFont) text(vg, x + BT_PG_W / 2, BT_TAB_Y + BT_TAB_H / 2, string::f("%d", g + 1),
			                  playing ? sfs::SCREEN_HOT : reach ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM,
			                  NVG_ALIGN_CENTER | middle, sfs::TYPE_SCREEN_SMALL);
		}
		if (hasFont) {
			std::string h = string::f("PAT %d", m->cur + 1);
			if (m->target != m->cur) h += string::f(" > %d", m->target + 1);
			text(vg, BT_SW - 2.5f, BT_TAB_Y + BT_TAB_H / 2, h, sfs::SCREEN_TEXT, NVG_ALIGN_RIGHT | middle);
			text(vg, 116.f, BT_TAB_Y + BT_TAB_H / 2, m->locked ? "LOCKED" : "FREE",
			     m->locked ? sfs::SCREEN_HOT : sfs::SCREEN_DIM, NVG_ALIGN_LEFT | middle, sfs::TYPE_SCREEN_SMALL);
		}

		// lanes
		bool showPlay = m->editPattern == m->cur;
		for (int l = 0; l < BT_L; l++) {
			const BtLane& ln = P.lane[l];
			const Battery::Run& r = m->run[l];
			float y = BT_GRID_Y + l * BT_LANE_P;
			bool mu = m->muted(l);
			float age = (float)(m->now - r.fireT) / m->sr;
			float fl = clamp(1.f - age / 0.15f, 0.f, 1.f);
			if (hasFont) {
				NVGcolor nc = mu ? sfs::SCREEN_PURP : sfs::SCREEN_TEXT;
				if (fl > 0.f && !mu) nc = r.fireSide ? BT_B_COL : sfs::SCREEN_HOT;
				text(vg, BT_NUM_X + BT_NUM_W / 2, y + BT_CELL_H / 2, mu ? "M" : string::f("%d", l + 1), nc,
				     NVG_ALIGN_CENTER | middle);
			}
			for (int c = 0; c < BT_PAGE; c++) {
				int si = m->page * BT_PAGE + c;
				float x = BT_CELL_X + c * BT_CELL_P;
				const BtStep& st = ln.st[si];
				if (si >= ln.len) { rect(vg, x, y, BT_CELL_W, BT_CELL_H, outside); continue; }
				NVGcolor base = (c % 4 == 0) ? offBeat : off;
				if (!st.on) { rect(vg, x, y, BT_CELL_W, BT_CELL_H, base); }
				else if (m->tab == BT_TAB_STEP) {
					// A below, B above in proportion to the route; dimmer the less likely
					float a = 0.35f + 0.65f * st.chance;
					rect(vg, x, y, BT_CELL_W, BT_CELL_H, base);
					float hb = BT_CELL_H * st.route;
					if (st.route < 1.f) rect(vg, x, y + hb, BT_CELL_W, BT_CELL_H - hb, nvgTransRGBAf(BT_A_COL, a), 0.f);
					if (st.route > 0.f) rect(vg, x, y, BT_CELL_W, hb, nvgTransRGBAf(BT_B_COL, a), 0.f);
					if (st.ratch > 1)
						for (int k = 0; k < st.ratch; k++)
							rect(vg, x + 0.8f + k * 1.2f, y + BT_CELL_H - 1.1f, 0.7f, 0.6f, sfs::SCREEN_TEXT, 0.f);
				}
				else if (m->tab == BT_TAB_ROUTE) {
					float hb = BT_CELL_H * st.route;
					rect(vg, x, y, BT_CELL_W, BT_CELL_H, base);
					rect(vg, x, y + hb, BT_CELL_W, BT_CELL_H - hb, BT_A_COL, 0.f);
					rect(vg, x, y, BT_CELL_W, hb, BT_B_COL, 0.f);
					if (hasFont) text(vg, x + BT_CELL_W / 2, y + BT_CELL_H / 2, string::f("%d", (int)std::round(st.route * 100.f)),
					                  sfs::SCREEN_TEXT, NVG_ALIGN_CENTER | middle, 1.6f);
				}
				else if (m->tab == BT_TAB_RATCH) {
					rect(vg, x, y, BT_CELL_W, BT_CELL_H, nvgTransRGBAf(BT_A_COL, 0.35f));
					float bw = (BT_CELL_W - 1.f) / BT_MAXR;
					for (int k = 0; k < st.ratch; k++)
						rect(vg, x + 0.5f + k * bw + 0.15f, y + 0.6f, bw - 0.3f, BT_CELL_H - 1.2f, BT_A_COL, 0.f);
				}
				else {
					float v = m->tab == BT_TAB_VEL ? st.vel : st.chance;
					rect(vg, x, y, BT_CELL_W, BT_CELL_H, nvgTransRGBAf(BT_A_COL, 0.3f));
					rect(vg, x, y + BT_CELL_H * (1.f - v), BT_CELL_W, BT_CELL_H * v, BT_A_COL, 0.f);
				}
				// the playhead
				if (showPlay && r.pos == si) {
					if (fl > 0.f && !mu) rect(vg, x, y, BT_CELL_W, BT_CELL_H, nvgTransRGBAf(r.fireSide ? BT_B_COL : sfs::SCREEN_HOT, 0.85f * fl));
					nvgBeginPath(vg);
					nvgRoundedRect(vg, x * s, y * s, BT_CELL_W * s, BT_CELL_H * s, 0.4f * s);
					nvgStrokeColor(vg, sfs::SCREEN_HOT);
					nvgStrokeWidth(vg, 0.35f * s);
					nvgStroke(vg);
				}
			}
			if (hasFont) {
				float ym = y + BT_CELL_H / 2;
				text(vg, BT_LEN_X + BT_LEN_W / 2, ym, string::f("%d", ln.len), sfs::SCREEN_TEXT, NVG_ALIGN_CENTER | middle);
				text(vg, BT_MODE_X + BT_MODE_W / 2, ym, BT_MODE_SHORT[clamp(ln.mode, 0, BT_NMODE - 1)],
				     ln.mode == BT_FWD ? sfs::SCREEN_DIM : sfs::SCREEN_TEXT, NVG_ALIGN_CENTER | middle, sfs::TYPE_SCREEN_SMALL);
				text(vg, BT_DIV_X + BT_DIV_W / 2, ym, BT_RATE_NAME[clamp(ln.div, 0, BT_NDIV - 1)],
				     ln.div == BT_UNITY ? sfs::SCREEN_DIM : sfs::SCREEN_TEXT, NVG_ALIGN_CENTER | middle, sfs::TYPE_SCREEN_SMALL);
			}
		}

		// patterns
		for (int p = 0; p < BT_P; p++) {
			float x = BT_CELL_X + p * BT_CELL_P;
			bool ed = p == m->editPattern;
			rect(vg, x, BT_FOOT_Y, BT_CELL_W, BT_FOOT_H, ed ? sfs::SCREEN_DEEP : m->pat[p].used() ? offBeat : off);
			if (p == m->target && m->target != m->cur) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x * s, BT_FOOT_Y * s, BT_CELL_W * s, BT_FOOT_H * s, 0.4f * s);
				nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_HOT, 0.5f + 0.5f * std::sin((float)system::getTime() * 8.f)));
				nvgStrokeWidth(vg, 0.35f * s);
				nvgStroke(vg);
			}
			if (hasFont) text(vg, x + BT_CELL_W / 2, BT_FOOT_Y + BT_FOOT_H / 2, string::f("%d", p + 1),
			                  p == m->cur ? sfs::SCREEN_HOT : ed ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM,
			                  NVG_ALIGN_CENTER | middle);
		}
		if (hasFont) {
			float x = BT_CELL_X + BT_P * BT_CELL_P + 2.f, ym = BT_FOOT_Y + BT_FOOT_H / 2;
			rect(vg, x, ym - 1.2f, 2.4f, 2.4f, BT_A_COL);
			text(vg, x + 3.4f, ym, "A 1-8", sfs::SCREEN_DIM, NVG_ALIGN_LEFT | middle, sfs::TYPE_SCREEN_SMALL);
			rect(vg, x + 15.f, ym - 1.2f, 2.4f, 2.4f, BT_B_COL);
			text(vg, x + 18.4f, ym, "B 9-16", sfs::SCREEN_DIM, NVG_ALIGN_LEFT | middle, sfs::TYPE_SCREEN_SMALL);
			text(vg, BT_SW - 2.5f, ym, string::f("SEED %04X", (unsigned)(m->seed & 0xFFFF)), sfs::SCREEN_DIM,
			     NVG_ALIGN_RIGHT | middle, sfs::TYPE_SCREEN_SMALL);
		}
		OpaqueWidget::drawLayer(args, layer);
	}
};

// =============================================================================
// Panel: 34HP, code-drawn until the Figma pass. The screen across the top;
// under it three groups of pot/button-and-jack pairs (transport, patterns,
// randomness) and the outputs on a plate as a grid: GATE, VEL and V/OCT rows
// by ALL (16 channels), A and B (8 each) columns, with EOC beside them.
// =============================================================================
static const float BT_SX = 5.08f, BT_SY = 10.16f;
static const float BT_Y1 = 92.71f, BT_Y2 = 106.68f, BT_Y3 = 120.65f;
static const float BT_GX[6] = {10.16f, 20.32f, 43.18f, 53.34f, 76.2f, 86.36f};
static const float BT_DIV1 = 31.75f, BT_DIV2 = 64.77f;
static const float BT_OX[3] = {137.16f, 147.32f, 157.48f};
static const float BT_EOC_X = 111.76f;
static const float BT_ROWLBL_X = 131.6f;

struct BatteryWidget : ModuleWidget {
	BatteryWidget(Battery* module) {
		setModule(module);
		setPanel(sfs::themedPanel("battery"));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("BATTERY");

		BatteryDisplay* disp = new BatteryDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(BT_SX, BT_SY));
		disp->box.size = mm2px(Vec(BT_SW, BT_SH));
		addChild(disp);

		// transport
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
			mm2px(Vec(BT_GX[0], BT_Y1)), module, Battery::RUN_PARAM, Battery::RUN_LIGHT));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[1], BT_Y1)), module, Battery::RUN_INPUT));
		lbl->pair(BT_GX[0], BT_Y1, "RUN");
		addParam(createParamCentered<VCVButton>(mm2px(Vec(BT_GX[0], BT_Y2)), module, Battery::RESET_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[1], BT_Y2)), module, Battery::RESET_INPUT));
		lbl->pair(BT_GX[0], BT_Y2, "RESET");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[0], BT_Y3)), module, Battery::CLOCK_INPUT));
		lbl->jack(BT_GX[0], BT_Y3, "CLOCK");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[1], BT_Y3)), module, Battery::BAR_INPUT));
		lbl->jack(BT_GX[1], BT_Y3, "BAR");
		lbl->divider(BT_DIV1, 80.01f, BT_DIV1, 125.f);

		// patterns
		addParam(createParamCentered<Trimpot>(mm2px(Vec(BT_GX[2], BT_Y1)), module, Battery::PATTERN_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[3], BT_Y1)), module, Battery::PATTERN_INPUT));
		lbl->pair(BT_GX[2], BT_Y1, "PATTERN");
		addParam(createParamCentered<VCVButton>(mm2px(Vec(BT_GX[2], BT_Y2)), module, Battery::NEXT_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[3], BT_Y2)), module, Battery::NEXT_INPUT));
		lbl->pair(BT_GX[2], BT_Y2, "NEXT");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[2], BT_Y3)), module, Battery::MUTE_INPUT));
		lbl->jack(BT_GX[2], BT_Y3, "MUTE");
		lbl->divider(BT_DIV2, 80.01f, BT_DIV2, 125.f);

		// randomness
		addParam(createParamCentered<Trimpot>(mm2px(Vec(BT_GX[4], BT_Y1)), module, Battery::BIAS_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[5], BT_Y1)), module, Battery::BIAS_INPUT));
		lbl->pair(BT_GX[4], BT_Y1, "BIAS");
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
			mm2px(Vec(BT_GX[4], BT_Y2)), module, Battery::LOCK_PARAM, Battery::LOCK_LIGHT));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[5], BT_Y2)), module, Battery::LOCK_INPUT));
		lbl->pair(BT_GX[4], BT_Y2, "LOCK");
		addParam(createParamCentered<VCVButton>(mm2px(Vec(BT_GX[4], BT_Y3)), module, Battery::NEW_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(BT_GX[5], BT_Y3)), module, Battery::NEW_INPUT));
		lbl->pair(BT_GX[4], BT_Y3, "NEW");

		// outputs
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_EOC_X, BT_Y3)), module, Battery::EOC_OUTPUT));
		lbl->jackOnPlate(BT_EOC_X, BT_Y3, "EOC");
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_OX[0], BT_Y1)), module, Battery::GATE_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_OX[1], BT_Y1)), module, Battery::A_GATE_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_OX[2], BT_Y1)), module, Battery::B_GATE_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_OX[0], BT_Y2)), module, Battery::VEL_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_OX[1], BT_Y2)), module, Battery::A_VEL_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_OX[2], BT_Y2)), module, Battery::B_VEL_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_OX[0], BT_Y3)), module, Battery::VOCT_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_OX[1], BT_Y3)), module, Battery::A_VOCT_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(BT_OX[2], BT_Y3)), module, Battery::B_VOCT_OUTPUT));
		lbl->jackOnPlate(BT_OX[0], BT_Y1, "ALL");
		lbl->jackOnPlate(BT_OX[1], BT_Y1, "A");
		lbl->jackOnPlate(BT_OX[2], BT_Y1, "B");
		lbl->add(BT_ROWLBL_X, BT_Y1 + 0.83f, "GATE", sfs::PanelLabels::ON_PLATE, NVG_ALIGN_RIGHT);
		lbl->add(BT_ROWLBL_X, BT_Y2 + 0.83f, "VEL", sfs::PanelLabels::ON_PLATE, NVG_ALIGN_RIGHT);
		lbl->add(BT_ROWLBL_X, BT_Y3 + 0.83f, "V/OCT", sfs::PanelLabels::ON_PLATE, NVG_ALIGN_RIGHT);
	}

	void appendContextMenu(Menu* menu) override {
		Battery* m = dynamic_cast<Battery*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Templates", "", [=](ui::Menu* tm) {
			const std::vector<BtTemplate>& T = btTemplates();
			if (T.empty()) { tm->addChild(createMenuLabel("The template bank could not be read")); return; }
			tm->addChild(createMenuLabel("Grooves from Fill's bank. Swing the clock for the swung ones."));
			size_t k = 0;
			for (const BtTplGroup& g : btTplGroups()) {
				tm->addChild(new MenuSeparator);
				tm->addChild(createMenuLabel(g.heading));
				for (; k < T.size(); k++) {
					const BtTemplate& t = T[k];
					bool inGroup = false;
					for (const char* id : g.ids) if (t.id == id) inGroup = true;
					if (!inGroup) break;
					std::string right = t.swing > 0.f ? string::f("swing %d%%", (int)std::round(t.swing * 100.f)) : "";
					size_t ti = k;
					tm->addChild(createSubmenuItem(t.name, right, [=](ui::Menu* sm) {
						const BtTemplate& u = btTemplates()[ti];
						int ep = m->editPattern;
						sm->addChild(createMenuItem(string::f("Load the main groove into pattern %d (on screen)", ep + 1), "",
							[=]() { m->pat[ep] = btTemplates()[ti].roles[btTemplates()[ti].mainIdx]; }));
						if (u.roles.size() > 1) {
							std::string names;
							for (size_t r = 0; r < u.roleName.size(); r++) names += (r ? ", " : "") + u.roleName[r];
							sm->addChild(createMenuItem(string::f("Load every variation into patterns 1-%d (%s)", (int)u.roles.size(), names.c_str()), "",
								[=]() {
									const BtTemplate& v = btTemplates()[ti];
									for (size_t r = 0; r < v.roles.size(); r++) m->pat[r] = v.roles[r];
								}));
						}
					}));
				}
			}
		}));
		menu->addChild(createIndexPtrSubmenuItem("Gate length", {"Trigger (5 ms)", "Half a step"}, &m->gateMode));
		menu->addChild(createIndexSubmenuItem("When a new pattern starts",
			{"At the next BAR (or the end of the longest lane), every lane from step 1",
			 "On the next step, every lane keeping its position"},
			[=]() { return m->switchNow ? 1 : 0; },
			[=](size_t i) { m->switchNow = i == 1; }));
		menu->addChild(createBoolPtrMenuItem("Reset when RUN starts", "", &m->resetOnRun));
		menu->addChild(createBoolPtrMenuItem("Screen follows the playing pattern and page", "", &m->viewFollow));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Click a step to turn it on or off; in VEL, CHANCE, ROUTE and RATCH drag it up or down"));
		menu->addChild(createMenuLabel("Shift-click a step to end the lane there; right-click a lane or a page tab for its menu"));
		menu->addChild(createMenuLabel("Click a pattern to edit it, double-click to play it next"));
	}
};

Model* modelBattery = createModel<Battery, BatteryWidget>("Battery");

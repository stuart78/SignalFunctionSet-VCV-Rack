// =============================================================================
// Cistern: real spaces, modelled from their geometry.
//
// Named for the Dan Harpole Cistern at Fort Worden, where Deep Listening was
// recorded: a concrete tank 56.7 m across and 4.3 m high on a grid of columns,
// with a reverb quoted at 45 s. That room is built from its dimensions; the
// others are photogrammetry scans of real interiors (the Pantheon, Hintze Hall,
// a 300 m tunnel, churches, an undercroft, a mosque, a tower room), reduced to
// their air by tools/cistern-convert.cpp. The sources and the listener stand in
// the room, and the sound is what the geometry and its materials do: early
// reflections ray-traced on a worker, a late tail whose decay per band is the
// room's own (Eyring for the cistern, measured from rays for a scan, with ISO
// 9613-1 air absorption), and a calibrated handover between them. The engine is
// src/cistern-engine.hpp, free of Rack. Design: docs/cistern-design.md.
//
// The screen is the room in 3D at its true proportions, drawn as Crystal draws
// a habit: the room's edges in the display blue over faint glass. Drag the two
// sources (one per input channel) and the listener, shift-drag to lift them,
// drag empty space to orbit.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "waveguide.hpp"
#include "cistern-engine.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

using cistern::V3;
using cistern::NB;
using cistern::NSPACES;

// Each space loaded once for the whole plugin (a scan is half a megabyte and a
// BVH), from whichever thread asks first: the worker, the screen, or a new
// instance working out its default positions.
static std::shared_ptr<const cistern::Space> cisternSpace(int id) {
	static std::mutex mx;
	static std::shared_ptr<const cistern::Space> cache[NSPACES];
	id = std::max(0, std::min(NSPACES - 1, id));
	std::lock_guard<std::mutex> lk(mx);
	if (!cache[id]) {
		std::shared_ptr<cistern::Space> s = std::make_shared<cistern::Space>();
		std::string err;
		if (!cistern::makeSpace(id, asset::plugin(pluginInstance, "res/spaces"), *s, err)) {
			WARN("Cistern: %s: %s; using the cistern", cistern::SPACES[id].name, err.c_str());
			*s = cistern::makeCistern();
		}
		cache[id] = s;
	}
	return cache[id];
}

struct Cistern : Module {
	enum ParamId { SIZE_PARAM, DECAY_PARAM, SCATTER_PARAM, HUMID_PARAM, MOD_PARAM, MIX_PARAM, FREEZE_PARAM,
	               SPACE_PARAM, PARAMS_LEN };
	enum InputId { SIZE_INPUT, DECAY_INPUT, SCATTER_INPUT, HUMID_INPUT, MOD_INPUT, MIX_INPUT, FREEZE_INPUT,
	               IN_L_INPUT, IN_R_INPUT, SPACE_INPUT, INPUTS_LEN };
	enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, OUTPUTS_LEN };
	enum LightId { FREEZE_LIGHT, LIGHTS_LEN };

	static constexpr float DECAY_MIN = -3.321928f;      // log2(0.1)
	static constexpr float YAW0 = 0.55f, PITCH0 = 0.42f;

	// Where things stand, PER SPACE (a spot in the Pantheon means nothing in a
	// tunnel), normalised to that space's bounds: x and z from -1 to 1, y from 0
	// to 1, so a SIZE change takes them with it. Dragged on the screen; saved.
	enum { P_SRC_L, P_SRC_R, P_LIS, NPOS };
	float pos[NSPACES][NPOS][3] = {};
	bool posSet[NSPACES] = {};
	std::mutex posMutex;                                  // the screen and the worker both fill in defaults
	std::atomic<uint32_t> posGen{1};
	float camYaw = YAW0, camPitch = PITCH0;
	bool trueDistance = false;                            // menu: far sources pulled in (musical) or where they are

	// what process() wants built, published for the worker
	std::atomic<float> wSize{0.f}, wDecay{0.f}, wScatter{0.5f}, wHumid{50.f}, wSr{0.f};
	std::atomic<int> wSpace{0};

	// The engine. The audio thread owns `rv`; the worker builds programs (and a
	// whole Reverb the first time, or after a sample-rate change) and hands them
	// over through the atomics; what the audio thread lets go of goes back the
	// same way to be freed, so nothing is allocated or freed in process().
	cistern::Reverb* rv = nullptr;
	std::atomic<cistern::Reverb*> newRv{nullptr}, oldRv{nullptr};
	// A SPACE change starts a new reverb and leaves the last one RINGING OUT in
	// its own room, fed nothing, until it is inaudible. Retargeting the one tail
	// to the new room (as SIZE does) glided its delay lines to the new lengths,
	// and between rooms that is a pitch bend lasting seconds.
	std::atomic<bool> newRvRing{false};
	cistern::Reverb* ringing = nullptr;
	float ringGain = 1.f, ringEnv = 0.f, ringStep = 0.f;
	bool ringFading = false;
	std::atomic<cistern::Program*> incoming{nullptr}, retired{nullptr};
	std::thread worker;
	std::atomic<bool> running{false}, tracing{false};

	// for the screen, written as each program is published
	std::atomic<float> dT60[NB], dBase[NB];
	std::atomic<float> dX{56.7f}, dY{4.27f}, dZ{56.7f}, dVol{0.f}, dTMix{0.f};
	std::atomic<int> dSpace{0};
	std::atomic<float> inEnv{0.f};

	int ctl = 0;
	float env = 0.f, mixNow = 0.5f;
	bool frozen = false;

	struct SizeQ : ParamQuantity {
		std::string getDisplayValueString() override {
			float k = getDisplayValue();
			Cistern* m = dynamic_cast<Cistern*>(module);
			if (!m) return string::f("%.2fx", k);
			std::shared_ptr<const cistern::Space> s = cisternSpace(m->wSpace.load());
			V3 e = s->hi - s->lo;
			return string::f("%.2fx (%.1f x %.1f m, %.1f m high)", k, e.x * k, e.z * k, e.y * k);
		}
	};
	struct DecayQ : ParamQuantity {
		std::string getDisplayValueString() override {
			float k = getDisplayValue();
			Cistern* m = dynamic_cast<Cistern*>(module);
			float b = m ? m->dBase[2].load() : 0.f;
			if (b <= 0.f) return string::f("%.2fx", k);
			return string::f("%.2fx (%.1f s at 500 Hz)", k, b * k);
		}
	};

	Cistern() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<SizeQ>(SIZE_PARAM, -2.f, 1.f, 0.f, "Size, against the real room", "", 2.f);
		configParam<DecayQ>(DECAY_PARAM, DECAY_MIN, 2.f, 0.f, "Decay, against the room's own", "", 2.f);
		configParam(SCATTER_PARAM, 0.f, 1.f, 0.5f, "Scatter (how much of each reflection goes diffuse)", "%", 0.f, 100.f);
		configParam(HUMID_PARAM, 10.f, 90.f, 50.f, "Humidity (drier air absorbs more high frequencies)", "%");
		configParam(MOD_PARAM, 0.f, 2.f, 1.f, "Movement in the tail", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix", "% wet", 0.f, 100.f);
		configSwitch(FREEZE_PARAM, 0.f, 1.f, 0.f, "Freeze", {"Off", "On"});
		std::vector<std::string> names;
		for (int i = 0; i < NSPACES; i++) names.push_back(cistern::SPACES[i].name);
		configSwitch(SPACE_PARAM, 0.f, (float)(NSPACES - 1), 0.f, "Space", names);
		configInput(SIZE_INPUT, "Size CV (1 V = 0.3 octave of size)");
		configInput(DECAY_INPUT, "Decay CV (1 V = half an octave)");
		configInput(SCATTER_INPUT, "Scatter CV (±5 V = the whole range)");
		configInput(HUMID_INPUT, "Humidity CV (1 V = 8%)");
		configInput(MOD_INPUT, "Movement CV (±5 V = the whole range)");
		configInput(MIX_INPUT, "Mix CV (±5 V = the whole range)");
		configInput(FREEZE_INPUT, "Freeze gate (high holds the tail and shuts the input out)");
		configInput(SPACE_INPUT, "Space CV (1 V per space)");
		configInput(IN_L_INPUT, "Left (the L source)");
		configInput(IN_R_INPUT, "Right (the R source; normalled to left)");
		configOutput(OUT_L_OUTPUT, "Left");
		configOutput(OUT_R_OUTPUT, "Right");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		for (int b = 0; b < NB; b++) { dT60[b] = 0.f; dBase[b] = 0.f; }
		startWorker();
	}
	~Cistern() override {
		stopWorker();
		freeReverb(rv); rv = nullptr;
		freeReverb(ringing); ringing = nullptr;
		freeReverb(newRv.exchange(nullptr));
		freeReverb(oldRv.exchange(nullptr));
		delete incoming.exchange(nullptr);
		delete retired.exchange(nullptr);
	}
	static void freeReverb(cistern::Reverb* r) {
		if (!r) return;
		if (r->prev != r->cur) delete r->prev;
		delete r->cur;
		delete r;
	}

	// ── positions ───────────────────────────────────────────────────────────
	static void toNorm(const cistern::Space& s, const V3& w, float* n) {
		n[0] = (float)(2 * (w.x - s.lo.x) / (s.hi.x - s.lo.x) - 1);
		n[1] = (float)((w.y - s.lo.y) / (s.hi.y - s.lo.y));
		n[2] = (float)(2 * (w.z - s.lo.z) / (s.hi.z - s.lo.z) - 1);
	}
	static V3 toWorld(const cistern::Space& s, const float* n) {
		return V3(s.lo.x + (n[0] + 1) * 0.5 * (s.hi.x - s.lo.x),
		          s.lo.y + n[1] * (s.hi.y - s.lo.y),
		          s.lo.z + (n[2] + 1) * 0.5 * (s.hi.z - s.lo.z));
	}
	// can something stand at this normalised position in space `id`
	static bool standable(int id, const float* n) {
		if (std::fabs(n[0]) > 0.99f || std::fabs(n[2]) > 0.99f || n[1] < 0.01f || n[1] > 0.99f) return false;
		std::shared_ptr<const cistern::Space> s = cisternSpace(id);
		return s->inside(toWorld(*s, n));
	}
	// The defaults: the space's own source position split into a pair 3 m
	// apart on the listener's left and right (closer if a wall is in the way).
	void defaultPositions(int id, float out[NPOS][3]) {
		std::shared_ptr<const cistern::Space> s = cisternSpace(id);
		V3 lis = s->lis, mid = s->src;
		V3 face = mid - lis; face.y = 0; face = face.norm();
		V3 side(face.z, 0, -face.x);                         // the listener's right
		toNorm(*s, lis, out[P_LIS]);
		for (double half : {1.5, 0.8, 0.3, 0.0}) {
			V3 a = mid - side * half, b = mid + side * half;
			if (s->inside(a) && s->inside(b)) { toNorm(*s, a, out[P_SRC_L]); toNorm(*s, b, out[P_SRC_R]); return; }
		}
		toNorm(*s, mid, out[P_SRC_L]); toNorm(*s, mid, out[P_SRC_R]);
	}
	void ensurePositions(int id) {
		std::lock_guard<std::mutex> lk(posMutex);
		if (posSet[id]) return;
		defaultPositions(id, pos[id]);
		posSet[id] = true;
	}
	void resetPositions(int id) {
		float d[NPOS][3];
		defaultPositions(id, d);
		std::lock_guard<std::mutex> lk(posMutex);
		std::memcpy(pos[id], d, sizeof(d));
		posSet[id] = true;
		posGen++;
	}
	// the room at a SIZE (log2 of the scale against the real one)
	static cistern::Space spaceFor(int id, float sizeLog2) {
		return cistern::scaledCopy(*cisternSpace(id), std::exp2((double)sizeLog2));
	}

	// ── the worker ──────────────────────────────────────────────────────────
	void startWorker() {
		running = true;
		worker = std::thread([this]() { workerLoop(); });
	}
	void stopWorker() {
		running = false;
		if (worker.joinable()) worker.join();
	}
	void collect() {
		delete retired.exchange(nullptr);
		freeReverb(oldRv.exchange(nullptr));
	}
	void publishStats(int id, const cistern::Space& s, const cistern::Program& P, const cistern::Program& traced) {
		for (int b = 0; b < NB; b++) { dT60[b] = (float)P.t60[b]; dBase[b] = (float)traced.t60[b]; }
		V3 e = s.hi - s.lo;
		dX = (float)e.x; dY = (float)e.y; dZ = (float)e.z; dVol = (float)s.volume; dTMix = (float)P.tMix;
		dSpace = id;
	}
	// Build what the panel asks for whenever it changes. A DECAY change reuses
	// the last trace (the early field does not depend on it); a moved source
	// reuses the last decay measurement (the room's decay does not depend on
	// where anyone stands); anything else traces again. Coalesces: a knob turned
	// while a trace runs is picked up at its latest value when the trace ends.
	void workerLoop() {
		const std::chrono::milliseconds ms5(5), ms10(10);
		std::unique_ptr<cistern::Program> traced;
		cistern::Space space;
		float tSize = 1e9f, tScatter = -1.f, tHumid = -1.f, tDecay = 1e9f, tSr = 0.f, rvSr = 0.f;
		int tSpace = -1, rvSpace = -1;
		uint32_t tPos = 0;
		while (running) {
			collect();
			float sr = wSr.load();
			if (sr <= 0.f) { std::this_thread::sleep_for(ms10); continue; }
			float size = wSize, sc = wScatter, hu = wHumid, de = wDecay;
			int id = clamp(wSpace.load(), 0, NSPACES - 1);
			uint32_t pg = posGen.load();
			bool roomSame = traced && sr == tSr && id == tSpace && std::fabs(size - tSize) <= 0.004f
			                && std::fabs(sc - tScatter) <= 0.01f && std::fabs(hu - tHumid) <= 0.5f;
			bool retrace = !roomSame || pg != tPos;
			bool fresh = sr != rvSr || id != rvSpace;      // a new reverb, not a new program for this one
			bool redecay = retrace || std::fabs(de - tDecay) > 0.004f || fresh;
			if (!redecay) { std::this_thread::sleep_for(ms10); continue; }
			// the audio thread has not taken the last one yet
			if (!fresh && incoming.load()) { std::this_thread::sleep_for(ms5); continue; }
			tracing = true;
			if (retrace) {
				if (!roomSame) space = spaceFor(id, size);
				ensurePositions(id);
				float p[NPOS][3];
				{
					std::lock_guard<std::mutex> lk(posMutex);
					std::memcpy(p, pos[id], sizeof(p));
				}
				// a position that cannot be stood in (a patch saved by another
				// build, say) falls back to the space's default
				float d[NPOS][3]; bool haveD = false;
				for (int i = 0; i < NPOS; i++)
					if (!standable(id, p[i])) {
						if (!haveD) { defaultPositions(id, d); haveD = true; }
						std::memcpy(p[i], d[i], sizeof(p[i]));
					}
				V3 src[2] = {toWorld(space, p[P_SRC_L]), toWorld(space, p[P_SRC_R])};
				V3 lis = toWorld(space, p[P_LIS]);
				cistern::BuildParams bp;
				bp.rh = hu; bp.scatter = sc; bp.trueDistance = trueDistance;
				if (space.mesh) bp.rays = 60000;            // two sources on two threads: ~0.6 s in the largest scans
				traced = cistern::trace(space, bp, sr, src, 2, lis, roomSame ? traced.get() : nullptr);
				tSize = size; tScatter = sc; tHumid = hu; tPos = pg; tSr = sr; tSpace = id;
			}
			std::unique_ptr<cistern::Program> P = cistern::assemble(*traced, std::exp2((double)de));
			tDecay = de;
			publishStats(id, space, *P, *traced);
			if (fresh) {
				// a fresh reverb: the first one, after a rate change (the old one
				// is dropped), or in another space (the old one rings out there)
				delete incoming.exchange(nullptr);
				cistern::Reverb* n = new cistern::Reverb();
				n->setProgram(P.release(), 1.0, 7);
				while (running && newRv.load()) { collect(); std::this_thread::sleep_for(ms5); }
				if (!running) { freeReverb(n); break; }
				newRvRing = sr == rvSr && rvSpace >= 0;
				newRv = n;
				rvSr = sr; rvSpace = id;
			} else {
				cistern::Program* expect = nullptr;
				cistern::Program* p = P.get();
				while (running && !incoming.compare_exchange_strong(expect, p)) { expect = nullptr; collect(); std::this_thread::sleep_for(ms5); }
				if (running) { cistern::Program* handed = P.release(); (void)handed; }   // `incoming` owns it now
			}
			tracing = false;
		}
		tracing = false;
	}

	// fade whatever is ringing out (tests, and a reset)
	void dropRinging() { ringFading = true; }

	// The preview has no worker: one trace, on the caller's thread, for the
	// screen's readout.
	void buildNow() {
		ensurePositions(0);
		cistern::Space s = spaceFor(0, 0.f);
		V3 src[2] = {toWorld(s, pos[0][P_SRC_L]), toWorld(s, pos[0][P_SRC_R])};
		cistern::BuildParams bp; bp.rays = 20000;
		auto T = cistern::trace(s, bp, 48000, src, 2, toWorld(s, pos[0][P_LIS]));
		auto P = cistern::assemble(*T, 1.0);
		publishStats(0, s, *P, *T);
	}

	float pv(int p, int in, float scale) {
		float v = params[p].getValue();
		if (inputs[in].isConnected()) v += inputs[in].getVoltage() * scale;
		return v;
	}

	void process(const ProcessArgs& args) override {
		if (--ctl <= 0) {
			ctl = 32;
			wSize = clamp(pv(SIZE_PARAM, SIZE_INPUT, 0.3f), -2.f, 1.f);
			wDecay = clamp(pv(DECAY_PARAM, DECAY_INPUT, 0.5f), DECAY_MIN, 2.f);
			wScatter = clamp(pv(SCATTER_PARAM, SCATTER_INPUT, 0.1f), 0.f, 1.f);
			wHumid = clamp(pv(HUMID_PARAM, HUMID_INPUT, 8.f), 10.f, 90.f);
			wSpace = clamp((int)std::lround(pv(SPACE_PARAM, SPACE_INPUT, 1.f)), 0, NSPACES - 1);
			wSr = args.sampleRate;
			mixNow = clamp(pv(MIX_PARAM, MIX_INPUT, 0.1f), 0.f, 1.f);
			frozen = params[FREEZE_PARAM].getValue() > 0.5f || inputs[FREEZE_INPUT].getVoltage() >= 1.f;
			lights[FREEZE_LIGHT].setBrightness(frozen ? 1.f : 0.f);
			inEnv = env;
			// take what the worker built: a whole reverb, or a program for this one
			cistern::Reverb* n = newRv.load();
			if (n && !oldRv.load()) {
				if (newRvRing.load() && rv) {
					// the room changed: the old reverb rings out, unless one is already
					// ringing, which first fades in 100 ms to make room
					if (!ringing) {
						newRv = nullptr; ringing = rv; rv = n;
						ringing->freeze = false;
						ringGain = 1.f; ringEnv = 1.f; ringFading = false;
						ringStep = 1.f / (0.1f * args.sampleRate);
					} else ringFading = true;
				} else { newRv = nullptr; oldRv = rv; rv = n; }
			}
			if (rv) {
				cistern::Program* p = incoming.load();
				if (p && p->sr == rv->sr && !rv->fading() && !retired.load() && incoming.compare_exchange_strong(p, nullptr))
					rv->adopt(p);
				rv->setMod(clamp(pv(MOD_PARAM, MOD_INPUT, 0.2f), 0.f, 2.f));
				rv->freeze = frozen;
			}
		}
		float inL = inputs[IN_L_INPUT].getVoltage();
		float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() : inL;
		float a = std::max(std::fabs(inL), std::fabs(inR)) * 0.2f;
		env = a > env ? a : env * 0.9995f;
		float wl = 0.f, wr = 0.f;
		if (rv) {
			rv->process(inL, inR, wl, wr);
			if (const cistern::Program* q = rv->finished()) retired = const_cast<cistern::Program*>(q);
		}
		if (ringing) {
			float a2, b2;
			ringing->process(0.f, 0.f, a2, b2);
			wl += ringGain * a2; wr += ringGain * b2;
			float pk = std::max(std::fabs(a2), std::fabs(b2)) * ringGain;
			ringEnv = pk > ringEnv ? pk : ringEnv * 0.9999f;
			if (ringFading) ringGain -= ringStep;
			// gone below -94 dBV (or faded to make room): back to the worker to free
			if ((ringGain <= 0.f || ringEnv < 2e-5f) && !oldRv.load()) { oldRv = ringing; ringing = nullptr; }
		}
		float m = mixNow;
		outputs[OUT_L_OUTPUT].setVoltage(sfs::softClip((1.f - m) * inL + m * wl));
		outputs[OUT_R_OUTPUT].setVoltage(sfs::softClip((1.f - m) * inR + m * wr));
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_t* all = json_object();
		std::lock_guard<std::mutex> lk(posMutex);
		for (int s = 0; s < NSPACES; s++) {
			if (!posSet[s]) continue;
			json_t* p = json_array();
			for (int i = 0; i < NPOS; i++) for (int k = 0; k < 3; k++) json_array_append_new(p, json_real(pos[s][i][k]));
			json_object_set_new(all, cistern::SPACES[s].slug, p);
		}
		json_object_set_new(r, "positions", all);
		json_object_set_new(r, "camYaw", json_real(camYaw));
		json_object_set_new(r, "camPitch", json_real(camPitch));
		json_object_set_new(r, "trueDistance", json_boolean(trueDistance));
		return r;
	}
	void dataFromJson(json_t* r) override {
		json_t* all = json_object_get(r, "positions");
		std::lock_guard<std::mutex> lk(posMutex);
		auto read = [&](int s, json_t* p) {
			if (!p || json_array_size(p) != NPOS * 3) return;
			for (int i = 0; i < NPOS; i++) for (int k = 0; k < 3; k++)
				pos[s][i][k] = (float)json_number_value(json_array_get(p, i * 3 + k));
			posSet[s] = true;
		};
		if (json_is_array(all)) read(0, all);              // the first builds: the cistern's alone
		else if (json_is_object(all))
			for (int s = 0; s < NSPACES; s++) read(s, json_object_get(all, cistern::SPACES[s].slug));
		posGen++;
		if (json_t* j = json_object_get(r, "camYaw")) camYaw = (float)json_number_value(j);
		if (json_t* j = json_object_get(r, "camPitch")) camPitch = clamp((float)json_number_value(j), 0.05f, 1.5f);
		if (json_t* j = json_object_get(r, "trueDistance")) trueDistance = json_boolean_value(j);
	}
};

// =============================================================================
// The screen: the room in 3D at its true proportions. Display units are the
// room's largest half-extent across the floor, centred, floor at y = 0, so a
// SIZE change redraws nothing (it reads in the header) and every room fills
// the screen. Orbit by dragging empty space (as Strata and Flock); the sources
// and the listener drag across the floor, and lift with shift.
// =============================================================================
struct CisternView {
	float yaw = 0.f, pitch = 0.4f;
	float sx = 1.f, ox = 0.f, oy = 0.f;
	void rotate(float x, float y, float z, float& px, float& py, float& d) const {
		float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
		float x1 = x * cy + z * sy, z1 = -x * sy + z * cy;
		px = x1; py = y * cp + z1 * sp; d = z1 * cp - y * sp;
	}
	void project(float x, float y, float z, float& X, float& Y, float& d) const {
		float px, py; rotate(x, y, z, px, py, d);
		X = ox + px * sx; Y = oy - py * sx;
	}
};

static Cistern* cisternPreview() {
	static Cistern* m = nullptr;
	if (m) return m;
	m = new Cistern();
	m->stopWorker();
	m->buildNow();
	m->inEnv = 0.5f;
	return m;
}

struct CisternDisplay;
// The room, drawn into a framebuffer: redrawn only when the space or the view
// changes, since a few thousand shaded faces every frame is a cost the markers
// over it do not need to pay.
struct CisternRoomLayer : Widget {
	CisternDisplay* d = nullptr;
	void draw(const DrawArgs& args) override;
};

struct CisternDisplay : OpaqueWidget {
	Cistern* module = nullptr;
	std::shared_ptr<Font> font;
	Vec pressPos; float dragDist = 0.f;
	int dragPos = -1, hoverPos = -1;
	// the CREDITS button (drawn under the room's size, top right) and the list it opens
	bool showCredits = false, hoverCredits = false;
	Rect creditsBtn;
	// the room drawn, and its frame: rebuilt when SPACE changes
	int shownId = -1;
	std::shared_ptr<const cistern::Space> room;
	double cx = 0, cz = 0, rn = 1, y0 = 0;
	// What the screen draws, in display units. The cistern: its geometry as a
	// model, faces facing into the air, with its EDGES (where two faces meet at
	// more than FACET_DEG, or the surface ends) and the silhouette per view. A
	// scan: section lines cut through its surface (buildSections). Both drawn as
	// Crystal draws, thin lines in the display blue, nearer brighter.
	static constexpr float FACET_DEG = 12.f;                 // flatter than this is one facet: no line
	struct MFace { int v[3]; float sn[3], gn[3]; uint8_t kind; };   // smooth and facet normals; kind 0 floor, 1 wall, 2 ceiling
	struct MEdge { int a, b, f0, f1; bool fold; float w; };   // w: how sharp, 0..1
	std::vector<float> mv;                                    // vertices x y z
	std::vector<MFace> mf;
	std::vector<MEdge> me;
	std::unordered_map<long long, int> vkey;                  // the cistern's vertices, shared by position
	struct Seg { float a[3], b[3]; uint8_t kind; };          // kind: 0 cross-section, 1 long section, 2 plan
	std::vector<Seg> segs;
	FramebufferWidget* fb = nullptr;
	CisternRoomLayer* layer = nullptr;
	int fbId = -1; float fbYaw = 1e9f, fbPitch = 1e9f; Vec fbSize;

	CisternDisplay() {
		fb = new FramebufferWidget;
		layer = new CisternRoomLayer;
		layer->d = this;
		fb->addChild(layer);
		addChild(fb);
	}
	// the module being shown: the real one, or the browser's preview instance
	Cistern* M() const { return module ? module : cisternPreview(); }
	int spaceId() const { return clamp(M()->wSpace.load(), 0, NSPACES - 1); }
	void sync() {
		int id = spaceId();
		if (id == shownId && room) return;
		shownId = id;
		room = cisternSpace(id);
		cx = 0.5 * (room->lo.x + room->hi.x); cz = 0.5 * (room->lo.z + room->hi.z);
		rn = std::max(0.5 * (room->hi.x - room->lo.x), 0.5 * (room->hi.z - room->lo.z));
		y0 = room->lo.y;
		M()->ensurePositions(id);
		buildModel();
	}
	int vert(const V3& w) {
		float x = (float)((w.x - cx) / rn), y = (float)((w.y - y0) / rn), z = (float)((w.z - cz) / rn);
		long long kx = (long long)std::lround(x * 20000) + (1 << 20), ky = (long long)std::lround(y * 20000) + (1 << 20), kz = (long long)std::lround(z * 20000) + (1 << 20);
		long long key = (kx << 42) ^ (ky << 21) ^ kz;
		auto it = vkey.find(key);
		if (it != vkey.end()) return it->second;
		int n = (int)(mv.size() / 3);
		mv.push_back(x); mv.push_back(y); mv.push_back(z);
		vkey[key] = n;
		return n;
	}
	// a face, wound so its normal faces the air (`smooth` says which way that is)
	void face(int a, int b, int c, V3 smooth, int kind = -1) {
		auto P = [&](int i) { return V3(mv[i * 3], mv[i * 3 + 1], mv[i * 3 + 2]); };
		V3 n = (P(b) - P(a)).cross(P(c) - P(a));
		if (n.len() < 1e-12) return;
		if (smooth.len() < 0.5) smooth = n.norm();
		if (n.dot(smooth) < 0) std::swap(b, c);
		smooth = smooth.norm();
		if (n.dot(smooth) < 0) n = n * -1.0;
		n = n.norm();
		MFace f; f.v[0] = a; f.v[1] = b; f.v[2] = c;
		f.sn[0] = (float)smooth.x; f.sn[1] = (float)smooth.y; f.sn[2] = (float)smooth.z;
		f.gn[0] = (float)n.x; f.gn[1] = (float)n.y; f.gn[2] = (float)n.z;
		f.kind = (uint8_t)(kind >= 0 ? kind : smooth.y > 0.7 ? 0 : smooth.y < -0.5 ? 2 : 1);
		mf.push_back(f);
	}
	void buildEdges() {
		me.clear();
		std::unordered_map<long long, int> at;
		for (int f = 0; f < (int)mf.size(); f++)
			for (int k = 0; k < 3; k++) {
				int a = mf[f].v[k], b = mf[f].v[(k + 1) % 3];
				long long key = ((long long)std::min(a, b) << 32) | (unsigned)std::max(a, b);
				auto it = at.find(key);
				if (it == at.end()) { at[key] = (int)me.size(); me.push_back({std::min(a, b), std::max(a, b), f, -1, false, 1.f}); }
				else if (me[it->second].f1 < 0) me[it->second].f1 = f;
			}
		// a line between two facets that are not one plane, weighted by how
		// sharply they meet; the surface's end (an opening's rim) at full weight
		for (MEdge& e : me) {
			if (e.f1 < 0) { e.fold = true; e.w = 1.f; continue; }
			const float* p = mf[e.f0].gn; const float* q = mf[e.f1].gn;
			float ang = std::acos(clamp(p[0] * q[0] + p[1] * q[1] + p[2] * q[2], -1.f, 1.f)) * 180.f / (float)M_PI;
			const float facet = FACET_DEG;                 // a copy: std::max binds a reference, and C++11 wants a definition for that
			float fd = std::max(facet, room->foldDeg);
			e.fold = ang > fd;
			e.w = clamp((ang - fd) / 35.f, 0.35f, 1.f);
		}
	}
	void buildModel() {
		mv.clear(); mf.clear(); me.clear(); vkey.clear(); segs.clear();
		const cistern::Space& s = *room;
		if (s.clean) {
			// a built room: its own triangles, vertices shared by position, so
			// its edges are its real corners (a column's arrises, a beam's
			// soffit, where the floor meets the wall) and nothing else
			for (const std::vector<cistern::Tri>* L : {&s.tris, &s.drawTris})
				for (const cistern::Tri& T : *L) {
					int a = vert(T.a), b = vert(T.a + T.e1), c = vert(T.a + T.e2);
					face(a, b, c, T.n, T.n.y > 0.75 ? 0 : T.n.y < -0.75 ? 2 : 1);
				}
			buildEdges();
			return;
		}
		// A scan, drawn as an architectural drawing is: the room cut by a march of
		// vertical sections across its long axis (a nave becomes its arches, a
		// tunnel its rings, a dome its circles), one section along its length,
		// and its plan a metre above the floor. Cut from the full surface, so
		// they follow the real vaults; openings are left out, so a doorway or an
		// oculus is a gap in the line. A reduced mesh's own edges described the
		// scan's noise rather than the building.
		buildSections();
	}
	// a chained line: dropped if shorter than `minLen`, else smoothed and kept
	void emit(std::vector<V3> pts, double minLen, uint8_t kind) {
		double len = 0;
		for (size_t k = 1; k < pts.size(); k++) len += (pts[k] - pts[k - 1]).len();
		if (pts.size() < 2 || len < minLen) return;
		bool closed = (pts.front() - pts.back()).len() < 1e-3;
		for (int it = 0; it < 2; it++) {
			std::vector<V3> sm;
			size_t m = pts.size();
			if (!closed) sm.push_back(pts.front());
			for (size_t k = 0; k + 1 < m; k++) {
				sm.push_back(pts[k] * 0.75 + pts[k + 1] * 0.25);
				sm.push_back(pts[k] * 0.25 + pts[k + 1] * 0.75);
			}
			if (closed) sm.push_back(sm.front()); else sm.push_back(pts.back());
			pts.swap(sm);
		}
		for (size_t k = 1; k < pts.size(); k++) {
			Seg sg;
			const V3 &p0 = pts[k - 1], &p1 = pts[k];
			sg.a[0] = (float)((p0.x - cx) / rn); sg.a[1] = (float)((p0.y - y0) / rn); sg.a[2] = (float)((p0.z - cz) / rn);
			sg.b[0] = (float)((p1.x - cx) / rn); sg.b[1] = (float)((p1.y - y0) / rn); sg.b[2] = (float)((p1.z - cz) / rn);
			sg.kind = kind;
			segs.push_back(sg);
		}
	}
	// chain loose segments that meet end to end into lines, and emit them
	void chainAndEmit(const std::vector<V3>& pa, const std::vector<V3>& pb, double minLen, uint8_t kind) {
		std::unordered_map<long long, std::vector<int>> ends;
		auto key = [](const V3& v) {
			return ((long long)std::lround(v.x * 1000) & 0x1fffff) << 42 | ((long long)std::lround(v.y * 1000) & 0x1fffff) << 21 | ((long long)std::lround(v.z * 1000) & 0x1fffff);
		};
		int ns = (int)pa.size();
		for (int i = 0; i < ns; i++) { ends[key(pa[i])].push_back(i); ends[key(pb[i])].push_back(i); }
		std::vector<char> used(ns, 0);
		for (int i0 = 0; i0 < ns; i0++) {
			if (used[i0]) continue;
			used[i0] = 1;
			std::deque<V3> line = {pa[i0], pb[i0]};
			for (int dir = 0; dir < 2; dir++)
				for (;;) {
					V3 tip = dir == 0 ? line.back() : line.front();
					int next = -1;
					for (int j : ends[key(tip)]) if (!used[j]) { next = j; break; }
					if (next < 0) break;
					used[next] = 1;
					V3 far = key(pa[next]) == key(tip) ? pb[next] : pa[next];
					if (dir == 0) line.push_back(far); else line.push_front(far);
				}
			emit(std::vector<V3>(line.begin(), line.end()), minLen, kind);
		}
	}
	// The footprint: the outline of every column that holds air, drawn at the
	// floor under it, so it follows a floor that slopes or a tunnel that bends,
	// where a flat cut a metre up missed most of Leake Street.
	void buildFootprint(double L) {
		const cistern::Space& s = *room;
		int gx = s.ggx, gz = s.ggz;
		std::vector<float> fl((size_t)gx * gz, -1e9f);
		for (int z = 0; z < gz; z++) for (int x = 0; x < gx; x++)
			for (int y = 0; y < s.ggy; y++) if (s.airAt(x, y, z)) { fl[(size_t)z * gx + x] = (float)(s.glo.y + y * s.gcell); break; }
		auto has = [&](int x, int z) { return x >= 0 && z >= 0 && x < gx && z < gz && fl[(size_t)z * gx + x] > -1e8f; };
		auto floorAt = [&](int x, int z) { return fl[(size_t)z * gx + x]; };
		std::vector<V3> pa, pb;
		for (int z = -1; z < gz; z++) for (int x = -1; x < gx; x++) {
			int c = (has(x, z) ? 1 : 0) | (has(x + 1, z) ? 2 : 0) | (has(x + 1, z + 1) ? 4 : 0) | (has(x, z + 1) ? 8 : 0);
			if (c == 0 || c == 15) continue;
			// edge midpoints between column centres, at the floor of the air side
			auto mid = [&](int e) {
				int ax, az, bx, bz;
				if (e == 0) { ax = x; az = z; bx = x + 1; bz = z; }
				else if (e == 1) { ax = x + 1; az = z; bx = x + 1; bz = z + 1; }
				else if (e == 2) { ax = x; az = z + 1; bx = x + 1; bz = z + 1; }
				else { ax = x; az = z; bx = x; bz = z + 1; }
				float y = has(ax, az) ? floorAt(ax, az) : floorAt(bx, bz);
				return V3(s.glo.x + (0.5 * (ax + bx) + 0.5) * s.gcell, y + 0.05, s.glo.z + (0.5 * (az + bz) + 0.5) * s.gcell);
			};
			static const int T[16][4] = {{-1,-1,-1,-1},{3,0,-1,-1},{0,1,-1,-1},{3,1,-1,-1},{1,2,-1,-1},{3,0,1,2},{0,2,-1,-1},{3,2,-1,-1},
			                             {2,3,-1,-1},{2,0,-1,-1},{0,1,2,3},{2,1,-1,-1},{1,3,-1,-1},{1,0,-1,-1},{0,3,-1,-1},{-1,-1,-1,-1}};
			for (int q = 0; q < 4 && T[c][q] >= 0; q += 2) { pa.push_back(mid(T[c][q])); pb.push_back(mid(T[c][q + 1])); }
		}
		chainAndEmit(pa, pb, 0.04 * L, 2);
	}
	void buildSections() {
		segs.clear();
		const cistern::Space& s = *room;
		double ex = s.hi.x - s.lo.x, ez = s.hi.z - s.lo.z;
		int longAxis = ex >= ez ? 0 : 2, crossAxis = 2 - longAxis;
		double L = std::max(ex, ez);
		auto cut = [&](int axis, double c, uint8_t kind) {
			std::vector<V3> pa, pb;
			for (const cistern::Tri& T : s.tris) {
				if (T.mat == cistern::M_OPEN) continue;
				V3 v[3] = {T.a, T.a + T.e1, T.a + T.e2};
				double d[3] = {v[0].at(axis) - c, v[1].at(axis) - c, v[2].at(axis) - c};
				if ((d[0] > 0) == (d[1] > 0) && (d[1] > 0) == (d[2] > 0)) continue;
				V3 p[2]; int n = 0;
				for (int k = 0; k < 3 && n < 2; k++) {
					int k2 = (k + 1) % 3;
					if ((d[k] > 0) == (d[k2] > 0)) continue;
					double t = d[k] / (d[k] - d[k2]);
					p[n++] = v[k] + (v[k2] - v[k]) * t;
				}
				if (n < 2) continue;
				pa.push_back(p[0]); pb.push_back(p[1]);
			}
			chainAndEmit(pa, pb, 0.1 * L, kind);
		};
		const int N = 9;
		double lo = s.lo.at(longAxis), hi = s.hi.at(longAxis);
		for (int i = 1; i <= N; i++) cut(longAxis, lo + (hi - lo) * i / (N + 1), 0);
		// the long section through the room's centre of air, not its bounding box
		double sum = 0; long cnt = 0;
		for (int z = 0; z < s.ggz; z++) for (int y = 0; y < s.ggy; y++) for (int x = 0; x < s.ggx; x++)
			if (s.airAt(x, y, z)) { sum += crossAxis == 0 ? s.glo.x + (x + 0.5) * s.gcell : s.glo.z + (z + 0.5) * s.gcell; cnt++; }
		cut(crossAxis, cnt ? sum / cnt : 0.5 * (s.lo.at(crossAxis) + s.hi.at(crossAxis)), 1);
		buildFootprint(L);
	}
	float heightTop() const { return (float)((room->hi.y - y0) / rn); }
	float halfX() const { return (float)(0.5 * (room->hi.x - room->lo.x) / rn); }
	float halfZ() const { return (float)(0.5 * (room->hi.z - room->lo.z) / rn); }
	void world(int i, float& x, float& y, float& z) const {
		V3 w = Cistern::toWorld(*room, M()->pos[shownId][i]);
		x = (float)((w.x - cx) / rn); y = (float)((w.y - y0) / rn); z = (float)((w.z - cz) / rn);
	}
	CisternView view() const {
		CisternView V; V.yaw = M()->camYaw; V.pitch = M()->camPitch;
		float ht = heightTop(), hx = halfX(), hz = halfZ();
		float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
		bool round = !room->mesh;
		for (int k = 0; k < 48; k++) {
			float x, z;
			if (round) { float a = 2 * M_PI * k / 48; x = std::cos(a); z = std::sin(a); }
			else { if (k >= 4) break; x = (k & 1) ? hx : -hx; z = (k & 2) ? hz : -hz; }
			for (float y : {0.f, ht}) {
				float px, py, d; V.rotate(x, y, z, px, py, d);
				mnx = std::min(mnx, px); mxx = std::max(mxx, px); mny = std::min(mny, py); mxy = std::max(mxy, py);
			}
		}
		float pad = mm2px(3.f), top = mm2px(8.5f), bot = mm2px(6.f);
		float w = box.size.x - 2 * pad, h = box.size.y - top - bot;
		V.sx = std::min(w / (mxx - mnx), h / (mxy - mny));
		V.ox = pad + w / 2.f - (mnx + mxx) / 2.f * V.sx;
		V.oy = top + h / 2.f + (mny + mxy) / 2.f * V.sx;
		return V;
	}
	int posAt(Vec p) const {
		if (!module || !room) return -1;
		CisternView V = view();
		int best = -1; float bd = mm2px(3.f);
		for (int i = 0; i < Cistern::NPOS; i++) {
			float x, y, z, X, Y, d; world(i, x, y, z);
			V.project(x, y, z, X, Y, d);
			float dist = std::hypot(X - p.x, Y - p.y);
			if (dist < bd) { bd = dist; best = i; }
		}
		return best;
	}
	// the floor under a marker, in display units: the lowest air in its column
	float floorUnder(int i) const {
		if (!room->mesh) return 0.f;
		V3 w = Cistern::toWorld(*room, M()->pos[shownId][i]);
		int gx = (int)std::floor((w.x - room->glo.x) / room->gcell), gz = (int)std::floor((w.z - room->glo.z) / room->gcell);
		int gy = (int)std::floor((w.y - room->glo.y) / room->gcell);
		while (gy > 0 && room->airAt(gx, gy - 1, gz)) gy--;
		return (float)((room->glo.y + gy * room->gcell - y0) / rn);
	}

	void onButton(const ButtonEvent& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS && (showCredits || creditsBtn.contains(e.pos))) {
			showCredits = !showCredits;
			dragPos = -1; dragDist = 1e9f;                    // not a drag, not an orbit
			e.consume(this);
			return;
		}
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
			pressPos = e.pos; dragDist = 0.f;
			dragPos = posAt(e.pos);
			e.consume(this);
		}
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT && e.action == GLFW_PRESS && module) {
			e.consume(this);
			Cistern* m = module;
			int id = spaceId();
			ui::Menu* menu = createMenu();
			menu->addChild(createMenuLabel("Drag a source or the listener; shift-drag lifts it"));
			menu->addChild(createMenuItem("Reset the positions", "", [=]() { m->resetPositions(id); }));
			menu->addChild(createMenuItem("Reset the view", "", [=]() { m->camYaw = Cistern::YAW0; m->camPitch = Cistern::PITCH0; }));
		}
	}
	void onHover(const HoverEvent& e) override {
		hoverCredits = creditsBtn.contains(e.pos);
		hoverPos = showCredits ? -1 : posAt(e.pos);
		OpaqueWidget::onHover(e);
	}
	void onLeave(const LeaveEvent& e) override { hoverPos = -1; hoverCredits = false; OpaqueWidget::onLeave(e); }
	void onDragMove(const DragMoveEvent& e) override {
		if (!module || !room || e.button != GLFW_MOUSE_BUTTON_LEFT) return;
		float z = getAbsoluteZoom();
		dragDist += std::hypot(e.mouseDelta.x, e.mouseDelta.y) / z;
		if (dragPos >= 0) {
			// the inverse of the camera's yaw and pitch, so the marker follows
			// the pointer from any angle: across the floor, or up with shift
			CisternView V = view();
			float a = e.mouseDelta.x / z / V.sx, b = -e.mouseDelta.y / z / V.sx;
			float cy = std::cos(V.yaw), sy = std::sin(V.yaw), cp = std::cos(V.pitch), sp = std::sin(V.pitch);
			// display units to world metres to normalised
			double ex = room->hi.x - room->lo.x, ey = room->hi.y - room->lo.y, ez = room->hi.z - room->lo.z;
			float n[3];
			std::memcpy(n, module->pos[shownId][dragPos], sizeof(n));
			if ((APP->window->getMods() & RACK_MOD_MASK) == GLFW_MOD_SHIFT) {
				n[1] += (float)(b / std::max(std::fabs(cp), 0.2f) * rn / ey);
			} else {
				float bb = b / (std::fabs(sp) < 0.2f ? std::copysign(0.2f, sp) : sp);
				n[0] += (float)((a * cy - bb * sy) * rn * 2 / ex);
				n[2] += (float)((a * sy + bb * cy) * rn * 2 / ez);
			}
			// only where something can stand: against a wall it stops
			if (Cistern::standable(shownId, n)) {
				std::lock_guard<std::mutex> lk(module->posMutex);
				std::memcpy(module->pos[shownId][dragPos], n, sizeof(n));
				module->posGen++;
			}
			return;
		}
		if (dragDist < 3.f) return;
		module->camYaw += e.mouseDelta.x / z * 0.01f;
		module->camPitch = clamp(module->camPitch + e.mouseDelta.y / z * 0.01f, 0.05f, 1.5f);
	}
	void onDragEnd(const DragEndEvent& e) override { dragPos = -1; }
	void onDoubleClick(const DoubleClickEvent& e) override {
		if (module) { module->camYaw = Cistern::YAW0; module->camPitch = Cistern::PITCH0; }
		e.consume(this);
	}

	void step() override {
		sync();
		fb->box.size = box.size;
		layer->box.size = box.size;
		Cistern* m = M();
		if (shownId != fbId || m->camYaw != fbYaw || m->camPitch != fbPitch || !box.size.equals(fbSize)) {
			fbId = shownId; fbYaw = m->camYaw; fbPitch = m->camPitch; fbSize = box.size;
			fb->setDirty();
		}
		OpaqueWidget::step();
	}

	// Where the room's picture goes: the screen (NanoVG) or, in the harness, an
	// SVG, so a preview is drawn by exactly this code.
	struct RoomSink {
		virtual void fill(const float* xy, int n, NVGcolor c) = 0;
		virtual void line(float x0, float y0, float x1, float y1, NVGcolor c, float w, float t) = 0;   // t: nearness 0..1
		virtual ~RoomSink() {}
	};
	void paintRoom(RoomSink& out) {
		if (!room) return;
		CisternView V = view();
		// depth across the room, for the near-is-brighter cue
		float dmin = 1e9f, dmax = -1e9f;
		auto depth = [&](const float* p) { float X, Y, d; V.project(p[0], p[1], p[2], X, Y, d); return d; };
		int nv = (int)(mv.size() / 3);
		for (int i = 0; i < nv; i++) { float d = depth(&mv[i * 3]); dmin = std::min(dmin, d); dmax = std::max(dmax, d); }
		for (const Seg& sg : segs) { float d = depth(sg.a); dmin = std::min(dmin, d); dmax = std::max(dmax, d); }
		float drange = std::max(dmax - dmin, 1e-4f);
		// SOLID colours, dimmed towards the background with distance: a
		// translucent line brightens wherever it crosses another, and a dense
		// part of the room turned into a bright knot
		auto ink = [&](float t, float boost) { return nvgLerpRGBA(sfs::SCREEN_BG, sfs::SCREEN_BLUE, clamp(0.22f + 0.6f * t + boost, 0.f, 1.f)); };
		auto width = [&](float t) { return 0.4f + 0.35f * t; };
		if (!room->clean) {
			for (const Seg& sg : segs) {
				float Xa, Ya, da, Xb, Yb, db;
				V.project(sg.a[0], sg.a[1], sg.a[2], Xa, Ya, da);
				V.project(sg.b[0], sg.b[1], sg.b[2], Xb, Yb, db);
				float t = clamp((dmax - 0.5f * (da + db)) / drange, 0.f, 1.f);
				out.line(Xa, Ya, Xb, Yb, ink(t, sg.kind == 2 ? 0.12f : sg.kind == 1 ? 0.05f : 0.f), width(t), t);
			}
			return;
		}
		// a built room: the floor as one dim surface, then its edges (rims,
		// columns, beams) and the silhouette for this view
		float cyw = std::cos(V.yaw), syw = std::sin(V.yaw), cp = std::cos(V.pitch), sp = std::sin(V.pitch);
		std::vector<float> X(nv), Y(nv), D(nv);
		for (int i = 0; i < nv; i++) V.project(mv[i * 3], mv[i * 3 + 1], mv[i * 3 + 2], X[i], Y[i], D[i]);
		std::vector<float> nd(mf.size());
		for (size_t f = 0; f < mf.size(); f++) {
			const float* n = mf[f].gn;
			nd[f] = (-n[0] * syw + n[2] * cyw) * cp - n[1] * sp;
		}
		for (size_t f = 0; f < mf.size(); f++) {
			if (mf[f].kind != 0 || nd[f] > 0) continue;
			const MFace& m = mf[f];
			float xy[6] = {X[m.v[0]], Y[m.v[0]], X[m.v[1]], Y[m.v[1]], X[m.v[2]], Y[m.v[2]]};
			out.fill(xy, 3, nvgLerpRGBA(sfs::SCREEN_BG, sfs::SCREEN_BLUE, 0.10f));
		}
		// Drawn by hand: each edge in short pieces, nudged by a smooth noise of
		// where it is in the room (so the wobble holds still as the view turns,
		// and two lines that meet at a corner still meet). A scan's lines wobble
		// with its own surface; a built room's would otherwise be ruled.
		for (const MEdge& e : me) {
			bool sil = e.f1 >= 0 && ((nd[e.f0] < 0) != (nd[e.f1] < 0));
			if (!e.fold && !sil) continue;
			const float* A = &mv[e.a * 3]; const float* B = &mv[e.b * 3];
			float len = std::sqrt((B[0] - A[0]) * (B[0] - A[0]) + (B[1] - A[1]) * (B[1] - A[1]) + (B[2] - A[2]) * (B[2] - A[2]));
			int n = clamp((int)std::ceil(len / HAND_STEP), 1, 24);
			float px = 0, py = 0, pd = 0;
			for (int k = 0; k <= n; k++) {
				float u = (float)k / n, p[3];
				for (int i = 0; i < 3; i++) p[i] = A[i] + (B[i] - A[i]) * u;
				float w[3]; handWobble(p, w);
				float qx, qy, qd;
				V.project(p[0] + w[0], p[1] + w[1], p[2] + w[2], qx, qy, qd);
				if (k > 0) {
					float t = clamp((dmax - 0.5f * (pd + qd)) / drange, 0.f, 1.f);
					out.line(px, py, qx, qy, ink(t, 0.f), width(t), t);
				}
				px = qx; py = qy; pd = qd;
			}
		}
	}
	// a pen's wobble: smooth 3D value noise, two octaves, in display units
	static constexpr float HAND_AMP = 0.005f, HAND_SCALE = 22.f, HAND_STEP = 0.015f;
	static float hash3(int x, int y, int z, int c) {
		uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u ^ (uint32_t)c * 2654435761u;
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		return (h & 0xffffff) / 16777215.f * 2.f - 1.f;
	}
	static float vnoise(float x, float y, float z, int c) {
		int ix = (int)std::floor(x), iy = (int)std::floor(y), iz = (int)std::floor(z);
		float fx = x - ix, fy = y - iy, fz = z - iz;
		fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy); fz = fz * fz * (3 - 2 * fz);
		float r = 0;
		for (int k = 0; k < 8; k++) {
			float w = ((k & 1) ? fx : 1 - fx) * ((k & 2) ? fy : 1 - fy) * ((k & 4) ? fz : 1 - fz);
			r += w * hash3(ix + (k & 1), iy + ((k >> 1) & 1), iz + ((k >> 2) & 1), c);
		}
		return r;
	}
	static void handWobble(const float* p, float* w) {
		for (int c = 0; c < 3; c++) {
			float s = HAND_SCALE;
			w[c] = HAND_AMP * (vnoise(p[0] * s, p[1] * s, p[2] * s, c) + 0.5f * vnoise(p[0] * s * 2.3f, p[1] * s * 2.3f, p[2] * s * 2.3f, c + 7));
		}
	}
	// the screen's sink: fills without anti-aliasing (adjacent faces would
	// otherwise overlap at their fringes and draw the triangulation), lines
	// gathered into a few depth bins and stroked far to near, so a nearer line
	// lies on top of a farther one
	struct NvgSink : RoomSink {
		NVGcontext* vg;
		static const int BINS = 10;
		std::vector<float> bin[BINS];
		float binW[BINS] = {}; NVGcolor binC[BINS];
		explicit NvgSink(NVGcontext* v) : vg(v) { nvgShapeAntiAlias(vg, 0); }
		void fill(const float* xy, int n, NVGcolor c) override {
			nvgBeginPath(vg);
			for (int i = 0; i < n; i++) { if (i == 0) nvgMoveTo(vg, xy[0], xy[1]); else nvgLineTo(vg, xy[i * 2], xy[i * 2 + 1]); }
			nvgClosePath(vg);
			nvgFillColor(vg, c); nvgFill(vg);
		}
		void line(float x0, float y0, float x1, float y1, NVGcolor c, float w, float t) override {
			int b = clamp((int)(t * BINS), 0, BINS - 1);
			bin[b].push_back(x0); bin[b].push_back(y0); bin[b].push_back(x1); bin[b].push_back(y1);
			binW[b] = w; binC[b] = c;
		}
		void flush() {
			nvgShapeAntiAlias(vg, 1);
			for (int b = 0; b < BINS; b++) {
				if (bin[b].empty()) continue;
				nvgBeginPath(vg);
				for (size_t i = 0; i + 3 < bin[b].size(); i += 4) { nvgMoveTo(vg, bin[b][i], bin[b][i + 1]); nvgLineTo(vg, bin[b][i + 2], bin[b][i + 3]); }
				nvgStrokeColor(vg, binC[b]); nvgStrokeWidth(vg, binW[b]); nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
			}
		}
	};
	void drawRoom(NVGcontext* vg) {
		NvgSink sink(vg);
		paintRoom(sink);
		sink.flush();
	}

	void ring(NVGcontext* vg, const CisternView& V, float rcx, float rcz, float r, float y, int n = 96) {
		nvgBeginPath(vg);
		for (int k = 0; k <= n; k++) {
			float a = 2 * M_PI * k / n, X, Y, d;
			V.project(rcx + r * std::cos(a), y, rcz + r * std::sin(a), X, Y, d);
			if (k == 0) nvgMoveTo(vg, X, Y); else nvgLineTo(vg, X, Y);
		}
	}
	static std::string secs(float t) { return t >= 10.f ? string::f("%.0fs", t) : string::f("%.1fs", t); }

	// The credits, over the room: every scan's source and licence (CC BY asks
	// for the name, the work and a link), the room on screen brighter, and a
	// line for the rooms built from published dimensions. Any click closes it.
	void drawCredits(NVGcontext* vg, float w, float h) {
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, mm2px(1.27f));
		nvgFillColor(vg, nvgTransRGBAf(sfs::SCREEN_BG, 0.94f)); nvgFill(vg);
		if (!font || font->handle < 0) return;
		sfs::screenFont(vg, font);
		float x = mm2px(3.f), y = mm2px(3.f), lw = w - 2 * x;
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
		nvgFontSize(vg, mm2px(2.6f));
		nvgFillColor(vg, sfs::SCREEN_TEXT);
		nvgText(vg, x, y, "CREDITS", NULL);
		y += mm2px(4.6f);
		nvgFontSize(vg, mm2px(1.9f));
		nvgTextLineHeight(vg, 1.15f);
		for (int i = 0; i < NSPACES; i++) {
			const cistern::SpaceInfo& I = cistern::SPACES[i];
			if (!I.credit[0]) continue;
			nvgFillColor(vg, i == shownId ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM);
			float b[4];
			nvgTextBoxBounds(vg, x, y, lw, I.credit, NULL, b);
			nvgTextBox(vg, x, y, lw, I.credit, NULL);
			y = b[3] + mm2px(0.9f);
		}
		y += mm2px(1.2f);
		nvgFillColor(vg, sfs::SCREEN_DIM);
		nvgTextBox(vg, x, y, lw, "Built from published dimensions: the Dan Harpole Cistern, Fort Worden (its interior after "
		                          "Centrum's photographs), the Buffalo Bayou Cistern, Houston, and the Basilica Cistern, "
		                          "Istanbul. Where a detail is estimated, docs/cistern-design.md says so.", NULL);
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
		nvgFillColor(vg, nvgTransRGBAf(sfs::SCREEN_DIM, 0.7f));
		nvgText(vg, w - mm2px(2.f), h - mm2px(1.4f), "click to close", NULL);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		Cistern* m = M();
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, mm2px(1.27f));
		nvgFillColor(vg, sfs::SCREEN_BG); nvgFill(vg);
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		sync();
		nvgSave(vg); nvgScissor(vg, 0, 0, w, h);
		Widget::draw(args);                                   // the room, from its framebuffer
		CisternView V = view();

		// the sources and the listener: drop lines to the floor for height, the
		// direct paths faint, the listener's two ears drawn as its facing
		float P[Cistern::NPOS][3], F[Cistern::NPOS];
		for (int i = 0; i < Cistern::NPOS; i++) { world(i, P[i][0], P[i][1], P[i][2]); F[i] = floorUnder(i); }
		auto proj = [&](const float* p, float& X, float& Y) { float d; V.project(p[0], p[1], p[2], X, Y, d); };
		for (int i = 0; i < Cistern::NPOS; i++) {
			float X, Y, Xf, Yf, d;
			proj(P[i], X, Y); V.project(P[i][0], F[i], P[i][2], Xf, Yf, d);
			nvgBeginPath(vg); nvgMoveTo(vg, X, Y); nvgLineTo(vg, Xf, Yf);
			nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_TEXT, 0.55f)); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
			ring(vg, V, P[i][0], P[i][2], 0.02f, F[i], 24);
			nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_TEXT, 0.55f)); nvgStroke(vg);
		}
		float Xl, Yl; proj(P[Cistern::P_LIS], Xl, Yl);
		for (int i = 0; i < 2; i++) {
			float X, Y; proj(P[i], X, Y);
			nvgBeginPath(vg); nvgMoveTo(vg, X, Y); nvgLineTo(vg, Xl, Yl);
			nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_HOT, 0.35f)); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		}
		float env = clamp(m->inEnv.load(), 0.f, 1.f);
		for (int i = 0; i < 2; i++) {
			float X, Y; proj(P[i], X, Y);
			float r = mm2px(hoverPos == i || dragPos == i ? 1.6f : 1.25f);
			if (env > 0.01f) {
				nvgBeginPath(vg); nvgCircle(vg, X, Y, r + env * mm2px(3.f));
				nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_HOT, 0.25f + 0.4f * env)); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
			}
			nvgBeginPath(vg); nvgCircle(vg, X, Y, r);
			nvgFillColor(vg, sfs::SCREEN_HOT); nvgFill(vg);
			if (font && font->handle >= 0) {
				sfs::screenFont(vg, font);
				nvgFontSize(vg, mm2px(2.4f));
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
				nvgFillColor(vg, sfs::SCREEN_TEXT);
				nvgText(vg, X, Y - r - mm2px(0.5f), i == 0 ? "L" : "R", NULL);
			}
		}
		{
			// facing: towards the sources' midpoint, as the engine aims the ears
			float mx = 0.5f * (P[0][0] + P[1][0]), mz = 0.5f * (P[0][2] + P[1][2]);
			float fx = mx - P[2][0], fz = mz - P[2][2], fl = std::hypot(fx, fz);
			if (fl > 1e-4f) { fx /= fl; fz /= fl; }
			for (float s : {-1.f, 1.f}) {
				float a = s * 55.f * (float)M_PI / 180.f, ca = std::cos(a), sa = std::sin(a);
				float ex = fx * ca + fz * sa, ez = -fx * sa + fz * ca;
				float tip[3] = {P[2][0] + 0.07f * ex, P[2][1], P[2][2] + 0.07f * ez}, X, Y;
				proj(tip, X, Y);
				nvgBeginPath(vg); nvgMoveTo(vg, Xl, Yl); nvgLineTo(vg, X, Y);
				nvgStrokeColor(vg, sfs::SCREEN_BLUE); nvgStrokeWidth(vg, 1.2f); nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
			}
			float r = mm2px(hoverPos == 2 || dragPos == 2 ? 1.6f : 1.25f);
			nvgBeginPath(vg); nvgCircle(vg, Xl, Yl, r);
			nvgFillColor(vg, sfs::SCREEN_TEXT); nvgFill(vg);
		}
		// height, while a marker is held or hovered: how far above the floor
		// under it, in metres at the current SIZE
		int hi = dragPos >= 0 ? dragPos : hoverPos;
		if (hi >= 0 && font && font->handle >= 0) {
			float X, Y; proj(P[hi], X, Y);
			double metres = (P[hi][1] - F[hi]) * rn * std::exp2((double)m->wSize.load());
			sfs::screenFont(vg, font);
			nvgFontSize(vg, mm2px(2.2f));
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			nvgText(vg, X + mm2px(2.2f), Y, string::f("%.1f m up", metres).c_str(), NULL);
		}

		// the header: what the room is (and whose scan); the footer: what it does to sound
		if (font && font->handle >= 0) {
			const cistern::SpaceInfo& I = cistern::SPACES[shownId];
			sfs::screenFont(vg, font);
			nvgFontSize(vg, mm2px(2.6f));
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			nvgText(vg, mm2px(2.f), mm2px(1.6f), string::uppercase(I.name).c_str(), NULL);
			bool fresh = m->dSpace.load() == shownId;
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			if (fresh) {
				std::string dims = string::f("%.0f x %.0f m  %.1f m high  %.0f m3",
					m->dX.load(), m->dZ.load(), m->dY.load(), m->dVol.load());
				nvgText(vg, w - mm2px(2.f), mm2px(1.6f), dims.c_str(), NULL);
			}
			{
				// CREDITS, under the size: who scanned each room, on request
				nvgFontSize(vg, mm2px(2.0f));
				float bounds[4];
				nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
				nvgTextBounds(vg, w - mm2px(2.f), mm2px(4.8f), "CREDITS", NULL, bounds);
				float pad = mm2px(0.6f);
				creditsBtn = Rect(Vec(bounds[0] - pad, bounds[1] - pad), Vec(bounds[2] - bounds[0] + 2 * pad, bounds[3] - bounds[1] + 2 * pad));
				nvgBeginPath(vg); nvgRoundedRect(vg, creditsBtn.pos.x, creditsBtn.pos.y, creditsBtn.size.x, creditsBtn.size.y, mm2px(0.5f));
				nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_DIM, hoverCredits || showCredits ? 0.9f : 0.45f)); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
				nvgFillColor(vg, hoverCredits || showCredits ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM);
				nvgText(vg, w - mm2px(2.f), mm2px(4.8f), "CREDITS", NULL);
				nvgFontSize(vg, mm2px(2.6f));
			}
			static const char* BN[NB] = {"125", "250", "500", "1k", "2k", "4k"};
			std::string t60 = "T60";
			for (int b = 0; b < NB; b++) t60 += string::f("   %s %s", BN[b], secs(m->dT60[b].load()).c_str());
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			if (fresh && m->dT60[2].load() > 0.f) nvgText(vg, mm2px(2.f), h - mm2px(1.4f), t60.c_str(), NULL);
			const char* st = m->frozen ? "FROZEN" : (m->tracing.load() || !fresh) ? "TRACING" : "";
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
			nvgFillColor(vg, m->frozen ? sfs::SCREEN_BLUE : sfs::SCREEN_HOT);
			nvgText(vg, w - mm2px(2.f), h - mm2px(1.4f), st, NULL);
		}
		if (showCredits) drawCredits(vg, w, h);
		nvgRestore(vg);
	}
};

void CisternRoomLayer::draw(const DrawArgs& args) { d->drawRoom(args.vg); }

// =============================================================================
// Panel: 22HP, laid out from code to the designer's measured conventions
// (panel-style.hpp) until the Figma pass. A tall screen, one row of seven
// trimpot-over-CV pairs and FREEZE, the audio at the foot with the outputs on
// a plate. The rule parts the room (SPACE..HUMID) from what is done with it
// (movement, mix, freeze).
// =============================================================================
static const float CI_SX = 5.08f, CI_SY = 10.16f, CI_SW = 101.6f, CI_SH = 63.5f;
static const float CI_PY = 82.25f, CI_PCV = 93.25f, CI_FY = 121.f;
static const float CI_PX[8] = {11.43f, 24.13f, 36.83f, 49.53f, 62.23f, 74.93f, 87.63f, 100.33f};
static const float CI_RULE_X = 68.58f, CI_RTOP = 74.93f, CI_RBOT = 96.52f;

struct CisternWidget : ModuleWidget {
	CisternWidget(Cistern* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/cistern.svg")));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("CISTERN");

		CisternDisplay* disp = new CisternDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(CI_SX, CI_SY));
		disp->box.size = mm2px(Vec(CI_SW, CI_SH));
		addChild(disp);

		static const char* PN[7] = {"SPACE", "SIZE", "DECAY", "SCATTER", "HUMID", "MOVE", "MIX"};
		static const int PP[7] = {Cistern::SPACE_PARAM, Cistern::SIZE_PARAM, Cistern::DECAY_PARAM, Cistern::SCATTER_PARAM,
		                          Cistern::HUMID_PARAM, Cistern::MOD_PARAM, Cistern::MIX_PARAM};
		static const int PI[7] = {Cistern::SPACE_INPUT, Cistern::SIZE_INPUT, Cistern::DECAY_INPUT, Cistern::SCATTER_INPUT,
		                          Cistern::HUMID_INPUT, Cistern::MOD_INPUT, Cistern::MIX_INPUT};
		for (int i = 0; i < 7; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(CI_PX[i], CI_PY)), module, PP[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CI_PX[i], CI_PCV)), module, PI[i]));
			lbl->pairDown(CI_PX[i], CI_PY, CI_PCV, PN[i]);
		}
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
			mm2px(Vec(CI_PX[7], CI_PY)), module, Cistern::FREEZE_PARAM, Cistern::FREEZE_LIGHT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CI_PX[7], CI_PCV)), module, Cistern::FREEZE_INPUT));
		lbl->pairDown(CI_PX[7], CI_PY, CI_PCV, "FREEZE");
		lbl->divider(CI_RULE_X, CI_RTOP, CI_RULE_X, CI_RBOT);

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CI_PX[0], CI_FY)), module, Cistern::IN_L_INPUT));
		lbl->jack(CI_PX[0], CI_FY, "IN L");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CI_PX[1], CI_FY)), module, Cistern::IN_R_INPUT));
		lbl->jack(CI_PX[1], CI_FY, "IN R");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CI_PX[6], CI_FY)), module, Cistern::OUT_L_OUTPUT));
		lbl->jackOnPlate(CI_PX[6], CI_FY, "OUT L");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CI_PX[7], CI_FY)), module, Cistern::OUT_R_OUTPUT));
		lbl->jackOnPlate(CI_PX[7], CI_FY, "OUT R");
	}

	void appendContextMenu(Menu* menu) override {
		Cistern* m = dynamic_cast<Cistern*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Space", cistern::SPACES[clamp(m->wSpace.load(), 0, NSPACES - 1)].name, [=](Menu* sub) {
			for (int i = 0; i < NSPACES; i++)
				sub->addChild(createCheckMenuItem(cistern::SPACES[i].name, "",
					[=]() { return (int)std::lround(m->params[Cistern::SPACE_PARAM].getValue()) == i; },
					[=]() { m->params[Cistern::SPACE_PARAM].setValue((float)i); }));
		}));
		menu->addChild(createIndexSubmenuItem("Distance", {"Musical (far sources pulled in past 25 m)", "True (delay and level as the room gives them)"},
			[=]() { return m->trueDistance ? 1 : 0; },
			[=](size_t i) { m->trueDistance = i == 1; m->posGen++; }));
		menu->addChild(createMenuLabel("Drag the sources and the listener on the screen; shift-drag lifts"));
		menu->addChild(createMenuItem("Reset the positions", "", [=]() { m->resetPositions(clamp(m->wSpace.load(), 0, NSPACES - 1)); }));
		menu->addChild(createMenuItem("Reset the view", "", [=]() { m->camYaw = Cistern::YAW0; m->camPitch = Cistern::PITCH0; }));
		menu->addChild(createSubmenuItem("Scan credits", "", [=](Menu* sub) {
			for (int i = 0; i < NSPACES; i++)
				if (cistern::SPACES[i].credit[0]) sub->addChild(createMenuLabel(cistern::SPACES[i].credit));
		}));
	}
};

Model* modelCistern = createModel<Cistern, CisternWidget>("Cistern");

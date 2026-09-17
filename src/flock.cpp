#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "flock-messages.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

// =============================================================================
// Flock -- a murmuration as a microtonal granular voice.
//
// THE BIRDS ARE PERSISTENT, THE GRAINS ARE THEIR CALLS. A granular engine
// draws stateless grains from a distribution, and a distribution never takes a
// new shape. Here N birds stay alive the whole time, each with a position that
// moves continuously under Reynolds' three flocking rules (separation,
// alignment, cohesion; Boids, 1986), and a grain is one bird calling: a short
// window onto where that bird is right now. "Always a new shape" is free,
// because the shape is the flock's state.
//
// SPACE. The birds live in a UNIT CUBE round the roost: the flock always has
// the same physical size and shape, whatever the controls say. Height is
// pitch, and LATITUDE is how many semitones the cube is tall, so it flattens
// the flock in pitch without touching its shape; at a tenth of a semitone the
// flock is a chorus, at three octaves a cloud. (LATITUDE was the leash on all
// three axes for a while, and at its bottom the birds were pinned into a
// point and the picture was a column.) The horizontal plane is the stage.
// Rotating the listener never moves a note. The composer's V/OCT is the
// height of the ROOST the cube is centred on.
//
// NEIGHBOURS ARE TOPOLOGICAL. Each bird steers by its seven nearest, whatever
// the distance, which is what the Rome starling studies found (Ballerini et
// al. 2008, six to seven). A radius model fragments when sparse and gridlocks
// when dense; seven nearest keeps one flock at any WEIGHT, and fixes the cost
// per bird.
//
// THE FLOCK DOES NOT FLY TO THE PITCH. It tried: birds chased the roost
// through pitch space and the followers never got close, because a flock
// that arrives is a flock that has stopped. Instead the birds live in OFFSET
// space around the roost, the shape is the shape, and a change of pitch
// moves every call instantly. What is late is what each bird HEARS: every
// bird carries its own copy of the roost, slewed toward the real one at a
// rate drawn once from its DISPOSITION and scaled by LAG. At LAG zero the
// whole flock takes the new pitch at once; wide open, the sluggish birds
// glide to it over seconds, still singing the old note over the new one.
// A delay with no delay line, and it always arrives.
//
// STARTLE IS A HAWK: a predator dives through the centroid, birds flee it
// locally, and because they only see neighbours the hole propagates outward,
// which is how the comma and the hourglass form in the sky.
//
// Design in docs/flock-design.md.
// =============================================================================

static const int FL_MAXBIRDS  = 1024;
static const int FL_MAXGRAINS = 64;
static const int FL_K         = 7;       // nearest neighbours per bird
static const int FL_CTRL      = 256;     // samples per physics tick (187 Hz at 48k; 1024 birds cost 15% of a core at 375)
static const int FL_NBEVERY   = 4;       // neighbour refresh, in ticks (47 Hz)
static const int FL_GRIDN     = 24;      // neighbour grid, cells per axis
static const int FL_BUFN      = 1 << 19; // ring buffer frames (10.9 s at 48k) for Flock In

// ── the mappings, each in one place so the tooltip and the engine agree ─────
static inline int   flBirds(float k)   { return clamp((int)std::round(1.f + 1023.f * k * k), 1, FL_MAXBIRDS); }
static inline float flLatSt(float k)   { return 0.1f * std::pow(360.f, clamp(k, 0.f, 1.f)); }      // 0.1 .. 36 st
static inline float flLenSec(float k)  { return 0.01f * std::pow(30.f, clamp(k, 0.f, 1.f)); }      // 10 .. 300 ms
static inline float flRateHz(float k)  { return 0.2f * std::pow(100.f, clamp(k, 0.f, 1.f)); }      // 0.2 .. 20 /s
static inline float flRelSec(float k)  { return 0.05f * std::pow(160.f, clamp(k, 0.f, 1.f)); }     // 50 ms .. 8 s
static inline float flLagSec(float k)  { k = clamp(k, 0.f, 1.f); return 4.f * k * k; }             // slowest bird, seconds

// QUANT grids: sets of semitone offsets from the roost, repeating every
// octave, so "fifths" means the roost and its fifth in every octave and the
// grid moves with the composer's pitch rather than sitting on C.
struct FlockGrid { const char* name; int n; int semis[12]; };
static const FlockGrid FL_GRIDS[] = {
	{"Semitones",             12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
	{"Whole tones",            6, {0, 2, 4, 6, 8, 10}},
	{"Root and fifth",         2, {0, 7}},
	{"Octaves",                1, {0}},
	{"Root, third, fifth",     3, {0, 4, 7}},
	{"Root, minor third, fifth", 3, {0, 3, 7}},
	{"Root, third, fifth, seventh", 4, {0, 4, 7, 11}},
	{"Pentatonic",             5, {0, 2, 4, 7, 9}},
	{"Fourths and fifths",     3, {0, 5, 7}},
};
static const int FL_NGRID = (int)(sizeof(FL_GRIDS) / sizeof(FL_GRIDS[0]));
// nearest grid pitch to `rel` (semitones above the roost, any octave)
static inline float flSnap(float rel, int grid) {
	const FlockGrid& g = FL_GRIDS[clamp(grid, 0, FL_NGRID - 1)];
	float oct = std::floor(rel / 12.f) * 12.f;
	float best = rel, bd = 1e9f;
	for (int o = -1; o <= 1; o++)
		for (int k = 0; k < g.n; k++) {
			float c = oct + 12.f * o + (float)g.semis[k];
			float d = std::fabs(c - rel);
			if (d < bd) { bd = d; best = c; }
		}
	return best;
}

struct FlockCountQ : ParamQuantity {
	std::string getDisplayValueString() override { return string::f("%d birds", flBirds(getValue())); }
};
struct FlockLatQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float st = flLatSt(getValue());
		return st < 1.f ? string::f("%.0f cents", st * 100.f) : string::f("%.1f semitones", st);
	}
};
struct FlockLenQ : ParamQuantity {
	std::string getDisplayValueString() override { return string::f("%.0f ms", flLenSec(getValue()) * 1000.f); }
};
struct FlockRateQ : ParamQuantity {
	std::string getDisplayValueString() override { return string::f("%.1f calls/s per bird", flRateHz(getValue())); }
};
struct FlockRelQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float s = flRelSec(getValue());
		return s < 1.f ? string::f("%.0f ms", s * 1000.f) : string::f("%.2f s", s);
	}
};
struct FlockLagQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float t = flLagSec(getValue());
		return t < 0.01f ? std::string("all at once") : string::f("slowest bird %.2f s behind", t);
	}
};
struct FlockChirpQ : ParamQuantity {
	std::string getDisplayValueString() override { return string::f("%+.1f semitones over the call", getValue() * 7.f); }
};

struct Flock : Module {
	enum ParamId {
		WEIGHT_PARAM, LATITUDE_PARAM, STRUCTURE_PARAM, AGILITY_PARAM, LAG_PARAM, ROTATE_PARAM,
		LENGTH_PARAM, CHIRP_PARAM, QUANT_PARAM, RATE_PARAM, RELEASE_PARAM,
		STARTLE_PARAM,
		VARIETY_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT, GATE_INPUT, STARTLE_INPUT,
		WEIGHT_INPUT, LATITUDE_INPUT, STRUCTURE_INPUT, AGILITY_INPUT, LAG_INPUT, ROTATE_INPUT,
		LENGTH_INPUT, CHIRP_INPUT, QUANT_INPUT, RATE_INPUT, RELEASE_INPUT,
		VARIETY_INPUT,
		INPUTS_LEN
	};
	enum OutputId { L_OUTPUT, R_OUTPUT, CENTRE_OUTPUT, DENSITY_OUTPUT, HAWK_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	struct Bird {
		float x = 0.f, y = 0.f, z = 0.f;      // unit-cube offsets from the roost; y * LATITUDE is pitch
		float vx = 0.f, vy = 0.f, vz = 0.f;   // units per second
		float heard = 0.f;                    // the roost this bird believes, semitones
		float disp = 0.5f;                    // 0 eager .. 1 sluggish, fixed per bird
		float chat = 1.f;                     // how talkative, 0.4 .. 2.5, fixed per bird
		float h2 = 0.f, h3 = 0.f;             // voice: second and third partial, fixed per bird
		int   nSyl = 1;                       // song: syllables per call, 1 .. 3
		float sylOff[3] = {};                 // semitone offset of each syllable
		float sylChirp[3] = {1.f, 1.f, 1.f};  // chirp direction of each
		float sylLen[3] = {1.f, 1.f, 1.f};    // relative length of each
		int   sylAt = 0;                      // the syllable due next (0 = a fresh call)
		float callT = 0.f;                    // seconds to the next call
		float flash = 0.f;                    // display: 1 at a call, decays
		int   nb[FL_K] = {};                  // set by findNeighbours; cppcheck wants it initialised
	};
	struct Grain {
		bool  on = false;
		int   bird = 0;
		float t = 0.f, len = 0.05f;
		double ph = 0.0;
		float pitch0 = 0.f, chirp = 0.f;
		float gl = 0.7f, gr = 0.7f, amp = 1.f;
		float h2 = 0.f, h3 = 0.f;
		float inc = 0.f;                      // phase per sample, refreshed every 16
		int   fctl = 0;
		// a sampled call: reads the ring buffer instead of a sine
		bool   samp = false;
		double pos = 0.0;                     // read position, frames (unwrapped)
		float  rate = 1.f;                    // frames per sample, refreshed with inc
		float  att = 0.25f;                   // envelope attack fraction
		float lp = 0.f, lpk = 1.f;
	};

	Bird  bird[FL_MAXBIRDS];
	Grain grain[FL_MAXGRAINS];
	int   nActive = 0;

	// the flock, as the display and the outputs see it
	float cx = 0.f, cy = 0.f, cz = 0.f;   // centroid, in offset space
	float cyAbs = 0.f;                    // the centroid's pitch as heard: mean(heard + y)
	float spread = 1.f;                   // rms distance to the centroid, cube units
	float spreadH = 1.f;                  // the same in the stage plane only
	float roostY = 0.f;
	float latSt = 3.f, rotDeg = 0.f;
	float flight = 0.f;                   // 1 flying, decays on gate release
	float density = 0.f;                  // calls per second, leaky
	int   callsThisTick = 0;

	// a shared intent: the whole flock leans one way for a while, then
	// another, so it streams and folds rather than sitting as a ball
	float drx = 1.f, dry = 0.f, drz = 0.f;
	// the hawk
	bool  hawkOn = false;
	float hx = 0.f, hy = 0.f, hz = 0.f, hvx = 0.f, hvy = 0.f, hvz = 0.f;
	float hawkT = 0.f;
	bool  hawkInside = false;

	// ── Flock In: the ring buffer the birds sing from when it is patched ──
	std::vector<float> bufL, bufR;
	int64_t wHead = 0;                    // frames written, unwrapped
	int64_t frozenHead = 0;
	bool  frozen = false;
	bool  sampling = false;               // Flock In present and patched
	float sampEnv = 0.25f, sampReach = 1.f;
	int   ctl = 0;
	int   tick = 0;
	int   octave = 1;                     // register: roost = V/OCT + octave
	int   grid = 0;                       // QUANT grid, index into FL_GRIDS
	int   width = 1;                      // stereo width: pan sharpness x4 / x8 / x16
	dsp::SchmittTrigger startleTrig, startleBtn;
	bool  gateWas = false;
	float sr = 48000.f;

	Flock() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<FlockCountQ>(WEIGHT_PARAM, 0.f, 1.f, 0.35f, "Weight (how many birds)");
		configParam<FlockLatQ>(LATITUDE_PARAM, 0.f, 1.f, 0.578f, "Latitude (how far the flock may stray from the pitch)");
		configParam(STRUCTURE_PARAM, 0.f, 1.f, 0.f, "Structure (pull toward just intervals of the pitch)", "%", 0.f, 100.f);
		configParam(AGILITY_PARAM, 0.f, 1.f, 0.5f, "Agility (how fast the flock changes shape)", "%", 0.f, 100.f);
		configParam<FlockLagQ>(LAG_PARAM, 0.f, 1.f, 0.3f, "Lag (how late the slowest birds hear a new pitch)");
		configParam(ROTATE_PARAM, 0.f, 1.f, 0.f, "Rotate (the listener round the flock)", "°", 0.f, 360.f);
		configParam<FlockLenQ>(LENGTH_PARAM, 0.f, 1.f, 0.527f, "Call length");
		configParam<FlockChirpQ>(CHIRP_PARAM, -1.f, 1.f, 0.3f, "Chirp (pitch glide within a call)");
		configParam(QUANT_PARAM, 0.f, 1.f, 0.f, "Quantize (snap each call toward the grid in the menu)", "%", 0.f, 100.f);
		configParam<FlockRateQ>(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate (how often each bird calls)");
		configParam<FlockRelQ>(RELEASE_PARAM, 0.f, 1.f, 0.49f, "Release (how long the flock takes to settle)");
		configButton(STARTLE_PARAM, "Startle (a hawk through the flock)");
		configParam(VARIETY_PARAM, 0.f, 1.f, 0.5f, "Variety (how far the birds differ: syllables and voices)", "%", 0.f, 100.f);

		configInput(VOCT_INPUT, "Pitch (1V/oct): the roost the flock leans toward");
		configInput(GATE_INPUT, "Gate: high and the flock flies and calls");
		configInput(STARTLE_INPUT, "Startle trigger");
		configInput(WEIGHT_INPUT, "Weight CV (+-5V = full range)");
		configInput(LATITUDE_INPUT, "Latitude CV");
		configInput(STRUCTURE_INPUT, "Structure CV");
		configInput(AGILITY_INPUT, "Agility CV");
		configInput(LAG_INPUT, "Lag CV");
		configInput(ROTATE_INPUT, "Rotate CV (10V = a full turn)");
		configInput(LENGTH_INPUT, "Call length CV");
		configInput(CHIRP_INPUT, "Chirp CV");
		configInput(QUANT_INPUT, "Quantize CV");
		configInput(RATE_INPUT, "Rate CV");
		configInput(RELEASE_INPUT, "Release CV");
		configInput(VARIETY_INPUT, "Variety CV");
		configOutput(L_OUTPUT, "Left");
		configOutput(R_OUTPUT, "Right");
		configOutput(CENTRE_OUTPUT, "Pitch: where the flock actually is, its centre as 1V/oct");
		configOutput(DENSITY_OUTPUT, "Density: calls per second (10V = 200/s)");
		configOutput(HAWK_OUTPUT, "Hawk: gate while the predator is in the flock");
		bufL.assign(FL_BUFN, 0.f);
		bufR.assign(FL_BUFN, 0.f);
		seedBirds();
	}

	// Dispositions are drawn ONCE, at birth, so a bird that was slow stays
	// slow: the delay is a property of the flock, not fresh noise every tick.
	void seedBirds() {
		for (int i = 0; i < FL_MAXBIRDS; i++) {
			Bird& b = bird[i];
			b.disp = random::uniform();
			b.chat = 0.4f * std::pow(6.25f, random::uniform());   // 0.4 .. 2.5, log-uniform
			// A VOICE: some birds are pure, some reedy. A SONG: a call is one,
			// two or three syllables, each a step away with its own sweep.
			// Both are the bird's for life, so the crowd is a crowd of
			// individuals rather than one instrument with noise on it.
			b.h2 = random::uniform() * 0.6f;
			b.h3 = random::uniform() * 0.35f;
			float u = random::uniform();
			b.nSyl = u < 0.45f ? 1 : u < 0.8f ? 2 : 3;
			for (int k = 0; k < 3; k++) {
				b.sylOff[k] = (k == 0) ? 0.f
				            : (random::uniform() < 0.5f ? -1.f : 1.f) * (1.f + 4.f * random::uniform());
				b.sylChirp[k] = (random::uniform() < 0.5f ? -1.f : 1.f) * (0.5f + random::uniform());
				b.sylLen[k] = 0.6f + 0.6f * random::uniform();
			}
			b.sylAt = 0;
			b.callT = random::uniform() * 0.5f;
			for (int k = 0; k < FL_K; k++) b.nb[k] = -1;
		}
		nActive = 0;
	}
	void spawn(int i) {
		Bird& b = bird[i];
		b.heard = roostY;
		float r = 0.5f;
		b.x = cx + (random::uniform() * 2.f - 1.f) * r;
		b.y = cy + (random::uniform() * 2.f - 1.f) * r;
		b.z = cz + (random::uniform() * 2.f - 1.f) * r;
		b.vx = b.vy = b.vz = 0.f;
		b.flash = 0.f;
	}
	void onReset() override {
		seedBirds();
		for (int g = 0; g < FL_MAXGRAINS; g++) grain[g].on = false;
		hawkOn = false; flight = 0.f;
	}

	inline float pv(int p, int in, float lo, float hi) {
		float v = params[p].getValue();
		if (inputs[in].isConnected()) v += inputs[in].getVoltage() * 0.1f * (hi - lo);
		return clamp(v, lo, hi);
	}

	// ── neighbours: the seven nearest, found through a grid ──────────────
	// Every pair was fine at 256 birds and is a million distances at 1024.
	// The birds are binned into a grid whose cell follows the flock's own
	// spread, so a dense flock gets small cells and a scattered one large,
	// and each bird looks in its own cell and the 26 round it. A bird with
	// fewer than seven found there widens to the whole flock, which only
	// happens to a straggler.
	int gridStart[FL_GRIDN * FL_GRIDN * FL_GRIDN + 1];
	int gridBird[FL_MAXBIRDS];
	int gridCell[FL_MAXBIRDS];
	static inline int gcoord(float v, float cell, float org) {
		return clamp((int)((v - org) / cell), 0, FL_GRIDN - 1);
	}
	void findNeighbours() {
		const int NC = FL_GRIDN * FL_GRIDN * FL_GRIDN;
		float cell = std::max(0.05f, spread * 0.45f);
		float org[3] = {cx - cell * FL_GRIDN * 0.5f, cy - cell * FL_GRIDN * 0.5f, cz - cell * FL_GRIDN * 0.5f};
		for (int c = 0; c <= NC; c++) gridStart[c] = 0;
		for (int i = 0; i < nActive; i++) {
			const Bird& b = bird[i];
			int ix = gcoord(b.x, cell, org[0]), iy = gcoord(b.y, cell, org[1]), iz = gcoord(b.z, cell, org[2]);
			gridCell[i] = (ix * FL_GRIDN + iy) * FL_GRIDN + iz;
			gridStart[gridCell[i] + 1]++;
		}
		for (int c = 0; c < NC; c++) gridStart[c + 1] += gridStart[c];
		{
			static int fill[NC];
			for (int c = 0; c < NC; c++) fill[c] = gridStart[c];
			for (int i = 0; i < nActive; i++) gridBird[fill[gridCell[i]]++] = i;
		}
		for (int i = 0; i < nActive; i++) {
			Bird& b = bird[i];
			float bd[FL_K]; int bi[FL_K];
			for (int k = 0; k < FL_K; k++) { bd[k] = 1e30f; bi[k] = -1; }
			auto consider = [&](int j) {
				if (j == i) return;
				const Bird& o = bird[j];
				float dx = o.x - b.x, dy = o.y - b.y, dz = o.z - b.z;
				float d = dx * dx + dy * dy + dz * dz;
				if (d >= bd[FL_K - 1]) return;
				int k = FL_K - 1;
				while (k > 0 && bd[k - 1] > d) { bd[k] = bd[k - 1]; bi[k] = bi[k - 1]; k--; }
				bd[k] = d; bi[k] = j;
			};
			int ix = gridCell[i] / (FL_GRIDN * FL_GRIDN), iy = (gridCell[i] / FL_GRIDN) % FL_GRIDN, iz = gridCell[i] % FL_GRIDN;
			for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
				int jx = ix + dx, jy = iy + dy, jz = iz + dz;
				if (jx < 0 || jy < 0 || jz < 0 || jx >= FL_GRIDN || jy >= FL_GRIDN || jz >= FL_GRIDN) continue;
				int c = (jx * FL_GRIDN + jy) * FL_GRIDN + jz;
				for (int q = gridStart[c]; q < gridStart[c + 1]; q++) consider(gridBird[q]);
			}
			if (bi[std::min(FL_K, nActive - 1) - 1] < 0 && nActive > 1)
				for (int j = 0; j < nActive; j++) consider(j);
			for (int k = 0; k < FL_K; k++) b.nb[k] = bi[k];
		}
	}

	// ── STRUCTURE: soft wells at simple ratios of the roost ─────────────────
	// Harmonic above, subharmonic below, each a Gaussian valley 0.7 semitones
	// wide. The force is the slope, so a bird between two wells is pushed
	// toward the nearer and one sitting in a well is held only softly.
	static float structureForce(float dy, float depth) {
		if (depth <= 0.f) return 0.f;
		static const float W[15] = {0.f, 12.f, 19.02f, 24.f, 27.86f, 31.02f, 33.69f, 36.f,
		                            -12.f, -19.02f, -24.f, -27.86f, -31.02f, -33.69f, -36.f};
		const float s2 = 0.7f * 0.7f;
		float f = 0.f;
		for (int k = 0; k < 15; k++) {
			float e = W[k] - dy;
			if (std::fabs(e) > 3.f) continue;
			f += e * std::exp(-e * e / (2.f * s2));
		}
		return f * depth;
	}

	void physics(float dt) {
		// ── what the panel says ──────────────────────────────────────────
		int N = flBirds(pv(WEIGHT_PARAM, WEIGHT_INPUT, 0.f, 1.f));
		latSt = flLatSt(pv(LATITUDE_PARAM, LATITUDE_INPUT, 0.f, 1.f));
		float agility = pv(AGILITY_PARAM, AGILITY_INPUT, 0.f, 1.f);
		float lagSec  = flLagSec(pv(LAG_PARAM, LAG_INPUT, 0.f, 1.f));
		float structD = pv(STRUCTURE_PARAM, STRUCTURE_INPUT, 0.f, 1.f);
		rotDeg = pv(ROTATE_PARAM, ROTATE_INPUT, 0.f, 1.f) * 360.f;
		roostY = inputs[VOCT_INPUT].getVoltage() * 12.f + 12.f * octave;
		float rate  = flRateHz(pv(RATE_PARAM, RATE_INPUT, 0.f, 1.f));

		if (N != nActive) {
			for (int i = nActive; i < N; i++) spawn(i);
			nActive = N;
			tick = 0;                          // refresh neighbours now
		}
		if ((tick++ % FL_NBEVERY) == 0) findNeighbours();

		// Speeds are in cube units per second, so the flock's motion looks
		// and behaves the same at any LATITUDE; AGILITY is the multiplier.
		float vmax = 1.5f + 4.f * agility;
		float fmax = vmax * 4.f;
		const float sepD = 0.12f;
		bool flying = flight > 0.5f;

		// the shared drift wanders slowly and stays a unit vector
		{
			drx += (random::uniform() * 2.f - 1.f) * dt * 1.2f;
			dry += (random::uniform() * 2.f - 1.f) * dt * 0.6f;
			drz += (random::uniform() * 2.f - 1.f) * dt * 1.2f;
			float l = std::sqrt(drx * drx + dry * dry + drz * drz) + 1e-6f;
			drx /= l; dry /= l; drz /= l;
		}

		// ── steering ─────────────────────────────────────────────────────
		for (int i = 0; i < nActive; i++) {
			Bird& b = bird[i];
			float ax = 0.f, ay = 0.f, az = 0.f;
			// neighbours
			float ccx = 0.f, ccy = 0.f, ccz = 0.f, avx = 0.f, avy = 0.f, avz = 0.f;
			float sx = 0.f, sy = 0.f, sz = 0.f;
			int n = 0;
			for (int k = 0; k < FL_K; k++) {
				int j = b.nb[k];
				if (j < 0 || j >= nActive) continue;
				const Bird& o = bird[j];
				ccx += o.x; ccy += o.y; ccz += o.z;
				avx += o.vx; avy += o.vy; avz += o.vz;
				float dx = b.x - o.x, dy = b.y - o.y, dz = b.z - o.z;
				float d = std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-6f;
				if (d < sepD) {
					float w = (sepD - d) / sepD / d;
					sx += dx * w; sy += dy * w; sz += dz * w;
				}
				n++;
			}
			// ARRIVE, NOT CHASE. A steer that asks for full speed at any
			// distance overshoots its target and oscillates about it, and the
			// whole flock bounced up and down on the roost. Within a slowing
			// radius the wanted speed falls with the distance left, so a bird
			// settles onto a target instead of crossing it.
			auto steerTo = [&](float tx, float ty, float tz, float w, float slowR) {
				float l = std::sqrt(tx * tx + ty * ty + tz * tz);
				if (l < 1e-6f) return;
				float want = vmax * (slowR > 0.f ? std::min(1.f, l / slowR) : 1.f);
				float s = want / l;
				ax += (tx * s - b.vx) * w; ay += (ty * s - b.vy) * w; az += (tz * s - b.vz) * w;
			};
			const float slowR = 0.8f;
			if (n > 0) {
				float inv = 1.f / n;
				steerTo(ccx * inv - b.x, ccy * inv - b.y, ccz * inv - b.z, 1.0f, slowR);   // cohesion
				// ALIGNMENT MATCHES THE NEIGHBOURS' VELOCITY, not full speed along
				// their heading. The textbook form normalises the heading to
				// vmax, which asks every bird for top speed whenever the flock
				// is moving at all: the flock could arrive nowhere, and sailed
				// through the roost by three semitones (measured).
				ax += (avx * inv - b.vx) * 1.2f; ay += (avy * inv - b.vy) * 1.2f; az += (avz * inv - b.vz) * 1.2f;
				steerTo(sx, sy, sz, 1.6f, 0.f);                                             // separation
			}
			// THE FLOCK KNOWS WHERE THE FLOCK IS. Seven neighbours alone let a
			// sub-flock split off and orbit on its own (measured: four clusters
			// from forty-eight birds), because a real flock also reads the
			// density gradient. A weak pull to the global centroid is that.
			steerTo(cx - b.x, cy - b.y, cz - b.z, 0.15f, slowR);
			// the shared drift: what makes the flock a shape rather than a
			// ball. Weak, and the leash bends it back, so the flock streams
			// round inside the cube and folds on itself as it turns.
			if (flying) steerTo(drx, dry, drz, 0.3f, 0.f);
			// the tether: the roost is the origin of offset space, so this is
			// a gentle pull home, stronger while settling
			float tx = -b.x, ty = -b.y, tz = -b.z;
			float td = std::sqrt(tx * tx + ty * ty + tz * tz);
			steerTo(tx, ty, tz, flying ? 0.3f : 2.5f, slowR);
			// the leash: from three quarters of the way out the pull grows
			// with the overshoot, so the flock turns before the wall rather
			// than pressing against it (at 0.5 drift it sat flat on the face)
			if (td > 0.75f) steerTo(tx, ty, tz, 2.5f * (td - 0.75f), slowR);
			// structure, on the pitch axis only, felt in semitones
			ay += structureForce(b.y * latSt, structD) * fmax * 0.5f;
			// wander, so a settled flock still breathes
			ax += (random::uniform() * 2.f - 1.f) * fmax * 0.25f;
			ay += (random::uniform() * 2.f - 1.f) * fmax * 0.25f;
			az += (random::uniform() * 2.f - 1.f) * fmax * 0.25f;

			// DISPOSITION shows in how a bird flies as well as in what it
			// hears: the sluggish are a little slower to turn, so the flock
			// is not one rigid thing. A mild factor; the delay lives in `heard`.
			float resp = 1.f - 0.5f * b.disp;
			ax *= resp; ay *= resp; az *= resp;
			float al = std::sqrt(ax * ax + ay * ay + az * az);
			if (al > fmax) { float s = fmax / al; ax *= s; ay *= s; az *= s; }
			// THE HAWK, after disposition: fear is faster than temperament.
			// Birds are thrown SIDEWAYS off the hawk's path rather than
			// straight away from it, which is what opens a hole and sends a
			// wave outward; pushed radially they just ran ahead of it.
			if (hawkOn) {
				float dx = b.x - hx, dy = b.y - hy, dz = b.z - hz;
				float d = std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-6f;
				const float rh = 1.3f;
				if (d < rh) {
					float hl = std::sqrt(hvx * hvx + hvy * hvy + hvz * hvz) + 1e-6f;
					float ux = hvx / hl, uy = hvy / hl, uz = hvz / hl;
					float along = dx * ux + dy * uy + dz * uz;
					float px = dx - along * ux, py = dy - along * uy, pz = dz - along * uz;
					float pl = std::sqrt(px * px + py * py + pz * pz);
					if (pl < 1e-3f) { px = -uz; py = 0.f; pz = ux; pl = std::sqrt(px * px + pz * pz) + 1e-6f; }
					float fear = 2.f * fmax * (1.f - d / rh);
					ax += px / pl * fear; ay += py / pl * fear; az += pz / pl * fear;
				}
			}
			b.vx += ax * dt; b.vy += ay * dt; b.vz += az * dt;
			float vl = std::sqrt(b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
			float vm = (flying ? vmax : vmax * 0.6f) * std::sqrt(resp);
			if (vl > vm) { float s = vm / vl; b.vx *= s; b.vy *= s; b.vz *= s; }
			if (!flying) { float k = std::exp(-dt / 0.4f); b.vx *= k; b.vy *= k; b.vz *= k; }
			b.x += b.vx * dt; b.y += b.vy * dt; b.z += b.vz * dt;
			b.flash *= std::exp(-dt / 0.12f);
			// WHAT THIS BIRD HEARS. Its own copy of the roost slews toward the
			// real one with a time constant of its own: zero for the keenest,
			// LAG seconds for the most sluggish. At LAG zero the whole flock
			// takes a new pitch on the same tick.
			float tau = lagSec * b.disp * b.disp;
			if (tau < 0.005f) b.heard = roostY;
			else b.heard += (roostY - b.heard) * (1.f - std::exp(-dt / tau));
		}

		// ── the flock as one thing ───────────────────────────────────────
		float sx = 0.f, sy = 0.f, sz = 0.f;
		for (int i = 0; i < nActive; i++) { sx += bird[i].x; sy += bird[i].y; sz += bird[i].z; }
		float inv = 1.f / std::max(nActive, 1);
		cx = sx * inv; cy = sy * inv; cz = sz * inv;
		float sh = 0.f;
		for (int i = 0; i < nActive; i++) sh += bird[i].heard + bird[i].y * latSt;
		cyAbs = sh * inv;
		float ss = 0.f, sh2 = 0.f;
		for (int i = 0; i < nActive; i++) {
			float dx = bird[i].x - cx, dy = bird[i].y - cy, dz = bird[i].z - cz;
			ss += dx * dx + dy * dy + dz * dz;
			sh2 += dx * dx + dz * dz;
		}
		spread = std::sqrt(ss * inv) + 1e-3f;
		spreadH = std::sqrt(sh2 * inv) + 1e-3f;

		// ── the hawk ─────────────────────────────────────────────────────
		if (hawkOn) {
			hx += hvx * dt; hy += hvy * dt; hz += hvz * dt;
			hawkT += dt;
			float dx = hx - cx, dy = hy - cy, dz = hz - cz;
			float d = std::sqrt(dx * dx + dy * dy + dz * dz);
			hawkInside = d < spread * 1.5f + 0.7f;
			if (hawkT > 2.2f) { hawkOn = false; hawkInside = false; }
		}

		// ── calls ────────────────────────────────────────────────────────
		callsThisTick = 0;
		float variety = pv(VARIETY_PARAM, VARIETY_INPUT, 0.f, 1.f);
		if (flight > 0.001f) {
			for (int i = 0; i < nActive; i++) {
				Bird& b = bird[i];
				b.callT -= dt;
				if (b.callT > 0.f) continue;
				// POISSON, not a metronome per bird: exponential intervals, so
				// calls bunch and thin the way a flock's do, floored so one
				// bird cannot machine-gun. And some birds talk more than
				// others.
				auto wait = [&]() {
					return std::max(0.03f, -std::log(std::max(1e-4f, random::uniform())) / (rate * b.chat));
				};
				if (b.sylAt > 0) {
					// the next syllable of a song already under way; when the
					// song is over the bird waits for the clock like anyone
					// else (it did not, once, and a two-syllable bird sang
					// without pause: 136 calls a second from eleven birds)
					if (!startGrain(i, b.sylAt, variety)) b.sylAt = 0;
					if (b.sylAt == 0) b.callT = wait();
					continue;
				}
				b.callT = wait();
				if (random::uniform() > flight) continue;      // a settling flock calls less
				if (!startGrain(i, 0, variety)) b.callT = 0.02f;   // no voice free: try again soon
			}
		}
		density += callsThisTick / dt * 0.02f - density * 0.02f;   // ~50 ms leaky rate
		// with N below the cap the density scales with WEIGHT; above it each
		// bird calls less, because a voice has to be free
	}

	void startle() {
		// The hawk enters from beyond the flock on a random bearing in the
		// stage plane, aimed through the centroid, and is gone in a second and
		// a half; the birds do the rest.
		float a = random::uniform() * 2.f * (float)M_PI;
		float dirx = std::cos(a), dirz = std::sin(a);
		float reach = spread * 2.5f + 1.f;
		hx = cx - dirx * reach; hy = cy + (random::uniform() - 0.5f) * spread; hz = cz - dirz * reach;
		float speed = 1.4f * reach;          // across the flock in about a second and a half
		hvx = dirx * speed; hvy = 0.f; hvz = dirz * speed;
		hawkT = 0.f; hawkOn = true; hawkInside = false;
	}

	// ── a call, or one syllable of one: the listeners' geometry latched at its start ──
	// VARIETY is how far this bird's own song and voice are allowed to show:
	// at zero every bird is a single plain sine and the flock is a crowd of
	// one instrument; at full each has its syllables, its steps and its
	// partials.
	bool startGrain(int i, int syl, float variety) {
		int g = -1;
		for (int k = 0; k < FL_MAXGRAINS; k++) if (!grain[k].on) { g = k; break; }
		if (g < 0) return false;
		Bird& b = bird[i];
		Grain& G = grain[g];
		G.on = true; G.bird = i; G.t = 0.f;
		// no two calls the same length or the same sweep: LENGTH and CHIRP
		// are the middle of a spread, not a fixed value
		G.len = flLenSec(pv(LENGTH_PARAM, LENGTH_INPUT, 0.f, 1.f)) * (0.6f + 0.8f * random::uniform())
		      * (1.f + variety * (b.sylLen[syl] - 1.f));
		// QUANT pulls the call's pitch toward the grid, by a fraction: at zero
		// the bird sings exactly where it is, at full it sings the nearest
		// grid note. The flock stays microtonal underneath and only the calls
		// are snapped, so the shape is unchanged and the harmony is what
		// changes.
		float q = pv(QUANT_PARAM, QUANT_INPUT, 0.f, 1.f);
		float off = variety * b.sylOff[syl];
		float rel = b.y * latSt + off;                    // semitones above what this bird hears
		float p = b.heard + rel;
		float snap = b.heard + flSnap(rel, grid);         // the grid hangs off what this bird hears
		G.pitch0 = p + q * (snap - p);
		float dir = 1.f + variety * (b.sylChirp[syl] - 1.f);
		G.chirp = pv(CHIRP_PARAM, CHIRP_INPUT, -1.f, 1.f) * 7.f * (0.5f + random::uniform()) * dir;
		G.h2 = variety * b.h2; G.h3 = variety * b.h3;
		G.ph = 0.0; G.lp = 0.f; G.fctl = 0;
		// A SAMPLED CALL. With Flock In patched the bird sings a grain of the
		// buffer rather than a sine: pitch becomes playback rate, unity when
		// the call sits on 0V (the register is ignored, so 0V plays the input
		// at its own speed and V/OCT is varispeed), and the bird's DEPTH is where
		// in the last REACH seconds it reads, near birds recent and far ones
		// old, so the flock's shape along z is a smear in time.
		G.samp = sampling;
		G.att = sampling ? clamp(sampEnv, 0.05f, 0.95f) : 0.25f;
		if (sampling) {
			// unity when the call sits on 0V: the roost already carries V/OCT,
			// so only the register is taken back out (subtracting V/OCT again
			// left every octave playing at the same speed, measured)
			G.rate = std::exp2((G.pitch0 - 12.f * octave) / 12.f);
			int64_t head = frozen ? frozenHead : wHead;
			double reachF = std::min((double)sampReach * sr, (double)(FL_BUFN - 64));
			double back = reachF * (0.5 + 0.5 * clamp(b.z, -1.f, 1.f));
			// the grain must not overtake the write head while it plays
			double need = (double)G.len * sr * std::max(0.0, (double)G.rate - 1.0) + 64.0;
			back = std::max(back, need);
			back = std::min(back, reachF);
			G.pos = (double)head - back;
		}
		// the rest of the song follows, syllable by syllable, with a breath
		// between; a fresh call waits for the Poisson clock
		int nEff = 1 + (int)std::round(variety * (b.nSyl - 1));
		if (syl + 1 < nEff) { b.sylAt = syl + 1; b.callT = G.len * (1.15f + 0.35f * random::uniform()); }
		else b.sylAt = 0;
		// The listener stands on the stage plane, a fixed distance from the
		// flock's centre, and ROTATE walks it round. Pan is the bird's azimuth
		// from the listening axis; level and brightness fall with distance.
		// FIXED IN THE WORLD, at the roost, not at the flock's centre: a pair
		// that followed the centroid drifted into the middle of whatever the
		// birds did and never let the flock move past it.
		float th = rotDeg * (float)M_PI / 180.f;
		float rx = b.x, rz = b.z;
		float xr = rx * std::cos(th) - rz * std::sin(th);
		float zr = rx * std::sin(th) + rz * std::cos(th);
		// THE LISTENERS STAND JUST OUTSIDE THE FLOCK, a quarter leash from its
		// centre on the stage plane, and the image is the flock seen from
		// there: pan is a bird's bearing from the pair, level and brightness
		// fall with its distance. Three models were measured before this
		// one. A single point inside the flock heard birds from every side
		// at once (wide the way a crowd is wide, and it said nothing about
		// the shape); a spaced pair of cardioids a leash out could not get
		// wider than an L/R correlation of 0.8 at rest whatever its spacing,
		// because a cardioid is a gentle pattern. So the bearing is panned
		// SHARPLY: the sine of the bearing, times 1.6, clamped, into an
		// equal-power law, so the edge of a flock at rest already sits near
		// the speakers and a startled bird crossing the pair swings hard.
		// Only the stage plane counts: x is the stereo axis, z is depth,
		// height is pitch and nothing else.
		// The pair stands at the FRONT FACE OF THE LEASH, a whole leash from
		// the roost, so it is visibly outside the flock at rest; the bearing
		// is panned sharply enough (times 6, measured over five flocks each:
		// x2.5 left an L/R correlation of 0.91 at rest, x6 gives 0.48 at rest
		// and 0.25 after a startle) that the flock still fills the field from
		// there, and a startled bird that reaches the face of the cube passes
		// the pair.
		const float D = 1.1f;                          // the front face of the cube
		float dx = xr, dz = zr + D;
		float dist = std::sqrt(dx * dx + dz * dz) + 1e-3f;
		float azm = std::atan2(dx, dz);
		// THE PAIR TURNS ITS HEAD TO FOLLOW THE FLOCK. Once the flock could
		// drift inside the cube, a pair staring straight ahead heard the
		// whole flock off to one side and panned it there (measured: L 0.08 V,
		// R 2.03 V). The image is centred on the flock's centre and its
		// width is the flock's spread about that, which is what you see.
		float ccx = cx * std::cos(th) - cz * std::sin(th);
		float ccz = cx * std::sin(th) + cz * std::cos(th);
		float azm0 = std::atan2(ccx, ccz + D);
		static const float WIDE[3] = {4.f, 8.f, 16.f};
		float t = clamp(std::sin(azm - azm0) * WIDE[clamp(width, 0, 2)], -1.f, 1.f);
		G.gl = std::cos((t + 1.f) * (float)M_PI_4); G.gr = std::sin((t + 1.f) * (float)M_PI_4);
		float near = clamp(D / dist, 0.2f, 2.5f);
		G.amp = near * (0.75f + 0.25f * random::uniform());
		float fc = 12000.f * clamp(D / dist, 0.25f, 1.f) * clamp(D / dist, 0.25f, 1.f);
		G.lpk = 1.f - std::exp(-2.f * (float)M_PI * fc / sr);
		bird[i].flash = 1.f;
		callsThisTick++;
		return true;
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "octave", json_integer(octave));
		json_object_set_new(r, "grid", json_integer(grid));
		json_object_set_new(r, "width", json_integer(width));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "octave")) octave = clamp((int)json_integer_value(j), 0, 3);
		if (json_t* j = json_object_get(r, "grid")) grid = clamp((int)json_integer_value(j), 0, FL_NGRID - 1);
		if (json_t* j = json_object_get(r, "width")) width = clamp((int)json_integer_value(j), 0, 2);
	}

	void process(const ProcessArgs& args) override {
		sr = args.sampleRate;
		// flight follows the gate; release is how long the flock takes to settle
		bool gate = inputs[GATE_INPUT].getVoltage() >= 1.f;
		float rel = flRelSec(pv(RELEASE_PARAM, RELEASE_INPUT, 0.f, 1.f));
		// TAKE-OFF FANS IN. A gate used to open in 20 ms onto whichever birds
		// were already due, and every one of them called on the first tick.
		// Now the flock takes off over about 150 ms, and each bird's next
		// call is re-drawn at the gate so they arrive one by one.
		if (gate && !gateWas)
			for (int i = 0; i < nActive; i++)
				bird[i].callT = random::uniform() * 0.6f / flRateHz(params[RATE_PARAM].getValue());
		gateWas = gate;
		if (gate) flight += (1.f - flight) * std::min(1.f, args.sampleTime / 0.15f);
		else      flight *= std::exp(-args.sampleTime / rel);
		if (flight < 1e-4f) flight = 0.f;

		if (startleTrig.process(inputs[STARTLE_INPUT].getVoltage(), 0.1f, 1.f)
		 || startleBtn.process(params[STARTLE_PARAM].getValue(), 0.1f, 0.9f))
			startle();

		// ── Flock In, on the left: audio into the ring, controls into hand ──
		{
			const FlockInMessage* xin = nullptr;
			if (leftExpander.module && leftExpander.module->model == modelFlockIn)
				xin = (const FlockInMessage*)leftExpander.module->rightExpander.consumerMessage;
			bool was = sampling;
			sampling = xin && xin->on;
			if (sampling) {
				sampEnv = xin->env; sampReach = xin->reach;
				if (xin->freeze && !frozen) frozenHead = wHead;
				frozen = xin->freeze;
				if (!frozen) {
					int w = (int)(wHead & (FL_BUFN - 1));
					bufL[w] = xin->l; bufR[w] = xin->r;
					wHead++;
				}
			} else if (was) {
				frozen = false;
			}
		}

		if (--ctl <= 0) { ctl = FL_CTRL; physics(FL_CTRL * args.sampleTime); }

		// ── the calls ────────────────────────────────────────────────────
		float L = 0.f, R = 0.f, sumE = 0.f;
		const float dt = args.sampleTime;
		for (int g = 0; g < FL_MAXGRAINS; g++) {
			Grain& G = grain[g];
			if (!G.on) continue;
			G.t += dt;
			if (G.t >= G.len) { G.on = false; continue; }
			float u = G.t / G.len;
			// quick in, tapered out: a call, not a bell. A quarter of the call
			// is the rise; at 15% a 60 ms call had a 9 ms edge and clicked.
			// Smoothstep rather than a raised cosine: the same S with no
			// transcendental, and there are sixty-four of these a sample.
			// A sampled call takes its attack fraction from Flock In's ENV.
			const float att = G.att;
			float w = (u < att) ? u / att : 1.f - (u - att) / (1.f - att);
			float e = w * w * (3.f - 2.f * w);
			if (G.samp) {
				// the grain reads the buffer at its rate, gliding with the
				// chirp; linear interpolation, mono sum, volts to unit
				if (--G.fctl <= 0) {
					G.fctl = 16;
					float pitch = G.pitch0 + G.chirp * (u - 0.5f);
					G.rate = std::exp2((pitch - 12.f * octave) / 12.f);
				}
				G.pos += G.rate;
				int64_t i0 = (int64_t)std::floor(G.pos);
				float fr = (float)(G.pos - (double)i0);
				int a = (int)(i0 & (FL_BUFN - 1)), c = (int)((i0 + 1) & (FL_BUFN - 1));
				float sv = ((bufL[a] + bufR[a]) * (1.f - fr) + (bufL[c] + bufR[c]) * fr) * 0.1f;
				sv *= e * G.amp;
				G.lp += (sv - G.lp) * G.lpk;
				L += G.lp * G.gl; R += G.lp * G.gr;
				sumE += e * G.amp;
				continue;
			}
			// THE COST IS THE VOICES, NOT THE BIRDS: halving the physics rate
			// changed nothing, and 64 grains each taking an exp2 per sample was
			// 90 ms of every second. The chirp is a glide, so its frequency is
			// refreshed every sixteen samples (a third of a millisecond).
			if (--G.fctl <= 0) {
				G.fctl = 16;
				float pitch = G.pitch0 + G.chirp * (u - 0.5f);
				G.inc = 261.6256f * std::exp2(pitch / 12.f) * dt;
			}
			G.ph += G.inc;
			if (G.ph >= 1.0) G.ph -= 1.0;
			// A SINE. The call had a phase-modulation index that opened in the
			// middle; at 2:1 it croaked and at 1:1 it screeched, and a flock of
			// sines is the sound that was asked for. Timbre is left to the
			// chirp, the envelope and the crowd.
			float phs = (float)G.ph * 2.f * (float)M_PI;
			float s = std::sin(phs);
			if (G.h2 > 0.f || G.h3 > 0.f) {
				// the partials from the fundamental's own sine and cosine, not
				// two more sine calls: sin 2x = 2 sin x cos x, sin 3x = 3 sin x - 4 sin^3 x
				float c = std::cos(phs);
				float s2 = 2.f * s * c, s3 = s * (3.f - 4.f * s * s);
				s = (s + G.h2 * s2 + G.h3 * s3) / (1.f + G.h2 + G.h3);
			}
			s *= e * G.amp;
			G.lp += (s - G.lp) * G.lpk;
			L += G.lp * G.gl; R += G.lp * G.gr;
			sumE += e * G.amp;
		}
		// Normalised by the summed envelopes of the sounding calls, so a dense
		// flock and a lone bird sit at about the same level and nothing steps
		// when a call starts or stops.
		float gain = 5.f / std::sqrt(1.f + 0.6f * sumE);
		L *= gain; R *= gain;
		outputs[L_OUTPUT].setVoltage(10.f * std::tanh(L * 0.1f));
		outputs[R_OUTPUT].setVoltage(10.f * std::tanh(R * 0.1f));
		outputs[CENTRE_OUTPUT].setVoltage(clamp(cyAbs / 12.f, -10.f, 10.f));
		outputs[DENSITY_OUTPUT].setVoltage(clamp(density / 200.f, 0.f, 1.f) * 10.f);
		outputs[HAWK_OUTPUT].setVoltage(hawkInside ? 10.f : 0.f);
	}
};

// ── the browser thumbnail: a flock in flight, a hawk half way through ──────
static Flock* flockPreview() {
	static Flock* pm = nullptr;
	if (pm) return pm;
	pm = new Flock();
	pm->params[Flock::WEIGHT_PARAM].setValue(0.6f);        // about 370 birds
	sfs::previewConnect(pm->inputs[Flock::GATE_INPUT], 1);
	pm->inputs[Flock::GATE_INPUT].setVoltage(10.f);
	int64_t f = 0;
	sfs::previewRun(*pm, 2.0f, f);
	pm->startle();
	sfs::previewRun(*pm, 0.3f, f);
	return pm;
}

// =============================================================================
// The display: the listener's view of the flock, drawn on the faceplate.
// No screen, as Wheel: the panel's greys, depth by size and alpha, orange for
// a bird at the moment it calls. The picture and the stereo image are the
// same viewpoint, so turning ROTATE turns both.
// =============================================================================
static const NVGcolor FL_LINE = nvgRGB(0xB4, 0xB4, 0xB4);
static const NVGcolor FL_SOFT = nvgRGB(0x7C, 0x7C, 0x7C);
static const NVGcolor FL_INK  = nvgRGB(0x2E, 0x2E, 0x2E);
static const NVGcolor FL_BIRD = nvgRGB(0x14, 0x14, 0x16);   // a bird, near: almost black
static const NVGcolor FL_FAR  = nvgRGB(0x50, 0x50, 0x54);   // a bird, far
static const NVGcolor FL_HOT  = nvgRGB(0xE8, 0x64, 0x1F);

struct FlockDisplay : OpaqueWidget {
	Flock* module = nullptr;
	std::shared_ptr<Font> font;
	float visL = 3.f;                         // smoothed vertical scale, semitones
	float viewY = 0.f;                        // smoothed view centre, semitones
	bool  viewInit = false;

	void draw(const DrawArgs& args) override {
		if (!module) { module = flockPreview(); draw(args); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		float w = box.size.x, h = box.size.y;
		nvgSave(vg);
		nvgScissor(vg, 0, 0, w, h);

		// ── the view ─────────────────────────────────────────────────────
		// The camera stands behind and a little above the listener, on the
		// listener's bearing, so the listener is in the picture and the
		// wireframe of the leash turns as ROTATE turns. The frame follows the
		// flock's centre slowly; the vertical scale follows its reach and is
		// smoothed, or a startle would zoom the picture.
		float th = module->rotDeg * (float)M_PI / 180.f;
		float ct = std::cos(th), st = std::sin(th);
		const float tilt = 0.38f;                       // camera looks down 22 degrees
		const float cphi = std::cos(tilt), sphi = std::sin(tilt);
		if (!viewInit) { viewY = module->cyAbs; viewInit = true; }
		viewY += (module->cyAbs - viewY) * 0.06f;
		visL = 1.f;                                     // the unit cube, always
		float lat = std::max(module->latSt, 0.1f);
		// a bird's height in cube units: its own offset plus how far the
		// pitch it hears sits from the view, held to the cube's reach
		auto heightOf = [&](const Flock::Bird& b) {
			return b.y + clamp((b.heard - viewY) / lat, -1.5f, 1.5f);
		};
		float Dc = 3.8f * visL;                         // camera distance
		float fx = 0.28f * w * Dc / visL;               // horizontal scale
		float fy = 0.25f * h * Dc / visL;               // vertical, so the cube fits
		float ox = w * 0.5f, oy = h * 0.46f;

		struct Dot { float sx, sy, r, depth, flash; bool hawk; };
		static std::vector<Dot> dots;
		dots.clear();
		// bearing about the pitch axis, then the camera's tilt about x, then
		// perspective. Offsets are from the flock's centre on the stage plane
		// and from the view height in pitch.
		auto project = [&](float x, float y, float z, Dot& d) {
			float rx = x, rz = z, ry = y;              // all three are offsets, in cube units
			float xr = rx * ct - rz * st, zr = rx * st + rz * ct;
			float yt = ry * cphi + zr * sphi, zt = -ry * sphi + zr * cphi;
			float depth = zt + Dc;
			if (depth < 0.2f * Dc) depth = 0.2f * Dc;
			d.sx = ox + fx * xr / depth;
			d.sy = oy - fy * yt / depth;
			d.depth = depth / Dc;                       // 1 at the centroid plane
			return true;
		};

		// ── the leash, as a wireframe cube round the flock's centre ──────
		// A cube, light, with a grid on its floor (after the storm cell on
		// the cover of Tufte's Visual Explanations): the grid gives the eye a
		// ground and the cube gives the birds somewhere to be in front of or
		// behind. Edges fade with depth; the floor is lighter still.
		{
			float L = visL;
			Dot c[8];
			for (int k = 0; k < 8; k++)
				project(((k & 1) ? L : -L), ((k & 2) ? L : -L),
				        ((k & 4) ? L : -L), c[k]);
			const int NG = 6;
			for (int g = 1; g < NG; g++) {
				float t = -L + 2.f * L * g / NG;
				Dot a, b2, e, f2;
				project(t, -L, -L, a); project(t, -L, L, b2);
				project(-L, -L, t, e); project(L, -L, t, f2);
				nvgBeginPath(vg);
				nvgMoveTo(vg, a.sx, a.sy); nvgLineTo(vg, b2.sx, b2.sy);
				nvgMoveTo(vg, e.sx, e.sy); nvgLineTo(vg, f2.sx, f2.sy);
				nvgStrokeColor(vg, nvgTransRGBA(FL_LINE, 0x48));
				nvgStrokeWidth(vg, 0.5f);
				nvgStroke(vg);
			}
			static const int E[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
			for (int e = 0; e < 12; e++) {
				const Dot& a = c[E[e][0]]; const Dot& b2 = c[E[e][1]];
				float near = clamp(1.6f - 0.5f * (a.depth + b2.depth), 0.2f, 1.f);
				nvgBeginPath(vg);
				nvgMoveTo(vg, a.sx, a.sy); nvgLineTo(vg, b2.sx, b2.sy);
				nvgStrokeColor(vg, nvgTransRGBA(FL_LINE, (unsigned char)(50 + 110 * near)));
				nvgStrokeWidth(vg, 0.5f + 0.4f * near);
				nvgStroke(vg);
			}
		}

		// ── the listeners: the mic pair on the stage plane, a leash out ──
		// Where the stereo image is taken from, with each mic's aim drawn
		// as a short line. It sits on the camera's own bearing, so as ROTATE
		// turns the pair walks round the cube.
		{
			const float D = 1.1f, base = 0.18f;
			// a point at bearing 0 in the rotated frame is (xr, zr) = (x, -D):
			// undo the bearing to get flock coordinates
			auto place = [&](float xr, float zr, Dot& d) {
				float x = xr * ct + zr * st, z = -xr * st + zr * ct;
				project(x, 0.f, z, d);
			};
			Dot l, r, m;
			place(-base, -D, l); place(base, -D, r); place(0.f, -D, m);
			nvgBeginPath(vg);
			nvgMoveTo(vg, l.sx, l.sy); nvgLineTo(vg, r.sx, r.sy);
			nvgStrokeColor(vg, FL_SOFT); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			const Dot* ear[2] = {&l, &r};
			const char* nm[2] = {"L", "R"};
			for (int k = 0; k < 2; k++) {
				float rr = mm2px(1.1f) / ear[k]->depth;
				// the aim, 55 degrees out
				Dot tip; float ax = (k ? 0.82f : -0.82f) * base, az = 0.57f * base;
				place((k ? base : -base) + ax, -D + az, tip);
				nvgBeginPath(vg);
				nvgMoveTo(vg, ear[k]->sx, ear[k]->sy); nvgLineTo(vg, tip.sx, tip.sy);
				nvgStrokeColor(vg, FL_SOFT); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
				nvgBeginPath(vg);
				nvgCircle(vg, ear[k]->sx, ear[k]->sy, rr);
				nvgFillColor(vg, nvgRGB(0xF0, 0xF0, 0xF0)); nvgFill(vg);
				nvgStrokeColor(vg, FL_INK); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
				if (font && font->handle >= 0) {
					nvgFontFaceId(vg, font->handle);
					nvgFontSize(vg, rr * 1.5f);
					nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
					nvgFillColor(vg, FL_INK);
					nvgText(vg, ear[k]->sx, ear[k]->sy, nm[k], NULL);
				}
			}
		}
		for (int i = 0; i < module->nActive; i++) {
			const Flock::Bird& b = module->bird[i];
			Dot d; project(b.x, heightOf(b), b.z, d);
			d.r = mm2px(0.2f) / d.depth;
			d.flash = b.flash; d.hawk = false;
			dots.push_back(d);
		}
		if (module->hawkOn) {
			Dot d; project(module->hx, module->hy, module->hz, d);
			d.r = mm2px(1.1f) / d.depth; d.flash = 0.f; d.hawk = true;
			dots.push_back(d);
		}
		// far birds first, so a near one paints over them
		std::sort(dots.begin(), dots.end(), [](const Dot& a, const Dot& b) { return a.depth > b.depth; });
		for (const Dot& d : dots) {
			// SOLID. Depth is size and a shade toward the soft grey, never
			// transparency: a see-through bird reads as a ghost.
			float near = clamp(1.6f - d.depth, 0.25f, 1.f);   // 1 near, 0.25 far
			NVGcolor c = d.hawk ? FL_INK
			           : nvgRGBAf(FL_BIRD.r + (FL_FAR.r - FL_BIRD.r) * (1.f - near),
			                      FL_BIRD.g + (FL_FAR.g - FL_BIRD.g) * (1.f - near),
			                      FL_BIRD.b + (FL_FAR.b - FL_BIRD.b) * (1.f - near), 1.f);
			// a calling bird lightens to grey (orange read as a scatter of
			// sparks over a thousand birds)
			if (d.flash > 0.02f)
				c = nvgRGBAf(c.r + (FL_LINE.r - c.r) * d.flash, c.g + (FL_LINE.g - c.g) * d.flash,
				             c.b + (FL_LINE.b - c.b) * d.flash, 1.f);
			nvgBeginPath(vg);
			if (d.hawk) {
				// a hawk is a chevron, not a dot
				nvgMoveTo(vg, d.sx - d.r * 1.6f, d.sy - d.r * 0.5f);
				nvgLineTo(vg, d.sx, d.sy + d.r * 0.5f);
				nvgLineTo(vg, d.sx + d.r * 1.6f, d.sy - d.r * 0.5f);
				nvgStrokeColor(vg, c); nvgStrokeWidth(vg, std::max(1.f, d.r * 0.5f)); nvgStroke(vg);
			} else {
				nvgCircle(vg, d.sx, d.sy, std::max(0.45f, d.r));
				nvgFillColor(vg, c); nvgFill(vg);
			}
		}
		nvgRestore(vg);
		OpaqueWidget::draw(args);
	}
};

// =============================================================================
// Panel: 18HP. The flock across the top, two rows of trimpots with their CVs
// beneath, the transport row at the foot with the outputs on a plate.
// =============================================================================
static const float FL_AX[6] = {7.62f, 22.86f, 38.10f, 53.34f, 68.58f, 83.82f};
static const float FL_AY = 72.5f, FL_ACV = 84.0f;
static const float FL_BY = 97.0f, FL_BCV = 108.5f;   // row B shares row A's six columns
static const float FL_TX[9] = {5.08f, 15.24f, 25.40f, 35.56f, 45.72f, 55.88f, 66.04f, 76.20f, 86.36f};
static const float FL_TY = 121.0f;

struct FlockWidget : ModuleWidget {
	FlockWidget(Flock* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/flock.svg")));

		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(6.f, 8.f, "FLOCK");

		FlockDisplay* disp = new FlockDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(3.0f, 11.0f));
		disp->box.size = mm2px(Vec(85.44f, 54.0f));
		addChild(disp);

		struct K { int p; int in; const char* t; };
		const K rowA[6] = {
			{Flock::WEIGHT_PARAM,    Flock::WEIGHT_INPUT,    "WEIGHT"},
			{Flock::LATITUDE_PARAM,  Flock::LATITUDE_INPUT,  "LATITUDE"},
			{Flock::STRUCTURE_PARAM, Flock::STRUCTURE_INPUT, "STRUCT"},
			{Flock::AGILITY_PARAM,   Flock::AGILITY_INPUT,   "AGILITY"},
			{Flock::LAG_PARAM,       Flock::LAG_INPUT,       "LAG"},
			{Flock::ROTATE_PARAM,    Flock::ROTATE_INPUT,    "ROTATE"},
		};
		for (int i = 0; i < 6; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(FL_AX[i], FL_AY)), module, rowA[i].p));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FL_AX[i], FL_ACV)), module, rowA[i].in));
			lbl->pairDown(FL_AX[i], FL_AY, FL_ACV, rowA[i].t);
		}
		const K rowB[6] = {
			{Flock::LENGTH_PARAM,  Flock::LENGTH_INPUT,  "LENGTH"},
			{Flock::CHIRP_PARAM,   Flock::CHIRP_INPUT,   "CHIRP"},
			{Flock::QUANT_PARAM,   Flock::QUANT_INPUT,   "QUANT"},
			{Flock::RATE_PARAM,    Flock::RATE_INPUT,    "RATE"},
			{Flock::RELEASE_PARAM, Flock::RELEASE_INPUT, "RELEASE"},
			{Flock::VARIETY_PARAM, Flock::VARIETY_INPUT, "VARIETY"},
		};
		for (int i = 0; i < 6; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(FL_AX[i], FL_BY)), module, rowB[i].p));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FL_AX[i], FL_BCV)), module, rowB[i].in));
			lbl->pairDown(FL_AX[i], FL_BY, FL_BCV, rowB[i].t);
		}
		// the transport row: what is played in, and what comes out
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FL_TX[0], FL_TY)), module, Flock::VOCT_INPUT));
		lbl->jack(FL_TX[0], FL_TY, "V/OCT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FL_TX[1], FL_TY)), module, Flock::GATE_INPUT));
		lbl->jack(FL_TX[1], FL_TY, "GATE");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FL_TX[2], FL_TY)), module, Flock::STARTLE_INPUT));
		lbl->jack(FL_TX[2], FL_TY, "STARTLE");
		addParam(createParamCentered<VCVButton>(mm2px(Vec(FL_TX[3], FL_TY)), module, Flock::STARTLE_PARAM));
		lbl->link(FL_TX[2], FL_TY, FL_TX[3], FL_TY);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(FL_TX[4], FL_TY)), module, Flock::L_OUTPUT));
		lbl->jackOnPlate(FL_TX[4], FL_TY, "L");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(FL_TX[5], FL_TY)), module, Flock::R_OUTPUT));
		lbl->jackOnPlate(FL_TX[5], FL_TY, "R");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(FL_TX[6], FL_TY)), module, Flock::CENTRE_OUTPUT));
		lbl->jackOnPlate(FL_TX[6], FL_TY, "PITCH");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(FL_TX[7], FL_TY)), module, Flock::DENSITY_OUTPUT));
		lbl->jackOnPlate(FL_TX[7], FL_TY, "DENS");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(FL_TX[8], FL_TY)), module, Flock::HAWK_OUTPUT));
		lbl->jackOnPlate(FL_TX[8], FL_TY, "HAWK");
	}

	void appendContextMenu(Menu* menu) override {
		Flock* m = dynamic_cast<Flock*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		// Birds sing high, and 0V is C4, where frogs sing: the roost sits an
		// octave above the CV unless told otherwise.
		menu->addChild(createIndexPtrSubmenuItem("Register",
			{"As patched (0V = C4)", "+1 octave", "+2 octaves", "+3 octaves"}, &m->octave));
		std::vector<std::string> gn;
		for (int i = 0; i < FL_NGRID; i++) gn.push_back(FL_GRIDS[i].name);
		menu->addChild(createIndexPtrSubmenuItem("Quantize grid (offsets from the pitch)", gn, &m->grid));
		menu->addChild(createIndexPtrSubmenuItem("Stereo width", {"Normal", "Wide", "Extreme"}, &m->width));
	}
};

Model* modelFlock = createModel<Flock, FlockWidget>("Flock");

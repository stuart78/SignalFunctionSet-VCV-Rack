// CANOPY -- drops falling through leaves onto a steel tongue drum. See docs/canopy-design.md.
//
// The drops are real objects: spheres under gravity in a 3D volume, bouncing off
// leaves the user places and off the drum's domed top. Every landing on the drum
// makes a sound, because the body carries it: a tongue under the drop rings
// fully, the ones near it less, and the shell knocks. How it sounds comes from the
// hit: how fast it was falling, what it is made of, how big it is, and where it
// landed. The screen draws what the physics is doing and has no state of its own
// beyond the camera, so nothing drawn can disagree with what is heard.
//
// The mode tables are ESTIMATES. Little has been published on measured tongue drum
// partials; CP_RATIO starts from one uncited set and is there to be tuned by ear.
#include "plugin.hpp"
#include "root-cv.hpp"
#include "fastmath.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "scale-bus.hpp"
#include "scales.hpp"
#include "waveguide.hpp"   // sfs::softClip
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

static const int CP_MAXN = 16;             // tongues, at most
static const int CP_MODES = 6;             // per tongue
static const int CP_CONTACTS = 3;          // hits in progress on one tongue
static const int CP_SHELL_CONTACTS = 6;
static const int CP_MAXDROPS = 128;
static const int CP_MAXLEAVES = 12;
static const int CP_SHAPE_N = 33;          // points along a tongue in the shape table

// The world, in drum radii: the top is a shallow dome over the disc of radius 1,
// and drops start at y = CP_H. Gravity is chosen for the look of the fall, about
// 0.8 s from the top, and the physics runs at a fixed 2 kHz whatever the sample rate.
static const float CP_H = 2.6f;
static const float CP_G = 8.f;
static const float CP_DT = 1.f / 2000.f;
static const float CP_DOME = 0.11f;        // height of the dome's middle above its rim
static const float CP_V0 = 6.f;            // a fall from the top, about: the reference speed
static const float CP_R0 = 0.06f;          // the reference drop radius
static const float CP_WATER_MIN = 0.024f;  // water smaller than this stays whole
static const float CP_MIN_STRIKE = 0.15f;  // slower than this and a hit makes no sound
static const int   CP_FORM_TAB = 512;      // points round a leaf's outline
static const float CP_LEAF_TH = 0.012f;    // half-thickness
static const float CP_LEAF_I = 1.5f;       // how little a hit bends a leaf
static const float CP_LEAF_BEND = 0.5f;    // radians, at most
// A draught toward the middle of the column: it bends a thrown drop back over the
// drum, where a wall would make it bounce. Nothing in the column is pushed until
// it is past CP_DRAUGHT_R.
static const float CP_DRAUGHT_R = 0.86f;
static const float CP_DRAUGHT_K = 30.f;

static inline float cpDomeY(float x, float z) { return CP_DOME * (1.f - x * x - z * z); }

static const float CP_RATIO[CP_MODES] = {1.000f, 2.005f, 2.998f, 3.520f, 4.015f, 5.980f};
static const float CP_DB[CP_MODES]    = {0.f, -9.f, -16.f, -24.f, -22.f, -30.f};
static const float CP_T60[CP_MODES]   = {5.0f, 2.2f, 1.1f, 0.5f, 0.6f, 0.25f};  // seconds at 220 Hz
// the clamped-free beam's roots, and sigma = (cosh + cos) / (sinh + sin) at each
static const double CP_BETA[CP_MODES]  = {1.875104, 4.694091, 7.854757, 10.995541, 14.137168, 17.278760};
static const double CP_SIGMA[CP_MODES] = {0.734096, 1.018467, 0.999224, 1.000034, 0.999999, 1.000000};

enum CanopyMaterial { TM_WATER, TM_RUBBER, TM_WOOD, TM_GLASS, TM_STEEL, TM_MIX, TM_COUNT };
struct CanopyMat {
	const char* name;
	float density;    // relative to water: mass, and so the weight of the strike
	float bounce;     // coefficient of restitution on the drum
	float friction;   // Coulomb: tangential speed lost, per unit of speed into the surface
	float contactMs;  // contact time at the reference size and speed
};
// The contact times are longer than a real marble's (a steel ball on steel is
// tens of microseconds), because that would sound the same for every material.
static const CanopyMat CP_MAT[TM_MIX] = {
	{"Water",  1.0f, 0.05f, 0.06f, 4.0f},
	{"Rubber", 1.1f, 0.78f, 0.16f, 3.0f},
	{"Wood",   0.7f, 0.55f, 0.13f, 1.2f},
	{"Glass",  2.5f, 0.80f, 0.08f, 0.45f},
	{"Steel",  7.8f, 0.85f, 0.08f, 0.3f},
};

// What a leaf is made of, and so what it does to what lands on it. A leaf is held
// by its stem and bends under a hit, then springs back, so it can throw a drop lying
// on it. A drop's bounce is multiplied by `give`; friction is the average of the two
// surfaces; `splash` scales how readily water breaks up on it; `hz` and `zeta` are how
// it bends. A living leaf is waxy, thin and springy: little friction, a moderate bounce
// and a quick, damped return. Felt sags and stays down, rubber and steel keep flapping,
// glass barely moves.
enum CanopyLeafKind { TP_LEAF, TP_FELT, TP_RUBBER, TP_WOOD, TP_GLASS, TP_STEEL, TP_MIX, TP_COUNT };
struct CanopyLeafMat { const char* name; float give, friction, splash, hz, zeta; };
static const CanopyLeafMat CP_LEAF_MAT[TP_MIX] = {
	{"Leaf",   0.40f, 0.05f, 0.5f,  6.f, 0.22f},
	{"Felt",   0.10f, 0.16f, 0.0f,  3.f, 0.70f},
	{"Rubber", 1.00f, 0.16f, 0.6f,  4.f, 0.10f},
	{"Wood",   0.50f, 0.10f, 0.7f,  8.f, 0.18f},
	{"Glass",  0.70f, 0.04f, 1.0f, 40.f, 0.40f},
	{"Steel",  0.95f, 0.05f, 1.0f, 11.f, 0.04f},
};

// The outlines, chosen from the menu. A form is a distance from a centre point at each
// angle, in units of the leaf's half-length, so a maple's lobes can point where they
// point and a monstera's cuts can run in toward the middle. The physics reads the same
// table, so a drop falls through a cut. `flat` scales the fold: a broad leaf folded
// as hard as a narrow one would close up. `palmate` veins start at the centre.
enum CanopyLeafForm { LF_BEECH, LF_MAPLE, LF_MONSTERA, LF_GINKGO, LF_OAK, LF_WILLOW, LF_MIXED, LF_COUNT = LF_MIXED };
static const char* const CP_FORM_NAME[LF_COUNT + 1] = {"Beech", "Maple", "Monstera", "Ginkgo", "Oak", "Willow", "All forms in turn"};
static const float CP_FORM_C[LF_COUNT]    = {-0.15f, -0.32f, -0.30f, -0.92f, 0.f, 0.f};   // the centre, along the leaf
// Half-length in drum radii, for a 45 cm drum, so the forms are the right size against the
// drum and each other. In cm tip to stem: beech 10, maple 12, monstera 30, ginkgo 6, oak 15,
// willow 8. A monstera has cuts only once it is mature, and 30 cm is the small end of that.
// On a 30 cm drum, the commonest size, one such leaf is as wide as the drum.
// Divided by how many half-lengths each outline spans.
static const float CP_FORM_LEN[LF_COUNT]  = {0.222f, 0.31f, 0.717f, 0.155f, 0.333f, 0.178f};
static const float CP_SLOT_SIZE[12] = {1.f, 0.92f, 1.08f, 0.96f, 1.12f, 0.88f, 1.04f, 0.94f, 1.1f, 0.9f, 1.02f, 0.98f};
static const float CP_FORM_FLAT[LF_COUNT] = {1.f, 0.35f, 0.40f, 0.30f, 0.8f, 1.2f};
static const bool  CP_FORM_PALM[LF_COUNT] = {false, true, false, true, false, false};
static const int   CP_FORM_NS[LF_COUNT]   = {28, 64, 72, 40, 48, 24};   // facets round the leaf
static const int   CP_FORM_NR[LF_COUNT]   = {4, 3, 3, 3, 3, 3};         // and out from the centre

// Each leaf has its own values for how much it arches, folds along the midrib and twists, how
// its midrib sweeps to one side, and how wide it is. Fixed per slot, so a patch keeps
// its leaves. The physics reads the same numbers as the drawing.
struct CanopyLeafShape { float arch, fold, twist, sweep, width; };
static inline CanopyLeafShape canopyLeafShape(int i) {
	static const float R[12][5] = {
		{0.20f, 0.20f,  0.10f,  0.08f, 1.00f}, {0.14f, 0.26f, -0.14f, -0.10f, 0.92f},
		{0.24f, 0.16f,  0.05f, -0.05f, 1.08f}, {0.17f, 0.22f, -0.08f,  0.12f, 0.96f},
		{0.12f, 0.28f,  0.14f,  0.05f, 0.88f}, {0.22f, 0.18f, -0.12f, -0.12f, 1.04f},
		{0.18f, 0.24f,  0.08f,  0.10f, 1.00f}, {0.26f, 0.15f, -0.06f, -0.06f, 0.94f},
		{0.15f, 0.20f,  0.12f, -0.09f, 1.06f}, {0.20f, 0.26f, -0.10f,  0.06f, 0.90f},
		{0.13f, 0.18f,  0.06f,  0.11f, 1.02f}, {0.23f, 0.22f, -0.14f, -0.04f, 0.98f},
	};
	const float* r = R[((i % 12) + 12) % 12];
	CanopyLeafShape S; S.arch = r[0]; S.fold = r[1]; S.twist = r[2]; S.sweep = r[3]; S.width = r[4];
	return S;
}

struct CanopyDrop {
	bool  alive = false;
	bool  droplet = false;      // a piece of a drop that has broken
	bool  original = false;     // the drop as it first appeared, or what is left of it on a leaf
	int   mat = TM_GLASS;
	float x = 0.f, y = 0.f, z = 0.f;
	float vx = 0.f, vy = 0.f, vz = 0.f;
	float r = CP_R0;
	float mass = 1.f;
	float age = 0.f;
	float still = 0.f;          // seconds barely moving on a surface
	bool  landed = false;       // has struck the drum
	bool  onTongue = false;     // has landed on a tongue
	bool  touched = false;      // has touched a leaf
	bool  released = false;     // the hoop and the draught no longer act on it, so it leaves
};

struct CanopyLeaf {
	float cx = 0.f, cy = 1.f, cz = 0.f;   // the centre
	float ax = 0.4f, ay = 0.f, az = 0.f;  // from the centre to one end
};
// What the physics reads: the plank's own axes, worked out once when it moves.
struct CanopyLeafLive {
	float cx = 0.f, cy = 0.f, cz = 0.f;
	float ax = 1.f, ay = 0.f, az = 0.f, L = 0.4f;   // unit vector along the length, half-length
	float bx = 0.f, by = 0.f, bz = 1.f;             // unit vector across, always level
	float nx = 0.f, ny = 1.f, nz = 0.f;             // the upward normal
	int   mat = TP_LEAF;
	int   form = LF_MONSTERA;
	float inertia = 1.5f;      // how little a hit bends it: a bigger leaf is harder to bend
	CanopyLeafShape shape = {0.f, 0.f, 0.f, 0.f, 1.f};
};

static inline void canopyMakeLeaf(CanopyLeafLive& K, float cx, float cy, float cz, float ax, float ay, float az) {
	K.cx = cx; K.cy = cy; K.cz = cz;
	float L = std::max(std::sqrt(ax * ax + ay * ay + az * az), 1e-3f);
	K.L = L;
	K.ax = ax / L; K.ay = ay / L; K.az = az / L;
	// across = up x along
	float bx = K.az, bz = -K.ax;
	float bl = std::sqrt(bx * bx + bz * bz);
	if (bl < 1e-4f) { bx = 0.f; bz = 1.f; bl = 1.f; }   // standing on end
	K.bx = bx / bl; K.by = 0.f; K.bz = bz / bl;
	// normal = along x across, turned to face up
	float nx = K.ay * K.bz - K.az * K.by;
	float ny = K.az * K.bx - K.ax * K.bz;
	float nz = K.ax * K.by - K.ay * K.bx;
	if (ny < 0.f) { nx = -nx; ny = -ny; nz = -nz; }
	K.nx = nx; K.ny = ny; K.nz = nz;
}

struct Canopy : Module {
	enum ParamId {
		RATE_PARAM, MATERIAL_PARAM, SIZE_PARAM, SCATTER_PARAM,
		DRIFT_PARAM, SPEED_PARAM, TILT_PARAM, LEAF_PARAM,
		DECAY_PARAM, TONE_PARAM, ROOT_PARAM, SCALE_PARAM,
		DENSITY_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		RATE_INPUT, MATERIAL_INPUT, SIZE_INPUT, SCATTER_INPUT,
		DRIFT_INPUT, SPEED_INPUT, TILT_INPUT, LEAF_INPUT,
		DECAY_INPUT, TONE_INPUT, ROOT_INPUT, SCALE_INPUT,
		DENSITY_INPUT,
		VOCT_INPUT, GATE_INPUT, VEL_INPUT, CLOCK_INPUT, X_INPUT, Z_INPUT,
		INPUTS_LEN
	};
	enum OutputId { POLY_OUTPUT, L_OUTPUT, R_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	// ── the drops ─────────────────────────────────────────────────────────
	CanopyDrop drop[CP_MAXDROPS];
	float physAcc = 0.f;
	float nextDrop = 0.f;         // how long until the next drop, in gaps at the present RATE, so turning it up acts at once
	uint32_t rng = 0x12345u;
	dsp::SchmittTrigger clockTrig;
	dsp::SchmittTrigger gateTrig[CP_MAXN];
	bool  evenly = false;         // saved: drops fall evenly rather than at random
	float rateK = 1.f, scatterK = 1.f, sizeK = CP_R0;
	float colX = 0.f, colZ = 0.f, colR = 0.95f;   // the column drops fall through
	int   matK = TM_MIX, padK = TP_LEAF;
	long  strikes = 0, spawned = 0, landed = 0, onTongue = 0, touched = 0;   // counted, for the harness

	// ── the leaves ───────────────────────────────────────────────────────
	// The UI thread edits `leaf` and publishes it; the audio thread moves each
	// plank round the published place by DRIFT and TILT, and that is what the
	// physics and the screen both read.
	CanopyLeaf leaf[CP_MAXLEAVES];
	float formR[LF_COUNT][CP_FORM_TAB] = {};   // the outlines
	float formMax[LF_COUNT] = {};
	int   leafForm = LF_MONSTERA;       // saved: what the menu chose
	CanopyLeaf home[2][CP_MAXLEAVES];
	std::atomic<int> homeBank{0};
	CanopyLeafLive live[CP_MAXLEAVES];
	int nLive = 0;
	static const int DRIFT_AXES = 5;      // across, along, height, swing, pitch
	float driftPh[CP_MAXLEAVES][DRIFT_AXES] = {};
	float driftRatio[CP_MAXLEAVES][DRIFT_AXES] = {};
	float driftK = 0.f, tiltK = 0.f, speedK = 0.1f;
	int   leavesK = 3;
	float bend[CP_MAXLEAVES] = {}, bendV[CP_MAXLEAVES] = {};   // how far each leaf is bent down from its stem
	float leafFlash[CP_MAXLEAVES] = {};                        // for the screen: 1 just after a hit, falling to 0

	// ── the tongues ───────────────────────────────────────────────────────
	struct Contact { float ph = 2.f, dph = 0.f, amp = 0.f; float g[CP_MODES] = {}; };
	struct TongueVoice {
		float re[CP_MODES] = {}, im[CP_MODES] = {}, c[CP_MODES] = {}, s[CP_MODES] = {}, lv[CP_MODES] = {};
		float ex[CP_MODES] = {};
		Contact contact[CP_CONTACTS];
		float freq = 0.f, semis = 0.f;    // what the coefficients were worked out for
		float tx = 0.f, tz = 0.f;         // the free end
		float ux = 0.f, uz = 1.f;         // from the free end toward the clamp
		float len = 0.4f, w = 0.14f;      // length, half-width
		float panL = 0.7071f, panR = 0.7071f;
		float out = 0.f;
		float env = 0.f;                  // for the screen
		bool  awake = false, fading = false;
	} tv[CP_MAXN];
	int   nT = 10;                 // tongues in play: the scale's notes, over two octaves when there is room
	int   layoutN = 0;
	float shape[CP_MODES][CP_SHAPE_N] = {};
	float tilt[CP_MODES] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
	Contact shellHit[CP_SHELL_CONTACTS];
	float bodyRe[2] = {}, bodyIm[2] = {}, bodyC[2] = {1.f, 1.f}, bodyS[2] = {};
	float decayK = 1.f, coefDecay = -1.f, coefSr = 0.f;
	int   ctl = 0;
	int   regOct = -1;            // saved: octave of the lowest tongue's root from C4
	int   rootCvMode = sfs::ROOT_CV_VOCT;
	int   root = 0;
	sfs::BusScale sc;

	// ── the output limiter, as Carillon's: 1 ms look-ahead, then soft clip ─────
	static const int LOOK = 48;
	float limBuf[2][LOOK] = {};
	int   limHead = 0;
	float limGain = 1.f, limDt = 0.f, limAtk = 1.f, limRel = 0.f;

	// ── the camera, saved ───────────────────────────────────────────────────
	float camYaw = 0.f, camTilt = 0.62f;

	Canopy() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate", " drops/s", 240.f, 0.05f);
		std::vector<std::string> mats;
		for (int i = 0; i < TM_MIX; i++) mats.push_back(CP_MAT[i].name);
		mats.push_back("Mix (a random material per drop)");
		configSwitch(MATERIAL_PARAM, 0.f, (float)(TM_COUNT - 1), (float)TM_MIX, "Drop material", mats);
		configParam(SIZE_PARAM, 0.f, 1.f, 0.46f, "Drop size (diameter, on a 45 cm drum)", " mm", 3.143f, 15.75f);
		configParam(SCATTER_PARAM, 0.f, 1.f, 1.f, "Scatter (width of the column the drops fall through)", "% of the drum", 0.f, 100.f);
		configParam(DRIFT_PARAM, 0.f, 1.f, 0.2f, "Drift (how far each leaf moves from where it was placed)", "%", 0.f, 100.f);
		configParam(SPEED_PARAM, 0.f, 1.f, 0.35f, "Drift speed", " Hz", 75.f, 0.02f);
		configParam(TILT_PARAM, 0.f, 1.f, 0.2f, "Tilt (how far each leaf turns and rocks as it drifts)", "%", 0.f, 100.f);
		std::vector<std::string> pads;
		for (int i = 0; i < TP_MIX; i++) pads.push_back(CP_LEAF_MAT[i].name);
		pads.push_back("Mix (a different material per leaf)");
		configSwitch(LEAF_PARAM, 0.f, (float)(TP_COUNT - 1), (float)TP_LEAF, "Leaf material", pads);
		configParam(DENSITY_PARAM, 0.f, (float)CP_MAXLEAVES, 3.f, "Density (number of leaves)");
		paramQuantities[DENSITY_PARAM]->snapEnabled = true;
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay (ring time)", "x", 16.f, 0.25f);
		configParam(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone (level of the overtones)", "%", 0.f, 200.f, -100.f);
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
		std::vector<std::string> names;
		for (int i = 0; i < sfs::NUM_SCALES; i++) names.push_back(sfs::SCALES[i].longName);
		// Pentatonic Minor rather than the bottom of the list: a tongue drum a
		// semitone apart does not sound like the instrument.
		configSwitch(SCALE_PARAM, 0.f, (float)(sfs::NUM_SCALES - 1), 4.f, "Scale", names);
		configInput(RATE_INPUT, "Rate CV (each volt doubles the drops per second)");
		configInput(MATERIAL_INPUT, "Drop material CV (1 V per material)");
		configInput(SIZE_INPUT, "Drop size CV (+-5V = full range)");
		configInput(SCATTER_INPUT, "Scatter CV (+-5V = full range)");
		configInput(DRIFT_INPUT, "Drift CV (+-5V = full range)");
		configInput(SPEED_INPUT, "Drift speed CV (+-5V = full range)");
		configInput(TILT_INPUT, "Tilt CV (+-5V = full range)");
		configInput(LEAF_INPUT, "Leaf material CV (1 V per material)");
		configInput(DENSITY_INPUT, "Density CV (1 V per leaf)");
		configInput(DECAY_INPUT, "Decay CV (+-5V = full range)");
		configInput(TONE_INPUT, "Tone CV (+-5V = full range)");
		configInput(ROOT_INPUT, sfs::rootCvName(sfs::ROOT_CV_VOCT));
		configInput(SCALE_INPUT, "Scale CV (1V per scale, or Key's scale bus)");
		configInput(VOCT_INPUT, "V/OCT (polyphonic, selects the nearest tongue; a note out of range moves by octaves into range)");
		configInput(GATE_INPUT, "Gate (polyphonic, strikes a tongue with no drop; with V/OCT unpatched, channel N strikes tongue N)");
		configInput(VEL_INPUT, "Velocity (polyphonic, 0-10 V sets how hard a gate strikes; 70% when unpatched)");
		configInput(CLOCK_INPUT, "Clock (each pulse releases one drop; RATE has no effect while this is patched)");
		configInput(X_INPUT, "Column X (+-5V moves the column from the left edge to the right, narrowing it to stay over the drum)");
		configInput(Z_INPUT, "Column Z (+-5V moves the column from the back edge to the front, narrowing it to stay over the drum)");
		configOutput(POLY_OUTPUT, "Tongues (polyphonic, one channel per tongue, lowest note first)");
		configOutput(L_OUTPUT, "Mix left");
		configOutput(R_OUTPUT, "Mix right");
		buildShape();
		buildForms();
		rng ^= random::u32() | 1u;
		for (int i = 0; i < CP_MAXLEAVES; i++)
			for (int a = 0; a < DRIFT_AXES; a++) {
				driftPh[i][a] = uni();
				driftRatio[i][a] = 0.6f + 0.9f * uni();
			}
		resetLeaves();
		layoutTongues(sc.size > 0 ? tongueCount(sc.size) : 10);
	}

	// Half an outline as points from the tip round to the stem, across >= 0.
	struct FormPt { float a, y; };
	void formFromOutline(int f, const std::vector<FormPt>& half) {
		std::vector<FormPt> loop(half);
		for (int i = (int)half.size() - 2; i >= 1; i--) loop.push_back({half[i].a, -half[i].y});
		const float c = CP_FORM_C[f];
		for (int i = 0; i < CP_FORM_TAB; i++) {
			float th = -(float)M_PI + 2.f * (float)M_PI * (float)i / (float)CP_FORM_TAB;
			float dx = std::cos(th), dy = std::sin(th), best = 0.f;
			for (size_t k = 0; k < loop.size(); k++) {
				const FormPt& A = loop[k]; const FormPt& B = loop[(k + 1) % loop.size()];
				float ex = B.a - A.a, ey = B.y - A.y;
				float det = dx * -ey + dy * ex;
				if (std::fabs(det) < 1e-7f) continue;
				float ox = A.a - c, oy = A.y;
				float t = (ox * -ey + oy * ex) / det;
				float q = (dx * oy - dy * ox) / det;
				if (t > 0.f && q >= 0.f && q <= 1.f) best = std::max(best, t);
			}
			formR[f][i] = best;
		}
	}
	void buildForms() {
		std::vector<FormPt> beech, willow, oak;
		for (int i = 64; i >= 0; i--) {
			float t = (float)i / 64.f, a = 2.f * t - 1.f;
			// widest about a third of the way along, rounded at the stem, drawn out to a
			// point; the power rises along the leaf, which makes the tip concave
			beech.push_back({a, 0.56f * std::pow(std::max(std::sin((float)M_PI * std::pow(t, 0.72f)), 0.f), 0.7f + 0.9f * t)});
			willow.push_back({a, 0.2f * std::pow(std::max(std::sin((float)M_PI * std::pow(t, 0.85f)), 0.f), 1.1f)});
			float env = std::pow(std::max(std::sin((float)M_PI * std::pow(t, 0.9f)), 0.f), 0.75f);
			oak.push_back({a, 0.56f * env * (0.56f + 0.44f * std::pow(std::fabs(std::cos((float)M_PI * 3.5f * t)), 0.6f))});
		}
		formFromOutline(LF_BEECH, beech);
		formFromOutline(LF_WILLOW, willow);
		formFromOutline(LF_OAK, oak);
		// five lobes spreading from near the stem, the middle one longest, and each with a tooth
		formFromOutline(LF_MAPLE, {
			{1.00f, 0.00f}, {0.70f, 0.13f}, {0.76f, 0.30f}, {0.46f, 0.25f}, {0.14f, 0.14f},
			{0.34f, 0.52f}, {0.22f, 0.56f}, {0.52f, 0.96f}, {0.14f, 0.76f}, {-0.02f, 0.80f},
			{-0.12f, 0.36f}, {-0.30f, 0.46f}, {-0.48f, 0.84f}, {-0.64f, 0.46f}, {-0.76f, 0.36f},
			{-0.66f, 0.14f}, {-0.72f, 0.00f}});
		// a heart, with cuts running in from the edge most of the way to the middle
		formFromOutline(LF_MONSTERA, {
			{1.00f, 0.00f}, {0.86f, 0.28f}, {0.62f, 0.56f}, {0.28f, 0.80f}, {-0.14f, 0.90f},
			{-0.56f, 0.80f}, {-0.84f, 0.50f}, {-0.86f, 0.22f}, {-0.70f, 0.06f}, {-0.62f, 0.00f}});
		const float cuts[5] = {0.60f, 1.03f, 1.46f, 1.89f, 2.32f};    // radians from the tip
		for (int i = 0; i < CP_FORM_TAB; i++) {
			float th = std::fabs(-(float)M_PI + 2.f * (float)M_PI * (float)i / (float)CP_FORM_TAB);
			float keep = 1.f;
			for (float at : cuts) {
				float d = (th - at) / 0.05f;
				keep -= 0.70f * std::exp(-d * d * d * d);
			}
			formR[LF_MONSTERA][i] *= std::max(keep, 0.22f);
		}
		// a fan from the stem, with a notch in the middle of its outer edge
		for (int i = 0; i < CP_FORM_TAB; i++) {
			float th = std::fabs(-(float)M_PI + 2.f * (float)M_PI * (float)i / (float)CP_FORM_TAB);
			float edge = 1.0f - 0.5f * (th / 0.88f) * (th / 0.88f);
			float n = th / 0.07f;
			float notch = 1.f - 0.30f * std::exp(-n * n);
			float side = clamp((0.88f - th) / 0.06f, 0.f, 1.f);       // closes at the fan's sides
			formR[LF_GINKGO][i] = std::max(0.05f, 1.72f * edge * notch * side);
		}
		for (int f = 0; f < LF_COUNT; f++) {
			formMax[f] = 0.f;
			for (int i = 0; i < CP_FORM_TAB; i++) formMax[f] = std::max(formMax[f], formR[f][i]);
		}
	}
	// How far the edge is from the centre, at an angle from the tip.
	float formRadius(int f, float th) const {
		const float* tab = formR[clamp(f, 0, LF_COUNT - 1)];
		float p = (th + (float)M_PI) / (2.f * (float)M_PI) * (float)CP_FORM_TAB;
		int i0 = (int)std::floor(p);
		float fr = p - (float)i0;
		i0 = ((i0 % CP_FORM_TAB) + CP_FORM_TAB) % CP_FORM_TAB;
		return tab[i0] + (tab[(i0 + 1) % CP_FORM_TAB] - tab[i0]) * fr;
	}
	// Height of the surface over the leaf's flat plane, per unit of half-length, at `u`
	// along (-1 stem, 1 tip) and `y` across: it arches down toward both ends, folds up
	// from the midrib to the edges, and twists along its length.
	static float leafLift(const CanopyLeafShape& S, int form, float u, float y) {
		return -S.arch * u * u + S.fold * CP_FORM_FLAT[clamp(form, 0, LF_COUNT - 1)] * std::fabs(y) + S.twist * u * y;
	}

	// Mode k of a clamped-free beam at xi along it, 0 at the clamp and 1 at the
	// free end, normalised so the free end is 1. A hit near the clamp barely moves
	// the fundamental and gives the overtones relatively more.
	void buildShape() {
		for (int m = 0; m < CP_MODES; m++) {
			double b = CP_BETA[m], sg = CP_SIGMA[m];
			double tipV = std::cosh(b) - std::cos(b) - sg * (std::sinh(b) - std::sin(b));
			for (int i = 0; i < CP_SHAPE_N; i++) {
				double x = b * (double)i / (double)(CP_SHAPE_N - 1);
				double v = std::cosh(x) - std::cos(x) - sg * (std::sinh(x) - std::sin(x));
				shape[m][i] = (float)std::fabs(v / tipV);
			}
		}
	}

	void onReset() override {
		evenly = false; regOct = -1; leafForm = LF_MONSTERA;
		camYaw = 0.f; camTilt = 0.62f;
		for (CanopyDrop& d : drop) d.alive = false;
		resetLeaves();
	}

	// Twelve places, each stem higher and further out than its tip. Heights follow a
	// low-discrepancy sequence and bearings the golden angle, so however many are in
	// play, the first N are spread through the column rather than stacked at the top.
	void resetLeaves() {
		for (int i = 0; i < CP_MAXLEAVES; i++) {
			CanopyLeaf& P = leaf[i];
			float vdc = 0.f, f = 0.5f;
			for (int n = i + 1; n > 0; n >>= 1, f *= 0.5f) if (n & 1) vdc += f;
			float th = 2.4f * (float)i + 0.6f;
			float rad = 0.34f + 0.2f * (float)((i * 5) % 3) * 0.5f;
			P.cx = rad * std::cos(th); P.cz = rad * std::sin(th);
			P.cy = 0.85f + 1.4f * vdc;
			float half = 0.38f + 0.05f * (float)((i * 7) % 3);
			// pointing round the column and a little inward, tip down
			float dir = th + 2.2f;
			P.ax = half * std::cos(dir); P.az = half * std::sin(dir);
			P.ay = -half * 0.24f;
		}
		publishLeaves();
	}

	// UI thread, or the patch loading.
	void publishLeaves() {
		int bank = 1 - homeBank.load();
		for (int i = 0; i < CP_MAXLEAVES; i++) home[bank][i] = leaf[i];
		homeBank.store(bank);
	}

	// Each leaf moves around where it was placed: two sines per axis at rates no two
	// leaves share, so it never quite repeats, and DRIFT and TILT set how far it can go.
	void updateLive(float dt) {
		int bank = homeBank.load();
		int n = clamp(leavesK, 0, CP_MAXLEAVES);
		float hz = speedK * dt;
		for (int i = 0; i < n; i++) {
			const CanopyLeaf& P = home[bank][i];
			float v[DRIFT_AXES];
			for (int a = 0; a < DRIFT_AXES; a++) {
				float& ph = driftPh[i][a];
				ph += hz * driftRatio[i][a];
				if (ph >= 1.f) ph -= 1.f;
				float second = ph * 2.37f;
				second -= std::floor(second);
				v[a] = 0.65f * SFS_SIN2PI(ph) + 0.35f * SFS_SIN2PI(second);
			}
			float cx = P.cx + driftK * 0.42f * v[0];
			float cz = P.cz + driftK * 0.42f * v[1];
			float cy = clamp(P.cy + driftK * 0.22f * v[2], 0.25f, 2.4f);
			// the stored axis is only a direction: a leaf's size comes from its form
			const int form = clamp(leafForm == LF_MIXED ? i % LF_COUNT : leafForm, 0, LF_COUNT - 1);
			const float L = CP_FORM_LEN[form] * CP_SLOT_SIZE[i % 12];
			float pl = std::max(std::sqrt(P.ax * P.ax + P.ay * P.ay + P.az * P.az), 1e-3f);
			float sw = tiltK * 1.1f * v[3], csw = std::cos(sw), ssw = std::sin(sw);
			float ax = (P.ax * csw - P.az * ssw) / pl;
			float az = (P.ax * ssw + P.az * csw) / pl;
			float ay = P.ay / pl + tiltK * 0.22f * v[4];
			canopyMakeLeaf(live[i], cx, cy, cz, ax * L, ay * L, az * L);
			const float big = L / 0.43f;
			live[i].inertia = CP_LEAF_I * big * big * big;
			live[i].mat = padK == TP_MIX ? i % TP_MIX : padK;
			live[i].shape = canopyLeafShape(i);
			live[i].form = form;
		}
		nLive = n;
	}

	// ── random ───────────────────────────────────────────────────────────
	uint32_t next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
	float uni() { return (float)(next() >> 8) * (1.f / 16777216.f); }
	float bi() { return uni() * 2.f - 1.f; }

	// ── a drop ─────────────────────────────────────────────────────────────
	CanopyDrop* freeDrop() {
		CanopyDrop* oldest = &drop[0];
		for (CanopyDrop& d : drop) {
			if (!d.alive) return &d;
			if (d.age > oldest->age) oldest = &d;
		}
		return oldest;
	}
	void setMaterial(CanopyDrop& d, int m, float radius) {
		d.mat = m; d.r = radius;
		float q = radius / CP_R0;
		d.mass = CP_MAT[m].density * q * q * q;
	}
	int chooseMaterial() { return clamp(matK == TM_MIX ? (int)(uni() * TM_MIX) : matK, 0, TM_MIX - 1); }
	void spawn(float x, float z) {
		CanopyDrop& d = *freeDrop();
		d = CanopyDrop();
		d.alive = true;
		d.original = true;
		spawned++;
		setMaterial(d, chooseMaterial(), sizeK);
		d.x = x; d.y = CP_H; d.z = z;
	}
	// Water breaks into pieces about half its radius, and a piece still big enough breaks
	// again when it lands: big to medium to small. A thrown piece needs less speed to
	// break than a falling drop, because it has only fallen as far as it was thrown.
	static bool canBreak(const CanopyDrop& d, float speed) {
		return d.mat == TM_WATER && d.r >= CP_WATER_MIN && speed > (d.droplet ? 0.8f : 1.5f);
	}
	void splash(const CanopyDrop& from, float speed) {
		int n = 2 + (int)(uni() * 3.f);
		for (int i = 0; i < n; i++) {
			CanopyDrop& d = *freeDrop();
			float a = uni() * 2.f * (float)M_PI;
			float out = speed * (0.08f + 0.14f * uni());
			d = CanopyDrop();
			d.alive = true; d.droplet = true;
			setMaterial(d, TM_WATER, from.r * (0.45f + 0.2f * uni()));
			d.x = from.x; d.y = from.y + from.r; d.z = from.z;
			d.vx = out * std::cos(a); d.vz = out * std::sin(a);
			d.vy = speed * (0.12f + 0.2f * uni());
		}
	}

	// ── the tongues on the drum ───────────────────────────────────────────
	// A pentatonic scale over two octaves is ten, the common drum: the lowest note
	// is the big tongue in the middle and the other nine go round it. A scale with
	// more than eight notes gets one octave, and there are never more than sixteen.
	static int tongueCount(int scaleSize) {
		int s = std::max(scaleSize, 1);
		return clamp(s <= 8 ? s * 2 : s, 3, CP_MAXN);
	}
	// Going round from the front, the notes rise up alternate sides to the back.
	void layoutTongues(int n) {
		layoutN = n;
		int ring = n - 1;
		TongueVoice& D = tv[0];
		D.tx = 0.f; D.tz = 0.24f; D.ux = 0.f; D.uz = -1.f; D.len = 0.54f; D.w = 0.17f;
		D.panL = D.panR = 0.7071f;
		float w = std::min(0.15f, 0.72f * (float)M_PI / (float)ring * 0.82f);
		for (int r = 1; r < n; r++) {
			int q = r - 1;
			int j = q == 0 ? 0 : ((q + 1) / 2) * (q % 2 == 1 ? -1 : 1);
			float a = 2.f * (float)M_PI * (float)j / (float)ring;
			float sa = std::sin(a), ca = std::cos(a);
			TongueVoice& T = tv[r];
			T.w = w;
			// a higher tongue is shorter, as cutting one longer lowers its note
			T.len = clamp(0.40f * std::pow(2.f, -T.semis / 42.f), 0.20f, 0.40f);
			float tipR = 0.90f - T.len;
			T.tx = sa * tipR; T.tz = ca * tipR;
			T.ux = sa; T.uz = ca;
			float pan = sa * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
			T.panL = std::cos(th); T.panR = std::sin(th);
		}
	}

	// `over` is how much of the overtones this hit gives the tongue: a tongue rung
	// through the body gets little, or their inharmonic partials pile up into a wash.
	void startContact(int t, float A, float n, float over, float xi) {
		TongueVoice& T = tv[t];
		int slot = 0;
		float oldest = -1.f;
		for (int i = 0; i < CP_CONTACTS; i++) if (T.contact[i].ph > oldest) { oldest = T.contact[i].ph; slot = i; }
		Contact& C = T.contact[slot];
		C.ph = 0.f;
		C.dph = 1.f / n;
		C.amp = A * 2.f / n;              // a raised cosine of area A
		float pos = clamp(xi, 0.f, 1.f) * (float)(CP_SHAPE_N - 1);
		int i0 = std::min((int)pos, CP_SHAPE_N - 2);
		float fr = pos - (float)i0;
		for (int m = 0; m < CP_MODES; m++) {
			float s = shape[m][i0] + (shape[m][i0 + 1] - shape[m][i0]) * fr;
			C.g[m] = (0.15f + 0.85f * s) * tilt[m] * (m == 0 ? 1.f : over);
		}
		T.awake = true;
	}

	// The hit. Harder and heavier is louder, and contact time follows Hertz: with mass
	// proportional to r^3, (m^2 / (r v))^(1/5) reduces to r * v^(-1/5), so material and
	// size set the brightness far more than the speed does.
	//
	// The top is one piece of steel, so every landing sounds. The tongue under the drop
	// receives the whole hit; off a tongue, the nearest ones receive part of it through
	// the body and the shell sounds louder; on one, its neighbours ring faintly.
	void hitDrum(float x, float z, float speed, int mat, float radius, float mass, bool& wasOnTongue) {
		const CanopyMat& M = CP_MAT[mat];
		float q = radius / CP_R0;
		float tc = 0.001f * M.contactMs * q * SFS_POWABS(speed / CP_V0, -0.2f);
		float n = std::max(tc * coefSr, 2.f);
		float A = SFS_POWABS(mass, 0.35f) * (speed / CP_V0);

		int on = -1;
		float onXi = 0.f;
		float dist[CP_MAXN] = {}, xiAt[CP_MAXN] = {};
		for (int t = 0; t < nT; t++) {
			const TongueVoice& T = tv[t];
			float px = x - T.tx, pz = z - T.tz;
			float a = px * T.ux + pz * T.uz;
			float c = px * T.uz - pz * T.ux;
			float da = std::max(std::max(-a, a - T.len), 0.f);
			float dc = std::max(std::fabs(c) - T.w, 0.f);
			dist[t] = std::sqrt(da * da + dc * dc);
			xiAt[t] = 1.f - clamp(a / T.len, 0.f, 1.f);
			if (dist[t] <= 0.f && on < 0) { on = t; onXi = xiAt[t]; }
		}
		for (int t = 0; t < nT; t++) {
			float g;
			if (t == on) g = 1.f;
			else if (on >= 0) g = 0.08f * SFS_EXP(-dist[t] / 0.3f);
			else g = 0.26f * SFS_EXP(-dist[t] / 0.14f);
			if (g < 0.025f) continue;
			startContact(t, A * g, n, t == on ? 1.f : 0.25f, t == on ? onXi : 0.6f);
		}
		// the shell
		int slot = 0; float oldest = -1.f;
		for (int i = 0; i < CP_SHELL_CONTACTS; i++) if (shellHit[i].ph > oldest) { oldest = shellHit[i].ph; slot = i; }
		Contact& S = shellHit[slot];
		S.ph = 0.f; S.dph = 1.f / n;
		S.amp = A * (on >= 0 ? 0.3f : 1.f) * 2.f / n;
		strikes++;
		wasOnTongue = on >= 0;
	}

	// A drop that has settled, or has been in play a long time, is let go: it rolls down
	// the dome and off the rim, and it is removed once it has fallen out of the view.
	void release(CanopyDrop& d) {
		if (d.released) return;
		d.released = true;
		d.still = 0.f;
		float rad = std::sqrt(d.x * d.x + d.z * d.z);
		if (rad > 1e-3f) { d.vx += 0.5f * d.x / rad; d.vz += 0.5f * d.z / rad; }
		else d.vx += 0.5f;
	}

	// Coulomb friction: it removes speed in proportion to how hard the drop presses,
	// so a drop lying on a slope still rolls off it.
	void reflect(CanopyDrop& d, float nx, float ny, float nz, float vn, float e, float mu, bool jitter) {
		float tx = d.vx - vn * nx, ty = d.vy - vn * ny, tz = d.vz - vn * nz;
		float vt = std::sqrt(tx * tx + ty * ty + tz * tz);
		bool rolling = -vn < 0.5f;             // too slow to leave the surface
		float bounce = rolling ? 0.f : e;
		// a round thing rolls: it keeps most of what sliding friction would remove
		float grip = rolling ? mu * 0.15f : mu;
		float keep = vt > 1e-5f ? std::max(1.f - grip * (1.f + bounce) * -vn / vt, 0.f) : 0.f;
		float out = -vn * bounce;
		d.vx = tx * keep + out * nx;
		d.vy = ty * keep + out * ny;
		d.vz = tz * keep + out * nz;
		if (jitter) {
			// nothing lands perfectly flat, and this is what sends a bounce to a neighbour
			float j = 0.02f * -vn;
			d.vx += j * bi(); d.vz += j * bi();
		}
	}

	// One step of one drop against the planks and the drum.
	void stepDrop(CanopyDrop& d) {
		d.age += CP_DT;
		d.vy -= CP_G * CP_DT;
		float rad2 = d.x * d.x + d.z * d.z;
		float surface = cpDomeY(d.x, d.z);
		// the draught, once it is out past the edge of the column's middle
		if (!d.released && rad2 > CP_DRAUGHT_R * CP_DRAUGHT_R && d.y > surface) {
			float rad = std::sqrt(rad2);
			// stronger in the last stretch above the drum, where there is least time left to turn
			float low = 1.f + 3.f * clamp(1.f - d.y, 0.f, 1.f);
			float pull = low * CP_DRAUGHT_K * (rad - CP_DRAUGHT_R) * CP_DT / rad;
			d.vx -= d.x * pull; d.vz -= d.z * pull;
		}
		d.x += d.vx * CP_DT; d.y += d.vy * CP_DT; d.z += d.vz * CP_DT;
		const CanopyMat& M = CP_MAT[d.mat];

		bool slow = false, wedged = false;
		for (int i = 0; i < nLive; i++) {
			const CanopyLeafLive& K = live[i];
			float rx = d.x - K.cx, ry = d.y - K.cy, rz = d.z - K.cz;
			const float reach = d.r + CP_LEAF_TH;
			const float rawA = rx * K.ax + ry * K.ay + rz * K.az;
			const float centre = CP_FORM_C[clamp(K.form, 0, LF_COUNT - 1)];
			const float xRaw = rawA / K.L - centre;
			const float uRaw = clamp(centre + xRaw, -2.f, 2.f);
			const float off = K.shape.sweep * uRaw * uRaw;       // where the midrib has swept to
			const float yRaw = ((rx * K.bx + rz * K.bz) / K.L - off) / K.shape.width;
			const float R = std::sqrt(xRaw * xRaw + yRaw * yRaw);
			if (R > formMax[clamp(K.form, 0, LF_COUNT - 1)] + (reach + 0.05f) / K.L) continue;
			const float rr = formRadius(K.form, std::atan2(yRaw, xRaw));
			const bool inside = R <= rr;
			const float toEdge = inside || R < 1e-5f ? 1.f : rr / R;
			const float px = xRaw * toEdge, py = yRaw * toEdge;  // the point on the leaf nearest
			const float u = centre + px;
			const float s = (u + 1.f) * K.L;                     // along from the stem
			const float lift = K.L * leafLift(K.shape, K.form, u, py) - s * bend[i];
			const float h = rx * K.nx + ry * K.ny + rz * K.nz - lift;
			if (std::fabs(h) > reach + 0.05f) continue;
			float nx, ny, nz, pen;
			if (inside) {
				// on the face: the surface's own slope, so a drop runs down the fold and off the tip
				if (std::fabs(h) >= reach) continue;
				float dA = -2.f * K.shape.arch * u + K.shape.twist * py - bend[i];
				const float flat = CP_FORM_FLAT[clamp(K.form, 0, LF_COUNT - 1)];
				float dB = (py >= 0.f ? K.shape.fold * flat : -K.shape.fold * flat) + K.shape.twist * u;
				nx = K.nx - K.ax * dA - K.bx * dB;
				ny = K.ny - K.ay * dA;
				nz = K.nz - K.az * dA - K.bz * dB;
				float nl = (h >= 0.f ? 1.f : -1.f) / std::sqrt(nx * nx + ny * ny + nz * nz);
				nx *= nl; ny *= nl; nz *= nl;
				pen = reach - std::fabs(h);
			}
			else {
				// over the edge: that point of it
				float la = u * K.L, lb = (off + py * K.shape.width) * K.L;
				float qx = rx - la * K.ax - lb * K.bx - K.nx * lift;
				float qy = ry - la * K.ay - K.ny * lift;
				float qz = rz - la * K.az - lb * K.bz - K.nz * lift;
				float dist2 = qx * qx + qy * qy + qz * qz;
				if (dist2 >= reach * reach || dist2 < 1e-10f) continue;
				float dist = std::sqrt(dist2);
				nx = qx / dist; ny = qy / dist; nz = qz / dist;
				pen = reach - dist;
			}
			d.x += nx * pen; d.y += ny * pen; d.z += nz * pen;
			// the surface here is moving, so everything is relative to it
			float sv = -s * bendV[i];
			float sx = K.nx * sv, sy = K.ny * sv, sz = K.nz * sv;
			float vn = (d.vx - sx) * nx + (d.vy - sy) * ny + (d.vz - sz) * nz;
			if (vn >= 0.f) continue;
			float speed = -vn;
			const CanopyLeafMat& Pd = CP_LEAF_MAT[K.mat];
			float e = M.bounce * Pd.give;
			d.vx -= sx; d.vy -= sy; d.vz -= sz;
			reflect(d, nx, ny, nz, vn, e, 0.5f * (M.friction + Pd.friction), speed > 0.8f);
			d.vx += sx; d.vy += sy; d.vz += sz;
			if (speed > 0.6f) leafFlash[i] = std::max(leafFlash[i], clamp(speed / 2.5f, 0.35f, 1.f));
			// and the drop pushes back
			float along = nx * K.nx + ny * K.ny + nz * K.nz;
			bendV[i] += d.mass * (1.f + e) * speed * along * s / K.inertia;
			if (canBreak(d, speed * Pd.splash * 0.75f)) {
				// what is left on the leaf is one of the pieces
				splash(d, speed);
				setMaterial(d, TM_WATER, d.r * 0.6f);
				d.droplet = true;
			}
			if (speed < 0.6f) wedged = true;
			if (!d.touched) { d.touched = true; if (d.original) touched++; }
		}

		rad2 = d.x * d.x + d.z * d.z;
		surface = cpDomeY(d.x, d.z);
		if (rad2 <= 1.f && d.y - d.r <= surface && d.y > surface - 0.2f && d.vy < 0.f) {
			// the dome's normal
			float nx = 2.f * CP_DOME * d.x, nz = 2.f * CP_DOME * d.z;
			float nl = 1.f / std::sqrt(1.f + nx * nx + nz * nz);
			nx *= nl; nz *= nl;
			float vn = d.vx * nx + d.vy * nl + d.vz * nz;
			if (vn < 0.f) {
				float speed = -vn;
				d.y = surface + d.r;
				if (speed >= CP_MIN_STRIKE) {
					bool on = false;
					hitDrum(d.x, d.z, speed, d.mat, d.r, d.mass, on);
					if (d.original) {
						if (!d.landed) { d.landed = true; landed++; }
						if (on && !d.onTongue) { d.onTongue = true; onTongue++; }
					}
				}
				if (d.mat == TM_WATER) {
					if (canBreak(d, speed)) splash(d, speed);
					d.alive = false;
					return;
				}
				reflect(d, nx, nl, nz, vn, M.bounce, M.friction, speed > 0.8f);
				if (speed < 0.6f) slow = true;
			}
		}
		// the hoop round the rim turns back a drop rolling off
		if (!d.released && rad2 > 0.92f && d.y < 0.1f && d.y > -0.1f) {
			float rad = std::sqrt(rad2);
			float vr = (d.vx * d.x + d.vz * d.z) / rad;
			if (vr > 0.f) {
				d.vx -= 1.4f * vr * d.x / rad; d.vz -= 1.4f * vr * d.z / rad;
				d.x *= 0.96f / rad; d.z *= 0.96f / rad;
			}
		}
		if ((slow || wedged) && d.vx * d.vx + d.vy * d.vy + d.vz * d.vz < 0.25f) {
			d.still += CP_DT;
			if (d.still > 1.f && wedged) {
				// held in a dip or between two leaves: a random push frees it
				d.vx += 0.7f * bi(); d.vz += 0.7f * bi(); d.vy += 0.4f;
				d.still = 0.f;
			}
			else if (d.still > 1.5f) release(d);
		}
		else if (d.still > 0.f) d.still = std::max(0.f, d.still - CP_DT * 0.25f);
		if (d.age > 15.f) release(d);
		if (d.y < -1.8f || d.age > 40.f) d.alive = false;
	}

	void physics(float dt) {
		for (int i = 0; i < nLive; i++) {
			const CanopyLeafMat& Pd = CP_LEAF_MAT[live[i].mat];
			float w = 2.f * (float)M_PI * Pd.hz;
			bendV[i] += (-w * w * bend[i] - 2.f * Pd.zeta * w * bendV[i]) * dt;
			bend[i] += bendV[i] * dt;
			leafFlash[i] *= 1.f - dt / 0.12f;
			if (std::fabs(bend[i]) > CP_LEAF_BEND) {
				bend[i] = std::copysign(CP_LEAF_BEND, bend[i]);
				bendV[i] = 0.f;
			}
		}
		if (!inputs[CLOCK_INPUT].isConnected()) {
			nextDrop -= dt * rateK;
			if (nextDrop <= 0.f) {
				spawnNow();
				nextDrop = evenly ? 1.f : -std::log(std::max(uni(), 1e-4f));
			}
		}
		for (CanopyDrop& d : drop) if (d.alive) stepDrop(d);
	}

	// Anywhere in the column above the drum.
	void spawnNow() {
		float a = uni() * 2.f * (float)M_PI, rr = colR * std::sqrt(uni());
		spawn(colX + rr * std::cos(a), colZ + rr * std::sin(a));
	}

	// ── a direct strike, from the GATE input ──────────────────────────────
	int tongueForNote(float semisFromC4) const {
		float base = 12.f * (float)regOct + (float)root;
		float lo = base + tv[0].semis, hi = base + tv[nT - 1].semis;
		float s = semisFromC4;
		while (s < lo - 0.5f) s += 12.f;
		while (s > hi + 0.5f) s -= 12.f;
		int best = 0; float bd = 1e9f;
		for (int t = 0; t < nT; t++) {
			float d = std::fabs(s - (base + tv[t].semis));
			if (d < bd) { bd = d; best = t; }
		}
		return best;
	}
	void gates() {
		const int gch = inputs[GATE_INPUT].getChannels();
		// all of them every sample: a Rack trigger first read high does not fire
		for (int c = 0; c < CP_MAXN; c++) {
			float g = c < gch ? inputs[GATE_INPUT].getVoltage(c) : 0.f;
			if (!gateTrig[c].process(g, 0.1f, 1.f)) continue;
			int t = inputs[VOCT_INPUT].isConnected()
				? tongueForNote(inputs[VOCT_INPUT].getPolyVoltage(c) * 12.f)
				: c % nT;
			float vel = inputs[VEL_INPUT].isConnected() ? clamp(inputs[VEL_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f) : 0.7f;
			if (vel <= 0.f) continue;
			const TongueVoice& T = tv[t];
			// a mallet on the free end, three quarters of the way out
			float x = T.tx + T.ux * T.len * 0.25f, z = T.tz + T.uz * T.len * 0.25f;
			int m = chooseMaterial();
			float q = sizeK / CP_R0;
			bool on = false;
			hitDrum(x, z, std::max(vel * CP_V0, CP_MIN_STRIKE), m, sizeK, CP_MAT[m].density * q * q * q, on);
		}
	}

	// ── the knobs and the key, every 32 samples ──────────────────────────────
	void control(float sr) {
		rateK = 0.05f * SFS_POWABS(240.f, params[RATE_PARAM].getValue());
		if (inputs[RATE_INPUT].isConnected()) rateK *= SFS_EXP2(clamp(inputs[RATE_INPUT].getVoltage(), -10.f, 10.f));
		matK = clamp((int)std::round(params[MATERIAL_PARAM].getValue() + (inputs[MATERIAL_INPUT].isConnected() ? inputs[MATERIAL_INPUT].getVoltage() : 0.f)), 0, TM_COUNT - 1);
		padK = clamp((int)std::round(params[LEAF_PARAM].getValue() + (inputs[LEAF_INPUT].isConnected() ? inputs[LEAF_INPUT].getVoltage() : 0.f)), 0, TP_COUNT - 1);
		leavesK = clamp((int)std::round(params[DENSITY_PARAM].getValue() + (inputs[DENSITY_INPUT].isConnected() ? inputs[DENSITY_INPUT].getVoltage() : 0.f)), 0, CP_MAXLEAVES);
		float size = clamp(params[SIZE_PARAM].getValue() + inputs[SIZE_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		sizeK = 0.035f * SFS_POWABS(3.143f, size);
		scatterK = clamp(params[SCATTER_PARAM].getValue() + inputs[SCATTER_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		driftK = clamp(params[DRIFT_PARAM].getValue() + inputs[DRIFT_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		tiltK = clamp(params[TILT_PARAM].getValue() + inputs[TILT_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		speedK = 0.02f * SFS_POWABS(75.f, clamp(params[SPEED_PARAM].getValue() + inputs[SPEED_INPUT].getVoltage() * 0.2f, 0.f, 1.f));
		// X and Z move the column, which narrows to stay over the drum: at full SCATTER it
		// is already as wide as the drum and would otherwise have nowhere to go.
		colX = inputs[X_INPUT].isConnected() ? inputs[X_INPUT].getVoltage() * 0.19f : 0.f;
		colZ = inputs[Z_INPUT].isConnected() ? inputs[Z_INPUT].getVoltage() * 0.19f : 0.f;
		float off = std::sqrt(colX * colX + colZ * colZ);
		if (off > 0.95f) { colX *= 0.95f / off; colZ *= 0.95f / off; off = 0.95f; }
		colR = std::min(0.95f * scatterK, 0.95f - off);
		float dec = clamp(params[DECAY_PARAM].getValue() + inputs[DECAY_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		decayK = 0.25f * SFS_POWABS(16.f, dec);
		float tone = clamp(params[TONE_PARAM].getValue() + inputs[TONE_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		for (int m = 0; m < CP_MODES; m++) tilt[m] = SFS_POWABS(CP_RATIO[m], (tone - 0.5f) * 3.f);

		int r = (int)std::round(params[ROOT_PARAM].getValue());
		if (inputs[ROOT_INPUT].isConnected()) r += (int)std::round(sfs::rootCvSemis(inputs[ROOT_INPUT].getVoltage(), rootCvMode));
		root = ((r % 12) + 12) % 12;
		sc = sfs::busResolve(inputs[SCALE_INPUT], (int)std::round(params[SCALE_PARAM].getValue()));
		nT = tongueCount(sc.size);

		bool all = sr != coefSr || decayK != coefDecay;
		coefSr = sr; coefDecay = decayK;
		bool moved = nT != layoutN;
		for (int t = 0; t < CP_MAXN; t++) {
			float semis = sc.degree(t);
			float f = dsp::FREQ_C4 * std::pow(2.f, (float)regOct + ((float)root + semis) / 12.f);
			f = std::min(f, sr * 0.45f);
			TongueVoice& T = tv[t];
			if (T.semis != semis) moved = true;
			T.semis = semis;
			if (!all && f == T.freq) continue;
			T.freq = f;
			float rShort = std::sqrt(220.f / f);
			for (int m = 0; m < CP_MODES; m++) {
				float fm = f * CP_RATIO[m];
				if (fm >= sr * 0.45f) { T.lv[m] = 0.f; T.c[m] = 1.f; T.s[m] = 0.f; continue; }
				float t60 = std::max(CP_T60[m] * decayK * clamp(rShort, 0.5f, 1.6f), 0.02f);
				float dec1 = std::pow(10.f, -3.f / (t60 * sr));
				float w = 2.f * (float)M_PI * fm / sr;
				T.c[m] = dec1 * std::cos(w); T.s[m] = dec1 * std::sin(w);
				T.lv[m] = std::pow(10.f, CP_DB[m] / 20.f);
			}
		}
		if (moved) layoutTongues(nT);
		if (all) {
			static const float BF[2] = {190.f, 410.f}, BT[2] = {0.22f, 0.10f};
			for (int b = 0; b < 2; b++) {
				float dec1 = std::pow(10.f, -3.f / (BT[b] * sr));
				float w = 2.f * (float)M_PI * BF[b] / sr;
				bodyC[b] = dec1 * std::cos(w); bodyS[b] = dec1 * std::sin(w);
			}
		}
	}

	void process(const ProcessArgs& args) override {
		if ((ctl & 31) == 0) control(args.sampleRate);
		if ((ctl & 127) == 0) updateLive(args.sampleTime * 128.f);
		ctl++;

		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) spawnNow();
		gates();
		physAcc += args.sampleTime;
		while (physAcc >= CP_DT) { physAcc -= CP_DT; physics(CP_DT); }

		float mixL = 0.f, mixR = 0.f;
		for (int t = 0; t < CP_MAXN; t++) {
			TongueVoice& T = tv[t];
			if (!T.awake) { T.out = 0.f; continue; }
			bool touching = false;
			for (int i = 0; i < CP_CONTACTS; i++) {
				Contact& C = T.contact[i];
				if (C.ph >= 1.f) continue;
				touching = true;
				float f = 0.5f - 0.5f * SFS_COS2PI(C.ph);
				float af = f * C.amp;
				for (int m = 0; m < CP_MODES; m++) T.ex[m] += af * C.g[m];
				C.ph += C.dph;
			}
			float y = 0.f;
			for (int m = 0; m < CP_MODES; m++) {
				float re = T.re[m] * T.c[m] - T.im[m] * T.s[m] + T.ex[m];
				float im = T.re[m] * T.s[m] + T.im[m] * T.c[m];
				T.re[m] = re; T.im[m] = im;
				T.ex[m] = 0.f;
				y += im * T.lv[m];
			}
			T.out = y;
			T.env += (std::fabs(T.out) - T.env) * (std::fabs(T.out) > T.env ? 0.02f : 0.0005f);
			mixL += T.out * T.panL;
			mixR += T.out * T.panR;
			// Once it is down by 47 dB the rest of the tail is only adding to the wash
			// under whatever rings next, so it fades out over about 50 ms.
			if (!touching && (args.frame & 255) == 0) {
				float e = 0.f;
				for (int m = 0; m < CP_MODES; m++) e += T.re[m] * T.re[m] + T.im[m] * T.im[m];
				T.fading = e < 2e-5f;
				if (e < 1e-8f) { T.awake = false; T.fading = false; T.env = 0.f; for (int m = 0; m < CP_MODES; m++) { T.re[m] = 0.f; T.im[m] = 0.f; } }
			}
			if (T.fading) { for (int m = 0; m < CP_MODES; m++) { T.re[m] *= 0.9996f; T.im[m] *= 0.9996f; } }
		}
		// the shell, knocked by every hit and heard through both sides
		float force = 0.f;
		for (int i = 0; i < CP_SHELL_CONTACTS; i++) {
			Contact& C = shellHit[i];
			if (C.ph >= 1.f) continue;
			force += (0.5f - 0.5f * SFS_COS2PI(C.ph)) * C.amp;
			C.ph += C.dph;
		}
		float body = 0.f;
		for (int b = 0; b < 2; b++) {
			float re = bodyRe[b] * bodyC[b] - bodyIm[b] * bodyS[b] + force;
			float im = bodyRe[b] * bodyS[b] + bodyIm[b] * bodyC[b];
			bodyRe[b] = re; bodyIm[b] = im;
			body += im;
		}
		body = body * 0.1f;

		static const float OUT = 7.5f;
		float l = (mixL + body) * OUT, r = (mixR + body) * OUT;
		float dl = limBuf[0][limHead], dr = limBuf[1][limHead];
		limBuf[0][limHead] = l; limBuf[1][limHead] = r;
		limHead = (limHead + 1) % LOOK;
		float pk = 0.f;
		for (int k = 0; k < LOOK; k++) pk = std::max(pk, std::max(std::fabs(limBuf[0][k]), std::fabs(limBuf[1][k])));
		float want = pk > 6.f ? 6.f / pk : 1.f;
		if (args.sampleTime != limDt) {
			limDt = args.sampleTime;
			limAtk = 1.f - std::exp(-limDt / 0.0003f);
			limRel = 1.f - std::exp(-limDt / 0.35f);
		}
		limGain += (want - limGain) * (want < limGain ? limAtk : limRel);
		outputs[L_OUTPUT].setVoltage(sfs::softClip(dl * limGain));
		outputs[R_OUTPUT].setVoltage(sfs::softClip(dr * limGain));
		outputs[POLY_OUTPUT].setChannels(nT);
		for (int t = 0; t < nT; t++) outputs[POLY_OUTPUT].setVoltage(sfs::softClip(tv[t].out * OUT * 0.6f), t);
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		sfs::rootCvToJson(r, rootCvMode);
		json_object_set_new(r, "evenly", json_boolean(evenly));
		json_object_set_new(r, "leafForm", json_integer(leafForm));
		json_object_set_new(r, "regOct", json_integer(regOct));
		json_object_set_new(r, "camYaw", json_real(camYaw));
		json_object_set_new(r, "camTilt", json_real(camTilt));
		json_t* a = json_array();
		for (int i = 0; i < CP_MAXLEAVES; i++) {
			const CanopyLeaf& P = leaf[i];
			json_t* p = json_array();
			for (float v : {P.cx, P.cy, P.cz, P.ax, P.ay, P.az}) json_array_append_new(p, json_real(v));
			json_array_append_new(a, p);
		}
		json_object_set_new(r, "leaves", a);
		return r;
	}

	void dataFromJson(json_t* r) override {
		sfs::rootCvFromJson(r, this, ROOT_INPUT, rootCvMode);
		if (json_t* j = json_object_get(r, "evenly")) evenly = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "leafForm")) leafForm = clamp((int)json_integer_value(j), 0, (int)LF_MIXED);
		if (json_t* j = json_object_get(r, "regOct")) regOct = clamp((int)json_integer_value(j), -2, 0);
		if (json_t* j = json_object_get(r, "camYaw")) camYaw = (float)json_real_value(j);
		if (json_t* j = json_object_get(r, "camTilt")) camTilt = clamp((float)json_real_value(j), 0.15f, 1.4f);
		if (json_t* a = json_object_get(r, "leaves")) {
			int n = std::min((int)json_array_size(a), CP_MAXLEAVES);
			for (int i = 0; i < n; i++) {
				json_t* p = json_array_get(a, i);
				float v[6] = {};
				for (int k = 0; k < 6; k++) v[k] = (float)json_real_value(json_array_get(p, k));
				CanopyLeaf& P = leaf[i];
				P.cx = v[0]; P.cy = v[1]; P.cz = v[2];
				P.ax = v[3]; P.ay = v[4]; P.az = v[5];
			}
			publishLeaves();
		}
	}
};
const int Canopy::DRIFT_AXES;
const int Canopy::LOOK;

static Canopy* canopyPreview() {
	static Canopy* m = nullptr;
	if (m) return m;
	m = new Canopy();
	m->params[Canopy::RATE_PARAM].setValue(0.62f);
	int64_t frame = 0;
	sfs::previewRun(*m, 3.2f, frame);
	return m;
}

// ── the view ─────────────────────────────────────────────────────────────
// A perspective camera on an orbit round the middle of the volume.
struct CanopyView {
	float w, h, cy, sy, cp, sp, S;
	static constexpr float DIST = 8.0f;
	static constexpr float MIDY = 1.24f;
	CanopyView(float w_, float h_, float yaw, float tilt) : w(w_), h(h_) {
		cy = std::cos(yaw); sy = std::sin(yaw);
		cp = std::cos(tilt); sp = std::sin(tilt);
		// fitted to the volume at the default camera, so orbiting does not change the scale
		S = std::min(w / 2.4f, h / 3.42f);
	}
	struct Pt { float x, y, depth, scale; };
	Pt project(float x, float y, float z) const {
		float X = x * cy - z * sy;
		float Z = x * sy + z * cy;
		float Y = y - MIDY;
		float yc = Y * cp - Z * sp;      // up on screen
		float zc = Y * sp + Z * cp;      // toward the camera
		float k = DIST / std::max(DIST - zc, 0.2f);
		Pt p;
		p.x = w * 0.5f + X * S * k;
		p.y = h * 0.5f - yc * S * k;
		p.depth = zc;
		p.scale = S * k;
		return p;
	}
	// The ray through a screen point, in world coordinates.
	void ray(float px, float py, float o[3], float d[3]) const {
		float X = (px - w * 0.5f) / S, yc = (h * 0.5f - py) / S;
		// camera at (0, 0, DIST) in view space, looking toward -z, with the screen at z = 0
		float vx = X, vy = yc, vz = -DIST;
		fromView(0.f, 0.f, DIST, o); o[1] += MIDY;
		fromView(vx, vy, vz, d);
	}
	void fromView(float X, float yc, float zc, float out[3]) const {
		float Y = yc * cp + zc * sp;
		float Z = -yc * sp + zc * cp;
		out[0] = X * cy + Z * sy;
		out[1] = Y;
		out[2] = -X * sy + Z * cy;
	}
	// Where the ray meets the level plane y = planeY.
	bool onLevel(float px, float py, float planeY, float& x, float& z) const {
		float o[3], d[3];
		ray(px, py, o, d);
		if (std::fabs(d[1]) < 1e-5f) return false;
		float t = (planeY - o[1]) / d[1];
		if (t <= 0.f) return false;
		x = o[0] + d[0] * t; z = o[2] + d[2] * t;
		return true;
	}
	// Where the ray meets the plane through (px, py, pz) that faces the camera.
	void onFacing(float mx, float my, const float p[3], float out[3]) const {
		float o[3], d[3], f[3];
		ray(mx, my, o, d);
		fromView(0.f, 0.f, -1.f, f);
		float den = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
		float t = ((p[0] - o[0]) * f[0] + (p[1] - o[1]) * f[1] + (p[2] - o[2]) * f[2]) / den;
		for (int i = 0; i < 3; i++) out[i] = o[i] + d[i] * t;
	}
};
constexpr float CanopyView::DIST;
constexpr float CanopyView::MIDY;

static const NVGcolor CP_INK  = nvgRGB(0x23, 0x1F, 0x20);
static const NVGcolor CP_SOFT = nvgRGB(0x8A, 0x8A, 0x96);
static const NVGcolor CP_LINE = nvgRGB(0xC4, 0xC4, 0xCC);
static const NVGcolor CP_HOT  = nvgRGB(0xEC, 0x65, 0x2E);

struct CanopyDisplay : OpaqueWidget {
	Canopy* module = nullptr;
	enum Grab { NONE, BODY, HEIGHT, END_A, END_B, ORBIT };
	Grab grab = NONE;
	int  grabbed = -1;
	int  hover = -1;
	Vec  lastPos;
	float grabX = 0.f, grabZ = 0.f;   // where on the leaf it was picked up

	CanopyView view() const { return CanopyView(box.size.x, box.size.y, module->camYaw, module->camTilt); }

	// a point on a leaf's surface: `u` along it (-1 stem, 1 tip), `y` across, in half-lengths
	CanopyView::Pt leafAt(const CanopyView& V, const CanopyLeafLive& K, float bent, float u, float y, float world[3] = nullptr) const {
		float la = u * K.L;
		float lb = (K.shape.sweep * u * u + y * K.shape.width) * K.L;
		float lift = K.L * Canopy::leafLift(K.shape, K.form, u, y) - (u + 1.f) * K.L * bent;
		float x = K.cx + K.ax * la + K.bx * lb + K.nx * lift;
		float yy = K.cy + K.ay * la + K.ny * lift;
		float z = K.cz + K.az * la + K.bz * lb + K.nz * lift;
		if (world) { world[0] = x; world[1] = yy; world[2] = z; }
		return V.project(x, yy, z);
	}
	// a point `rho` of the way out from the centre to the edge, at an angle from the tip
	CanopyView::Pt leafPolar(const CanopyView& V, const CanopyLeafLive& K, float bent, float th, float rho, float world[3] = nullptr) const {
		float r = rho * module->formRadius(K.form, th);
		return leafAt(V, K, bent, CP_FORM_C[clamp(K.form, 0, LF_COUNT - 1)] + r * std::cos(th), r * std::sin(th), world);
	}
	static bool inPoly(const CanopyView::Pt* q, int n, float x, float y) {
		bool in = false;
		for (int i = 0, j = n - 1; i < n; j = i++)
			if ((q[i].y > y) != (q[j].y > y) && x < (q[j].x - q[i].x) * (y - q[i].y) / (q[j].y - q[i].y) + q[i].x) in = !in;
		return in;
	}
	// the leaf under a point, nearest the camera first; which end, if near one
	int leafUnder(Vec p, Grab* end = nullptr) const {
		CanopyView V = view();
		const Canopy& M = *module;
		int best = -1; float bestDepth = -1e9f;
		for (int i = 0; i < M.nLive; i++) {
			const CanopyLeafLive& K = M.live[i];
			CanopyView::Pt q[32];
			for (int k = 0; k < 32; k++) q[k] = leafPolar(V, K, M.bend[i], -(float)M_PI + 2.f * (float)M_PI * (float)k / 32.f, 1.f);
			CanopyView::Pt ca = V.project(K.cx + K.ax * K.L, K.cy + K.ay * K.L, K.cz + K.az * K.L);
			CanopyView::Pt cb = V.project(K.cx - K.ax * K.L, K.cy - K.ay * K.L, K.cz - K.az * K.L);
			float da = std::hypot(ca.x - p.x, ca.y - p.y), db = std::hypot(cb.x - p.x, cb.y - p.y);
			bool nearEnd = std::min(da, db) < 9.f;
			if (!inPoly(q, 32, p.x, p.y) && !nearEnd) continue;
			CanopyView::Pt c = V.project(K.cx, K.cy, K.cz);
			if (c.depth > bestDepth) {
				bestDepth = c.depth; best = i;
				if (end) *end = !nearEnd ? BODY : (da < db ? END_A : END_B);
			}
		}
		return best;
	}

	void onButton(const ButtonEvent& e) override {
		OpaqueWidget::onButton(e);
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) return;
		lastPos = e.pos;
		grab = ORBIT; grabbed = -1;
		Grab end = BODY;
		int i = leafUnder(e.pos, &end);
		if (i >= 0) {
			grabbed = i;
			grab = end;
			if (end == BODY && (e.mods & GLFW_MOD_SHIFT)) grab = HEIGHT;
			if (end == BODY) {
				float x = 0.f, z = 0.f;
				view().onLevel(e.pos.x, e.pos.y, module->leaf[i].cy, x, z);
				grabX = module->leaf[i].cx - x; grabZ = module->leaf[i].cz - z;
			}
		}
		e.consume(this);
	}
	void onHover(const HoverEvent& e) override {
		OpaqueWidget::onHover(e);
		lastPos = e.pos;
		if (module) hover = leafUnder(e.pos);
		e.consume(this);
	}
	void onLeave(const LeaveEvent& e) override { hover = -1; }
	void onDragHover(const DragHoverEvent& e) override {
		if (!module || grab == NONE || grab == ORBIT) return;
		Vec p(clamp(e.pos.x, 0.f, box.size.x), clamp(e.pos.y, 0.f, box.size.y));
		CanopyView V = view();
		if (grabbed >= 0 && grabbed < CP_MAXLEAVES) {
			CanopyLeaf& P = module->leaf[grabbed];
			if (grab == BODY) {
				float x, z;
				if (V.onLevel(p.x, p.y, P.cy, x, z)) { P.cx = clamp(x + grabX, -0.95f, 0.95f); P.cz = clamp(z + grabZ, -0.95f, 0.95f); }
			}
			else if (grab == HEIGHT) {
				P.cy = clamp(P.cy - (p.y - lastPos.y) / V.S, 0.3f, 2.4f);
			}
			else {
				// the end steers the leaf; its size comes from its form
				float sgn = grab == END_A ? 1.f : -1.f;
				float pl = std::max(std::sqrt(P.ax * P.ax + P.ay * P.ay + P.az * P.az), 1e-3f);
				float Ln = module->live[grabbed].L;
				float at[3] = {P.cx + sgn * P.ax / pl * Ln, P.cy + sgn * P.ay / pl * Ln, P.cz + sgn * P.az / pl * Ln};
				float hit[3];
				V.onFacing(p.x, p.y, at, hit);
				float ax = sgn * (hit[0] - P.cx), ay = sgn * (hit[1] - P.cy), az = sgn * (hit[2] - P.cz);
				float nl = std::sqrt(ax * ax + ay * ay + az * az);
				if (nl > 1e-3f) { P.ax = ax / nl; P.ay = ay / nl; P.az = az / nl; }
			}
			module->publishLeaves();
		}
		lastPos = p;
	}
	void onDragMove(const DragMoveEvent& e) override {
		if (!module || grab != ORBIT || e.button != GLFW_MOUSE_BUTTON_LEFT) return;
		Vec d = e.mouseDelta.div(APP->scene->rackScroll->getZoom());
		module->camYaw += d.x * 0.012f;
		module->camTilt = clamp(module->camTilt + d.y * 0.008f, 0.15f, 1.4f);
	}
	void onDragEnd(const DragEndEvent& e) override { grab = NONE; grabbed = -1; }
	// ── drawing ──────────────────────────────────────────────────────────
	// Two convex shells welded at the rim, a shape like a flying saucer, the top one
	// a plain dome with a hoop round it and the tongues cut into it as thin lines.
	void circleOn(NVGcontext* vg, const CanopyView& V, float r, float y) {
		nvgBeginPath(vg);
		for (int i = 0; i <= 72; i++) {
			float a = 2.f * (float)M_PI * (float)i / 72.f;
			CanopyView::Pt p = V.project(r * std::cos(a), y, r * std::sin(a));
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgClosePath(vg);
	}
	CanopyView::Pt onTongue(const CanopyView& V, const Canopy::TongueVoice& T, float a, float c, float dip) const {
		float x = T.tx + T.ux * a + T.uz * c, z = T.tz + T.uz * a - T.ux * c;
		return V.project(x, cpDomeY(x, z) - dip, z);
	}
	// the cut: up one side, round the free end, down the other
	void tonguePath(NVGcontext* vg, const CanopyView& V, const Canopy::TongueVoice& T, float dip) {
		nvgBeginPath(vg);
		CanopyView::Pt p = onTongue(V, T, T.len, -T.w, 0.f);
		nvgMoveTo(vg, p.x, p.y);
		for (int i = 0; i <= 12; i++) {
			float th = (float)M_PI * (float)i / 12.f;
			float a = T.w - T.w * std::sin(th), c = -T.w * std::cos(th);
			CanopyView::Pt q = onTongue(V, T, a, c, dip);
			nvgLineTo(vg, q.x, q.y);
		}
		p = onTongue(V, T, T.len, T.w, 0.f);
		nvgLineTo(vg, p.x, p.y);
	}
	void drawDrum(NVGcontext* vg, const CanopyView& V) {
		const Canopy& M = *module;
		// the lower shell, hanging under the rim
		static const float LOW_R[4] = {0.98f, 0.86f, 0.62f, 0.34f}, LOW_Y[4] = {-0.05f, -0.2f, -0.34f, -0.40f};
		nvgFillColor(vg, nvgRGB(0xD0, 0xD0, 0xD6));
		for (int k = 0; k < 4; k++) { circleOn(vg, V, LOW_R[k], LOW_Y[k]); nvgFill(vg); }
		for (int k = 0; k < 3; k++) {
			for (int i = 0; i < 48; i++) {
				float a0 = 2.f * (float)M_PI * (float)i / 48.f, a1 = 2.f * (float)M_PI * (float)(i + 1) / 48.f;
				CanopyView::Pt p0 = V.project(LOW_R[k] * std::cos(a0), LOW_Y[k], LOW_R[k] * std::sin(a0));
				CanopyView::Pt p1 = V.project(LOW_R[k] * std::cos(a1), LOW_Y[k], LOW_R[k] * std::sin(a1));
				CanopyView::Pt q0 = V.project(LOW_R[k + 1] * std::cos(a0), LOW_Y[k + 1], LOW_R[k + 1] * std::sin(a0));
				CanopyView::Pt q1 = V.project(LOW_R[k + 1] * std::cos(a1), LOW_Y[k + 1], LOW_R[k + 1] * std::sin(a1));
				nvgBeginPath(vg); nvgMoveTo(vg, p0.x, p0.y); nvgLineTo(vg, p1.x, p1.y); nvgLineTo(vg, q1.x, q1.y); nvgLineTo(vg, q0.x, q0.y); nvgClosePath(vg);
				nvgFill(vg);
			}
		}
		circleOn(vg, V, 1.f, 0.f);
		nvgFillColor(vg, nvgRGB(0xE8, 0xE8, 0xEC));
		nvgFill(vg);

		for (int t = 0; t < M.nT; t++) {
			const Canopy::TongueVoice& T = M.tv[t];
			float glow = clamp(T.env * 1.3f, 0.f, 1.f);
			float dip = clamp(T.env * 0.05f, 0.f, 0.035f);
			// ringing, a tongue goes from grey where it joins the drum to orange at its free end
			CanopyView::Pt gRoot = onTongue(V, T, T.len, 0.f, 0.f), gTip = onTongue(V, T, 0.f, 0.f, dip);
			if (glow > 0.01f) {
				tonguePath(vg, V, T, dip);
				nvgClosePath(vg);
				nvgFillPaint(vg, nvgLinearGradient(vg, gRoot.x, gRoot.y, gTip.x, gTip.y,
					nvgRGBAf(0.62f, 0.62f, 0.65f, 0.55f * glow), nvgRGBAf(CP_HOT.r, CP_HOT.g, CP_HOT.b, 0.75f * glow)));
				nvgFill(vg);
			}
			NVGcolor ink = nvgLerpRGBA(CP_INK, CP_HOT, glow);
			NVGpaint line = nvgLinearGradient(vg, gRoot.x, gRoot.y, gTip.x, gTip.y, CP_INK, ink);
			tonguePath(vg, V, T, dip);
			nvgStrokePaint(vg, line); nvgStrokeWidth(vg, 0.9f);
			nvgStroke(vg);
			// the tuning slits, hanging from the round end at lengths that differ
			nvgStrokeWidth(vg, 0.7f);
			for (int k = 0; k < 3; k++) {
				float c = T.w * (-0.5f + 0.5f * (float)k);
				float a0 = T.w - std::sqrt(std::max(T.w * T.w - c * c, 0.f));
				float reach = 0.34f + 0.26f * (float)(((t + 1) * (k + 2) * 7) % 5) / 4.f;
				float a1 = a0 + T.len * reach;
				CanopyView::Pt p0 = onTongue(V, T, a0, c, dip), p1 = onTongue(V, T, a1, c, dip * 0.5f);
				nvgBeginPath(vg); nvgMoveTo(vg, p0.x, p0.y); nvgLineTo(vg, p1.x, p1.y); nvgStroke(vg);
			}
			if (t == 0) {
				// the big tongue's relief holes
				for (float s : {-1.f, 1.f}) {
					CanopyView::Pt h = onTongue(V, T, T.len, s * T.w, 0.f);
					nvgBeginPath(vg); nvgCircle(vg, h.x, h.y, 1.5f);
					nvgFillColor(vg, CP_INK); nvgFill(vg);
				}
			}
		}
		// the hoop
		circleOn(vg, V, 1.f, 0.f);
		nvgStrokeColor(vg, CP_INK); nvgStrokeWidth(vg, 2.2f);
		nvgStroke(vg);
	}
	// The surface as small flat facets, each shaded by which way it faces, with the
	// outline, midrib and veins on top. The underside, when it shows, is darker.
	void drawLeaf(NVGcontext* vg, const CanopyView& V, const CanopyLeafLive& K, float bent, float flash, bool hot) {
		static const unsigned char SHADE[TP_MIX] = {0xCC, 0xA8, 0x58, 0xE4, 0xFF, 0x96};
		const int form = clamp(K.form, 0, LF_COUNT - 1);
		const int NS = CP_FORM_NS[form], NR = CP_FORM_NR[form];
		const float base = (float)SHADE[clamp(K.mat, 0, TP_MIX - 1)];
		const float alpha = K.mat == TP_GLASS ? 0.55f : 1.f;
		static const int MS = 72, MR = 4;
		CanopyView::Pt P[MS + 1][MR + 1];
		float W[MS + 1][MR + 1][3];
		for (int a = 0; a <= NS; a++) for (int b = 0; b <= NR; b++)
			P[a][b] = leafPolar(V, K, bent, -(float)M_PI + 2.f * (float)M_PI * (float)a / (float)NS, (float)b / (float)NR, W[a][b]);
		float view[3];
		V.fromView(0.f, 0.f, 1.f, view);
		struct Facet { float depth; int a, b; };
		Facet F[MS * MR];
		for (int a = 0; a < NS; a++) for (int b = 0; b < NR; b++) {
			F[a * NR + b].depth = 0.25f * (P[a][b].depth + P[a + 1][b].depth + P[a][b + 1].depth + P[a + 1][b + 1].depth);
			F[a * NR + b].a = a; F[a * NR + b].b = b;
		}
		const int nF = NS * NR;
		std::sort(F, F + nF, [](const Facet& x, const Facet& y) { return x.depth < y.depth; });
		for (int fi = 0; fi < nF; fi++) {
			const Facet& f = F[fi];
			const float* p0 = W[f.a][f.b + 1]; const float* p1 = W[f.a + 1][f.b + 1]; const float* p2 = W[f.a][f.b];
			float ux = p1[0] - p0[0], uy = p1[1] - p0[1], uz = p1[2] - p0[2];
			float vx = p2[0] - p0[0], vy = p2[1] - p0[1], vz = p2[2] - p0[2];
			float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (nl < 1e-8f) continue;
			nx /= nl; ny /= nl; nz /= nl;
			if (ny < 0.f) { nx = -nx; ny = -ny; nz = -nz; }     // the upper side's normal
			bool under = nx * view[0] + ny * view[1] + nz * view[2] < 0.f;
			float lit = clamp(0.62f * nx + 0.35f * nz + 0.7f * ny, 0.f, 1.f);
			float k = (under ? 0.62f : 0.78f) + 0.26f * lit;
			float sh = clamp(base * k, 0.f, 255.f) / 255.f;
			// a hit turns it green, by as much as the hit was hard
			NVGcolor col = nvgRGBAf(sh + (0.30f * k - sh) * flash, sh + (0.72f * k - sh) * flash, sh + (0.32f * k - sh) * flash, alpha + (1.f - alpha) * flash);
			const CanopyView::Pt& A = P[f.a][f.b]; const CanopyView::Pt& B = P[f.a + 1][f.b];
			const CanopyView::Pt& C = P[f.a + 1][f.b + 1]; const CanopyView::Pt& D = P[f.a][f.b + 1];
			nvgBeginPath(vg);
			nvgMoveTo(vg, A.x, A.y); nvgLineTo(vg, B.x, B.y); nvgLineTo(vg, C.x, C.y); nvgLineTo(vg, D.x, D.y); nvgClosePath(vg);
			nvgFillColor(vg, col); nvgFill(vg);
			if (alpha >= 1.f) { nvgStrokeColor(vg, col); nvgStrokeWidth(vg, 0.5f); nvgStroke(vg); }     // closes the seams between facets
		}
		const bool darkLeaf = base < 0x70;
		const NVGcolor ink = CP_INK;
		const float centre = CP_FORM_C[clamp(form, 0, LF_COUNT - 1)];

		// the veins: to the point of each lobe where it has them, evenly otherwise
		float tips[10]; int nTips = 0;
		if (form == LF_BEECH || form == LF_WILLOW) {
			for (int k = 0; k < 5; k++) tips[nTips++] = 0.42f + 0.46f * (float)k;
		}
		else if (form == LF_GINKGO) {
			for (float t : {0.22f, 0.48f, 0.74f, 0.98f}) tips[nTips++] = t;
		}
		else {
			const float* tab = module->formR[clamp(form, 0, LF_COUNT - 1)];
			for (int i = CP_FORM_TAB / 2 + 8; i < CP_FORM_TAB - 8 && nTips < 10; i++) {
				bool peak = tab[i] > tab[i - 1] && tab[i] >= tab[i + 1] && tab[i] > tab[i - 12] && tab[i] > tab[i + 12] && tab[i] > 0.3f;
				if (peak) tips[nTips++] = -(float)M_PI + 2.f * (float)M_PI * (float)i / (float)CP_FORM_TAB;
			}
		}
		nvgStrokeColor(vg, darkLeaf ? nvgRGBA(0xE8, 0xE8, 0xEC, 0x90) : nvgRGBA(0x23, 0x1F, 0x20, 0x70));
		nvgStrokeWidth(vg, 0.5f);
		for (int side = -1; side <= 1; side += 2) {
			for (int k = 0; k < nTips; k++) {
				float th = tips[k];
				float r = 0.88f * module->formRadius(form, (float)side * th);
				float endU = centre + r * std::cos(th), endY = (float)side * r * std::sin(th);
				float startU = CP_FORM_PALM[form] ? centre : centre + 0.4f * r * std::cos(th) - 0.12f;
				nvgBeginPath(vg);
				for (int i = 0; i <= 5; i++) {
					float q = (float)i / 5.f;
					// bowed toward the tip, as a real side vein is
					float uu = startU + (endU - startU) * q + (CP_FORM_PALM[form] ? 0.f : 0.08f * q * (1.f - q));
					CanopyView::Pt p = leafAt(V, K, bent, uu, endY * q);
					if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
				}
				nvgStroke(vg);
			}
		}
		const float baseU = centre - module->formRadius(form, (float)M_PI - 1e-3f);
		const float tipU = form == LF_GINKGO ? centre + 0.5f : centre + module->formRadius(form, 0.f) * 0.98f;
		// the midrib, which stops at the leaf
		nvgBeginPath(vg);
		for (int i = 0; i <= 16; i++) {
			CanopyView::Pt p = leafAt(V, K, bent, baseU + (tipU - baseU) * (float)i / 16.f, 0.f);
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgStrokeColor(vg, darkLeaf ? nvgRGB(0xD8, 0xD8, 0xDC) : ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		// the stem: heavier, short, and curving away to one side, so it reads as a stem
		// and not as more of the midrib
		nvgBeginPath(vg);
		for (int i = 0; i <= 6; i++) {
			float q = (float)i / 6.f;
			CanopyView::Pt p = leafAt(V, K, bent, baseU - 0.17f * q, 0.09f * q * q);
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgStrokeColor(vg, ink); nvgStrokeWidth(vg, 1.3f); nvgStroke(vg);
		// the outline
		nvgBeginPath(vg);
		for (int i = 0; i < 200; i++) {
			CanopyView::Pt p = leafPolar(V, K, bent, -(float)M_PI + 2.f * (float)M_PI * (float)i / 200.f, 1.f);
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgClosePath(vg);
		nvgStrokeColor(vg, ink); nvgStrokeWidth(vg, hot ? 1.2f : 0.8f); nvgStroke(vg);
		if (hot) {
			for (float u : {baseU, tipU}) {
				CanopyView::Pt c = leafAt(V, K, bent, u, 0.f);
				nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, 2.5f);
				nvgFillColor(vg, ink); nvgFill(vg);
			}
		}
	}
	// A plain circle, darker for heavier. Water is only an outline.
	void drawDrop(NVGcontext* vg, const CanopyView& V, const CanopyDrop& d) {
		static const unsigned char SHADE[TM_MIX] = {0xFF, 0xA8, 0xD8, 0x6C, 0x23};
		CanopyView::Pt p = V.project(d.x, d.y, d.z);
		float r = std::max(d.r * p.scale, 1.0f);
		nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, r);
		unsigned char sh = SHADE[clamp(d.mat, 0, TM_MIX - 1)];
		if (d.mat != TM_WATER) { nvgFillColor(vg, nvgRGB(sh, sh, sh)); nvgFill(vg); }
		nvgStrokeColor(vg, CP_INK); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
	}

	void draw(const DrawArgs& args) override {
		if (!module) { module = canopyPreview(); draw(args); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		CanopyView V = view();
		const Canopy& M = *module;
		nvgSave(vg);
		nvgScissor(vg, 0, 0, box.size.x, box.size.y);

		// the column the drops fall through, where X and Z have put it
		{
			float rr = std::max(M.colR, 0.03f);
			nvgStrokeColor(vg, CP_LINE); nvgStrokeWidth(vg, 0.7f);
			nvgBeginPath(vg);
			float xMin = 1e9f, xMax = -1e9f, aMin = 0.f, aMax = 0.f;
			for (int i = 0; i <= 48; i++) {
				float a = 2.f * (float)M_PI * (float)i / 48.f;
				CanopyView::Pt p = V.project(M.colX + rr * std::cos(a), CP_H, M.colZ + rr * std::sin(a));
				if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
				if (p.x < xMin) { xMin = p.x; aMin = a; }
				if (p.x > xMax) { xMax = p.x; aMax = a; }
			}
			nvgStroke(vg);
			for (float a : {aMin, aMax}) {
				float x = M.colX + rr * std::cos(a), z = M.colZ + rr * std::sin(a);
				CanopyView::Pt p = V.project(x, CP_H, z), q = V.project(x, cpDomeY(x, z), z);
				nvgBeginPath(vg); nvgMoveTo(vg, p.x, p.y); nvgLineTo(vg, q.x, q.y); nvgStroke(vg);
			}
		}

		// a drop that has rolled off the back falls behind the drum
		const float drumDepth = V.project(0.f, 0.f, 0.f).depth;
		auto behindDrum = [&](const CanopyDrop& d) {
			return d.y < 0.f && V.project(d.x, d.y, d.z).depth < drumDepth;
		};
		for (const CanopyDrop& d : M.drop) if (d.alive && behindDrum(d)) drawDrop(vg, V, d);

		drawDrum(vg, V);

		// shadows on the dome, straight down, so a height can be read
		nvgFillColor(vg, nvgRGBA(0x23, 0x1F, 0x20, 0x0E));
		for (int i = 0; i < M.nLive; i++) {
			const CanopyLeafLive& K = M.live[i];
			nvgBeginPath(vg);
			for (int k = 0; k < 28; k++) {
				float wd[3];
				leafPolar(V, K, M.bend[i], -(float)M_PI + 2.f * (float)M_PI * (float)k / 28.f, 1.f, wd);
				float rad = std::sqrt(wd[0] * wd[0] + wd[2] * wd[2]);
				if (rad > 0.97f) { wd[0] *= 0.97f / rad; wd[2] *= 0.97f / rad; }
				CanopyView::Pt p = V.project(wd[0], cpDomeY(wd[0], wd[2]), wd[2]);
				if (k == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
			}
			nvgClosePath(vg);
			nvgFill(vg);
		}
		for (const CanopyDrop& d : M.drop) {
			if (!d.alive || d.x * d.x + d.z * d.z > 0.85f) continue;
			float s = cpDomeY(d.x, d.z);
			if (d.y < s) continue;
			CanopyView::Pt p = V.project(d.x, s, d.z);
			float r = d.r * p.scale;
			nvgBeginPath(vg);
			nvgEllipse(vg, p.x, p.y, r, r * V.sp);
			nvgFillColor(vg, nvgRGBA(0x23, 0x1F, 0x20, 0x20));
			nvgFill(vg);
		}

		// leaves and drops together, farthest first
		struct Item { float depth; int kind, i; };
		std::vector<Item> items;
		items.reserve(CP_MAXLEAVES + CP_MAXDROPS);
		for (int i = 0; i < M.nLive; i++)
			items.push_back({V.project(M.live[i].cx, M.live[i].cy, M.live[i].cz).depth, 0, i});
		for (int i = 0; i < CP_MAXDROPS; i++) {
			if (!M.drop[i].alive || behindDrum(M.drop[i])) continue;
			items.push_back({V.project(M.drop[i].x, M.drop[i].y, M.drop[i].z).depth, 1, i});
		}
		std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.depth < b.depth; });
		for (const Item& it : items) {
			if (it.kind == 0) drawLeaf(vg, V, M.live[it.i], M.bend[it.i], M.leafFlash[it.i], it.i == hover || it.i == grabbed);
			else drawDrop(vg, V, M.drop[it.i]);
		}
		nvgRestore(vg);
	}
};

// A tight grid, so the screen gets the room. The foot is one row of jacks, inputs at the
// left as on the other modules, a gap, then the outputs under the last three columns
// of the controls. The screen fills everything left of the controls.
static const float CP_FX[9] = {7.62f, 19.05f, 30.48f, 41.91f, 53.34f, 64.77f, 111.76f, 123.19f, 134.62f};
static const float CP_PX[5] = {88.9f, 100.33f, 111.76f, 123.19f, 134.62f};
static const float CP_PY[3] = {24.13f, 55.88f, 87.63f};
static const float CP_CV = 10.16f;        // from a pot down to its jack
static const float CP_HEAD = 10.16f;      // from a pot up to its group's heading
static const float CP_Y_FOOT = 119.38f;
static const float CP_DISP_X = 2.f, CP_DISP_Y = 11.f, CP_DISP_W = 80.f, CP_DISP_H = 98.f;

struct CanopyWidget : ModuleWidget {
	CanopyWidget(Canopy* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/canopy.svg")));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("CANOPY");

		CanopyDisplay* disp = new CanopyDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(CP_DISP_X, CP_DISP_Y));
		disp->box.size = mm2px(Vec(CP_DISP_W, CP_DISP_H));
		addChild(disp);

		// each group's heading, in a rule across the five columns
		static const char* HEAD[3] = {"DROPS", "LEAVES", "DRUM"};
		for (int g = 0; g < 3; g++) {
			float y = CP_PY[g] - CP_HEAD;
			lbl->add(CP_PX[2], y, HEAD[g]);
			lbl->divider(CP_PX[0] - 5.f, y - 0.85f, CP_PX[2] - 8.f, y - 0.85f);
			lbl->divider(CP_PX[2] + 8.f, y - 0.85f, CP_PX[4] + 5.f, y - 0.85f);
		}

		// written out so tools/panel_reticules.py can read them
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[0], CP_PY[0])), module, Canopy::RATE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[0], CP_PY[0] + CP_CV)), module, Canopy::RATE_INPUT));
		lbl->pairDown(CP_PX[0], CP_PY[0], CP_PY[0] + CP_CV, "RATE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[1], CP_PY[0])), module, Canopy::MATERIAL_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[1], CP_PY[0] + CP_CV)), module, Canopy::MATERIAL_INPUT));
		lbl->pairDown(CP_PX[1], CP_PY[0], CP_PY[0] + CP_CV, "MATERIAL");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[2], CP_PY[0])), module, Canopy::SIZE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[2], CP_PY[0] + CP_CV)), module, Canopy::SIZE_INPUT));
		lbl->pairDown(CP_PX[2], CP_PY[0], CP_PY[0] + CP_CV, "SIZE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[3], CP_PY[0])), module, Canopy::SCATTER_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[3], CP_PY[0] + CP_CV)), module, Canopy::SCATTER_INPUT));
		lbl->pairDown(CP_PX[3], CP_PY[0], CP_PY[0] + CP_CV, "SCATTER");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[0], CP_PY[1])), module, Canopy::DENSITY_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[0], CP_PY[1] + CP_CV)), module, Canopy::DENSITY_INPUT));
		lbl->pairDown(CP_PX[0], CP_PY[1], CP_PY[1] + CP_CV, "DENSITY");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[1], CP_PY[1])), module, Canopy::LEAF_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[1], CP_PY[1] + CP_CV)), module, Canopy::LEAF_INPUT));
		lbl->pairDown(CP_PX[1], CP_PY[1], CP_PY[1] + CP_CV, "LEAF");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[2], CP_PY[1])), module, Canopy::DRIFT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[2], CP_PY[1] + CP_CV)), module, Canopy::DRIFT_INPUT));
		lbl->pairDown(CP_PX[2], CP_PY[1], CP_PY[1] + CP_CV, "DRIFT");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[3], CP_PY[1])), module, Canopy::SPEED_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[3], CP_PY[1] + CP_CV)), module, Canopy::SPEED_INPUT));
		lbl->pairDown(CP_PX[3], CP_PY[1], CP_PY[1] + CP_CV, "SPEED");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[4], CP_PY[1])), module, Canopy::TILT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[4], CP_PY[1] + CP_CV)), module, Canopy::TILT_INPUT));
		lbl->pairDown(CP_PX[4], CP_PY[1], CP_PY[1] + CP_CV, "TILT");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[0], CP_PY[2])), module, Canopy::DECAY_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[0], CP_PY[2] + CP_CV)), module, Canopy::DECAY_INPUT));
		lbl->pairDown(CP_PX[0], CP_PY[2], CP_PY[2] + CP_CV, "DECAY");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[1], CP_PY[2])), module, Canopy::TONE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[1], CP_PY[2] + CP_CV)), module, Canopy::TONE_INPUT));
		lbl->pairDown(CP_PX[1], CP_PY[2], CP_PY[2] + CP_CV, "TONE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[2], CP_PY[2])), module, Canopy::ROOT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[2], CP_PY[2] + CP_CV)), module, Canopy::ROOT_INPUT));
		lbl->pairDown(CP_PX[2], CP_PY[2], CP_PY[2] + CP_CV, "ROOT");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[3], CP_PY[2])), module, Canopy::SCALE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_PX[3], CP_PY[2] + CP_CV)), module, Canopy::SCALE_INPUT));
		lbl->pairDown(CP_PX[3], CP_PY[2], CP_PY[2] + CP_CV, "SCALE");

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_FX[0], CP_Y_FOOT)), module, Canopy::VOCT_INPUT));
		lbl->jack(CP_FX[0], CP_Y_FOOT, "V/OCT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_FX[1], CP_Y_FOOT)), module, Canopy::GATE_INPUT));
		lbl->jack(CP_FX[1], CP_Y_FOOT, "GATE");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_FX[2], CP_Y_FOOT)), module, Canopy::VEL_INPUT));
		lbl->jack(CP_FX[2], CP_Y_FOOT, "VEL");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_FX[3], CP_Y_FOOT)), module, Canopy::CLOCK_INPUT));
		lbl->jack(CP_FX[3], CP_Y_FOOT, "CLOCK");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_FX[4], CP_Y_FOOT)), module, Canopy::X_INPUT));
		lbl->jack(CP_FX[4], CP_Y_FOOT, "X");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CP_FX[5], CP_Y_FOOT)), module, Canopy::Z_INPUT));
		lbl->jack(CP_FX[5], CP_Y_FOOT, "Z");

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CP_FX[6], CP_Y_FOOT)), module, Canopy::POLY_OUTPUT));
		lbl->jackOnPlate(CP_FX[6], CP_Y_FOOT, "POLY");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CP_FX[7], CP_Y_FOOT)), module, Canopy::L_OUTPUT));
		lbl->jackOnPlate(CP_FX[7], CP_Y_FOOT, "LEFT");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(CP_FX[8], CP_Y_FOOT)), module, Canopy::R_OUTPUT));
		lbl->jackOnPlate(CP_FX[8], CP_Y_FOOT, "RIGHT");
	}

	void appendContextMenu(Menu* menu) override {
		Canopy* m = dynamic_cast<Canopy*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		sfs::rootCvMenu(menu, m, Canopy::ROOT_INPUT, m->rootCvMode);
		menu->addChild(createIndexSubmenuItem("Lowest tongue", {"C2 to B2", "C3 to B3", "C4 to B4"},
			[=]() { return (size_t)(m->regOct + 2); },
			[=](size_t v) { m->regOct = (int)v - 2; }));
		{
			std::vector<std::string> forms(CP_FORM_NAME, CP_FORM_NAME + LF_COUNT + 1);
			menu->addChild(createIndexPtrSubmenuItem("Leaf form", forms, &m->leafForm));
		}
		menu->addChild(createBoolPtrMenuItem("Evenly spaced drops (off: random spacing)", "", &m->evenly));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Drag a leaf to move it. Shift-drag to change its height. Drag an end to tilt it."));
		menu->addChild(createMenuLabel("Drag empty space to turn the view."));
		menu->addChild(createMenuItem("Reset leaf positions", "", [=]() { m->resetLeaves(); }));
		menu->addChild(createMenuItem("Reset view", "", [=]() { m->camYaw = 0.f; m->camTilt = 0.62f; }));
	}
};

Model* modelCanopy = createModel<Canopy, CanopyWidget>("Canopy");

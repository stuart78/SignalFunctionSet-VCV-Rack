#include "plugin.hpp"
#include "panel-style.hpp"
#include <cmath>
#include <cstdint>
#include <vector>

// ─── Field — nineteen random voltages that are one thing ─────────────────────
// A random modulation source with many outputs, where the outputs are RELATED
// BY WHERE THEY ARE. The nineteen jacks sit on a hexagonal grid (centre, a ring
// of six, a ring of twelve), and under the grid is one smooth random FIELD --
// a surface in x, y and time. Each jack simply reads the field at its own
// position. Every control is a statement about the field, and the relation
// between jacks falls out of geometry: two neighbours agree because they are
// sampling nearby points of the same surface, and how nearby is what COHERENCE
// means.
//
// It is nineteen sample-and-holds only at COHERENCE zero. Turn it up and the
// grid becomes weather: a pattern that appears at one edge and, with FLOW,
// travels to the other, so the jacks fire in a spatial order rather than as
// unrelated sources. BIAS tilts the amplitude by radius, which is what the
// rings are for -- centre hot and rim quiet, or the reverse.

static const int FD_N = 19;

// ── the field: 3D value noise, quintic-smoothed, two octaves ─────────────────
// Value noise rather than gradient noise because its lattice period is exactly
// 1: at a spatial frequency of 1 per jack pitch, neighbours are uncorrelated,
// which is the honest bottom of the COHERENCE knob and needs no guesswork.
struct FieldNoise {
	static inline uint32_t hash(int x, int y, int z) {
		uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu;
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		return h;
	}
	static inline float lat(int x, int y, int z) {          // -1 .. 1 at a lattice point
		return (float)(hash(x, y, z) & 0xffffu) / 32767.5f - 1.f;
	}
	static inline float fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }
	static float at(float x, float y, float z) {
		int xi = (int)std::floor(x), yi = (int)std::floor(y), zi = (int)std::floor(z);
		float fx = fade(x - xi), fy = fade(y - yi), fz = fade(z - zi);
		float v = 0.f;
		for (int dz = 0; dz < 2; dz++) {
			float wz = dz ? fz : 1.f - fz;
			for (int dy = 0; dy < 2; dy++) {
				float wy = dy ? fy : 1.f - fy;
				float a = lat(xi, yi + dy, zi + dz), b = lat(xi + 1, yi + dy, zi + dz);
				v += wz * wy * (a + (b - a) * fx);
			}
		}
		return v;
	}
	// Two octaves, the second at half weight: one octave of value noise is a
	// little too round to read as random. Renormalised to about -1..1.
	static float sample(float x, float y, float t) {
		return (at(x, y, t) + 0.5f * at(x * 2.03f + 7.1f, y * 2.03f + 3.7f, t * 1.7f + 11.3f)) / 1.2f;
	}
};

// ── jack geometry, in mm on the panel and in grid units for the field ────────
// Pointy-top hexagonal lattice, cell size 8.6mm (centre to corner, so
// neighbouring jacks are sqrt3 * 8.6 = 14.9mm apart), centred on the panel.
// GX/GY are the same positions in units of the cell size, which is what the
// field samples in; the COHERENCE range was measured on these distances. The
// reticule tool reads JX/JY straight from here.
static const float FD_JX[FD_N] = {40.63f, 33.18f, 48.08f, 55.53f, 48.08f, 33.18f, 25.74f, 25.74f, 40.63f, 55.53f, 62.97f, 70.42f, 62.97f, 55.53f, 40.63f, 25.74f, 18.30f, 10.85f, 18.30f};
static const float FD_JY[FD_N] = {44.99f, 32.09f, 32.09f, 44.99f, 57.89f, 57.89f, 44.99f, 19.20f, 19.20f, 19.20f, 32.09f, 44.99f, 57.89f, 70.79f, 70.79f, 70.79f, 57.89f, 44.99f, 32.09f};
static const float FD_GX[FD_N] = {0.0000f, -0.8660f, 0.8660f, 1.7321f, 0.8660f, -0.8660f, -1.7321f, -1.7321f, 0.0000f, 1.7321f, 2.5981f, 3.4641f, 2.5981f, 1.7321f, 0.0000f, -1.7321f, -2.5981f, -3.4641f, -2.5981f};
static const float FD_GY[FD_N] = {0.0000f, -1.5000f, -1.5000f, 0.0000f, 1.5000f, 1.5000f, 0.0000f, -3.0000f, -3.0000f, -3.0000f, -1.5000f, 0.0000f, 1.5000f, 3.0000f, 3.0000f, 3.0000f, 1.5000f, 0.0000f, -1.5000f};
static const float FD_R[FD_N]  = {0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
static const float FD_CX = 40.63f, FD_CY = 44.99f, FD_PITCH = 8.6f;


// ── the tiling, shared by the automaton and the display ──────────────────────
// The plate is tiled with pointy-top hexagons on the jack lattice subdivided
// FD_SUB times, so every jack sits on a cell centre. The cell list is built
// once and shared: the Life automaton lives on it, and the display draws it.
static const int   FD_SUB = 4;
static const float FD_PLATE_R = 34.6f;               // mm, centre to corner, flat-top
struct HexCell { int i, j; float gx, gy; int nb[6]; };
static bool fieldInFlatHex(float x, float y, float R) {
	float ax = std::fabs(x), ay = std::fabs(y);
	return ay <= 0.866f * R && 0.866f * ax + 0.5f * ay <= 0.866f * R;
}
static const std::vector<HexCell>& fieldCells() {
	static std::vector<HexCell> cells;
	if (!cells.empty()) return cells;
	const float sc = FD_PITCH / FD_SUB;                // cell circumradius, mm
	int n = (int)std::ceil(FD_PLATE_R / (sc * 1.5f)) + 2;
	std::vector<std::pair<int,int>> ij;
	for (int j = -n; j <= n; j++)
		for (int i = -n; i <= n; i++) {
			float dx = sc * (1.7320508f * i + 0.8660254f * j), dy = sc * 1.5f * j;
			if (!fieldInFlatHex(dx, dy, FD_PLATE_R + sc * 1.2f)) continue;   // near enough to touch the plate
			HexCell c; c.i = i; c.j = j; c.gx = dx / FD_PITCH; c.gy = dy / FD_PITCH;
			for (int q = 0; q < 6; q++) c.nb[q] = -1;
			cells.push_back(c); ij.push_back({i, j});
		}
	static const int D[6][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,-1},{-1,1}};
	for (size_t a = 0; a < cells.size(); a++)
		for (int q = 0; q < 6; q++) {
			int ti = cells[a].i + D[q][0], tj = cells[a].j + D[q][1];
			for (size_t b = 0; b < cells.size(); b++)
				if (ij[b].first == ti && ij[b].second == tj) { cells[a].nb[q] = (int)b; break; }
		}
	return cells;
}
// which cell each jack sits on
static const int* fieldJackCell() {
	static int jc[FD_N] = {-1};
	if (jc[0] == -1) {
		const std::vector<HexCell>& cells = fieldCells();
		for (int k = 0; k < FD_N; k++) {
			float bd = 1e9f; jc[k] = 0;
			for (size_t c = 0; c < cells.size(); c++) {
				float dx = cells[c].gx - FD_GX[k], dy = cells[c].gy - FD_GY[k], d = dx * dx + dy * dy;
				if (d < bd) { bd = d; jc[k] = (int)c; }
			}
		}
	}
	return jc;
}

// ── how the field moves ──────────────────────────────────────────────────────
// FLOW is always the speed and DIR always the direction, but what they move
// depends on the animation: the window across the field (Flow), the scale of
// the field (Zoom: DIR at 0 radiates outward, at 180 inward), the field's
// rotation (Spin: 0 clockwise, 180 anticlockwise), or each jack's own window
// (Random). Life is a cellular automaton on the tiling and moves on RATE.
// Rain and Wave are one machine, a damped wave equation on the cell graph,
// driven two ways: drops that dent the surface and ring outward, or crests
// launched from the edge facing DIR that cross the plate and reflect. RATE is
// the wave speed, FLOW how often, COHERENCE how far a wave travels before it
// dies (the damping, inverted).
//
// Then four more simulations on the same cell graph. React is Gray-Scott
// reaction-diffusion: two chemicals, spots that split and stripes that grow;
// Cyclic is the rock-paper-scissors automaton that organises itself into
// spirals; Sand is the Bak-Tang-Wiesenfeld sandpile, grains dropped until a
// cell topples onto its neighbours, avalanches of every size; Worley is a set
// of drifting seed points, each cell reading its distance to the nearest, so
// the plate is a moving Voronoi mosaic with ridges between the cells.
enum FieldAnim { ANIM_FLOW, ANIM_ZOOM, ANIM_SPIN, ANIM_RANDOM, ANIM_LIFE, ANIM_RAIN, ANIM_WAVE,
                 ANIM_REACT, ANIM_CYCLIC, ANIM_SAND, ANIM_WORLEY, ANIM_COUNT };
static const char* FIELD_ANIM_NAMES[ANIM_COUNT] = {"Flow", "Zoom", "Spin", "Random", "Life", "Rain", "Wave",
                                                   "React", "Cyclic", "Sand", "Worley"};
// The simulations all leave a bipolar value per cell in vis[]; Life keeps its
// own 0..1 with a fade.
static inline bool fieldIsSim(int a) { return a >= ANIM_RAIN; }

struct FieldMotion {
	int    anim = ANIM_FLOW;
	double t = 0.0;                 // field time, lattice units
	double ox = 0.0, oy = 0.0;      // flow offset, jack units
	float  phase = 0.f;             // zoom: 0..1, one doubling per cycle
	float  theta = 0.f;             // spin, radians
	float  k = 0.17f;               // spatial frequency
	// A point of the field under the current motion, in jack units.
	float at(float gx, float gy) const {
		switch (anim) {
			case ANIM_ZOOM: {
				// AN INFINITE ZOOM out of two octaves. Layer A is the field at
				// scale 2^-phase, layer B one octave up; as phase runs 0 -> 1
				// A fades out and B fades in, and at the wrap B is exactly
				// what A was at 0, so the zoom never has a seam.
				float sA = std::pow(2.f, -phase), sB = 2.f * sA;
				float wB = 0.5f - 0.5f * std::cos((float)M_PI * phase), wA = 1.f - wB;
				return wA * FieldNoise::sample(gx * k * sA, gy * k * sA, (float)t)
				     + wB * FieldNoise::sample(gx * k * sB, gy * k * sB, (float)t);
			}
			case ANIM_SPIN: {
				float c = std::cos(theta), s = std::sin(theta);
				float rx = c * gx - s * gy, ry = s * gx + c * gy;
				return FieldNoise::sample(rx * k, ry * k, (float)t);
			}
			default:
				return FieldNoise::sample((gx + (float)ox) * k, (gy + (float)oy) * k, (float)t);
		}
	}
};

// ── a colour for a value, by palette ─────────────────────────────────────────
// Purely aesthetic. Each entry is the colour BELOW the offset and the colour
// ABOVE it; brightness follows magnitude. Rainbow maps the value across the
// hue circle instead.
struct FieldPalette { const char* name; NVGcolor lo, hi; };
static const FieldPalette FIELD_PALETTES[] = {
	{"Blue / Orange",  nvgRGB(0x00, 0x97, 0xDE), nvgRGB(0xEC, 0x65, 0x2E)},
	{"Teal / Magenta", nvgRGB(0x1F, 0xC8, 0xB4), nvgRGB(0xE6, 0x3B, 0xA5)},
	{"Green / Violet", nvgRGB(0x3F, 0xBF, 0x6F), nvgRGB(0x9B, 0x6B, 0xD6)},
	{"Ice / Fire",     nvgRGB(0x8F, 0xD8, 0xFF), nvgRGB(0xFF, 0x3D, 0x2E)},
	{"Lime / Plum",    nvgRGB(0xC8, 0xF0, 0x3A), nvgRGB(0x7A, 0x1F, 0x6E)},
	{"Amber / Indigo", nvgRGB(0xFF, 0xB0, 0x2E), nvgRGB(0x3A, 0x3F, 0xC8)},
	{"Mono",           nvgRGB(0x50, 0x50, 0x68), nvgRGB(0xEE, 0xEE, 0xF4)},
	{"Rainbow",        nvgRGB(0, 0, 0),          nvgRGB(0, 0, 0)},
};
static const int FIELD_NPAL = (int)(sizeof(FIELD_PALETTES) / sizeof(FIELD_PALETTES[0]));
static const int FIELD_PAL_RAINBOW = FIELD_NPAL - 1;
static NVGcolor fieldColour(int pal, float v) {              // v in -1..1
	v = clamp(v, -1.f, 1.f);
	if (pal == FIELD_PAL_RAINBOW) {
		// -1 = violet, 0 = green, +1 = red: the spectrum laid across the range
		float hue = (1.f - (v + 1.f) * 0.5f) * 0.78f;
		return nvgHSLA(hue, 0.85f, 0.52f, (unsigned char)(90 + 165 * std::fabs(v)));
	}
	const FieldPalette& p = FIELD_PALETTES[clamp(pal, 0, FIELD_NPAL - 1)];
	NVGcolor c = v >= 0.f ? p.hi : p.lo;
	c.a = 0.08f + 0.85f * std::fabs(v);
	return c;
}

// RATE reads as the number it is. 0.002 Hz at the bottom -- a new feature
// every eight minutes or so, glacial on purpose, since a field that only ever
// drifts is the whole point of one with FLOW -- up to 20 Hz. Four decades,
// exponential; the first draft started at 0.02 and the bottom was not slow.
static inline float fieldRateHz(float r) { return 0.002f * std::pow(10000.f, r); }
struct FieldRateQ : ParamQuantity {
	std::string getDisplayValueString() override {
		float hz = fieldRateHz(getValue());
		if (hz < 0.1f) return string::f("%.4f Hz (%.0f s)", hz, 1.f / hz);
		if (hz < 1.f)  return string::f("%.3f Hz (%.1f s)", hz, 1.f / hz);
		return string::f("%.2f Hz", hz);
	}
};

struct Field : Module {
	enum ParamId {
		AMP_PARAM, OFFSET_PARAM, COHER_PARAM, RATE_PARAM, FLOW_PARAM, DIR_PARAM, BIAS_PARAM,
		SMOOTH_PARAM,
		ANIM_PARAM,                 // appended: params serialise by index
		PARAMS_LEN
	};
	enum InputId {
		AMP_INPUT, OFFSET_INPUT, COHER_INPUT, RATE_INPUT, FLOW_INPUT, DIR_INPUT, BIAS_INPUT,
		SMOOTH_INPUT, TRIG_INPUT,
		ANIM_INPUT,                 // appended
		INPUTS_LEN
	};
	enum OutputId { ENUMS(OUT_OUTPUT, FD_N), POLY_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	FieldMotion m;
	float  ampV = 5.f, offV = 0.f, bias = 0.f;
	int    palette = 0;
	float  target[FD_N] = {0.f}, out[FD_N] = {0.f}, latchCandidate[FD_N] = {0.f};
	bool   held = false;            // TRIG patched: outputs hold between triggers
	dsp::SchmittTrigger trig[FD_N];
	int    ctl = 0;
	// Random: each jack's own window, drifting in its own wandering direction
	float  rox[FD_N] = {0.f}, roy[FD_N] = {0.f};
	// Life: the automaton on the tiling. vis[] is what the display draws --
	// 1 alive, fading after death -- and what the jacks read.
	std::vector<uint8_t> life, lifeNext;
	std::vector<float>   vis;
	float  lifeAcc = 0.f;
	int    lifeStill = 0, lifeGen = 0;
	// Rain / Wave: the surface, its previous step (leapfrog), and the timers
	std::vector<float> wu, wuPrev;
	float  waveAcc = 0.f, dropAcc = 0.f;
	int    lastAnim = -1;
	// React: the two chemicals.  Cyclic: a state per cell.  Sand: grains.
	std::vector<float>   rdU, rdV;
	std::vector<uint8_t> cyc, sand, sandNext;
	float  simAcc = 0.f, grainAcc = 0.f;
	// Worley: up to sixteen seeds wandering the plate, in jack units
	static const int WMAX = 16;
	float  sx[WMAX] = {0.f}, sy[WMAX] = {0.f}, sw[WMAX] = {0.f};
	int    seedN = 8;
	uint32_t rng = 0x2545F491u;
	float  rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng & 0xffffff) / 16777216.f; }

	Field() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(AMP_PARAM,    0.f, 10.f, 5.f, "Amplitude", " V");
		configParam(OFFSET_PARAM, -5.f, 5.f, 0.f, "Offset", " V");
		configParam(COHER_PARAM,  0.f, 1.f, 0.5f, "Coherence", "%", 0.f, 100.f);
		configParam<FieldRateQ>(RATE_PARAM, 0.f, 1.f, 0.45f, "Rate");
		configParam(FLOW_PARAM,   0.f, 1.f, 0.f, "Flow", " jacks/s", 0.f, 4.f);
		configParam(DIR_PARAM,    0.f, 1.f, 0.f, "Flow direction", "°", 0.f, 360.f);
		configParam(BIAS_PARAM,  -1.f, 1.f, 0.f, "Bias (centre <> rim)");
		configParam(SMOOTH_PARAM, 0.f, 1.f, 0.f, "Smooth", " s", 0.f, 2.f);
		configSwitch(ANIM_PARAM, 0.f, (float)(ANIM_COUNT - 1), 0.f, "Animation",
		             std::vector<std::string>(FIELD_ANIM_NAMES, FIELD_ANIM_NAMES + ANIM_COUNT));
		configInput(AMP_INPUT, "Amplitude CV (+-5V = full range)");
		configInput(OFFSET_INPUT, "Offset CV");
		configInput(COHER_INPUT, "Coherence CV");
		configInput(RATE_INPUT, "Rate CV");
		configInput(FLOW_INPUT, "Flow CV");
		configInput(DIR_INPUT, "Flow direction CV");
		configInput(BIAS_INPUT, "Bias CV");
		configInput(SMOOTH_INPUT, "Smooth CV");
		configInput(TRIG_INPUT, "Sample & hold trigger (poly: channel N holds jack N; mono holds all)");
		configInput(ANIM_INPUT, "Animation CV (1V per mode, added to the knob)");
		for (int i = 0; i < FD_N; i++)
			configOutput(OUT_OUTPUT + i, string::f("Jack %d", i + 1));
		configOutput(POLY_OUTPUT, "Poly: jacks 1-16");
		size_t nc = fieldCells().size();
		life.assign(nc, 0); lifeNext.assign(nc, 0); vis.assign(nc, 0.f);
		wu.assign(nc, 0.f); wuPrev.assign(nc, 0.f);
		rdU.assign(nc, 1.f); rdV.assign(nc, 0.f); cyc.assign(nc, 0); sand.assign(nc, 0); sandNext.assign(nc, 0);
		for (int i = 0; i < WMAX; i++) seedReset(i);
		fieldJackCell();
		reseed(0.3f);
	}

	inline float pv(int p, int in, float lo, float hi) {
		float v = params[p].getValue();
		if (inputs[in].isConnected()) v += inputs[in].getVoltage() * 0.1f * (hi - lo);
		return clamp(v, lo, hi);
	}

	// ── Life ─────────────────────────────────────────────────────────────────
	// Hex Life, B2/S345 WITH A TRICKLE: a dead cell with exactly two live
	// neighbours is born, a live one with three, four or five survives, and
	// three dead cells in a thousand are born for no reason each step. The rule
	// was chosen by running nine of them on this exact cell graph for 3000
	// steps: B2/S34, the usual hex rule, settles to 2% alive in a few blinkers
	// within a hundred steps and is a picture of nothing; B24/S23 changes 44%
	// of the cells every step, which is noise. B2/S345 with the trickle holds
	// about half the cells alive with a sixth of them changing each step and
	// never went quiet -- a surface that boils. The reseed below is a safety
	// net it did not need in 3000 steps; COHERENCE sets the seed density.
	void reseed(float density) {
		for (size_t c = 0; c < life.size(); c++) life[c] = rnd() < density ? 1 : 0;
		lifeStill = 0; lifeGen++;
	}
	void lifeStep(float density) {
		const std::vector<HexCell>& cells = fieldCells();
		int changed = 0, alive = 0;
		for (size_t c = 0; c < cells.size(); c++) {
			int n = 0;
			for (int q = 0; q < 6; q++) if (cells[c].nb[q] >= 0) n += life[cells[c].nb[q]];
			uint8_t next = life[c] ? (uint8_t)(n >= 3 && n <= 5) : (uint8_t)(n == 2);
			if (!next && !life[c] && rnd() < 0.003f) next = 1;
			lifeNext[c] = next;
			changed += (next != life[c]); alive += next;
		}
		life.swap(lifeNext);
		lifeStill = changed ? 0 : lifeStill + 1;
		if (alive < (int)(0.02f * cells.size()) || lifeStill > 6) reseed(density);
		for (size_t c = 0; c < cells.size(); c++)
			vis[c] = life[c] ? 1.f : vis[c] * 0.55f;
	}

	// ── the surface ──────────────────────────────────────────────────────────
	// Leapfrog on the hex graph: u' = 2u - u_prev + c2 * L(u) - damping * (u -
	// u_prev), with L the mean difference to the neighbours a cell has (so the
	// edge reflects). c2 = 0.9 moves a front about 0.15 cells per step and
	// measured stable; RATE sets steps per second.
	//
	// THE MEAN IS REMOVED EVERY STEP. A wave equation conserves the mean
	// displacement, so a rain of downward drops left a permanent dent that
	// only ever deepened -- measured at 814 units after 6000 steps, with the
	// clamp turning the whole plate one colour. A pond keeps its volume: a
	// drop pushes water down here and up everywhere else.
	void waveStep(float damp) {
		const std::vector<HexCell>& cells = fieldCells();
		double mean = 0.0;
		for (size_t c = 0; c < cells.size(); c++) {
			float lap = 0.f; int nn = 0;
			for (int q = 0; q < 6; q++) if (cells[c].nb[q] >= 0) { lap += wu[cells[c].nb[q]] - wu[c]; nn++; }
			lap /= (float)std::max(nn, 1);
			float vel = (wu[c] - wuPrev[c]) * (1.f - damp);
			vis[c] = wu[c] + vel + 0.9f * lap;       // vis doubles as the scratch for u'
			mean += vis[c];
		}
		mean /= (double)cells.size();
		wuPrev.swap(wu);                             // old u becomes u_prev
		for (size_t c = 0; c < cells.size(); c++) wu[c] = vis[c] - (float)mean;
	}
	// A drop: the surface pushed DOWN at a random cell and, half as much, its
	// neighbours, so the splash is round. The ring that follows is the wave
	// equation's own answer -- nothing here draws a ring.
	void rainDrop() {
		const std::vector<HexCell>& cells = fieldCells();
		int c = (int)(rnd() * cells.size()) % (int)cells.size();
		wu[c] -= 1.2f; wuPrev[c] -= 0.6f;
		for (int q = 0; q < 6; q++) if (cells[c].nb[q] >= 0) { wu[cells[c].nb[q]] -= 0.6f; wuPrev[cells[c].nb[q]] -= 0.3f; }
	}
	// A crest: the band of cells along the edge facing DIR lifted together, so
	// a plane wave sets off across the plate. Its trough follows by itself.
	void waveCrest(float dir) {
		const std::vector<HexCell>& cells = fieldCells();
		float ux = std::cos(dir), uy = std::sin(dir), lo = 1e9f;
		for (auto& c : cells) lo = std::min(lo, c.gx * ux + c.gy * uy);
		for (size_t i = 0; i < cells.size(); i++) {
			float d = cells[i].gx * ux + cells[i].gy * uy - lo;
			if (d < 0.45f) { wu[i] += 0.8f; wuPrev[i] += 0.5f; }
		}
	}

	// ── React: Gray-Scott ────────────────────────────────────────────────────
	// u feeds, v eats u and reproduces, both diffuse, v diffuses slower. With
	// the textbook diffusion the pattern wavelength is ten to twenty cells and
	// this plate is eight across, so the diffusion is scaled down by four to
	// bring the spots and stripes onto the plate. COHERENCE walks the feed/kill
	// pair through the regimes: moving spots at the bottom, dividing spots in
	// the middle, mazes at the top. FLOW seeds new v now and then so a run that
	// burns out restarts.
	void reactSeed() {
		for (size_t c = 0; c < rdU.size(); c++) { rdU[c] = 1.f; rdV[c] = 0.f; }
		const std::vector<HexCell>& cells = fieldCells();
		for (int k = 0; k < 4; k++) {
			int c = (int)(rnd() * cells.size()) % (int)cells.size();
			rdV[c] = 1.f; for (int q = 0; q < 6; q++) if (cells[c].nb[q] >= 0) rdV[cells[c].nb[q]] = 1.f;
		}
	}
	void reactStep(float F, float K) {
		const std::vector<HexCell>& cells = fieldCells();
		const float Du = 0.25f, Dv = 0.125f;
		for (size_t c = 0; c < cells.size(); c++) {
			float lu = 0.f, lv = 0.f; int nn = 0;
			for (int q = 0; q < 6; q++) if (cells[c].nb[q] >= 0) { lu += rdU[cells[c].nb[q]]; lv += rdV[cells[c].nb[q]]; nn++; }
			lu = lu / std::max(nn, 1) - rdU[c]; lv = lv / std::max(nn, 1) - rdV[c];
			float u = rdU[c], v = rdV[c], uvv = u * v * v;
			wu[c] = clamp(u + Du * lu - uvv + F * (1.f - u), 0.f, 1.f);        // wu/wuPrev as scratch
			wuPrev[c] = clamp(v + Dv * lv + uvv - (F + K) * v, 0.f, 1.f);
		}
		rdU.swap(wu); rdV.swap(wuPrev);
	}
	// ── Cyclic: rock-paper-scissors ──────────────────────────────────────────
	// N states in a ring; a cell is taken by the state that beats it when at
	// least one neighbour holds it. From noise it organises into spirals whose
	// arms sweep past a jack as a sawtooth. COHERENCE sets N, 4 to 8.
	void cyclicStep(int N) {
		const std::vector<HexCell>& cells = fieldCells();
		for (size_t c = 0; c < cells.size(); c++) {
			uint8_t nxt = (uint8_t)((cyc[c] + 1) % N); sandNext[c] = cyc[c];
			for (int q = 0; q < 6; q++) if (cells[c].nb[q] >= 0 && cyc[cells[c].nb[q]] == nxt) { sandNext[c] = nxt; break; }
		}
		cyc.swap(sandNext);
	}
	// ── Sand: the Bak-Tang-Wiesenfeld pile ───────────────────────────────────
	// Grains fall on random cells (FLOW sets how fast); a cell holding six
	// topples one onto each neighbour, and grains that fall off the edge are
	// gone. One round of toppling per step, so an avalanche is seen travelling
	// rather than resolved in a flash; a toppling cell reads full positive.
	void sandStep(bool& toppledAny) {
		const std::vector<HexCell>& cells = fieldCells();
		for (size_t c = 0; c < cells.size(); c++) sandNext[c] = sand[c];
		toppledAny = false;
		for (size_t c = 0; c < cells.size(); c++) {
			if (sand[c] < 6) { vis[c] = (float)sand[c] / 5.f * 1.6f - 1.f; continue; }
			sandNext[c] -= 6; toppledAny = true; vis[c] = 1.f;
			for (int q = 0; q < 6; q++) if (cells[c].nb[q] >= 0) sandNext[cells[c].nb[q]]++;
		}
		sand.swap(sandNext);
	}
	// ── Worley ───────────────────────────────────────────────────────────────
	void seedReset(int i) {
		float a = rnd() * 2.f * (float)M_PI, r = std::sqrt(rnd()) * 3.4f;
		sx[i] = r * std::cos(a); sy[i] = r * std::sin(a); sw[i] = rnd() * 2.f * (float)M_PI;
	}
	// Distance to the nearest seed, as a bipolar value: +1 on a seed, -1 on the
	// ridges between cells, the scale set by how densely the seeds are packed.
	float worleyAt(float gx, float gy) const {
		float best = 1e9f;
		for (int i = 0; i < seedN; i++) { float dx = gx - sx[i], dy = gy - sy[i]; best = std::min(best, dx * dx + dy * dy); }
		float d0 = 0.75f * std::sqrt(41.f / (float)seedN);          // plate area ~41 jack units squared
		return clamp(1.f - 2.f * std::sqrt(best) / d0, -1.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		if (--ctl <= 0) {
			ctl = 16;
			float dt = 16.f * args.sampleTime;
			ampV = pv(AMP_PARAM, AMP_INPUT, 0.f, 10.f);
			offV = pv(OFFSET_PARAM, OFFSET_INPUT, -5.f, 5.f);
			bias = pv(BIAS_PARAM, BIAS_INPUT, -1.f, 1.f);
			// ANIMATION from the knob plus 1 V per mode of CV, so a sequencer can
			// step the field between Flow and Life.
			m.anim = clamp((int)std::round(params[ANIM_PARAM].getValue()
			                               + (inputs[ANIM_INPUT].isConnected() ? inputs[ANIM_INPUT].getVoltage() : 0.f)),
			               0, ANIM_COUNT - 1);
			float coh = pv(COHER_PARAM, COHER_INPUT, 0.f, 1.f);
			// COHERENCE range measured, see the harness note: 0.6 (neighbours
			// independent) to 0.05 (the far side agrees) on a log scale.
			m.k = 0.6f * std::pow(0.05f / 0.6f, coh);
			float rate = fieldRateHz(pv(RATE_PARAM, RATE_INPUT, 0.f, 1.f));
			m.t += (double)(rate * dt);
			float flow = pv(FLOW_PARAM, FLOW_INPUT, 0.f, 1.f) * 4.f;       // jacks per second
			float dir  = pv(DIR_PARAM, DIR_INPUT, 0.f, 1.f) * 2.f * (float)M_PI;
			switch (m.anim) {
				case ANIM_FLOW:
					m.ox -= (double)(flow * std::cos(dir) * dt);
					m.oy -= (double)(flow * std::sin(dir) * dt);
					break;
				case ANIM_ZOOM:
					// FLOW 4 = one doubling per second; DIR 0 radiates out.
					m.phase += flow * 0.25f * std::cos(dir) * dt;
					m.phase -= std::floor(m.phase);
					break;
				case ANIM_SPIN:
					// the outer ring (3.46 jacks out) moves at FLOW jacks/s
					m.theta += flow / 3.4641f * std::cos(dir) * dt;
					if (m.theta > 2.f * (float)M_PI) m.theta -= 2.f * (float)M_PI;
					if (m.theta < 0.f) m.theta += 2.f * (float)M_PI;
					break;
				case ANIM_RANDOM:
					// each jack drifts at FLOW in a direction that wanders on its own
					for (int i = 0; i < FD_N; i++) {
						float d = (float)M_PI * 2.f * FieldNoise::at(i * 3.17f + 0.5f, 7.3f, (float)m.t * 0.35f + i);
						rox[i] += flow * std::cos(d) * dt; roy[i] += flow * std::sin(d) * dt;
					}
					break;
				case ANIM_LIFE:
					lifeAcc += rate * dt;
					while (lifeAcc >= 1.f) { lifeStep(0.15f + 0.4f * coh); lifeAcc -= 1.f; }
					break;
				case ANIM_REACT: {
					if (lastAnim != m.anim) { reactSeed(); simAcc = 0.f; }
					// FEED/KILL ALONG THE DIAGONAL THAT MAKES PATTERNS. Measured on
					// this cell graph with this diffusion (a table of coverage and
					// blob count over 4000 steps): a straight line through F/K space
					// saturated the plate for most of the knob, one blob of v
					// everywhere. These five points sit just on the pattern side of
					// the saturation ridge all the way along -- spots at the bottom,
					// dividing spots, then mazes -- with 20-40% coverage throughout.
					static const float PF[5] = {0.014f, 0.022f, 0.030f, 0.042f, 0.060f};
					static const float PK[5] = {0.045f, 0.055f, 0.062f, 0.065f, 0.068f};
					float u4 = coh * 4.f; int i0 = std::min((int)u4, 3); float fr = u4 - i0;
					float F = PF[i0] + (PF[i0 + 1] - PF[i0]) * fr, K = PK[i0] + (PK[i0 + 1] - PK[i0]) * fr;
					dropAcc += (0.02f + flow * 0.3f) * dt;
					while (dropAcc >= 1.f) {                       // a fresh drop of v
						int c = (int)(rnd() * rdV.size()) % (int)rdV.size(); rdV[c] = 1.f; dropAcc -= 1.f;
					}
					simAcc += rate * 200.f * dt;
					int steps = (int)simAcc; simAcc -= steps; if (steps > 8) steps = 8;
					for (int q = 0; q < steps; q++) reactStep(F, K);
					for (size_t c = 0; c < vis.size(); c++) vis[c] = clamp(rdV[c] * 5.f - 1.f, -1.f, 1.f);
					break;
				}
				case ANIM_CYCLIC: {
					// N from 4 to 8: measured, N=5..8 organise from noise into wave
					// trains within fifty steps (neighbour phase coherence 0.4-0.7 and
					// holding), while N=10 collapses to every cell in lockstep.
					int N = 4 + (int)std::round(coh * 4.f);
					if (lastAnim != m.anim) { for (size_t c = 0; c < cyc.size(); c++) cyc[c] = (uint8_t)((int)(rnd() * N) % N); simAcc = 0.f; }
					simAcc += rate * 8.f * dt;                     // eight steps per Hz: a spiral turn is N steps
					while (simAcc >= 1.f) { cyclicStep(N); simAcc -= 1.f; }
					for (size_t c = 0; c < vis.size(); c++) vis[c] = (float)(cyc[c] % N) / (float)(N - 1) * 2.f - 1.f;
					break;
				}
				case ANIM_SAND: {
					if (lastAnim != m.anim) { for (size_t c = 0; c < sand.size(); c++) { sand[c] = 0; vis[c] = -1.f; } simAcc = grainAcc = 0.f; }
					grainAcc += (2.f + flow * 30.f) * dt;           // grains per second
					while (grainAcc >= 1.f) {
						int c = (int)(rnd() * sand.size()) % (int)sand.size();
						if (sand[c] < 250) sand[c]++; grainAcc -= 1.f;
					}
					simAcc += rate * 200.f * dt;
					int steps = (int)simAcc; simAcc -= steps; if (steps > 8) steps = 8;
					bool any = false;
					for (int q = 0; q < steps; q++) sandStep(any);
					if (steps == 0)                                // between steps, show the pile
						for (size_t c = 0; c < vis.size(); c++) if (sand[c] < 6) vis[c] = (float)sand[c] / 5.f * 1.6f - 1.f;
					break;
				}
				case ANIM_WORLEY: {
					int nS = 3 + (int)std::round((1.f - coh) * 13.f);
					if (lastAnim != m.anim || nS != seedN) { seedN = nS; for (int i = 0; i < WMAX; i++) seedReset(i); }
					// each seed drifts at FLOW along DIR plus its own wander, which
					// turns at RATE; a seed that leaves the plate is reborn inside it
					for (int i = 0; i < seedN; i++) {
						sw[i] += (float)(FieldNoise::at(i * 2.71f + 0.3f, 4.2f, (float)m.t + i) * 6.f) * rate * dt * 4.f;
						float a = dir + sw[i] * 0.6f;
						sx[i] += (flow * std::cos(a) + rate * 0.3f * std::cos(sw[i])) * dt;
						sy[i] += (flow * std::sin(a) + rate * 0.3f * std::sin(sw[i])) * dt;
						if (sx[i] * sx[i] + sy[i] * sy[i] > 4.2f * 4.2f) {
							float ang = std::atan2(sy[i], sx[i]) + (float)M_PI;      // re-enter opposite
							sx[i] = 4.1f * std::cos(ang); sy[i] = 4.1f * std::sin(ang);
						}
					}
					const std::vector<HexCell>& cs = fieldCells();
					for (size_t c = 0; c < cs.size(); c++) vis[c] = worleyAt(cs[c].gx, cs[c].gy);
					break;
				}
				case ANIM_RAIN:
				case ANIM_WAVE: {
					if (lastAnim != m.anim) {                      // a still surface to start on
						for (size_t c = 0; c < wu.size(); c++) wu[c] = wuPrev[c] = vis[c] = 0.f;
						waveAcc = dropAcc = 0.f;
					}
					// COHERENCE, inverted, is the damping: at the top a wave
					// crosses the plate and comes back; at the bottom it dies
					// within a few cells and each drop is its own event.
					float damp = 0.003f + 0.06f * (1.f - coh);
					if (m.anim == ANIM_RAIN) dropAcc += (0.1f + flow) * dt;         // drops per second
					else                     dropAcc += (0.05f + flow * 0.5f) * dt; // crests per second
					while (dropAcc >= 1.f) { if (m.anim == ANIM_RAIN) rainDrop(); else waveCrest(dir); dropAcc -= 1.f; }
					// RATE: 200 solver steps per Hz, so the default 0.13 Hz sends a
					// front across the plate in about two seconds. Capped per tick
					// so the top of the knob costs bounded CPU rather than 8000
					// steps a second.
					waveAcc += rate * 200.f * dt;
					int steps = (int)waveAcc; waveAcc -= steps;
					if (steps > 6) { steps = 6; waveAcc = 0.f; }
					for (int q = 0; q < steps; q++) waveStep(damp);
					for (size_t c = 0; c < wu.size(); c++) vis[c] = clamp(wu[c], -1.f, 1.f);
					break;
				}
			}
			lastAnim = m.anim;

			held = inputs[TRIG_INPUT].isConnected();
			const int* jc = fieldJackCell();
			const std::vector<HexCell>& cells = fieldCells();
			for (int i = 0; i < FD_N; i++) {
				float n;
				if (fieldIsSim(m.anim)) {
					n = clamp(vis[jc[i]], -1.f, 1.f);
				} else if (m.anim == ANIM_LIFE) {
					// the cell's own state, and how alive its neighbourhood is
					int c = jc[i], cnt = 0, nn = 0;
					for (int q = 0; q < 6; q++) if (cells[c].nb[q] >= 0) { nn++; cnt += life[cells[c].nb[q]]; }
					n = 0.6f * (life[c] ? 1.f : -1.f) + 0.4f * ((float)cnt / std::max(nn, 1) * 2.f - 1.f);
				} else if (m.anim == ANIM_RANDOM) {
					n = FieldNoise::sample((FD_GX[i] + rox[i]) * m.k, (FD_GY[i] + roy[i]) * m.k, (float)m.t);
				} else n = m.at(FD_GX[i], FD_GY[i]);
				float g = bias >= 0.f ? (1.f - bias * (1.f - FD_R[i])) : (1.f + bias * FD_R[i]);
				float v = offV + ampV * g * n;
				if (!held) target[i] = v; else latchCandidate[i] = v;
			}
		}
		if (held) {
			int tch = inputs[TRIG_INPUT].getChannels();
			for (int i = 0; i < FD_N; i++) {
				float tv = inputs[TRIG_INPUT].getVoltage(std::min(i, std::max(tch, 1) - 1));
				if (trig[i].process(tv, 0.1f, 1.f)) target[i] = latchCandidate[i];
			}
		}
		float sm = 0.001f + pv(SMOOTH_PARAM, SMOOTH_INPUT, 0.f, 1.f) * 2.f;
		float a = 1.f - std::exp(-args.sampleTime / sm);
		outputs[POLY_OUTPUT].setChannels(16);
		for (int i = 0; i < FD_N; i++) {
			out[i] += (target[i] - out[i]) * a;
			float v = clamp(out[i], -10.f, 10.f);
			outputs[OUT_OUTPUT + i].setVoltage(v);
			if (i < 16) outputs[POLY_OUTPUT].setVoltage(v, i);
		}
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "palette", json_integer(palette));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "palette")) palette = clamp((int)json_integer_value(j), 0, FIELD_NPAL - 1);
	}
};

// ── the field, drawn under the jacks ─────────────────────────────────────────
// No screen. The hexagon of jacks IS the display: the plate behind them is
// tiled with the cells, each coloured by the field at its centre, from the
// same functions the outputs read, so what you see under a jack is what comes
// out of it. Drawn in the PANEL layer, before the jacks, so they paint over
// it -- a light-layer draw would come back on top of them.
struct FieldDisplay : Widget {
	Field* module = nullptr;

	static void hexPath(NVGcontext* vg, float cx, float cy, float r, float rot) {
		nvgBeginPath(vg);
		for (int i = 0; i < 6; i++) {
			float th = rot + (float)M_PI / 3.f * i;
			float x = cx + r * std::cos(th), y = cy + r * std::sin(th);
			if (i == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
		}
		nvgClosePath(vg);
	}
	static int clipToFlatHex(const float* px, const float* py, int n, float R,
	                         float* ox, float* oy) {
		float ax[16], ay[16], bx[16], by[16];
		int na = n; for (int i = 0; i < n; i++) { ax[i] = px[i]; ay[i] = py[i]; }
		const float in = 0.866f * R;
		for (int e = 0; e < 6; e++) {
			// edge normal at 30 + 60e degrees; inside is n.p <= inradius
			float th = (float)M_PI / 6.f + (float)M_PI / 3.f * e;
			float nx = std::cos(th), ny = std::sin(th);
			int nb = 0;
			for (int i = 0; i < na && nb < 15; i++) {
				int j = (i + 1) % na;
				float da = ax[i] * nx + ay[i] * ny - in, db = ax[j] * nx + ay[j] * ny - in;
				if (da <= 0.f) { bx[nb] = ax[i]; by[nb] = ay[i]; nb++; }
				if ((da <= 0.f) != (db <= 0.f)) {
					float t = da / (da - db);
					bx[nb] = ax[i] + (ax[j] - ax[i]) * t; by[nb] = ay[i] + (ay[j] - ay[i]) * t; nb++;
				}
			}
			na = nb; for (int i = 0; i < na; i++) { ax[i] = bx[i]; ay[i] = by[i]; }
			if (na == 0) return 0;
		}
		for (int i = 0; i < na; i++) { ox[i] = ax[i]; oy[i] = ay[i]; }
		return na;
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float cx = mm2px(FD_CX), cy = mm2px(FD_CY), R = mm2px(FD_PLATE_R);
		hexPath(vg, cx, cy, R, 0.f);                  // flat-top
		nvgFillColor(vg, sfs::SCREEN_BG);
		nvgFill(vg);

		FieldMotion pv; pv.t = 3.7;                   // the browser's stand-in
		const FieldMotion& mo = module ? module->m : pv;
		float amp = module ? module->ampV : 5.f, bias = module ? module->bias : 0.f;
		int pal = module ? module->palette : 0;
		const std::vector<HexCell>& cells = fieldCells();
		float sc = mm2px(FD_PITCH) / FD_SUB;
		for (size_t c = 0; c < cells.size(); c++) {
			float gx = cells[c].gx, gy = cells[c].gy;
			float r = std::min(1.f, std::sqrt(gx * gx + gy * gy) / 3.f);
			float g = bias >= 0.f ? (1.f - bias * (1.f - r)) : (1.f + bias * r);
			float v;
			if (module && mo.anim == ANIM_LIFE) v = module->vis[c] * 2.f - 1.f;
			else if (module && fieldIsSim(mo.anim)) v = module->vis[c];
			else v = clamp(mo.at(gx, gy) * g * amp / 5.f, -1.f, 1.f);
			float dx = gx * mm2px(FD_PITCH), dy = gy * mm2px(FD_PITCH);
			float hx[6], hy[6], qx[16], qy[16];
			for (int q = 0; q < 6; q++) {
				float th = (float)M_PI / 6.f + (float)M_PI / 3.f * q;
				hx[q] = dx + sc * 0.94f * std::cos(th); hy[q] = dy + sc * 0.94f * std::sin(th);
			}
			int nq = clipToFlatHex(hx, hy, 6, R, qx, qy);
			if (nq < 3) continue;
			nvgBeginPath(vg);
			for (int q = 0; q < nq; q++) {
				if (q == 0) nvgMoveTo(vg, cx + qx[q], cy + qy[q]); else nvgLineTo(vg, cx + qx[q], cy + qy[q]);
			}
			nvgClosePath(vg);
			nvgFillColor(vg, fieldColour(pal, v));
			nvgFill(vg);
		}
		Widget::draw(args);
	}
};

// ── the panel: 16HP ──────────────────────────────────────────────────────────
// EVERY NUMBER HERE IS READ OUT OF res/field.svg, the designer's export: the
// guide circles are the control centres. The trim row and the CV row are on
// the same 11.43mm pitch but 0.17mm apart in x, so each row keeps its own.
static const float FD_TX[7] = {6.56f, 17.99f, 29.42f, 40.84f, 52.27f, 63.70f, 75.13f};
static const float FD_CVX[7] = {6.39f, 17.82f, 29.25f, 40.67f, 52.10f, 63.53f, 74.96f};
static const float FD_Y_TRIM = 89.26f, FD_Y_CV = 100.95f, FD_Y_FOOT = 121.43f;
// the foot row: TRIG, SLEW + CV, ANIM + CV, POLY
static const float FD_FX[6] = {10.20f, 23.66f, 34.07f, 46.52f, 56.93f, 74.28f};
static const float FD_Y_POLY = 121.01f;      // on its plate, and not quite on the row

struct FieldWidget : ModuleWidget {
	FieldWidget(Field* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/field.svg")));

		// NO sfs::PanelLabels: res/field.svg is the designer's export and carries
		// its own labels as outlined paths, which Rack renders; the runtime
		// labels would print every one of them twice. Place components only.

		// The heat map goes in FIRST so the jacks draw over it.
		FieldDisplay* disp = new FieldDisplay();
		disp->module = module;
		disp->box.pos  = mm2px(Vec(0.f, 0.f));
		disp->box.size = box.size;
		addChild(disp);

		for (int i = 0; i < FD_N; i++)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(FD_JX[i], FD_JY[i])), module, Field::OUT_OUTPUT + i));

		const char* nm[7] = {"AMP", "OFFSET", "COHER", "RATE", "FLOW", "DIR", "BIAS"};
		const int pp[7] = {Field::AMP_PARAM, Field::OFFSET_PARAM, Field::COHER_PARAM, Field::RATE_PARAM,
		                   Field::FLOW_PARAM, Field::DIR_PARAM, Field::BIAS_PARAM};
		const int ii[7] = {Field::AMP_INPUT, Field::OFFSET_INPUT, Field::COHER_INPUT, Field::RATE_INPUT,
		                   Field::FLOW_INPUT, Field::DIR_INPUT, Field::BIAS_INPUT};
		for (int i = 0; i < 7; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(FD_TX[i], FD_Y_TRIM)), module, pp[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FD_CVX[i], FD_Y_CV)), module, ii[i]));
		}
		(void)nm;
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FD_FX[0], FD_Y_FOOT)), module, Field::TRIG_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(FD_FX[1], FD_Y_FOOT)), module, Field::SMOOTH_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FD_FX[2], FD_Y_FOOT)), module, Field::SMOOTH_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(FD_FX[3], FD_Y_FOOT)), module, Field::ANIM_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FD_FX[4], FD_Y_FOOT)), module, Field::ANIM_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(FD_FX[5], FD_Y_POLY)), module, Field::POLY_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Field* m = dynamic_cast<Field*>(this->module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		std::vector<std::string> pn;
		for (int i = 0; i < FIELD_NPAL; i++) pn.push_back(FIELD_PALETTES[i].name);
		menu->addChild(createIndexPtrSubmenuItem("Colour", pn, &m->palette));
	}
};

Model* modelField = createModel<Field, FieldWidget>("Field");

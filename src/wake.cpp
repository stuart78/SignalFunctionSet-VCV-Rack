// =============================================================================
// Wake: up to four cycles driving on a grid, each leaving a trail behind it.
//
// Each cycle moves one cell per step and never turns back on itself. A cycle
// that runs into an edge, a trail (its own or another's) or another cycle
// crashes: its CRASH out fires, its trail is removed, and after the restart
// delay it starts again from a free cell somewhere else.
//
// Pitch: every heading has an interval (by default N +12, S -12, E +7, W -7
// semitones). Each time a cycle turns, the interval of its new heading is added
// to its running total, and the V/OCT out is that total folded into RANGE
// octaves above 0 V. So a cycle walking east turn after turn walks the circle
// of fifths, and north and south move it between octaves. The menu can instead
// add the interval at every move. A new life starts at 0 V.
//
// Score: a point for every cell a cycle moves, and 50 for every other cycle
// that crashes into its trail. SCORE is 0.01 V per point.
//
// Each cycle has a STYLE: how it chooses where to go. Seven are automatic; the
// eighth, Keys, is steered from the keyboard (click the screen first).
//
// Design: docs/wake-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "root-cv.hpp"
#include "scale-bus.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

static const int WK_N = 4;                       // cycles
static const int WK_MAXW = 96, WK_MAXH = 44;
static const int WK_MAXC = WK_MAXW * WK_MAXH;    // cells
static const int WK_NSIZE = 4;
static const int WK_COLS[WK_NSIZE] = {32, 48, 64, 96};
static const int WK_KILL = 50;                   // points for a crash into your trail
static const int WK_ZIG = 3;                     // cells between a Zigzag cycle's turns

// The screen, in mm. The grid has the arena's proportions, so its row count
// follows from the column count.
static const float WK_SW = 132.08f, WK_SH = 60.96f;
static const float WK_STRIP = 6.f;               // the status line across the top
static const float WK_AW = 129.54f, WK_AH = 53.3f;
static const float WK_WALLH = 0.9f;              // 3D: wall height, in cells
static const float WK_ELEV0 = 1.05f;             // 3D: the camera's elevation, radians (60 degrees)
static const float WK_ELEV_MIN = 0.35f, WK_ELEV_MAX = 1.5707f;
static int wkRows(int cols) { return std::min(WK_MAXH, (int)(WK_AH * cols / WK_AW)); }

// The grid's shape (menu). Every shape keeps neighbouring cell centres 1 apart,
// so the drawing works in one unit throughout.
//   Square: four headings, turns of 90 degrees.
//   Hex: hexagonal cells in rows, odd rows shifted half a cell right; six
//     headings, turns of 60 degrees.
//   Triangle: triangular cells, alternately pointing down and up along a row
//     (down where x + y is even). A triangle has three neighbours, so a cycle
//     can never go straight on: every move is a left or a right turn, and
//     alternating them is what keeps a line going in one direction.
enum WkShape { WK_SQUARE, WK_HEX, WK_TRI, WK_NSHAPE };
static const float WK_R3 = 0.8660254f;           // sqrt(3) / 2
static const float WK_HEXR = 0.5773503f;         // a hexagon's corner radius, 1 / sqrt(3)
// Rows for a column count, so the grid has the arena's proportions. Hex and
// Triangle need an even number of rows (and Triangle even columns, which every
// size has) so that a cycle crossing an open edge lands on a matching cell.
static int wkRowsFor(int shape, int cols) {
	float a = WK_AH / WK_AW;
	switch (shape) {
		case WK_HEX: return std::min(WK_MAXH, 2 * (int)std::round(0.5f * a * (cols + 0.5f) / WK_R3));
		case WK_TRI: return std::min(WK_MAXH, 2 * (int)std::round(0.5f * a * (cols + 1) * WK_R3 / 1.5f));
		default: return wkRows(cols);
	}
}

// Headings, numbered clockwise, so a right turn is +1. y grows downwards.
// Square: N E S W. Hex: NE E SE SW W NW. Triangle: N, then every 60 degrees.
enum { WK_DN, WK_DE, WK_DS, WK_DW };
static const int WK_DX[4] = {0, 1, 0, -1};
static const int WK_DY[4] = {-1, 0, 1, 0};
static inline int wkLeft(int d) { return (d + 3) & 3; }
static inline int wkRight(int d) { return (d + 1) & 3; }
static inline int wkBack(int d) { return (d + 2) & 3; }
static const float WK_HEX_DX[6] = {0.5f, 1.f, 0.5f, -0.5f, -1.f, -0.5f};
static const float WK_HEX_DY[6] = {-WK_R3, 0.f, WK_R3, WK_R3, 0.f, -WK_R3};
static const float WK_TRI_DX[6] = {0.f, WK_R3, WK_R3, 0.f, -WK_R3, -WK_R3};
static const float WK_TRI_DY[6] = {-1.f, -0.5f, 0.5f, 1.f, 0.5f, -0.5f};

struct WkIntervals { const char* name; int iv[4]; };   // N E S W, semitones
static const WkIntervals WK_IV[] = {
	{"Octave and fifth (N +12, S -12, E +7, W -7)", {12, 7, -12, -7}},
	{"Fifth and fourth (N +7, S -7, E +5, W -5)", {7, 5, -7, -5}},
	{"Fifth and major third (N +7, S -7, E +4, W -4)", {7, 4, -7, -4}},
	{"Fifth and minor third (N +7, S -7, E +3, W -3)", {7, 3, -7, -3}},
	{"Fourth and whole tone (N +5, S -5, E +2, W -2)", {5, 2, -5, -2}},
	{"Whole tone and semitone (N +2, S -2, E +1, W -1)", {2, 1, -2, -1}},
};
static const int WK_NIV = sizeof(WK_IV) / sizeof(WK_IV[0]);

enum WkStyle { WK_STRAIGHT, WK_CAREFUL, WK_HUNTER, WK_RANDOM, WK_HUGGER, WK_ZIGZAG, WK_SPIRAL, WK_KEYS, WK_NSTYLE };
static const char* WK_STYLE_NAME[WK_NSTYLE] = {
	"Straight", "Careful", "Hunter", "Random", "Hugger", "Zigzag", "Spiral", "Keys"};
static const char* WK_STYLE_SHORT[WK_NSTYLE] = {
	"STRT", "CARE", "HUNT", "RAND", "HUG", "ZIG", "SPRL", "KEYS"};
static const char* WK_STYLE_DESC[WK_NSTYLE] = {
	"goes straight, and turns only when the cell ahead is blocked",
	"takes the move with the most free cells reachable beyond it",
	"heads for the cell in front of the nearest other cycle",
	"turns at random, about one move in five, and looks one cell ahead",
	"keeps beside walls and trails, filling the space it is in",
	"turns every 3 cells, left and right in turn, so it moves diagonally",
	"turns the same way after runs of 1, 1, 2, 2, 3, 3... cells",
	"steered from the keyboard: Z turns left and X right, or , and . for a second Keys cycle",
};

// How the pitch is made (NOTES). The first two count semitones and snap the
// total to the scale; the rest choose a degree of the scale.
enum WkNotes { WK_NT_TURNS, WK_NT_CELLS, WK_NT_STEPS, WK_NT_COLUMN, WK_NT_ROW, WK_NT_ROOM, WK_NT_CHASE, WK_NNOTES };
static const char* WK_NOTES_NAME[WK_NNOTES] = {
	"Turns: each turn adds the new heading's interval",
	"Every cell: each move adds the heading's interval",
	"Steps: a right turn goes up one degree of the scale, a left turn down one",
	"Column: the column sets the note, low at the left, high at the right",
	"Row: the row sets the note, low at the bottom, high at the top",
	"Room: the free cells ahead set the note, high in open space, low when enclosed",
	"Chase: the distance to the nearest other cycle sets the note, high when close",
};

// The plugin's four channel colours, as Trace's lanes and Canon's channels.
static const NVGcolor WK_COL[WK_N] = {
	nvgRGB(0x00, 0x97, 0xDE),   // A blue
	nvgRGB(0x3F, 0xBF, 0x6F),   // B green
	nvgRGB(0xEC, 0x65, 0x2E),   // C orange
	nvgRGB(0x9B, 0x6B, 0xD6),   // D purple
};

// A gate that goes low for 1 ms before a retrigger, so a voice sees a new note.
struct WkGate {
	int high = 0, gap = 0, pend = 0;
	void fire(int len, int gapLen) {
		if (high > 0 || gap > 0) { gap = gapLen; pend = len; high = 0; }
		else high = len;
	}
	float out() {
		if (gap > 0) { if (--gap == 0) high = pend; return 0.f; }
		if (high > 0) { high--; return 10.f; }
		return 0.f;
	}
};

struct Wake : Module {
	enum ParamId {
		RATE_PARAM, TRAIL_PARAM, RANGE_PARAM, RESET_PARAM,
		ENUMS(ON_PARAM, WK_N), ENUMS(STYLE_PARAM, WK_N),
		ROOT_PARAM, SCALE_PARAM, NOTES_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT, RESET_INPUT, RATE_INPUT, TRAIL_INPUT, RANGE_INPUT,
		ENUMS(STYLE_INPUT, WK_N), ENUMS(TURN_INPUT, WK_N),
		ROOT_INPUT, SCALE_INPUT, NOTES_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(VOCT_OUTPUT, WK_N), ENUMS(GATE_OUTPUT, WK_N),
		ENUMS(CRASH_OUTPUT, WK_N), ENUMS(SCORE_OUTPUT, WK_N),
		OUTPUTS_LEN
	};
	enum LightId { ENUMS(ON_LIGHT, WK_N), LIGHTS_LEN };

	struct Cyc {
		bool on = true, alive = false;
		int x = 0, y = 0, dir = WK_DE;
		int trail[WK_MAXC] = {};          // cells, oldest first from tStart (a ring)
		int tStart = 0, tLen = 0;
		int acc = 0;                   // semitones since this life started (Turns, Every cell)
		int deg = 0;                   // the scale degree (Steps, and the position modes)
		int roomFor[6] = {};           // free cells beyond each move, from the last choice
		int lastSide = 0;              // the last move: +1 right, -1 left, 0 straight on
		int pendTurn = 0;              // Triangle: a turn that needs a second move the same way
		int roomNow = 0;
		double moveT = -10.0;          // when it last moved, and how long a move takes,
		float moveDur = 0.125f;        // so the screen can draw it between cells
		int wait = 0;                  // steps until it starts again
		long score = 0;
		int life = 0;                  // cells moved this life
		int div = 1;                   // moves on every div-th step
		bool forceTurn = false;
		int run = 0;                   // cells since the last turn
		int leg = 1, legTurns = 0;     // Spiral
		int zigSide = 0;               // Zigzag
		int nd = 0, ncell = -1;        // this step's decision
		bool crash = false; int killer = -1;
		// keys, written by the UI thread, read by the audio thread
		std::atomic<unsigned> kw{0}, kr{0};
		int kq[8] = {};
		// the screen
		int ghost[WK_MAXC] = {};
		int ghostLen = 0, crashCell = -1;
		double ghostT = -10.0;
		// outputs
		WkGate gate, crashGate;
		dsp::SchmittTrigger turnTrig;
		long kills = 0, crashes = 0;
	};
	Cyc cyc[WK_N];
	uint8_t owner[WK_MAXC] = {};       // 0 empty, c + 1 = cycle c's trail
	int W = 64, H = 28;
	int sizeIdx = 2;
	int shape = WK_SQUARE;
	std::atomic<int> sizeReq{-1}, shapeReq{-1};
	std::atomic<bool> clearReq{false};

	// settings (menu)
	bool wrapEdges = false;
	int respawnDelay = 2;              // steps
	int gateMode = 1;                  // 0 trigger, 1 half a step
	int ivIdx = 0;
	int view = 1;                      // the screen: 0 flat, 1 3D
	float camYaw = 0.f, camElev = WK_ELEV0;   // the 3D camera, dragged on the screen
	int rootCvMode = sfs::ROOT_CV_VOCT;
	bool scoreWrap = true;

	// timing
	double tSec = 0.0;
	float sr = 48000.f;
	float phase = 0.f;
	float period = 0.125f;             // seconds per step
	float sinceEdge = 0.f;
	bool periodValid = false;
	long tickN = 0;
	dsp::SchmittTrigger clockTrig, resetTrig, resetBtnTrig;
	int curStyle[WK_N] = {};
	int curRange = 2;
	int curNotes = WK_NT_TURNS;
	int tmaxNow = 0;                   // the trail limit at the last step, for the screen
	int root = 0;
	sfs::BusScale sc;
	float outV[WK_N] = {};

	// flood fill
	int ffQ[WK_MAXC];
	uint32_t ffMark[WK_MAXC] = {};
	uint32_t ffStamp = 0;

	struct TrailQ : ParamQuantity {
		std::string getDisplayValueString() override {
			Wake* m = dynamic_cast<Wake*>(module);
			int n = m ? m->trailCellsFor(getValue()) : 0;
			return n <= 0 ? "no limit" : string::f("%d cells", n);
		}
	};

	Wake() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, 0.f, 1.f, 4.f / 6.f, "Rate (not used while CLOCK is patched)", " steps/s", 64.f, 0.5f);
		configParam<TrailQ>(TRAIL_PARAM, 0.f, 1.f, 1.f, "Trail length");
		configParam(RANGE_PARAM, 1.f, 4.f, 2.f, "Pitch range", " oct");
		paramQuantities[RANGE_PARAM]->snapEnabled = true;
		configButton(RESET_PARAM, "Reset: clear the arena and the scores");
		static const int defStyle[WK_N] = {WK_CAREFUL, WK_HUNTER, WK_SPIRAL, WK_RANDOM};
		std::vector<std::string> names(WK_STYLE_NAME, WK_STYLE_NAME + WK_NSTYLE);
		for (int c = 0; c < WK_N; c++) {
			std::string n = std::string(1, (char)('A' + c));
			configSwitch(ON_PARAM + c, 0.f, 1.f, 1.f, "Cycle " + n, {"Off", "On"});
			configSwitch(STYLE_PARAM + c, 0.f, WK_NSTYLE - 1, defStyle[c], "Cycle " + n + " style", names);
			configInput(STYLE_INPUT + c, "Cycle " + n + " style CV (1 V per style)");
			configInput(TURN_INPUT + c, "Cycle " + n + " turn (a trigger turns it at its next move, toward the side with more room)");
			configOutput(VOCT_OUTPUT + c, "Cycle " + n + " pitch (1 V/oct)");
			configOutput(GATE_OUTPUT + c, "Cycle " + n + " gate (fires when its pitch is set: each turn, and each start)");
			configOutput(CRASH_OUTPUT + c, "Cycle " + n + " crash");
			configOutput(SCORE_OUTPUT + c, "Cycle " + n + " score (0.01 V per point)");
		}
		configInput(CLOCK_INPUT, "Clock (one step per pulse)");
		configInput(RESET_INPUT, "Reset: clear the arena and the scores");
		configInput(RATE_INPUT, "Rate CV (1 V doubles the rate)");
		configInput(TRAIL_INPUT, "Trail length CV (10 V is the whole range)");
		configInput(RANGE_INPUT, "Pitch range CV (1 V per octave)");
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
		std::vector<std::string> scales;
		for (int i = 0; i < sfs::NUM_SCALES; i++) scales.push_back(sfs::SCALES[i].longName);
		configSwitch(SCALE_PARAM, 0.f, (float)(sfs::NUM_SCALES - 1), 0.f, "Scale", scales);
		std::vector<std::string> notes(WK_NOTES_NAME, WK_NOTES_NAME + WK_NNOTES);
		configSwitch(NOTES_PARAM, 0.f, (float)(WK_NNOTES - 1), 0.f, "Notes", notes);
		configInput(ROOT_INPUT, sfs::rootCvName(sfs::ROOT_CV_VOCT));
		configInput(SCALE_INPUT, "Scale CV (1 V per scale, or Key's scale bus)");
		configInput(NOTES_INPUT, "Notes CV (1 V per mode)");
		sfs::busScaleFromIndex(0, sc);
		applySize(sizeIdx);
	}

	// ---------------------------------------------------------------- the grid
	int trailCellsFor(float v) const {     // 0 = no limit
		if (v >= 0.995f) return 0;
		return (int)std::round(4.f * std::pow(2.f, clamp(v, 0.f, 1.f) * 8.f));
	}
	int trailMax() {
		float v = params[TRAIL_PARAM].getValue() + inputs[TRAIL_INPUT].getVoltage() * 0.1f;
		return trailCellsFor(clamp(v, 0.f, 1.f));
	}
	void applySize(int i) {
		sizeIdx = clamp(i, 0, WK_NSIZE - 1);
		W = WK_COLS[sizeIdx];
		H = wkRowsFor(shape, W);
		clearArena(false);
	}
	void applyShape(int s) {
		shape = clamp(s, 0, WK_NSHAPE - 1);
		applySize(sizeIdx);
	}

	// ---------------------------------------------------------------- the shape
	int nHead() const { return shape == WK_SQUARE ? 4 : 6; }
	int left(int d) const { int n = nHead(); return (d + n - 1) % n; }
	int right(int d) const { return (d + 1) % nHead(); }
	int back(int d) const { int n = nHead(); return (d + n / 2) % n; }
	bool threeWay() const { return shape == WK_TRI; }
	// Triangle: a down-pointing cell (x + y even) has edges N, SE and SW, an
	// up-pointing one NE, S and NW, so a heading exists where its parity matches
	bool hasEdge(int x, int y, int d) const { return shape != WK_TRI || (d & 1) == ((x + y) & 1); }
	// Cell centres on the floor, in units of the distance between neighbours.
	float wxOf(int x, int y) const {
		switch (shape) {
			case WK_HEX: return x + 0.5f * (y & 1) + 0.5f;
			case WK_TRI: return (x + 1) * WK_R3;
			default: return x + 0.5f;
		}
	}
	float wyOf(int x, int y) const {
		switch (shape) {
			case WK_HEX: return y * WK_R3 + WK_HEXR;
			case WK_TRI: return 1.5f * y + (((x + y) & 1) ? 1.f : 0.5f);
			default: return y + 0.5f;
		}
	}
	float worldW() const { return shape == WK_HEX ? W + 0.5f : shape == WK_TRI ? (W + 1) * WK_R3 : (float)W; }
	float worldH() const { return shape == WK_HEX ? (H - 1) * WK_R3 + 2.f * WK_HEXR : shape == WK_TRI ? 1.5f * H : (float)H; }
	// how far the floor repeats across an open edge
	float periodW() const { return shape == WK_HEX ? (float)W : shape == WK_TRI ? W * WK_R3 : (float)W; }
	float periodH() const { return shape == WK_HEX ? H * WK_R3 : shape == WK_TRI ? 1.5f * H : (float)H; }
	void dirVec(int d, float& dx, float& dy) const {
		switch (shape) {
			case WK_HEX: dx = WK_HEX_DX[d % 6]; dy = WK_HEX_DY[d % 6]; break;
			case WK_TRI: dx = WK_TRI_DX[d % 6]; dy = WK_TRI_DY[d % 6]; break;
			default: dx = (float)WK_DX[d & 3]; dy = (float)WK_DY[d & 3];
		}
	}
	void dropTrail(int c, bool ghost) {
		Cyc& k = cyc[c];
		if (ghost) {
			for (int i = 0; i < k.tLen; i++) k.ghost[i] = k.trail[(k.tStart + i) % WK_MAXC];
			k.ghostLen = k.tLen;
			k.ghostT = tSec;
		}
		for (int i = 0; i < k.tLen; i++) {
			int cell = k.trail[(k.tStart + i) % WK_MAXC];
			if (owner[cell] == c + 1) owner[cell] = 0;
		}
		k.tStart = 0; k.tLen = 0;
	}
	void clearArena(bool scores) {
		std::memset(owner, 0, sizeof(owner));
		for (int c = 0; c < WK_N; c++) {
			Cyc& k = cyc[c];
			k.tStart = 0; k.tLen = 0;
			k.alive = false; k.wait = 0;
			k.ghostLen = 0;
			if (scores) { k.score = 0; k.kills = 0; k.crashes = 0; }
		}
		tickN = 0;
	}
	void pushCell(int c, int cell) {
		Cyc& k = cyc[c];
		k.trail[(k.tStart + k.tLen) % WK_MAXC] = cell;
		k.tLen++;
		owner[cell] = (uint8_t)(c + 1);
	}
	void popTail(int c) {
		Cyc& k = cyc[c];
		if (k.tLen <= 0) return;
		int cell = k.trail[k.tStart];
		if (owner[cell] == c + 1) owner[cell] = 0;
		k.tStart = (k.tStart + 1) % WK_MAXC;
		k.tLen--;
	}
	// the cell one step from (x, y) heading d; false off the edge, or where a
	// triangle has no edge that way
	bool stepRaw(int x, int y, int d, int& nx, int& ny) const {
		switch (shape) {
			case WK_HEX: {
				int odd = y & 1;
				switch (d) {
					case 0: nx = x + odd; ny = y - 1; break;
					case 1: nx = x + 1; ny = y; break;
					case 2: nx = x + odd; ny = y + 1; break;
					case 3: nx = x - 1 + odd; ny = y + 1; break;
					case 4: nx = x - 1; ny = y; break;
					default: nx = x - 1 + odd; ny = y - 1;
				}
				break;
			}
			case WK_TRI:
				if (!hasEdge(x, y, d)) return false;
				switch (d) {
					case 0: nx = x; ny = y - 1; break;
					case 3: nx = x; ny = y + 1; break;
					case 1: case 2: nx = x + 1; ny = y; break;
					default: nx = x - 1; ny = y;
				}
				break;
			default: nx = x + WK_DX[d]; ny = y + WK_DY[d];
		}
		return true;
	}
	bool stepFrom(int x, int y, int d, int& nx, int& ny) const {
		if (!stepRaw(x, y, d, nx, ny)) return false;
		if (wrapEdges) { nx = (nx + W) % W; ny = (ny + H) % H; return true; }
		return nx >= 0 && nx < W && ny >= 0 && ny < H;
	}
	bool freeCell(int x, int y) const {
		if (wrapEdges) { x = (x + W) % W; y = (y + H) % H; }
		else if (x < 0 || x >= W || y < 0 || y >= H) return false;
		return owner[y * W + x] == 0;
	}
	// How many free cells a cycle at (x, y) heading d has straight ahead (on
	// Triangle, turning right and left in turn), up to maxr.
	int runAhead(int x, int y, int d, int maxr) const {
		int r = 0, cx = x, cy = y, h = d;
		while (r < maxr) {
			int nx, ny;
			if (!stepFrom(cx, cy, h, nx, ny) || owner[ny * W + nx]) break;
			cx = nx; cy = ny; r++;
			if (threeWay()) h = (r & 1) ? right(h) : left(h);
		}
		return r;
	}
	// free cells reachable from (x, y), counting it
	int room(int x, int y) {
		if (!freeCell(x, y)) return 0;
		if (++ffStamp == 0) { std::memset(ffMark, 0, sizeof(ffMark)); ffStamp = 1; }
		int qh = 0, qt = 0;
		int s = y * W + x;
		ffQ[qt++] = s; ffMark[s] = ffStamp;
		while (qh < qt) {
			int cell = ffQ[qh++];
			int cx = cell % W, cy = cell / W;
			for (int d = 0, nd = nHead(); d < nd; d++) {
				int nx, ny;
				if (!stepFrom(cx, cy, d, nx, ny)) continue;
				int n = ny * W + nx;
				if (owner[n] || ffMark[n] == ffStamp) continue;
				ffMark[n] = ffStamp;
				ffQ[qt++] = n;
			}
		}
		return qt;
	}
	// How many more moves (up to depth) a cycle arriving at (x, y) heading d
	// can make, turning as it is allowed to. On Hex and Triangle a cycle cannot
	// turn sharply, so a cell with free cells round it can still be a dead end:
	// the last cell of a shifted Hex row, entered heading east, has nothing
	// ahead of it. The flood fill does not see that, so choose() checks this too.
	int movesAhead(int x, int y, int d, int depth) const {
		if (depth <= 0) return 0;
		int best = 0;
		int ds[3] = {d, left(d), right(d)};
		for (int i = 0; i < 3 && best < depth; i++) {
			int nx, ny;
			if (!stepFrom(x, y, ds[i], nx, ny) || owner[ny * W + nx]) continue;
			best = std::max(best, 1 + movesAhead(nx, ny, ds[i], depth - 1));
		}
		return best;
	}
	// Square: steps along the grid. Hex and Triangle: straight-line distance
	// between centres, where neighbours are 1 apart.
	float distTo(float ax, float ay, float bx, float by) const {
		float dx = std::fabs(bx - ax), dy = std::fabs(by - ay);
		if (wrapEdges) { dx = std::min(dx, periodW() - dx); dy = std::min(dy, periodH() - dy); }
		return shape == WK_SQUARE ? dx + dy : std::sqrt(dx * dx + dy * dy);
	}
	float dist(int x0, int y0, int x1, int y1) const {
		if (shape == WK_SQUARE) {
			int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
			if (wrapEdges) { dx = std::min(dx, W - dx); dy = std::min(dy, H - dy); }
			return (float)(dx + dy);
		}
		return distTo(wxOf(x0, y0), wyOf(x0, y0), wxOf(x1, y1), wyOf(x1, y1));
	}
	int blockedAround(int x, int y) const {
		int n = 0;
		for (int d = 0, nd = nHead(); d < nd; d++) {
			int nx, ny;
			if (!hasEdge(x, y, d)) continue;
			if (!stepFrom(x, y, d, nx, ny) || owner[ny * W + nx]) n++;
		}
		return n;
	}

	// ---------------------------------------------------------------- keys
	// A key turns a Keys cycle left (-1) or right (+1) at its next move; the
	// presses queue, one per move. Called from the UI thread.
	void pushKey(int c, int d) {
		Cyc& k = cyc[c];
		unsigned w = k.kw.load(), r = k.kr.load();
		if (w - r >= 8) return;
		k.kq[w & 7] = d;
		k.kw.store(w + 1);
	}
	int popKey(int c) {
		Cyc& k = cyc[c];
		unsigned r = k.kr.load();
		if (r == k.kw.load()) return 0;                  // nothing pressed
		int d = k.kq[r & 7];
		k.kr.store(r + 1);
		return d;
	}
	void flushKeys(int c) { cyc[c].kr.store(cyc[c].kw.load()); }
	// The cycle Z and X (set 0) or , and . (set 1) steer: the first and
	// second Keys cycles that are on, A to D.
	int keysCycle(int set) const {
		int n = 0;
		for (int c = 0; c < WK_N; c++)
			if (cyc[c].on && curStyle[c] == WK_KEYS) { if (n == set) return c; n++; }
		return -1;
	}

	// ---------------------------------------------------------------- choosing
	struct Cand { int d, x, y; bool open; int room, fill; bool risky; };

	int choose(int c) {
		Cyc& k = cyc[c];
		Cand cd[3];
		// straight on, left, right. A triangle has no straight on: there it is
		// the other way from the last turn, which keeps the line going
		int ds[3] = {k.dir, left(k.dir), right(k.dir)};
		if (threeWay()) ds[0] = k.lastSide > 0 ? ds[1] : ds[2];
		int best = 0;
		uint32_t stamp[3] = {};
		for (int i = 0; i < 3; i++) {
			Cand& a = cd[i];
			a.d = ds[i];
			a.open = stepFrom(k.x, k.y, a.d, a.x, a.y) && owner[a.y * W + a.x] == 0;
			a.room = 0; a.fill = 0;
			if (a.open) {
				// a move into an area an earlier move's fill already covered has
				// the same room: on an open board that is all three, and one fill does
				int cell = a.y * W + a.x;
				for (int j = 0; j < i; j++)
					if (stamp[j] && ffMark[cell] == stamp[j]) { a.fill = cd[j].fill; stamp[i] = stamp[j]; break; }
				if (!stamp[i]) { a.fill = room(a.x, a.y); stamp[i] = ffStamp; }
				a.room = a.fill;
				if (shape != WK_SQUARE) {
					int ma = movesAhead(a.x, a.y, a.d, 4);
					if (ma < 4) a.room = std::min(a.room, ma + 1);
				}
			}
			a.risky = false;
			if (a.open)
				for (int o = 0; o < WK_N; o++)
					if (o != c && cyc[o].alive && dist(a.x, a.y, cyc[o].x, cyc[o].y) <= 1.01f) a.risky = true;
			best = std::max(best, a.room);
		}
		for (int i = 0; i < 3; i++) k.roomFor[cd[i].d] = cd[i].room;
		k.roomFor[back(k.dir)] = 0;
		int st = curStyle[c];
		// the safest move: open, not next to another head, most room, straight on a tie
		auto careful = [&]() {
			int pick = 0; long bestKey = -1;
			for (int i = 0; i < 3; i++) {
				if (!cd[i].open) continue;
				long key = (long)(cd[i].risky ? 0 : 1) * 1000000L + (long)cd[i].room * 4 + (i == 0 ? 1 : 0);
				if (key > bestKey) { bestKey = key; pick = i; }
			}
			return cd[pick].d;
		};
		auto ok = [&](int i) { return cd[i].open && cd[i].room * 2 >= best; };
		auto towardRoom = [&]() {
			if (cd[1].open && (!cd[2].open || cd[1].room >= cd[2].room)) return 1;
			if (cd[2].open) return 2;
			return 1;
		};
		auto turnTowardRoom = [&]() { return cd[towardRoom()].d; };
		// On Triangle a move the other way from the last turn only keeps the line
		// going; the line turns when two moves turn the same way
		auto lineTurn = [&](int i) { return !threeWay() || k.lastSide == (i == 2 ? 1 : -1); };
		// a turn asked for (a key, TURN): on Triangle it may take this move and the next
		auto turnTo = [&](int i) {
			if (!lineTurn(i)) k.pendTurn = i == 2 ? 1 : -1;
			return cd[i].d;
		};

		if (k.pendTurn) {
			int i = k.pendTurn > 0 ? 2 : 1;
			k.pendTurn = 0;
			if (cd[i].open) return cd[i].d;
		}
		if (k.forceTurn) { k.forceTurn = false; return turnTo(towardRoom()); }

		switch (st) {
			case WK_KEYS: {
				// left or right of the heading; with no key pressed it goes straight
				int t = popKey(c);
				if (t == 0) return cd[0].d;
				return turnTo(t > 0 ? 2 : 1);
			}
			case WK_STRAIGHT:
				if (cd[0].open) return cd[0].d;
				return turnTowardRoom();
			case WK_CAREFUL:
				return careful();
			case WK_HUNTER: {
				int tgt = -1; float td = 1e30f;
				for (int o = 0; o < WK_N; o++) {
					if (o == c || !cyc[o].alive) continue;
					float dd = dist(k.x, k.y, cyc[o].x, cyc[o].y);
					if (dd < td) { td = dd; tgt = o; }
				}
				if (tgt < 0) return careful();
				const Cyc& o = cyc[tgt];
				// the point two cells ahead of it, kept inside the arena
				float tx, ty;
				if (shape == WK_SQUARE) {
					tx = clamp(o.x + 2 * WK_DX[o.dir], 0, W - 1) + 0.5f;
					ty = clamp(o.y + 2 * WK_DY[o.dir], 0, H - 1) + 0.5f;
				} else {
					float dx, dy;
					dirVec(o.dir, dx, dy);
					tx = clamp(wxOf(o.x, o.y) + 2.f * dx, 0.f, worldW());
					ty = clamp(wyOf(o.x, o.y) + 2.f * dy, 0.f, worldH());
				}
				int pick = -1; float pd = 1e30f;
				for (int i = 0; i < 3; i++) {
					if (!ok(i)) continue;
					float dd = distTo(wxOf(cd[i].x, cd[i].y), wyOf(cd[i].x, cd[i].y), tx, ty) * 2.f + (i == 0 ? 0.f : 1.f);
					if (dd < pd) { pd = dd; pick = i; }
				}
				return pick >= 0 ? cd[pick].d : careful();
			}
			case WK_RANDOM: {
				bool turn = !cd[0].open || random::uniform() < 0.2f;
				if (!turn) return cd[0].d;
				int opts[2], n = 0;
				if (cd[1].open) opts[n++] = 1;
				if (cd[2].open) opts[n++] = 2;
				if (n == 0) return cd[0].d;
				return cd[opts[n == 1 ? 0 : (random::uniform() < 0.5f ? 0 : 1)]].d;
			}
			case WK_HUGGER: {
				int pick = -1; int bestKey = -1;
				for (int i = 0; i < 3; i++) {
					if (!ok(i)) continue;
					int key = blockedAround(cd[i].x, cd[i].y) * 2 + (i == 0 ? 1 : 0);
					if (key > bestKey) { bestKey = key; pick = i; }
				}
				return pick >= 0 ? cd[pick].d : careful();
			}
			case WK_ZIGZAG: {
				if (k.run >= WK_ZIG) {
					int i = k.zigSide ? 2 : 1;
					if (ok(i)) { if (lineTurn(i)) k.zigSide ^= 1; return cd[i].d; }
					return careful();
				}
				if (ok(0)) return cd[0].d;
				return careful();
			}
			case WK_SPIRAL: {
				if (k.run >= k.leg) {
					if (ok(2)) {
						if (lineTurn(2) && ++k.legTurns % (nHead() == 4 ? 2 : 3) == 0) k.leg++;
						return cd[2].d;
					}
					k.leg = 1; k.legTurns = 0;
					return careful();
				}
				if (ok(0)) return cd[0].d;
				k.leg = 1; k.legTurns = 0;
				return careful();
			}
		}
		return careful();
	}

	// ---------------------------------------------------------------- a life
	void fire(WkGate& g, int c) {
		float stepSec = period * cyc[c].div;
		float len = gateMode == 0 ? 0.005f : std::max(0.001f, 0.5f * stepSec);
		g.fire(std::max(1, (int)(len * sr)), std::max(1, (int)(0.001f * sr)));
	}
	// Puts cycle c at (x, y) heading d, as a new life. Used by spawn() and the harness.
	void place(int c, int x, int y, int d) {
		Cyc& k = cyc[c];
		dropTrail(c, false);
		k.alive = true; k.x = x; k.y = y; k.dir = d;
		k.lastSide = 0; k.pendTurn = 0;
		// Triangle: the heading is the one it arrived on, so start it as though it
		// had just turned right, which makes straight on (a left) leave along d
		if (threeWay()) { k.dir = right(d); k.lastSide = 1; }
		pushCell(c, y * W + x);
		k.acc = 0; k.deg = 0; k.life = 0; k.run = 0; k.leg = 1; k.legTurns = 0; k.zigSide = 0;
		k.forceTurn = false;
		k.moveT = -10.0;
		int nx, ny;
		k.roomNow = stepFrom(x, y, d, nx, ny) ? room(nx, ny) : 0;
		flushKeys(c);
		setNote(c, 0, true);
	}

	// ---------------------------------------------------------------- notes
	// Degrees in RANGE octaves of the scale. A position mode can reach the top
	// one too, the root RANGE octaves up.
	int degSpan() const { return std::max(1, curRange * std::max(1, sc.size)); }
	float nearestDist(int c) const {
		float best = -1.f;
		for (int o = 0; o < WK_N; o++) {
			if (o == c || !cyc[o].on || !cyc[o].alive) continue;
			float dd = dist(cyc[c].x, cyc[c].y, cyc[o].x, cyc[o].y);
			if (best < 0 || dd < best) best = dd;
		}
		return best;
	}
	// The degree a position mode gives for where cycle c is now.
	int posDegree(int c, int mode) const {
		const Cyc& k = cyc[c];
		int n = degSpan() + 1;
		float f = 0.f;
		switch (mode) {
			case WK_NT_COLUMN: f = wxOf(k.x, k.y) / worldW(); break;
			case WK_NT_ROW: f = (worldH() - wyOf(k.x, k.y)) / worldH(); break;
			case WK_NT_ROOM: {
				// the free cells ahead against the whole arena: 0 to 1 as the room
				// runs from nothing to every cell
				f = (float)k.roomNow / (float)(W * H);
				break;
			}
			case WK_NT_CHASE: {
				float dd = nearestDist(c);
				float far;
				if (shape == WK_SQUARE) far = wrapEdges ? 0.5f * (W + H) : (float)(W + H - 2);
				else {
					float pw = wrapEdges ? 0.5f * periodW() : worldW(), ph = wrapEdges ? 0.5f * periodH() : worldH();
					far = std::sqrt(pw * pw + ph * ph);
				}
				f = dd < 0 ? 0.f : 1.f - clamp((dd - 1) / std::max(1.f, far - 1.f), 0.f, 1.f);
				break;
			}
		}
		return clamp((int)(f * n), 0, n - 1);
	}
	// The interval for a heading. A set names two, N and E on the square grid.
	// On Hex and Triangle, E (on Triangle, the heading 30 degrees above east)
	// takes the set's E interval, the heading 60 degrees anticlockwise from it
	// the N interval, the one 120 degrees anticlockwise N minus E, and the three
	// opposite headings the negatives. In Every cell, a cell's total is then the
	// same whichever path reached it, as on the square grid; on Hex with the
	// fifth and major third set, the grid is the Tonnetz.
	int interval(int d) const {
		const int* iv = WK_IV[clamp(ivIdx, 0, WK_NIV - 1)].iv;
		if (shape == WK_SQUARE) return iv[d & 3];
		int b = iv[0], a = iv[1];
		const int six[6] = {b, a, a - b, -b, -a, b - a};
		return six[d % 6];
	}
	// Sets cycle c's note after a move (side: +1 a right turn, -1 a left turn,
	// 0 straight on) or at the start of a life, and fires GATE when it is due.
	void setNote(int c, int side, bool start) {
		Cyc& k = cyc[c];
		bool g = start;
		switch (curNotes) {
			case WK_NT_TURNS:
				if (!start && side != 0) { k.acc += interval(k.dir); g = true; }
				break;
			case WK_NT_CELLS:
				if (!start) { k.acc += interval(k.dir); g = true; }
				break;
			case WK_NT_STEPS:
				if (!start && side != 0) { k.deg += side; g = true; }
				break;
			default: {
				int d = posDegree(c, curNotes);
				if (d != k.deg) g = true;
				k.deg = d;
			}
		}
		if (g) fire(k.gate, c);
	}
	// Semitones above the root for cycle c's note, on the scale and inside RANGE.
	float noteSemis(int c) const {
		const Cyc& k = cyc[c];
		float period = sc.period > 0.01f ? sc.period : 12.f;
		if (curNotes == WK_NT_TURNS || curNotes == WK_NT_CELLS) {
			float span = curRange * period;
			float s = sfs::busSnap((float)k.acc, sc);
			s -= span * std::floor(s / span + 1e-5f);
			return std::max(0.f, s);
		}
		int n = degSpan();
		int d = curNotes == WK_NT_STEPS ? ((k.deg % n) + n) % n : clamp(k.deg, 0, n);
		return sc.size > 0 ? sc.degree(d) : 0.f;
	}
	bool spawn(int c) {
		// a free cell with a clear run ahead and no other head close by
		for (int tries = 0; tries < 400; tries++) {
			int x = (int)(random::uniform() * W), y = (int)(random::uniform() * H);
			x = clamp(x, 0, W - 1); y = clamp(y, 0, H - 1);
			if (owner[y * W + x]) continue;
			bool near = false;
			for (int o = 0; o < WK_N; o++)
				if (o != c && cyc[o].alive && dist(x, y, cyc[o].x, cyc[o].y) < 4.f) near = true;
			if (near && tries < 300) continue;
			int bd = -1, br = -1, nh = nHead();
			int first = (int)(random::uniform() * nh) % nh;
			for (int j = 0; j < nh; j++) {
				int d = (first + j) % nh, r = runAhead(x, y, d, 8);
				if (r > br) { br = r; bd = d; }
			}
			if (br < std::min(4, tries / 60)) continue;
			place(c, x, y, bd);
			return true;
		}
		return false;
	}
	void crashCycle(int c) {
		Cyc& k = cyc[c];
		k.crashCell = k.y * W + k.x;
		dropTrail(c, true);
		k.alive = false;
		k.wait = respawnDelay;
		k.crashes++;
		fire(k.crashGate, c);
		if (k.killer >= 0 && k.killer != c) { cyc[k.killer].score += WK_KILL; cyc[k.killer].kills++; }
	}

	// One step of the arena: every cycle due to move decides on the board as it
	// is, the tails that are due go, then they all move at once.
	void doTick() {
		tickN++;
		int tmax = trailMax();
		tmaxNow = tmax;
		bool due[WK_N];
		for (int c = 0; c < WK_N; c++) {
			Cyc& k = cyc[c];
			due[c] = k.on && k.alive && (tickN % std::max(1, k.div) == 0);
			if (due[c]) k.nd = choose(c);
		}
		for (int c = 0; c < WK_N; c++)
			if (due[c] && tmax > 0) while (cyc[c].tLen >= tmax) popTail(c);
		for (int c = 0; c < WK_N; c++) {
			if (!due[c]) continue;
			Cyc& k = cyc[c];
			int nx, ny;
			k.killer = -1;
			if (!stepFrom(k.x, k.y, k.nd, nx, ny)) { k.crash = true; k.ncell = -1; continue; }
			k.ncell = ny * W + nx;
			k.crash = owner[k.ncell] != 0;
			if (k.crash) k.killer = owner[k.ncell] - 1;
		}
		// two cycles into one cell: both crash
		for (int a = 0; a < WK_N; a++)
			for (int b = a + 1; b < WK_N; b++)
				if (due[a] && due[b] && cyc[a].ncell >= 0 && cyc[a].ncell == cyc[b].ncell) {
					cyc[a].crash = cyc[b].crash = true;
					cyc[a].killer = cyc[b].killer = -1;
				}
		int side[WK_N] = {};
		bool moved[WK_N] = {};
		for (int c = 0; c < WK_N; c++) {
			if (!due[c]) continue;
			Cyc& k = cyc[c];
			if (k.crash) { crashCycle(c); continue; }
			bool turned = k.nd != k.dir;
			int sd = !turned ? 0 : k.nd == right(k.dir) ? 1 : -1;
			// a turn of the line: on Triangle, two moves turning the same way
			bool lt = threeWay() ? sd == k.lastSide : turned;
			side[c] = sd;
			k.lastSide = sd;
			k.run = lt ? 1 : k.run + 1;
			k.roomNow = k.roomFor[k.nd];
			k.dir = k.nd;
			k.x = k.ncell % W; k.y = k.ncell / W;
			pushCell(c, k.ncell);
			k.score++;
			k.life++;
			k.moveT = tSec;
			k.moveDur = period * std::max(1, k.div);
			moved[c] = true;
		}
		// after every move, so Chase measures where the others are now
		for (int c = 0; c < WK_N; c++) if (moved[c]) setNote(c, side[c], false);
		for (int c = 0; c < WK_N; c++) {
			Cyc& k = cyc[c];
			if (!k.on || k.alive) continue;
			if (due[c]) continue;               // crashed on this step: the wait starts on the next
			if (k.wait > 0) { k.wait--; continue; }
			if (!spawn(c)) k.wait = 1;
		}
	}

	float voct(int c) const { return (root + noteSemis(c)) / 12.f; }

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;
		sr = args.sampleRate;
		tSec += dt;

		int sh = shapeReq.exchange(-1);
		if (sh >= 0) applyShape(sh);
		int sz = sizeReq.exchange(-1);
		if (sz >= 0) applySize(sz);
		bool rIn = resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		bool rBtn = resetBtnTrig.process(params[RESET_PARAM].getValue() > 0.5f ? 10.f : 0.f, 0.1f, 1.f);
		if (rIn || rBtn || clearReq.exchange(false)) { clearArena(true); phase = 1.f; }

		curRange = clamp((int)std::round(params[RANGE_PARAM].getValue() + inputs[RANGE_INPUT].getVoltage()), 1, 4);
		curNotes = clamp((int)std::round(params[NOTES_PARAM].getValue() + inputs[NOTES_INPUT].getVoltage()), 0, WK_NNOTES - 1);
		int r = (int)std::round(params[ROOT_PARAM].getValue());
		if (inputs[ROOT_INPUT].isConnected()) r += (int)std::round(sfs::rootCvSemis(inputs[ROOT_INPUT].getVoltage(), rootCvMode));
		root = ((r % 12) + 12) % 12;
		sc = sfs::busResolve(inputs[SCALE_INPUT], (int)std::round(params[SCALE_PARAM].getValue()));
		for (int c = 0; c < WK_N; c++) {
			Cyc& k = cyc[c];
			curStyle[c] = clamp((int)std::round(params[STYLE_PARAM + c].getValue()
			                    + inputs[STYLE_INPUT + c].getVoltage()), 0, WK_NSTYLE - 1);
			bool on = params[ON_PARAM + c].getValue() > 0.5f;
			if (!on && k.on) { dropTrail(c, true); k.alive = false; }
			if (on && !k.on) k.wait = 0;
			k.on = on;
			if (k.turnTrig.process(inputs[TURN_INPUT + c].getVoltage(), 0.1f, 1.f)) k.forceTurn = true;
		}

		bool tick = false;
		if (inputs[CLOCK_INPUT].isConnected()) {
			sinceEdge += dt;
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (sinceEdge > 0.002f && sinceEdge < 10.f) { period = sinceEdge; periodValid = true; }
				sinceEdge = 0.f;
				tick = true;
			}
		} else {
			float rate = 0.5f * std::pow(64.f, params[RATE_PARAM].getValue()) * std::pow(2.f, inputs[RATE_INPUT].getVoltage());
			rate = clamp(rate, 0.05f, 200.f);
			period = 1.f / rate;
			phase += rate * dt;
			if (phase >= 1.f) { phase -= std::floor(phase); tick = true; }
		}
		if (tick) doTick();

		for (int c = 0; c < WK_N; c++) {
			Cyc& k = cyc[c];
			outV[c] = voct(c);
			outputs[VOCT_OUTPUT + c].setVoltage(outV[c]);
			outputs[GATE_OUTPUT + c].setVoltage(k.gate.out());
			outputs[CRASH_OUTPUT + c].setVoltage(k.crashGate.out());
			float sv = k.score * 0.01f;
			if (scoreWrap) sv = std::fmod(sv, 10.f);
			else sv = std::min(sv, 10.f);
			outputs[SCORE_OUTPUT + c].setVoltage(sv);
			lights[ON_LIGHT + c].setBrightness(!k.on ? 0.f : k.alive ? 1.f : 0.25f);
		}
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		wrapEdges = false; respawnDelay = 2; gateMode = 1; ivIdx = 0; scoreWrap = true; view = 1;
		camYaw = 0.f; camElev = WK_ELEV0;
		for (int c = 0; c < WK_N; c++) cyc[c].div = 1;
		shapeReq = WK_SQUARE;
		sizeReq = 2;
		clearReq = true;
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "size", json_integer(sizeIdx));
		json_object_set_new(r, "shape", json_integer(shape));
		json_object_set_new(r, "wrap", json_boolean(wrapEdges));
		json_object_set_new(r, "respawn", json_integer(respawnDelay));
		json_object_set_new(r, "gateMode", json_integer(gateMode));
		json_object_set_new(r, "view", json_integer(view));
		json_object_set_new(r, "camYaw", json_real(camYaw));
		json_object_set_new(r, "camElev", json_real(camElev));
		sfs::rootCvToJson(r, rootCvMode);
		json_object_set_new(r, "intervals", json_integer(ivIdx));
		json_object_set_new(r, "scoreWrap", json_boolean(scoreWrap));
		json_t* dv = json_array();
		json_t* sc = json_array();
		for (int c = 0; c < WK_N; c++) {
			json_array_append_new(dv, json_integer(cyc[c].div));
			json_array_append_new(sc, json_integer(cyc[c].score));
		}
		json_object_set_new(r, "div", dv);
		json_object_set_new(r, "scores", sc);
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "size")) sizeReq = clamp((int)json_integer_value(j), 0, WK_NSIZE - 1);
		if (json_t* j = json_object_get(r, "shape")) shapeReq = clamp((int)json_integer_value(j), 0, WK_NSHAPE - 1);
		if (json_t* j = json_object_get(r, "wrap")) wrapEdges = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "respawn")) respawnDelay = clamp((int)json_integer_value(j), 0, 16);
		if (json_t* j = json_object_get(r, "gateMode")) gateMode = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(r, "view")) view = clamp((int)json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(r, "camYaw")) camYaw = (float)json_number_value(j);
		if (json_t* j = json_object_get(r, "camElev")) camElev = clamp((float)json_number_value(j), WK_ELEV_MIN, WK_ELEV_MAX);
		sfs::rootCvFromJson(r, this, ROOT_INPUT, rootCvMode);
		if (json_t* j = json_object_get(r, "intervals")) ivIdx = clamp((int)json_integer_value(j), 0, WK_NIV - 1);
		if (json_t* j = json_object_get(r, "scoreWrap")) scoreWrap = json_boolean_value(j);
		if (json_t* a = json_object_get(r, "div"))
			for (int c = 0; c < WK_N && c < (int)json_array_size(a); c++)
				cyc[c].div = clamp((int)json_integer_value(json_array_get(a, c)), 1, 4);
		if (json_t* a = json_object_get(r, "scores"))
			for (int c = 0; c < WK_N && c < (int)json_array_size(a); c++)
				cyc[c].score = std::max<long>(0, (long)json_integer_value(json_array_get(a, c)));
	}
};

// =============================================================================
// The screen: a status line (each cycle's letter, style and score) over the
// arena. Trails are drawn as lines through the cells they cover; a crashed
// trail fades out over half a second and a ring opens where it crashed.
// Click the arena to steer a Keys cycle from the keyboard.
// =============================================================================
static Wake* wakePreview() {
	static Wake* m = nullptr;
	if (m) return m;
	m = new Wake();
	m->params[Wake::RATE_PARAM].setValue(1.f);     // 32 steps a second
	int64_t frame = 0;
	sfs::previewRun(*m, 3.2f, frame);
	return m;
}

struct WakeDisplay : OpaqueWidget {
	Wake* module = nullptr;
	std::shared_ptr<Font> font;
	bool hovered = false;

	float sc() const { return box.size.x / WK_SW; }

	struct Arena { float x0, y0, cs; };
	Arena arena(const Wake* m) const {
		Arena a;
		float ww = m->worldW(), wh = m->worldH();
		a.cs = std::min(WK_AW / ww, WK_AH / wh);
		a.x0 = (WK_SW - a.cs * ww) / 2.f;
		a.y0 = WK_STRIP + 0.4f + (WK_AH - a.cs * wh) / 2.f;
		return a;
	}

	// The floor's lines and the arena's edge, as segments on the floor (x0 y0
	// x1 y1), made again when the grid changes. Square: the lines between
	// cells. Hex and Triangle: every cell's edges, each drawn once; the edges
	// with no cell beyond them are the arena's edge.
	std::vector<float> floorL, rimL;
	int floorKey = -1;
	void buildFloor(const Wake* m) {
		int key = (m->shape * 128 + m->W) * 64 + m->H;
		if (key == floorKey) return;
		floorKey = key;
		floorL.clear(); rimL.clear();
		auto seg = [](std::vector<float>& v, float a, float b, float c, float d) {
			v.push_back(a); v.push_back(b); v.push_back(c); v.push_back(d);
		};
		float W = m->worldW(), H = m->worldH();
		if (m->shape == WK_SQUARE) {
			for (int x = 0; x <= m->W; x++) seg(floorL, (float)x, 0.f, (float)x, H);
			for (int y = 0; y <= m->H; y++) seg(floorL, 0.f, (float)y, W, (float)y);
			seg(rimL, 0, 0, W, 0); seg(rimL, W, 0, W, H); seg(rimL, W, H, 0, H); seg(rimL, 0, H, 0, 0);
			return;
		}
		float hl = m->shape == WK_HEX ? 0.5f * WK_HEXR : WK_R3;     // half an edge
		for (int y = 0; y < m->H; y++)
			for (int x = 0; x < m->W; x++) {
				float cx = m->wxOf(x, y), cy = m->wyOf(x, y);
				for (int d = 0; d < 6; d++) {
					if (!m->hasEdge(x, y, d)) continue;
					int nx, ny;
					bool in = m->stepRaw(x, y, d, nx, ny) && nx >= 0 && nx < m->W && ny >= 0 && ny < m->H;
					if (in && ny * m->W + nx < y * m->W + x) continue;    // its neighbour drew it
					float ux, uy;
					m->dirVec(d, ux, uy);
					float ex = cx + 0.5f * ux, ey = cy + 0.5f * uy;
					seg(floorL, ex + hl * uy, ey - hl * ux, ex - hl * uy, ey + hl * ux);
					if (!in) seg(rimL, ex + hl * uy, ey - hl * ux, ex - hl * uy, ey + hl * ux);
				}
			}
	}
	// A cell's outline on the floor, scaled about its centre; returns the corner count.
	int cellPoly(const Wake* m, int x, int y, float k, float* px, float* py) const {
		float cx = m->wxOf(x, y), cy = m->wyOf(x, y);
		int n = 0;
		if (m->shape == WK_SQUARE) {
			static const float SX[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, SY[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
			for (; n < 4; n++) { px[n] = cx + k * SX[n]; py[n] = cy + k * SY[n]; }
		} else if (m->shape == WK_HEX) {
			for (; n < 6; n++) {
				float t = (float)M_PI * (-0.5f + n / 3.f);
				px[n] = cx + k * WK_HEXR * std::cos(t); py[n] = cy + k * WK_HEXR * std::sin(t);
			}
		} else {
			float f = ((x + y) & 1) ? -1.f : 1.f;     // down-pointing where x + y is even
			const float TX[3] = {-WK_R3, WK_R3, 0.f}, TY[3] = {-0.5f, -0.5f, 1.f};
			for (; n < 3; n++) { px[n] = cx + k * TX[n]; py[n] = cy + k * f * TY[n]; }
		}
		return n;
	}

	void rect(NVGcontext* vg, float x, float y, float w, float h, NVGcolor c, float r = 0.f) {
		float s = sc();
		nvgBeginPath(vg);
		if (r > 0.f) nvgRoundedRect(vg, x * s, y * s, w * s, h * s, r * s);
		else nvgRect(vg, x * s, y * s, w * s, h * s);
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

	// a path through a run of cells, broken where two neighbours in the run are
	// not adjacent on screen (a wrap across an edge)
	void trailPath(NVGcontext* vg, const Wake* m, const Arena& a, const int* cells, int start, int len, int ring) {
		float s = sc();
		float px = -100.f, py = -100.f;
		nvgBeginPath(vg);
		for (int i = 0; i < len; i++) {
			int cell = cells[(start + i) % ring];
			int x = cell % m->W, y = cell / m->W;
			float wx = m->wxOf(x, y), wy = m->wyOf(x, y);
			float cx = (a.x0 + wx * a.cs) * s, cy = (a.y0 + wy * a.cs) * s;
			if (std::fabs(wx - px) + std::fabs(wy - py) < 1.5f) nvgLineTo(vg, cx, cy);
			else { nvgMoveTo(vg, cx, cy); nvgLineTo(vg, cx + 0.01f, cy); }
			px = wx; py = wy;
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!module) { module = wakePreview(); drawLayer(args, layer); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		bool hasFont = font && font->handle >= 0;
		Wake* m = module;
		int middle = NVG_ALIGN_MIDDLE;

		rect(vg, 0.f, 0.f, WK_SW, WK_SH, sfs::SCREEN_BG, 1.27f);
		nvgSave(vg);
		if (m->view == 1) draw3D(vg, m);
		else drawFlat(vg, m);
		nvgRestore(vg);
		Arena a = arena(m);
		float aw = a.cs * m->worldW(), ah = a.cs * m->worldH();

		// the status line
		if (hasFont) {
			float cw = WK_SW / WK_N, ym = WK_STRIP / 2.f + 0.3f;
			for (int c = 0; c < WK_N; c++) {
				const Wake::Cyc& k = m->cyc[c];
				float x = c * cw + 1.6f;
				NVGcolor col = k.on ? WK_COL[c] : sfs::SCREEN_PMID;
				rect(vg, x, ym - 1.3f, 2.6f, 2.6f, col, 0.3f);
				std::string name = string::f("%c %s", 'A' + c, k.on ? WK_STYLE_SHORT[m->curStyle[c]] : "OFF");
				text(vg, x + 3.6f, ym, name, k.on && k.alive ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM,
				     NVG_ALIGN_LEFT | middle, sfs::TYPE_SCREEN_SMALL);
				if (k.on && k.alive) {
					static const char* NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
					int n = (int)std::round(m->outV[c] * 12.f);
					int pc = ((n % 12) + 12) % 12, oct = 4 + (n - pc) / 12;
					text(vg, c * cw + 0.66f * cw, ym, string::f("%s%d", NN[pc], oct), WK_COL[c],
					     NVG_ALIGN_RIGHT | middle, sfs::TYPE_SCREEN_SMALL);
				}
				if (k.on)
					text(vg, (c + 1) * cw - 1.6f, ym, string::f("%ld", k.score),
					     k.alive ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM, NVG_ALIGN_RIGHT | middle, sfs::TYPE_SCREEN_SMALL);
			}
			// where the keys go, while a Keys cycle is on
			bool selected = APP->event->selectedWidget == this;
			if (m->keysCycle(0) >= 0 && !selected && !hovered)
				text(vg, a.x0 + aw - 0.8f, a.y0 + ah - 1.4f, "CLICK HERE TO STEER", sfs::SCREEN_DIM,
				     NVG_ALIGN_RIGHT | middle, sfs::TYPE_SCREEN_SMALL);
		}
		OpaqueWidget::drawLayer(args, layer);
	}

	// ------------------------------------------------------------ flat
	void drawFlat(NVGcontext* vg, const Wake* m) {
		float s = sc();
		Arena a = arena(m);
		buildFloor(m);
		// the grid, then the edge
		for (int pass = 0; pass < 2; pass++) {
			const std::vector<float>& L = pass ? rimL : floorL;
			nvgBeginPath(vg);
			for (size_t i = 0; i + 3 < L.size(); i += 4) {
				nvgMoveTo(vg, (a.x0 + L[i] * a.cs) * s, (a.y0 + L[i + 1] * a.cs) * s);
				nvgLineTo(vg, (a.x0 + L[i + 2] * a.cs) * s, (a.y0 + L[i + 3] * a.cs) * s);
			}
			nvgStrokeColor(vg, pass == 0 ? sfs::SCREEN_PURP : m->wrapEdges ? sfs::SCREEN_PMID : sfs::SCREEN_DIM);
			nvgStrokeWidth(vg, (pass ? 0.3f : 0.12f) * s);
			nvgStroke(vg);
		}

		nvgLineCap(vg, NVG_ROUND);
		nvgLineJoin(vg, NVG_ROUND);
		for (int c = 0; c < WK_N; c++) {
			const Wake::Cyc& k = m->cyc[c];
			// a crashed trail fades and a ring opens where it ended
			float age = (float)(m->tSec - k.ghostT);
			if (age >= 0.f && age < 0.5f && k.ghostLen > 0) {
				float f = 1.f - age / 0.5f;
				trailPath(vg, m, a, k.ghost, 0, k.ghostLen, WK_MAXC);
				nvgStrokeColor(vg, nvgTransRGBAf(WK_COL[c], 0.7f * f));
				nvgStrokeWidth(vg, 0.45f * a.cs * s);
				nvgStroke(vg);
				if (k.crashCell >= 0) {
					int x = k.crashCell % m->W, y = k.crashCell / m->W;
					float cx = a.x0 + m->wxOf(x, y) * a.cs, cy = a.y0 + m->wyOf(x, y) * a.cs;
					nvgBeginPath(vg);
					nvgCircle(vg, cx * s, cy * s, (0.6f + 3.f * age / 0.5f) * a.cs * s);
					nvgStrokeColor(vg, nvgTransRGBAf(WK_COL[c], f));
					nvgStrokeWidth(vg, 0.3f * s);
					nvgStroke(vg);
				}
			}
			if (!k.on || !k.alive || k.tLen <= 0) continue;
			trailPath(vg, m, a, k.trail, k.tStart, k.tLen, WK_MAXC);
			nvgStrokeColor(vg, nvgTransRGBAf(WK_COL[c], 0.85f));
			nvgStrokeWidth(vg, 0.45f * a.cs * s);
			nvgStroke(vg);
			if (m->shape == WK_SQUARE) {
				float hx = a.x0 + k.x * a.cs, hy = a.y0 + k.y * a.cs;
				rect(vg, hx + 0.05f * a.cs, hy + 0.05f * a.cs, 0.9f * a.cs, 0.9f * a.cs, WK_COL[c], 0.15f * a.cs);
				rect(vg, hx + 0.3f * a.cs, hy + 0.3f * a.cs, 0.4f * a.cs, 0.4f * a.cs, nvgRGB(0xFF, 0xFF, 0xFF));
			} else {
				// the head's cell, filled, with a white dot at its centre
				float px[6], py[6];
				int n = cellPoly(m, k.x, k.y, 0.85f, px, py);
				nvgBeginPath(vg);
				nvgMoveTo(vg, (a.x0 + px[0] * a.cs) * s, (a.y0 + py[0] * a.cs) * s);
				for (int i = 1; i < n; i++) nvgLineTo(vg, (a.x0 + px[i] * a.cs) * s, (a.y0 + py[i] * a.cs) * s);
				nvgClosePath(vg);
				nvgFillColor(vg, WK_COL[c]);
				nvgFill(vg);
				nvgBeginPath(vg);
				nvgCircle(vg, (a.x0 + m->wxOf(k.x, k.y) * a.cs) * s, (a.y0 + m->wyOf(k.x, k.y) * a.cs) * s, 0.2f * a.cs * s);
				nvgFillColor(vg, nvgRGB(0xFF, 0xFF, 0xFF));
				nvgFill(vg);
			}
		}

	}

	// ------------------------------------------------------------ 3D
	// The arena seen from above in perspective. A trail is a semi-transparent
	// wall standing on the cells it passed through, each cycle is drawn as a
	// motorcycle at the end of its wall, and the point where the wall comes out
	// of the cycle glows. Between steps the cycles are drawn part of the way to
	// the cell they are entering, so they move smoothly. Drag to turn the camera
	// round the arena or tilt it; double-click to put it back.
	struct Cam {
		float ce = 1.f, se = 0.f, cy = 1.f, sy = 0.f, D = 1.f, W = 1.f, H = 1.f;
		float k = 1.f, ox = 0.f, oy = 0.f;
		float ex = 0.f, ey = 0.f, eh = 0.f;    // the eye, in grid units
		// (gx, gy) in cells across the floor and h in cells up, to mm on the screen
		Vec p(float gx, float gy, float h) const {
			float wx = gx - 0.5f * W, wz = gy - 0.5f * H;
			float x = wx * cy - wz * sy, z = wx * sy + wz * cy;
			float yc = h * ce - z * se;
			float d = std::max(0.1f, D - h * se - z * ce);
			return Vec(ox + k * x / d, oy - k * yc / d);
		}
		float depth(float gx, float gy, float h) const {
			float wx = gx - 0.5f * W, wz = gy - 0.5f * H;
			return D - h * se - (wx * sy + wz * cy) * ce;
		}
		float cellAt(float gx, float gy) const { return k / std::max(0.1f, depth(gx, gy, 0.f)); }
		bool facing(float nx, float ny, float nh, float qx, float qy, float qh) const {
			return nx * (ex - qx) + ny * (ey - qy) + nh * (eh - qh) > 0.f;
		}
	};
	Cam cam3D(const Wake* m) const {
		Cam c;
		c.ce = std::cos(m->camElev); c.se = std::sin(m->camElev);
		c.cy = std::cos(m->camYaw); c.sy = std::sin(m->camYaw);
		c.W = m->worldW(); c.H = m->worldH();
		c.D = 1.7f * std::max(c.W, 2.4f * c.H);
		c.ex = 0.5f * c.W + c.sy * c.D * c.ce;
		c.ey = 0.5f * c.H + c.cy * c.D * c.ce;
		c.eh = c.D * c.se;
		float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
		for (int i = 0; i < 8; i++) {
			Vec q = c.p((i & 1) ? c.W : 0.f, (i & 2) ? c.H : 0.f, (i & 4) ? WK_WALLH : 0.f);
			u0 = std::min(u0, q.x); u1 = std::max(u1, q.x);
			v0 = std::min(v0, q.y); v1 = std::max(v1, q.y);
		}
		c.k = std::min((WK_AW - 2.f) / (u1 - u0), (WK_AH - 2.f) / (v1 - v0));
		c.ox = 0.5f * WK_SW - c.k * 0.5f * (u0 + u1);
		c.oy = WK_STRIP + 0.4f + 0.5f * WK_AH - c.k * 0.5f * (v0 + v1);
		return c;
	}

	// One straight stretch of wall between two points on the floor (cell
	// centres, corners, or the part of a cell a cycle has reached). c 0-3 is a
	// cycle's, 4 the arena edge. A stretch is drawn as ONE panel, never a panel
	// per cell: the edges of panels that meet show as lines.
	struct Seg { float x0, y0, x1, y1, h, depth; uint8_t c, ghost; };
	struct Bike { float x, y, dx, dy, depth; uint8_t c; };
	std::vector<Seg> segs;
	std::vector<Bike> bikes;
	std::vector<Seg> parts;                 // the stretches as drawn, cut where a cycle needs it
	std::vector<int> bin[WK_N + 1][2 * (WK_N + 1)];

	void addSeg(const Cam& cm, float x0, float y0, float x1, float y1, float h, int c, bool ghost) {
		float d = cm.depth(0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * h);
		segs.push_back(Seg{x0, y0, x1, y1, h, d, (uint8_t)c, (uint8_t)ghost});
	}
	void addTrail(const Wake* m, const Cam& cm, const int* cells, int start, int len, int c, bool ghost,
	              float h, float headF, float tailF) {
		auto cx = [&](int i) { int cl = cells[(start + i) % WK_MAXC]; return m->wxOf(cl % m->W, cl / m->W); };
		auto cy = [&](int i) { int cl = cells[(start + i) % WK_MAXC]; return m->wyOf(cl % m->W, cl / m->W); };
		size_t first = segs.size();
		for (int i = 0; i + 1 < len; i++) {
			float ax = cx(i), ay = cy(i), bx = cx(i + 1), by = cy(i + 1);
			if (std::fabs(bx - ax) + std::fabs(by - ay) > 1.5f) continue;     // a wrap across an edge
			float t0 = (i == 0) ? tailF : 0.f, t1 = (i + 2 == len) ? headF : 1.f;
			if (t1 <= t0) continue;
			float x0 = ax + (bx - ax) * t0, y0 = ay + (by - ay) * t0;
			float x1 = ax + (bx - ax) * t1, y1 = ay + (by - ay) * t1;
			// carry on the stretch before when this piece continues it in a straight line
			if (segs.size() > first) {
				Seg& L = segs.back();
				bool joined = std::fabs(L.x1 - x0) + std::fabs(L.y1 - y0) < 1e-4f;
				bool straight = std::fabs((L.x1 - L.x0) * (y1 - y0) - (L.y1 - L.y0) * (x1 - x0)) < 1e-4f;
				if (joined && straight) { L.x1 = x1; L.y1 = y1; continue; }
			}
			addSeg(cm, x0, y0, x1, y1, h, c, ghost);
		}
		for (size_t i = first; i < segs.size(); i++) {
			Seg& g = segs[i];
			g.depth = cm.depth(0.5f * (g.x0 + g.x1), 0.5f * (g.y0 + g.y1), 0.5f * g.h);
		}
	}

	void polyPath(NVGcontext* vg, const Vec* q, int n) {
		float s = sc();
		nvgMoveTo(vg, q[0].x * s, q[0].y * s);
		for (int i = 1; i < n; i++) nvgLineTo(vg, q[i].x * s, q[i].y * s);
		nvgClosePath(vg);
	}
	void glow(NVGcontext* vg, Vec at, float r, NVGcolor col, float a) {
		float s = sc();
		NVGcolor in = nvgLerpRGBA(col, nvgRGB(0xFF, 0xFF, 0xFF), 0.55f);
		nvgBeginPath(vg);
		nvgCircle(vg, at.x * s, at.y * s, r * s);
		nvgFillPaint(vg, nvgRadialGradient(vg, at.x * s, at.y * s, 0.f, r * s,
		                                   nvgTransRGBAf(in, a), nvgTransRGBAf(col, 0.f)));
		nvgFill(vg);
	}

	// Where a cycle's body lands on the screen, in mm (the glow is light and
	// needs no order, so it is not included).
	Rect bikeBox(const Cam& cm, const Bike& b) const {
		const float w = 0.12f, L = 1.9f, back = 0.15f, top = 0.7f;
		float rx = b.x - b.dx * back, ry = b.y - b.dy * back;
		Vec lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
		for (int i = 0; i < 8; i++) {
			float a = (i & 1) ? L : 0.f, l = (i & 2) ? w : -w, h = (i & 4) ? top : 0.f;
			Vec q = cm.p(rx + b.dx * a - b.dy * l, ry + b.dy * a + b.dx * l, h);
			lo = lo.min(q); hi = hi.max(q);
		}
		return Rect(lo, hi.minus(lo));
	}

	// The cycle: a long low hull over a wheel at each end, black with lit
	// edges, larger than a cell so it reads as a motorcycle. The hull is convex,
	// so drawing only the faces turned toward the eye draws it correctly.
	void drawBike(NVGcontext* vg, const Wake* m, const Cam& cm, const Bike& b) {
		// a long teardrop: raised at the tail, the cowl highest over the front
		// wheel, falling to the nose, and clear of the floor so the wheels show
		static const float PA[6] = {0.f, 0.04f, 0.42f, 0.72f, 1.f, 0.86f};    // along, from the rear
		static const float PH[6] = {0.26f, 0.56f, 0.62f, 0.66f, 0.36f, 0.16f}; // up, in cells
		const float w = 0.11f, L = 1.9f, back = 0.15f;
		float s = sc();
		float cs = cm.cellAt(b.x, b.y);
		NVGcolor col = WK_COL[b.c];
		float rx = b.x - b.dx * back, ry = b.y - b.dy * back;   // the rear
		auto at = [&](float a, float l, float h) {
			return cm.p(rx + b.dx * a * L - b.dy * l, ry + b.dy * a * L + b.dx * l, h);
		};
		nvgLineJoin(vg, NVG_ROUND);
		auto face = [&](const Vec* q, int n, float light) {
			nvgBeginPath(vg);
			polyPath(vg, q, n);
			nvgFillColor(vg, nvgLerpRGBA(nvgRGB(0x08, 0x08, 0x12), col, light));
			nvgFill(vg);
			nvgStrokeColor(vg, nvgTransRGBAf(col, 0.95f));
			nvgStrokeWidth(vg, std::max(0.06f, 0.04f * cs) * s);
			nvgStroke(vg);
		};
		// the band round the hull: one face for each edge of the side profile
		for (int i = 0; i < 6; i++) {
			int j = (i + 1) % 6;
			float ea = (PA[j] - PA[i]) * L, eh = PH[j] - PH[i];
			float na = -eh, nh = ea;                // outward, as the profile runs clockwise
			float nl = std::sqrt(na * na + nh * nh);
			if (nl < 1e-6f) continue;
			na /= nl; nh /= nl;
			float qa = 0.5f * (PA[i] + PA[j]) * L, qh = 0.5f * (PH[i] + PH[j]);
			if (!cm.facing(b.dx * na, b.dy * na, nh, rx + b.dx * qa, ry + b.dy * qa, qh)) continue;
			Vec q[4] = {at(PA[i], -w, PH[i]), at(PA[j], -w, PH[j]), at(PA[j], w, PH[j]), at(PA[i], w, PH[i])};
			face(q, 4, 0.08f + 0.14f * std::max(0.f, nh));
		}
		// the side turned toward the eye, with its two wheels
		for (int sd = -1; sd <= 1; sd += 2) {
			float nx = -b.dy * sd, ny = b.dx * sd;
			if (!cm.facing(nx, ny, 0.f, rx + 0.5f * L * b.dx + nx * w, ry + 0.5f * L * b.dy + ny * w, 0.3f)) continue;
			Vec q[6];
			for (int i = 0; i < 6; i++) q[i] = at(PA[i], sd * w, PH[i]);
			face(q, 6, 0.04f);
			for (int wh = 0; wh < 2; wh++) {
				float wa = wh ? 0.80f : 0.19f, r = 0.33f, hc = 0.34f;
				Vec rim[16];
				for (int i = 0; i < 16; i++) {
					float t = 2.f * (float)M_PI * i / 16.f;
					rim[i] = at(wa + r * std::cos(t) / L, sd * (w + 0.01f), hc + r * std::sin(t));
				}
				nvgBeginPath(vg);
				polyPath(vg, rim, 16);
				nvgFillColor(vg, nvgRGB(0x04, 0x04, 0x0a));
				nvgFill(vg);
				nvgStrokeColor(vg, nvgLerpRGBA(col, nvgRGB(0xFF, 0xFF, 0xFF), 0.3f));
				nvgStrokeWidth(vg, std::max(0.08f, 0.07f * cs) * s);
				nvgStroke(vg);
				nvgBeginPath(vg);
				Vec hub = at(wa, sd * (w + 0.01f), hc);
				nvgCircle(vg, hub.x * s, hub.y * s, std::max(0.1f, 0.05f * cs) * s);
				nvgFillColor(vg, col);
				nvgFill(vg);
			}
		}
		// where the wall comes out of the cycle
		nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
		Vec e = cm.p(b.x, b.y, 0.5f * WK_WALLH);
		float pulse = 0.85f + 0.15f * std::sin((float)m->tSec * 9.f + b.c);
		glow(vg, e, 1.1f * cs, col, 0.75f * pulse);
		glow(vg, e, 0.28f * cs, nvgRGB(0xFF, 0xFF, 0xFF), 0.95f);
		nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
	}

	// Draws the stretches of wall listed in idx, which all belong to one cycle
	// (or the edge). Walls are light: everything here is ADDED to what is
	// already drawn, so the walls need no order among themselves and two that
	// overlap are simply brighter there.
	void drawWalls(NVGcontext* vg, const Cam& cm, const std::vector<int>& idx) {
		if (idx.empty()) return;
		float s = sc();
		const Seg& g0 = parts[idx[0]];
		bool edge = g0.c >= WK_N;
		NVGcolor col = edge ? sfs::SCREEN_PMID : WK_COL[g0.c];
		float fade = g0.ghost ? g0.h / WK_WALLH : 1.f;
		float cs = cm.cellAt(0.5f * cm.W, 0.5f * cm.H);
		nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
		// the panel, then a brighter band along its upper part, so the wall is
		// lit toward its top edge in true perspective
		for (int band = 0; band < 2; band++) {
			if (edge && band) break;
			nvgBeginPath(vg);
			for (int i : idx) {
				const Seg& g = parts[i];
				float lo = band ? 0.62f * g.h : 0.f;
				Vec q[4] = {cm.p(g.x0, g.y0, lo), cm.p(g.x1, g.y1, lo), cm.p(g.x1, g.y1, g.h), cm.p(g.x0, g.y0, g.h)};
				polyPath(vg, q, 4);
			}
			float a = edge ? 0.10f : band ? 0.24f : 0.2f;
			nvgFillColor(vg, nvgTransRGBAf(col, a * fade));
			nvgFill(vg);
		}
		// a line along the top and one along the floor, continuous through the corners
		for (int top = 1; top >= 0; top--) {
			nvgBeginPath(vg);
			float px = -1e9f, py = -1e9f;
			for (int i : idx) {
				const Seg& g = parts[i];
				Vec a = cm.p(g.x0, g.y0, top ? g.h : 0.f), b = cm.p(g.x1, g.y1, top ? g.h : 0.f);
				if (std::fabs(g.x0 - px) + std::fabs(g.y0 - py) > 1e-3f) nvgMoveTo(vg, a.x * s, a.y * s);
				nvgLineTo(vg, b.x * s, b.y * s);
				px = g.x1; py = g.y1;
			}
			if (edge) {
				if (top) {
					nvgStrokeColor(vg, nvgTransRGBAf(col, 0.6f));
					nvgStrokeWidth(vg, 0.15f * s);
					nvgStroke(vg);
				}
				continue;
			}
			if (top) {
				nvgStrokeColor(vg, nvgTransRGBAf(col, 0.22f * fade));
				nvgStrokeWidth(vg, 0.45f * cs * s);
				nvgStroke(vg);
				nvgStrokeColor(vg, nvgTransRGBAf(nvgLerpRGBA(col, nvgRGB(0xFF, 0xFF, 0xFF), 0.45f), 0.9f * fade));
				nvgStrokeWidth(vg, std::max(0.08f, 0.08f * cs) * s);
			} else {
				nvgStrokeColor(vg, nvgTransRGBAf(col, 0.45f * fade));
				nvgStrokeWidth(vg, std::max(0.06f, 0.06f * cs) * s);
			}
			nvgStroke(vg);
		}
		nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
	}

	void draw3D(NVGcontext* vg, const Wake* m) {
		float s = sc();
		Cam cm = cam3D(m);
		float W = cm.W, H = cm.H;

		// the floor and its grid
		buildFloor(m);
		Vec fl[4] = {cm.p(0, 0, 0), cm.p(W, 0, 0), cm.p(W, H, 0), cm.p(0, H, 0)};
		nvgBeginPath(vg);
		polyPath(vg, fl, 4);
		nvgFillColor(vg, nvgRGB(0x10, 0x10, 0x22));
		nvgFill(vg);
		nvgBeginPath(vg);
		for (size_t i = 0; i + 3 < floorL.size(); i += 4) {
			Vec a = cm.p(floorL[i], floorL[i + 1], 0), b = cm.p(floorL[i + 2], floorL[i + 3], 0);
			nvgMoveTo(vg, a.x * s, a.y * s); nvgLineTo(vg, b.x * s, b.y * s);
		}
		nvgStrokeColor(vg, sfs::SCREEN_PURP);
		nvgStrokeWidth(vg, 0.1f * s);
		nvgStroke(vg);
		if (m->wrapEdges) {
			nvgBeginPath(vg);
			polyPath(vg, fl, 4);
			nvgStrokeColor(vg, sfs::SCREEN_PMID);
			nvgStrokeWidth(vg, 0.2f * s);
			nvgStroke(vg);
		}

		// gather the walls, the edge and the cycles
		segs.clear(); bikes.clear();
		if (!m->wrapEdges) {
			const float RIM = 0.4f;
			for (size_t i = 0; i + 3 < rimL.size(); i += 4)
				addSeg(cm, rimL[i], rimL[i + 1], rimL[i + 2], rimL[i + 3], RIM, WK_N, false);
		}
		int tmax = m->tmaxNow;
		for (int c = 0; c < WK_N; c++) {
			const Wake::Cyc& k = m->cyc[c];
			float age = (float)(m->tSec - k.ghostT);
			if (age >= 0.f && age < 0.5f && k.ghostLen > 1) {
				float f = 1.f - age / 0.5f;
				addTrail(m, cm, k.ghost, 0, k.ghostLen, c, true, WK_WALLH * f * f, 1.f, 0.f);
			}
			if (!k.on || !k.alive || k.tLen <= 0) continue;
			float f = 1.f;
			if (k.moveT > -1.0 && k.moveDur > 1e-4f) f = clamp((float)((m->tSec - k.moveT) / k.moveDur), 0.f, 1.f);
			int n = k.tLen;
			int hc = k.trail[(k.tStart + n - 1) % WK_MAXC];
			float bx = m->wxOf(hc % m->W, hc / m->W), by = m->wyOf(hc % m->W, hc / m->W);
			float dx, dy;
			m->dirVec(k.dir, dx, dy);
			if (n >= 2) {
				int pc = k.trail[(k.tStart + n - 2) % WK_MAXC];
				float px = m->wxOf(pc % m->W, pc / m->W), py = m->wyOf(pc % m->W, pc / m->W);
				if (std::fabs(bx - px) + std::fabs(by - py) < 1.5f) {
					dx = bx - px; dy = by - py;
					bx = px + dx * f; by = py + dy * f;
				}
			}
			// On Triangle every move turns 60 degrees one way or the other, so the
			// cycle is pointed along its line instead (two moves at a time), turning
			// from the last line to this one as it moves
			if (m->threeWay() && n >= 4) {
				float qx[4], qy[4];
				bool ok = true;
				for (int i = 0; i < 4; i++) {
					int cl = k.trail[(k.tStart + n - 1 - i) % WK_MAXC];
					qx[i] = m->wxOf(cl % m->W, cl / m->W); qy[i] = m->wyOf(cl % m->W, cl / m->W);
					if (i > 0 && std::fabs(qx[i] - qx[i - 1]) + std::fabs(qy[i] - qy[i - 1]) > 1.5f) ok = false;
				}
				if (ok) {
					float lx = (qx[1] - qx[3]) + f * ((qx[0] - qx[2]) - (qx[1] - qx[3]));
					float ly = (qy[1] - qy[3]) + f * ((qy[0] - qy[2]) - (qy[1] - qy[3]));
					float l = std::sqrt(lx * lx + ly * ly);
					if (l > 1e-3f) { dx = lx / l; dy = ly / l; }
				}
			}
			bool shrinking = tmax > 0 && n >= tmax;
			addTrail(m, cm, k.trail, k.tStart, n, c, false, WK_WALLH, f, shrinking ? f : 0.f);
			bikes.push_back(Bike{bx, by, dx, dy, cm.depth(bx + 0.8f * dx, by + 0.8f * dy, 0.3f), (uint8_t)c});
		}
		// The cycles are solid, so each one has to come after the walls behind it
		// and before the walls in front of it; among themselves the walls need no
		// order. A stretch is ordered only against the cycles it overlaps on the
		// screen, so a stretch nowhere near a cycle never changes place. Where a
		// stretch overlaps a cycle and passes it in depth, it is cut at the
		// cycle's depth and each piece goes on its own side. (Placing a whole
		// stretch by its midpoint made long walls jump from behind a cycle to in
		// front of it as they grew, which showed as flicker.)
		std::sort(bikes.begin(), bikes.end(), [](const Bike& a, const Bike& b) { return a.depth > b.depth; });
		int nb = (int)bikes.size();
		Rect bb[WK_N];
		for (int j = 0; j < nb; j++) bb[j] = bikeBox(cm, bikes[j]);
		for (int b = 0; b <= WK_N; b++) for (auto& v : bin[b]) v.clear();
		parts.clear();
		for (const Seg& g : segs) {
			Vec q[4] = {cm.p(g.x0, g.y0, 0), cm.p(g.x1, g.y1, 0), cm.p(g.x1, g.y1, g.h), cm.p(g.x0, g.y0, g.h)};
			Vec lo = q[0], hi = q[0];
			for (int i = 1; i < 4; i++) { lo = lo.min(q[i]); hi = hi.max(q[i]); }
			int ov[WK_N], no = 0;
			for (int j = 0; j < nb; j++)
				if (lo.x < bb[j].pos.x + bb[j].size.x && hi.x > bb[j].pos.x && lo.y < bb[j].pos.y + bb[j].size.y && hi.y > bb[j].pos.y)
					ov[no++] = j;
			int key = g.c * 2 + g.ghost;
			if (no == 0) { bin[0][key].push_back((int)parts.size()); parts.push_back(g); continue; }
			float d0 = cm.depth(g.x0, g.y0, 0.5f * g.h), d1 = cm.depth(g.x1, g.y1, 0.5f * g.h);
			float ts[WK_N + 2]; int nt = 0;
			ts[nt++] = 0.f;
			for (int k = 0; k < no; k++) {
				float dd = d1 - d0;
				if (std::fabs(dd) < 1e-6f) continue;
				float t = (bikes[ov[k]].depth - d0) / dd;
				if (t > 1e-3f && t < 1.f - 1e-3f) ts[nt++] = t;
			}
			ts[nt++] = 1.f;
			std::sort(ts, ts + nt);
			for (int k = 0; k + 1 < nt; k++) {
				float ta = ts[k], tb = ts[k + 1];
				Seg pc = g;
				pc.x0 = g.x0 + (g.x1 - g.x0) * ta; pc.y0 = g.y0 + (g.y1 - g.y0) * ta;
				pc.x1 = g.x0 + (g.x1 - g.x0) * tb; pc.y1 = g.y0 + (g.y1 - g.y0) * tb;
				float dm = d0 + (d1 - d0) * 0.5f * (ta + tb);
				int b = 0;
				for (int i = 0; i < no; i++) if (bikes[ov[i]].depth > dm) b = std::max(b, ov[i] + 1);
				bin[b][key].push_back((int)parts.size());
				parts.push_back(pc);
			}
		}
		nvgLineCap(vg, NVG_BUTT);
		nvgLineJoin(vg, NVG_MITER);
		for (int b = 0; b <= nb; b++) {
			drawWalls(vg, cm, bin[b][2 * WK_N]);              // the edge first, under the trails
			for (int t = 0; t < 2 * WK_N; t++) drawWalls(vg, cm, bin[b][t]);
			if (b < nb) {
				nvgLineCap(vg, NVG_BUTT);
				drawBike(vg, m, cm, bikes[b]);
				nvgLineJoin(vg, NVG_MITER);
			}
		}

		// crashes: a ring opening on the floor and a flash
		for (int c = 0; c < WK_N; c++) {
			const Wake::Cyc& k = m->cyc[c];
			float age = (float)(m->tSec - k.ghostT);
			if (age < 0.f || age >= 0.5f || k.crashCell < 0) continue;
			float f = 1.f - age / 0.5f;
			float x = m->wxOf(k.crashCell % m->W, k.crashCell / m->W), y = m->wyOf(k.crashCell % m->W, k.crashCell / m->W);
			float r = 0.6f + 3.f * age / 0.5f;
			Vec ring[28];
			for (int i = 0; i < 28; i++) {
				float t = 2.f * (float)M_PI * i / 28.f;
				ring[i] = cm.p(x + r * std::cos(t), y + r * std::sin(t), 0);
			}
			nvgBeginPath(vg);
			polyPath(vg, ring, 28);
			nvgStrokeColor(vg, nvgTransRGBAf(WK_COL[c], f));
			nvgStrokeWidth(vg, 0.3f * s);
			nvgStroke(vg);
			nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
			glow(vg, cm.p(x, y, 0.4f), 3.f * cm.cellAt(x, y) * f, WK_COL[c], f);
			nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
		}
	}

	// ------------------------------------------------------------ the mouse
	void onEnter(const EnterEvent& e) override { hovered = true; OpaqueWidget::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; OpaqueWidget::onLeave(e); }

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS) { OpaqueWidget::onButton(e); return; }
		Vec p = e.pos.div(sc());
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT && p.y < WK_STRIP) {
			cycleMenu(clamp((int)(p.x / (WK_SW / WK_N)), 0, WK_N - 1));
			e.consume(this);
			return;
		}
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			APP->event->setSelectedWidget(this);
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
	void onDragMove(const DragMoveEvent& e) override {
		if (!module || module->view != 1 || e.button != GLFW_MOUSE_BUTTON_LEFT) return;
		Vec d = e.mouseDelta.div(APP->scene->rackScroll->getZoom());
		module->camYaw += d.x * 0.01f;
		if (module->camYaw > (float)M_PI) module->camYaw -= 2.f * (float)M_PI;
		if (module->camYaw < -(float)M_PI) module->camYaw += 2.f * (float)M_PI;
		module->camElev = clamp(module->camElev + d.y * 0.008f, WK_ELEV_MIN, WK_ELEV_MAX);
	}
	void onDoubleClick(const DoubleClickEvent& e) override {
		if (!module || module->view != 1) return;
		module->camYaw = 0.f; module->camElev = WK_ELEV0;
		e.consume(this);
	}

	void cycleMenu(int c) {
		Wake* m = module;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("Cycle %c", 'A' + c)));
		menu->addChild(createBoolMenuItem("On", "",
			[=]() { return m->params[Wake::ON_PARAM + c].getValue() > 0.5f; },
			[=](bool v) { m->params[Wake::ON_PARAM + c].setValue(v ? 1.f : 0.f); }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Style"));
		for (int st = 0; st < WK_NSTYLE; st++)
			menu->addChild(createCheckMenuItem(WK_STYLE_NAME[st], WK_STYLE_DESC[st],
				[=]() { return (int)std::round(m->params[Wake::STYLE_PARAM + c].getValue()) == st; },
				[=]() { m->params[Wake::STYLE_PARAM + c].setValue((float)st); }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Moves",
			{"On every step", "On every 2nd step", "On every 3rd step", "On every 4th step"},
			[=]() { return (size_t)(m->cyc[c].div - 1); },
			[=](size_t i) { m->cyc[c].div = (int)i + 1; }));
	}

	// ------------------------------------------------------------ the keyboard
	// Z and X turn the first Keys cycle left and right, , and . (the keys
	// marked < and >) the second. Each turn is relative to where the cycle is
	// heading, so it steers the same way whichever way the camera faces. Rack
	// uses none of these keys without Ctrl/Cmd, and the arrow keys are left to
	// Rack, which scrolls the rack with them. Rack sends keys to the selected
	// widget first, so after a click on the screen they steer wherever the
	// pointer is; hovering over the screen does too.
	bool steer(int key, int action) {
		if (!module) return false;
		int set = -1, t = 0;
		switch (key) {
			case GLFW_KEY_Z: set = 0; t = -1; break;
			case GLFW_KEY_X: set = 0; t = 1; break;
			case GLFW_KEY_COMMA: set = 1; t = -1; break;
			case GLFW_KEY_PERIOD: set = 1; t = 1; break;
			default: return false;
		}
		int c = module->keysCycle(set);
		if (c < 0) return false;
		if (action == GLFW_PRESS) module->pushKey(c, t);
		return true;    // the repeats and the release are ours too
	}
	void onSelectKey(const SelectKeyEvent& e) override {
		if ((e.mods & RACK_MOD_MASK) == 0 && steer(e.key, e.action)) { e.consume(this); return; }
		OpaqueWidget::onSelectKey(e);
	}
	void onHoverKey(const HoverKeyEvent& e) override {
		if ((e.mods & RACK_MOD_MASK) == 0 && steer(e.key, e.action)) { e.consume(this); return; }
		OpaqueWidget::onHoverKey(e);
	}
};

// =============================================================================
// Panel: 28HP, code-drawn until the Figma pass. The screen across the top; the
// global controls at the lower left (RATE, TRAIL, RANGE, ROOT, SCALE, NOTES,
// CLOCK, RESET); then
// one row per cycle: ON (lit in its colour), STYLE with its CV, TURN, and on
// the plate its V/OCT, GATE, CRASH and SCORE.
// =============================================================================
template <int I, typename TBase = GrayModuleLightWidget>
struct TWkLight : TBase {
	TWkLight() { this->addBaseColor(WK_COL[I]); }
};

static const float WK_SX = 5.08f, WK_SY = 10.16f;
static const float WK_RY[WK_N] = {83.82f, 95.25f, 106.68f, 118.11f};
static const float WK_GX0 = 10.16f, WK_GX1 = 20.32f, WK_GX2 = 30.48f, WK_GX3 = 40.64f;
static const float WK_DIV = 46.99f;
static const float WK_LETTER_X = 50.8f, WK_ON_X = 57.15f, WK_STYLE_X = 66.04f, WK_TURN_X = 86.36f;
static const float WK_OX[4] = {99.06f, 109.22f, 119.38f, 129.54f};

struct WakeWidget : ModuleWidget {
	WakeWidget(Wake* module) {
		setModule(module);
		setPanel(sfs::themedPanel("wake"));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("WAKE");

		WakeDisplay* disp = new WakeDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(WK_SX, WK_SY));
		disp->box.size = mm2px(Vec(WK_SW, WK_SH));
		addChild(disp);

		// globals
		addParam(createParamCentered<Trimpot>(mm2px(Vec(WK_GX0, WK_RY[0])), module, Wake::RATE_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_GX1, WK_RY[0])), module, Wake::RATE_INPUT));
		lbl->pair(WK_GX0, WK_RY[0], "RATE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(WK_GX0, WK_RY[1])), module, Wake::TRAIL_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_GX1, WK_RY[1])), module, Wake::TRAIL_INPUT));
		lbl->pair(WK_GX0, WK_RY[1], "TRAIL");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(WK_GX0, WK_RY[2])), module, Wake::RANGE_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_GX1, WK_RY[2])), module, Wake::RANGE_INPUT));
		lbl->pair(WK_GX0, WK_RY[2], "RANGE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(WK_GX2, WK_RY[0])), module, Wake::ROOT_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_GX3, WK_RY[0])), module, Wake::ROOT_INPUT));
		lbl->pair(WK_GX2, WK_RY[0], "ROOT");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(WK_GX2, WK_RY[1])), module, Wake::SCALE_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_GX3, WK_RY[1])), module, Wake::SCALE_INPUT));
		lbl->pair(WK_GX2, WK_RY[1], "SCALE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(WK_GX2, WK_RY[2])), module, Wake::NOTES_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_GX3, WK_RY[2])), module, Wake::NOTES_INPUT));
		lbl->pair(WK_GX2, WK_RY[2], "NOTES");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_GX0, WK_RY[3])), module, Wake::CLOCK_INPUT));
		lbl->jack(WK_GX0, WK_RY[3], "CLOCK");
		addParam(createParamCentered<VCVButton>(mm2px(Vec(WK_GX1, WK_RY[3])), module, Wake::RESET_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_GX2, WK_RY[3])), module, Wake::RESET_INPUT));
		lbl->pair(WK_GX1, WK_RY[3], "RESET");
		lbl->divider(WK_DIV, 76.2f, WK_DIV, 124.46f);

		// cycles
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<TWkLight<0>>>>(
			mm2px(Vec(WK_ON_X, WK_RY[0])), module, Wake::ON_PARAM + 0, Wake::ON_LIGHT + 0));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<TWkLight<1>>>>(
			mm2px(Vec(WK_ON_X, WK_RY[1])), module, Wake::ON_PARAM + 1, Wake::ON_LIGHT + 1));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<TWkLight<2>>>>(
			mm2px(Vec(WK_ON_X, WK_RY[2])), module, Wake::ON_PARAM + 2, Wake::ON_LIGHT + 2));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<TWkLight<3>>>>(
			mm2px(Vec(WK_ON_X, WK_RY[3])), module, Wake::ON_PARAM + 3, Wake::ON_LIGHT + 3));
		for (int c = 0; c < WK_N; c++) {
			float y = WK_RY[c];
			addParam(createParamCentered<Trimpot>(mm2px(Vec(WK_STYLE_X, y)), module, Wake::STYLE_PARAM + c));
			addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_STYLE_X + 10.16f, y)), module, Wake::STYLE_INPUT + c));
			addInput(createInputCentered<sfs::Jack>(mm2px(Vec(WK_TURN_X, y)), module, Wake::TURN_INPUT + c));
			addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(WK_OX[0], y)), module, Wake::VOCT_OUTPUT + c));
			addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(WK_OX[1], y)), module, Wake::GATE_OUTPUT + c));
			addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(WK_OX[2], y)), module, Wake::CRASH_OUTPUT + c));
			addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(WK_OX[3], y)), module, Wake::SCORE_OUTPUT + c));
			lbl->add(WK_LETTER_X, y + 0.83f, std::string(1, (char)('A' + c)));
			if (c == 0) lbl->pair(WK_STYLE_X, y, "STYLE");
			else lbl->link(WK_STYLE_X, y, WK_STYLE_X + 10.16f, y);
		}
		lbl->jack(WK_ON_X, WK_RY[0], "ON");
		lbl->jack(WK_TURN_X, WK_RY[0], "TURN");
		lbl->jackOnPlate(WK_OX[0], WK_RY[0], "V/OCT");
		lbl->jackOnPlate(WK_OX[1], WK_RY[0], "GATE");
		lbl->jackOnPlate(WK_OX[2], WK_RY[0], "CRASH");
		lbl->jackOnPlate(WK_OX[3], WK_RY[0], "SCORE");
	}

	void appendContextMenu(Menu* menu) override {
		Wake* m = dynamic_cast<Wake*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Screen",
			{"Flat: the arena seen from straight above", "3D: the arena in perspective, trails drawn as walls"}, &m->view));
		menu->addChild(createIndexSubmenuItem("Grid shape (a change clears the arena)",
			{"Square: four headings, turns of 90 degrees",
			 "Hex: six headings, turns of 60 degrees",
			 "Triangle: three neighbours, so every move is a turn"},
			[=]() { return (size_t)m->shape; },
			[=](size_t i) { m->shapeReq = (int)i; }));
		std::vector<std::string> sizes;
		for (int i = 0; i < WK_NSIZE; i++) sizes.push_back(string::f("%d x %d cells", WK_COLS[i], wkRowsFor(m->shape, WK_COLS[i])));
		menu->addChild(createIndexSubmenuItem("Grid size (a change clears the arena)", sizes,
			[=]() { return (size_t)m->sizeIdx; },
			[=](size_t i) { m->sizeReq = (int)i; }));
		menu->addChild(createIndexSubmenuItem("Edges",
			{"Solid: a cycle that reaches an edge crashes",
			 "Open: a cycle that leaves one edge comes in at the opposite edge"},
			[=]() { return (size_t)(m->wrapEdges ? 1 : 0); },
			[=](size_t i) { m->wrapEdges = i == 1; }));
		std::vector<std::string> ivs;
		for (int i = 0; i < WK_NIV; i++) ivs.push_back(WK_IV[i].name);
		menu->addChild(createIndexPtrSubmenuItem("Interval for each heading (NOTES on Turns or Every cell)", ivs, &m->ivIdx));
		sfs::rootCvMenu(menu, m, Wake::ROOT_INPUT, m->rootCvMode);
		menu->addChild(createIndexSubmenuItem("After a crash, the cycle starts again",
			{"On the next step", "After 1 step", "After 2 steps", "After 4 steps", "After 8 steps"},
			[=]() { int d = m->respawnDelay; return (size_t)(d <= 0 ? 0 : d == 1 ? 1 : d == 2 ? 2 : d <= 4 ? 3 : 4); },
			[=](size_t i) { static const int D[5] = {0, 1, 2, 4, 8}; m->respawnDelay = D[i]; }));
		menu->addChild(createIndexPtrSubmenuItem("Gate and crash length", {"Trigger (5 ms)", "Half a step"}, &m->gateMode));
		menu->addChild(createIndexSubmenuItem("SCORE above 10 V",
			{"Goes back to 0 V and counts up again", "Stays at 10 V"},
			[=]() { return (size_t)(m->scoreWrap ? 0 : 1); },
			[=](size_t i) { m->scoreWrap = i == 0; }));
		menu->addChild(createMenuItem("Clear the arena and the scores", "", [=]() { m->clearReq = true; }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Right-click a cycle's name on the screen for its style and speed"));
		menu->addChild(createMenuLabel("Click the screen, then Z turns a Keys cycle left and X right"));
		menu->addChild(createMenuLabel("A second Keys cycle turns with , and ."));
	}
};

Model* modelWake = createModel<Wake, WakeWidget>("Wake");

// Drives the REAL Wake -- src/wake.cpp compiled as-is against libRack -- and
// checks what docs/wake-design.md says must be true: a turn adds its heading's
// interval and V/OCT folds the total into RANGE octaves, the GATE fires on each
// turn and each start, a crash fires CRASH, removes the trail and starts the
// cycle again after the delay, a crash into another cycle's trail scores for
// that cycle, two cycles into one cell both crash, TRAIL limits the trail and
// a tail that leaves a cell lets a head in on the same step, open edges wrap,
// TURN turns, Keys never turns back, CLOCK steps once per pulse, RESET clears,
// and the automatic styles live as long as they should. Then the cost.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/wake-harness.cpp -o /tmp/wake-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/wake-harness
#include "../src/wake.cpp"
#include <chrono>
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static const int PER = 4800;      // a clock pulse every 100 ms
static int fails = 0;
static void check(bool ok, const std::string& what) {
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) fails++;
}

struct Rig {
	Wake m;
	rack::engine::Module::ProcessArgs a;
	int64_t frame = 0;
	bool wasG[WK_N] = {}, wasC[WK_N] = {};
	int gates[WK_N] = {}, crashes[WK_N] = {};
	Rig(bool clocked = true) {
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		if (clocked) sfs::previewConnect(m.inputs[Wake::CLOCK_INPUT], 1);
		for (int o = 0; o < Wake::OUTPUTS_LEN; o++) sfs::previewConnect(m.outputs[o], 1);
		run(16);
	}
	void run(int n) {
		for (int i = 0; i < n; i++) {
			a.frame = frame++;
			m.process(a);
			for (int c = 0; c < WK_N; c++) {
				bool g = m.outputs[Wake::GATE_OUTPUT + c].getVoltage() > 5.f;
				bool k = m.outputs[Wake::CRASH_OUTPUT + c].getVoltage() > 5.f;
				if (g && !wasG[c]) gates[c]++;
				if (k && !wasC[c]) crashes[c]++;
				wasG[c] = g; wasC[c] = k;
			}
		}
	}
	void clear() { for (int c = 0; c < WK_N; c++) gates[c] = crashes[c] = 0; }
	void clock(int per = PER) {
		m.inputs[Wake::CLOCK_INPUT].setVoltage(10.f); run(48);
		m.inputs[Wake::CLOCK_INPUT].setVoltage(0.f); run(per - 48);
	}
	void clocks(int n) { for (int i = 0; i < n; i++) clock(); }
	void only(std::initializer_list<int> on) {
		for (int c = 0; c < WK_N; c++) m.params[Wake::ON_PARAM + c].setValue(0.f);
		for (int c : on) m.params[Wake::ON_PARAM + c].setValue(1.f);
		run(4);
	}
	void style(int c, int s) { m.params[Wake::STYLE_PARAM + c].setValue((float)s); run(2); }
	int owned(int c) {
		int n = 0;
		for (int i = 0; i < m.W * m.H; i++) if (m.owner[i] == c + 1) n++;
		return n;
	}
	float voct(int c) { return m.outputs[Wake::VOCT_OUTPUT + c].getVoltage(); }
	// the key press that turns cycle c toward heading d at its next move
	void turnTo(int c, int d) {
		int h = m.cyc[c].dir;
		if (d == wkRight(h)) m.pushKey(c, 1);
		else if (d == wkLeft(h)) m.pushKey(c, -1);
	}
};

static double q0cells(int sh) { return 64.0 * wkRowsFor(sh, 64); }
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
	rack::random::init();

	printf("== pitch ==\n");
	{
		Rig r;
		r.only({0});
		r.style(0, WK_KEYS);
		r.clock();                           // the first step starts every cycle that is on
		r.m.place(0, 10, 10, WK_DE);
		r.clear();
		r.turnTo(0, WK_DN); r.clock();
		check(r.m.cyc[0].acc == 12 && near(r.voct(0), 1.f), string::f("turning north adds 12: %d, %.4f V", r.m.cyc[0].acc, r.voct(0)));
		r.turnTo(0, WK_DW); r.clock();
		check(r.m.cyc[0].acc == 5 && near(r.voct(0), 5.f / 12.f), string::f("then west adds -7: %d, %.4f V", r.m.cyc[0].acc, r.voct(0)));
		r.turnTo(0, WK_DS); r.clock();
		check(r.m.cyc[0].acc == -7 && near(r.voct(0), 17.f / 12.f), string::f("then south adds -12, folded into 2 octaves: %.4f V", r.voct(0)));
		r.clock(); r.clock();
		check(r.gates[0] == 3 && r.m.cyc[0].acc == -7, string::f("going straight holds the pitch; one gate per turn (%d)", r.gates[0]));
		r.m.params[Wake::RANGE_PARAM].setValue(1.f); r.run(2);
		check(near(r.voct(0), 5.f / 12.f), string::f("RANGE 1 folds into one octave: %.4f V", r.voct(0)));
		r.m.pushKey(0, -1); r.m.pushKey(0, -1); r.clock(); r.clock();
		check(r.m.cyc[0].dir == WK_DN && r.m.cyc[0].alive, "two lefts queue and make a U-turn over two moves");
		r.m.params[Wake::RANGE_PARAM].setValue(2.f);
		r.m.params[Wake::NOTES_PARAM].setValue((float)WK_NT_CELLS); r.run(2);
		r.m.place(0, 5, 5, WK_DE);
		r.clear();
		r.clocks(3);
		check(r.m.cyc[0].acc == 21 && near(r.voct(0), 21.f / 12.f) && r.gates[0] == 3,
		      string::f("added at every cell: three moves east are +21 (%d), three gates (%d)", r.m.cyc[0].acc, r.gates[0]));
		r.m.params[Wake::NOTES_PARAM].setValue(0.f);
	}

	printf("== key, scale and notes ==\n");
	{
		Rig r;
		r.only({0});
		r.style(0, WK_KEYS);
		r.m.params[Wake::SCALE_PARAM].setValue(1.f);      // Major
		r.m.params[Wake::ROOT_PARAM].setValue(2.f);       // D
		r.clock();
		r.m.place(0, 10, 10, WK_DE);
		r.run(2);
		check(near(r.voct(0), 2.f / 12.f), string::f("a new life starts on the root: D is %.4f V", r.voct(0)));
		r.turnTo(0, WK_DN); r.clock();
		r.turnTo(0, WK_DE); r.clock();
		check(r.m.cyc[0].acc == 19 && near(r.voct(0), (2.f + 19.f) / 12.f),
		      string::f("Turns: +12 then +7 is 19 semitones above D: %.4f V", r.voct(0)));
		r.turnTo(0, WK_DS); r.clock();
		r.turnTo(0, WK_DE); r.clock();
		check(r.m.cyc[0].acc == 14 && near(r.voct(0), (2.f + 14.f) / 12.f),
		      string::f("-12 then +7 is 14, which C major has: %.4f V", r.voct(0)));
		r.m.params[Wake::SCALE_PARAM].setValue(4.f);      // Pentatonic Minor, 0 3 5 7 10
		r.m.params[Wake::ROOT_PARAM].setValue(0.f);
		r.m.ivIdx = 5;                                    // N +2, E +1, S -2, W -1
		r.m.place(0, 10, 10, WK_DE);
		r.turnTo(0, WK_DN); r.clock();
		check(near(r.voct(0), 3.f / 12.f), string::f("the total is snapped to the scale: +2 in Pentatonic Minor plays 3 (%.4f V)", r.voct(0)));
		r.m.ivIdx = 0;

		// Steps: right up a degree, left down one
		r.m.params[Wake::SCALE_PARAM].setValue(1.f);
		r.m.params[Wake::RANGE_PARAM].setValue(1.f);
		r.m.params[Wake::NOTES_PARAM].setValue((float)WK_NT_STEPS); r.run(2);
		r.m.place(0, 30, 10, WK_DE);
		r.clear();
		r.turnTo(0, WK_DS); r.clock();
		float v1 = r.voct(0);
		r.turnTo(0, WK_DW); r.clock();
		float v2 = r.voct(0);
		r.turnTo(0, WK_DS); r.clock();
		float v3 = r.voct(0);
		r.clock();
		check(near(v1, 2.f / 12.f) && near(v2, 4.f / 12.f) && near(v3, 2.f / 12.f) && r.gates[0] == 3,
		      string::f("Steps: right, right, left plays D E D in C major (%.3f %.3f %.3f), a gate a turn (%d)", v1 * 12, v2 * 12, v3 * 12, r.gates[0]));
		for (int i = 0; i < 6; i++) r.m.setNote(0, 1, false);   // six more rights: degree 7, one octave
		r.run(2);
		check(near(r.voct(0), 0.f), string::f("and wraps at RANGE: degree 7 of a 1-octave range is the root again (%.4f V)", r.voct(0)));

		// Column: low at the left, high at the right
		r.m.params[Wake::NOTES_PARAM].setValue((float)WK_NT_COLUMN); r.run(2);
		r.clear();
		r.m.place(0, 0, 5, WK_DE);
		r.run(2);
		float vL = r.voct(0);
		r.clocks(r.m.W - 1);
		check(near(vL, 0.f) && near(r.voct(0), 1.f) && r.gates[0] == 8,
		      string::f("Column: the left edge is the root, the right edge an octave up (%.4f V); a gate per new note (%d)", r.voct(0), r.gates[0]));
		r.m.params[Wake::NOTES_PARAM].setValue((float)WK_NT_ROW); r.run(2);
		r.m.place(0, 10, r.m.H - 1, WK_DN);
		r.run(2);
		float vB = r.voct(0);
		r.clocks(r.m.H - 1);
		check(near(vB, 0.f) && near(r.voct(0), 1.f), string::f("Row: the bottom is the root, the top an octave up (%.4f V)", r.voct(0)));

		// NOTES CV, 1 V per mode
		sfs::previewConnect(r.m.inputs[Wake::NOTES_INPUT], 1);
		r.m.params[Wake::NOTES_PARAM].setValue(0.f);
		r.m.inputs[Wake::NOTES_INPUT].setVoltage(3.f); r.run(2);
		check(r.m.curNotes == WK_NT_COLUMN, "NOTES CV: 3 V is the fourth mode, Column");
		r.m.inputs[Wake::NOTES_INPUT].setVoltage(0.f); r.run(2);
		// ROOT CV, 1V/oct
		sfs::previewConnect(r.m.inputs[Wake::ROOT_INPUT], 1);
		r.m.inputs[Wake::ROOT_INPUT].setVoltage(7.f / 12.f); r.run(2);
		check(r.m.root == 7, string::f("ROOT CV at 7/12 V is G (%d)", r.m.root));
	}
	{
		// Room: open space plays high, an enclosed pocket low
		Rig r;
		r.only({0});
		r.style(0, WK_KEYS);
		r.m.params[Wake::RANGE_PARAM].setValue(1.f);
		r.m.params[Wake::SCALE_PARAM].setValue(1.f);
		r.m.params[Wake::NOTES_PARAM].setValue((float)WK_NT_ROOM); r.run(2);
		r.clock();
		r.m.place(0, 40, r.m.H - 2, WK_DN);
		r.clocks(2);
		int open = r.m.cyc[0].deg;
		for (int y = 0; y < r.m.H; y++) r.m.owner[y * r.m.W + 8] = 4;   // a wall down column 8, held by D
		r.m.place(0, 3, r.m.H - 2, WK_DN);
		r.clocks(2);
		int pocket = r.m.cyc[0].deg;
		check(open >= 6 && pocket <= 1, string::f("Room: degree %d in the open arena, %d in an 8-column pocket", open, pocket));
	}
	{
		// Chase: close plays high
		Rig r;
		r.only({0, 1});
		r.style(0, WK_KEYS); r.style(1, WK_KEYS);
		r.m.params[Wake::RANGE_PARAM].setValue(1.f);
		r.m.params[Wake::NOTES_PARAM].setValue((float)WK_NT_CHASE); r.run(2);
		r.clock();
		r.m.place(0, 5, 2, WK_DE); r.m.place(1, 5, r.m.H - 2, WK_DE);
		r.clock();
		int far = r.m.cyc[0].deg;
		r.m.place(0, 5, 10, WK_DE); r.m.place(1, 5, 12, WK_DE);
		r.clock();
		int close = r.m.cyc[0].deg;
		check(close >= 6 && far < close, string::f("Chase: degree %d far apart, %d two cells apart", far, close));
	}

	printf("== crashes ==\n");
	{
		Rig r;
		r.only({0});
		r.style(0, WK_KEYS);
		r.m.respawnDelay = 2;
		r.clock();
		r.m.place(0, r.m.W - 3, 4, WK_DE);
		r.clear();
		r.clocks(2);
		check(r.m.cyc[0].alive && r.owned(0) == 3, "two moves to the last column");
		r.clock();
		check(!r.m.cyc[0].alive && r.crashes[0] == 1 && r.owned(0) == 0,
		      string::f("off the edge: CRASH fires (%d) and the trail is removed (%d cells left)", r.crashes[0], r.owned(0)));
		r.clock(); r.clock();
		check(!r.m.cyc[0].alive, "it waits two steps");
		r.clear();
		r.clock();
		check(r.m.cyc[0].alive && r.m.cyc[0].acc == 0 && near(r.voct(0), 0.f) && r.gates[0] == 1,
		      "then starts again at 0 V, with a gate");
	}
	{
		Rig r;
		r.only({0, 1});
		r.style(0, WK_KEYS); r.style(1, WK_STRAIGHT);
		r.clock();
		r.m.place(1, 20, 10, WK_DS);        // B drives down column 20
		r.m.place(0, 17, 12, WK_DE);        // A drives east into it
		long b0 = r.m.cyc[1].score;
		r.clear();
		r.clocks(3);
		check(!r.m.cyc[0].alive && r.m.cyc[0].killer == 1 && r.m.cyc[1].score == b0 + 3 + WK_KILL,
		      string::f("A into B's trail: A crashes and B scores %d for it", WK_KILL));
	}
	{
		Rig r;
		r.only({0, 1});
		r.style(0, WK_KEYS); r.style(1, WK_KEYS);
		r.clock();
		r.m.place(0, 10, 10, WK_DE);
		r.m.place(1, 12, 10, WK_DW);
		long a0 = r.m.cyc[0].score, b0 = r.m.cyc[1].score;
		r.clear();
		r.clock();
		check(!r.m.cyc[0].alive && !r.m.cyc[1].alive && r.crashes[0] == 1 && r.crashes[1] == 1
		      && r.m.cyc[0].score == a0 && r.m.cyc[1].score == b0,
		      "two cycles into one cell both crash, and neither scores");
	}

	printf("== trail ==\n");
	{
		Rig r;
		r.only({0});
		r.style(0, WK_KEYS);
		r.m.params[Wake::TRAIL_PARAM].setValue(3.f / 8.f);   // 4 * 2^3 = 32 cells
		r.m.wrapEdges = true;
		r.clock();
		r.m.place(0, 0, 3, WK_DE);
		r.clocks(80);
		check(r.m.cyc[0].alive && r.m.cyc[0].tLen == 32 && r.owned(0) == 32,
		      string::f("TRAIL at 32 cells keeps 32 (%d); with open edges it wraps round and lives", r.owned(0)));
		r.m.params[Wake::TRAIL_PARAM].setValue(0.f);          // 4 cells
		r.m.wrapEdges = false;
		r.clock();
		r.m.place(0, 10, 10, WK_DE);
		r.clock();
		r.turnTo(0, WK_DS); r.clock();
		r.turnTo(0, WK_DW); r.clock();
		r.turnTo(0, WK_DN); r.clock();
		bool ok = true;
		for (int lap = 0; lap < 6; lap++) {
			r.turnTo(0, WK_DE); r.clock();
			r.turnTo(0, WK_DS); r.clock();
			r.turnTo(0, WK_DW); r.clock();
			r.turnTo(0, WK_DN); r.clock();
			ok &= r.m.cyc[0].alive;
		}
		check(ok && r.owned(0) == 4, "a 4-cell trail can circle a 2 x 2 square: the tail leaves the cell the head enters");
	}

	printf("== TURN, CLOCK, RESET ==\n");
	{
		Rig r;
		r.only({0});
		r.style(0, WK_STRAIGHT);
		sfs::previewConnect(r.m.inputs[Wake::TURN_INPUT + 0], 1);
		r.clock();
		r.m.place(0, 30, 14, WK_DE);
		r.m.inputs[Wake::TURN_INPUT].setVoltage(10.f); r.run(20);
		r.m.inputs[Wake::TURN_INPUT].setVoltage(0.f); r.run(20);
		r.clock();
		check(r.m.cyc[0].dir == WK_DN || r.m.cyc[0].dir == WK_DS, "a TURN trigger turns a Straight cycle at its next move");
		long t0 = r.m.tickN;
		r.run(PER * 5);
		check(r.m.tickN == t0, "with CLOCK patched and no pulses, nothing moves");
		r.clocks(5);
		check(r.m.tickN == t0 + 5, "one step per pulse");
		r.m.cyc[0].score = 1234;
		sfs::previewConnect(r.m.inputs[Wake::RESET_INPUT], 1);
		r.m.inputs[Wake::RESET_INPUT].setVoltage(10.f); r.run(20);
		r.m.inputs[Wake::RESET_INPUT].setVoltage(0.f); r.run(20);
		check(r.m.cyc[0].score == 0 && r.owned(0) == 0 && !r.m.cyc[0].alive, "RESET clears the arena and the scores");
		r.clock();
		check(r.m.cyc[0].alive, "and the next step starts the cycles again");
	}
	{
		Rig r(false);
		r.only({0});
		r.m.params[Wake::RATE_PARAM].setValue(4.f / 6.f);   // 8 steps/s
		long t0 = r.m.tickN;
		r.run((int)SR * 2);
		check(r.m.tickN - t0 == 16, string::f("unclocked, RATE 8 steps/s makes 16 steps in 2 s (%ld)", r.m.tickN - t0));
	}

	printf("== styles alone, 64 x 26, no trail limit ==\n");
	{
		const int ST[] = {WK_STRAIGHT, WK_CAREFUL, WK_HUNTER, WK_RANDOM, WK_HUGGER, WK_ZIGZAG, WK_SPIRAL};
		for (int st : ST) {
			Rig r;
			r.only({0});
			r.style(0, st);
			r.m.respawnDelay = 0;
			r.clear();
			for (int i = 0; i < 6000; i++) r.m.doTick();
			long cr = r.m.cyc[0].crashes;
			// finished lives only: the one still running is not counted
			double life = cr ? (double)(r.m.cyc[0].score - r.m.cyc[0].life) / cr : 6000.0;
			printf("    %-9s %4ld crashes, %6.1f cells a life\n", WK_STYLE_NAME[st], cr, life);
			if (st == WK_CAREFUL) check(life > 400.0, "Careful alone fills most of the arena before it crashes");
			if (st == WK_HUGGER) check(life > 400.0, "Hugger alone fills most of the arena before it crashes");
			if (st == WK_RANDOM) check(life < 400.0, "Random crashes sooner than Careful");
		}
	}

	printf("== grid shapes ==\n");
	for (int sh = WK_HEX; sh <= WK_TRI; sh++) {
		const char* nm = sh == WK_HEX ? "Hex" : "Triangle";
		Rig r;
		r.m.shapeReq = sh; r.run(2);
		check(r.m.shape == sh && r.m.H == wkRowsFor(sh, r.m.W) && r.m.H % 2 == 0,
		      string::f("%s: the grid is %d x %d, an even number of rows", nm, r.m.W, r.m.H));
		// every move is one unit long and the opposite heading leads back, also across open edges
		bool unit = true, back = true, three = true, inside = true;
		for (int wrap = 0; wrap < 2; wrap++) {
			r.m.wrapEdges = wrap;
			for (int y = 0; y < r.m.H; y++)
				for (int x = 0; x < r.m.W; x++) {
					int edges = 0;
					for (int d = 0; d < 6; d++) {
						int nx, ny, bx, by;
						if (!r.m.stepFrom(x, y, d, nx, ny)) continue;
						edges++;
						if (!wrap && std::fabs(r.m.dist(x, y, nx, ny) - 1.f) > 1e-4f) unit = false;
						if (!r.m.stepFrom(nx, ny, r.m.back(d), bx, by) || bx != x || by != y) back = false;
						float wx = r.m.wxOf(x, y), wy = r.m.wyOf(x, y);
						if (wx < 0.f || wy < 0.f || wx > r.m.worldW() || wy > r.m.worldH()) inside = false;
					}
					if (wrap && sh == WK_TRI && edges != 3) three = false;
					if (wrap && sh == WK_HEX && edges != 6) three = false;
				}
		}
		r.m.wrapEdges = false;
		check(unit && back && inside, string::f("%s: neighbours are 1 apart, the opposite heading leads back (open edges too), cells sit inside the floor", nm));
		check(three, string::f("%s: with open edges every cell has %d neighbours", nm, sh == WK_TRI ? 3 : 6));

		// Straight keeps a line: on Triangle that is two headings in turn
		r.only({0});
		r.style(0, WK_STRAIGHT);
		r.clock();
		int cx = r.m.W / 2, cy = r.m.H / 2;
		int d0 = sh == WK_TRI ? (((cx + cy) & 1) ? 1 : 2) : 1;
		r.m.place(0, cx, cy, d0);
		int seen[6] = {};
		for (int i = 0; i < 8; i++) { r.clock(); seen[r.m.cyc[0].dir]++; }
		int kinds = 0;
		for (int d = 0; d < 6; d++) kinds += seen[d] > 0;
		check(r.m.cyc[0].alive && kinds == (sh == WK_TRI ? 2 : 1),
		      string::f("%s: Straight holds %s (%d headings over 8 moves)", nm, sh == WK_TRI ? "two headings in turn" : "one heading", kinds));
		// one key press turns the line by 60 degrees
		r.style(0, WK_KEYS);
		r.m.place(0, cx, cy, d0);
		r.clock(); r.clock(); r.clock();
		int before[6] = {};
		for (int i = 0; i < 4; i++) { r.clock(); before[r.m.cyc[0].dir]++; }
		r.m.pushKey(0, 1);
		r.clock(); r.clock();
		int after[6] = {};
		for (int i = 0; i < 4; i++) { r.clock(); after[r.m.cyc[0].dir]++; }
		bool rotated = true;
		for (int d = 0; d < 6; d++) if ((before[d] > 0) != (after[(d + 1) % 6] > 0)) rotated = false;
		check(r.m.cyc[0].alive && rotated, string::f("%s: one press of X turns the line 60 degrees right", nm));

		// Every cell: a cell's total does not depend on the path to it
		r.m.params[Wake::NOTES_PARAM].setValue((float)WK_NT_CELLS); r.run(2);
		bool pathFree = true;
		for (int iv = 0; iv < WK_NIV; iv++) {
			r.m.ivIdx = iv;
			for (int d = 0; d < 6; d++) {
				int a = r.m.interval(d), b = r.m.interval((d + 1) % 6), c = r.m.interval((d + 2) % 6);
				// two sides of a unit triangle of headings: d then d + 2 is d + 1 (on both grids)
				if (a + c != b || r.m.interval(r.m.back(d)) != -a) pathFree = false;
			}
		}
		r.m.ivIdx = 2;
		check(pathFree && r.m.interval(1) == 4 && r.m.interval(0) == 7 && r.m.interval(5) == 3,
		      string::f("%s: the heading intervals add up the same along any path; fifth and major third gives E +4, 60 degrees round +7, 120 round +3", nm));
		r.m.ivIdx = 0;
		r.m.params[Wake::NOTES_PARAM].setValue(0.f); r.run(2);

		printf("  %s, styles alone, %d x %d, no trail limit:\n", nm, r.m.W, r.m.H);
		const int ST[] = {WK_STRAIGHT, WK_CAREFUL, WK_HUNTER, WK_RANDOM, WK_HUGGER, WK_ZIGZAG, WK_SPIRAL};
		double careful = 0, rnd = 0;
		for (int st : ST) {
			Rig q;
			q.m.shapeReq = sh; q.run(2);
			q.only({0});
			q.style(0, st);
			q.m.respawnDelay = 0;
			for (int i = 0; i < 6000; i++) q.m.doTick();
			long cr = q.m.cyc[0].crashes;
			double life = cr ? (double)(q.m.cyc[0].score - q.m.cyc[0].life) / cr : 6000.0;
			printf("    %-9s %4ld crashes, %6.1f cells a life\n", WK_STYLE_NAME[st], cr, life);
			if (st == WK_CAREFUL) careful = life;
			if (st == WK_RANDOM) rnd = life;
		}
		check(careful > 0.25 * q0cells(sh) && rnd < careful, string::f("%s: Careful fills much of the arena, Random crashes sooner", nm));

		Rig f;
		f.m.shapeReq = sh; f.run(2);
		f.only({0, 1, 2, 3});
		for (int i = 0; i < 20000; i++) f.m.doTick();
		bool agree = true, all = true;
		for (int c = 0; c < WK_N; c++) { agree &= f.owned(c) == f.m.cyc[c].tLen; all &= f.m.cyc[c].crashes > 0; }
		check(agree && all, string::f("%s: four together crash and restart, and the board agrees with the trails", nm));
	}

	printf("== four together ==\n");
	{
		Rig r;
		r.only({0, 1, 2, 3});
		r.m.respawnDelay = 2;
		for (int i = 0; i < 20000; i++) r.m.doTick();
		bool all = true;
		long kills = 0;
		for (int c = 0; c < WK_N; c++) {
			const Wake::Cyc& k = r.m.cyc[c];
			printf("    %c %-8s score %6ld  crashes %4ld  kills %4ld\n", 'A' + c, WK_STYLE_NAME[r.m.curStyle[c]], k.score, k.crashes, k.kills);
			all &= k.crashes > 0 && k.score > 0;
			kills += k.kills;
		}
		check(all && kills > 0, "every cycle crashes and starts again, and some crashes score for another cycle");
		// the board agrees with the trails
		bool agree = true;
		for (int c = 0; c < WK_N; c++) agree &= r.owned(c) == r.m.cyc[c].tLen;
		check(agree, "every cell on the board belongs to exactly the trail that holds it");
	}

	printf("== keys through Rack's event routing ==\n");
	{
		// KeyRoot stands where Rack's scroll widget does: it scrolls the rack with
		// the arrow keys only when nothing inside it used the key.
		struct KeyRoot : Widget {
			int reached = 0;
			void onHoverKey(const HoverKeyEvent& e) override { Widget::onHoverKey(e); if (!e.isConsumed()) reached++; }
		};
		rack::contextSet(new rack::Context);
		APP->event = new rack::widget::EventState;
		static char windowInternal[4096] = {};
		static void* fakeWindow[64] = {windowInternal};
		APP->window = reinterpret_cast<rack::window::Window*>(fakeWindow);
		KeyRoot* root = new KeyRoot;
		root->box.size = Vec(2000, 1000);
		APP->event->rootWidget = root;
		Rig r;
		r.only({0});
		r.style(0, WK_STRAIGHT);
		r.clock();
		WakeDisplay* d = new WakeDisplay;
		d->module = &r.m;
		d->box.size = Vec(600, 280);
		root->addChild(d);
		Vec on(300, 150), off(1500, 800);
		auto key = [&](Vec p, int k, int action) { APP->event->handleKey(p, k, 0, action, 0); };
		key(on, GLFW_KEY_X, GLFW_PRESS);
		check(root->reached == 1, "with no cycle on Keys, X over the screen is left to Rack");
		r.style(0, WK_KEYS);
		r.m.place(0, 20, 10, WK_DE);
		root->reached = 0;
		key(on, GLFW_KEY_Z, GLFW_PRESS); key(on, GLFW_KEY_Z, GLFW_REPEAT); key(on, GLFW_KEY_Z, GLFW_RELEASE);
		r.clock();
		check(root->reached == 0 && r.m.cyc[0].dir == WK_DN, "with A on Keys, Z over the screen turns it left, and the repeat and release stop there");
		key(on, GLFW_KEY_UP, GLFW_PRESS);
		check(root->reached == 1, "the arrow keys are left to Rack, which scrolls the rack with them");
		root->reached = 0;
		APP->event->handleButton(on, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
		APP->event->handleButton(on, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
		check(APP->event->selectedWidget == d, "a click selects the screen");
		key(off, GLFW_KEY_X, GLFW_PRESS); key(off, GLFW_KEY_X, GLFW_RELEASE);
		r.clock();
		check(root->reached == 0 && r.m.cyc[0].dir == WK_DE, "once selected, X turns it right with the pointer anywhere");
		r.only({0, 1});
		r.style(1, WK_KEYS);
		r.clock();
		r.m.place(1, 40, 10, WK_DE);
		key(off, GLFW_KEY_PERIOD, GLFW_PRESS); key(off, GLFW_KEY_PERIOD, GLFW_RELEASE);
		r.clock();
		check(root->reached == 0 && r.m.cyc[1].dir == WK_DS, ". turns the second Keys cycle right");
		APP->event->handleKey(off, GLFW_KEY_Z, 0, GLFW_PRESS, RACK_MOD_CTRL);
		check(root->reached == 1, "Ctrl+Z is left to Rack");
		root->clearChildren();
		APP->event->rootWidget = nullptr;
	}

	printf("== saving ==\n");
	{
		Rig r;
		r.m.wrapEdges = true; r.m.ivIdx = 3; r.m.view = 0; r.m.rootCvMode = sfs::ROOT_CV_SEMI; r.m.cyc[2].div = 3; r.m.cyc[1].score = 777;
		r.m.shapeReq = WK_HEX; r.run(2);
		r.m.sizeReq = 3; r.run(2);
		json_t* j = r.m.dataToJson();
		Rig q;
		q.m.dataFromJson(j);
		json_decref(j);
		q.run(2);
		check(q.m.shape == WK_HEX, "the grid shape is saved");
		check(q.m.wrapEdges && q.m.ivIdx == 3 && q.m.view == 0 && q.m.rootCvMode == sfs::ROOT_CV_SEMI && q.m.cyc[2].div == 3 && q.m.cyc[1].score == 777
		      && q.m.W == 96 && q.m.H == wkRowsFor(WK_HEX, 96), string::f("settings, speeds, scores and the grid (%d x %d) are saved", q.m.W, q.m.H));
	}

	printf("== cost ==\n");
	for (int sh = 0; sh < WK_NSHAPE; sh++) {
		Rig r(false);
		r.only({0, 1, 2, 3});
		r.m.shapeReq = sh; r.run(2);
		r.m.sizeReq = 3;
		r.m.params[Wake::RATE_PARAM].setValue(1.f);      // 32 steps/s, the top of RATE
		r.run(4);
		int n = (int)SR * 20;
		auto t0 = std::chrono::steady_clock::now();
		r.run(n);
		double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		double pct = 100.0 * sec / (n / SR);
		static const char* NM[3] = {"Square", "Hex", "Triangle"};
		printf("  %.2f%% of one core, four cycles on %s 96 x %d at 32 steps/s\n", pct, NM[sh], r.m.H);
		check(pct < 1.5, "under 1.5% of a core");
	}
	{
		// the worst single step: every cycle looks at three moves on an empty board
		Rig w(false);
		w.only({0, 1, 2, 3});
		w.m.sizeReq = 3; w.run(2);
		for (int c = 0; c < WK_N; c++) w.style(c, WK_CAREFUL);
		w.m.doTick();
		double worst = 0;
		for (int i = 0; i < 200; i++) {
			auto s0 = std::chrono::steady_clock::now();
			w.m.doTick();
			worst = std::max(worst, std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count());
		}
		printf("  longest step %.1f us (a sample at 48 kHz is 20.8 us; a step happens once in thousands)\n", worst * 1e6);
	}

	printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
	return fails ? 1 : 0;
}

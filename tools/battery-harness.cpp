// Drives the REAL Battery -- src/battery.cpp compiled as-is against libRack --
// and checks what docs/battery-design.md says must be true: every play order
// visits the steps it should, serialist plays every step once per shuffle and
// never starts a shuffle on the step the last one ended on, CHANCE and ROUTE
// fire at the rate they say, BIAS moves the route, LOCK repeats a pass exactly
// and NEW changes it, ratchets and lane rates land inside the clock, a pattern
// change waits for the end of the longest lane (or BAR), NEXT skips empty
// patterns, MUTE and RUN silence, RESET starts again, and the outputs carry
// lane N on channel N (A) and N + 8 (B).
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/battery-harness.cpp -o /tmp/battery-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/battery-harness
#include "../src/battery.cpp"
#include <chrono>
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static const int PER = 6000;           // a clock pulse every 125 ms: sixteenths at 120 BPM
static int fails = 0;
static void check(bool ok, const std::string& what) {
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) fails++;
}

struct Rig {
	Battery m;
	rack::engine::Module::ProcessArgs a;
	int64_t frame = 0;
	bool was[16] = {};
	int hits[16] = {};          // rising edges per GATE channel since the last clear
	int lows = 0;               // samples a channel spent low between two of its hits
	Rig() {
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		for (int p = 0; p < BT_P; p++) m.pat[p] = BtPattern();
		sfs::previewConnect(m.inputs[Battery::CLOCK_INPUT], 1);
		for (int o = 0; o < Battery::OUTPUTS_LEN; o++) sfs::previewConnect(m.outputs[o], 1);
		// a Rack trigger read high on its first sample does not fire, so every
		// input starts low, as it does in a patch
		run(16);
	}
	void run(int n) {
		for (int i = 0; i < n; i++) {
			a.frame = frame++;
			m.process(a);
			for (int c = 0; c < 16; c++) {
				bool h = m.outputs[Battery::GATE_OUTPUT].getVoltage(c) > 5.f;
				if (h && !was[c]) hits[c]++;
				was[c] = h;
			}
		}
	}
	void clear() { for (int c = 0; c < 16; c++) hits[c] = 0; }
	// one clock pulse and the time to the next
	void clock(int per = PER) {
		m.inputs[Battery::CLOCK_INPUT].setVoltage(10.f);
		run(48);
		m.inputs[Battery::CLOCK_INPUT].setVoltage(0.f);
		run(per - 48);
	}
	// the channels that fired during one clock, as a mask
	unsigned tick(int per = PER) {
		clear();
		clock(per);
		unsigned g = 0;
		for (int c = 0; c < 16; c++) if (hits[c]) g |= 1u << c;
		return g;
	}
	BtLane& lane(int l, int p = 0) { return m.pat[p].lane[l]; }
	void fill(int l, int len, int p = 0) {
		BtLane& ln = lane(l, p);
		ln.len = len;
		for (int i = 0; i < len; i++) ln.st[i].on = true;
	}
	void trig(int in) {
		m.inputs[in].setVoltage(10.f); run(48);
		m.inputs[in].setVoltage(0.f); run(48);
	}
	void connect(int in, int ch = 1) { sfs::previewConnect(m.inputs[in], ch); run(4); }
	// the positions one lane visits over n clocks
	std::string positions(int l, int n) {
		std::string s;
		for (int i = 0; i < n; i++) { clock(); s += (char)('0' + m.run[l].pos); }
		return s;
	}
};

static bool isPerm(const std::string& s, int L) {
	if ((int)s.size() != L) return false;
	unsigned seen = 0;
	for (char c : s) { int v = c - '0'; if (v < 0 || v >= L || (seen & (1u << v))) return false; seen |= 1u << v; }
	return true;
}

int main() {
	rack::random::init();

	printf("== a new module, and the templates ==\n");
	{
		Battery fresh;
		bool blank = true;
		for (int p = 0; p < BT_P; p++) blank &= !fresh.pat[p].used();
		check(blank, "a new module has no steps");
		std::vector<BtTemplate> T = btLoadTemplates("res/patterns/drum-patterns-v2.json");
		size_t want = 0;
		for (const BtTplGroup& g : btTplGroups()) want += g.ids.size();
		int sets4 = 0, ratch = 0, chance = 0;
		bool ok = true;
		for (const BtTemplate& t : T) {
			ok &= t.mainIdx >= 0 && t.roles[t.mainIdx].used();
			if (t.roles.size() == 4) sets4++;
			for (const BtPattern& p : t.roles)
				for (int l = 0; l < BT_L; l++)
					for (int i = 0; i < BT_S; i++) { ratch += p.lane[l].st[i].ratch > 1; chance += p.lane[l].st[i].chance < 1.f; }
		}
		check(T.size() == want && ok, string::f("all %d templates read, each with a main groove (%d with four variations)", (int)T.size(), sets4));
		check(ratch > 0, string::f("Fill's ratchets come across (%d steps)", ratch));
		// the shuffle loads at 12 steps, the others at 16
		bool lens = true;
		for (const BtTemplate& t : T) {
			int L = t.roles[t.mainIdx].lane[0].len;
			lens &= (t.id == "rock.shuffle") ? L == 12 : L == 16;
		}
		check(lens, "lane lengths follow the grid: 12 for the 12/8 shuffle, 16 for the rest");
		Rig r;
		for (const BtTemplate& t : T) if (t.id == "rock.basic") r.m.pat[0] = t.roles[t.mainIdx];
		unsigned g0 = r.tick();
		check(g0 & 0x1u, string::f("Basic rock plays a kick on the first step (%04x)", g0));
	}

	printf("== play orders over a four-step lane ==\n");
	{
		struct T { int mode; const char* want; };
		static const T Ts[] = {
			{BT_FWD,  "012301230123"},
			{BT_BACK, "321032103210"},
			{BT_PEND, "012321012321"},
			{BT_PING, "012332100123"},
		};
		for (const T& t : Ts) {
			Rig r;
			r.fill(0, 4);
			r.lane(0).mode = t.mode;
			std::string got = r.positions(0, 12);
			check(got == t.want, std::string(BT_MODE_SHORT[t.mode]) + ": " + got + " (want " + t.want + ")");
		}
		Rig r;
		r.fill(0, 6);
		r.lane(0).mode = BT_RAND;
		std::string got = r.positions(0, 60);
		bool inRange = true; unsigned seen = 0;
		for (char c : got) { int v = c - '0'; inRange &= v >= 0 && v < 6; seen |= 1u << v; }
		check(inRange && seen == 0x3F, "RAND stays in the lane and reaches every step: " + got.substr(0, 24) + "...");
		Rig d;
		d.fill(0, 8);
		d.lane(0).mode = BT_DRUNK;
		got = d.positions(0, 40);
		bool walk = true;
		for (size_t i = 1; i < got.size(); i++) {
			int a = got[i - 1] - '0', b = got[i] - '0';
			int dd = ((b - a) % 8 + 8) % 8;
			walk &= (dd == 1 || dd == 7);
		}
		check(walk, "DRNK moves one step either way every step: " + got.substr(0, 24) + "...");
	}

	printf("== serialist ==\n");
	{
		Rig r;
		r.fill(0, 8);
		r.lane(0).mode = BT_SERIAL;
		std::string got = r.positions(0, 8 * 12);
		bool perms = true, joins = true, differ = false;
		for (int p = 0; p < 12; p++) {
			std::string block = got.substr(p * 8, 8);
			perms &= isPerm(block, 8);
			if (p > 0) {
				joins &= got[p * 8] != got[p * 8 - 1];
				differ |= block != got.substr(0, 8);
			}
		}
		check(perms, "every pass of 8 plays all 8 steps once: " + got.substr(0, 32) + "...");
		check(joins, "a new shuffle never starts on the step the last one ended on");
		check(differ, "unlocked, the shuffle changes from pass to pass");
		r.m.params[Battery::LOCK_PARAM].setValue(1.f);
		r.positions(0, 8 - (int)(r.m.run[0].count % 8) - 1);   // finish the pass in progress
		std::string a = r.positions(0, 8), b = r.positions(0, 8), c = r.positions(0, 8);
		check(isPerm(a, 8) && a == b && b == c, "LOCK repeats one shuffle: " + a + " " + b + " " + c);
		r.m.params[Battery::NEW_PARAM].setValue(1.f); r.run(48); r.m.params[Battery::NEW_PARAM].setValue(0.f); r.run(48);
		std::string n1 = r.positions(0, 8), n2 = r.positions(0, 8);
		check(isPerm(n1, 8) && n1 == n2 && n1 != a, "NEW gives a different shuffle, still locked: " + n1);
	}

	printf("== chance, route and bias ==\n");
	{
		Rig r;
		r.fill(0, 16);
		for (int i = 0; i < 16; i++) r.lane(0).st[i].chance = 0.25f;
		int fired = 0, N = 800;
		for (int i = 0; i < N; i++) if (r.tick() & 0x101u) fired++;
		check(fired > N * 0.20 && fired < N * 0.30, string::f("CHANCE 25%%: %d of %d steps fired", fired, N));

		Rig a;
		a.fill(0, 16);
		bool allA = true;
		for (int i = 0; i < 32; i++) allA &= a.tick() == 0x1u;
		check(allA, "ROUTE 0: every hit on channel 1");
		for (int i = 0; i < 16; i++) a.lane(0).st[i].route = 1.f;
		bool allB = true;
		for (int i = 0; i < 32; i++) allB &= a.tick() == 0x100u;
		check(allB, "ROUTE 100%: every hit on channel 9");
		for (int i = 0; i < 16; i++) a.lane(0).st[i].route = 0.5f;
		int nb = 0, both = 0;
		for (int i = 0; i < N; i++) { unsigned g = a.tick(); if (g & 0x100u) nb++; if ((g & 0x101u) == 0x101u) both++; }
		check(nb > N * 0.44 && nb < N * 0.56 && both == 0, string::f("ROUTE 50%%: %d of %d on B, never both", nb, N));
		for (int i = 0; i < 16; i++) a.lane(0).st[i].route = 0.f;
		a.m.params[Battery::BIAS_PARAM].setValue(1.f);
		allB = true;
		for (int i = 0; i < 32; i++) allB &= a.tick() == 0x100u;
		check(allB, "BIAS +100% sends route-0 steps to B");
		a.m.params[Battery::BIAS_PARAM].setValue(0.f);
		a.fill(1, 16);
		a.connect(Battery::BIAS_INPUT, 2);
		a.m.inputs[Battery::BIAS_INPUT].setVoltage(0.f, 0);
		a.m.inputs[Battery::BIAS_INPUT].setVoltage(5.f, 1);
		bool split = true;
		for (int i = 0; i < 32; i++) split &= a.tick() == (0x1u | 0x200u);
		check(split, "poly BIAS CV: channel 2 moves lane 2 only");
	}

	printf("== LOCK on a route ==\n");
	{
		Rig r;
		r.fill(0, 16);
		for (int i = 0; i < 16; i++) r.lane(0).st[i].route = 0.5f;
		auto pass = [&]() { std::string s; for (int i = 0; i < 16; i++) s += (r.tick() & 0x100u) ? 'B' : 'A'; return s; };
		std::string p1 = pass(), p2 = pass();
		check(p1 != p2, "unlocked, two passes differ: " + p1 + " / " + p2);
		// lock at the start of a pass: that pass and every one after it match
		r.m.params[Battery::LOCK_PARAM].setValue(1.f);
		std::string l1 = pass(), l2 = pass(), l3 = pass();
		check(l1 == l2 && l2 == l3, "locked, passes repeat: " + l1);
		check(l1 == p2, "locked at the end of a pass, it is that pass that repeats");
		r.m.params[Battery::LOCK_PARAM].setValue(0.f);
		std::string u1 = pass();
		check(u1 != l1, "unlocked again, the draws move on: " + u1);
		// a locked module saves and comes back the same
		r.m.params[Battery::LOCK_PARAM].setValue(1.f);
		std::string k1 = pass();
		check(k1 == u1, "locking keeps the pass just heard: " + k1);
		json_t* j = r.m.dataToJson();
		Rig q;
		q.m.dataFromJson(j);
		q.m.params[Battery::LOCK_PARAM].setValue(1.f);
		json_decref(j);
		std::string k2;
		for (int i = 0; i < 16; i++) k2 += (q.tick() & 0x100u) ? 'B' : 'A';
		check(k1 == k2, "a locked pass survives saving: " + k2);
	}

	printf("== ratchets and lane rates ==\n");
	{
		Rig r;
		r.fill(0, 4);
		r.lane(0).st[1].ratch = 3;
		r.lane(0).st[2].ratch = 4;
		int got[4];
		for (int i = 0; i < 4; i++) { r.tick(); got[i] = r.hits[0]; }   // the first pass measures the clock
		for (int i = 0; i < 4; i++) { r.tick(); got[i] = r.hits[0]; }
		check(got[0] == 1 && got[1] == 3 && got[2] == 4 && got[3] == 1,
		      string::f("ratchets 1 3 4 1 inside their steps: %d %d %d %d", got[0], got[1], got[2], got[3]));
		r.m.gateLen = 2;
		for (int i = 0; i < 4; i++) { r.tick(); got[i] = r.hits[0]; }
		check(got[1] == 3 && got[2] == 4, "and with 5 ms triggers");

		Rig x;
		x.fill(0, 16); x.lane(0).div = 8;    // x2
		x.fill(1, 16); x.lane(1).div = 2;    // /2
		x.tick(); x.tick();
		int a2 = 0, h2 = 0;
		for (int i = 0; i < 8; i++) { x.tick(); a2 += x.hits[0]; h2 += x.hits[1]; }
		check(a2 == 16 && h2 == 4, string::f("x2 plays 16 steps in 8 clocks (%d), /2 plays 4 (%d)", a2, h2));
	}

	printf("== patterns ==\n");
	{
		Rig r;
		r.fill(0, 16, 0);               // pattern 1: lane 1, 16 steps
		r.fill(1, 16, 1);               // pattern 2: lane 2
		bool p1 = true;
		for (int i = 0; i < 5; i++) p1 &= r.tick() == 0x1u;
		check(p1 && r.m.cur == 0, "pattern 1 plays");
		r.m.params[Battery::PATTERN_PARAM].setValue(1.f);
		r.run(16);
		check(r.m.target == 1 && r.m.cur == 0, "PATTERN 2 is queued, not taken mid-pattern");
		bool held = true;
		for (int i = 5; i < 16; i++) { held &= r.tick() == 0x1u; }
		unsigned first = r.tick();
		check(held && first == 0x2u && r.m.cur == 1, "it lands on the 17th clock, at the first step of pattern 2");

		// NEXT skips empty patterns: 2 -> (3..8 empty) -> 1
		r.m.params[Battery::PATTERN_PARAM].setValue(0.f);
		r.run(16);
		int t0 = r.m.target;
		r.trig(Battery::NEXT_INPUT);
		int t1 = r.m.target;
		r.m.params[Battery::NEXT_PARAM].setValue(1.f); r.run(48); r.m.params[Battery::NEXT_PARAM].setValue(0.f); r.run(48);
		int t2 = r.m.target;
		check(t0 == 0 && t1 == 1 && t2 == 0, string::f("NEXT goes 1 -> 2 -> 1, past the six empty patterns (%d %d %d)", t0 + 1, t1 + 1, t2 + 1));

		// with BAR patched the change waits for BAR, not the end of the lane
		Rig b;
		b.fill(0, 16, 0);
		b.fill(1, 16, 1);
		b.connect(Battery::BAR_INPUT);
		for (int i = 0; i < 3; i++) b.tick();
		b.m.params[Battery::PATTERN_PARAM].setValue(1.f);
		for (int i = 3; i < 20; i++) b.tick();
		check(b.m.cur == 0, "with BAR patched, the end of the lane does not change the pattern");
		b.m.inputs[Battery::BAR_INPUT].setVoltage(10.f);
		unsigned g = b.tick();
		b.m.inputs[Battery::BAR_INPUT].setVoltage(0.f);
		check(g == 0x2u && b.m.cur == 1 && b.m.run[1].pos == 0, "a BAR with its clock changes the pattern on that step");
		// a BAR 2 ms early waits for its clock
		b.m.params[Battery::PATTERN_PARAM].setValue(0.f);
		b.tick(); b.tick();
		b.m.inputs[Battery::BAR_INPUT].setVoltage(10.f);
		b.run(96);
		b.m.inputs[Battery::BAR_INPUT].setVoltage(0.f);
		g = b.tick();
		check(g == 0x1u && b.m.cur == 0 && b.m.run[0].pos == 0, string::f("a BAR 2 ms before its clock lands on that clock (%x)", g));

		// EOC fires as the longest lane wraps
		Rig e;
		e.fill(0, 4); e.fill(1, 6);
		int eocs = 0, tickAt[8], n = 0;
		for (int i = 0; i < 25; i++) {
			e.m.inputs[Battery::CLOCK_INPUT].setVoltage(10.f);
			bool seen = false;
			for (int k = 0; k < 96; k++) { e.run(1); seen |= e.m.outputs[Battery::EOC_OUTPUT].getVoltage() > 5.f; if (k == 47) e.m.inputs[Battery::CLOCK_INPUT].setVoltage(0.f); }
			e.run(PER - 96);
			if (seen) { eocs++; if (n < 8) tickAt[n++] = i; }
		}
		check(eocs == 4 && tickAt[0] == 6 && tickAt[1] == 12, string::f("EOC every 6 clocks with lanes of 4 and 6 (%d, first at %d)", eocs, n ? tickAt[0] : -1));
	}

	printf("== outputs, mute, run, reset ==\n");
	{
		Rig r;
		for (int l = 0; l < BT_L; l++) { r.fill(l, 16); for (int i = 0; i < 16; i++) r.lane(l).st[i].route = (l % 2) ? 1.f : 0.f; }
		unsigned g = r.tick();
		check(g == 0xAA55u, string::f("lanes 1 3 5 7 on A (1, 3, 5, 7), 2 4 6 8 on B (10, 12, 14, 16): %04x", g));
		check(r.m.outputs[Battery::GATE_OUTPUT].getChannels() == 16 && r.m.outputs[Battery::A_GATE_OUTPUT].getChannels() == 8
		      && r.m.outputs[Battery::B_VOCT_OUTPUT].getChannels() == 8, "16 channels on ALL, 8 on A and B");
		r.m.inputs[Battery::CLOCK_INPUT].setVoltage(10.f); r.run(60);
		bool split = true;
		for (int c = 0; c < 8; c++) {
			split &= (r.m.outputs[Battery::A_GATE_OUTPUT].getVoltage(c) > 5.f) == (r.m.outputs[Battery::GATE_OUTPUT].getVoltage(c) > 5.f);
			split &= (r.m.outputs[Battery::B_GATE_OUTPUT].getVoltage(c) > 5.f) == (r.m.outputs[Battery::GATE_OUTPUT].getVoltage(c + 8) > 5.f);
		}
		r.m.inputs[Battery::CLOCK_INPUT].setVoltage(0.f); r.run(PER - 60);
		check(split, "A and B carry channels 1-8 and 9-16 of ALL");
		check(std::fabs(r.m.outputs[Battery::VOCT_OUTPUT].getVoltage(9) - 9.f / 12.f) < 1e-6f
		      && std::fabs(r.m.outputs[Battery::A_VOCT_OUTPUT].getVoltage(3) - 3.f / 12.f) < 1e-6f, "V/OCT: channel N is N semitones above C4 by default");
		r.lane(2).st[r.m.run[2].pos == 15 ? 0 : r.m.run[2].pos + 1].vel = 0.3f;
		r.tick();
		check(std::fabs(r.m.outputs[Battery::VEL_OUTPUT].getVoltage(2) - 3.f) < 1e-4f, "VEL holds the step's velocity, 0-10 V");

		r.connect(Battery::MUTE_INPUT, 3);
		r.m.inputs[Battery::MUTE_INPUT].setVoltage(10.f, 1);
		g = r.tick();
		check(g == (0xAA55u & ~0x200u), string::f("poly MUTE channel 2 silences lane 2 only: %04x", g));
		r.m.inputs[Battery::MUTE_INPUT].setChannels(0); r.run(4);
		r.m.mute[0] = true;
		g = r.tick();
		check(g == (0xAA55u & ~0x1u), "the screen's mute silences lane 1");
		r.m.mute[0] = false;

		r.m.params[Battery::RUN_PARAM].setValue(0.f);
		long c0 = r.m.run[0].count;
		g = r.tick() | r.tick();
		check(g == 0 && r.m.run[0].count == c0, "RUN off: no gates and the lanes hold");
		r.m.params[Battery::RUN_PARAM].setValue(1.f);
		g = r.tick();
		check(g == 0xAA55u && r.m.run[0].count == c0 + 1, "RUN on: carries on from the next step");

		r.tick(); r.tick();
		r.trig(Battery::RESET_INPUT);
		r.run(PER);
		check(r.m.run[0].count == -1, "RESET: nothing plays until the next clock");
		r.tick();
		check(r.m.run[0].pos == 0, "and that clock plays the first step");
	}

	printf("== a retrigger on a high gate goes low first ==\n");
	{
		Rig r;
		r.fill(0, 2);
		r.tick(); r.tick();
		// halve the clock period suddenly: the half-step gate from the last period
		// is still high when the next step comes
		int minLow = 1 << 30, low = 0; bool was = false; int edges = 0;
		for (int i = 0; i < 6; i++) {
			r.m.inputs[Battery::CLOCK_INPUT].setVoltage(10.f);
			for (int k = 0; k < PER / 4; k++) {
				if (k == 48) r.m.inputs[Battery::CLOCK_INPUT].setVoltage(0.f);
				r.run(1);
				bool h = r.m.outputs[Battery::GATE_OUTPUT].getVoltage(0) > 5.f;
				if (h && !was) { edges++; if (edges > 1) minLow = std::min(minLow, low); }
				low = h ? 0 : low + 1;
				was = h;
			}
		}
		check(edges >= 5 && minLow >= 40, string::f("%d hits, at least %d samples low between them", edges, minLow));
	}

	printf("== saving ==\n");
	{
		Rig r;
		r.fill(3, 24);
		r.lane(3).mode = BT_SERIAL; r.lane(3).div = 8;
		r.lane(3).st[5].vel = 0.4f; r.lane(3).st[6].chance = 0.3f; r.lane(3).st[7].route = 0.7f; r.lane(3).st[8].ratch = 3;
		r.m.noteB[3] = -12.f; r.m.mute[5] = true;
		json_t* j = r.m.dataToJson();
		Rig q;
		q.m.dataFromJson(j);
		json_decref(j);
		const BtLane& a = q.lane(3);
		check(a.len == 24 && a.mode == BT_SERIAL && a.div == 8 && a.st[23].on && !a.st[24].on
		      && std::fabs(a.st[5].vel - 0.4f) < 1e-6f && std::fabs(a.st[6].chance - 0.3f) < 1e-6f
		      && std::fabs(a.st[7].route - 0.7f) < 1e-6f && a.st[8].ratch == 3
		      && q.m.noteB[3] == -12.f && q.m.mute[5], "every step and lane setting round-trips");
	}

	printf("== pages ==\n");
	{
		Battery b;
		for (int l = 0; l < BT_L; l++) b.pat[2].lane[l].len = 16;
		b.pat[2].lane[5].len = 12;
		b.pat[2].lane[6].len = 40;
		for (int c = 0; c < 16; c++) {
			BtStep& s = b.pat[2].lane[c % BT_L].st[c];
			s.on = true; s.vel = 0.1f * (c % 9); s.chance = 0.5f; s.route = 0.25f; s.ratch = 1 + c % 4;
		}
		b.duplicatePage(2, 0, 1);
		bool same = true;
		for (int l = 0; l < BT_L; l++)
			for (int c = 0; c < 16; c++) {
				const BtStep &a = b.pat[2].lane[l].st[c], &d = b.pat[2].lane[l].st[16 + c];
				same &= a.on == d.on && a.vel == d.vel && a.chance == d.chance && a.route == d.route && a.ratch == d.ratch;
			}
		check(same, "duplicating page 1 copies every field of every step onto page 2");
		check(b.pat[2].lane[0].len == 32 && b.pat[2].lane[5].len == 12 && b.pat[2].lane[6].len == 40,
		      string::f("16-step lanes become 32, a 12-step and a 40-step lane keep theirs (%d %d %d)",
		                b.pat[2].lane[0].len, b.pat[2].lane[5].len, b.pat[2].lane[6].len));
		b.duplicatePage(2, 1, 3);
		bool all = b.pat[2].lane[3].st[51].on == b.pat[2].lane[3].st[3].on && b.pat[2].lane[3].st[51].on;
		check(all && b.pat[2].lane[0].len == 64, "duplicating page 2 onto pages 3-4 fills them and lengthens to 64");
		Battery::BtPage pg = b.readPage(2, 0);
		b.clearPage(2, 0);
		check(!b.pat[2].lane[0].st[0].on && b.pat[2].lane[0].st[16].on, "clearing page 1 leaves page 2");
		b.writePage(2, 0, pg);
		check(b.pat[2].lane[0].st[0].on && b.pat[2].lane[0].len == 64, "pasting it back restores it and leaves the length");
		bool others = true;
		for (int p = 0; p < BT_P; p++) if (p != 2) others &= !b.pat[p].used();
		check(others, "page edits touch only their own pattern");
	}

	printf("== cost ==\n");
	{
		Rig r;
		for (int l = 0; l < BT_L; l++) {
			r.fill(l, 16);
			r.lane(l).mode = l % BT_NMODE;
			for (int i = 0; i < 16; i++) { r.lane(l).st[i].chance = 0.7f; r.lane(l).st[i].route = 0.4f; r.lane(l).st[i].ratch = 1 + i % 3; }
		}
		r.connect(Battery::BIAS_INPUT, 8);
		r.connect(Battery::MUTE_INPUT, 8);
		r.tick(); r.tick();
		auto t0 = std::chrono::steady_clock::now();
		int clocks = 80;   // 10 s
		for (int i = 0; i < clocks; i++) r.tick();
		double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		double pct = 100.0 * sec / (clocks * PER / SR);
		printf("  %.2f%% of one core, eight lanes playing\n", pct);
		check(pct < 1.5, "under 1.5% of a core");
	}

	printf(fails ? "\n%d FAILED\n" : "\nall passed (0 failures)\n", fails);
	return fails ? 1 : 0;
}

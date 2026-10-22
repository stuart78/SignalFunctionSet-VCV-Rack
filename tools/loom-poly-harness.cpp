// Loom's polyphonic GATE + V/OCT, against the real module: a chord on a poly
// cable must sound one string per note, notes picked one after another must
// ring together on separate strings, two channels asking for the same note
// must not share a string, and a mono cable must behave as it always did.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/loom-poly-harness.cpp -o /tmp/loom-poly-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/loom-poly-harness
// (-I src FIRST, or "plugin.hpp" resolves to the Rack SDK's own header.)
#include "../src/loom.cpp"
#include <cstdio>
#include <string>

rack::plugin::Plugin* pluginInstance = nullptr;
static int fails = 0;
static void check(bool ok, const std::string& what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }

struct Rig {
	Loom* m = new Loom();
	Module::ProcessArgs a;
	int64_t fr = 0;
	Rig() {
		a.sampleRate = 48000.f; a.sampleTime = 1.f / 48000.f;
		for (auto& o : m->outputs) o.channels = 1;
		m->onReset();
		run(4800);
	}
	~Rig() { delete m; }
	void run(int n) { for (int i = 0; i < n; i++) { a.frame = fr++; m->process(a); } }
	void poly(int n) { m->inputs[Loom::GATE_INPUT].channels = n; m->inputs[Loom::VOCT_INPUT].channels = n; }
	void note(int c, float v) { m->inputs[Loom::VOCT_INPUT].setVoltage(v, c); }
	void gate(int c, bool on) { m->inputs[Loom::GATE_INPUT].setVoltage(on ? 10.f : 0.f, c); }
	bool sounding(int s) const { return m->str[s].amp > 1e-3f; }
	// the pitch the string is at, in semitones from the note asked for
	float centsOff(int s, float v) const {
		float want = dsp::FREQ_C4 * std::pow(2.f, m->params[Loom::OCT_PARAM].getValue()
		           + m->params[Loom::ROOT_PARAM].getValue() / 12.f + v);
		return 1200.f * std::log2(m->str[s].freq / want);
	}
};

int main() {
	contextSet(new Context);
	APP->engine = new engine::Engine;
	APP->engine->setSampleRate(48000.f);
	const float C = 0.f, E = 4 / 12.f, G = 7 / 12.f;

	printf("== a chord on a three-channel cable\n");
	{
		Rig r; r.poly(3);
		float v[3] = {C, E, G};
		for (int c = 0; c < 3; c++) { r.note(c, v[c]); r.gate(c, true); }
		r.run(4800);
		int a = r.m->chanStr[0], b = r.m->chanStr[1], d = r.m->chanStr[2];
		check(a >= 0 && b >= 0 && d >= 0 && a != b && b != d && a != d, "three notes on three strings");
		bool all = a >= 0 && b >= 0 && d >= 0 && r.sounding(a) && r.sounding(b) && r.sounding(d);
		check(all, "all three sounding");
		bool tuned = all;
		for (int c = 0; c < 3 && all; c++) tuned = tuned && std::fabs(r.centsOff(r.m->chanStr[c], v[c])) < 5.f;
		check(tuned, "each at its own channel's V/OCT, within 5 cents");
	}

	printf("== three notes picked one after another (gates not overlapping)\n");
	{
		Rig r; r.poly(3);
		float v[3] = {C, G, C + 1.f};
		for (int c = 0; c < 3; c++) r.note(c, v[c]);
		for (int c = 0; c < 3; c++) { r.gate(c, true); r.run(2400); r.gate(c, false); r.run(2400); }
		int a = r.m->chanStr[0], b = r.m->chanStr[1], d = r.m->chanStr[2];
		check(a != b && b != d && a != d, "each on its own string");
		check(r.sounding(a) && r.sounding(b) && r.sounding(d), "all three still ringing together");
	}

	printf("== two channels asking for the same note\n");
	{
		Rig r; r.poly(2);
		for (int c = 0; c < 2; c++) { r.note(c, C); r.gate(c, true); }
		r.run(4800);
		check(r.m->chanStr[0] != r.m->chanStr[1], "two strings, not one plucked twice");
	}

	printf("== a channel that repeats its note re-plucks its own string\n");
	{
		Rig r; r.poly(2);
		r.note(0, C); r.note(1, G);
		r.gate(0, true); r.gate(1, true); r.run(2400);
		int first = r.m->chanStr[0];
		r.gate(0, false); r.run(240); r.gate(0, true); r.run(2400);
		check(r.m->chanStr[0] == first, "same string as before");
	}

	printf("== a mono cable, as before\n");
	{
		Rig r; r.poly(1);
		r.note(0, E); r.gate(0, true); r.run(4800);
		int s1 = r.m->chanStr[0];
		check(s1 >= 0 && r.sounding(s1) && std::fabs(r.centsOff(s1, E)) < 5.f, "a note sounds at the V/OCT");
		r.gate(0, false); r.run(240);
		r.note(0, G); r.gate(0, true); r.run(4800);
		int s2 = r.m->chanStr[0];
		check(s2 >= 0 && std::fabs(r.centsOff(s2, G)) < 5.f, "the next note sounds at its V/OCT");
		check(r.m->lastNearest == s2, "the screen's last string is the one played");
	}

	printf("== a gate high on the first sample a channel exists\n");
	{
		Rig r;                                    // run with the cable unpatched
		r.poly(2);
		r.note(0, C); r.note(1, G); r.gate(0, true); r.gate(1, true);
		r.run(4800);
		check(r.m->chanStr[0] >= 0 && r.m->chanStr[1] >= 0, "both notes play");
	}

	printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
	return fails ? 1 : 0;
}

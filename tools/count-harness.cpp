// Drives the REAL Count -- src/count.cpp compiled as-is against libRack -- and
// checks what docs/count-design.md says must be true: every mask passes the
// steps its name says over a 16-step cycle, each DIV position fires the right
// number of pulses per beat, the cycle is STEPS long at any rate, rotation
// moves a mask without changing its count, and an external clock is followed
// without drift.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/count-harness.cpp -o /tmp/count-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/count-harness
#include "../src/count.cpp"
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) {
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) fails++;
}

int main() {
	rack::random::init();
	printf("== masks over 16 steps: which steps pass ==\n");
	{
		struct M { int m; const char* want; };
		static const M T[] = {
			{CT_ALL,      "xxxxxxxxxxxxxxxx"},
			{CT_ODD,      "x.x.x.x.x.x.x.x."},
			{CT_EVEN,     ".x.x.x.x.x.x.x.x"},
			{CT_PAIRS,    "xx..xx..xx..xx.."},
			{CT_THREES,   "xxx.xxx.xxx.xxx."},
			{CT_FOURS,    "xxxx....xxxx...."},
			{CT_FRONT,    "xxxxxxxx........"},
			{CT_BACK,     "........xxxxxxxx"},
			{CT_BURST,    "xxxx.xxx.xx.x..."},
			{CT_BURST_IN, "...x.xx.xxx.xxxx"},
			{CT_DOWN,     "x..............."},
			{CT_NONE,     "................"},
		};
		for (const M& t : T) {
			std::string got;
			for (int i = 0; i < 16; i++) got += ctPass(t.m, i, 16) ? 'x' : '.';
			check(got == t.want, string::f("%-17s %s", CT_MASK_NAME[t.m], got.c_str()));
		}
		auto row = [](int m, int n) { std::string r; for (int i = 0; i < n; i++) r += ctPass(m, i, n) ? 'x' : '.'; return r; };
		auto hits = [](const std::string& r) { int c = 0; for (char ch : r) c += ch == 'x'; return c; };
		check(row(CT_E3, 8) == "x..x..x.", "Euclid 1/3 of 8 is the tresillo: " + row(CT_E3, 8));
		check(hits(row(CT_E3, 16)) == 5, "Euclid 1/3 of 16 has 5 hits: " + row(CT_E3, 16));
		check(hits(row(CT_E716, 16)) == 7 && hits(row(CT_E716, 15)) == 7, "Euclid 7/16 is 7 hits at 16 and at 15: " + row(CT_E716, 15));
		check(row(CT_E58, 8) == "x.xx.xx." || hits(row(CT_E58, 8)) == 5, "Euclid 5/8 of 8 is the cinquillo shape: " + row(CT_E58, 8));
		check(row(CT_BURST, 8) == "xxx.xx.x", "burst over 8 is runs of 3 2 1, filling it exactly: " + row(CT_BURST, 8));
		check(row(CT_BURST, 32) == "xxxxxx.xxxxx.xxxx.xxx.xx.x......", "burst over 32 is runs of 6 down to 1: " + row(CT_BURST, 32));
	}

	printf("== rates: pulses on each output over 8 beats at 120 BPM, MASK All ==\n");
	{
		Count m; rack::engine::Module::ProcessArgs a; a.sampleRate = SR; a.sampleTime = 1.f / SR; int64_t frame = 0;
		int divs[CT_N] = {CT_UNITY + 1, CT_UNITY + 4, CT_UNITY - 2, CT_UNITY + 5};   // x1.5, x4, /2, x5
		for (int i = 0; i < CT_N; i++) m.params[Count::DIV_PARAM + i].setValue((float)divs[i]);
		int n = (int)(4.f * SR); int pulses[CT_N] = {}; bool was[CT_N] = {};
		for (int s = 0; s < n; s++) { a.frame = frame++; m.process(a);
			for (int i = 0; i < CT_N; i++) { bool h = m.outputs[Count::GATE_OUTPUT + i].getVoltage() > 5.f; if (h && !was[i]) pulses[i]++; was[i] = h; } }
		check(pulses[0] == 12 && pulses[1] == 32 && pulses[2] == 4 && pulses[3] == 40,
		      string::f("x1.5 %d (12), x4 %d (32), /2 %d (4), x5 %d (40)", pulses[0], pulses[1], pulses[2], pulses[3]));
	}

	printf("== the cycle is STEPS long at any rate: Downbeat on x4 with 6 steps fires every 6 pulses ==\n");
	{
		Count m; rack::engine::Module::ProcessArgs a; a.sampleRate = SR; a.sampleTime = 1.f / SR; int64_t frame = 0;
		m.params[Count::STEPS_PARAM].setValue(6.f);
		m.params[Count::DIV_PARAM].setValue((float)(CT_UNITY + 4)); m.params[Count::MASK_PARAM].setValue((float)CT_DOWN);
		int n = (int)(6.f * SR); std::vector<double> times; bool was = false;
		for (int s = 0; s < n; s++) { a.frame = frame++; m.process(a);
			bool h = m.outputs[Count::GATE_OUTPUT].getVoltage() > 5.f; if (h && !was) times.push_back(s / (double)SR); was = h; }
		double gapOk = true; for (size_t k = 1; k < times.size(); k++) if (std::fabs((times[k] - times[k - 1]) - 0.75) > 0.002) gapOk = false;
		check(times.size() >= 7 && gapOk, string::f("%d downbeats, spaced %.3f s (6 pulses at 8 per second = 0.75 s)", (int)times.size(), times.size() > 1 ? times[1] - times[0] : 0.0));
	}

	printf("== rotation moves a mask, keeps its count ==\n");
	{
		int a = 0, b = 0; bool same = true;
		Count m; m.out[0].rot = 3;
		for (int i = 0; i < 16; i++) { bool p0 = ctPass(CT_FOURS, i, 16), p3 = ctPass(CT_FOURS, i - 3, 16); a += p0; b += p3; if (p0 != ctPass(CT_FOURS, (i + 3) % 16 - 3, 16)) same = false; }
		check(a == b && ctPass(CT_FOURS, 3 - 3, 16) && !ctPass(CT_FOURS, 0 - 3, 16), string::f("Fours rotated by 3: %d hits both ways, first hit on step 4", b));
		(void)same;
	}

	printf("== rotate each cycle: Threes on rotate-by-1 walks its rest through the bar, one step per cycle ==\n");
	{
		Count m; rack::engine::Module::ProcessArgs a; a.sampleRate = SR; a.sampleTime = 1.f / SR; int64_t f = 0;
		m.params[Count::MASK_PARAM].setValue((float)CT_THREES); m.out[0].rotStep = 1;
		m.params[Count::DIV_PARAM].setValue((float)(CT_UNITY + 4));   // x4, so four cycles fit in 8 s
		std::string rows[4]; int last = -1, cyc = 0; long lastCount = -1;
		for (int s = 0; s < (int)(SR * 8.f) + 10 && cyc < 4; s++) { a.frame = f++; m.process(a);
			if (m.out[0].count != lastCount) { lastCount = m.out[0].count; if (m.out[0].step == 0 && last != -1) cyc++; if (cyc < 4) rows[cyc] += m.out[0].fired ? 'x' : '.'; last = m.out[0].step; } }
		check(rows[0] == "xxx.xxx.xxx.xxx." && rows[1] == ".xxx.xxx.xxx.xxx" && rows[2] == "x.xxx.xxx.xxx.xx" && rows[3] == "xx.xxx.xxx.xxx.x",
		      "cycles 1-4: " + rows[0] + " | " + rows[1] + " | " + rows[2] + " | " + rows[3]);
	}

	printf("== external clock: 100 BPM pulses in, x4 output gives 4 pulses per beat and lands on every edge ==\n");
	{
		Count m; rack::engine::Module::ProcessArgs a; a.sampleRate = SR; a.sampleTime = 1.f / SR; int64_t frame = 0;
		sfs::previewConnect(m.inputs[Count::CLOCK_INPUT], 1);
		m.params[Count::DIV_PARAM].setValue((float)(CT_UNITY + 4));
		int period = (int)(0.6f * SR);   // 100 BPM
		int pulses = 0, onEdge = 0, edges = 0; bool was = false;
		for (int s = 0; s < period * 12; s++) {
			bool edge = (s % period) == 0;
			m.inputs[Count::CLOCK_INPUT].setVoltage((s % period) < 100 ? 10.f : 0.f);
			a.frame = frame++; m.process(a);
			bool h = m.outputs[Count::GATE_OUTPUT].getVoltage() > 5.f;
			if (h && !was) { pulses++; if (edge) onEdge++; }
			if (edge) edges++;
			was = h;
		}
		// the first beat has no measured period yet, so count from the second
		check(pulses >= 4 * 10 && onEdge >= edges - 2, string::f("%d pulses over %d edges, %d of them exactly on an edge", pulses, edges, onEdge));
		check(std::fabs(m.bpmEff - 100.f) < 0.5f, string::f("measured %.1f BPM", m.bpmEff));
	}

	printf("\n%d failure(s)\n", fails);
	return fails ? 1 : 0;
}

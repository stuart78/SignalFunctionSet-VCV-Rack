// Drives the REAL Garamut -- src/garamut.cpp compiled as-is against libRack --
// and checks what the panel promises: each GATE fires once per rising edge per
// channel on its own lip, ROLL strikes at its rate for as long as it is high and
// stays on the lip that channel last struck (or alternates, from the menu), Y
// and VEL are read per channel, a click on the screen strikes, the output stays
// bounded with every channel rolling, and what it costs.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/garamut-harness.cpp -o /tmp/garamut-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/garamut-harness
#include "../src/garamut.cpp"
#include <chrono>
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) {
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) fails++;
}

struct Rig {
	Garamut m;
	rack::engine::Module::ProcessArgs a;
	int64_t frame = 0;
	float peak = 0.f;
	bool finite = true;
	Rig() {
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		Module::SampleRateChangeEvent e; e.sampleRate = SR; e.sampleTime = 1.f / SR;
		m.onSampleRateChange(e);
		sfs::previewConnect(m.outputs[Garamut::LEFT_OUTPUT], 1);
		sfs::previewConnect(m.outputs[Garamut::RIGHT_OUTPUT], 1);
	}
	void connect(int in, int ch) { sfs::previewConnect(m.inputs[in], ch); for (int c = 0; c < ch; c++) m.inputs[in].setVoltage(0.f, c); }
	void run(float secs) {
		int n = (int)(secs * SR);
		for (int i = 0; i < n; i++) {
			a.frame = frame++;
			m.process(a);
			float l = m.outputs[Garamut::LEFT_OUTPUT].getVoltage(), r = m.outputs[Garamut::RIGHT_OUTPUT].getVoltage();
			if (!std::isfinite(l) || !std::isfinite(r)) finite = false;
			peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
		}
	}
	// the marks written since strike number `from`
	int sideOf(int k) { return m.marks[k % GM_MARKS].side; }
};

int main() {
	printf("Garamut harness\n");

	printf("\nGATE L and GATE R\n");
	{
		Rig g;
		g.connect(Garamut::GATE_L_INPUT, 4);
		g.connect(Garamut::GATE_R_INPUT, 2);
		g.run(0.01f);
		for (int c = 0; c < 4; c++) g.m.inputs[Garamut::GATE_L_INPUT].setVoltage(10.f, c);
		g.run(0.05f);
		int afterL = g.m.strikes;
		bool allLeft = true;
		for (int k = 0; k < afterL; k++) allLeft &= g.sideOf(k) == 0;
		check(afterL == 4, string::f("a 4-channel GATE L fires 4 strikes (%d)", afterL));
		check(allLeft, "all on the left lip");
		g.run(0.3f);
		check(g.m.strikes == 4, "a held gate does not strike again");
		for (int c = 0; c < 2; c++) g.m.inputs[Garamut::GATE_R_INPUT].setVoltage(10.f, c);
		g.run(0.05f);
		check(g.m.strikes == 6 && g.sideOf(4) == 1 && g.sideOf(5) == 1, "a 2-channel GATE R fires 2 strikes on the right lip");
		check(g.m.lastSide[0] == 1 && g.m.lastSide[2] == 0, "each channel remembers the lip it struck last");
		check(g.finite && g.peak > 1.f && g.peak < 10.f, string::f("the output sounds and stays under the clip (peak %.2f V)", g.peak));
	}

	printf("\nROLL\n");
	{
		Rig g;
		g.connect(Garamut::ROLL_INPUT, 1);
		g.run(0.01f);
		g.m.inputs[Garamut::ROLL_INPUT].setVoltage(10.f, 0);
		g.run(1.f);
		int n1 = g.m.strikes;
		g.m.inputs[Garamut::ROLL_INPUT].setVoltage(0.f, 0);
		g.run(0.5f);
		check(n1 >= 13 && n1 <= 16, string::f("held for 1 s at 14 strokes/s: %d strokes", n1));
		check(g.m.strikes == n1, "no strokes after the gate falls");
		bool left = true;
		for (int k = 0; k < n1; k++) left &= g.sideOf(k) == 0;
		check(left, "with nothing struck before, it rolls on the left lip");
		// faster with CV
		g.connect(Garamut::RATE_INPUT, 1);
		g.m.inputs[Garamut::RATE_INPUT].setVoltage(1.f, 0);
		int s0 = g.m.strikes;
		g.m.inputs[Garamut::ROLL_INPUT].setVoltage(10.f, 0);
		g.run(1.f);
		g.m.inputs[Garamut::ROLL_INPUT].setVoltage(0.f, 0);
		int n2 = g.m.strikes - s0;
		check(n2 >= 26 && n2 <= 31, string::f("1 V on ROLL RATE doubles it: %d strokes", n2));
	}
	{
		Rig g;
		g.connect(Garamut::GATE_R_INPUT, 1);
		g.connect(Garamut::ROLL_INPUT, 1);
		g.run(0.01f);
		g.m.inputs[Garamut::GATE_R_INPUT].setVoltage(10.f, 0);
		g.run(0.1f);
		g.m.inputs[Garamut::ROLL_INPUT].setVoltage(10.f, 0);
		g.run(0.5f);
		int n = g.m.strikes;
		bool right = true;
		for (int k = 1; k < n; k++) right &= g.sideOf(k) == 1;
		check(right, "after GATE R, the roll stays on the right lip");
		g.m.inputs[Garamut::ROLL_INPUT].setVoltage(0.f, 0);
		g.run(0.1f);
		g.m.rollAlternates = true;
		int s0 = g.m.strikes;
		g.m.inputs[Garamut::ROLL_INPUT].setVoltage(10.f, 0);
		g.run(0.5f);
		int lefts = 0, rights = 0;
		for (int k = s0; k < g.m.strikes; k++) (g.sideOf(k) ? rights : lefts)++;
		check(std::abs(lefts - rights) <= 1 && lefts > 2, string::f("with \"A roll alternates the two lips\": %d left, %d right", lefts, rights));
		check(g.sideOf(s0) == 1, "the alternating roll starts on the lip struck last");
	}

	printf("\nY and VEL, per channel\n");
	{
		Rig g;
		g.connect(Garamut::GATE_L_INPUT, 3);
		g.connect(Garamut::Y_INPUT, 3);
		g.connect(Garamut::VEL_INPUT, 2);
		g.m.inputs[Garamut::Y_INPUT].setVoltage(-5.f, 0);
		g.m.inputs[Garamut::Y_INPUT].setVoltage(0.f, 1);
		g.m.inputs[Garamut::Y_INPUT].setVoltage(2.5f, 2);
		g.m.inputs[Garamut::VEL_INPUT].setVoltage(10.f, 0);
		g.m.inputs[Garamut::VEL_INPUT].setVoltage(3.f, 1);
		g.run(0.01f);
		for (int c = 0; c < 3; c++) g.m.inputs[Garamut::GATE_L_INPUT].setVoltage(10.f, c);
		g.run(0.01f);
		const Garamut::Mark* M = g.m.marks;
		check(std::fabs(M[0].y - 0.f) < 1e-4f && std::fabs(M[1].y - 0.5f) < 1e-4f && std::fabs(M[2].y - 0.75f) < 1e-4f,
		      string::f("Y per channel: %.2f %.2f %.2f (want 0, 0.5, 0.75)", M[0].y, M[1].y, M[2].y));
		check(std::fabs(M[0].vel - 1.f) < 1e-4f && std::fabs(M[1].vel - 0.3f) < 1e-4f,
		      string::f("VEL per channel: %.2f %.2f", M[0].vel, M[1].vel));
	}
	{
		Rig g;
		g.connect(Garamut::GATE_L_INPUT, 3);
		g.connect(Garamut::VEL_INPUT, 1);
		g.m.inputs[Garamut::VEL_INPUT].setVoltage(4.f, 0);
		g.run(0.01f);
		for (int c = 0; c < 3; c++) g.m.inputs[Garamut::GATE_L_INPUT].setVoltage(10.f, c);
		g.run(0.01f);
		bool all = true;
		for (int k = 0; k < 3; k++) all &= std::fabs(g.m.marks[k].vel - 0.4f) < 1e-4f;
		check(all, "a mono VEL sets every channel");
	}
	{
		Rig g;
		g.connect(Garamut::GATE_L_INPUT, 1);
		g.run(0.01f);
		g.m.inputs[Garamut::GATE_L_INPUT].setVoltage(10.f, 0);
		g.run(0.01f);
		check(std::fabs(g.m.marks[0].vel - 0.7f) < 1e-4f, "VEL unpatched strikes at 70%");
	}

	printf("\nThe screen\n");
	{
		Rig g;
		g.m.uiY = 0.3f; g.m.uiSide.store(1);
		g.run(0.05f);
		check(g.m.strikes == 1 && g.m.marks[0].side == 1 && std::fabs(g.m.marks[0].y - 0.3f) < 1e-4f, "a click strikes the right lip where it lands");
	}

	printf("\nSixteen channels rolling at the fastest rate, full velocity\n");
	{
		Rig g;
		g.connect(Garamut::ROLL_INPUT, 16);
		g.connect(Garamut::VEL_INPUT, 16);
		g.connect(Garamut::Y_INPUT, 16);
		for (int c = 0; c < 16; c++) {
			g.m.inputs[Garamut::VEL_INPUT].setVoltage(10.f, c);
			g.m.inputs[Garamut::Y_INPUT].setVoltage(-4.f + 0.5f * c, c);
		}
		g.m.params[Garamut::RATE_PARAM].setValue(1.f);
		g.run(0.01f);
		for (int c = 0; c < 16; c++) g.m.inputs[Garamut::ROLL_INPUT].setVoltage(10.f, c);
		auto t0 = std::chrono::steady_clock::now();
		g.run(5.f);
		double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		check(g.finite && g.peak <= 10.f, string::f("finite and under 10 V (peak %.2f V, %d strokes)", g.peak, g.m.strikes));
		printf("  cost: %.2f%% of a core\n", 100.0 * s / 5.0);
	}
	{
		Rig g;
		g.connect(Garamut::GATE_L_INPUT, 1);
		g.run(0.01f);
		g.m.inputs[Garamut::GATE_L_INPUT].setVoltage(10.f, 0);
		g.run(0.01f);
		g.m.inputs[Garamut::GATE_L_INPUT].setVoltage(0.f, 0);
		g.run(8.f);
		check(g.m.drum.asleep, "the drum sleeps once a strike has rung out");
		auto t0 = std::chrono::steady_clock::now();
		g.run(5.f);
		double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		printf("  idle cost: %.2f%% of a core\n", 100.0 * s / 5.0);
	}

	printf("\nEvery control swept while a roll plays\n");
	{
		// SIZE once left a value uninitialised that the size scale then multiplied on
		// every update, and the output went to NaN within half a second and stayed there.
		const int ps[9] = {Garamut::SIZE_PARAM, Garamut::SHAPE_PARAM, Garamut::MATERIAL_PARAM, Garamut::SLIT_PARAM,
		                   Garamut::MALLET_PARAM, Garamut::INTERVAL_PARAM, Garamut::DECAY_PARAM, Garamut::TUNE_PARAM, Garamut::Y_PARAM};
		for (float hz : {0.3f, 4.f}) {
			Rig g;
			g.connect(Garamut::ROLL_INPUT, 2);
			g.run(0.01f);
			g.m.inputs[Garamut::ROLL_INPUT].setVoltage(10.f, 0);
			g.m.inputs[Garamut::ROLL_INPUT].setVoltage(10.f, 1);
			// Some settings are quiet on purpose (a strike at the clamped end of the
			// slit, the shortest DECAY), so the test is per quarter second: the drum
			// never stops sounding, never falls asleep, and never goes non-finite.
			float lo = 1e9f;
			bool slept = false;
			for (int blk = 0; blk < 16; blk++) {
				for (int k = 0; k < 12000; k++) {
					float t = (blk * 12000 + k) / SR;
					for (int i = 0; i < 9; i++) {
						ParamQuantity* q = g.m.paramQuantities[ps[i]];
						float u = 0.5f + 0.5f * std::sin(2.f * (float)M_PI * hz * (1.f + 0.37f * i) * t + i);
						g.m.params[ps[i]].setValue(q->getMinValue() + u * (q->getMaxValue() - q->getMinValue()));
					}
					g.run(1.f / SR);
					slept |= g.m.drum.asleep;
				}
				float p = g.peak; g.peak = 0.f;
				lo = std::min(lo, p);
			}
			check(g.finite && !slept && lo > 0.3f, string::f("swept at %.1f Hz: finite, awake, and every quarter second sounds (quietest peak %.2f V)", hz, lo));
		}
	}

	printf("\nLevel across V/OCT (one strike, default mallet)\n");
	{
		float lo = 1e9f, hi = 0.f;
		for (float v = -1.f; v <= 1.01f; v += 0.5f) {
			Rig g;
			g.connect(Garamut::VOCT_INPUT, 1);
			g.connect(Garamut::GATE_L_INPUT, 1);
			g.m.inputs[Garamut::VOCT_INPUT].setVoltage(v, 0);
			g.run(0.1f);        // the output gain glides to a new pitch over about 10 ms
			g.m.inputs[Garamut::GATE_L_INPUT].setVoltage(10.f, 0);
			g.run(1.f);
			lo = std::min(lo, g.peak); hi = std::max(hi, g.peak);
		}
		check(20.f * std::log10(hi / lo) < 1.f, string::f("-1 V to +1 V within 1 dB (%.2f to %.2f V)", lo, hi));
	}

	printf("\nPatch storage\n");
	{
		Garamut a; a.rollAlternates = true;
		json_t* j = a.dataToJson();
		Garamut b; b.dataFromJson(j);
		json_decref(j);
		check(b.rollAlternates, "\"A roll alternates the two lips\" is saved");
	}

	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all passed", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}

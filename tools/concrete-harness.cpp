// Drives the REAL Concrete -- src/concrete.cpp compiled as-is against libRack -- and
// checks what docs/concrete-design.md says must be true: a clip starts on its bar,
// is cut off by the next clip and by the loop end, plays at its speed (and
// follows the tempo when told to), a clip put down replaces what is under it
// and leaves the rest after it, the curves act, a clock drives the playhead and
// stopping it stops the tape, every edge is faded, and recording lands where
// it was made, splitting a clip it lands in the middle of.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/concrete-harness.cpp -o /tmp/concrete-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/concrete-harness
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "../src/concrete.cpp"
#include <chrono>
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;
Model* modelConcreteIn = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) {
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) fails++;
}

// a sample whose every frame is v (a DC level is easy to find in the output)
static ConcreteSample* dc(float v, float secs) {
	int64_t n = (int64_t)(secs * SR);
	std::vector<float> raw((size_t)n, v);
	return ccFromInterleaved(raw.data(), n, 1, SR);
}
static ConcreteSample* ramp(float secs) {
	int64_t n = (int64_t)(secs * SR);
	std::vector<float> raw((size_t)n);
	for (int64_t i = 0; i < n; i++) raw[(size_t)i] = (float)i / n;
	return ccFromInterleaved(raw.data(), n, 1, SR);
}

struct Rig {
	Concrete m;
	rack::engine::Module::ProcessArgs a;
	int64_t frame = 0;
	ConcreteInMessage in;
	float maxStep = 0.f, lastL = 0.f;
	Rig(int bars = 4) {
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		m.params[Concrete::LENGTH_PARAM].setValue((float)bars);
		m.params[Concrete::BPM_PARAM].setValue(120.f);
		run(16);
	}
	void run(int n) {
		for (int i = 0; i < n; i++) {
			a.frame = frame++;
			m.process(a);
			if (frame % 800 == 0) m.uiService();   // the UI thread, at 60 frames a second
			float o = m.outputs[Concrete::L_OUTPUT].getVoltage();
			maxStep = std::max(maxStep, std::fabs(o - lastL));
			lastL = o;
		}
	}
	// play forward to `bar`, round the loop end if it is behind the playhead
	void runTo(double bar) {
		int guard = 0;
		if (bar < m.P) { double p = m.P; while (guard++ < 48000 * 60) { run(1); if (m.P < p) break; p = m.P; } }
		while (m.P < bar - 1e-6 && guard++ < 48000 * 60) run(1);
	}
	void play() { m.params[Concrete::PLAY_PARAM].setValue(1.f); }
	void stop() { m.params[Concrete::PLAY_PARAM].setValue(0.f); }
	int put(int l, ConcreteSample* s, float t0, float speed = 1.f, bool rev = false, float fit = 0.f) {
		int si = m.publish(s);
		ConcreteClip c; c.smp = si; c.t0 = t0; c.a = 0.0; c.b = (double)s->n; c.speed = speed; c.rev = rev; c.fit = fit;
		return m.placeClip(l, c);
	}
	float out(int l = 0) const { return m.laneL[l]; }
	int clips(int l) const { int n = 0; for (int j = 0; j < CC_CLIPS; j++) n += m.clip[l][j].used; return n; }
	// samples until lane l first moves more than thr from silence
	int64_t onsetAfter(int l, int maxN, float thr = 1e-3f) {
		for (int i = 0; i < maxN; i++) { run(1); if (std::fabs(out(l)) > thr) return i; }
		return -1;
	}
	int64_t soundFor(int l, int maxN, float thr = 0.05f) {
		int64_t n = 0;
		for (int i = 0; i < maxN; i++) { run(1); if (std::fabs(out(l)) > thr) n++; }
		return n;
	}
};

int main() {
	rack::random::init();

	printf("== the transport ==\n");
	{
		Rig r; r.play(); r.run(48000);
		check(std::fabs(r.m.P - 0.5) < 1e-3, string::f("120 BPM: one second is half a bar (%.4f)", r.m.P));
		r.run(48000 * 7 + 4800);
		check(std::fabs(r.m.P - 0.05) < 1e-3, string::f("a 4-bar loop wraps at 8 s (%.4f at 8.1 s)", r.m.P));
		r.stop(); r.run(4800);
		double held = r.m.P;
		r.run(4800);
		check(r.m.P == held, "stopped, the playhead holds");
	}

	printf("\n== the lane outputs ==\n");
	{
		Rig r; r.put(0, dc(0.2f, 10.f), 0.f); r.put(2, dc(0.1f, 10.f), 0.f); r.play(); r.run(4800);
		float lane0 = r.m.outputs[Concrete::LANE_L_OUTPUT + 0].getVoltage(), lane2 = r.m.outputs[Concrete::LANE_L_OUTPUT + 2].getVoltage();
		float sum = 0.f;
		for (int l = 0; l < CC_N; l++) sum += r.m.outputs[Concrete::LANE_L_OUTPUT + l].getVoltage();
		check(lane0 > 0.5f && lane2 > 0.2f && r.m.outputs[Concrete::LANE_L_OUTPUT + 1].getVoltage() == 0.f,
			string::f("each lane has its own output (%.3f, 0, %.3f V)", lane0, lane2));
		check(std::fabs(sum - r.m.outputs[Concrete::L_OUTPUT].getVoltage()) < 0.05f,
			string::f("below the clip, the lanes sum to the mix (%.3f against %.3f V)", sum, r.m.outputs[Concrete::L_OUTPUT].getVoltage()));
	}

	printf("\n== a clip on its bar ==\n");
	{
		Rig r; r.put(0, dc(0.5f, 10.f), 1.f); r.play();
		int64_t on = r.onsetAfter(0, 48000 * 3);
		check(on >= 96000 - 20 && on <= 96000 + 2, string::f("a clip at bar 2 starts 2 s in (sample %lld)", (long long)on - 16));
		r.run(1000);
		check(std::fabs(r.out() - 2.5f) < 1e-3f, string::f("a full-scale 0.5 plays at 2.5 V (%.4f)", r.out()));
	}
	{
		Rig r; r.put(0, dc(0.5f, 10.f), 0.f); r.put(0, dc(-0.5f, 10.f), 1.f); r.play();
		r.runTo(1.5);
		check(std::fabs(r.out() + 2.5f) < 1e-3f, string::f("the next clip's start cuts the one before (%.3f V)", r.out()));
		check(r.maxStep < 0.06f, string::f("and the switch is faded (largest step %.4f V)", r.maxStep));
	}
	{
		Rig r(2); r.put(0, dc(0.5f, 20.f), 1.f); r.play();
		r.runTo(1.9);
		bool before = std::fabs(r.out() - 2.5f) < 1e-3f;
		r.runTo(1.999); r.run(4800);
		check(before && r.m.P < 0.2 && std::fabs(r.out()) < 1e-4f,
		      string::f("the loop end cuts a clip off (%.4f V at bar %.3f)", r.out(), r.m.P + 1));
	}

	printf("\n== speed ==\n");
	{
		Rig r; r.put(0, dc(0.5f, 1.f), 0.f, 0.5f); r.play();
		int64_t n = r.soundFor(0, 48000 * 4);
		check(std::abs((int)(n - 96000)) < 300, string::f("a 1 s sample at x0.5 lasts 2 s (%.3f s)", n / SR));
	}
	{
		Rig r; r.put(0, ramp(1.f), 0.f, 1.f, true); r.play();
		r.run(4800); float a = r.out(); r.run(4800); float b = r.out();
		check(a > b && a > 4.f, string::f("reversed, a rising ramp falls (%.2f then %.2f V)", a, b));
	}
	{
		Rig r; r.put(0, dc(0.5f, 1.5f), 0.f, 1.f, false, 1.f); r.play();
		int64_t n = r.soundFor(0, 48000 * 3);
		check(std::abs((int)(n - 96000)) < 300, string::f("following the tempo, 1.5 s of audio fills one bar at 120 BPM (%.3f s)", n / SR));
		Rig q; q.m.params[Concrete::BPM_PARAM].setValue(60.f); q.run(16);
		q.put(0, dc(0.5f, 1.5f), 0.f, 1.f, false, 1.f); q.play();
		int64_t n2 = q.soundFor(0, 48000 * 6);
		check(std::abs((int)(n2 - 192000)) < 600, string::f("and fills one bar at 60 BPM (%.3f s)", n2 / SR));
	}

	printf("\n== putting a clip down ==\n");
	{
		Rig r; r.put(0, dc(0.5f, 8.f), 0.f); r.put(0, dc(-0.5f, 2.f), 1.f); r.play();
		check(r.clips(0) == 3, string::f("a clip put in the middle of another splits it (%d clips)", r.clips(0)));
		r.runTo(1.5); float mid = r.out();
		r.runTo(2.5); float after = r.out();
		check(std::fabs(mid + 2.5f) < 1e-3f && std::fabs(after - 2.5f) < 1e-3f,
		      string::f("it replaces what it covers and the rest plays after it (%.2f, %.2f V)", mid, after));
		// the tail carries on from where the audio had got to: a ramp tells
		Rig q; q.put(0, ramp(8.f), 0.f); q.put(0, dc(-0.5f, 2.f), 1.f); q.play();
		q.runTo(2.5);
		float want = 5.f * (float)(2.5 * 2.0 / 8.0);
		check(std::fabs(q.out() - want) < 0.01f, string::f("the tail resumes where the tape had got to (%.3f V, want %.3f)", q.out(), want));
		Rig z; z.put(0, dc(0.5f, 1.f), 1.f); z.put(0, dc(-0.5f, 4.f), 0.f);
		check(z.clips(0) == 1, string::f("a clip covering another entirely replaces it (%d clip)", z.clips(0)));
	}

	printf("\n== the curves ==\n");
	{
		Rig r; r.put(0, dc(0.5f, 20.f), 0.f);
		for (int i = 0; i < CC_CN; i++) r.m.curve[0][CC_VOL][i] = 0.5f;
		r.play(); r.run(4800);
		check(std::fabs(r.out() - 0.625f) < 1e-3f, string::f("VOL at half travel is a quarter of the level (%.4f V)", r.out()));
		for (int i = 0; i < CC_CN; i++) { r.m.curve[0][CC_VOL][i] = 1.f; r.m.curve[0][CC_PAN][i] = 1.f; }
		r.run(4800);
		check(std::fabs(r.m.laneL[0]) < 1e-3f && std::fabs(r.m.laneR[0] - 2.5f) < 1e-3f,
		      string::f("PAN right: left silent, right untouched (%.3f, %.3f V)", r.m.laneL[0], r.m.laneR[0]));
	}
	{
		// noise through the filter, lowpass at the bottom of its travel
		int64_t n = (int64_t)(10 * SR);
		std::vector<float> raw((size_t)n);
		uint32_t s = 1;
		for (auto& x : raw) { s = s * 1664525u + 1013904223u; x = 0.5f * ((float)(s >> 8) / 8388608.f - 1.f); }
		std::vector<float> low((size_t)n);
		for (int64_t i = 0; i < n; i++) low[(size_t)i] = 0.5f * std::sin(2.f * (float)M_PI * 100.f * i / SR);
		auto rms = [&](float f, std::vector<float>& src) {
			Rig r; r.put(0, ccFromInterleaved(src.data(), n, 1, SR), 0.f);
			for (int i = 0; i < CC_CN; i++) r.m.curve[0][CC_FILT][i] = f;
			r.play(); r.run(4800);
			double a = 0; for (int i = 0; i < 48000; i++) { r.run(1); a += r.out() * r.out(); }
			return std::sqrt(a / 48000);
		};
		double dry = rms(0.f, raw), lp = rms(-0.9f, raw);
		double dryLow = rms(0.f, low), hp = rms(0.9f, low);
		check(lp < dry * 0.3, string::f("FILTER below the centre: a lowpass cuts white noise to %.2f", lp / dry));
		check(hp < dryLow * 0.05, string::f("FILTER above the centre: a highpass cuts a 100 Hz sine to %.3f", hp / dryLow));
	}

	printf("\n== CLOCK and BAR ==\n");
	{
		Rig r;
		rack::engine::Port& ck = r.m.inputs[Concrete::CLOCK_INPUT];
		rack::engine::Port& br = r.m.inputs[Concrete::BAR_INPUT];
		sfs::previewConnect(ck, 1); sfs::previewConnect(br, 1);
		r.put(0, dc(0.5f, 60.f), 0.f);
		r.play();
		// 16 clocks a bar, a bar every 2 s, bar and clock together on the downbeat
		auto tick = [&](int k) {
			ck.setVoltage(10.f); if (k % 16 == 0) br.setVoltage(10.f);
			r.run(48); ck.setVoltage(0.f); br.setVoltage(0.f); r.run(6000 - 48);
		};
		tick(0);
		check(r.m.P < 1.0 / 16, string::f("the first clock after a reset is bar 1 (%.4f)", r.m.P));
		for (int k = 1; k < 20; k++) tick(k);
		check(r.m.cpb == 16, string::f("clocks per bar are counted between BARs (%d)", r.m.cpb));
		check(std::fabs(r.m.P - 20.0 / 16) < 0.002, string::f("the playhead follows the clock, never past the next one (%.4f, want just under %.4f)", r.m.P, 20.0 / 16));
		check(std::fabs(r.m.spbSm - 2.0) < 0.01, string::f("and measures the bar (%.3f s)", r.m.spbSm));
		float playing = r.out();
		r.run(48000);
		check(std::fabs(playing - 2.5f) < 1e-3f && std::fabs(r.out()) < 1e-4f, "a clock that stops stops the tape");
	}

	printf("\n== recording ==\n");
	{
		Rig r; r.m.inOverride = &r.in;
		r.in.on[0] = true;
		r.run(16); r.m.uiService();
		check(r.m.take[0][0].state.load() == TK_READY || r.m.take[0][1].state.load() == TK_READY, "a buffer is ready while Concrete In is attached");
		r.play(); r.runTo(1.0);
		r.in.l[0] = 1.f; r.in.r[0] = -1.f; r.in.rec[0] = true;
		r.runTo(2.0);
		check(std::fabs(r.m.laneL[0] - 1.f) < 1e-3f, string::f("recording, the lane plays its input (%.3f V)", r.m.laneL[0]));
		r.in.rec[0] = false; r.run(1);
		r.m.uiService();
		check(r.clips(0) == 1, string::f("the take becomes a clip (%d)", r.clips(0)));
		int j = -1; for (int k = 0; k < CC_CLIPS; k++) if (r.m.clip[0][k].used) j = k;
		if (j >= 0) {
			check(std::fabs(r.m.clip[0][j].t0 - 1.f) < 1e-3f && std::fabs(r.m.clipBars(r.m.clip[0][j], 2.0) - 1.0) < 1e-3,
			      string::f("where it was made: bar %.3f, %.3f bars long", r.m.clip[0][j].t0 + 1, r.m.clipBars(r.m.clip[0][j], 2.0)));
		}
		r.in.l[0] = r.in.r[0] = 0.f;
		r.runTo(1.5);
		check(std::fabs(r.m.laneL[0] - 1.f) < 1e-3f && std::fabs(r.m.laneR[0] + 1.f) < 1e-3f,
		      string::f("and plays back in stereo on the next pass (%.3f, %.3f V)", r.m.laneL[0], r.m.laneR[0]));
	}
	{
		// held across the loop end: two takes, one up to the end and one from bar 1
		Rig r(2); r.m.inOverride = &r.in; r.in.on[0] = true;
		r.run(16); r.m.uiService(); r.play();
		r.runTo(1.5);
		r.in.l[0] = r.in.r[0] = 1.f; r.in.rec[0] = true;
		r.runTo(0.5);
		r.m.uiService();
		r.in.rec[0] = false; r.run(1); r.m.uiService();
		int n = r.clips(0);
		float t[2] = {-1, -1}; int q = 0;
		for (int k = 0; k < CC_CLIPS && q < 2; k++) if (r.m.clip[0][k].used) t[q++] = r.m.clip[0][k].t0;
		check(n == 2 && std::min(t[0], t[1]) < 1e-3f && std::fabs(std::max(t[0], t[1]) - 1.5f) < 1e-3f,
		      string::f("recording across the loop end leaves a take at each side (%d clips, at %.2f and %.2f)", n, t[0], t[1]));
	}
	{
		// punched into the middle of a clip: the clip carries on after the take
		Rig r; r.m.inOverride = &r.in; r.in.on[0] = true;
		r.put(0, dc(0.5f, 10.f), 0.f);
		r.run(16); r.m.uiService(); r.play();
		r.runTo(1.0);
		r.in.l[0] = r.in.r[0] = -1.f; r.in.rec[0] = true;
		r.runTo(2.0);
		r.in.rec[0] = false; r.run(1); r.m.uiService();
		r.runTo(2.5);
		check(r.clips(0) == 3 && std::fabs(r.out() - 2.5f) < 1e-3f,
		      string::f("a punch-in replaces only what it covered (%d clips, %.2f V after it)", r.clips(0), r.out()));
	}

	printf("\n== clicks ==\n");
	{
		Rig r(2); r.put(0, dc(0.8f, 0.3f), 0.f); r.put(0, dc(-0.8f, 0.7f), 0.25f); r.put(0, dc(0.8f, 3.f), 1.f);
		r.play(); r.run(48000 * 9);
		check(r.maxStep < 0.08f, string::f("starts, cuts, ends and the loop end are all faded (largest step %.4f V)", r.maxStep));
	}

	printf("\n== the screen's zoom ==\n");
	{
		Rig r(8);
		ConcreteDisplay d;
		d.module = &r.m;
		d.box.size = mm2px(Vec(152.4f, 83.82f));
		bool rt = true;
		for (double v : {0.0, 1.37, 5.5, 7.99}) rt &= std::fabs(d.barOf(d.xOf(v)) - v) < 1e-6;
		check(rt && !d.zoomed(), "unzoomed: the lanes span the loop, and screen <-> bar is exact");
		d.viewBars(2, 5.3);
		check(d.zoomed() && d.vLen() == 2.0 && d.vStart() == 4.0, string::f("2 bars shows the page holding the playhead: bars %.0f-%.0f", d.vStart() + 1, d.vStart() + d.vLen()));
		rt = true;
		for (double v : {4.1, 5.0, 5.9}) rt &= std::fabs(d.barOf(d.xOf(v)) - v) < 1e-6;
		check(rt, "zoomed, screen <-> bar is exact");
		d.zoomStep(-1, 5.3);
		check(d.vLen() == 1.0 && d.vStart() == 5.0, string::f("stepping in from 2 bars gives 1, the bar under the pointer (bar %.0f)", d.vStart() + 1));
		d.zoomStep(-1, 5.3);
		check(d.vLen() == 1.0, "and one bar is the closest view");
		d.zoomStep(1, 5.3); d.zoomStep(1, 5.3);
		check(d.vLen() == 4.0 && d.vStart() == 4.0, string::f("stepping out: 2, then 4 bars (bars %.0f-%.0f)", d.vStart() + 1, d.vStart() + d.vLen()));
		d.setView(5.4, 4.0);
		check(d.vStart() == 4.0, string::f("the view stays inside the loop and on a bar line (starts at bar %.0f)", d.vStart() + 1));
		r.m.params[Concrete::LENGTH_PARAM].setValue(6.f); r.run(1);
		check(d.vStart() + d.vLen() <= 6.0 + 1e-9, "and inside it again when the loop is shortened");
		d.zoomStep(1, 1.0); d.zoomStep(1, 1.0);
		check(!d.zoomed() && r.m.viewLen == 0.f, "stepping out past 4 bars is the whole loop");
	}

	printf("\n== the curves are points ==\n");
	{
		Rig r;
		r.m.pts[0][CC_VOL] = {{1.f, 0.f}, {2.f, 1.f}};
		r.m.renderCurve(0, CC_VOL);
		auto at = [&](float b) { return r.m.curve[0][CC_VOL][(int)std::round(b * CC_RES)]; };
		check(at(0.5f) == 0.f && std::fabs(at(1.5f) - 0.5f) < 1e-6f && at(3.f) == 1.f,
		      string::f("straight lines between points, held before the first and after the last (%.2f %.2f %.2f)", at(0.5f), at(1.5f), at(3.f)));
		r.m.pts[0][CC_VOL] = {{1.f, 0.2f}, {2.f, 0.2f}, {2.f, 0.9f}};
		r.m.renderCurve(0, CC_VOL);
		check(std::fabs(at(1.99f) - 0.2f) < 0.02f && std::fabs(at(2.02f) - 0.9f) < 1e-6f, "two points on one bar make a step");
		r.m.pts[0][CC_VOL].clear(); r.m.renderCurve(0, CC_VOL);
		check(at(1.f) == 1.f, "no points is the default");
	}

	printf("\n== cost ==\n");
	{
		// four lanes busy, every curve moving, one lane at a tempo-following speed
		Rig r(8);
		for (int l = 0; l < CC_N; l++)
			for (int b = 0; b < 8; b++) r.put(l, ramp(1.1f), (float)b, l == 2 ? 0.7f : 1.f, l == 3, l == 1 ? 1.f : 0.f);
		for (int l = 0; l < CC_N; l++)
			for (int i = 0; i < CC_CN; i++) {
				r.m.curve[l][CC_VOL][i] = 0.6f + 0.4f * std::sin(i * 0.05f);
				r.m.curve[l][CC_FILT][i] = 0.8f * std::sin(i * 0.03f + l);
				r.m.curve[l][CC_PAN][i] = 0.7f * std::sin(i * 0.02f);
			}
		r.play();
		auto t0 = std::chrono::steady_clock::now();
		r.run(48000 * 10);
		double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
		double pct = us / 1e6 / 10.0 * 100.0;
		printf("  %.2f%% of one core at 48 kHz (four lanes playing, every curve moving)\n", pct);
		check(pct < 3.0, "under 3% of a core");
	}

	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all passed", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}

// Drives the REAL Pole -- src/pole.cpp compiled as-is against libRack -- and
// checks the claims in docs/pole-design.md: the analytic response the
// screen draws (and the level is corrected from) matches what the filter does,
// the level holds across a SPREAD sweep, order is inaudible without DRIVE,
// every stereo relation sets R as described, LAG staggers the pairs, nothing
// blows up under fast modulation, and what it costs.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/pole-harness.cpp -o /tmp/pole-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/pole-harness
#include "../src/pole.cpp"
#include <chrono>
#include <functional>
#include <vector>
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
	Pole m;
	rack::engine::Module::ProcessArgs a;
	int64_t frame = 0;
	double ph = 0.0;
	Rig() {
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		Module::SampleRateChangeEvent e; e.sampleRate = SR; e.sampleTime = 1.f / SR;
		m.onSampleRateChange(e);
		sfs::previewConnect(m.inputs[Pole::IN_L_INPUT], 1);
		sfs::previewConnect(m.outputs[Pole::OUT_L_OUTPUT], 1);
		sfs::previewConnect(m.outputs[Pole::OUT_R_OUTPUT], 1);
	}
	void set(int p, float v) { m.params[p].setValue(v); }
	void step(float in) { m.inputs[Pole::IN_L_INPUT].setVoltage(in); a.frame = frame++; m.process(a); }
	float L() { return m.outputs[Pole::OUT_L_OUTPUT].getVoltage(); }
	float R() { return m.outputs[Pole::OUT_R_OUTPUT].getVoltage(); }
	// gain in dB at f for a 1 V sine, after settling
	void sineGain(float f, float& gl, float& gr, float secs = 0.4f) {
		int n = (int)(secs * SR), tail = (int)(0.15f * SR);
		double el = 0, er = 0;
		for (int i = 0; i < n; i++) {
			ph += f / SR; if (ph >= 1.0) ph -= 1.0;
			step((float)std::sin(2.0 * M_PI * ph));
			if (i >= n - tail) { el += L() * L(); er += R() * R(); }
		}
		gl = 10.f * std::log10((float)(2.0 * el / tail) + 1e-30f);
		gr = 10.f * std::log10((float)(2.0 * er / tail) + 1e-30f);
	}
	float predicted(int s, float f) {
		const Pole::Side& S = m.side[s];
		float mag = m.magAt(S, f, S.pubQs, S.pubQp) * (m.levelHold ? S.pubGain : 1.f);
		return 20.f * std::log10(std::max(mag, 1e-9f));
	}
};

// a polyBLEP saw chord, as the prototype used
struct Saw { float ph = 0, f; explicit Saw(float f_) : f(f_) {}
	float next() { float dt = f / SR, t = ph, b = 0;
		if (t < dt) { t /= dt; b = t + t - t * t - 1; } else if (t > 1 - dt) { t = (t - 1) / dt; b = t * t + t + t + 1; }
		float y = 2 * ph - 1 - b; ph += dt; if (ph >= 1) ph -= 1; return y; } };
struct Chord { Saw s[4] = {Saw(65.41f), Saw(98.f), Saw(130.81f), Saw(164.81f)};
	float next() { return 1.25f * (s[0].next() + s[1].next() + s[2].next() + s[3].next()); } };

static float lc(float hz) { return std::log2(hz / dsp::FREQ_C4); }

int main() {
	rack::random::init();

	printf("the response the screen draws is what the filter does\n");
	{
		struct Case { const char* name; float spread, width, mode, topo; int parEnd; };
		Case cases[] = {
			{"gathered series lowpass", 0.f, 0.57f, 0.f, 0.f, 0},
			{"spread series bandpass", 0.8f, 0.45f, 1.f, 0.f, 0},
			{"spread parallel bandpass", 0.8f, 0.45f, 1.f, 1.f, 0},
			{"two chains of two, highpass-ish", 0.6f, 0.5f, 1.6f, 1.f, 1},
			{"half series, notch", 0.4f, 0.6f, 3.f, 0.5f, 0},
		};
		for (const Case& c : cases) {
			Rig r;
			r.set(Pole::CUTOFF_PARAM, lc(900.f));
			r.set(Pole::SPREAD_PARAM, c.spread); r.set(Pole::WIDTH_PARAM, c.width);
			r.set(Pole::MODE_PARAM, c.mode); r.set(Pole::TOPO_PARAM, c.topo);
			r.m.parEnd = c.parEnd;
			float worst = 0.f; int n = 0;
			for (float f : {120.f, 400.f, 700.f, 900.f, 1300.f, 2500.f, 6000.f}) {
				float gl, gr; r.sineGain(f, gl, gr);
				float p = r.predicted(0, f);
				if (p > -60.f) { worst = std::max(worst, std::fabs(gl - p)); n++; }
			}
			check(n >= 3 && worst < 0.5f, string::f("%s: %d frequencies measured within %.3f dB of the drawn curve", c.name, n, worst));
		}
	}

	printf("gathered series lowpass\n");
	{
		Rig r; r.set(Pole::CUTOFF_PARAM, lc(800.f)); r.set(Pole::WIDTH_PARAM, 1.f); // widest: Q 0.40
		float g100, g6k, dummy;
		r.sineGain(100.f, g100, dummy); r.sineGain(6400.f, g6k, dummy);
		check(std::fabs(g100) < 1.5f, string::f("passes 100 Hz at %.1f dB", g100));
		check(g6k < g100 - 60.f, string::f("three octaves up is %.0f dB down (eight poles)", g100 - g6k));
	}
	{
		// the shared Q: four pairs of Q 8 gathered make one peak of about Q 8, not 8^4
		Rig r; r.set(Pole::CUTOFF_PARAM, lc(800.f));
		r.set(Pole::WIDTH_PARAM, std::log(0.18f / 0.02f) / std::log(150.f));   // 0.18 oct, Q 8
		r.m.levelHold = false;
		float pk = -999.f, base, d;
		r.sineGain(60.f, base, d);
		for (float f = 600.f; f < 1000.f; f *= 1.02f) { float g; r.sineGain(f, g, d, 0.15f); pk = std::max(pk, g); }
		float rise = pk - base;
		check(rise > 10.f && rise < 24.f, string::f("gathered Q 8 resonance rises %.1f dB over the passband (Q 8 is 18 dB; 8^4 would be 72)", rise));
	}

	printf("level across a SPREAD sweep\n");
	{
		for (int mode = 0; mode < 2; mode++) for (int hold = 0; hold < 2; hold++) {
			Rig r; Chord c;
			r.set(Pole::CUTOFF_PARAM, lc(900.f)); r.set(Pole::MODE_PARAM, (float)mode);
			r.set(Pole::WIDTH_PARAM, std::log(0.18f / 0.02f) / std::log(150.f));
			r.m.levelHold = hold;
			float lo = 1e9f, hi = -1e9f;
			int blk = (int)(0.25f * SR);
			for (int b = 0; b < 32; b++) {
				r.set(Pole::SPREAD_PARAM, 1.5f * b / 31.f);
				double e = 0;
				for (int i = 0; i < blk; i++) { r.step(c.next()); if (i > blk / 2) e += r.L() * r.L(); }
				float db = 10.f * std::log10((float)(e / (blk / 2)) + 1e-20f);
				lo = std::min(lo, db); hi = std::max(hi, db);
			}
			const char* mn = mode ? "bandpass" : "lowpass";
			if (hold) check(hi - lo < 9.f, string::f("series %s, level held: swings %.1f dB", mn, hi - lo));
			else printf("  info  series %s, uncorrected: swings %.1f dB\n", mn, hi - lo);
		}
	}

	printf("order\n");
	{
		for (int drive = 0; drive < 2; drive++) {
			Rig a, b; Chord c;
			for (Rig* r : {&a, &b}) {
				r->set(Pole::CUTOFF_PARAM, lc(700.f)); r->set(Pole::SPREAD_PARAM, 0.9f); r->set(Pole::MODE_PARAM, 1.f);
				r->set(Pole::WIDTH_PARAM, 0.35f); r->set(Pole::DRIVE_PARAM, drive ? 0.8f : 0.f);
			}
			b.m.order = 1;
			double e = 0, d = 0;
			for (int i = 0; i < 48000; i++) { float x = c.next(); a.step(x); b.step(x);
				e += a.L() * a.L(); d += (a.L() - b.L()) * (a.L() - b.L()); }
			float db = 10.f * std::log10((float)(d / std::max(e, 1e-30)));
			if (drive) check(db > -30.f, string::f("with DRIVE, reversing the order changes the output (%.0f dB)", db));
			else check(db < -90.f, string::f("without DRIVE, reversing the order changes nothing (%.0f dB)", db));
		}
	}

	printf("stereo\n");
	{
		Rig r; Chord c;
		r.set(Pole::SPREAD_PARAM, 0.5f);
		double d = 0; for (int i = 0; i < 9600; i++) { r.step(c.next()); d += std::fabs(r.L() - r.R()); }
		check(d == 0.0, "Linked: R is L exactly (and IN R is normalled from IN L)");
		auto pubs = [](Rig& r, int s, int i) { return r.m.side[s].pubF[i]; };
		r.set(Pole::STEREO_PARAM, PO_RATIO); r.set(Pole::AMOUNT_PARAM, 0.5f);
		for (int i = 0; i < 4800; i++) r.step(0.f);
		check(std::fabs(pubs(r, 1, 0) / pubs(r, 0, 0) - 2.f) < 0.01f, string::f("Offset in octaves, AMOUNT 0.5: R is %.3fx L", pubs(r, 1, 0) / pubs(r, 0, 0)));
		r.set(Pole::STEREO_PARAM, PO_HZ); r.set(Pole::AMOUNT_PARAM, 0.1f);
		for (int i = 0; i < 4800; i++) r.step(0.f);
		check(std::fabs(pubs(r, 1, 2) - pubs(r, 0, 2) - 100.f) < 0.5f, string::f("Offset in Hz, AMOUNT 0.1: R is L + %.1f Hz", pubs(r, 1, 2) - pubs(r, 0, 2)));
		r.set(Pole::STEREO_PARAM, PO_MIRROR); r.set(Pole::AMOUNT_PARAM, 0.f); r.set(Pole::SPREAD_PARAM, 0.f);
		r.set(Pole::CUTOFF_PARAM, lc(1000.f));                              // on the axis
		sfs::previewConnect(r.m.inputs[Pole::CUTOFF_INPUT], 1);
		r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(0.f);
		for (int i = 0; i < 4800; i++) r.step(0.f);
		float l0 = pubs(r, 0, 0), r0 = pubs(r, 1, 0);
		r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(1.f);
		for (int i = 0; i < 4800; i++) r.step(0.f);
		check(std::fabs(l0 / r0 - 1.f) < 0.01f, "Mirror at rest: L and R at one frequency");
		check(std::fabs(pubs(r, 0, 0) / l0 - 2.f) < 0.02f && std::fabs(r0 / pubs(r, 1, 0) - 2.f) < 0.02f,
			string::f("Mirror, +1 V: L x%.2f, R x%.2f", pubs(r, 0, 0) / l0, pubs(r, 1, 0) / r0));
		check(std::fabs(r.m.side[1].pubMode - 2.f) < 1e-4f, "Mirror turns L's lowpass into R's highpass");
		r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(0.f);
		r.set(Pole::CUTOFF_PARAM, lc(250.f));
		for (int i = 0; i < 4800; i++) r.step(0.f);
		check(std::fabs(pubs(r, 1, 0) / 4000.f - 1.f) < 0.01f, string::f("Mirror, CUTOFF knob at 250 Hz: R at %.0f Hz (the knob mirrors too)", pubs(r, 1, 0)));
		r.set(Pole::AMOUNT_PARAM, 0.5f);
		for (int i = 0; i < 4800; i++) r.step(0.f);
		check(std::fabs(pubs(r, 1, 0) / 16000.f - 1.f) < 0.01f, string::f("Mirror, AMOUNT 0.5 moves the axis to 2 kHz: R at %.0f Hz", pubs(r, 1, 0)));
		r.set(Pole::CUTOFF_PARAM, lc(800.f)); r.set(Pole::AMOUNT_PARAM, 0.f);
		r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(0.f);
		r.set(Pole::STEREO_PARAM, PO_SPREADREL); r.set(Pole::AMOUNT_PARAM, 1.f); r.set(Pole::SPREAD_PARAM, 1.f);
		for (int i = 0; i < 4800; i++) r.step(0.f);
		check(std::fabs(pubs(r, 1, 0) / pubs(r, 0, 3) - 1.f) < 0.01f, "Spread, AMOUNT 1: R's layout is L's reversed");
		r.set(Pole::AMOUNT_PARAM, 0.5f);
		for (int i = 0; i < 4800; i++) r.step(0.f);
		check(std::fabs(pubs(r, 1, 0) / pubs(r, 1, 3) - 1.f) < 0.01f, "Spread, AMOUNT 0.5: R's pairs gathered");
		// a 2-channel CUTOFF cable: R reads channel 1
		r.set(Pole::STEREO_PARAM, PO_LINKED);
		sfs::previewConnect(r.m.inputs[Pole::CUTOFF_INPUT], 2);
		r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(0.f, 0); r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(-1.f, 1);
		for (int i = 0; i < 4800; i++) r.step(0.f);
		check(std::fabs(pubs(r, 0, 0) / pubs(r, 1, 0) - 2.f) < 0.02f, "a 2-channel CUTOFF cable: channel 1 moves R only");
	}
	{
		Rig r;
		r.set(Pole::STEREO_PARAM, PO_INDEP);
		for (int i = 0; i < 32; i++) r.step(0.f);
		float lBefore = r.m.val[0][0];
		r.m.editReq = 1; r.step(0.f);
		r.set(Pole::CUTOFF_PARAM, lc(3000.f));
		for (int i = 0; i < 4800; i++) r.step(0.f);
		check(r.m.val[0][0] == lBefore && std::fabs(r.m.side[1].pubF[0] - 3000.f) < 3000.f, "Independent: editing R leaves L alone");
		check(std::fabs(r.m.val[1][0] - lc(3000.f)) < 1e-5f, "Independent: R holds the knob's new value");
		r.m.editReq = 0; r.step(0.f);
		check(std::fabs(r.m.params[Pole::CUTOFF_PARAM].getValue() - lBefore) < 1e-6f, "Independent: the L tab puts L's values back on the knobs");
		r.set(Pole::STEREO_PARAM, PO_LINKED); r.m.editReq = 1;
		for (int i = 0; i < 4; i++) r.step(0.f);
		check(r.m.edit == 0, "outside Independent the knobs always edit L");
		json_t* j = r.m.dataToJson();
		Rig q; q.m.dataFromJson(j); json_decref(j);
		check(std::fabs(q.m.val[1][0] - lc(3000.f)) < 1e-5f && q.m.rInit, "R's values survive a save");
	}

	printf("lag\n");
	{
		// driven from the CV, which does not glide, so the leading pair is exact
		Rig r;
		r.m.cvSlewMs = 0.f;                                                 // the lead pair follows the CV exactly
		r.set(Pole::SPREAD_PARAM, 0.f); r.set(Pole::LAG_PARAM, 0.6f);      // slowest pair ~70 ms
		r.set(Pole::CUTOFF_PARAM, lc(200.f));
		sfs::previewConnect(r.m.inputs[Pole::CUTOFF_INPUT], 1);
		for (int i = 0; i < 48000; i++) r.step(0.f);
		r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(4.f);
		for (int i = 0; i < 960; i++) r.step(0.f);                       // 20 ms
		const float* f = r.m.side[0].pubF;
		check(std::fabs(f[0] - 3200.f) < 5.f && f[1] < f[0] && f[2] < f[1] && f[3] < f[2],
			string::f("20 ms after a jump the pairs are fanned out: %.0f %.0f %.0f %.0f Hz", f[0], f[1], f[2], f[3]));
		for (int i = 0; i < 48000; i++) r.step(0.f);
		check(std::fabs(f[3] - 3200.f) < 10.f, "and gathered again once the move has stopped");
		r.m.lagLead = 1;
		r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(0.f);
		for (int i = 0; i < 960; i++) r.step(0.f);
		check(f[3] < f[2] && f[2] < f[1] && f[1] < f[0], "with the highest leading, pair 4 arrives first");
	}

	printf("big changes do not click\n");
	{
		// Three sines through the filter; a setting is changed in one step and the
		// output above 8 kHz in the 60 ms after is compared with the larger of
		// the two settled states'. The sines and every resonance sit below
		// 3 kHz, so what reaches 8 kHz is a step in the output: a click. (A
		// sweep through a resonance can rightly be loud for a moment; the level
		// is printed for information.)
		struct Change { const char* name; std::function<void(Rig&)> a, b; int side; };
		// Wide pairs (Q near 1), so a move does not ring a resonance at its far
		// end; ringing at 3 kHz leaks through the highpass and is not a click.
		auto base = [](Rig& r) { r.set(Pole::CUTOFF_PARAM, lc(700.f)); r.set(Pole::WIDTH_PARAM, 0.8f); };
		std::vector<Change> cs = {
			{"CUTOFF 300 Hz to 3 kHz", [&](Rig& r) { base(r); r.set(Pole::CUTOFF_PARAM, lc(300.f)); }, [](Rig& r) { r.set(Pole::CUTOFF_PARAM, lc(3000.f)); }, 0},
			{"WIDTH narrow to wide", [&](Rig& r) { base(r); r.set(Pole::WIDTH_PARAM, 0.15f); }, [](Rig& r) { r.set(Pole::WIDTH_PARAM, 0.85f); }, 0},
			{"SPREAD 0 to 1.5, series bandpass", [&](Rig& r) { base(r); r.set(Pole::MODE_PARAM, 1.f); }, [](Rig& r) { r.set(Pole::SPREAD_PARAM, 1.5f); }, 0},
			{"SPREAD 1.5 to 0, series bandpass", [&](Rig& r) { base(r); r.set(Pole::MODE_PARAM, 1.f); r.set(Pole::SPREAD_PARAM, 1.5f); }, [](Rig& r) { r.set(Pole::SPREAD_PARAM, 0.f); }, 0},
			{"MODE lowpass to highpass", [&](Rig& r) { base(r); }, [](Rig& r) { r.set(Pole::MODE_PARAM, 2.f); }, 0},
			{"MODE bandpass to notch", [&](Rig& r) { base(r); r.set(Pole::MODE_PARAM, 1.f); }, [](Rig& r) { r.set(Pole::MODE_PARAM, 3.f); }, 0},
			{"TOPOLOGY series to parallel", [&](Rig& r) { base(r); r.set(Pole::MODE_PARAM, 1.f); r.set(Pole::SPREAD_PARAM, 0.8f); }, [](Rig& r) { r.set(Pole::TOPO_PARAM, 1.f); }, 0},
			{"TOPOLOGY parallel to series", [&](Rig& r) { base(r); r.set(Pole::MODE_PARAM, 1.f); r.set(Pole::SPREAD_PARAM, 0.8f); r.set(Pole::TOPO_PARAM, 1.f); }, [](Rig& r) { r.set(Pole::TOPO_PARAM, 0.f); }, 0},
			{"DRIVE off to full", [&](Rig& r) { base(r); }, [](Rig& r) { r.set(Pole::DRIVE_PARAM, 1.f); }, 0},
			{"SHIFT 0 to +1 kHz", [&](Rig& r) { base(r); r.set(Pole::MODE_PARAM, 1.f); r.set(Pole::TOPO_PARAM, 1.f); r.set(Pole::SPREAD_PARAM, 0.5f); }, [](Rig& r) { r.set(Pole::SHIFT_PARAM, 0.8f); }, 0},
			{"STEREO Linked to Mirror (R)", [&](Rig& r) { base(r); r.set(Pole::CUTOFF_PARAM, lc(300.f)); }, [](Rig& r) { r.set(Pole::STEREO_PARAM, PO_MIRROR); r.set(Pole::AMOUNT_PARAM, 0.6f); }, 1},
			{"AMOUNT 0 to 1, octaves (R)", [&](Rig& r) { base(r); r.set(Pole::STEREO_PARAM, PO_RATIO); }, [](Rig& r) { r.set(Pole::AMOUNT_PARAM, 1.f); }, 1},
			{"series order, with DRIVE", [&](Rig& r) { base(r); r.set(Pole::MODE_PARAM, 1.f); r.set(Pole::SPREAD_PARAM, 0.9f); r.set(Pole::DRIVE_PARAM, 0.8f); }, [](Rig& r) { r.m.order = 1; }, 0},
			{"parallel end, bands to chains", [&](Rig& r) { base(r); r.set(Pole::MODE_PARAM, 1.f); r.set(Pole::SPREAD_PARAM, 0.6f); r.set(Pole::TOPO_PARAM, 1.f); }, [](Rig& r) { r.m.parEnd = 1; }, 0},
			{"pair dragged two octaves", [&](Rig& r) { base(r); r.set(Pole::MODE_PARAM, 1.f); r.set(Pole::TOPO_PARAM, 1.f); r.set(Pole::SPREAD_PARAM, 0.5f); }, [](Rig& r) { r.m.trim[0][2] = 2.f; }, 0},
		};
		for (const Change& c : cs) {
			Rig r;
			c.a(r);
			double ph1 = 0, ph2 = 0, ph3 = 0;
			auto src = [&]() {
				ph1 += 110.0 / SR; ph2 += 440.0 / SR; ph3 += 1300.0 / SR;
				return (float)(1.5 * std::sin(2 * M_PI * ph1) + 1.5 * std::sin(2 * M_PI * ph2) + 1.5 * std::sin(2 * M_PI * ph3));
			};
			// 4th-order Butterworth highpass at 8 kHz (two RBJ biquads)
			struct Bq { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
				Bq(double f, double q) { double w = 2 * M_PI * f / SR, al = std::sin(w) / (2 * q), c0 = std::cos(w), a0 = 1 + al;
					b0 = (1 + c0) / 2 / a0; b1 = -(1 + c0) / a0; b2 = b0; a1 = -2 * c0 / a0; a2 = (1 - al) / a0; }
				double tick(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
			Bq h1(8000, 0.5412), h2(8000, 1.3066);
			auto run = [&](int n, int from, float& hpk, float& apk) {
				hpk = 0; apk = 0;
				for (int i = 0; i < n; i++) {
					r.step(src());
					float y = c.side ? r.R() : r.L();
					float h = (float)h2.tick(h1.tick(y));
					if (i >= from) { hpk = std::max(hpk, std::fabs(h)); apk = std::max(apk, std::fabs(y)); }
				}
			};
			float dA, aA, dW, aW, dB, aB, tmp1, tmp2;
			run((int)(0.6f * SR), (int)(0.45f * SR), dA, aA);
			c.b(r);
			run((int)(0.06f * SR), 0, dW, aW);
			run((int)(0.6f * SR), (int)(0.45f * SR), dB, aB);
			(void)tmp1; (void)tmp2;
			float sig = std::max(std::max(aA, aB), 1e-6f);
			float click = 20.f * std::log10(std::max(dW, 1e-9f) / sig);
			float settled = 20.f * std::log10(std::max(std::max(dA, dB), 1e-9f) / sig);
			float burst = 20.f * std::log10(aW / sig);
			check(click < std::max(-60.f, settled + 10.f), string::f("%-34s above 8 kHz %6.1f dB re the signal (settled %6.1f), level %+5.1f dB", c.name, click, settled, burst));
		}
	}

	printf("an envelope with no attack opening a closed bandpass\n");
	{
		// The filter's states hold the signal it was passing; moved in one
		// sample, that signal rings at the new frequency. Measured above 8 kHz
		// in the 10 ms after the step, against the input's own level.
		float ring[2];
		for (int k = 0; k < 2; k++) {
			Rig r;
			r.m.cvSlewMs = k ? 3.f : 0.f;
			sfs::previewConnect(r.m.inputs[Pole::CUTOFF_INPUT], 1);
			r.set(Pole::CUTOFF_PARAM, -4.f); r.set(Pole::WIDTH_PARAM, 0.6f); r.set(Pole::SPREAD_PARAM, 0.4f);
			r.set(Pole::MODE_PARAM, 1.f); r.set(Pole::TOPO_PARAM, 0.27f);
			struct Bq { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
				Bq(double f, double q) { double w = 2 * M_PI * f / SR, al = std::sin(w) / (2 * q), c0 = std::cos(w), a0 = 1 + al;
					b0 = (1 + c0) / 2 / a0; b1 = -(1 + c0) / a0; b2 = b0; a1 = -2 * c0 / a0; a2 = (1 - al) / a0; }
				double tick(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
			Bq h1(8000, 0.5412), h2(8000, 1.3066);
			float ph = 0, pk = 0;
			for (int i = 0; i < 24000 + 480; i++) {
				ph += 110.f / SR; if (ph >= 1) ph -= 1;
				r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(i >= 24000 ? 8.f : 0.f);
				r.step(3.f * std::sin(2 * (float)M_PI * ph) + 2.f * std::sin(6 * (float)M_PI * ph));
				float h = std::fabs((float)h2.tick(h1.tick(r.L())));
				if (i >= 24000) pk = std::max(pk, h);
			}
			ring[k] = 20.f * std::log10(std::max(pk, 1e-9f) / 5.f);
		}
		check(ring[1] < ring[0] - 30.f, string::f("the ring above 8 kHz: %.0f dB re the input unsmoothed, %.0f dB with the default 3 ms", ring[0], ring[1]));
	}

	printf("starting points\n");
	for (int n = 0; n < PO_NPRESETS; n++) {
		const PoPreset& p = PO_PRESETS[n];
		Rig r; Chord c;
		r.m.applyPreset(n);
		double e = 0, ein = 0; float pk = 0;
		for (int i = 0; i < 48000; i++) {
			float x = c.next(); r.step(x);
			if (i > 24000) { e += r.L() * r.L() + r.R() * r.R(); ein += 2 * x * x; pk = std::max(pk, std::max(std::fabs(r.L()), std::fabs(r.R()))); }
		}
		float db = 10.f * std::log10((float)(e / ein));
		float q = r.m.cfg[0].q[0], f0 = r.m.side[0].pubF[0];
		bool ok = std::fabs(q / p.q - 1.f) < 0.02f && db > -24.f && db < 6.f && pk <= 10.f;
		if (p.spread == 0.f && p.trim[0] == 0.f && p.shiftHz == 0.f) ok = ok && std::fabs(f0 / p.cutoffHz - 1.f) < 0.01f;
		check(ok, string::f("%-34s Q %.1f, pair 1 at %.0f Hz, chord in %+.1f dB out, peak %.1f V", p.name, q, f0, db, pk));
	}

	printf("stability under fast modulation\n");
	{
		Rig r; Chord c;
		for (int p : {Pole::CUTOFF_INPUT, Pole::WIDTH_INPUT, Pole::SPREAD_INPUT, Pole::MODE_INPUT, Pole::TOPO_INPUT, Pole::SHIFT_INPUT})
			sfs::previewConnect(r.m.inputs[p], 1);
		r.set(Pole::DRIVE_PARAM, 0.5f); r.set(Pole::STEREO_PARAM, PO_MIRROR);
		bool finite = true; float peak = 0.f;
		uint32_t s = 1;
		auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / 8388608.f - 1.f; };
		for (int i = 0; i < 10 * 48000; i++) {
			if (i % 64 == 0) {
				r.m.inputs[Pole::WIDTH_INPUT].setVoltage(5.f * rnd());
				r.m.inputs[Pole::SPREAD_INPUT].setVoltage(5.f * rnd());
				r.m.inputs[Pole::MODE_INPUT].setVoltage(2.f + 2.f * rnd());
				r.m.inputs[Pole::TOPO_INPUT].setVoltage(5.f * rnd());
				r.m.inputs[Pole::SHIFT_INPUT].setVoltage(5.f * rnd());
			}
			r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(3.f * std::sin(2.f * (float)M_PI * 220.f * i / SR));   // audio-rate FM
			r.step(c.next());
			if (!std::isfinite(r.L()) || !std::isfinite(r.R())) finite = false;
			peak = std::max(peak, std::max(std::fabs(r.L()), std::fabs(r.R())));
		}
		check(finite && peak <= 10.f, string::f("10 s of random CV and audio-rate FM: finite, peak %.1f V", peak));
	}

	printf("cost\n");
	{
		for (int mod = 0; mod < 2; mod++) {
			Rig r; Chord c;
			r.set(Pole::SPREAD_PARAM, 0.6f); r.set(Pole::TOPO_PARAM, 0.5f); r.set(Pole::DRIVE_PARAM, 0.3f);
			if (mod) sfs::previewConnect(r.m.inputs[Pole::CUTOFF_INPUT], 1);
			int n = 48000 * 4;
			auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < n; i++) {
				if (mod) r.m.inputs[Pole::CUTOFF_INPUT].setVoltage(2.f * std::sin(6.28f * 3.f * i / SR));
				r.step(c.next());
			}
			double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
			printf("  info  %s, series and parallel both running: %.3f us/sample = %.2f%% of a core\n",
				mod ? "cutoff modulated" : "static", us, us * 48000.0 / 1e4);
		}
	}

	printf("%d failure(s)\n", fails);
	return fails ? 1 : 0;
}

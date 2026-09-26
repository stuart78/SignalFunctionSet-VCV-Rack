// Drives the REAL Phase -- src/phase.cpp compiled as-is against libRack, with
// synthetic samples loaded straight into its buffers -- and checks the
// per-player LEVEL and DJ FILTER, the POLY out, and the SYNC out's timing in
// every loop mode.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/phase-harness.cpp -o /tmp/phase-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/phase-harness
//
// With -DPHASE_RENDER=out.raw it instead renders the stereo out at the
// default settings, so a build of the previous commit can be compared
// sample for sample (the new controls must change nothing until touched).
// Put -I src FIRST: a source outside src/ otherwise resolves "plugin.hpp" to
// the Rack SDK's own header of that name and fails with rack types undeclared.
#ifndef PHASE_SRC
#define PHASE_SRC "../src/phase.cpp"
#endif
#include PHASE_SRC
#include <cstdio>
#include <string>
#include <vector>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }

static void load(SampleData& sd, const std::vector<float>& x) {
	sd.samples.assign(x.begin(), x.end());
	sd.length = x.size();
	sd.loopStart = 0.f; sd.loopEnd = 1.f;
	sd.loaded = true;
}
static std::vector<float> sine(float hz, float sec) {
	std::vector<float> v((size_t)(sec * SR));
	for (size_t i = 0; i < v.size(); i++) v[i] = 0.5f * std::sin(2.f * (float)M_PI * hz * i / SR);
	return v;
}
static std::vector<float> noise(float sec, uint32_t seed) {
	std::vector<float> v((size_t)(sec * SR));
	for (auto& s : v) { seed = seed * 1664525u + 1013904223u; s = ((seed >> 9) * (1.f / 8388608.f) - 1.f) * 0.5f; }
	return v;
}

struct Run {
	Phase m; rack::engine::Module::ProcessArgs a; int64_t frame = 0;
	Run() { a.sampleRate = SR; a.sampleTime = 1.f / SR; m.playing = true;
		for (auto& o : m.outputs) o.channels = 1; }
	void tick() { a.frame = frame++; m.process(a); }
};

#ifdef PHASE_RENDER
int main() {
	rack::random::init();
	Run r; load(r.m.sampleA, sine(220.f, 0.37f)); load(r.m.sampleB, noise(0.41f, 7));
	r.m.params[Phase::SLEEP_A_PARAM].setValue(40.f);
	r.m.params[Phase::PAN_A_PARAM].setValue(-0.6f); r.m.params[Phase::PAN_B_PARAM].setValue(0.3f);
	r.m.params[Phase::MODE_B_PARAM].setValue(0.f);                      // B rotates
	r.m.params[Phase::SLEEP_B_PARAM].setValue(-25.f);
	FILE* f = fopen(PHASE_RENDER, "wb");
	for (int i = 0; i < (int)(SR * 4); i++) {
		r.tick();
		float lr[2] = {r.m.outputs[Phase::LEFT_OUTPUT].getVoltage(), r.m.outputs[Phase::RIGHT_OUTPUT].getVoltage()};
		fwrite(lr, 4, 2, f);
	}
	fclose(f);
	return 0;
}
#else
// rising edges of one SYNC channel over a run, in seconds
static std::vector<double> edges(Run& r, int ch, float sec) {
	std::vector<double> t; bool was = false;
	for (int i = 0; i < (int)(sec * SR); i++) {
		r.tick();
		bool hi = r.m.outputs[Phase::SYNC_OUTPUT].getVoltage(ch) > 5.f;
		if (hi && !was) t.push_back(i / (double)SR);
		was = hi;
	}
	return t;
}
static std::string list(const std::vector<double>& t) { std::string s; for (double v : t) s += string::f(" %.4f", v); return s; }
static double rms(const std::vector<float>& v) { double e = 0; for (float x : v) e += x * x; return std::sqrt(e / std::max<size_t>(v.size(), 1)); }
// band energy of a signal by a crude DFT over a set of bins
static double band(const std::vector<float>& x, float lo, float hi) {
	const int N = 8192; rack::dsp::RealFFT fft(N); std::vector<float> in(N), sp(2 * N);
	double e = 0;
	for (size_t off = 0; off + N <= x.size(); off += N) {
		for (int i = 0; i < N; i++) in[i] = x[off + i] * (0.5f - 0.5f * std::cos(2.f * (float)M_PI * i / (N - 1)));
		fft.rfft(in.data(), sp.data());
		for (int k = 1; k < N / 2; k++) { float f = k * SR / N; if (f >= lo && f < hi) e += sp[2 * k] * sp[2 * k] + sp[2 * k + 1] * sp[2 * k + 1]; }
	}
	return e;
}

int main() {
	rack::random::init();
	// ParamQuantity reads through APP->engine
	contextSet(new Context);
	APP->engine = new engine::Engine;

	printf("== SYNC out: one trigger per loop start, on the sample the audio starts over ==\n");
	{
		Run r; load(r.m.sampleA, sine(220.f, 0.5f));
		// in anti-click mode each pass is 1 ms longer than the loop: the 1 ms
		// fade-out holds the last sample before the jump (existing behaviour)
		auto t = edges(r, 0, 2.2f);
		bool ok = t.size() == 4; for (size_t k = 1; k < t.size(); k++) ok = ok && std::fabs(t[k] - t[k - 1] - 0.501) < 2.0 / SR;
		check(ok, "sleep mode, no drift, 0.5 s loop, anti-click on: starts at" + list(t) + " (each as the fade-in begins)");
		auto tb = edges(r, 1, 0.5f);
		check(tb.empty(), string::f("channel 2 (B, empty) stays low: %d triggers", (int)tb.size()));
	}
	{
		Run r; load(r.m.sampleA, sine(220.f, 0.5f)); r.m.params[Phase::SLEEP_A_PARAM].setValue(100.f);
		auto t = edges(r, 0, 2.0f);
		bool ok = t.size() == 3; for (size_t k = 1; k < t.size(); k++) ok = ok && std::fabs(t[k] - t[k - 1] - 0.6) < 2.0 / SR;
		check(ok, "sleep mode, +100 ms drift: a start when it wakes, every 0.6 s:" + list(t));
	}
	{
		Run r; load(r.m.sampleA, sine(220.f, 0.5f)); r.m.params[Phase::MODE_A_PARAM].setValue(0.f);
		r.m.params[Phase::SLEEP_A_PARAM].setValue(30.f);
		auto t = edges(r, 0, 2.2f);
		bool ok = t.size() == 4; for (size_t k = 1; k < t.size(); k++) ok = ok && std::fabs(t[k] - t[k - 1] - 0.5) < 2.0 / SR;
		check(ok, "rotate mode (drifting content, fixed period): every 0.5 s:" + list(t));
	}
	{
		Run r; load(r.m.sampleA, sine(220.f, 0.5f)); r.m.params[Phase::SPEED_A_PARAM].setValue(-1.f);
		// the playhead begins at the loop start, so a reverse loop begins by
		// wrapping to the end: that is a start too
		auto t = edges(r, 0, 1.2f);
		check(t.size() == 3 && t[0] < 0.002 && std::fabs(t[2] - t[1] - 0.501) < 2.0 / SR, "reverse: one start per pass:" + list(t));
	}
	{
		Run r; load(r.m.sampleA, sine(220.f, 0.5f)); load(r.m.sampleB, sine(330.f, 0.7f));
		for (int i = 0; i < (int)(0.2f * SR); i++) r.tick();
		r.m.params[Phase::SYNC_PARAM].setValue(1.f);
		double tA = -1, tB = -1; bool wa = false, wb = false;
		for (int i = 0; i < (int)(0.05f * SR); i++) {
			r.tick();
			bool a = r.m.outputs[Phase::SYNC_OUTPUT].getVoltage(0) > 5.f, b = r.m.outputs[Phase::SYNC_OUTPUT].getVoltage(1) > 5.f;
			if (a && !wa && tA < 0) tA = i / (double)SR; if (b && !wb && tB < 0) tB = i / (double)SR; wa = a; wb = b;
		}
		check(tA > 0 && std::fabs(tA - tB) < 1.0 / SR && tA < 0.0015, string::f("SYNC button: both players start together, %.2f ms after the press (the 1 ms fade)", tA * 1000));
	}

	printf("== LEVEL ==\n");
	{
		auto outL = [](float lvl) {
			Run r; load(r.m.sampleA, sine(220.f, 0.5f)); r.m.params[Phase::LEVEL_A_PARAM].setValue(lvl);
			std::vector<float> v; for (int i = 0; i < (int)(0.5f * SR); i++) { r.tick(); v.push_back(r.m.outputs[Phase::LEFT_OUTPUT].getVoltage()); }
			return rms(v); };
		double unity = outL(1.f), top = outL(std::sqrt(2.f)), half = outL(std::sqrt(0.5f)), off = outL(0.f);
		check(std::fabs(20 * std::log10(top / unity) - 6.02) < 0.05 && std::fabs(20 * std::log10(half / unity) + 6.02) < 0.05 && off < 1e-9,
		      string::f("top %+.2f dB, a quarter down %+.2f dB, bottom %s", 20 * std::log10(top / unity), 20 * std::log10(half / unity), off < 1e-9 ? "silent" : "NOT silent"));
		Run q; check(q.m.paramQuantities[Phase::LEVEL_A_PARAM]->getDisplayValueString() == "+0.0 dB", "the default reads " + q.m.paramQuantities[Phase::LEVEL_A_PARAM]->getDisplayValueString());
	}

	printf("== DJ FILTER: centre is a wire, left is lowpass, right is highpass ==\n");
	{
		auto run = [](float knob, float cv, bool patchCv) {
			Run r; load(r.m.sampleA, noise(1.0f, 3)); r.m.params[Phase::FILTER_A_PARAM].setValue(knob);
			if (patchCv) { r.m.inputs[Phase::FILTER_A_INPUT].channels = 1; r.m.inputs[Phase::FILTER_A_INPUT].setVoltage(cv); }
			std::vector<float> v; for (int i = 0; i < (int)(0.9f * SR); i++) { r.tick(); v.push_back(r.m.outputs[Phase::LEFT_OUTPUT].getVoltage()); }
			return v; };
		auto dry = run(0.f, 0.f, false);
		auto lp = run(-1.f, 0.f, false), hp = run(1.f, 0.f, false), cvhp = run(0.f, 5.f, true);
		auto lpMid = run(-0.5f, 0.f, false), hpMid = run(0.5f, 0.f, false);
		double dHi = band(dry, 4000, 20000), dLo = band(dry, 20, 300);
		double lpHi = 10 * std::log10(band(lp, 4000, 20000) / dHi), hpLo = 10 * std::log10(band(hp, 20, 300) / dLo);
		check(lpHi < -40, string::f("fully left: 4-20 kHz down %.1f dB (lowpass at 60 Hz)", lpHi));
		check(hpLo < -40, string::f("fully right: 20-300 Hz down %.1f dB (highpass at 8 kHz)", hpLo));
		double lmLo = 10 * std::log10(band(lpMid, 20, 300) / dLo), lmHi = 10 * std::log10(band(lpMid, 4000, 20000) / dHi);
		double hmLo = 10 * std::log10(band(hpMid, 20, 300) / dLo), hmHi = 10 * std::log10(band(hpMid, 4000, 20000) / dHi);
		check(std::fabs(lmLo) < 1.5 && lmHi < -12 && std::fabs(hmHi) < 1.5 && hmLo < -8,
		      string::f("halfway (lowpass 1.1 kHz, highpass 400 Hz): lowpass keeps lows %+.1f dB, cuts highs %+.1f; highpass keeps highs %+.1f dB, cuts lows %+.1f", lmLo, lmHi, hmHi, hmLo));
		double diff = 0; for (size_t i = 0; i < hp.size(); i++) diff = std::max(diff, (double)std::fabs(hp[i] - cvhp[i]));
		check(diff < 1e-6, string::f("+5 V on the CV with the knob centred = the knob fully right (max difference %.1e V)", diff));
		Run q; q.m.params[Phase::FILTER_A_PARAM].setValue(-1.f);
		check(q.m.paramQuantities[Phase::FILTER_A_PARAM]->getDisplayValueString() == "lowpass 60 Hz", "fully left reads " + q.m.paramQuantities[Phase::FILTER_A_PARAM]->getDisplayValueString());
	}
	{
		// sweep the knob through the centre with a sine playing: no step at the crossing
		Run r; load(r.m.sampleA, sine(220.f, 0.5f));
		float prev = 0.f, worst = 0.f, typical = 0.f; int n = (int)(0.4f * SR);
		for (int i = 0; i < n; i++) {
			r.m.params[Phase::FILTER_A_PARAM].setValue(-0.2f + 0.4f * i / n);
			r.tick(); float y = r.m.outputs[Phase::LEFT_OUTPUT].getVoltage();
			float d = std::fabs(y - prev); if (i > 10) { worst = std::max(worst, d); typical = std::max(typical, i < n / 4 ? d : typical); } prev = y;
		}
		check(worst < typical * 1.5f, string::f("sweeping through the centre: largest sample step %.3f V against %.3f V before it (no click)", worst, typical));
	}

	printf("== POLY out: each player on its own pair, and together they are the stereo mix ==\n");
	{
		Run r; load(r.m.sampleA, sine(220.f, 0.37f)); load(r.m.sampleB, noise(0.41f, 5));
		r.m.params[Phase::PAN_A_PARAM].setValue(-1.f); r.m.params[Phase::PAN_B_PARAM].setValue(0.4f);
		r.m.params[Phase::LEVEL_B_PARAM].setValue(0.8f); r.m.params[Phase::FILTER_B_PARAM].setValue(0.3f);
		double mixErr = 0, aR = 0, aL = 0; int ch = 0;
		for (int i = 0; i < (int)(1.f * SR); i++) {
			r.tick(); Output& p = r.m.outputs[Phase::POLY_OUTPUT]; ch = p.getChannels();
			mixErr = std::max(mixErr, (double)std::fabs(p.getVoltage(0) + p.getVoltage(2) - r.m.outputs[Phase::LEFT_OUTPUT].getVoltage()));
			mixErr = std::max(mixErr, (double)std::fabs(p.getVoltage(1) + p.getVoltage(3) - r.m.outputs[Phase::RIGHT_OUTPUT].getVoltage()));
			aL += p.getVoltage(0) * p.getVoltage(0); aR += p.getVoltage(1) * p.getVoltage(1);
		}
		check(ch == 4 && mixErr < 1e-5, string::f("%d channels; A L + B L = LEFT and A R + B R = RIGHT to %.1e V", ch, mixErr));
		check(aR < aL * 1e-10, string::f("A panned hard left puts nothing on A R (%.1e of A L)", aR / std::max(aL, 1e-20)));
	}

	printf("\n%d failure(s)\n", fails);
	return fails ? 1 : 0;
}
#endif

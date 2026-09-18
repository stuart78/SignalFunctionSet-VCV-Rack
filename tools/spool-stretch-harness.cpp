// Drives the REAL Spool -- src/spool.cpp compiled as-is against libRack -- and
// measures the time-stretched pitch-shift mode: does the pitch land, how much
// does the level warble at the tap handovers (pitch-synchronous against a
// fixed window), does broadband material come through flat, does the loop keep
// its length, and what does it cost. Nothing is re-implemented.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/spool-stretch-harness.cpp -o /tmp/spool-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/spool-harness
#define DR_WAV_IMPLEMENTATION
#include "../src/spool.cpp"
#include <chrono>
#include <cstdio>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Run {
	Spool m; rack::engine::Module::ProcessArgs a; int64_t frame = 0;
	std::vector<float> out;
	bool gateHigh = false;
	Run() {
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		sfs::previewConnect(m.inputs[Spool::GATE_INPUT], 1);
		sfs::previewConnect(m.inputs[Spool::VOCT_INPUT], 1);
		m.params[Spool::ATTACK_PARAM].setValue(0.f);
		m.params[Spool::RELEASE_PARAM].setValue(0.f);
		m.params[Spool::RAMP_PARAM].setValue(0.f);
		m.params[Spool::WOW_PARAM].setValue(0.f);
		m.params[Spool::FLUTTER_PARAM].setValue(0.f);
		m.params[Spool::SAG_PARAM].setValue(0.f);
		m.params[Spool::SAT_PARAM].setValue(0.f);
	}
	void tape(const std::vector<float>& v, float detHz) {
		Spool::Tape& t = m.tape[0];
		for (size_t i = 0; i < v.size(); i++) t.buf[i] = v[i];
		t.len = (int)v.size(); t.pos = 0.0;
		t.detHz = detHz; t.detOk = detHz > 0.f;
	}
	void run(float sec, float voct, bool stretch, bool gate = true) {
		m.params[Spool::STRETCH_PARAM].setValue(stretch ? 1.f : 0.f);
		m.inputs[Spool::VOCT_INPUT].setVoltage(voct);
		// Rack's SchmittTrigger starts HIGH, so a gate that is already up when
		// the module is born never fires: hold it low for a moment first.
		if (gate && !gateHigh) {
			m.inputs[Spool::GATE_INPUT].setVoltage(0.f);
			for (int i = 0; i < 4; i++) { a.frame = frame++; m.process(a); }
		}
		gateHigh = gate;
		m.inputs[Spool::GATE_INPUT].setVoltage(gate ? 10.f : 0.f);
		int n = (int)(sec * SR);
		for (int i = 0; i < n; i++) { a.frame = frame++; m.process(a); out.push_back(m.outputs[Spool::OUT_OUTPUT].getVoltage()); }
	}
};

static std::vector<float> pitchedTape(float hz, float vibCents, float sec) {
	std::vector<float> v((size_t)(sec * SR));
	double ph = 0.0;
	for (size_t i = 0; i < v.size(); i++) {
		float t = (float)i / SR;
		float f = hz * std::pow(2.f, vibCents / 1200.f * std::sin(2.f * (float)M_PI * 5.f * t));
		ph += f / SR; if (ph >= 1.0) ph -= 1.0;
		float s = 0.f;
		for (int k = 1; k <= 12; k++) s += std::sin(2.f * (float)M_PI * (float)k * (float)ph) / (float)k;
		v[i] = s * 0.3f;
	}
	return v;
}
static std::vector<float> noiseTape(float sec) {
	std::vector<float> v((size_t)(sec * SR));
	for (auto& x : v) x = (rack::random::uniform() * 2.f - 1.f) * 0.3f;
	return v;
}

static float medianPitch(const std::vector<float>& x, int from, int to) {
	sfs::PitchTracker pt; std::vector<float> f;
	for (int at = from; at + sfs::PitchTracker::FFT_N < to; at += 4800) {
		pt.analyseSpan(x.data(), (int)x.size(), at, SR);
		if (pt.valid) f.push_back(pt.f0);
	}
	if (f.empty()) return 0.f;
	std::sort(f.begin(), f.end()); return f[f.size() / 2];
}
// level ripple: rms in 20 ms frames, reported as peak-to-peak dB and std/mean
static void ripple(const std::vector<float>& x, int from, int to, float& ppDb, float& cv) {
	int F = (int)(0.020f * SR);   // longer than a period at 110 Hz, or the frame itself ripples
	std::vector<float> r;
	for (int a = from; a + F <= to; a += F) { double e = 0; for (int i = a; i < a + F; i++) e += x[i] * x[i]; r.push_back(std::sqrt(e / F)); }
	float mn = 1e9f, mx = 0.f; double s = 0, s2 = 0;
	for (float v : r) { mn = std::min(mn, v); mx = std::max(mx, v); s += v; s2 += v * v; }
	double mean = s / r.size(); double var = s2 / r.size() - mean * mean;
	ppDb = 20.f * std::log10(mx / std::max(mn, 1e-6f)); cv = (float)(std::sqrt(std::max(var, 0.0)) / mean);
}
// spectral flatness 200 Hz - 10 kHz of averaged 2048-point power spectra
static float flatness(const std::vector<float>& x, int from, int to) {
	const int N = 2048; rack::dsp::RealFFT fft(N);
	std::vector<float> in(N), sp(2 * N), acc(N / 2, 0.f);
	int frames = 0;
	for (int a = from; a + N <= to; a += N / 2) {
		for (int i = 0; i < N; i++) in[i] = x[a + i] * (0.5f - 0.5f * std::cos(2.f * (float)M_PI * i / (N - 1)));
		fft.rfft(in.data(), sp.data());
		for (int k = 1; k < N / 2; k++) acc[k] += sp[2 * k] * sp[2 * k] + sp[2 * k + 1] * sp[2 * k + 1];
		frames++;
	}
	int k0 = (int)(200.f * N / SR), k1 = (int)(10000.f * N / SR);
	double lg = 0, ar = 0; int n = 0;
	for (int k = k0; k < k1; k++) { double p = acc[k] / frames + 1e-12; lg += std::log(p); ar += p; n++; }
	return (float)(std::exp(lg / n) / (ar / n));
}

int main() {
	rack::random::init();
	const float semis[] = {0.f, 3.f, 7.f, 12.f, -5.f, -12.f, 0.5f};
	printf("== pitched tape (220 Hz, 12 harmonics, 5 Hz vibrato +-15 c): does the pitch land, and how much does the level warble ==\n");
	printf("%8s | %-28s | %-28s | %-28s\n", "shift", "pitch-synchronous (new)", "fixed 50 ms window", "varispeed (for reference)");
	for (float st : semis) {
		float target = 220.f * std::pow(2.f, st / 12.f);
		char cols[3][64];
		for (int mode = 0; mode < 3; mode++) {
			Run r; r.tape(pitchedTape(220.f, 15.f, 3.f), mode == 1 ? 0.f : 220.f);
			r.run(2.0f, st / 12.f, mode != 2);
			int from = (int)(0.3f * SR), to = (int)r.out.size();
			float hz = medianPitch(r.out, from, to); float pp, cv; ripple(r.out, from, to, pp, cv);
			float cents = hz > 0.f ? 1200.f * std::log2(hz / target) : 0.f;
			snprintf(cols[mode], 64, "%+5.0f c  pp %4.1f dB  cv %.3f", cents, pp, cv);
		}
		printf("%+7.1f  | %-28s | %-28s | %-28s\n", st, cols[0], cols[1], cols[2]);
	}
	printf("   (a tape with vibrato has a level ripple of its own; the varispeed column is that floor)\n");

	printf("\n== broadband tape (white noise): spectral flatness in 200 Hz - 10 kHz, 1.0 is white; is the shifter a comb? ==\n");
	{
		Run r0; std::vector<float> nz = noiseTape(3.f); r0.tape(nz, 0.f);
		printf("  the tape itself                 %.3f\n", flatness(nz, 0, (int)nz.size()));
		const float sh[] = {0.f, 0.02f, 7.f / 12.f, -7.f / 12.f, 1.f, -1.f};
		for (float v : sh) {
			Run r; r.tape(nz, 0.f); r.run(2.0f, v, true);
			printf("  stretched, V/OCT %+6.3f V        %.3f\n", v, flatness(r.out, (int)(0.3f * SR), (int)r.out.size()));
		}
		// unity reached from elsewhere: sweep in, then stop at 0 V and wait
		Run r; r.tape(nz, 0.f); r.run(0.5f, 0.3f, true); r.run(3.0f, 0.f, true);
		printf("  stretched, 0 V after a shift     %.3f (last second)\n", flatness(r.out, (int)(2.5f * SR), (int)r.out.size()));
		const float vs[] = {0.f, 1.f, -1.f};
		for (float v : vs) {
			Run rv; rv.tape(nz, 0.f); rv.run(2.0f, v, false);
			printf("  varispeed, V/OCT %+6.3f V        %.3f\n", v, flatness(rv.out, (int)(0.3f * SR), (int)rv.out.size()));
		}
	}

	printf("\n== when the detector is wrong: the same tape at +7 st with the period it was told, ripple as above ==\n");
	{
		std::vector<float> tp = pitchedTape(220.f, 15.f, 3.f);
		sfs::PitchTracker pt; std::vector<float> f;
		for (int k = 0; k < 8; k++) { pt.analyseSpan(tp.data(), (int)tp.size(), (int)(k * tp.size() / 8), SR); if (pt.valid) f.push_back(pt.f0); }
		std::sort(f.begin(), f.end());
		printf("  the tracker itself says %.1f Hz for this tape\n", f.empty() ? 0.f : f[f.size() / 2]);
		struct C { const char* name; float hz; } cases[] = {
			{"exact 220", 220.f}, {"an octave high, 440", 440.f}, {"an octave low, 110", 110.f},
			{"5% sharp, 231", 231.f}, {"a fifth off, 330", 330.f}, {"nothing detected", 0.f}};
		for (auto& c : cases) {
			Run rr; rr.tape(tp, c.hz); rr.run(2.0f, 7.f / 12.f, true);
			float pp, cv; ripple(rr.out, (int)(0.3f * SR), (int)rr.out.size(), pp, cv);
			printf("  %-22s pp %4.1f dB  cv %.3f\n", c.name, pp, cv);
		}
	}

	printf("\n== the loop keeps its length: transport position after exactly one tape length at -1 octave ==\n");
	{
		std::vector<float> tp = pitchedTape(220.f, 0.f, 2.f);
		for (int mode = 0; mode < 2; mode++) {
			Run r; r.tape(tp, 220.f);
			r.run((float)tp.size() / SR, -1.f, mode == 1);
			double p = r.m.tape[0].head[0].pos;
			printf("  %-10s head at %.3f of the tape after one lap's worth of time\n", mode ? "stretched" : "varispeed", p / tp.size());
		}
	}

	printf("\n== cost: eight heads on one tape, percent of one core at 48 kHz ==\n");
	{
		for (int mode = 0; mode < 2; mode++) {
			Run r; r.tape(pitchedTape(220.f, 10.f, 3.f), 220.f);
			sfs::previewConnect(r.m.inputs[Spool::GATE_INPUT], 8);
			sfs::previewConnect(r.m.inputs[Spool::VOCT_INPUT], 8);
			for (int c = 0; c < 8; c++) { r.m.inputs[Spool::GATE_INPUT].setVoltage(10.f, c); r.m.inputs[Spool::VOCT_INPUT].setVoltage(0.1f * c, c); }
			r.m.params[Spool::STRETCH_PARAM].setValue(mode ? 1.f : 0.f);
			double t0 = now(); int n = (int)(2.f * SR);
			for (int i = 0; i < n; i++) { r.a.frame = r.frame++; r.m.process(r.a); }
			double t = now() - t0;
			printf("  %-10s %.2f%%\n", mode ? "stretched" : "varispeed", 100.0 * t / 2.0);
		}
	}
	return 0;
}

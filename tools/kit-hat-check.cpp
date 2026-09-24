// Checks Kit's hi-hat engine (src/kit-hat.hpp) against the reference it was
// ported from (tools/hat-plate-harness.cpp, round Q1), case by case, with the
// harness's own measurements. The contact is chaotic, so the two renders are
// not sample-identical after the first collisions; what must agree is every
// measurement: decay shape, flatness, band timing, contacts, centroid.
//
//   clang++ -std=c++11 -O3 -ffast-math -I src tools/kit-hat-check.cpp -o /tmp/khc && /tmp/khc [outdir]
//
// Also reports the engine's cost, and the peak level of a Kit-velocity hit so
// outGain can be set against the membrane.
#define main harness_main
#include "hat-plate-harness.cpp"
#undef main
#include "../src/kit-hat.hpp"

static std::vector<float> renderEngine(sfs::Hat& e, float pedal, float speed, float hard, float secs,
                                       std::vector<float>& R, double& us) {
	e.pedalT = pedal; e.control(0, true); e.clear();
	int N = (int)(secs * SR);
	std::vector<float> L(N); R.assign(N, 0.f);
	e.strike(speed, hard, 1.f);
	auto t0 = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < N; i++) {
		float o[2]; e.process(o, true); L[i] = o[0]; R[i] = o[1];
		if ((i & 31) == 31) e.control(32);
	}
	auto t1 = std::chrono::high_resolution_clock::now();
	us = std::chrono::duration<double, std::micro>(t1 - t0).count() / N;
	return L;
}

int main(int argc, char** argv) {
	std::string out = argc > 1 ? argv[1] : ".";
	// the reference, at its defaults (Q1)
	Hat ref; gHat = nullptr;
	ref.build(0.85f); ref.calibrate();
	sfs::Hat eng; eng.init(SR);
	printf("reference: %zu + %zu modes, strike scale %.4g, eRef %.4g\n", ref.top.modes.size(), ref.bot.modes.size(), ref.strikeScale, ref.eRef);
	printf("engine:    %d + %d modes, strike scale %.4g, eRef %.4g\n", eng.top.n, eng.bot.n, eng.strikeScale0, eng.eRef0);
	struct Case { const char* name; float pedal; float secs; };
	Case cases[] = {{"closed", 0.f, 1.f}, {"half open", 0.35f, 2.f}, {"open", 1.f, 2.5f}};
	int fails = 0;
	for (auto& c : cases) {
		std::vector<float> L, R;
		Result a = run(ref, c.pedal, 1.f, 0.35f, c.secs, &L, &R);
		double us; std::vector<float> R2;
		// Kit's speed for a vel-1 reference hit is 6 (speed / 6 = 1), hardest beater
		std::vector<float> L2 = renderEngine(eng, c.pedal, 6.f, 1.f, c.secs, R2, us);
		Result b = measure(L2, R2); b.us = us;
		std::string nm = std::string(c.name);
		report((nm + " (reference)").c_str(), a);
		report((nm + " (engine)").c_str(), b);
		std::string fn = out + "/engine-" + nm + ".wav"; for (auto& ch : fn) if (ch == ' ') ch = '_';
		wav(fn, L2, R2);
		// the verdict: decay within 3 dB at every checkpoint to 400 ms, flatness within 0.05
		for (int t = 0; t < 6; t++)
			if (a.db[t] > -80 && std::fabs(a.db[t] - b.db[t]) > 3.f) { printf("   !! decay at checkpoint %d differs: %.1f vs %.1f dB\n", t, a.db[t], b.db[t]); fails++; }
		if (std::fabs(a.flat - b.flat) > 0.05f) { printf("   !! flatness 2-16k differs\n"); fails++; }
		if (std::fabs(a.tonal - b.tonal) > 0.05f) { printf("   !! flatness 0.5-5k differs\n"); fails++; }
	}
	// level: a default-velocity (0.7) hit from Kit is speed 0.4 + 9 * 0.7
	{
		std::vector<float> R; double us;
		std::vector<float> L = renderEngine(eng, 1.f, 0.4f + 9.f * 0.7f, 1.f, 0.5f, R, us);
		float pk = 0; for (float x : L) pk = std::max(pk, std::fabs(x));
		printf("\npeak of a vel-0.7 open hit at outGain 1: %.4g\n", pk);
	}
	printf(fails ? "\nFAIL %d\n" : "\nPASS\n", fails);
	return fails;
}

// Sigma CPU bench and render check: every preset played the same way (a held
// four-note chord plus a melody retriggering over it, so long releases stack up
// to all sixteen voices), timed, and rendered so two builds can be compared.
//
//   clang++ -std=c++11 -O3 -funsafe-math-optimizations -fno-omit-frame-pointer -DARCH_MAC -I src \
//     -I ../Rack-SDK/include -I ../Rack-SDK/dep/include tools/sigma-bench.cpp -o /tmp/sigma-bench \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/sigma-bench [render-dir]
//
// With a render directory it writes <dir>/<preset>.raw (float L,R per frame);
// compare two directories with tools/perf/compare.py, file by file.
// (-I src FIRST, or "plugin.hpp" resolves to the Rack SDK's own header.)
#ifndef SIGMA_SRC
#define SIGMA_SRC "../src/sigma.cpp"
#endif
#include SIGMA_SRC
#include <chrono>
#include <cstdio>

rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;

// 6 s: a four-note chord held throughout on channels 0-3, and a melody on
// channels 4-7 struck every 250 ms and held for 120 ms, so released notes
// pile up behind it.
static double play(Sigma* m, float secs, FILE* out, int* peakVoices) {
	Module::ProcessArgs a; a.sampleRate = SR; a.sampleTime = 1.f / SR;
	m->inputs[Sigma::GATE_INPUT].channels = 8;
	m->inputs[Sigma::VOCT_INPUT].channels = 8;
	m->outputs[Sigma::L_OUTPUT].channels = 1;
	m->outputs[Sigma::R_OUTPUT].channels = 1;
	static const float chord[4] = {-1.f, -1.f + 4 / 12.f, -1.f + 7 / 12.f, 0.f};
	static const int mel[8] = {0, 7, 12, 3, 10, 5, 14, 2};
	int n = (int)(secs * SR), period = (int)(SR * 0.25f), held = (int)(SR * 0.12f);
	double busy = 0;
	*peakVoices = 0;
	for (int i = 0; i < n; i++) {
		for (int c = 0; c < 4; c++) {
			m->inputs[Sigma::GATE_INPUT].setVoltage(10.f, c);
			m->inputs[Sigma::VOCT_INPUT].setVoltage(chord[c], c);
		}
		int step = i / period, ph = i % period, ch = 4 + step % 4;
		for (int c = 4; c < 8; c++) {
			bool on = c == ch && ph < held;
			m->inputs[Sigma::GATE_INPUT].setVoltage(on ? 10.f : 0.f, c);
			if (on) m->inputs[Sigma::VOCT_INPUT].setVoltage(mel[step % 8] / 12.f, c);
		}
		a.frame = i;
		auto t0 = std::chrono::steady_clock::now();
		m->process(a);
		busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (out) {
			float f[2] = {m->outputs[Sigma::L_OUTPUT].getVoltage(), m->outputs[Sigma::R_OUTPUT].getVoltage()};
			fwrite(f, sizeof f, 1, out);
		}
		if ((i & 1023) == 0) {
			int v = 0;
			for (int k = 0; k < SG_VOICES; k++) v += m->voice[k].mStage != Sigma::ST_IDLE;
			*peakVoices = std::max(*peakVoices, v);
		}
	}
	return busy / secs;
}

int main(int argc, char** argv) {
	const char* dir = argc > 1 ? argv[1] : nullptr;
	double total = 0;
	printf("preset  partials  peak voices  %% of one core\n");
	// SIGMA_PRESET=n SIGMA_SECS=s: one preset for s seconds, to profile
	int only = getenv("SIGMA_PRESET") ? atoi(getenv("SIGMA_PRESET")) : -99;
	float secs = getenv("SIGMA_SECS") ? (float)atof(getenv("SIGMA_SECS")) : 6.f;
	for (int p = -1; p < SG_NPRESET; p++) {
		if (only != -99 && p != only) continue;
		Sigma* m = new Sigma();
		if (p >= 0) m->loadPreset(p);
		FILE* f = nullptr;
		if (dir) {
			char path[512]; snprintf(path, sizeof path, "%s/%02d.raw", dir, p + 1);
			f = fopen(path, "wb");
		}
		int pv;
		double c = play(m, secs, f, &pv);
		if (f) fclose(f);
		total += c;
		printf("%6d  %8d  %11d  %6.2f%%\n", p, m->nPartials, pv, c * 100.0);
		delete m;
	}
	printf("mean %.2f%%\n", total * 100.0 / (SG_NPRESET + 1));
}

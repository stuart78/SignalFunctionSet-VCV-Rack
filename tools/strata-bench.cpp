// Strata CPU bench and render check: the real module against libRack, timed
// over a set of settings at one voice and at sixteen, with the outputs written
// so two builds can be compared sample for sample.
//
//   clang++ -std=c++11 -O3 -funsafe-math-optimizations -fno-omit-frame-pointer -DARCH_MAC -march=armv8-a+fp+simd \
//     -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include tools/strata-bench.cpp -o /tmp/strata-bench \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/strata-bench [render-dir]
//
// STRATA_CASE=name STRATA_SECS=s runs one case for s seconds, to attach a profiler.
// (-I src FIRST, or "plugin.hpp" resolves to the Rack SDK's own header.)
#define DR_WAV_IMPLEMENTATION
#ifndef STRATA_SRC
#define STRATA_SRC "../src/strata.cpp"
#endif
#include STRATA_SRC
#include <chrono>
#include <cstdio>
#include <cstring>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;

struct Case {
	const char* name;
	int voices;
	// param, value pairs, terminated by -1
	int p[16];
	float v[16];
	bool moving;      // the point travels: X swept by an LFO on every voice
};

static const Case CASES[] = {
	{"plain",          1, {-1}, {0}, false},
	{"plain-16",      16, {-1}, {0}, false},
	{"moving",         1, {-1}, {0}, true},
	{"moving-16",     16, {-1}, {0}, true},
	{"warp-sync",      1, {Strata::WARP_PARAM, Strata::SYNC_PARAM, -1}, {0.6f, 0.7f}, false},
	{"fold",           1, {Strata::FOLD_PARAM, -1}, {0.8f}, false},
	{"crush",          1, {Strata::CRUSH_PARAM, -1}, {0.6f}, false},
	{"shuffle",        1, {Strata::SHUFFLE_PARAM, -1}, {0.8f}, false},
	{"rate-start",     1, {Strata::RATE_PARAM, Strata::START_PARAM, -1}, {0.7f, 0.3f}, false},
	{"everything",     1, {Strata::WARP_PARAM, Strata::SYNC_PARAM, Strata::FOLD_PARAM, Strata::CRUSH_PARAM,
	                       Strata::SHUFFLE_PARAM, Strata::RATE_PARAM, Strata::START_PARAM, -1},
	                      {0.6f, 0.7f, 0.8f, 0.6f, 0.8f, 0.7f, 0.3f}, true},
	// FOLD on and off and CRUSH on, a second each: the x4 chain starting and
	// stopping mid-note (renders must match a build that always runs it)
	{"steps",          4, {-1}, {0}, true},
	{"everything-16", 16, {Strata::WARP_PARAM, Strata::SYNC_PARAM, Strata::FOLD_PARAM, Strata::CRUSH_PARAM,
	                       Strata::SHUFFLE_PARAM, Strata::RATE_PARAM, Strata::START_PARAM, -1},
	                      {0.6f, 0.7f, 0.8f, 0.6f, 0.8f, 0.7f, 0.3f}, true},
};

static double runCase(const Case& c, float secs, FILE* out) {
	Strata* m = new Strata();
	m->loadFactory(3); m->waitLoaded();
	Module::ProcessArgs a; a.sampleRate = SR; a.sampleTime = 1.f / SR;
	for (int o = 0; o < Strata::OUTPUTS_LEN; o++) m->outputs[o].channels = 1;
	m->inputs[Strata::VOCT_INPUT].channels = c.voices;
	if (c.moving) m->inputs[Strata::X_INPUT].channels = c.voices;
	for (int k = 0; c.p[k] >= 0; k++) m->params[c.p[k]].setValue(c.v[k]);
	m->params[Strata::Y_PARAM].setValue(0.43f); m->params[Strata::Z_PARAM].setValue(0.6f);
	for (int v = 0; v < c.voices; v++) m->inputs[Strata::VOCT_INPUT].setVoltage(v * 0.07f - 0.5f, v);
	int n = (int)(secs * SR);
	double busy = 0;
	for (int i = 0; i < n; i++) {
		if (!std::strcmp(c.name, "steps")) {
			int sec = i / (int)SR;
			m->params[Strata::FOLD_PARAM].setValue(sec == 1 || sec == 3 ? 0.4f : 0.f);
			m->params[Strata::CRUSH_PARAM].setValue(sec == 5 ? 0.5f : 0.f);
		}
		if (c.moving)
			for (int v = 0; v < c.voices; v++)
				m->inputs[Strata::X_INPUT].setVoltage(3.f * std::sin(2.f * (float)M_PI * (0.3f + 0.05f * v) * i / SR), v);
		a.frame = i;
		auto t0 = std::chrono::steady_clock::now();
		m->process(a);
		busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (out) {
			float f[3] = {m->outputs[Strata::OUT_OUTPUT].getVoltage(0), m->outputs[Strata::LEFT_OUTPUT].getVoltage(),
			              m->outputs[Strata::RIGHT_OUTPUT].getVoltage()};
			fwrite(f, sizeof f, 1, out);
		}
	}
	delete m;
	return busy / secs;
}

int main(int argc, char** argv) {
	rack::random::init();
	// the FPU flags Rack sets on its engine threads (denormals flushed to
	// zero): without them a decaying tail runs into denormals and times slow
	rack::system::resetFpuFlags();
	contextSet(new Context);
	APP->engine = new engine::Engine;
	const char* dir = argc > 1 ? argv[1] : nullptr;
	const char* only = getenv("STRATA_CASE");
	float secs = getenv("STRATA_SECS") ? (float)atof(getenv("STRATA_SECS")) : 3.f;
	printf("case            voices   %% of one core\n");
	for (const Case& c : CASES) {
		if (only && std::strcmp(only, c.name)) continue;
		FILE* f = nullptr;
		if (dir) { char p[512]; snprintf(p, sizeof p, "%s/%s.raw", dir, c.name); f = fopen(p, "wb"); }
		double cpu = runCase(c, secs, f);
		if (f) fclose(f);
		printf("%-15s %6d   %6.2f%%\n", c.name, c.voices, cpu * 100.0);
	}
}

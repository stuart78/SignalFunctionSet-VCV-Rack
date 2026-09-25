// Desktop CPU benchmark for every module in the plugin.
//
// Builds against the Rack SDK (tools/perf/build.sh), instantiates each
// registered module outside any engine and times process() three ways:
//
//   idle   nothing patched in, every output patched
//   stim   a generic stimulus: 4 Hz gates (50 ms) on anything that reads like a
//          gate/trigger/clock, an eight-note melody on V/OCT, noise on audio ins
//   poly8  the same with eight channels on every gate and V/OCT input, gates
//          staggered across the period -- the worst a poly sequencer can do
//
// Reported as % of ONE core at 48 kHz, which is what Rack's own meter shows
// for a module (time in process() over the block's real-time budget). Each
// case is the best of several runs: Apple Silicon moves a busy process between
// performance and efficiency cores, and the minimum is the P-core cost.
// Expanders are measured alone, with nothing to talk to. The stimulus itself is
// counted: a module with many inputs (PolyKit In: 112, so 896 writes a sample
// at poly8) reads higher than its own work.
//
//   tools/perf/build.sh && tools/perf/build/bench [slug]
//   BENCH_CASE=poly8 BENCH_SECONDS=300 tools/perf/build/bench Kit &   # then: sample <pid> 5
//   (BENCH_CASE runs just that case, so a profiler attached mid-run sees it)
//
// RENDER MODE, for checking an optimisation changed nothing audible:
//   BENCH_RENDER=out.raw BENCH_CASE=stim BENCH_SECONDS=5 bench Slide
// seeds Rack's random generator, builds the module, runs the case and writes
// every output's channel 0 and the sum of its channels. tools/perf/compare.py
// compares two such files (the last commit's build against this one).
#include "plugin.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

void init(Plugin* p);

static const float SR = 48000.f;

static std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	return s;
}
static bool has(const std::string& s, std::initializer_list<const char*> ws) {
	for (const char* w : ws) if (s.find(w) != std::string::npos) return true;
	return false;
}

enum Role { NONE, GATE, VOCT, AUDIO };

// What an input is, from its name. CV-ish controls stay unpatched: a benchmark
// should not wander a module through its whole parameter space.
static Role roleOf(const std::string& nameRaw) {
	std::string n = lower(nameRaw);
	if (has(n, {"reset", "cv", "bank", "voice", "scale", "root", "select"}))
		if (!has(n, {"gate", "trig", "clock", "ping"})) return NONE;
	if (has(n, {"gate", "trig", "clock", "ping", "strike", "pluck", "excite", "startle", "strum", "hit"})) return GATE;
	if (has(n, {"v/oct", "voct", "1v/oct", "pitch"})) return VOCT;
	if (has(n, {"audio", "left", "right", "in l", "in r", "signal", "input l", "input r"}) || n == "in" || n == "input")
		return AUDIO;
	return NONE;
}

static int POLY = 1;

// Seconds of CPU per second of audio.
static double runFor(engine::Module* m, float seconds, bool stim, const std::vector<Role>& roles) {
	engine::Module::ProcessArgs a;
	a.sampleRate = SR;
	a.sampleTime = 1.f / SR;
	uint32_t rng = 0x12345u;
	int n = (int)(seconds * SR);
	int period = (int)(SR * 0.25f);            // 4 Hz: a busy but musical clock
	int gateLen = (int)(SR * 0.05f);
	static const int melody[8] = {0, 7, 12, 3, 10, 5, 14, 2};
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < n; i++) {
		if (stim) {
			int ph = i % period, step = (i / period) % 8;
			for (size_t k = 0; k < roles.size(); k++) {
				switch (roles[k]) {
					case GATE:
						for (int c = 0; c < POLY; c++) {
							int pc = (ph + c * period / POLY) % period;
							m->inputs[k].setVoltage(pc < gateLen ? 10.f : 0.f, c);
						}
						break;
					case VOCT:
						for (int c = 0; c < POLY; c++)
							m->inputs[k].setVoltage(melody[(step + c) % 8] / 12.f - 1.f + (c % 3) * 0.25f, c);
						break;
					case AUDIO: rng = rng * 1664525u + 1013904223u;
					            m->inputs[k].setVoltage(((rng >> 9) * (1.f / 8388608.f) - 1.f) * 5.f); break;
					default: break;
				}
			}
		}
		a.frame = i;
		m->process(a);
	}
	auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double>(t1 - t0).count() / seconds;
}

// Render one case to a file. Header: int32 output count; then per frame, per
// output, float channel 0 and float sum of all channels.
static int render(Model* model, const char* path, const char* cs, float secs) {
	// identical draws in both builds; BENCH_SEED gives a second performance, to
	// measure how much two renders differ from randomness alone
	uint64_t seed = getenv("BENCH_SEED") ? (uint64_t)atoll(getenv("BENCH_SEED")) : 0xC0FFEEu;
	random::local().seed(0x5F5E1u, seed);
	engine::Module* m = model->createModule();
	engine::Module::SampleRateChangeEvent e; e.sampleRate = SR; e.sampleTime = 1.f / SR;
	m->onSampleRateChange(e);
	for (auto& o : m->outputs) o.channels = 1;
	std::vector<Role> roles(m->inputs.size(), NONE);
	bool stim = std::strcmp(cs, "idle") != 0;
	if (stim)
		for (size_t k = 0; k < m->inputs.size(); k++) {
			roles[k] = roleOf(m->inputInfos[k]->name);
			if (roles[k] == NONE) continue;
			bool poly = !std::strcmp(cs, "poly8") && (roles[k] == GATE || roles[k] == VOCT);
			m->inputs[k].channels = poly ? 8 : 1;
		}
	POLY = !std::strcmp(cs, "poly8") ? 8 : 1;
	FILE* f = std::fopen(path, "wb");
	if (!f) return 1;
	int32_t no = (int32_t)m->outputs.size();
	std::fwrite(&no, 4, 1, f);
	engine::Module::ProcessArgs a; a.sampleRate = SR; a.sampleTime = 1.f / SR;
	uint32_t rng = 0x12345u;
	int n = (int)(secs * SR), period = (int)(SR * 0.25f), gateLen = (int)(SR * 0.05f);
	static const int melody[8] = {0, 7, 12, 3, 10, 5, 14, 2};
	std::vector<float> fr(2 * no);
	for (int i = 0; i < n; i++) {
		if (stim) {
			int ph = i % period, step = (i / period) % 8;
			for (size_t k = 0; k < roles.size(); k++) {
				if (roles[k] == GATE)
					for (int c = 0; c < POLY; c++) { int pc = (ph + c * period / POLY) % period; m->inputs[k].setVoltage(pc < gateLen ? 10.f : 0.f, c); }
				else if (roles[k] == VOCT)
					for (int c = 0; c < POLY; c++) m->inputs[k].setVoltage(melody[(step + c) % 8] / 12.f - 1.f + (c % 3) * 0.25f, c);
				else if (roles[k] == AUDIO) { rng = rng * 1664525u + 1013904223u; m->inputs[k].setVoltage(((rng >> 9) * (1.f / 8388608.f) - 1.f) * 5.f); }
			}
		}
		a.frame = i;
		m->process(a);
		for (int k = 0; k < no; k++) {
			Output& o = m->outputs[k];
			float s = 0.f; for (int c = 0; c < std::max(1, (int)o.channels); c++) s += o.voltages[c];
			fr[2 * k] = o.voltages[0]; fr[2 * k + 1] = s;
		}
		std::fwrite(fr.data(), 4, fr.size(), f);
	}
	std::fclose(f);
	POLY = 1;
	delete m;
	return 0;
}

static double best(engine::Module* m, float secs, int runs, bool stim, const std::vector<Role>& roles) {
	double b = 1e9;
	for (int r = 0; r < runs; r++) b = std::min(b, runFor(m, secs, stim, roles));
	return b;
}

int main(int argc, char** argv) {
	random::init();
	// Some modules ask APP->engine for the sample rate in their constructors.
	contextSet(new Context);
	APP->engine = new engine::Engine;
	APP->engine->setSampleRate(SR);
	Plugin p;
	p.slug = "SignalFunctionSet";
	p.path = argc > 2 ? argv[2] : ".";       // the checkout, for res/
	const char* only = argc > 1 && std::strcmp(argv[1], "all") ? argv[1] : nullptr;
	init(&p);
	if (const char* out = getenv("BENCH_RENDER")) {
		const char* cs = getenv("BENCH_CASE") ? getenv("BENCH_CASE") : "stim";
		float rs = getenv("BENCH_SECONDS") ? (float)atof(getenv("BENCH_SECONDS")) : 5.f;
		for (Model* model : p.models) if (only && model->slug == only) return render(model, out, cs, rs);
		std::fprintf(stderr, "no module %s\n", only ? only : "(none given)");
		return 1;
	}
	const char* secsEnv = getenv("BENCH_SECONDS");
	float secs = secsEnv ? (float)atof(secsEnv) : 1.0f;
	int runs = secsEnv ? 1 : 5;
	std::printf("%-14s %8s %8s %8s   %% of one core at 48 kHz, one module alone\n", "module", "idle", "stim", "poly8");
	for (Model* model : p.models) {
		if (only && model->slug != only) continue;
		std::fflush(stdout);
		engine::Module* m = model->createModule();
		engine::Module::SampleRateChangeEvent e;
		e.sampleRate = SR;
		e.sampleTime = 1.f / SR;
		m->onSampleRateChange(e);
		for (auto& o : m->outputs) o.channels = 1;      // every output patched
		std::vector<Role> roles(m->inputs.size(), NONE);
		const char* only1 = getenv("BENCH_CASE");
		auto want = [&](const char* c) { return !only1 || !std::strcmp(only1, c); };
		runFor(m, 0.2f, false, roles);                   // warm
		double idle = want("idle") ? best(m, secs, runs, false, roles) : 0.0;
		for (size_t k = 0; k < m->inputs.size(); k++) {
			roles[k] = roleOf(m->inputInfos[k]->name);
			if (roles[k] != NONE) m->inputs[k].channels = 1;
		}
		runFor(m, 0.5f, true, roles);
		double stim = want("stim") ? best(m, secs * 2.f, runs, true, roles) : 0.0;
		for (size_t k = 0; k < m->inputs.size(); k++)
			if (roles[k] == GATE || roles[k] == VOCT) m->inputs[k].channels = 8;
		POLY = 8;
		runFor(m, 0.5f, true, roles);
		double poly = want("poly8") ? best(m, secs * 2.f, std::max(1, runs - 2), true, roles) : 0.0;
		POLY = 1;
		std::printf("%-14s %7.2f%% %7.2f%% %7.2f%%\n", model->slug.c_str(), idle * 100, stim * 100, poly * 100);
		delete m;
	}
	return 0;
}

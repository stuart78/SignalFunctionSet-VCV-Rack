// Cistern CPU bench and render check: the real module against libRack, every
// space timed in its steady state (after the worker has traced it and the
// crossfade has ended), plus the moment after a SPACE change, when the old
// room rings out beside the new one. Renders are written per space so two
// builds can be compared.
//
//   clang++ -std=c++11 -O3 -funsafe-math-optimizations -fno-omit-frame-pointer -DARCH_MAC -march=armv8-a+fp+simd \
//     -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include tools/cistern-bench.cpp -o /tmp/cistern-bench \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/cistern-bench [render-dir]
//
// Run from the repository root (it reads res/spaces). CISTERN_SPACE=n
// CISTERN_SECS=s times one space for s seconds of audio, to attach a profiler.
#ifndef CISTERN_SRC
#define CISTERN_SRC "../src/cistern.cpp"
#endif
#include CISTERN_SRC
#include <chrono>
#include <cstdio>
#include <thread>

rack::plugin::Plugin* pluginInstance = nullptr;

struct Rig {
	Cistern* m;
	Module::ProcessArgs a;
	int64_t frame = 0;
	Rig() {
		m = new Cistern();
		m->inputs[Cistern::IN_L_INPUT].channels = 1;
		m->inputs[Cistern::IN_R_INPUT].channels = 1;
		m->outputs[Cistern::OUT_L_OUTPUT].channels = 1;
		m->outputs[Cistern::OUT_R_OUTPUT].channels = 1;
		a.sampleRate = 48000.f; a.sampleTime = 1.f / 48000.f;
	}
	~Rig() { delete m; }
	void step(float l, float r, float& ol, float& orr) {
		m->inputs[Cistern::IN_L_INPUT].setVoltage(l);
		m->inputs[Cistern::IN_R_INPUT].setVoltage(r);
		a.frame = frame++;
		m->process(a);
		ol = m->outputs[Cistern::OUT_L_OUTPUT].getVoltage();
		orr = m->outputs[Cistern::OUT_R_OUTPUT].getVoltage();
	}
	// run silence in real time until `pred` holds (the worker is a real thread)
	template <typename F> bool waitFor(F pred, double maxSec = 30) {
		auto t0 = std::chrono::steady_clock::now();
		float l, r;
		while (!pred()) {
			for (int i = 0; i < 64; i++) step(0.f, 0.f, l, r);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			if (getenv("CISTERN_DEBUG") && (int)(el * 1000) % 2000 < 2)
				printf("    %.0fs: rv %d fading %d incoming %d newRv %d ringing %d (env %.2g) tracing %d oldRv %d retired %d\n", el,
				       m->rv != nullptr, m->rv && m->rv->fading(), m->incoming.load() != nullptr, m->newRv.load() != nullptr,
				       m->ringing != nullptr, m->ringEnv, (int)m->tracing.load(), m->oldRv.load() != nullptr, m->retired.load() != nullptr);
			if (el > maxSec) return false;
		}
		return true;
	}
	int traced = 0, adopted = 0;      // while timing: samples of the worker tracing, programs adopted
	bool settled() { return m->rv && !m->rv->fading() && m->incoming.load() == nullptr && m->newRv.load() == nullptr
	                        && !m->ringing && !m->tracing.load(); }
	// (newRv too: the worker publishes the new space's readout before the audio
	// thread takes the new reverb, and timing then caught the old room ringing out)
	// seconds of CPU per second of audio, over `secs` of seeded noise on both inputs
	double time(float secs, FILE* out) {
		cistern::Rng nz(7);
		int n = (int)(secs * 48000.f);
		double busy = 0;
		float l, r;
		traced = 0;
		double slice = 0;
		bool cont = getenv("CISTERN_CONTINUOUS") != nullptr;   // noise throughout, never silent
		const cistern::Program* last = m->rv ? m->rv->cur : nullptr;
		for (int i = 0; i < n; i++) {
			if ((i & 4095) == 0) {
				if (m->tracing.load()) traced++;
				if (m->rv && m->rv->cur != last) { last = m->rv->cur; adopted++; }
			}
			// bursts: half a second of noise, a second and a half of tail
			bool on = cont || (i % 96000) < 24000;
			float a0 = on ? (float)(nz.u() * 2 - 1) * 3.f : 0.f, b0 = on ? (float)(nz.u() * 2 - 1) * 3.f : 0.f;
			auto t0 = std::chrono::steady_clock::now();
			step(a0, b0, l, r);
			double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			busy += dt; slice += dt;
			if (getenv("CISTERN_SLICES") && i % 48000 == 47999) {
				printf("    second %d: %.2f%%%s\n", i / 48000, slice * 100.0, m->rv && m->rv->fading() ? " (crossfading)" : "");
				slice = 0;
			}
			if (out) { float f[2] = {l, r}; fwrite(f, sizeof f, 1, out); }
		}
		return busy / secs;
	}
};

int main(int argc, char** argv) {
	pluginInstance = new rack::plugin::Plugin();
	// the FPU flags Rack sets on its engine threads (denormals flushed to
	// zero): without them a decaying tail runs into denormals and times slow
	rack::system::resetFpuFlags();
	pluginInstance->path = ".";
	const char* dir = argc > 1 ? argv[1] : nullptr;
	int only = getenv("CISTERN_SPACE") ? atoi(getenv("CISTERN_SPACE")) : -1;
	// CISTERN_ORDER=5,6: these spaces in this order (to look for one leaving something behind)
	std::vector<int> order;
	if (const char* o = getenv("CISTERN_ORDER")) { for (const char* c = o; *c; ) { order.push_back(atoi(c)); while (*c && *c != ',') c++; if (*c) c++; } }
	else for (int id = 0; id < NSPACES; id++) if (only < 0 || id == only) order.push_back(id);
	float secs = getenv("CISTERN_SECS") ? (float)atof(getenv("CISTERN_SECS")) : 8.f;
	printf("space                            taps   T60 500Hz   %% of one core\n");
	Rig g;
	g.m->params[Cistern::MIX_PARAM].setValue(1.f);
	for (int id : order) {
		cistern::Reverb* before = g.m->rv;
		g.m->params[Cistern::SPACE_PARAM].setValue((float)id);
		// the old room rings out for as long as its own decay says (over a
		// minute after the cistern); fade it, so each space is timed alone
		// wait for the new reverb itself: the readout (dSpace) is published
		// before the worker hands it over
		bool ok = g.m->rv == before && g.m->dSpace.load() == id && g.settled();
		ok = ok || g.waitFor([&]() { return g.m->rv != before && g.m->dSpace.load() == id && g.m->newRv.load() == nullptr; }, 60);
		g.m->dropRinging();
		ok = ok && g.waitFor([&]() { return g.settled(); }, 60);
		// let the old room ring out and the worker go idle before timing
		ok = ok && g.waitFor([&]() { return g.settled(); });
		if (!ok) { printf("%-32s  did not settle\n", cistern::SPACES[id].name); continue; }
		FILE* f = nullptr;
		if (dir) { char p[512]; snprintf(p, sizeof p, "%s/%02d-%s.raw", dir, id, cistern::SPACES[id].slug); f = fopen(p, "wb"); }
		double c = g.time(secs, f);
		if (f) fclose(f);
		printf("%-32s %5d   %6.1f s     %6.2f%%%s\n", cistern::SPACES[id].name, (int)g.m->rv->cur->taps.size(), g.m->dT60[2].load(), c * 100.0,
		       g.traced || g.adopted ? string::f("   (worker busy: traced %d, adopted %d)", g.traced, g.adopted).c_str() : "");
		g.adopted = 0;
	}
	if (only < 0 && !getenv("CISTERN_ORDER")) {
		// a SPACE change: the old room rings out beside the new one
		g.m->params[Cistern::SPACE_PARAM].setValue(0.f);
		g.waitFor([&]() { return g.m->dSpace.load() == 0 && g.settled(); });
		float l, r;
		for (int i = 0; i < 48000; i++) g.step(1.f, -1.f, l, r);
		g.m->params[Cistern::SPACE_PARAM].setValue(2.f);
		g.waitFor([&]() { return g.m->dSpace.load() == 2 && g.m->ringing != nullptr; });
		double c = g.time(1.f, nullptr);
		printf("%-32s                     %6.2f%%\n", "while the old room rings out", c * 100.0);
	}
}

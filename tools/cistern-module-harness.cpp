// Drives the REAL Cistern -- src/cistern.cpp compiled as-is against libRack,
// worker thread and all -- and checks the parts the engine harness cannot see:
// that the worker's first program arrives and plays, that moving a source or
// turning DECAY hands a new program over and the old one is freed, that FREEZE
// holds the tail, that a sample-rate change rebuilds, and what it costs.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/cistern-module-harness.cpp -o /tmp/cistern-module-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/cistern-module-harness
// (-I src FIRST, or "plugin.hpp" resolves to the Rack SDK's own header.)
#include "../src/cistern.cpp"
#include <chrono>
#include <cstdio>
#include <string>

rack::plugin::Plugin* pluginInstance = nullptr;
static int fails = 0;
static void check(bool ok, const std::string& what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }

struct Rig {
	Cistern* m;
	Module::ProcessArgs a;
	int64_t frame = 0;
	explicit Rig(float sr = 48000.f) {
		m = new Cistern();
		m->inputs[Cistern::IN_L_INPUT].channels = 1;
		m->outputs[Cistern::OUT_L_OUTPUT].channels = 1;
		m->outputs[Cistern::OUT_R_OUTPUT].channels = 1;
		m->params[Cistern::MIX_PARAM].setValue(1.f);     // wet only
		setRate(sr);
	}
	~Rig() { delete m; }
	void setRate(float sr) { a.sampleRate = sr; a.sampleTime = 1.f / sr; }
	void step(float in, float& l, float& r) {
		m->inputs[Cistern::IN_L_INPUT].setVoltage(in);
		a.frame = frame++;
		m->process(a);
		l = m->outputs[Cistern::OUT_L_OUTPUT].getVoltage();
		r = m->outputs[Cistern::OUT_R_OUTPUT].getVoltage();
	}
	// run silence in real time until `pred` holds (the worker is a real thread)
	template <typename F> bool waitFor(F pred, double maxSec = 10) {
		auto t0 = std::chrono::steady_clock::now();
		float l, r;
		while (!pred()) {
			for (int i = 0; i < 64; i++) step(0.f, l, r);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > maxSec) return false;
		}
		return true;
	}
	double energy(int n, float in, std::vector<float>* keep = nullptr) {
		double e = 0; float l, r;
		for (int i = 0; i < n; i++) { step(in, l, r); e += 0.5 * (l * l + r * r); if (keep) keep->push_back(l); }
		return e / n;
	}
};

int main() {
	pluginInstance = new rack::plugin::Plugin();
	pluginInstance->path = ".";                        // res/spaces from the repository
	printf("== the first program\n");
	Rig g;
	auto t0 = std::chrono::steady_clock::now();
	bool got = g.waitFor([&]() { return g.m->rv != nullptr; });
	double first = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	check(got, string::f("the worker's first reverb arrives (%.2f s)", first));
	if (!got) return 1;
	check(g.m->dT60[0].load() > 30.f, string::f("the screen's readout is the cistern (T60 at 125 Hz %.1f s)", g.m->dT60[0].load()));

	printf("== it plays\n");
	{
		float l, r;
		g.step(5.f, l, r);                                     // an impulse
		std::vector<float> L, R;
		for (int i = 0; i < 48000 * 2; i++) { g.step(0.f, l, r); L.push_back(l); R.push_back(r); }
		double e1 = 0, e2 = 0, ll = 0, rr = 0, lr = 0;
		for (int i = 24000; i < 48000; i++) e1 += L[i] * L[i];
		for (int i = 72000; i < 96000; i++) e2 += L[i] * L[i];
		for (int i = 24000; i < 96000; i++) { ll += L[i] * L[i]; rr += R[i] * R[i]; lr += L[i] * R[i]; }
		check(e1 > 1e-9, string::f("the impulse rings (%.2e at 0.5-1 s)", e1));
		double drop = 10 * std::log10(e1 / e2);
		check(drop > 0.5 && drop < 8, string::f("and decays as a long room does: %.1f dB from 0.5-1 s to 1.5-2 s", drop));
		check(std::fabs(lr / std::sqrt(ll * rr)) < 0.5, string::f("left and right decorrelated (%.2f)", lr / std::sqrt(ll * rr)));
	}

	printf("== a source moved while playing\n");
	{
		cistern::Program* before = const_cast<cistern::Program*>(g.m->rv->cur);
		g.m->pos[0][Cistern::P_SRC_L][0] += 0.2f; g.m->posGen++;
		bool adopted = g.waitFor([&]() { return g.m->rv->cur != before; });
		check(adopted, "a new program is traced and adopted");
		bool freed = g.waitFor([&]() { return !g.m->rv->fading() && g.m->retired.load() == nullptr; });
		check(freed, "the crossfade ends and the old program is freed");
		cistern::Program* b2 = const_cast<cistern::Program*>(g.m->rv->cur);
		uint32_t tid = b2->traceId;
		g.m->params[Cistern::DECAY_PARAM].setValue(-1.f);
		adopted = g.waitFor([&]() { return g.m->rv->cur != b2; });
		check(adopted && g.m->rv->cur->traceId == tid, "a DECAY change reuses the trace");
		check(std::fabs(g.m->dT60[2].load() - g.m->dBase[2].load() * 0.5f) < 0.05f * g.m->dBase[2].load(),
		      string::f("and halves the decay (%.1f s at 500 Hz)", g.m->dT60[2].load()));
		g.m->params[Cistern::DECAY_PARAM].setValue(0.f);
		g.waitFor([&]() { return !g.m->rv->fading() && g.m->rv->cur->decay > 0.99 && g.m->incoming.load() == nullptr; });
	}

	printf("== another space\n");
	{
		// a tail ringing in the cistern when the space changes
		cistern::Rng nz(11); float l0, r0;
		for (int i = 0; i < 24000; i++) g.step((float)(nz.u() * 2 - 1) * 5.f, l0, r0);
		cistern::Reverb* oldRvP = g.m->rv;
		cistern::Program* before = const_cast<cistern::Program*>(g.m->rv->cur);
		g.m->params[Cistern::SPACE_PARAM].setValue(1.f);    // the next space
		bool adopted = g.waitFor([&]() { return g.m->rv->cur != before && g.m->dSpace.load() == 1; }, 20);
		check(adopted && g.m->ringing == oldRvP, string::f("the cistern's reverb is left ringing out, and %s gets a new one", cistern::SPACES[1].name));
		{
			// what rings is the cistern's tail at its own pitch: it is the same
			// object, fed nothing, with its decay untouched
			double e0 = g.energy(4800, 0.f), e1 = g.energy(48000, 0.f);
			check(e1 > 1e-6 && 10 * std::log10(e0 / e1) < 6, string::f("its tail carries on (%.1f dB down over the next second)", 10 * std::log10(e0 / e1)));
		}
		check(adopted, string::f("switching to %s traces it and adopts it (T60 at 500 Hz %.1f s)", cistern::SPACES[1].name, g.m->dT60[2].load()));
		check(g.m->posSet[1] && Cistern::standable(1, g.m->pos[1][Cistern::P_LIS]), "its default positions are filled in and stand in the air");
		float l, r; double e = 0;
		g.step(5.f, l, r);
		for (int i = 0; i < 48000; i++) { g.step(0.f, l, r); e += l * l; }
		check(e > 1e-9, "and it plays");
		cistern::Program* b2 = const_cast<cistern::Program*>(g.m->rv->cur);
		g.waitFor([&]() { return !g.m->rv->fading() && g.m->incoming.load() == nullptr; });
		auto t0 = std::chrono::steady_clock::now();
		g.m->pos[1][Cistern::P_SRC_L][0] += 0.05f; g.m->posGen++;
		adopted = g.waitFor([&]() { return g.m->rv->cur != b2; });
		double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		check(adopted && dt < 1.5, string::f("a moved source in it is retraced in %.2f s (the decay measurement reused)", dt));
		cistern::Reverb* pantheonRv = g.m->rv;
		g.m->params[Cistern::SPACE_PARAM].setValue(0.f);
		g.waitFor([&]() { return g.m->dSpace.load() == 0 && !g.m->rv->fading() && g.m->incoming.load() == nullptr; }, 20);
		bool swapped = g.waitFor([&]() { return g.m->ringing == pantheonRv; }, 5);
		check(swapped, "switching again fades the older ringing reverb and leaves the second space's ringing");
		g.m->dropRinging();
		check(g.waitFor([&]() { return g.m->ringing == nullptr; }, 5), "and a ringing reverb is handed back to be freed");
	}

	printf("== freeze\n");
	{
		cistern::Rng nz(5);
		float l, r;
		for (int i = 0; i < 48000; i++) g.step((float)(nz.u() * 2 - 1) * 5.f, l, r);
		g.m->params[Cistern::FREEZE_PARAM].setValue(1.f);
		g.energy(24000, 0.f);                                  // the early field clears (it is not frozen, only the tail)
		double a = g.energy(48000, 0.f);
		double b = g.energy(48000 * 2, 3.f);                  // input ignored while frozen
		check(std::fabs(10 * std::log10(b / a)) < 1.0, string::f("the tail holds, input shut out (%+.2f dB over 2 s)", 10 * std::log10(b / a)));
		g.m->params[Cistern::FREEZE_PARAM].setValue(0.f);
		double c = g.energy(48000, 0.f);
		double d = g.energy(48000, 0.f);
		check(10 * std::log10(c / d) > 0.2, string::f("and decays again once released (%.1f dB a second)", 10 * std::log10(c / d)));
	}

	printf("== cost\n");
	{
		cistern::Rng nz(9);
		float l, r;
		auto c0 = std::chrono::steady_clock::now();
		int n = 48000 * 5;
		for (int i = 0; i < n; i++) g.step((float)(nz.u() * 2 - 1), l, r);
		double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count() / 5.0 * 100;
		check(cpu < 8, string::f("%.2f%% of a core at 48 kHz, two sources, %d taps", cpu, (int)g.m->rv->cur->taps.size()));
	}

	printf("== the screen's model of every space\n");
	{
		CisternDisplay* d = new CisternDisplay();
		d->module = g.m;
		for (int id = 0; id < NSPACES; id++) {
			g.m->params[Cistern::SPACE_PARAM].setValue((float)id);
			g.m->wSpace = id;
			auto t0 = std::chrono::steady_clock::now();
			d->sync();
			double ms = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000;
			int floors = 0; for (auto& t : d->mf) floors += t.kind == 0;
			int folds = 0; for (auto& e : d->me) folds += e.fold;
			if (!d->room->clean) { floors = 1; folds = (int)d->segs.size(); }
			if (const char* dir = getenv("CISTERN_SVG")) {
				// the room as the screen paints it, through the same code, into an SVG
				d->box.size = mm2px(Vec(101.6f, 63.5f));
				struct SvgSink : CisternDisplay::RoomSink {
					FILE* f;
					void fill(const float* xy, int n, NVGcolor c) override {
						fprintf(f, "<polygon fill='rgb(%d,%d,%d)' fill-opacity='%.2f' points='", (int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), c.a);
						for (int i = 0; i < n; i++) fprintf(f, "%.1f,%.1f ", xy[i * 2], xy[i * 2 + 1]);
						fprintf(f, "'/>\n");
					}
					std::vector<std::pair<float, std::string>> lines;
					void line(float x0, float y0, float x1, float y1, NVGcolor c, float w, float t) override {
						char b[256];
						snprintf(b, sizeof(b), "<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='rgb(%d,%d,%d)' stroke-width='%.2f' stroke-linecap='round'/>\n",
						         x0, y0, x1, y1, (int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), w);
						lines.push_back({t, b});
					}
					void flush() {
						std::stable_sort(lines.begin(), lines.end(), [](const std::pair<float, std::string>& a, const std::pair<float, std::string>& b) { return a.first < b.first; });
						for (auto& l : lines) fputs(l.second.c_str(), f);
					}
				} sink;
				sink.f = fopen((std::string(dir) + "/" + cistern::SPACES[id].slug + ".svg").c_str(), "w");
				fprintf(sink.f, "<svg xmlns='http://www.w3.org/2000/svg' width='%.0f' height='%.0f'><rect width='100%%' height='100%%' fill='#1a1a32'/>\n", d->box.size.x, d->box.size.y);
				d->paintRoom(sink);
				sink.flush();
				CisternView V = d->view();
				for (int i = 0; i < Cistern::NPOS; i++) {
					float x, y, z, X, Y, dd; d->world(i, x, y, z); V.project(x, y, z, X, Y, dd);
					fprintf(sink.f, "<circle cx='%.1f' cy='%.1f' r='3.5' fill='%s'/>\n", X, Y, i < 2 ? "#ec652e" : "#e8e8f0");
				}
				fprintf(sink.f, "</svg>\n"); fclose(sink.f);
			}
			bool okModel = !d->room->clean ? d->segs.size() > 200 : (d->mf.size() > 100 && floors > 0);
			check(okModel, string::f("%-28s %s, built in %.0f ms", cistern::SPACES[id].name,
			      !d->room->clean ? string::f("%zu section segments", d->segs.size()).c_str() : string::f("%zu faces, %d edges", d->mf.size(), folds).c_str(), ms));
		}
		g.m->params[Cistern::SPACE_PARAM].setValue(0.f);
		g.m->wSpace = 0;
		// (not deleted: a widget's destructor reaches for Rack's app, which a harness has none of)
	}

	printf("== a sample-rate change\n");
	{
		g.setRate(96000.f);
		bool ok = g.waitFor([&]() { return g.m->rv && g.m->rv->sr == 96000; });
		check(ok, "the worker rebuilds the reverb at 96 kHz");
		float l, r; double e = 0;
		g.step(5.f, l, r);
		for (int i = 0; i < 96000; i++) { g.step(0.f, l, r); e += l * l; }
		check(e > 1e-9, "and it plays");
	}
	printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
	return fails ? 1 : 0;
}

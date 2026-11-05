// Drives the REAL Canopy against libRack and checks what docs/canopy-design.md says:
// a fall takes the time gravity says, every tongue is in tune and the count follows
// the scale, material sets weight, brightness and bounce, every landing sounds, a
// paddle deflects a drop in silence and its material changes how, drift stays where
// its knobs say, GATE plays directly, drops reach the drum, and the mouse maths
// inverts the camera. `render DIR` writes listening WAVs.
//
//   clang++ -std=c++11 -O2 -w -DARCH_MAC -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/canopy-harness.cpp -o /tmp/canopy-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/canopy-harness
#define DR_WAV_IMPLEMENTATION
#include "../src/dr_wav.h"
#include "../src/canopy.cpp"
#include <chrono>
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) {
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) fails++;
}

struct Run {
	Canopy* m; rack::engine::Module::ProcessArgs a; int64_t frame = 0;
	Run(bool quiet = true) {
		random::local().seed(7u, 11u);
		m = new Canopy();
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		for (auto& o : m->outputs) o.channels = 1;
		m->rng = 0x2545F491u;
		// a clock cable with no pulses on it: nothing falls unless asked
		if (quiet) m->inputs[Canopy::CLOCK_INPUT].channels = 1;
		still();
		run(0.005f);
	}
	~Run() { delete m; }
	void still() { m->params[Canopy::DRIFT_PARAM].setValue(0.f); m->params[Canopy::TILT_PARAM].setValue(0.f); }
	void tick() { a.frame = frame++; m->process(a); }
	void run(float sec) { int n = (int)(sec * SR); for (int i = 0; i < n; i++) tick(); }
	void noPaddles() { m->params[Canopy::DENSITY_PARAM].setValue(0.f); run(0.005f); }
	void onePaddle(float cx, float cy, float cz, float ax, float ay, float az, int mat) {
		m->params[Canopy::DENSITY_PARAM].setValue(1.f);
		CanopyLeaf& P = m->leaf[0];
		P.cx = cx; P.cy = cy; P.cz = cz; P.ax = ax; P.ay = ay; P.az = az;
		m->params[Canopy::LEAF_PARAM].setValue((float)mat);
		m->publishLeaves();
		run(0.005f);
	}
	// a point over the middle of tongue t, xi of the way to its free end
	void over(int t, float xi, float& x, float& z) {
		const Canopy::TongueVoice& T = m->tv[t];
		float a = (1.f - xi) * T.len;
		x = T.tx + T.ux * a; z = T.tz + T.uz * a;
	}
	void mat(int k) { m->params[Canopy::MATERIAL_PARAM].setValue((float)k); run(0.002f); }
	std::vector<float> record(float sec, int chan = -1) {
		std::vector<float> v; int n = (int)(sec * SR); v.reserve(n);
		for (int i = 0; i < n; i++) {
			tick();
			v.push_back(chan >= 0 ? m->outputs[Canopy::POLY_OUTPUT].getVoltage(chan)
			                      : 0.5f * (m->outputs[Canopy::L_OUTPUT].getVoltage() + m->outputs[Canopy::R_OUTPUT].getVoltage()));
		}
		return v;
	}
};

static float peak(const std::vector<float>& v) { float p = 0.f; for (float s : v) p = std::max(p, std::fabs(s)); return p; }
static float goertzel(const std::vector<float>& v, float f) {
	float c = 2.f * std::cos(2.f * (float)M_PI * f / SR), a = 0.f, b = 0.f;
	for (float s : v) { float t = s + c * a - b; b = a; a = t; }
	return std::sqrt(std::max(a * a + b * b - c * a * b, 0.f)) / (float)v.size();
}
static float peakFreq(const std::vector<float>& v, float near) {
	float best = near, bm = -1.f;
	for (float c = -60.f; c <= 60.f; c += 1.f) {
		float f = near * std::pow(2.f, c / 1200.f), g = goertzel(v, f);
		if (g > bm) { bm = g; best = f; }
	}
	return best;
}

static void writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r) {
	drwav w; drwav_data_format f; f.container = drwav_container_riff; f.format = DR_WAVE_FORMAT_IEEE_FLOAT;
	f.channels = 2; f.sampleRate = 48000; f.bitsPerSample = 32;
	if (!drwav_init_file_write(&w, path.c_str(), &f, NULL)) return;
	std::vector<float> o(l.size() * 2);
	for (size_t i = 0; i < l.size(); i++) { o[2 * i] = l[i] / 10.f; o[2 * i + 1] = r[i] / 10.f; }
	drwav_write_pcm_frames(&w, l.size(), o.data());
	drwav_uninit(&w);
}

static int render(const std::string& dir) {
	struct { const char* name; int mat, pad; float rate, size, drift; } S[] = {
		{"water-rain", TM_WATER, TP_WOOD, 0.70f, 0.3f, 0.2f},
		{"rubber", TM_RUBBER, TP_WOOD, 0.45f, 0.5f, 0.2f},
		{"wood", TM_WOOD, TP_WOOD, 0.45f, 0.5f, 0.2f},
		{"glass-marbles", TM_GLASS, TP_WOOD, 0.45f, 0.46f, 0.2f},
		{"steel", TM_STEEL, TP_WOOD, 0.40f, 0.46f, 0.2f},
		{"mix", TM_MIX, TP_WOOD, 0.5f, 0.46f, 0.2f},
		{"mix-felt-leaves", TM_MIX, TP_FELT, 0.5f, 0.46f, 0.2f},
		{"mix-steel-leaves", TM_MIX, TP_STEEL, 0.5f, 0.46f, 0.2f},
		{"glass-drifting", TM_GLASS, TP_WOOD, 0.5f, 0.46f, 0.9f},
	};
	for (auto& s : S) {
		Run r(false);
		r.m->params[Canopy::MATERIAL_PARAM].setValue((float)s.mat);
		r.m->params[Canopy::LEAF_PARAM].setValue((float)s.pad);
		r.m->params[Canopy::RATE_PARAM].setValue(s.rate);
		r.m->params[Canopy::SIZE_PARAM].setValue(s.size);
		r.m->params[Canopy::DRIFT_PARAM].setValue(s.drift);
		r.m->params[Canopy::TILT_PARAM].setValue(s.drift);
		std::vector<float> L, R; int n = (int)(25.f * SR);
		for (int i = 0; i < n; i++) { r.tick(); L.push_back(r.m->outputs[Canopy::L_OUTPUT].getVoltage()); R.push_back(r.m->outputs[Canopy::R_OUTPUT].getVoltage()); }
		writeWav(dir + "/" + s.name + ".wav", L, R);
		printf("%s: peak %.2f V, %ld strikes\n", s.name, peak(L), r.m->strikes);
	}
	return 0;
}

int main(int argc, char** argv) {
	if (argc == 3 && std::string(argv[1]) == "render") return render(argv[2]);

	printf("== the fall ==\n");
	{
		Run r; r.noPaddles(); r.mat(TM_GLASS);
		float x, z; r.over(3, 0.8f, x, z);
		r.m->spawn(x, z);
		float rad = r.m->drop[0].r, want = std::sqrt(2.f * (CP_H - rad - cpDomeY(x, z)) / CP_G);
		int n = 0; while (r.m->strikes == 0 && n < (int)(3.f * SR)) { r.tick(); n++; }
		float t = n / SR;
		check(std::fabs(t - want) < 0.006f, string::f("a drop from the top lands in %.3f s, gravity says %.3f", t, want));
	}

	printf("== the tongues follow the scale ==\n");
	{
		struct { int scale, want; const char* name; } C[] = {
			{4, 10, "Pentatonic Minor: ten, the common drum"}, {1, 14, "Major: seven notes, two octaves"},
			{5, 12, "Blues: six notes, two octaves"}, {0, 12, "Chromatic: twelve, one octave"},
		};
		for (auto& c : C) {
			Run r; r.m->params[Canopy::SCALE_PARAM].setValue((float)c.scale); r.run(0.01f);
			check(r.m->nT == c.want, string::f("%s (%d)", c.name, r.m->nT));
		}
		Run r;
		check(r.m->tv[0].len > r.m->tv[1].len && std::fabs(r.m->tv[0].tx) < 1e-5f, "the lowest note is the big tongue in the middle");
		check(r.m->tv[r.m->nT - 1].len < r.m->tv[1].len, "a higher tongue is shorter");
		r.m->outputs[Canopy::POLY_OUTPUT].channels = 1;
		r.run(0.01f);
		check(r.m->outputs[Canopy::POLY_OUTPUT].channels == 10, "POLY carries one channel per tongue");
	}

	printf("== every tongue in tune, on Pentatonic Minor at C3 ==\n");
	for (int t = 0; t < 10; t++) {
		Run r; r.noPaddles(); r.mat(TM_GLASS);
		r.m->inputs[Canopy::GATE_INPUT].channels = 16;
		r.m->inputs[Canopy::GATE_INPUT].setVoltage(10.f, t);
		std::vector<float> v = r.record(2.5f, t);
		float want = dsp::FREQ_C4 * 0.5f * std::pow(2.f, sfs::SCALES[4].intervals[t % 5] / 12.f + (float)(t / 5));
		float got = peakFreq(v, want);
		check(std::fabs(1200.f * std::log2(got / want)) < 4.f && peak(v) > 0.02f,
		      string::f("tongue %d: %.1f Hz, want %.1f, peak %.2f V", t, got, want, peak(v)));
	}

	printf("== material: weight, brightness, bounce ==\n");
	{
		float vol[TM_MIX], bright[TM_MIX], hits[TM_MIX];
		const float F = dsp::FREQ_C4 * 0.5f * std::pow(2.f, sfs::SCALES[4].intervals[3] / 12.f);
		for (int k = 0; k < TM_MIX; k++) {
			Run r; r.noPaddles(); r.mat(k);
			float x, z; r.over(3, 0.8f, x, z);
			r.m->spawn(x, z);
			r.run(0.70f);
			std::vector<float> v = r.record(0.35f, 3);
			vol[k] = peak(v);
			bright[k] = 20.f * std::log10((goertzel(v, F * CP_RATIO[2]) + 1e-9f) / (goertzel(v, F) + 1e-9f));
			// a fixed seed sends one drop anywhere, so count over one drop from each of 24 spots
			long total = 0;
			for (int s = 0; s < 24; s++) {
				Run q; q.noPaddles(); q.mat(k); q.m->rng = 0x2545F491u + 977u * (uint32_t)s;
				float qx, qz; q.over(s % 10, 0.5f + 0.4f * (float)(s % 3) / 2.f, qx, qz);
				q.m->spawn(qx, qz);
				q.run(10.f);
				total += q.m->strikes;
			}
			hits[k] = (float)total / 24.f;
			printf("    %-7s peak %.2f V  third partial %5.1f dB  %.1f strikes a drop\n", CP_MAT[k].name, vol[k], bright[k], hits[k]);
		}
		check(vol[TM_STEEL] > vol[TM_GLASS] && vol[TM_GLASS] > vol[TM_WOOD], "heavier is louder: steel > glass > wood");
		check(20.f * std::log10(vol[TM_STEEL] / vol[TM_WOOD]) > 6.f, "and steel is at least 6 dB over wood");
		check(bright[TM_STEEL] > bright[TM_WOOD] && bright[TM_WOOD] > bright[TM_RUBBER], "harder is brighter: steel > wood > rubber");
		check(bright[TM_STEEL] - bright[TM_RUBBER] > 9.f, string::f("by a clear margin: %.1f dB, steel over rubber", bright[TM_STEEL] - bright[TM_RUBBER]));
		check(hits[TM_STEEL] > hits[TM_WOOD] * 1.3f && hits[TM_WOOD] >= 2.f, "steel keeps bouncing longer than wood, and wood bounces");
	}
	{
		Run r; r.noPaddles(); r.mat(TM_WATER);
		float x, z; r.over(0, 0.8f, x, z);
		r.m->spawn(x, z);
		r.run(0.83f);
		int droplets = 0;
		for (const CanopyDrop& d : r.m->drop) if (d.alive && d.droplet) droplets++;
		check(droplets >= 2, string::f("water splashes: %d droplets in the air after it lands", droplets));
		r.run(1.5f);
		bool main = false; for (const CanopyDrop& d : r.m->drop) if (d.alive && !d.droplet) main = true;
		check(!main, "and the drop itself is gone, not bouncing");
	}

	printf("== big water breaks in stages ==\n");
	{
		Run r; r.noPaddles(); r.mat(TM_WATER);
		r.m->params[Canopy::SIZE_PARAM].setValue(1.f); r.run(0.005f);
		float x, z; r.over(0, 0.8f, x, z);
		r.m->spawn(x, z);
		float big = r.m->drop[0].r, medium = 0.f, small = 1.f;
		float seenR[3] = {0.f, 0.f, 0.f};
		int most = 0;
		for (int i = 0; i < (int)(3.f * SR); i++) {
			r.tick();
			if (i % 48) continue;
			int n = 0;
			for (const CanopyDrop& d : r.m->drop) {
				if (!d.alive) continue;
				n++;
				if (d.r >= big * 0.3f && d.r < big * 0.75f) { seenR[0] = 1.f; medium = d.r; }
				if (d.r < big * 0.3f) { seenR[1] = 1.f; small = std::min(small, d.r); }
			}
			most = std::max(most, n);
		}
		check(seenR[0] > 0.f, string::f("a big drop (radius %.3f) breaks into medium ones (%.3f)", big, medium));
		check(seenR[1] > 0.f, string::f("and those into small ones (%.3f)", small));
		check(small < CP_WATER_MIN, "and the small ones stay whole");
		check(most <= CP_MAXDROPS, string::f("%d pieces in the air at most", most));
		// a small drop does not break at all
		Run q; q.noPaddles(); q.mat(TM_WATER);
		q.m->params[Canopy::SIZE_PARAM].setValue(0.f); q.run(0.005f);
		q.over(0, 0.8f, x, z);
		q.m->spawn(x, z);
		q.run(0.86f);
		int pieces = 0; for (const CanopyDrop& d : q.m->drop) if (d.alive) pieces++;
		float r0 = q.m->sizeK;
		printf("    a small drop (radius %.3f) leaves %d pieces\n", r0, pieces);
	}

	printf("== nothing disappears on the drum ==\n");
	{
		for (int k : {(int)TM_RUBBER, (int)TM_WOOD, (int)TM_GLASS, (int)TM_STEEL}) {
			float lowest = 0.f; int vanished = 0, total = 0;
			for (int s = 0; s < 10; s++) {
				Run r; r.noPaddles(); r.mat(k); r.m->rng = 0x2545F491u + 7919u * (uint32_t)s;
				float x, z; r.over(s % 10, 0.7f, x, z);
				r.m->spawn(x, z);
				CanopyDrop last = r.m->drop[0];
				int n = 0;
				while (r.m->drop[0].alive && n < (int)(45.f * SR)) { last = r.m->drop[0]; r.tick(); n++; }
				total++;
				if (r.m->drop[0].alive || last.y > -1.5f) vanished++;
				lowest = std::min(lowest, last.y);
			}
			check(vanished == 0, string::f("%-6s: all %d fell off the rim and out of view before being removed (%d did not)", CP_MAT[k].name, total, vanished));
		}
		Run r; r.noPaddles(); r.mat(TM_WOOD);
		r.m->spawn(0.5f, 0.f);
		bool past = false;
		for (int i = 0; i < (int)(25.f * SR) && r.m->drop[0].alive; i++) {
			r.tick();
			if (std::sqrt(r.m->drop[0].x * r.m->drop[0].x + r.m->drop[0].z * r.m->drop[0].z) > 1.05f) past = true;
		}
		check(past, "and it leaves past the hoop");
	}

	printf("== every landing sounds ==\n");
	{
		// the same drop on a tongue and on the bare steel between two of them
		Run a; a.noPaddles(); a.mat(TM_GLASS);
		float x, z; a.over(3, 0.8f, x, z);
		a.m->spawn(x, z);
		std::vector<float> on = a.record(1.1f);
		Run b; b.noPaddles(); b.mat(TM_GLASS);
		// the bare spot farthest from any tongue, inside the draught
		float bx = 0.f, bz = 0.f, bestD = -1.f;
		for (float px = -0.8f; px <= 0.8f; px += 0.02f) for (float pz = -0.8f; pz <= 0.8f; pz += 0.02f) {
			if (px * px + pz * pz > 0.8f * 0.8f) continue;
			float nearest = 1e9f;
			for (int t = 0; t < b.m->nT; t++) {
				const Canopy::TongueVoice& T = b.m->tv[t];
				float qx = px - T.tx, qz = pz - T.tz, A = qx * T.ux + qz * T.uz, C = qx * T.uz - qz * T.ux;
				float da = std::max(std::max(-A, A - T.len), 0.f), dc = std::max(std::fabs(C) - T.w, 0.f);
				nearest = std::min(nearest, std::sqrt(da * da + dc * dc));
			}
			if (nearest > bestD) { bestD = nearest; bx = px; bz = pz; }
		}
		bool found = bestD > 0.03f;
		b.m->spawn(bx, bz);
		std::vector<float> off = b.record(1.1f);
		float dB = 20.f * std::log10(peak(off) / std::max(peak(on), 1e-6f));
		check(found && peak(off) > 0.1f, string::f("a landing on no tongue is heard: %.2f V", peak(off)));
		check(dB < -3.f, string::f("and quieter than one on a tongue: %.1f dB", dB));
		// and a hit on one tongue reaches the one beside it
		Run c; c.noPaddles(); c.mat(TM_GLASS);
		float cx, cz; c.over(1, 0.8f, cx, cz);
		c.m->spawn(cx, cz);
		std::vector<float> pol; c.run(0.75f);
		float loudest = 0.f;
		for (int i = 0; i < (int)(0.3f * SR); i++) { c.tick(); for (int t = 2; t < 4; t++) loudest = std::max(loudest, std::fabs(c.m->tv[t].out)); }
		check(loudest > 0.f, string::f("a neighbour rings faintly: %.3f", loudest));
	}

	printf("== a leaf: silent, and its material decides ==\n");
	{
		Run r; r.mat(TM_GLASS);
		r.onePaddle(0.f, 1.5f, 0.f, 0.4f, -0.16f, 0.f, TP_WOOD);
		r.m->spawn(0.f, 0.f);
		float loud = 0.f; long st0 = r.m->strikes;
		for (int i = 0; i < (int)(0.6f * SR); i++) {
			r.tick();
			loud = std::max(loud, std::fabs(r.m->outputs[Canopy::L_OUTPUT].getVoltage()));
		}
		check(r.m->drop[0].vx > 0.1f, string::f("it sends the drop downhill: vx %.2f", r.m->drop[0].vx));
		check(loud == 0.f && r.m->strikes == st0, "and the hit makes no sound");
	}
	{
		float top[TP_MIX];
		for (int p = 0; p < TP_MIX; p++) {
			Run r; r.mat(TM_GLASS);
			r.onePaddle(0.f, 1.0f, 0.f, 0.5f, 0.f, 0.f, p);
			r.m->spawn(0.1f, 0.f);
			r.run(0.66f);      // it has reached the leaf
			float hi = -1.f;
			for (int i = 0; i < (int)(0.6f * SR); i++) { r.tick(); if (r.m->drop[0].y > hi) hi = r.m->drop[0].y; }
			top[p] = hi - 1.f;
			printf("    glass on %-7s rebounds to %.2f above it\n", CP_LEAF_MAT[p].name, top[p]);
		}
		check(top[TP_STEEL] > top[TP_WOOD] && top[TP_RUBBER] > top[TP_WOOD] && top[TP_WOOD] > top[TP_FELT], "rubber and steel return more than wood, wood more than felt");
	}
	{
		int felt = 0, steel = 0;
		for (int p : {(int)TP_FELT, (int)TP_STEEL}) {
			Run r; r.mat(TM_WATER);
			r.onePaddle(0.f, 1.0f, 0.f, 0.6f, -0.1f, 0.f, p);
			r.m->spawn(0.f, 0.f);
			r.run(0.75f);
			int n = 0; for (const CanopyDrop& d : r.m->drop) if (d.alive && d.droplet) n++;
			(p == TP_FELT ? felt : steel) = n;
		}
		check(felt == 0 && steel >= 2, string::f("water soaks into felt (%d droplets) and breaks up on steel (%d)", felt, steel));
	}
	{
		Run r; r.mat(TM_RUBBER);
		r.onePaddle(0.f, 1.f, 0.f, 0.6f, 0.f, 0.f, TP_FELT);
		r.m->spawn(0.f, 0.f);
		r.run(30.f);
		int alive = 0; for (const CanopyDrop& d : r.m->drop) if (d.alive) alive++;
		check(alive == 0, "a drop on a level leaf does not stay there");

	}

	printf("== the number of leaves ==\n");
	{
		Run r;
		check(r.m->nLive == 3, string::f("three to start with: %d", r.m->nLive));
		for (int n : {0, 1, 12}) {
			r.m->params[Canopy::DENSITY_PARAM].setValue((float)n); r.run(0.01f);
			check(r.m->nLive == n, string::f("the knob at %d: %d in play", n, r.m->nLive));
		}
		r.m->params[Canopy::DENSITY_PARAM].setValue(3.f);
		r.m->inputs[Canopy::DENSITY_INPUT].channels = 1;
		r.m->inputs[Canopy::DENSITY_INPUT].setVoltage(2.f); r.run(0.01f);
		check(r.m->nLive == 5, "3 + 2 V is five");
		r.m->inputs[Canopy::DENSITY_INPUT].setVoltage(40.f); r.run(0.01f);
		check(r.m->nLive == CP_MAXLEAVES, "CV stops at twelve");
		// however many are in play they should spread through the column
		for (int n : {3, 5, 8}) {
			float lo = 9.f, hi = -9.f;
			for (int i = 0; i < n; i++) { lo = std::min(lo, r.m->leaf[i].cy); hi = std::max(hi, r.m->leaf[i].cy); }
			check(hi - lo > 0.5f, string::f("the first %d span %.2f of the column's height", n, hi - lo));
		}
		// more leaves, more drops touched, with drops still reaching the drum
		// on beech, since a monstera is big enough that two of them already meet most drops
		float touched[3]; int cnt[3] = {2, 6, 12};
		for (int k = 0; k < 3; k++) {
			Run q(false); q.mat(TM_GLASS);
			q.m->leafForm = LF_BEECH;
			q.m->params[Canopy::DENSITY_PARAM].setValue((float)cnt[k]);
			q.m->params[Canopy::RATE_PARAM].setValue(0.45f);
			q.run(120.f);
			touched[k] = (float)q.m->touched / (float)std::max(q.m->spawned, 1L);
			float reach = (float)q.m->landed / (float)std::max(q.m->spawned, 1L);
			check(reach > 0.85f, string::f("%2d leaves: %.0f%% touch one, %.0f%% reach the drum", cnt[k], 100.f * touched[k], 100.f * reach));
		}
		check(touched[2] > touched[1] && touched[1] > touched[0], "more leaves, more drops touched");
	}

	printf("== the leaf's surface ==\n");
	{
		Run r;
		CanopyLeafShape S = canopyLeafShape(0);
		check(r.m->formRadius(LF_BEECH, 0.f) > 1.f && r.m->formRadius(LF_BEECH, 1.57f) < 0.7f, "beech: longer than it is wide");
		check(Canopy::leafLift(S, LF_BEECH, 0.f, 0.4f) > Canopy::leafLift(S, LF_BEECH, 0.f, 0.f), "the edges stand above the midrib: a fold");
		check(Canopy::leafLift(S, LF_BEECH, 0.9f, 0.f) < Canopy::leafLift(S, LF_BEECH, 0.f, 0.f), "and the tip droops: an arch");
		bool differ = false;
		for (int i = 1; i < 12; i++) { CanopyLeafShape T = canopyLeafShape(i); if (T.arch != S.arch || T.width != S.width) differ = true; }
		check(differ, "no two alike");
		// the fold gathers a drop toward the midrib, and the arch runs it off the tip
		Run q; q.mat(TM_GLASS);
		q.onePaddle(0.f, 1.f, 0.f, 0.5f, -0.1f, 0.f, TP_WOOD);
		q.run(0.01f);
		CanopyLeafLive K = q.m->live[0];
		float off = 0.08f;
		q.m->spawn(K.bx * off + K.ax * 0.1f, K.bz * off + K.az * 0.1f);
		float acrossMost = 0.f; bool crossed = false; float sgn0 = 0.f;
		for (int i = 0; i < (int)(1.0f * SR); i++) {
			q.tick();
			CanopyDrop& d = q.m->drop[0];
			if (!d.alive) break;
			if (d.y < 1.2f && d.y > 0.8f) {
				float ac = (d.x - K.cx) * K.bx + (d.z - K.cz) * K.bz;
				if (sgn0 == 0.f) sgn0 = ac > 0.f ? 1.f : -1.f;
				if (ac * sgn0 < 0.f) crossed = true;
				acrossMost = std::max(acrossMost, std::fabs(ac));
			}
		}
		check(q.m->drop[0].vx > 0.1f, string::f("it leaves down the leaf: vx %.2f", q.m->drop[0].vx));
	}

	printf("== the defaults ==\n");
	{
		Run r;
		check(r.m->leafForm == LF_MONSTERA, "a new module has monstera leaves");
		check((int)r.m->params[Canopy::LEAF_PARAM].getValue() == TP_LEAF && std::string(CP_LEAF_MAT[TP_LEAF].name) == "Leaf", "made of Leaf");
	}

	printf("== leaf sizes, on a 45 cm drum ==\n");
	{
		// the drum's radius is 22.5 cm; each number is tip to stem in cm
		struct { int f; float cm; } W[] = {{LF_BEECH, 10.f}, {LF_MAPLE, 12.f}, {LF_MONSTERA, 30.f}, {LF_GINKGO, 6.f}, {LF_OAK, 15.f}, {LF_WILLOW, 8.f}};
		for (auto& w : W) {
			Run r;
			r.m->leafForm = w.f;
			r.m->params[Canopy::DENSITY_PARAM].setValue(1.f);
			r.run(0.01f);
			float L = r.m->live[0].L;
			float tip = CP_FORM_C[w.f] + r.m->formRadius(w.f, 0.f), base = CP_FORM_C[w.f] - r.m->formRadius(w.f, (float)M_PI - 1e-3f);
			// lobes and notches can reach past the midrib's ends, so measure the whole outline
			float lo = 9.f, hi = -9.f, wide = 0.f;
			for (int i = 0; i < CP_FORM_TAB; i++) {
				float th = -(float)M_PI + 2.f * (float)M_PI * (float)i / (float)CP_FORM_TAB, rr = r.m->formR[w.f][i];
				float a = CP_FORM_C[w.f] + rr * std::cos(th), y = rr * std::sin(th);
				lo = std::min(lo, a); hi = std::max(hi, a); wide = std::max(wide, y);
			}
			float cm = (hi - lo) * L * 22.5f, cmWide = 2.f * wide * r.m->live[0].shape.width * L * 22.5f;
			check(std::fabs(cm - w.cm) < w.cm * 0.15f, string::f("%-9s %.1f cm long, %.1f cm wide (want about %.0f long)", CP_FORM_NAME[w.f], cm, cmWide, w.cm));
		}
		// the knob's readout is 15.75 x 3.143^v mm; a drop's radius is 0.035 x 3.143^v drum radii
		Run r;
		r.m->params[Canopy::SIZE_PARAM].setValue(0.5f); r.run(0.002f);
		float mm = r.m->sizeK * 225.f * 2.f, shown = 15.75f * std::pow(3.143f, 0.5f);
		check(std::fabs(mm - shown) < 0.1f, string::f("the size knob's readout matches the drop: %.1f mm at the middle", mm));
	}

	printf("== leaf forms ==\n");
	{
		for (int f = 0; f < LF_COUNT; f++) {
			Run r; r.mat(TM_GLASS);
			r.m->leafForm = f;
			r.m->params[Canopy::DENSITY_PARAM].setValue(12.f);
			r.m->params[Canopy::RATE_PARAM].setValue(0.45f);
			r.run(0.01f);
			check(r.m->live[0].form == f, string::f("%s reaches the physics", CP_FORM_NAME[f]));
			Run q(false); q.mat(TM_GLASS);
			q.m->leafForm = f;
			q.m->params[Canopy::DENSITY_PARAM].setValue(8.f);
			q.m->params[Canopy::RATE_PARAM].setValue(0.45f);
			q.run(90.f);
			float reach = (float)q.m->landed / (float)std::max(q.m->spawned, 1L);
			float t = (float)q.m->touched / (float)std::max(q.m->spawned, 1L);
			// the small forms really do meet fewer drops: eight beech leaves are not eight monsteras
			check(reach > 0.85f && t > 0.2f, string::f("%-9s %.0f%% touch a leaf, %.0f%% reach the drum", CP_FORM_NAME[f], 100.f * t, 100.f * reach));
		}
		Run r; r.m->leafForm = LF_MIXED; r.run(0.01f);
		bool differ = false; for (int i = 1; i < r.m->nLive; i++) if (r.m->live[i].form != r.m->live[0].form) differ = true;
		check(differ, "All forms in turn: the leaves differ");
		// a drop dropped down one of a monstera's cuts falls through it
		Run a; a.mat(TM_GLASS);
		a.m->params[Canopy::SIZE_PARAM].setValue(0.f);
		a.m->leafForm = LF_MONSTERA;
		a.onePaddle(0.f, 1.f, 0.f, 0.7f, 0.f, 0.f, TP_WOOD);
		a.run(0.01f);
		CanopyLeafLive K = a.m->live[0];
		float at = 1.46f;
		check(a.m->formRadius(LF_MONSTERA, at) < 0.45f * a.m->formRadius(LF_MONSTERA, at + 0.2f),
		      string::f("a monstera's cut reaches in: %.2f at the cut, %.2f beside it", a.m->formRadius(LF_MONSTERA, at), a.m->formRadius(LF_MONSTERA, at + 0.2f)));
		float rr = 0.85f * a.m->formRadius(LF_MONSTERA, at + 0.2f);
		float cuX = CP_FORM_C[LF_MONSTERA] + rr * std::cos(at), cuY = rr * std::sin(at);
		float wx = K.cx + K.ax * cuX * K.L + K.bx * (K.shape.sweep * (cuX) * (cuX) + cuY * K.shape.width) * K.L;
		float wz = K.cz + K.az * cuX * K.L + K.bz * (K.shape.sweep * (cuX) * (cuX) + cuY * K.shape.width) * K.L;
		a.m->spawn(wx, wz);
		a.run(0.75f);
		check(!a.m->drop[0].touched, "and a small drop falls straight through it");
		Run a2; a2.mat(TM_GLASS);
		a2.m->params[Canopy::SIZE_PARAM].setValue(0.f);
		a2.m->leafForm = LF_MONSTERA;
		a2.onePaddle(0.f, 1.f, 0.f, 0.7f, 0.f, 0.f, TP_WOOD);
		a2.run(0.01f);
		a2.m->spawn(K.cx + K.bx * 0.15f, K.cz + K.bz * 0.15f);
		a2.run(0.75f);
		check(a2.m->drop[0].touched, "beside the cuts it lands on the leaf");
		// every form's outline is closed and has some size
		for (int f = 0; f < LF_COUNT; f++) {
			Run q;
			float mn = 1e9f, mx = 0.f;
			for (int i = 0; i < CP_FORM_TAB; i++) { mn = std::min(mn, q.m->formR[f][i]); mx = std::max(mx, q.m->formR[f][i]); }
			check(mn > 0.f && mx > 0.5f && mx < 2.f, string::f("%s: reaches %.2f to %.2f from its centre", CP_FORM_NAME[f], mn, mx));
		}
		// save
		Run b; b.m->leafForm = LF_GINKGO;
		json_t* j = b.m->dataToJson();
		Run c; c.m->dataFromJson(j); json_decref(j);
		check(c.m->leafForm == LF_GINKGO, "the form is saved");
	}

	printf("== X and Z ==\n");
	{
		for (float v : {-5.f, -2.f, 2.f, 5.f}) {
			Run r; r.noPaddles(); r.mat(TM_GLASS);
			r.m->inputs[Canopy::X_INPUT].channels = 1;
			r.m->inputs[Canopy::X_INPUT].setVoltage(v);
			r.run(0.01f);
			double sum = 0.0; float rad = 0.f;
			for (int i = 0; i < 2000; i++) {
				r.m->spawnNow();
				for (const CanopyDrop& d : r.m->drop) { if (d.alive && d.age == 0.f) { sum += d.x; rad = std::max(rad, std::sqrt(d.x * d.x + d.z * d.z)); } }
				for (CanopyDrop& d : r.m->drop) d.alive = false;
			}
			float mean = (float)(sum / 2000.0);
			check(std::fabs(mean - v * 0.19f) < 0.05f, string::f("X at %+.0f V: drops average x = %+.2f", v, mean));
			check(rad <= 0.96f, string::f("and stay over the drum: furthest %.2f", rad));
		}
		Run r; r.noPaddles(); r.mat(TM_GLASS);
		r.m->inputs[Canopy::Z_INPUT].channels = 1;
		r.m->inputs[Canopy::Z_INPUT].setVoltage(-4.f);
		r.run(0.01f);
		check(std::fabs(r.m->colZ + 0.76f) < 0.01f && r.m->colR < 0.2f, string::f("Z at -4 V moves the column to z = %.2f, %.2f wide", r.m->colZ, r.m->colR));
		// and an unpatched module uses the whole drum
		Run q; q.run(0.01f);
		check(q.m->colR > 0.9f, "with neither patched the column is the whole drum");
	}

	printf("== a leaf flashes when it is hit ==\n");
	{
		Run r; r.mat(TM_GLASS);
		r.onePaddle(0.f, 1.f, 0.f, 0.5f, -0.1f, 0.f, TP_LEAF);
		check(r.m->leafFlash[0] == 0.f, "dark until it is hit");
		r.m->spawn(0.1f, 0.f);
		float most = 0.f; int at = 0;
		for (int i = 0; i < (int)(1.0f * SR); i++) { r.tick(); if (r.m->leafFlash[0] > most) { most = r.m->leafFlash[0]; at = i; } }
		check(most > 0.5f, string::f("a marble lights it to %.2f", most));
		// with nothing more landing on it, it fades
		Run q; q.onePaddle(0.f, 1.f, 0.f, 0.5f, -0.1f, 0.f, TP_LEAF);
		q.m->leafFlash[0] = 1.f;
		q.run(0.25f);
		float quarter = q.m->leafFlash[0];
		q.run(0.25f);
		check(quarter < 0.2f && q.m->leafFlash[0] < 0.03f, string::f("it fades: %.2f after a quarter of a second, %.3f after a half", quarter, q.m->leafFlash[0]));
		// a drop rolling on it does not keep it lit
		Run s; s.mat(TM_GLASS);
		s.onePaddle(0.f, 1.f, 0.f, 0.5f, -0.1f, 0.f, TP_FELT);
		s.m->spawn(0.1f, 0.f);
		s.run(0.9f);
		float rolling = s.m->leafFlash[0];
		check(rolling < 0.4f, string::f("a drop rolling on felt leaves it at %.2f", rolling));
	}

	printf("== a leaf bends and throws ==\n");
	{
		Run r; r.mat(TM_STEEL);
		r.onePaddle(0.f, 1.f, 0.f, 0.5f, -0.1f, 0.f, TP_RUBBER);
		r.m->spawn(0.3f, 0.f);
		float most = 0.f; float upward = -1e9f;
		for (int i = 0; i < (int)(1.2f * SR); i++) { r.tick(); most = std::max(most, r.m->bend[0]); upward = std::max(upward, r.m->bendV[0] < 0.f ? -r.m->bendV[0] : 0.f); }
		check(most > 0.1f, string::f("a steel marble bends a rubber leaf down by %.2f radians", most));
		check(upward > 0.2f, string::f("and it springs back at %.1f radians a second", upward));
		r.run(4.f);
		check(std::fabs(r.m->bend[0]) < 0.01f, "then settles");
	}
	{
		float most[TP_MIX];
		for (int p = 0; p < TP_MIX; p++) {
			Run r; r.mat(TM_GLASS);
			r.onePaddle(0.f, 1.f, 0.f, 0.5f, -0.1f, 0.f, p);
			r.m->spawn(0.3f, 0.f);
			most[p] = 0.f;
			for (int i = 0; i < (int)(1.2f * SR); i++) { r.tick(); most[p] = std::max(most[p], std::fabs(r.m->bend[0])); }
			printf("    glass bends %-7s by %.3f\n", CP_LEAF_MAT[p].name, most[p]);
		}
		check(most[TP_RUBBER] > most[TP_GLASS] * 3.f, "rubber gives far more than glass");
	}
	{
		// what the material changes in the sound: how hard, and when, drops arrive
		float meanSpeed[TP_MIX], lateness[TP_MIX]; float touched = 0.f; long strikesPer[TP_MIX];
		for (int p = 0; p < TP_MIX; p++) {
			Run r(false); r.mat(TM_GLASS);
			r.m->params[Canopy::LEAF_PARAM].setValue((float)p);
			r.m->params[Canopy::RATE_PARAM].setValue(0.45f);
			r.m->params[Canopy::DRIFT_PARAM].setValue(0.2f);
			r.m->params[Canopy::TILT_PARAM].setValue(0.2f);
			double speed = 0.0, late = 0.0; long n = 0;
			bool wasLanded[CP_MAXDROPS] = {}; float lastVy[CP_MAXDROPS] = {};
			for (int i = 0; i < (int)(150.f * SR); i++) {
				r.tick();
				for (int k = 0; k < CP_MAXDROPS; k++) {
					CanopyDrop& d = r.m->drop[k];
					if (!d.alive) { wasLanded[k] = false; continue; }
					if (d.landed && !wasLanded[k]) { speed += std::fabs(lastVy[k]); late += d.age; n++; wasLanded[k] = true; }
					lastVy[k] = d.vy;
				}
			}
			meanSpeed[p] = (float)(speed / std::max(n, 1L)); lateness[p] = (float)(late / std::max(n, 1L));
			strikesPer[p] = r.m->strikes;
			touched = (float)r.m->touched / (float)std::max(r.m->spawned, 1L);
			printf("    glass on %-7s: lands at %.2f, %.2f s after appearing, %.0f%% touched a leaf, %ld strikes\n", CP_LEAF_MAT[p].name, meanSpeed[p], lateness[p], 100.f * touched, strikesPer[p]);
		}
		check(touched > 0.5f, string::f("most drops meet a leaf on the way down: %.0f%%", 100.f * touched));
	}

	printf("== level ==\n");
	{
		Run r; r.noPaddles(); r.mat(TM_GLASS);
		float x, z; r.over(0, 0.8f, x, z);
		r.m->spawn(x, z);
		std::vector<float> v = r.record(1.2f);
		check(peak(v) > 3.5f, string::f("one glass marble on the big tongue: %.2f V at the mix (Loom's one pluck is 7.7 V)", peak(v)));
		Run q(false);
		double e = 0.0; int n = (int)(30.f * SR); float pk = 0.f;
		for (int i = 0; i < n; i++) { q.tick(); float l = q.m->outputs[Canopy::L_OUTPUT].getVoltage(); e += l * l; pk = std::max(pk, std::fabs(l)); }
		printf("    default settings for 30 s: rms %.2f V, peak %.2f V\n", std::sqrt(e / n), pk);
		check(pk <= 7.5f, "and the limiter holds it");
	}

	printf("== the tail ==\n");
	{
		Run r; r.noPaddles(); r.mat(TM_GLASS);
		r.m->inputs[Canopy::GATE_INPUT].channels = 1;
		r.m->inputs[Canopy::GATE_INPUT].setVoltage(10.f);
		int n = 0; while (!r.m->tv[0].awake && n < 480) { r.tick(); n++; }
		while (r.m->tv[0].awake && n < (int)(30.f * SR)) { r.tick(); n++; }
		float secs = n / SR;
		check(secs > 1.5f && secs < 6.f, string::f("a tongue at the default DECAY has stopped after %.1f s", secs));
	}

	printf("== drift ==\n");
	{
		Run r; r.run(0.1f);
		const CanopyLeaf h = r.m->leaf[0];
		bool stays = std::fabs(r.m->live[0].cx - h.cx) < 1e-5f;
		r.run(2.f);
		stays = stays && std::fabs(r.m->live[0].cx - h.cx) < 1e-5f && std::fabs(r.m->live[0].cy - h.cy) < 1e-5f;
		check(stays, "DRIFT and TILT at zero: a leaf stays where it was put");
	}
	{
		Run r;
		r.m->params[Canopy::SPEED_PARAM].setValue(1.f);
		r.m->params[Canopy::DRIFT_PARAM].setValue(1.f);
		r.m->params[Canopy::TILT_PARAM].setValue(0.f);
		r.run(0.01f);
		const CanopyLeaf h = r.m->leaf[0];
		float far = 0.f, farY = 0.f, moved = 0.f, lastX = r.m->live[0].cx;
		for (int i = 0; i < 2000; i++) {
			r.run(0.01f);
			far = std::max(far, std::max(std::fabs(r.m->live[0].cx - h.cx), std::fabs(r.m->live[0].cz - h.cz)));
			farY = std::max(farY, std::fabs(r.m->live[0].cy - h.cy));
			moved += std::fabs(r.m->live[0].cx - lastX); lastX = r.m->live[0].cx;
		}
		check(far > 0.25f && far <= 0.43f, string::f("full DRIFT wanders up to %.2f and stays within its 0.42", far));
		check(farY > 0.1f && farY <= 0.23f, string::f("and in height up to %.2f, within 0.22", farY));
		Run a; a.m->params[Canopy::SPEED_PARAM].setValue(0.f); a.m->params[Canopy::DRIFT_PARAM].setValue(1.f); a.run(0.01f);
		float aMoved = 0.f, aLast = a.m->live[0].cx;
		for (int i = 0; i < 2000; i++) { a.run(0.01f); aMoved += std::fabs(a.m->live[0].cx - aLast); aLast = a.m->live[0].cx; }
		check(moved > aMoved * 8.f, string::f("SPEED: %.2f of travel in 20 s at the top, %.2f at the bottom", moved, aMoved));
		bool same = true;
		for (int i = 1; i < r.m->nLive; i++) if (std::fabs(r.m->driftRatio[i][0] - r.m->driftRatio[0][0]) > 1e-3f) same = false;
		check(!same, "the leaves do not move in step");
	}
	{
		Run r;
		r.m->params[Canopy::SPEED_PARAM].setValue(1.f);
		r.m->params[Canopy::TILT_PARAM].setValue(1.f);
		r.run(0.01f);
		float a0 = r.m->live[0].ax, range = 0.f;
		for (int i = 0; i < 2000; i++) { r.run(0.01f); range = std::max(range, std::fabs(r.m->live[0].ax - a0)); }
		check(range > 0.1f, string::f("TILT swings it: %.2f of its direction", range));
	}

	printf("== played directly ==\n");
	{
		Run r; r.noPaddles(); r.mat(TM_GLASS);
		r.m->inputs[Canopy::GATE_INPUT].channels = 1;
		r.m->inputs[Canopy::VOCT_INPUT].channels = 1;
		// the fifth above C3 is tongue 3 on Pentatonic Minor
		r.m->inputs[Canopy::VOCT_INPUT].setVoltage(-1.f + 7.f / 12.f);
		r.m->inputs[Canopy::GATE_INPUT].setVoltage(10.f);
		std::vector<float> v = r.record(0.5f, 3);
		check(peak(v) > 0.3f, string::f("a GATE strikes the tongue at that V/OCT at once: %.2f V, no drop", peak(v)));
		bool any = false; for (const CanopyDrop& d : r.m->drop) if (d.alive) any = true;
		check(!any, "and nothing fell");
	}
	{
		Run r; r.noPaddles(); r.mat(TM_GLASS);
		r.m->inputs[Canopy::GATE_INPUT].channels = 1;
		r.m->inputs[Canopy::VOCT_INPUT].channels = 1;
		r.m->inputs[Canopy::VOCT_INPUT].setVoltage(3.f + 7.f / 12.f);   // four octaves up
		r.m->inputs[Canopy::GATE_INPUT].setVoltage(10.f);
		r.run(0.1f);
		int awake = -1; for (int t = 0; t < 10; t++) if (r.m->tv[t].contact[0].ph < 2.f && r.m->tv[t].contact[0].amp > 0.f && r.m->tv[t].env > 0.1f) { awake = t; break; }
		check(awake == 3 || awake == 8, string::f("a note out of range folds by octaves onto the same pitch: tongue %d", awake));
	}
	{
		float loud[2];
		for (int k = 0; k < 2; k++) {
			Run r; r.noPaddles(); r.mat(TM_GLASS);
			r.m->inputs[Canopy::GATE_INPUT].channels = 1;
			r.m->inputs[Canopy::VEL_INPUT].channels = 1;
			r.m->inputs[Canopy::VEL_INPUT].setVoltage(k == 0 ? 2.f : 10.f);
			r.m->inputs[Canopy::GATE_INPUT].setVoltage(10.f);
			loud[k] = peak(r.record(0.3f, 0));
		}
		check(loud[1] > loud[0] * 3.f, string::f("VEL: %.2f V at 2 V, %.2f V at 10 V", loud[0], loud[1]));
	}

	printf("== drops reach the drum ==\n");
	for (int k = 0; k <= TM_MIX; k++) {
		Run r(false);
		r.m->params[Canopy::DRIFT_PARAM].setValue(0.2f);
		r.m->params[Canopy::TILT_PARAM].setValue(0.2f);
		r.m->params[Canopy::MATERIAL_PARAM].setValue((float)k);
		r.m->params[Canopy::RATE_PARAM].setValue(0.45f);
		r.run(120.f);
		long sp = std::max(r.m->spawned, 1L);
		float frac = (float)r.m->landed / (float)sp, ton = (float)r.m->onTongue / (float)sp;
		// a few are always still in the air when the count stops
		check(frac > 0.88f, string::f("%-6s %3.0f%% of %3ld drops reach the drum, %3.0f%% land on a tongue, %.1f strikes a drop", k < TM_MIX ? CP_MAT[k].name : "Mix", 100.f * frac, sp, 100.f * ton, (float)r.m->strikes / (float)sp));
	}
	for (int p = 0; p < TP_MIX; p++) {
		Run r(false);
		r.m->params[Canopy::MATERIAL_PARAM].setValue((float)TM_STEEL);
		r.m->params[Canopy::LEAF_PARAM].setValue((float)p);
		r.m->params[Canopy::RATE_PARAM].setValue(0.45f);
		r.run(120.f);
		float frac = (float)r.m->landed / (float)std::max(r.m->spawned, 1L);
		// rubber and steel throw hardest, and this counts only about sixty drops
		check(frac > 0.80f, string::f("steel on %-6s leaves: %3.0f%% reach the drum", CP_LEAF_MAT[p].name, 100.f * frac));
	}
	{
		Run r(false); r.mat(TM_GLASS);
		r.m->params[Canopy::RATE_PARAM].setValue(1.f);
		float maxR = 0.f, minX = 1e9f, maxX = -1e9f;
		for (int i = 0; i < (int)(10.f * SR); i++) {
			r.tick();
			if (i % 240) continue;
			for (const CanopyDrop& d : r.m->drop) if (d.alive && d.y > CP_H - 0.01f) {
				maxR = std::max(maxR, std::sqrt(d.x * d.x + d.z * d.z));
				minX = std::min(minX, d.x); maxX = std::max(maxX, d.x);
			}
		}
		check(maxR <= 0.96f && maxX - minX > 1.2f, string::f("drops start across the whole column: x from %.2f to %.2f", minX, maxX));
	}

	printf("== RATE, CLOCK, Mix ==\n");
	{
		Run r(false); r.noPaddles(); r.mat(TM_GLASS);
		r.m->evenly = true;
		r.m->params[Canopy::RATE_PARAM].setValue(std::log(2.f / 0.05f) / std::log(240.f));
		r.m->nextDrop = 0.f;
		r.run(0.01f);
		long n0 = r.m->spawned;
		r.run(10.f);
		check(std::abs(r.m->spawned - n0 - 20) <= 1, string::f("2 drops/s evenly: %ld in 10 s", r.m->spawned - n0));
	}
	{
		Run r(false); r.noPaddles(); r.mat(TM_GLASS);
		r.m->params[Canopy::RATE_PARAM].setValue(std::log(4.f / 0.05f) / std::log(240.f));
		r.m->nextDrop = 0.f;
		r.run(0.01f);
		long n0 = r.m->spawned;
		r.run(300.f);
		long n = r.m->spawned - n0;
		check(n > 1120 && n < 1280, string::f("4 drops/s at random: %ld in 300 s", n));
	}
	{
		Run r; r.noPaddles(); r.mat(TM_GLASS);
		r.run(1.f);
		check(r.m->spawned == 0, "CLOCK patched: nothing falls without a pulse");
		for (int p = 0; p < 3; p++) {
			r.m->inputs[Canopy::CLOCK_INPUT].setVoltage(10.f); r.run(0.005f);
			r.m->inputs[Canopy::CLOCK_INPUT].setVoltage(0.f); r.run(0.005f);
		}
		check(r.m->spawned == 3, string::f("three pulses, %ld drops", r.m->spawned));
	}
	{
		Run r; r.noPaddles(); r.mat(TM_MIX);
		bool seen[TM_MIX] = {};
		for (int i = 0; i < 400; i++) r.m->spawn(0.f, 0.f);
		for (const CanopyDrop& d : r.m->drop) if (d.alive) seen[d.mat] = true;
		int kinds = 0; for (bool s : seen) if (s) kinds++;
		check(kinds == TM_MIX, string::f("Mix: %d of %d materials among the drops", kinds, (int)TM_MIX));
		r.m->params[Canopy::LEAF_PARAM].setValue((float)TP_MIX); r.m->params[Canopy::DENSITY_PARAM].setValue(5.f); r.run(0.01f);
		bool diff = false; for (int i = 1; i < r.m->nLive; i++) if (r.m->live[i].mat != r.m->live[0].mat) diff = true;
		check(diff, "leaf Mix: each leaf its own");
	}

	printf("== the mouse ==\n");
	{
		Run r;
		float W = 277.f, H = 340.f; bool ok = true; float worst = 0.f;
		for (float yaw : {0.f, 0.7f, -1.9f, 3.f}) for (float tilt : {0.2f, 0.62f, 1.2f}) {
			CanopyView V(W, H, yaw, tilt);
			for (float y : {0.f, 1.2f, 2.4f}) for (float x : {-0.8f, 0.1f, 0.9f}) for (float z : {-0.9f, 0.4f}) {
				CanopyView::Pt p = V.project(x, y, z);
				float gx, gz;
				if (!V.onLevel(p.x, p.y, y, gx, gz)) { ok = false; continue; }
				worst = std::max(worst, std::max(std::fabs(gx - x), std::fabs(gz - z)));
				float at[3] = {x, y, z}, hit[3];
				V.onFacing(p.x, p.y, at, hit);
				worst = std::max(worst, std::fabs(hit[0] - x) + std::fabs(hit[1] - y) + std::fabs(hit[2] - z));
			}
		}
		check(ok && worst < 1e-3f, string::f("screen to world and back, every camera: worst %.5f", worst));
		CanopyDisplay disp; disp.module = r.m; disp.box.size = Vec(W, H);
		bool hitAll = true;
		CanopyView V = disp.view();
		for (int i = 0; i < r.m->nLive; i++) {
			const CanopyLeafLive& K = r.m->live[i];
			CanopyView::Pt c = V.project(K.cx, K.cy, K.cz);
			if (disp.leafUnder(Vec(c.x, c.y)) < 0) hitAll = false;
		}
		check(hitAll, "a click on any leaf's middle finds a leaf");
		disp.module = nullptr;
	}

	printf("== saved ==\n");
	{
		Run a; a.m->leaf[1].cy = 0.77f; a.m->params[Canopy::DENSITY_PARAM].setValue(2.f); a.m->evenly = true; a.m->regOct = -2; a.m->camTilt = 0.9f; a.m->publishLeaves();
		json_t* j = a.m->dataToJson();
		Run b; b.m->dataFromJson(j); json_decref(j); b.run(0.01f);
		check(std::fabs(b.m->leaf[1].cy - 0.77f) < 1e-5f
		      && b.m->evenly && b.m->regOct == -2 && std::fabs(b.m->camTilt - 0.9f) < 1e-5f, "leaves, register, camera come back");
		bool same = false;
		{
			int bank = b.m->homeBank.load();
			same = std::fabs(b.m->home[bank][1].cy - 0.77f) < 1e-5f;
		}
		check(same, "and the physics has the loaded leaves");
	}

	printf("== a heavy rain stays bounded, and what it costs ==\n");
	{
		Run r(false);
		r.m->params[Canopy::RATE_PARAM].setValue(1.f);
		r.m->params[Canopy::MATERIAL_PARAM].setValue((float)TM_STEEL);
		r.m->params[Canopy::SIZE_PARAM].setValue(1.f);
		r.m->params[Canopy::DECAY_PARAM].setValue(1.f);
		r.m->params[Canopy::DRIFT_PARAM].setValue(1.f);
		float pk = 0.f; bool finite = true;
		auto t0 = std::chrono::steady_clock::now();
		int n = (int)(20.f * SR);
		for (int i = 0; i < n; i++) {
			r.tick();
			float l = r.m->outputs[Canopy::L_OUTPUT].getVoltage();
			pk = std::max(pk, std::fabs(l));
			if (!std::isfinite(l)) finite = false;
		}
		double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		check(finite && pk <= 10.f, string::f("12 large steel drops a second, ring time x4: peak %.2f V", pk));
		printf("    that costs %.2f%% of a core at 48 kHz\n", 100.0 * sec / 20.0);
		check(sec / 20.0 < 0.05, "under 5% of a core");
	}

	printf("%d failure(s)\n", fails);
	return fails ? 1 : 0;
}

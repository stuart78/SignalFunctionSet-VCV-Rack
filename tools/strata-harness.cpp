// Drives the REAL Strata -- src/strata.cpp compiled as-is against libRack -- and
// checks what docs/strata-design.md says must be true: a WaveStack file's
// `wt3d` grid lands every frame in its cell, a malformed one opens as one row,
// a folder becomes layers, a grid point plays exactly that frame, the glide's
// travel time follows distance in rate mode and not in time mode, aliasing at
// high pitch, and the cost at 16 voices.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/strata-harness.cpp -o /tmp/strata-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/strata-harness
// (-I src FIRST, or "plugin.hpp" resolves to the Rack SDK's own header.)
// dr_wav's implementation lives in phase.cpp in the plugin; the harness compiles it here
#define DR_WAV_IMPLEMENTATION
#include "../src/strata.cpp"
#include <chrono>
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
static std::string tmpDir;

// A mono float WAV with optional `clm ` and `wt3d` chunks, as WaveStack writes them.
static void writeWav(const std::string& path, const std::vector<float>& s, int clm, int fs, int cols, int rows, int layers, uint32_t flags = 0) {
	std::vector<uint8_t> b;
	auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 255); };
	auto u16 = [&](uint16_t v) { b.push_back(v & 255); b.push_back(v >> 8); };
	auto tag = [&](const char* t) { b.insert(b.end(), t, t + 4); };
	tag("RIFF"); u32(0); tag("WAVE");
	tag("fmt "); u32(16); u16(3); u16(1); u32(44100); u32(44100 * 4); u16(4); u16(32);
	if (clm) {
		std::string t = string::f("<!>%04d 10000000 wavetable (WaveStack)", clm);
		tag("clm "); u32((uint32_t)t.size()); b.insert(b.end(), t.begin(), t.end()); if (t.size() & 1) b.push_back(0);
	}
	if (cols) { tag("wt3d"); u32(28); u32(1); u32(fs); u32(cols); u32(rows); u32(layers); u32(flags); u32(0); }
	tag("data"); u32((uint32_t)(s.size() * 4));
	for (float v : s) { uint32_t x; std::memcpy(&x, &v, 4); u32(x); }
	uint32_t riff = (uint32_t)b.size() - 8; std::memcpy(&b[4], &riff, 4);
	FILE* f = fopen(path.c_str(), "wb"); fwrite(b.data(), 1, b.size(), f); fclose(f);
}
// frame i: a sine of amplitude amp(i), so a cell is identified by its level
static float amp(int i) { return (i + 1) / 16.f; }
static std::vector<float> table(int frames, int fs = 2048) {
	std::vector<float> s((size_t)frames * fs);
	for (int i = 0; i < frames; i++) for (int k = 0; k < fs; k++) s[(size_t)i * fs + k] = amp(i) * std::sin(2.f * (float)M_PI * k / fs);
	return s;
}

struct Run {
	Strata m; rack::engine::Module::ProcessArgs a; int64_t frame = 0;
	Run() { a.sampleRate = SR; a.sampleTime = 1.f / SR; m.outputs[Strata::OUT_OUTPUT].channels = 1; tick(); }
	void tick() { a.frame = frame++; m.process(a); }
	void run(float sec) { for (int i = 0; i < (int)(sec * SR); i++) tick(); }
	void load(std::vector<std::string> p) { m.load(p); m.waitLoaded(); tick(); }
	// settles first: the output's 2 Hz DC blocker rings briefly after a change of cell
	float peak(float sec) { run(0.4f); float pk = 0.f; for (int i = 0; i < (int)(sec * SR); i++) { tick(); pk = std::max(pk, std::fabs(m.outputs[Strata::OUT_OUTPUT].getVoltage(0))); } return pk / 5.f; }
};

int main() {
	rack::random::init();
	contextSet(new Context);
	APP->engine = new engine::Engine;
	tmpDir = "/private/tmp/claude-501/-Users-sfs-code-SignalFunctionSet/55c78911-a901-4502-b51d-8423a7d5809a/scratchpad/strata-files";
	system::createDirectories(tmpDir);

	printf("== wt3d reference files (WaveStack docs/wt3d, written independently) ==\n");
	{
		const std::string ref = "../WaveStack/docs/wt3d/";
		auto near = [](float a, float b) { return std::fabs(a - b) < 1e-6f; };
		StrataFile g = stReadWav(ref + "grid-4x2.wav");
		check(g.err.empty() && g.cols == 4 && g.rows == 2 && g.layers == 1 && g.frameSize == 64 && near(g.samples[7 * 64], 0.07f),
		      string::f("grid-4x2: %d x %d x %d, row 2 col 4 = %.2f (want 4 x 2 x 1, 0.07)", g.cols, g.rows, g.layers, g.samples.size() > 448 ? g.samples[448] : -1.f));
		StrataFile v = stReadWav(ref + "volume-3x2x2.wav");
		check(v.cols == 3 && v.rows == 2 && v.layers == 2 && v.flags == 5 && v.layerNames.size() == 2 && v.layerNames[0] == "Glass" && v.layerNames[1] == "Reed" && near(v.samples[11 * 64], 0.11f),
		      string::f("volume-3x2x2: %d x %d x %d, flags %u, %d names, layer 2 row 2 col 3 = %.2f", v.cols, v.rows, v.layers, v.flags, (int)v.layerNames.size(), v.samples.size() > 704 ? v.samples[704] : -1.f));
		StrataFile u = stReadWav(ref + "future-version.wav");
		check(u.cols == 2 && u.rows == 2 && u.layers == 1, string::f("future-version: %d x %d x %d (want 2 x 2 x 1)", u.cols, u.rows, u.layers));
		StrataFile bad = stReadWav(ref + "invalid-count.wav");
		check(bad.cols == 0 && bad.frameSize == 64 && bad.samples.size() == 512, string::f("invalid-count: grid ignored, frame size %d from clm, %d samples", bad.frameSize, (int)bad.samples.size()));
		// every dimension 0xFFFFFFFF: the product overflows, so the grid is ignored rather than wrapping
		writeWav(tmpDir + "/overflow.wav", table(4, 64), 64, -1, -1, -1, -1);
		StrataFile o = stReadWav(tmpDir + "/overflow.wav");
		check(o.cols == 0 && o.frameSize == 64, string::f("overflowing grid ignored: cols %d, frame size %d", o.cols, o.frameSize));
	}

	printf("== a WaveStack 3D file: every frame in its cell ==\n");
	{
		writeWav(tmpDir + "/vol.wav", table(12), 2048, 2048, 3, 2, 2);
		Run r; r.m.conform.normalize = false; r.load({tmpDir + "/vol.wav"});
		const StrataTable* T = r.m.table;
		check(T && T->cols == 3 && T->rows == 2 && T->layers == 2, string::f("grid %d x %d x %d (want 3 x 2 x 2)", T ? T->cols : 0, T ? T->rows : 0, T ? T->layers : 0));
		r.m.params[Strata::FREQ_PARAM].setValue(-2.f);           // 65 Hz: low enough to read the full frames
		int wrong = 0; std::string row;
		for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 3; x++) {
			r.m.params[Strata::X_PARAM].setValue(x / 2.f);
			r.m.params[Strata::Y_PARAM].setValue((float)y);
			r.m.params[Strata::Z_PARAM].setValue((float)z);
			float pk = r.peak(0.07f), want = amp(T->index(x, y, z));
			if (std::fabs(pk - want) > want * 0.01f) { wrong++; row += string::f(" (%d,%d,%d) %.3f vs %.3f", x, y, z, pk, want); }
		}
		check(wrong == 0, string::f("each of the 12 grid points plays exactly its frame: %d wrong%s", wrong, row.c_str()));
		r.m.params[Strata::X_PARAM].setValue(0.25f); r.m.params[Strata::Y_PARAM].setValue(0.f); r.m.params[Strata::Z_PARAM].setValue(0.f);
		float mid = r.peak(0.07f), want = (amp(0) + amp(1)) / 2.f;
		check(std::fabs(mid - want) < want * 0.01f, string::f("halfway between frames 1 and 2: %.4f (want the blend, %.4f)", mid, want));
	}

	printf("== tables without a grid, malformed grids, folders of layers ==\n");
	{
		writeWav(tmpDir + "/serum.wav", table(16), 2048, 0, 0, 0, 0);
		Run a; a.load({tmpDir + "/serum.wav"});
		check(a.m.table->cols == 16 && a.m.table->rows == 1, string::f("a Serum file of 16 frames opens as one row: %d x %d", a.m.table->cols, a.m.table->rows));
		Run b; b.m.squareUp = true; b.load({tmpDir + "/serum.wav"});
		check(b.m.table->cols == 4 && b.m.table->rows == 4, string::f("...or square, when asked: %d x %d", b.m.table->cols, b.m.table->rows));
		writeWav(tmpDir + "/bad.wav", table(24), 2048, 2048, 8, 4, 1);  // claims 32 frames, holds 24
		Run c; c.load({tmpDir + "/bad.wav"});
		check(c.m.table->cols == 24 && c.m.table->rows == 1, string::f("a grid that does not add up opens as one row: %d x %d", c.m.table->cols, c.m.table->rows));
		writeWav(tmpDir + "/b-second.wav", table(6), 2048, 2048, 3, 2, 1);
		writeWav(tmpDir + "/a-first.wav", table(6), 2048, 2048, 3, 2, 1);
		Run d; d.load({tmpDir + "/b-second.wav", tmpDir + "/a-first.wav"});
		check(d.m.table->layers == 2 && d.m.table->layerNames.size() == 2 && d.m.table->layerNames[0] == "b-second",
		      string::f("two 3 x 2 files stack as 2 layers, in the order given (bottom first): %d layers, first \"%s\"", d.m.table->layers, d.m.table->layerNames.empty() ? "" : d.m.table->layerNames[0].c_str()));
		writeWav(tmpDir + "/c-odd.wav", table(8), 2048, 2048, 4, 2, 1);
		Run e; e.load({tmpDir + "/a-first.wav", tmpDir + "/c-odd.wav"});
		check(e.m.loadError[0] && e.m.table && e.m.table->name == "Factory", string::f("layers with different grids are refused, and the table in force stays: \"%s\"", e.m.loadError));
		writeWav(tmpDir + "/small.wav", table(4, 256), 256, 256, 2, 2, 1);
		Run f; f.m.conform.normalize = false; f.load({tmpDir + "/small.wav"});
		f.m.params[Strata::FREQ_PARAM].setValue(-2.f); f.m.params[Strata::X_PARAM].setValue(1.f); f.m.params[Strata::Y_PARAM].setValue(1.f);
		float pk = f.peak(0.07f);
		check(f.m.table->cols == 2 && std::fabs(pk - amp(3)) < amp(3) * 0.01f, string::f("256-sample frames are resampled to 2048: last frame plays at %.3f (want %.3f)", pk, amp(3)));
	}

	printf("== factory tables, and a layer stacked on top ==\n");
	{
		Run a;
		const StrataTable* T = a.m.table;
		check(T->cols == 8 && T->rows == 8 && T->layers == 4, string::f("a new module opens the 3D factory table: %d x %d x %d", T->cols, T->rows, T->layers));
		a.load(Strata::factorySources(1)); T = a.m.table;
		check(T->cols == 16 && T->rows == 1 && T->layers == 1, string::f("1D factory: %d x %d x %d (want 16 x 1 x 1)", T->cols, T->rows, T->layers));
		a.load(Strata::factorySources(2)); T = a.m.table;
		check(T->cols == 8 && T->rows == 8 && T->layers == 1, string::f("2D factory: %d x %d x %d (want 8 x 8 x 1)", T->cols, T->rows, T->layers));
		a.m.addLayer("factory:sync"); a.m.waitLoaded(); a.tick(); T = a.m.table;
		check(T->layers == 2 && T->layerNames.size() == 2 && T->layerNames[1] == "Sync" && a.m.sources.size() == 2,
		      string::f("a factory layer added on top: %d layers, top \"%s\"", T->layers, T->layerNames.size() > 1 ? T->layerNames[1].c_str() : ""));
		a.m.addLayer(tmpDir + "/a-first.wav"); a.m.waitLoaded(); a.tick(); T = a.m.table;
		check(T->layers == 2 && a.m.loadError[0], string::f("a 3 x 2 layer on an 8 x 8 table is refused and the stack stays: %d layers, \"%s\"", T->layers, a.m.loadError));
		a.m.removeTopLayer(); a.m.waitLoaded(); a.tick(); T = a.m.table;
		check(T->layers == 1, string::f("removing the top layer: %d left", T->layers));
		// every factory frame is a real, normalised wave
		int bad = 0;
		for (int f = 0; f < T->frames; f++) { float pk = 0.f; const float* d = T->level(f, 0); for (int i = 0; i < ST_N; i++) pk = std::max(pk, std::fabs(d[i])); if (!(pk > 0.5f && pk < 1.5f)) bad++; }
		Run b; b.load(Strata::factorySources(3)); const StrataTable* U = b.m.table;
		for (int f = 0; f < U->frames; f++) { float pk = 0.f; const float* d = U->level(f, 0); for (int i = 0; i < ST_N; i++) pk = std::max(pk, std::fabs(d[i])); if (!(pk > 0.5f && pk < 1.5f)) bad++; }
		check(bad == 0, string::f("every factory frame peaks near 1: %d do not", bad));
	}

	printf("== the stereo mix: where a voice reads is where it sits ==\n");
	{
		double lr[2][2];
		for (int side = 0; side < 2; side++) {
			Run r;
			r.m.outputs[Strata::LEFT_OUTPUT].channels = 1; r.m.outputs[Strata::RIGHT_OUTPUT].channels = 1;
			r.m.params[Strata::X_PARAM].setValue((float)side);
			r.m.params[Strata::Y_PARAM].setValue(0.5f);
			r.run(0.2f);
			double sl = 0, sr = 0;
			for (int i = 0; i < 4800; i++) { r.tick(); float L = r.m.outputs[Strata::LEFT_OUTPUT].getVoltage(), R = r.m.outputs[Strata::RIGHT_OUTPUT].getVoltage(); sl += L * L; sr += R * R; }
			lr[side][0] = 10 * std::log10(sl + 1e-12); lr[side][1] = 10 * std::log10(sr + 1e-12);
		}
		check(lr[0][0] - lr[0][1] > 6 && lr[1][1] - lr[1][0] > 6,
		      string::f("first column leans left by %.1f dB, last column right by %.1f dB (want > 6)", lr[0][0] - lr[0][1], lr[1][1] - lr[1][0]));
	}

	printf("== the read corrupted: RATE and START ==\n");
	{
		// START at a whole frame reads the NEXT frame in file order, cleanly
		writeWav(tmpDir + "/vol2.wav", table(12), 2048, 2048, 3, 2, 2);
		Run r; r.m.conform.normalize = false; r.load({tmpDir + "/vol2.wav"});
		r.m.params[Strata::FREQ_PARAM].setValue(-2.f);
		r.m.params[Strata::START_PARAM].setValue(1.f);
		float pk = r.peak(0.07f);
		check(std::fabs(pk - amp(1)) < amp(1) * 0.01f, string::f("START one frame on reads the next frame: %.4f (want %.4f)", pk, amp(1)));
		// RATE 2x: two frames a cycle, so the second harmonic leads
		r.m.params[Strata::START_PARAM].setValue(0.f);
		r.m.params[Strata::RATE_PARAM].setValue(0.5f);
		r.run(0.3f);
		const int N = 48000; std::vector<float> x(N);
		for (int i = 0; i < N; i++) { r.tick(); x[i] = r.m.outputs[Strata::OUT_OUTPUT].getVoltage(0); }
		float f0 = dsp::FREQ_C4 / 4.f;
		auto mag = [&](float f) { double re = 0, im = 0; for (int i = 0; i < N; i++) { re += x[i] * std::cos(2 * M_PI * f * i / SR); im += x[i] * std::sin(2 * M_PI * f * i / SR); } return std::sqrt(re * re + im * im); };
		double h1 = mag(f0), h2 = mag(2 * f0);
		check(h2 > 2 * h1, string::f("RATE 2x puts two frames in a cycle: harmonic 2 is %.1f dB over the fundamental", 20 * std::log10(h2 / std::max(h1, 1e-9))));
	}

	printf("== SHUFFLE and CRUSH ==\n");
	{
		// the first cut swaps the halves: a sine frame comes back inverted, exactly
		writeWav(tmpDir + "/sine.wav", table(4), 2048, 2048, 2, 2, 1);
		Run a, b;
		a.m.conform.normalize = false; b.m.conform.normalize = false;
		a.load({tmpDir + "/sine.wav"}); b.load({tmpDir + "/sine.wav"});
		b.m.params[Strata::SHUFFLE_PARAM].setValue(1.f / 6.f);
		a.run(0.1f); b.run(0.1f);
		double err = 0, ref = 0;
		for (int i = 0; i < 4800; i++) { a.tick(); b.tick(); float x = a.m.outputs[Strata::OUT_OUTPUT].getVoltage(0), y = b.m.outputs[Strata::OUT_OUTPUT].getVoltage(0); err += (x + y) * (x + y); ref += x * x; }
		check(err < ref * 1e-4, string::f("SHUFFLE's first level swaps the halves (a sine comes back inverted): residual %.1f dB", 10 * std::log10(err / ref + 1e-30)));
		// the order is the seed's: the same seed plays the same, a new one does not
		auto render = [](uint32_t seed) {
			Run r; r.m.shufSeed = seed; r.m.params[Strata::X_PARAM].setValue(0.6f); r.m.params[Strata::SHUFFLE_PARAM].setValue(1.f);
			r.run(0.05f); std::vector<float> o(2000);
			for (float& v : o) { r.tick(); v = r.m.outputs[Strata::OUT_OUTPUT].getVoltage(0); }
			return o;
		};
		std::vector<float> s1 = render(7), s2 = render(7), s3 = render(8);
		double d12 = 0, d13 = 0;
		for (size_t i = 0; i < s1.size(); i++) { d12 += std::fabs(s1[i] - s2[i]); d13 += std::fabs(s1[i] - s3[i]); }
		check(d12 == 0 && d13 > 1, string::f("a seed gives the same order every time, a new seed a new one (%.3f, %.1f)", d12, d13));
		// CRUSH at the top is a few levels: the output's distinct values collapse
		Run c; c.m.conform.normalize = false; c.load({tmpDir + "/sine.wav"});
		c.m.params[Strata::FREQ_PARAM].setValue(-3.f);
		c.m.params[Strata::CRUSH_PARAM].setValue(1.f);
		c.run(0.3f);
		float lo = 1e9f, hi = -1e9f; int mid = 0;
		for (int i = 0; i < 9600; i++) { c.tick(); float v = c.m.outputs[Strata::OUT_OUTPUT].getVoltage(0) / 5.f; lo = std::min(lo, v); hi = std::max(hi, v); if (std::fabs(v) > 0.2f && std::fabs(v) < 0.8f) mid++; }
		check(mid < 9600 / 10, string::f("CRUSH at 1 bit: the wave sits on its steps (%d of 9600 samples between them)", mid));
	}

	printf("== the listeners turn about the centre ==\n");
	{
		double lr[2];
		Run r;
		r.m.outputs[Strata::LEFT_OUTPUT].channels = 1; r.m.outputs[Strata::RIGHT_OUTPUT].channels = 1;
		r.m.params[Strata::X_PARAM].setValue(0.f); r.m.params[Strata::Y_PARAM].setValue(0.5f);
		r.m.params[Strata::ROTY_PARAM].setValue(1.f);          // half a turn: the pair swaps sides
		r.run(0.2f);
		double sl = 0, sr = 0;
		for (int i = 0; i < 4800; i++) { r.tick(); float L = r.m.outputs[Strata::LEFT_OUTPUT].getVoltage(), R = r.m.outputs[Strata::RIGHT_OUTPUT].getVoltage(); sl += L * L; sr += R * R; }
		lr[0] = 10 * std::log10(sl + 1e-12); lr[1] = 10 * std::log10(sr + 1e-12);
		check(lr[1] - lr[0] > 6, string::f("half a turn about the vertical: the first column now leans right by %.1f dB", lr[1] - lr[0]));
		float back[3]; r.m.unrotate(r.m.lisNow[0], back);
		check(std::fabs(back[0] - r.m.lis[0][0]) < 1e-5f && std::fabs(back[2] - r.m.lis[0][2]) < 1e-5f, "unrotate undoes rotate (so a drag lands where the pointer is)");
	}

	printf("== the glide: rate mode takes time in proportion to distance, time mode does not ==\n");
	for (int mode : {Strata::GLIDE_RATE, Strata::GLIDE_TIME}) {
		Run r; r.m.glideMode = mode;
		r.m.inputs[Strata::CLOCK_INPUT].channels = 1; r.m.inputs[Strata::RESET_INPUT].channels = 1;
		r.m.params[Strata::GLIDE_PARAM].setValue(std::log(20.f) / std::log(400.f));   // 0.1 s per cell
		r.m.addStep(0, 0, 0); r.m.addStep(4, 0, 0); r.m.addStep(5, 0, 0);
		auto pulse = [&](int in) { r.m.inputs[in].setVoltage(10.f); r.tick(); r.m.inputs[in].setVoltage(0.f); r.tick(); };
		pulse(Strata::RESET_INPUT); r.run(0.01f);
		auto arrive = [&](float target) { for (int i = 1; i < (int)(3 * SR); i++) { r.tick(); if (std::fabs(r.m.outputs[Strata::X_OUTPUT].getVoltage(0) - target) < 1e-3f) return i / SR; } return -1.f; };
		pulse(Strata::CLOCK_INPUT); float t4 = arrive(4.f);
		pulse(Strata::CLOCK_INPUT); float t1 = arrive(5.f);
		if (mode == Strata::GLIDE_RATE) check(std::fabs(t4 - 0.4f) < 0.01f && std::fabs(t1 - 0.1f) < 0.01f, string::f("rate: 4 cells in %.3f s, 1 cell in %.3f s (want 0.4 and 0.1)", t4, t1));
		else check(std::fabs(t4 - 0.1f) < 0.01f && std::fabs(t1 - 0.1f) < 0.01f, string::f("time: 4 cells in %.3f s, 1 cell in %.3f s (want 0.1 both)", t4, t1));
	}

	printf("== aliasing: a bright frame at 3911 Hz, energy between the harmonics, below 20 kHz ==\n");
	{
		auto alias = [](float warp, float sync, float f0 = 3911.f, float fold = 0.f) {
			Run r;
			r.m.params[Strata::X_PARAM].setValue(1.f);                 // the brightest factory frame: 99 harmonics
			// not a divisor of the sample rate, or aliases fold back onto harmonics and hide
			r.m.params[Strata::FREQ_PARAM].setValue(std::log2(f0 / dsp::FREQ_C4));
			r.m.params[Strata::WARP_PARAM].setValue(warp); r.m.params[Strata::SYNC_PARAM].setValue(sync);
			r.m.params[Strata::FOLD_PARAM].setValue(fold);
			r.run(0.05f);
			const int N = 32768; std::vector<float> x(N), sp(2 * N);
			// Blackman-Harris: sidelobes at -92 dB, where a Hann window's at -31..-41 dB
			// read as aliasing that was not there
			for (int i = 0; i < N; i++) {
				r.tick();
				double t = 2.0 * M_PI * i / (N - 1);
				double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
				x[i] = (float)(r.m.outputs[Strata::OUT_OUTPUT].getVoltage(0) * w);
			}
			rack::dsp::RealFFT fft(N); fft.rfft(x.data(), sp.data());
			double harm = 0, rest = 0;
			for (int k = 2; k < N / 2; k++) {
				double p = sp[2 * k] * sp[2 * k] + sp[2 * k + 1] * sp[2 * k + 1];
				float f = k * SR / N, nearest = std::round(f / f0) * f0;
				if (f < f0 * 0.5f) continue;      // DC and its window leakage are not aliasing
				// nor is anything above hearing: the 2x folder's halfband lets the
				// first few kHz past 24 kHz fold back to 20-24 kHz, and those
				// peaks dominated an unweighted count while nobody can hear them
				if (f > 20000.f) break;
				if (std::fabs(f - nearest) < 6 * SR / N && nearest > 0) harm += p; else rest += p;
			}
			return 10.0 * std::log10(rest / harm);
		};
		double plain = alias(0.f, 0.f), warped = alias(0.8f, 0.f), synced = alias(0.f, 0.5f), folded = alias(0.f, 0.f, 3911.f, 0.5f);
		check(plain < -65, string::f("plain: %.1f dB of the energy is off the harmonics", plain));
		// WARP's knee and SYNC's restart are corrected as steps and slope
		// changes (BLEP + BLAMP); near Nyquist every derivative of a restart
		// jumps as far as the one before it, and no finite correction converges
		printf("  info  at 3911 Hz, warp 0.8: %.1f dB   sync 4.5x: %.1f dB   fold 0.5: %.1f dB (the slave at 17.6 kHz)\n", warped, synced, folded);
		double worst = -999;
		for (float f : {110.f, 440.f, 1760.f}) {
			double p = alias(0.f, 0.f, f), w = alias(0.8f, 0.f, f), s = alias(0.f, 0.5f, f), fo = alias(0.f, 0.f, f, 0.5f);
			worst = std::max({worst, p, w, s});
			printf("  info  at %4.0f Hz: plain %.1f dB   warp 0.8 %.1f dB   sync 4.5x %.1f dB   fold 0.5 %.1f dB   fold 1 %.1f dB\n", f, p, w, s, fo, alias(0.f, 0.f, f, 1.f));
		}
		check(worst < -50, string::f("plain, warp and sync stay under -50 dB from 110 to 1760 Hz: worst %.1f dB", worst));
		// the corrupted read: every seam goes through the same BLEP/BLAMP
		auto aliasC = [](float rate, float start, float f0) {
			Run r;
			r.m.params[Strata::X_PARAM].setValue(1.f);
			r.m.params[Strata::FREQ_PARAM].setValue(std::log2(f0 / dsp::FREQ_C4));
			r.m.params[Strata::RATE_PARAM].setValue(rate); r.m.params[Strata::START_PARAM].setValue(start);
			r.run(0.05f);
			const int N = 32768; std::vector<float> x(N), sp(2 * N);
			for (int i = 0; i < N; i++) {
				r.tick();
				double t = 2.0 * M_PI * i / (N - 1);
				x[i] = (float)(r.m.outputs[Strata::OUT_OUTPUT].getVoltage(0) * (0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t)));
			}
			rack::dsp::RealFFT fft(N); fft.rfft(x.data(), sp.data());
			double harm = 0, rest = 0;
			for (int k = 2; k < N / 2; k++) {
				double p = sp[2 * k] * sp[2 * k] + sp[2 * k + 1] * sp[2 * k + 1];
				float f = k * SR / N, nearest = std::round(f / f0) * f0;
				if (f < f0 * 0.5f) continue;
				if (f > 20000.f) break;
				if (std::fabs(f - nearest) < 6 * SR / N && nearest > 0) harm += p; else rest += p;
			}
			return 10.0 * std::log10(rest / harm);
		};
		double cw = -999;
		for (float f : {110.f, 440.f, 1760.f}) {
			double a = aliasC(0.3f, 0.37f, f), b = aliasC(-0.4f, 0.6f, f);
			cw = std::max({cw, a, b});
			printf("  info  at %4.0f Hz: RATE 1.5x START 37%% %.1f dB   RATE 0.57x START 60%% %.1f dB\n", f, a, b);
		}
		check(cw < -45, string::f("the corrupted read stays under -45 dB from 110 to 1760 Hz: worst %.1f dB", cw));
		auto aliasS = [](float shuffle, float crush, float f0) {
			Run r;
			r.m.params[Strata::X_PARAM].setValue(1.f);
			r.m.params[Strata::FREQ_PARAM].setValue(std::log2(f0 / dsp::FREQ_C4));
			r.m.params[Strata::SHUFFLE_PARAM].setValue(shuffle); r.m.params[Strata::CRUSH_PARAM].setValue(crush);
			r.run(0.05f);
			const int N = 32768; std::vector<float> x(N), sp(2 * N);
			for (int i = 0; i < N; i++) {
				r.tick();
				double t = 2.0 * M_PI * i / (N - 1);
				x[i] = (float)(r.m.outputs[Strata::OUT_OUTPUT].getVoltage(0) * (0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t)));
			}
			rack::dsp::RealFFT fft(N); fft.rfft(x.data(), sp.data());
			double harm = 0, rest = 0;
			for (int k = 2; k < N / 2; k++) {
				double p = sp[2 * k] * sp[2 * k] + sp[2 * k + 1] * sp[2 * k + 1];
				float f = k * SR / N, nearest = std::round(f / f0) * f0;
				if (f < f0 * 0.5f) continue;
				if (f > 20000.f) break;
				if (std::fabs(f - nearest) < 6 * SR / N && nearest > 0) harm += p; else rest += p;
			}
			return 10.0 * std::log10(rest / harm);
		};
		double sw = -999;
		for (float f : {110.f, 440.f, 1760.f}) {
			double a = aliasS(0.6f, 0.f, f), b = aliasS(1.f, 0.f, f), c = aliasS(0.f, 0.5f, f), d = aliasS(0.f, 1.f, f);
			sw = std::max({sw, a, b});
			printf("  info  at %4.0f Hz: SHUFFLE 60%% %.1f dB   SHUFFLE 100%% %.1f dB   CRUSH 50%% %.1f dB   CRUSH 100%% %.1f dB\n", f, a, b, c, d);
		}
		check(sw < -45, string::f("SHUFFLE's cuts stay under -45 dB from 110 to 1760 Hz: worst %.1f dB", sw));
	}

	printf("== cost: 16 voices between grid points, percent of one core ==\n");
	{
		Run r;
		r.m.inputs[Strata::VOCT_INPUT].channels = 16; r.m.inputs[Strata::X_INPUT].channels = 16;
		for (int c = 0; c < 16; c++) { r.m.inputs[Strata::VOCT_INPUT].setVoltage(c * 0.07f - 0.5f, c); r.m.inputs[Strata::X_INPUT].setVoltage(c * 0.37f, c); }
		r.m.params[Strata::Y_PARAM].setValue(0.43f); r.m.params[Strata::Z_PARAM].setValue(0.6f);
		auto t0 = std::chrono::steady_clock::now(); r.run(2.f);
		double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		printf("  %.2f%%\n", 100.0 * t / 2.0);
	}

	printf("\n%d failure(s)\n", fails);
	return fails ? 1 : 0;
}

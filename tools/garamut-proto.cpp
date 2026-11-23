// Garamut prototype: measures the engine and renders listening WAVs.
// Free of Rack.
//
//   clang++ -std=c++11 -O2 -I src tools/garamut-proto.cpp -o /tmp/garamut-proto
//   /tmp/garamut-proto            measurements only
//   /tmp/garamut-proto renders    also writes renders/garamut/*.wav
//
// WAVs are written at a fixed gain (10 V = full scale), not normalised, so the
// level of one file against another is the module's.

#include "garamut-engine.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <functional>
#include <chrono>
#include <sys/stat.h>

using namespace garamut;
static const float SR = 48000.f;

static const char* noteName(float f, char* buf) {
	static const char* N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	float n = 12.f * std::log2(f / 440.f) + 69.f;
	int r = (int)std::lround(n);
	std::snprintf(buf, 16, "%s%d%+.0fc", N[((r % 12) + 12) % 12], r / 12 - 1, 100.f * (n - r));
	return buf;
}

struct Hit { float t; int ch, side; float y, vel; };

// Run the drum through a list of hits with settings that may change over time.
static void run(float secs, std::vector<Hit> hits, std::function<void(float, Params&)> set,
                std::vector<float>& L, std::vector<float>& R, Drum* keep = nullptr) {
	Drum local; Drum& D = keep ? *keep : local;
	D.setSampleRate(SR);
	Params P;
	size_t n = (size_t)(secs * SR);
	L.assign(n, 0.f); R.assign(n, 0.f);
	size_t hi = 0;
	std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.t < b.t; });
	for (size_t i = 0; i < n; i++) {
		float t = i / SR;
		if (i % 16 == 0) { set(t, P); D.update(P); }
		while (hi < hits.size() && hits[hi].t <= t) { D.strike(hits[hi].ch, hits[hi].side, hits[hi].y, hits[hi].vel); hi++; }
		D.process(L[i], R[i]);
	}
}

static float peakOf(const std::vector<float>& L, const std::vector<float>& R, size_t a = 0, size_t b = (size_t)-1) {
	float p = 0.f;
	b = std::min(b, L.size());
	for (size_t i = a; i < b; i++) p = std::max(p, std::max(std::fabs(L[i]), std::fabs(R[i])));
	return p;
}

static void writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R) {
	FILE* fp = std::fopen(path.c_str(), "wb");
	if (!fp) { std::printf("cannot write %s\n", path.c_str()); return; }
	uint32_t n = (uint32_t)L.size(), data = n * 4, sr = (uint32_t)SR, br = sr * 4;
	uint16_t ch = 2, bits = 16, fmt = 1, align = 4; uint32_t fmtLen = 16, riff = 36 + data;
	std::fwrite("RIFF", 1, 4, fp); std::fwrite(&riff, 4, 1, fp); std::fwrite("WAVEfmt ", 1, 8, fp);
	std::fwrite(&fmtLen, 4, 1, fp); std::fwrite(&fmt, 2, 1, fp); std::fwrite(&ch, 2, 1, fp);
	std::fwrite(&sr, 4, 1, fp); std::fwrite(&br, 4, 1, fp); std::fwrite(&align, 2, 1, fp);
	std::fwrite(&bits, 2, 1, fp); std::fwrite("data", 1, 4, fp); std::fwrite(&data, 4, 1, fp);
	int clipped = 0;
	for (uint32_t i = 0; i < n; i++) {
		float a = L[i] / 10.f, b = R[i] / 10.f;
		if (std::fabs(a) > 1.f || std::fabs(b) > 1.f) clipped++;
		int16_t x = (int16_t)std::lrint(std::max(-1.f, std::min(1.f, a)) * 32767.f);
		int16_t y = (int16_t)std::lrint(std::max(-1.f, std::min(1.f, b)) * 32767.f);
		std::fwrite(&x, 2, 1, fp); std::fwrite(&y, 2, 1, fp);
	}
	std::fclose(fp);
	std::printf("  %-30s peak %5.2f V%s\n", path.substr(path.rfind('/') + 1).c_str(), peakOf(L, R),
	            clipped ? "  CLIPPED" : "");
}

// One strike at the default settings with one parameter changed; reports pitch and level.
static void sweep(const char* name, std::function<void(Params&, float)> f, int steps = 9) {
	std::printf("\n%s\n", name);
	std::printf("  %5s  %-10s %-10s %8s %8s %8s %7s %7s %7s %8s\n", "knob", "left", "right", "air Hz", "tube1", "tube2", "peak V", "T60 s", "ms", "click V");
	for (int i = 0; i < steps; i++) {
		float u = (float)i / (steps - 1);
		Drum D; D.setSampleRate(SR);
		Params P; f(P, u); D.update(P);
		std::vector<float> L, R;
		run(2.f, {{0.01f, 0, 0, 0.5f, 0.7f}}, [&](float, Params& Q) { Q = P; }, L, R, &D);
		char a[16], b[16];
		float t60 = 6.91f / (D.baseZeta[0][0] * D.lip[0][0].omega);
		std::printf("  %5.2f  %-10s %-10s %8.1f %8.1f %8.1f %7.2f %7.2f %7.2f %8.3f\n", u, noteName(D.lipF0[0], a), noteName(D.lipF0[1], b),
		            D.fH, D.fTube[0], D.fTube[1], peakOf(L, R), t60, D.contactMs, D.pMalletPeak);
	}
}

int main(int argc, char** argv) {
	bool renders = argc > 1 && !std::strcmp(argv[1], "renders");

	// ── the default drum ──
	{
		Drum D; D.setSampleRate(SR);
		Params P; D.update(P);
		char a[16];
		std::printf("Default drum: H %.2f m, R %.3f m, slit %.0f%%..%.0f%% of the height, %.1f mm wide\n",
		            D.H, D.R, 100 * D.slit0, 100 * D.slit1, 1000 * D.slitW);
		std::printf("  Helmholtz %.1f Hz (%s), column air %.0f %.0f %.0f Hz\n", D.fH, noteName(D.fH, a), D.fTube[0], D.fTube[1], D.fTube[2]);
		for (int s = 0; s < 2; s++) {
			std::printf("  %s lip:", s ? "right" : "left ");
			for (int k = 0; k < NM; k++) std::printf("%s(%d,%d) %6.0f Hz x%.2f %.2fs", k % 6 ? " | " : "\n    ", D.mn[k][0] + 1, D.mn[k][1] + 1, D.modeF[s][k],
			            D.modeF[s][k] / D.modeF[s][0], 6.91f / (D.baseZeta[s][k] * D.lip[s][k].omega));
			std::printf("\n");
		}
	}

	sweep("SIZE", [](Params& P, float u) { P.size = u; });
	sweep("SHAPE (0 square, 0.5 round, 1 pear)", [](Params& P, float u) { P.shape = u; });
	sweep("SLIT", [](Params& P, float u) { P.slit = u; });
	sweep("MATERIAL (0 cedar, 0.33 hardwood, 0.67 aluminium, 1 steel)", [](Params& P, float u) { P.material = u; }, 13);
	sweep("MALLET (0 felt, 0.33 rubber, 0.67 wood, 1 brass)", [](Params& P, float u) { P.mallet = u; });
	sweep("MALLET on steel", [](Params& P, float u) { P.mallet = u; P.material = 1.f; });

	// interval accuracy
	{
		std::printf("\nINTERVAL accuracy (cents off), across shape x material, interval 7\n");
		float worst = 0.f;
		for (int a = 0; a <= 4; a++) for (int b = 0; b <= 4; b++) {
			Drum D; D.setSampleRate(SR); Params P; P.interval = 7.f; P.shape = a / 4.f; P.material = b / 4.f; D.update(P);
			float c = 1200.f * std::log2(D.lipF0[1] / D.lipF0[0]) - 700.f;
			worst = std::max(worst, std::fabs(c));
		}
		std::printf("  worst %.2f cents\n", worst);
	}

	// Y: which modes a strike excites
	{
		std::printf("\nY: mode shapes at the strike point, left lip (|phi|)\n  y    ");
		Drum D; D.setSampleRate(SR); Params P; D.update(P);
		for (int k = 0; k < NM; k++) std::printf(" (%d,%d)", D.mn[k][0] + 1, D.mn[k][1] + 1);
		std::printf("\n");
		for (int i = 0; i <= 8; i++) {
			float phi[NM]; D.shapesAt(i / 8.f, phi);
			std::printf("  %.3f", i / 8.f);
			for (int k = 0; k < NM; k++) std::printf(" %5.2f", std::fabs(phi[k]));
			std::printf("\n");
		}
	}

	// velocity
	{
		std::printf("\nVEL: peak and contact time\n");
		for (int i = 0; i <= 4; i++) {
			float v = i / 4.f;
			Drum D; std::vector<float> L, R;
			run(1.f, {{0.01f, 0, 0, 0.5f, v}}, [](float, Params&) {}, L, R, &D);
			std::printf("  vel %.2f  peak %5.2f V  contact %.2f ms\n", v, peakOf(L, R), D.contactMs);
		}
	}

	// two strikes on a ringing lip: the second meets a moving lip
	{
		std::printf("\nRepeated strikes on one point, 16 channels at once: peak\n");
		Drum D; std::vector<float> L, R;
		std::vector<Hit> h;
		for (int c = 0; c < 16; c++) h.push_back({0.01f, c, c & 1, 0.5f, 1.f});
		run(1.f, h, [](float, Params&) {}, L, R, &D);
		std::printf("  16 at full velocity: peak %.2f V\n", peakOf(L, R));
	}

	// cost
	{
		Drum D; D.setSampleRate(SR); Params P; D.update(P);
		const int N = 48000 * 10;
		float l, r; double sink = 0;
		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < N; i++) {
			if (i % 2400 == 0) D.strike((i / 2400) % 16, (i / 2400) & 1, 0.3f + 0.4f * ((i / 2400) % 5) / 4.f, 0.8f);
			if (i % 16 == 0) { P.slit = 0.5f + 0.3f * std::sin(i * 1e-5f); D.update(P); }
			D.process(l, r); sink += l;
		}
		double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("\nCPU: %.2f%% of a core (20 strikes/s, SLIT moving)  [%g]\n", 100.0 * s / 10.0, sink * 0);
	}

	if (!renders) return 0;
	std::string dir = "renders/garamut";
	mkdir("renders", 0755); mkdir(dir.c_str(), 0755);
	std::printf("\nRenders\n");
	std::vector<float> L, R;
	auto defaults = [](float, Params&) {};

	// 01 a groove alternating the lips
	{
		std::vector<Hit> h;
		float beat = 0.25f;
		int pat[16] = {1, 0, 2, 0, 1, 1, 2, 0, 1, 0, 2, 1, 0, 1, 2, 0};
		for (int bar = 0; bar < 4; bar++) for (int i = 0; i < 16; i++) {
			if (!pat[i]) continue;
			float t = 0.05f + (bar * 16 + i) * beat * 0.5f;
			h.push_back({t, i % 4, pat[i] - 1, 0.5f, (i % 4 == 0) ? 0.9f : 0.6f});
		}
		run(9.f, h, defaults, L, R); writeWav(dir + "/01-groove-two-lips.wav", L, R);
	}
	// 02 up the slit, bottom to top, then down
	{
		std::vector<Hit> h;
		for (int i = 0; i < 17; i++) h.push_back({0.05f + i * 0.4f, 0, 0, i <= 8 ? i / 8.f : (16 - i) / 8.f, 0.7f});
		run(8.f, h, defaults, L, R); writeWav(dir + "/02-strike-up-the-slit.wav", L, R);
	}
	// 03 materials, cedar to steel
	{
		std::vector<Hit> h;
		for (int i = 0; i < 9; i++) { h.push_back({0.05f + i * 1.6f, 0, 0, 0.5f, 0.7f}); h.push_back({0.85f + i * 1.6f, 0, 1, 0.5f, 0.7f}); }
		run(16.f, h, [](float t, Params& P) { P.material = std::min(1.f, std::floor(t / 1.6f) / 8.f); }, L, R);
		writeWav(dir + "/03-material-cedar-to-steel.wav", L, R);
	}
	// 04 mallets, felt to brass
	{
		std::vector<Hit> h;
		for (int i = 0; i < 9; i++) { h.push_back({0.05f + i * 1.2f, 0, 0, 0.5f, 0.7f}); h.push_back({0.65f + i * 1.2f, 0, 1, 0.5f, 0.7f}); }
		run(12.f, h, [](float t, Params& P) { P.mallet = std::min(1.f, std::floor(t / 1.2f) / 8.f); }, L, R);
		writeWav(dir + "/04-mallet-felt-to-brass.wav", L, R);
	}
	// 05 shapes, square to round to pear
	{
		std::vector<Hit> h;
		for (int i = 0; i < 9; i++) { h.push_back({0.05f + i * 1.4f, 0, 0, 0.5f, 0.7f}); h.push_back({0.75f + i * 1.4f, 0, 1, 0.5f, 0.7f}); }
		run(14.f, h, [](float t, Params& P) { P.shape = std::min(1.f, std::floor(t / 1.4f) / 8.f); }, L, R);
		writeWav(dir + "/05-shape-square-round-pear.wav", L, R);
	}
	// 06 slit, small to large
	{
		std::vector<Hit> h;
		for (int i = 0; i < 9; i++) { h.push_back({0.05f + i * 1.4f, 0, 0, 0.5f, 0.7f}); h.push_back({0.75f + i * 1.4f, 0, 1, 0.5f, 0.7f}); }
		run(14.f, h, [](float t, Params& P) { P.slit = std::min(1.f, std::floor(t / 1.4f) / 8.f); }, L, R);
		writeWav(dir + "/06-slit-small-to-large.wav", L, R);
	}
	// 07 size, small to large
	{
		std::vector<Hit> h;
		for (int i = 0; i < 9; i++) { h.push_back({0.05f + i * 1.6f, 0, 0, 0.5f, 0.7f}); h.push_back({0.85f + i * 1.6f, 0, 1, 0.5f, 0.7f}); }
		run(16.f, h, [](float t, Params& P) { P.size = std::min(1.f, std::floor(t / 1.6f) / 8.f); }, L, R);
		writeWav(dir + "/07-size-small-to-large.wav", L, R);
	}
	// 08 two players on one drum: a steady pulse on the left lip, a faster
	// pattern on the right that lands on top of it
	{
		std::vector<Hit> h;
		for (int i = 0; i < 24; i++) h.push_back({0.05f + i * 0.36f, 0, 0, 0.45f, 0.75f});
		for (int i = 0; i < 48; i++) if (i % 3 != 2) h.push_back({0.05f + i * 0.18f + 0.09f, 1, 1, 0.35f + 0.3f * ((i * 7) % 5) / 4.f, 0.45f + 0.1f * (i % 2)});
		for (int i = 0; i < 6; i++) h.push_back({1.49f + i * 1.44f, 2, 0, 0.8f, 0.9f});
		run(10.f, h, defaults, L, R); writeWav(dir + "/08-three-players.wav", L, R);
	}
	// 09 a roll on one spot, a rubber mallet on hardwood: each stroke meets a moving lip
	{
		std::vector<Hit> h;
		for (int i = 0; i < 60; i++) { float t = 0.05f + i * 0.045f; float v = 0.3f + 0.5f * std::sin(3.14159f * i / 59.f); h.push_back({t, i % 2, 0, 0.5f, v}); }
		run(6.f, h, defaults, L, R); writeWav(dir + "/09-roll.wav", L, R);
	}
	// 11 ROLL held high: a crescendo through VEL, then a faster roll, then one
	// that alternates the lips over a groove on channel 2
	{
		Drum D; D.setSampleRate(SR);
		Params P; D.update(P);
		Roller ro[2];
		ro[1].rng = 0x9E3779B9u;
		size_t n = (size_t)(12.f * SR);
		L.assign(n, 0.f); R.assign(n, 0.f);
		int rolls = 0;
		for (size_t i = 0; i < n; i++) {
			float t = i / SR;
			bool high = (t > 0.1f && t < 3.1f) || (t > 4.f && t < 6.f) || (t > 7.f && t < 10.f);
			float rate = t < 4.f ? 12.f : (t < 7.f ? 24.f : 9.f);
			float vel = t < 3.1f ? 0.1f + 0.8f * (t - 0.1f) / 3.f : 0.55f;
			float vm, dy;
			if (ro[0].step(high, rate, 1.f / SR, vm, dy)) {
				int side = t > 7.f ? (rolls & 1) : 0;
				D.strike(0, side, 0.5f + dy, std::min(1.f, vel * vm));
				rolls++;
			}
			if (t > 7.f && std::fmod(t - 7.f, 0.444f) < 1.f / SR) D.strike(1, 1, 0.3f, 0.8f);
			D.process(L[i], R[i]);
		}
		writeWav(dir + "/11-roll-gate.wav", L, R);
		std::printf("  %d roll strokes\n", rolls);
	}
	// 10 a big steel drum, brass mallet
	{
		std::vector<Hit> h;
		for (int i = 0; i < 8; i++) h.push_back({0.05f + i * 0.9f, i % 2, i % 2, 0.3f + 0.08f * i, 0.8f});
		run(12.f, h, [](float, Params& P) { P.material = 1.f; P.mallet = 0.85f; P.size = 0.8f; P.interval = 3.f; }, L, R);
		writeWav(dir + "/10-big-steel-brass.wav", L, R);
	}
	return 0;
}

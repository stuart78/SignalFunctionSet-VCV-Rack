// Every space Cistern ships, loaded, traced and played, without Rack.
//
//   clang++ -O2 -std=c++17 -Isrc tools/cistern-spaces-harness.cpp -o /tmp/cistern-spaces-harness
//   /tmp/cistern-spaces-harness [out dir for impulse WAVs]
//
// For each: the room's size, the decay the rays measure (and, on the cistern,
// that the rays agree with Eyring, which is exact there), that the default
// positions stand in the air, how long a trace takes, and the rendered decay
// at 500 Hz and 2 kHz against what the program asked for.
#include "cistern-engine.hpp"
#include <chrono>
#include <cstdio>
#include <string>

using namespace cistern;
static const double SR = 48000;
static int fails = 0;
#define CHECK(cond, ...) do { bool ok_ = (cond); std::printf("  [%s] ", ok_ ? "ok" : "FAIL"); std::printf(__VA_ARGS__); std::printf("\n"); if (!ok_) fails++; } while (0)

// Brick-wall octaves in the frequency domain (a gentle bandpass lets a slower
// octave leak in, and once the band has fallen 20 dB the leak is the
// measurement), and the target integrated over the same octave: within one
// octave the slowest frequencies dominate late, and a room whose decay falls
// steeply across frequency (the carpeted mosque: 3.5 s at 125 Hz, 0.7 s at
// 1 kHz) reads long against its centre value for that reason alone.
#include <complex>
static void fft(std::vector<std::complex<double>>& a) {
	size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; i++) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
	for (size_t len = 2; len <= n; len <<= 1) {
		double ang = -2 * M_PI / len; std::complex<double> wl(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t j = 0; j < len / 2; j++) {
			auto u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } }
	}
}
static double octWin(double f, double fc) {
	double lo = fc / std::sqrt(2.0), hi = fc * std::sqrt(2.0), e = std::pow(2.0, 1.0 / 12);
	if (f >= lo && f <= hi) return 1.0;
	if (f > lo / e && f < lo) return 0.5 - 0.5 * std::cos(M_PI * std::log(f / (lo / e)) / std::log(e));
	if (f > hi && f < hi * e) return 0.5 + 0.5 * std::cos(M_PI * std::log(f / hi) / std::log(e));
	return 0.0;
}
static double fitT20(const std::vector<double>& e) {   // e: energy per sample
	std::vector<double> edc(e.size());
	double acc = 0; for (size_t i = e.size(); i-- > 0;) { acc += e[i]; edc[i] = acc; }
	double top = edc[0], n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
	for (size_t i = 0; i < edc.size(); i += 8) {
		double d = 10 * std::log10(edc[i] / top + 1e-300);
		if (d > -5 || d < -25) continue;
		double t = i / SR; n++; sx += t; sy += d; sxx += t * t; sxy += t * d;
	}
	if (n < 4) return -1;
	return -60.0 / ((n * sxy - sx * sy) / (n * sxx - sx * sx));
}
static double t20(const std::vector<float>& x, double fc) {
	size_t N = 1; while (N < x.size()) N <<= 1;
	std::vector<std::complex<double>> X(N);
	for (size_t i = 0; i < x.size(); i++) X[i] = x[i];
	fft(X);
	for (size_t k = 0; k < N; k++) { double f = (k <= N / 2 ? k : N - k) * SR / N; X[k] = std::conj(X[k] * octWin(f, fc)); }
	fft(X);
	std::vector<double> e(x.size());
	for (size_t i = 0; i < x.size(); i++) { double y = X[i].real() / N; e[i] = y * y; }
	return fitT20(e);
}
// what a decay of t60[] (log-interpolated between octave centres) gives over an octave
static double t20Expected(const double t60[NB], double t8k, double fc, size_t n) {
	std::vector<double> e(n, 0.0);
	for (int k = 0; k < 200; k++) {
		double f = fc / 2 * std::pow(4.0, (k + 0.5) / 200), w = octWin(f, fc) * f;
		double lf = std::log2(f / 125.0), T;
		if (lf <= 0) T = t60[0];
		else if (lf >= NB) T = t8k;
		else { int i = (int)lf; double u = lf - i, a = t60[i], b = i + 1 < NB ? t60[i + 1] : t8k; T = std::exp(std::log(a) * (1 - u) + std::log(b) * u); }
		double r = 13.8155 / T;
		for (size_t i = 0; i < n; i += 16) e[i] += w * std::exp(-r * i / SR);
	}
	return fitT20(e);
}
static void writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R) {
	FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
	float peak = 1e-9f; for (size_t i = 0; i < L.size(); i++) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
	uint32_t n = (uint32_t)L.size(), data = n * 8, rate = (uint32_t)SR, br = rate * 8, riff = 36 + data, s16 = 16; uint16_t fmt = 3, ch = 2, ba = 8, bits = 32;
	std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&s16, 4, 1, f);
	std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f); std::fwrite(&br, 4, 1, f);
	std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
	for (size_t i = 0; i < n; i++) { float s[2] = {0.7f * L[i] / peak, 0.7f * R[i] / peak}; std::fwrite(s, 4, 2, f); }
	std::fclose(f);
}

int main(int argc, char** argv) {
	std::string out = argc > 1 ? argv[1] : "";
	std::printf("== the rays against Eyring, in a shoebox with fully diffuse walls (where Eyring holds)\n");
	{
		// 20 x 10 x 15 m of stone; every reflection Lambertian, so the field is diffuse
		Space b; b.name = "box";
		double X = 10, Y = 10, Z = 7.5;
		b.planes = {{V3(0, 1, 0), 0, M_STONE}, {V3(0, -1, 0), -Y, M_STONE}, {V3(1, 0, 0), -X, M_STONE},
		            {V3(-1, 0, 0), -X, M_STONE}, {V3(0, 0, 1), -Z, M_STONE}, {V3(0, 0, -1), -Z, M_STONE}};
		b.volume = 2 * X * Y * 2 * Z; b.area[M_STONE] = 2 * (2 * X * Y + 2 * X * 2 * Z + Y * 2 * Z);
		b.lo = V3(-X, 0, -Z); b.hi = V3(X, Y, Z);
		BuildParams bp; bp.scatter = 1.0;
		double t60[NB], t8, mfp, ey[NB];
		rayDecay(b, bp, V3(2, 3, 1), t60, t8, mfp);
		decayTimes(b, bp.tempC, bp.rh, ey);
		for (int k = 0; k < NB; k++) {
			double e = (t60[k] - ey[k]) / ey[k] * 100;
			CHECK(std::fabs(e) < 8, "%4.0f Hz: rays %.2f s, Eyring %.2f s (%+.0f%%)", BAND_HZ[k], t60[k], ey[k], e);
		}
		CHECK(std::fabs(mfp - b.meanFreePath()) / b.meanFreePath() < 0.05, "mean free path: rays %.2f m, 4V/S %.2f m", mfp, b.meanFreePath());
	}
	std::printf("== and on the cistern, which is not diffuse (for information)\n");
	{
		Space c = makeCistern();
		BuildParams bp; double t60[NB], t8, mfp, ey[NB];
		rayDecay(c, bp, c.src, t60, t8, mfp);
		decayTimes(c, bp.tempC, bp.rh, ey);
		std::printf("  rays");
		for (int k = 0; k < NB; k++) std::printf("  %.0f Hz %.1f s (Eyring %.1f)", BAND_HZ[k], t60[k], ey[k]);
		std::printf("\n  mean free path: rays %.2f m, 4V/S %.2f m\n", mfp, c.meanFreePath());
	}
	std::printf("== distance, musical and true (Leake Street, a source 150 m down the tunnel)\n");
	{
		int id = -1; for (int k = 0; k < NSPACES; k++) if (std::string(SPACES[k].slug) == "leake") id = k;
		Space s; std::string err; makeSpace(id, "res/spaces", s, err);
		// the farthest point down the tunnel from the listener, up to 150 m
		V3 far = s.lis; double best = 0;
		for (double dx = -150; dx <= 150; dx += 0.5) for (double dz = -40; dz <= 40; dz += 1) {
			V3 q = s.lis + V3(dx, 0, dz); double d = (q - s.lis).len();
			if (d <= 150 && d > best && s.inside(q)) { best = d; far = q; }
		}
		double arr[2], lvl[2];
		for (int t = 0; t < 2; t++) {
			BuildParams bp; bp.scatter = 0.5; bp.trueDistance = t == 1; bp.rays = 40000;
			auto T = trace(s, bp, SR, &far, 1, s.lis);
			auto P = assemble(*T, 1.0);
			int first = 1 << 30; double e = 0;
			for (const Tap& tp : P->taps) if (tp.delay < first) { first = tp.delay; e = tp.g[0][1] * tp.g[0][1] + tp.g[1][1] * tp.g[1][1]; }
			arr[t] = first / SR; lvl[t] = 10 * std::log10(e + 1e-30);
		}
		std::printf("  source %.0f m away: first arrival %.0f ms musical, %.0f ms true; %+.1f dB louder musical\n", best, arr[0] * 1000, arr[1] * 1000, lvl[0] - lvl[1]);
		CHECK(std::fabs(arr[1] - best / C_SOUND) < 0.01, "true distance arrives at d / c (%.0f ms against %.0f)", arr[1] * 1000, best / C_SOUND * 1000);
		CHECK(std::fabs(arr[0] - musicalDistance(best) / C_SOUND) < 0.01, "musical distance arrives as from %.0f m (%.0f ms)", musicalDistance(best), arr[0] * 1000);
		CHECK(musicalDistance(20) == 20 && musicalDistance(300) > musicalDistance(150), "nothing changes within 25 m, and further still sounds further");
	}
	for (int id = 0; id < NSPACES; id++) {
		const SpaceInfo& I = SPACES[id];
		std::printf("== %s\n", I.name);
		Space s; std::string err;
		auto t0 = std::chrono::steady_clock::now();
		bool ok = makeSpace(id, "res/spaces", s, err);
		double loadSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		CHECK(ok, "loads%s%s (%.2f s, %zu triangles)", ok ? "" : ": ", err.c_str(), loadSec, s.tris.size());
		if (!ok) continue;
		V3 ext = s.hi - s.lo;
		double open = s.area[M_OPEN], S = s.surface();
		std::printf("  %.1f x %.1f x %.1f m, %.0f m3, %.0f m2 of surface (%.1f%% open)\n", ext.x, ext.y, ext.z, s.volume, S, 100 * open / S);
		CHECK(s.inside(s.src) && s.inside(s.lis), "the default source and listener stand in the air (%.1f m apart)", (s.src - s.lis).len());
		BuildParams bp; bp.scatter = 0.5;                  // as the panel defaults it
		V3 src[2] = {s.src, s.src};
		t0 = std::chrono::steady_clock::now();
		auto T = trace(s, bp, SR, src, 1, s.lis);
		double traceSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		auto P = assemble(*T, 1.0);
		std::printf("  T60");
		for (int b = 0; b < NB; b++) std::printf("  %.0f Hz %.2f s", BAND_HZ[b], P->t60[b]);
		std::printf("\n  mean free path %.2f m, mixing %.0f ms, %zu taps from %d detections, traced in %.2f s\n",
		            P->mfp, P->tMix * 1000, P->taps.size(), P->detections, traceSec);
		std::printf("  the level at the join: rays %.3g, diffuse theory %.3g (%+.1f dB)\n", P->eEndRays, P->eEndDiffuse,
		            10 * std::log10(P->eEndRays / P->eEndDiffuse + 1e-30));
		CHECK(traceSec < 2.0, "a trace takes under 2 s per source (%.2f s)", traceSec);
		CHECK(P->detections > 100, "the receiver catches enough rays (%d)", P->detections);
		if (getenv("QUICK")) continue;
		double len = std::min(70.0, std::max(3.0, 1.6 * P->t60[0]));
		Reverb rv; rv.setProgram(P.get(), 1.0, 7);
		size_t n = (size_t)(len * SR);
		std::vector<float> L(n), R(n), M(n);
		for (size_t i = 0; i < n; i++) { float a = i == 0 ? 1.f : 0.f; rv.process(a, a, L[i], R[i]); M[i] = 0.5f * (L[i] + R[i]); }
		// the decay is checked on the TAIL alone: in a small room with the
		// source close, the direct sound and the early field are most of the
		// energy, and the full response's first 25 dB is their fall
		Program tp = *P; tp.taps.clear();
		Reverb tr; tr.setProgram(&tp, 1.0, 7);
		std::vector<float> Mt(n); float a2, b2;
		for (size_t i = 0; i < n; i++) { float a = i == 0 ? 1.f : 0.f; tr.process(a, a, a2, b2); Mt[i] = 0.5f * (a2 + b2); }
		for (double f : {500.0, 2000.0}) {
			int b = f == 500 ? 2 : 4;
			double t = t20(Mt, f), x = t20Expected(P->t60, P->t60x8k, f, n), e = (t - x) / x * 100;
			CHECK(t > 0 && std::fabs(e) < 12, "the tail's T20 at %.0f Hz %.2f s against %.2f s over the octave (%+.0f%%; %.2f s at the centre)", f, t, x, e, P->t60[b]);
		}
		if (!out.empty()) writeWav(out + "/" + I.slug + "-impulse.wav", L, R);
	}
	std::printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
	return fails ? 1 : 0;
}

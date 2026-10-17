// Cistern's engine, measured on its own (no Rack).
//
//   clang++ -O2 -std=c++17 -Isrc tools/cistern-harness.cpp -o /tmp/cistern-harness
//   /tmp/cistern-harness [seconds] [out.wav]
//
// Renders the impulse response of the default space and checks, per octave
// band, that the decay the listener hears is the decay the room's physics
// predicts (Schroeder integration, T30 fit, against Eyring with air), that the
// early reflections hand over to the tail without a step, that the tail does
// not ring, and what it costs.
#include "cistern-engine.hpp"
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>

using namespace cistern;
static const double SR = 48000;
static int fails = 0;
#define CHECK(cond, ...) do { bool ok_ = (cond); std::printf("  [%s] ", ok_ ? "ok" : "FAIL"); std::printf(__VA_ARGS__); std::printf("\n"); if (!ok_) fails++; } while (0)

// RBJ bandpass, one octave wide, run twice: a 4th-order octave filter
struct Biquad { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
	double run(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
static Biquad bandpass(double f) {
	double w = 2 * M_PI * f / SR, bw = 1.0;
	double alpha = std::sin(w) * std::sinh(std::log(2.0) / 2 * bw * w / std::sin(w));
	double a0 = 1 + alpha;
	Biquad q; q.b0 = alpha / a0; q.b1 = 0; q.b2 = -alpha / a0; q.a1 = -2 * std::cos(w) / a0; q.a2 = (1 - alpha) / a0;
	return q;
}
static std::vector<double> band(const std::vector<float>& x, double f) {
	Biquad a = bandpass(f), b = bandpass(f);
	std::vector<double> y(x.size());
	for (size_t i = 0; i < x.size(); i++) y[i] = b.run(a.run(x[i]));
	return y;
}
// Schroeder backward integral in dB, and a T30 fit (-5 to -35 dB) by regression
static double t30(const std::vector<double>& y, double* edt = nullptr) {
	std::vector<double> edc(y.size());
	double s = 0; for (size_t i = y.size(); i-- > 0;) { s += y[i] * y[i]; edc[i] = s; }
	double top = edc[0];
	auto fit = [&](double hi, double lo) {
		double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
		for (size_t i = 0; i < edc.size(); i += 16) {
			double d = 10 * std::log10(edc[i] / top + 1e-300);
			if (d > hi || d < lo) continue;
			double t = i / SR; n++; sx += t; sy += d; sxx += t * t; sxy += t * d;
		}
		if (n < 4) return -1.0;
		double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
		return -60.0 / slope;
	};
	if (edt) *edt = fit(0, -10);
	return fit(-5, -35);
}
static void fft(std::vector<std::complex<double>>& a) {
	size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; i++) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
	for (size_t len = 2; len <= n; len <<= 1) {
		double ang = -2 * M_PI / len; std::complex<double> wl(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t j = 0; j < len / 2; j++) {
			auto u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } }
	}
}
static void writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R) {
	FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
	float peak = 1e-9f; for (size_t i = 0; i < L.size(); i++) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
	uint32_t n = (uint32_t)L.size(), data = n * 8, rate = (uint32_t)SR, br = rate * 8; uint16_t fmt = 3, ch = 2, ba = 8, bits = 32;
	uint32_t riff = 36 + data, sixteen = 16;
	std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&sixteen, 4, 1, f);
	std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f); std::fwrite(&br, 4, 1, f);
	std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
	for (size_t i = 0; i < L.size(); i++) { float s[2] = {0.7f * L[i] / peak, 0.7f * R[i] / peak}; std::fwrite(s, 4, 2, f); }
	std::fclose(f);
}

int main(int argc, char** argv) {
	double seconds = argc > 1 ? std::atof(argv[1]) : 60.0;
	std::string wav = argc > 2 ? argv[2] : "";

	std::printf("== the space\n");
	Space s = makeCistern();
	std::printf("  %s: V %.0f m3, S %.0f m2, mean free path %.2f m, %d triangles\n",
		s.name, s.volume, s.surface(), s.meanFreePath(), (int)s.tris.size());
	std::printf("  air at 20 C 50%%: ");
	for (int b = 0; b < NB; b++) std::printf("%.0f Hz %.4f dB/m  ", BAND_HZ[b], airDbPerM(BAND_HZ[b], 20, 50));
	std::printf("\n");
	// ISO 9613-1's own table, 20 C 50%: 0.022 (125 is 0.44 dB/km)... spot values
	// ISO 9613-1's own table at 20 C, 70% (dB/km): 1 kHz 4.98, 2 kHz 9.02, 4 kHz 22.9, 8 kHz 76.6
	const double isoF[4] = {1000, 2000, 4000, 8000}, isoV[4] = {4.98, 9.02, 22.9, 76.6};
	for (int k = 0; k < 4; k++) { double v = airDbPerM(isoF[k], 20, 70) * 1000;
		CHECK(std::fabs(v - isoV[k]) / isoV[k] < 0.03, "air at %.0f Hz, 20 C 70%%: %.2f dB/km against ISO 9613-1's %.2f", isoF[k], v, isoV[k]); }

	BuildParams bp; bp.rays = 100000;
	auto t0 = std::chrono::steady_clock::now();
	auto P = build(s, bp, SR);
	calibrate(*P, 1.0, 7);
	double traceSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("  predicted T60 (Eyring + air):");
	for (int b = 0; b < NB; b++) std::printf(" %.0f Hz %.1f s", BAND_HZ[b], P->t60[b]);
	std::printf("\n  mixing time %.0f ms, %d taps, %d detections from %d rays, built in %.2f s, tail gain %.3g, tail starts %.0f ms\n",
		P->tMix * 1000, (int)P->taps.size(), P->detections, P->rays, traceSec, P->tailGain, P->tailDelay * 1000.0 / SR);
	CHECK(P->t60[0] > 35 && P->t60[0] < 60, "the cistern's low-frequency decay is near the quoted 45 s (%.1f s)", P->t60[0]);

	std::printf("== the impulse response (%.0f s)\n", seconds);
	Reverb rv; rv.setProgram(P.get(), 1.0, 7);
	size_t n = (size_t)(seconds * SR);
	std::vector<float> L(n), R(n);
	auto c0 = std::chrono::steady_clock::now();
	for (size_t i = 0; i < n; i++) { float a = i == 0 ? 1.f : 0.f; rv.process(a, a, L[i], R[i]); }
	double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count() / seconds * 100;
	std::vector<float> M(n); for (size_t i = 0; i < n; i++) M[i] = 0.5f * (L[i] + R[i]);

	std::printf("== decay per band (rendered T30 against the physics integrated over the octave)\n");
	{
		// Brick-wall octaves in the frequency domain: a 4th-order bandpass leaks
		// the slower octave below into the one being measured, and once the band
		// itself has fallen 30 dB the leak IS the measurement.
		size_t N = 1; while (N < n) N <<= 1;
		std::vector<std::complex<double>> X(N);
		for (size_t i = 0; i < n; i++) X[i] = M[i];
		fft(X);
		auto win = [](double f, double fc) {
			double lo = fc / std::sqrt(2.0), hi = fc * std::sqrt(2.0), e = std::pow(2.0, 1.0 / 12);
			if (f >= lo && f <= hi) return 1.0;
			if (f > lo / e && f < lo) return 0.5 - 0.5 * std::cos(M_PI * std::log(f / (lo / e)) / std::log(e));
			if (f > hi && f < hi * e) return 0.5 + 0.5 * std::cos(M_PI * std::log(f / hi) / std::log(e));
			return 0.0;
		};
		// the decay the program asked for, between octave centres in log f and
		// log T (the engine measures a built room's decay from rays, which
		// Eyring on its triangle areas, hidden faces and all, does not match)
		auto Tat = [&](double f) {
			double lf = std::log2(f / 125.0);
			if (lf <= 0) return P->t60[0];
			if (lf >= NB) return P->t60x8k;
			int i = (int)lf; double u = lf - i, a = P->t60[i], b = i + 1 < NB ? P->t60[i + 1] : P->t60x8k;
			return std::exp(std::log(a) * (1 - u) + std::log(b) * u);
		};

		for (int b = 0; b < NB; b++) {
			std::vector<std::complex<double>> Y(N);
			for (size_t k = 0; k < N; k++) { double f = (k <= N / 2 ? k : N - k) * SR / N; Y[k] = std::conj(X[k] * win(f, BAND_HZ[b])); }
			fft(Y);
			std::vector<double> y(n); for (size_t i = 0; i < n; i++) y[i] = Y[i].real() / N;
			double edt, t = t30(y, &edt);
			// the same fit on the curve the physics predicts for a flat impulse
			// through the same window: every frequency in the octave decaying at its own rate
			std::vector<double> pr(n);
			for (int k = 0; k < 400; k++) {
				double f = BAND_HZ[b] / 2 * std::pow(4.0, (k + 0.5) / 400), wgt = win(f, BAND_HZ[b]) * f;  // per log step, energy per Hz
				double T = Tat(f), r = 13.8155 / T;   // energy: 60 dB is ln(10^6)
				for (size_t i = 0; i < n; i += 64) pr[i] += wgt * std::exp(-r * i / SR);
			}
			std::vector<double> ps(n, 0.0); for (size_t i = 0; i < n; i += 64) ps[i] = std::sqrt(pr[i] * 64);
			double tgt = t30(ps);
			double err = (t - tgt) / tgt * 100;
			// the top octave gets 15%: the decay falls steepest there (4 kHz to 8 kHz
		// is 4.3 s to 1.5 s in the cistern), and between octave centres the
		// tail's shelves and this log interpolation part company most
		CHECK(t > 0 && std::fabs(err) < (b == NB - 1 ? 15 : 10), "%4.0f Hz: T30 %.1f s (EDT %.1f), asked for over the octave %.1f s (%+.0f%%), at the centre %.1f s",
				BAND_HZ[b], t, edt, tgt, err, P->t60[b]);
		}
	}

	std::printf("== the join at %.0f ms (energy in 10 ms windows, decay slope removed)\n", P->tMix * 1000);
	{
		int w = (int)(0.010 * SR);
		double tm = 0.5 * (P->t60[2] + P->t60[3]);
		auto win = [&](double tc) { int i0 = (int)(tc * SR) - w / 2; double e = 0; for (int i = i0; i < i0 + w; i++) e += M[i] * M[i];
			return 10 * std::log10(e / w + 1e-30) + 60.0 * tc / tm; };   // undo the room's decay so a level join is flat
		std::printf("  ");
		for (double tc = P->tMix - 0.06; tc < P->tMix + 0.065; tc += 0.01) std::printf("%+.0fms %.1f  ", (tc - P->tMix) * 1000, win(tc));
		std::printf("\n");
		double before = 0, after = 0;
		for (int k = 1; k <= 4; k++) { before += win(P->tMix - 0.01 * k); after += win(P->tMix + 0.01 * k); }
		before /= 4; after /= 4;
		double step = after - before;
		CHECK(std::fabs(step) < 3, "level across the join %+.1f dB", step);
		double late = 0; for (int k = 0; k < 10; k++) late += win(0.5 + 0.05 * k); late /= 10;
		CHECK(std::fabs(late - after) < 4, "the tail holds its level once decay is removed (%.1f dB at 0.5-1 s against %.1f just after the join)", late, after);
	}

	std::printf("== no echo where the tail comes in (the tail alone: peak / rms in 10 ms windows; diffuse noise sits near 3-4.5)\n");
	{
		// A level check cannot see this: one bare click and a burst of noise can
		// carry the same energy. Crest factor can. Measured on the tail ALONE,
		// because the early field is meant to be discrete (and the round wall's
		// focus near 2R/c is a real feature of a round room, reported below).
		Program tp = *P; tp.taps.clear();
		Reverb tr; tr.setProgram(&tp, 1.0, 7);
		int len = (int)((P->tMix + 0.5) * SR); std::vector<float> m(len); float a, b;
		for (int i = 0; i < len; i++) { float x = i == 0 ? 1.f : 0.f; tr.process(x, x, a, b); m[i] = 0.5f * (a + b); }
		// a window counts once it carries a tenth of the tail's settled energy:
		// a burst just arriving is a few pulses in near-silence, which scores a
		// high crest at any level, and what makes an echo audible is its level
		int w = (int)(0.010 * SR); double worst = 0, wt = 0;
		double settled = 0; int ns = 0;
		for (int i = (int)((P->tMix + 0.2) * SR); i < len; i++) { settled += m[i] * m[i]; ns++; }
		settled = settled / ns * w;
		std::printf("  ");
		for (int i0 = 0; i0 + w < len; i0 += w) {
			double e = 0, pk = 0; for (int i = i0; i < i0 + w; i++) { e += m[i] * m[i]; pk = std::max(pk, (double)std::fabs(m[i])); }
			if (e < 1e-14) continue;
			double cr = pk / std::sqrt(e / w + 1e-30), lv = 10 * std::log10(e / settled);
			if ((i0 / w) % 3 == 0 || cr > 6) std::printf("%.0fms %.1f (%+.0f dB)  ", i0 * 1000.0 / SR, cr, lv);
			if (lv < -10) continue;
			if (cr > worst) { worst = cr; wt = i0 / SR; }
		}
		std::printf("\n");
		CHECK(worst < 6, "largest crest factor in the tail's first half second: %.1f at %.0f ms", worst, wt * 1000);
		// the early field, for information: where it bunches up
		Program ep = *P; ep.tailGain = 0;
		Reverb er; er.setProgram(&ep, 1.0, 7);
		std::vector<double> e10(len / w + 1, 0.0);
		for (int i = 0; i < len; i++) { float x = i == 0 ? 1.f : 0.f; er.process(x, x, a, b); double v = 0.5 * (a + b); e10[i / w] += v * v; }
		double focus = 0, fwhen = 0, med = 0; int c = 0;
		for (int k = (int)(0.06 * SR / w); k < (int)(P->tMix * SR / w); k++) { med += e10[k]; c++; if (e10[k] > focus) { focus = e10[k]; fwhen = k * w / SR; } }
		med /= c;
		std::printf("  early field: loudest 10 ms after 60 ms is at %.0f ms, %+.1f dB over the field's mean (a round wall focuses at 2R/c = %.0f ms)\n",
			fwhen * 1000, 10 * std::log10(focus / med), 2 * 28.35 / C_SOUND * 1000);
	}

	std::printf("== ringing (Welch spectrum of 2..%.0f s, excess over its own third-octave smoothing)\n", std::min(seconds, 12.0));
	{
		const int N = 8192; std::vector<double> psd(N / 2, 0.0); int frames = 0;
		for (size_t st = (size_t)(2 * SR); st + N < std::min(n, (size_t)(12 * SR)); st += N / 2) {
			std::vector<std::complex<double>> a(N);
			for (int i = 0; i < N; i++) a[i] = M[st + i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / N));
			fft(a); for (int i = 0; i < N / 2; i++) psd[i] += std::norm(a[i]); frames++;
		}
		double worst = 0, wf = 0;
		for (int i = 1; i < N / 2; i++) {
			double f = i * SR / N; if (f < 100 || f > 8000) continue;
			double lo = f / std::pow(2, 1.0 / 6), hi = f * std::pow(2, 1.0 / 6), sm = 0; int c = 0;
			for (int j = std::max(1, (int)(lo * N / SR)); j <= std::min(N / 2 - 1, (int)(hi * N / SR)); j++) { sm += psd[j]; c++; }
			double ex = 10 * std::log10(psd[i] / (sm / c));
			if (ex > worst) { worst = ex; wf = f; }
		}
		// a Welch average of this many frames has its own spread; 8 dB is a peak the ear would pick out
		CHECK(worst < 8, "largest spectral peak %.1f dB above its neighbourhood, at %.0f Hz (%d frames)", worst, wf, frames);
	}

	std::printf("== stereo\n");
	{
		double ll = 0, rr = 0, lr = 0;
		for (size_t i = (size_t)(0.5 * SR); i < (size_t)(3 * SR); i++) { ll += L[i] * L[i]; rr += R[i] * R[i]; lr += L[i] * R[i]; }
		double corr = lr / std::sqrt(ll * rr);
		CHECK(std::fabs(corr) < 0.4, "late L/R correlation %.2f (a believable room is decorrelated)", corr);
		CHECK(std::fabs(10 * std::log10(ll / rr)) < 1.5, "late L/R balance %+.2f dB", 10 * std::log10(ll / rr));
	}

	std::printf("== a retrace while playing (room shrunk to 0.8 and the source moved, 2 s into continuous noise)\n");
	{
		// The module retraces whenever a source moves or SIZE turns, while sound
		// is going in. Compared against the same render with no retrace: before
		// the switch they are identical, and across the 40 ms crossfade the level
		// must not dip or bump. (A 10 ms window of this tail swings 3 dB on its
		// own, so it is judged in 40 ms windows against the windows either side.)
		Space s2 = scaledCopy(makeCistern(), 0.8);
		V3 src2 = s2.src + V3(3, 0, -2);
		auto T2 = trace(s2, bp, SR, &src2, 1, s2.lis);
		auto P2 = assemble(*T2, 1.0);
		int len = (int)(3.0 * SR), at = (int)(2.0 * SR);
		auto render = [&](bool sw, std::vector<float>& m, const Program** left) {
			Reverb rv2; rv2.setProgram(P.get(), 1.0, 7);
			m.assign(len, 0.f); float a, b; Rng nz(3);
			for (int i = 0; i < len; i++) {
				float x = (float)(nz.u() * 2 - 1);
				if (sw && i == at) rv2.adopt(P2.get());
				rv2.process(x, x, a, b); m[i] = 0.5f * (a + b);
				if (const Program* q = rv2.finished()) if (left) *left = q;
			}
		};
		std::vector<float> ms, mr; const Program* left = nullptr;
		render(true, ms, &left); render(false, mr, nullptr);
		int w = (int)(0.040 * SR);
		auto lvl = [&](const std::vector<float>& m, int i0) { double e = 0; for (int i = i0; i < i0 + w; i++) e += m[i] * m[i]; return 10 * std::log10(e / w + 1e-30); };
		double before = 0, after = 0;
		for (int k = 2; k <= 5; k++) { before += lvl(ms, at - k * w); after += lvl(ms, at + k * w); }
		before /= 4; after /= 4;
		double mid = lvl(ms, at - w / 2), mid2 = lvl(ms, at + w / 2);
		double ref = 0.5 * (before + after);
		std::printf("  40 ms windows: before %.2f, across the crossfade %.2f / %.2f, after %.2f dB\n", before, mid, mid2, after);
		CHECK(std::fabs(mid - ref) < 1.0 && std::fabs(mid2 - ref) < 1.0, "no dip or bump across the crossfade (%+.2f, %+.2f dB against the windows either side)", mid - ref, mid2 - ref);
		CHECK(std::fabs(after - before) < 1.5, "the wet level holds through a change of room (%+.2f dB)", after - before);
		double dmax = 0; for (int i = at - 100; i < at; i++) dmax = std::max(dmax, (double)std::fabs(ms[i] - mr[i]));
		CHECK(dmax == 0, "identical to the unretraced render up to the switch");
		CHECK(left == P.get(), "the crossfade hands the old program back to be freed");
	}

	std::printf("== cost\n");
	CHECK(cpu < 8, "%.2f%% of a core at 48 kHz, %d taps", cpu, (int)P->taps.size());

	if (!wav.empty()) { writeWav(wav, L, R); std::printf("wrote %s\n", wav.c_str()); }
	std::printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
	return fails ? 1 : 0;
}

// Measures src/diapason-engine.hpp without Rack and writes listening renders.
//
//   clang++ -std=c++11 -O3 -funsafe-math-optimizations -I src tools/diapason-proto.cpp -o /tmp/diapason-proto
//   /tmp/diapason-proto [renders/diapason]
//
// Checks: every stop sounds at its pitch, the pedal reaches 16 Hz, a coupler
// plays the same pipe once, the céleste is silent below tenor C, a big chord
// sags the wind and a regulated one does not, a pipe takes longer to speak the
// lower it is, and the cost of a full organ.
#include "diapason-engine.hpp"
#include <chrono>
#include <cstdio>
#include <string>

using namespace diapason;
static int fails = 0;
static void check(bool ok, const std::string& s) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", s.c_str()); if (!ok) fails++; }

static void wav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, float sr) {
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) return;
	int n = (int)L.size(), bytes = n * 8;
	auto w32 = [&](int v) { fwrite(&v, 4, 1, f); };
	auto w16 = [&](short v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(3); w16(2);
	w32((int)sr); w32((int)sr * 8); w16(8); w16(32); fwrite("data", 1, 4, f); w32(bytes);
	// through the module's own output stage (its gain and its soft clip), so a
	// render plays at the level the module would give it
	auto stage = [](float x) {
		x *= 0.5f;
		float a = std::fabs(x);
		if (a > 6.f) x = std::copysign(6.f + 4.f * (1.f - 4.f / (4.f + a - 6.f)), x);
		return x / 10.f;
	};
	for (int i = 0; i < n; i++) { float a = stage(L[i]), b = stage(R[i]); fwrite(&a, 4, 1, f); fwrite(&b, 4, 1, f); }
	fclose(f);
}

struct Run {
	Engine e;
	std::vector<float> L, R;
	void go(float secs) {
		int n = (int)(secs * e.sr);
		for (int i = 0; i < n; i++) { float l, r; e.next(l, r); L.push_back(l); R.push_back(r); }
	}
	void only(std::initializer_list<int> s) { for (int i = 0; i < NSTOP; i++) e.stop[i] = false; for (int x : s) e.stop[x] = true; }
};

static float rms(const std::vector<float>& x, size_t a, size_t b) {
	double s = 0; for (size_t i = a; i < b && i < x.size(); i++) s += x[i] * x[i];
	return (float)std::sqrt(s / std::max<size_t>(1, b - a));
}
// strongest frequency by a DFT scan around an expected one
static float peakNear(const std::vector<float>& x, size_t a, size_t n, float sr, float f0, float span) {
	float best = 0, bf = 0;
	for (float f = f0 / span; f < f0 * span; f *= 1.0005f) {
		double c = 0, s = 0;
		for (size_t i = 0; i < n; i++) { double w = 2 * M_PI * f * i / sr; c += x[a + i] * cos(w); s += x[a + i] * sin(w); }
		float m = (float)(c * c + s * s);
		if (m > best) { best = m; bf = f; }
	}
	return bf;
}

int main(int argc, char** argv) {
	std::string dir = argc > 1 ? argv[1] : "";
	const float SR = 48000.f;

	printf("== every stop alone, A3 held ==\n");
	for (int s = 0; s < NSTOP; s++) {
		Run r; r.only({s});
		int div = STOPS[s].div, key = 57;    // A3, 220 Hz at 8'
		r.e.setKey(div, key, true); r.go(1.5f);
		r.e.setKey(div, key, false); r.go(0.5f);
		float ratio = s == GT_MIXTURE ? mixRatio(key, 0) : STOPS[s].ratio;
		float f0 = 220.f * ratio * std::pow(2.f, STOPS[s].cents / 1200.f);
		float got = peakNear(r.L, (size_t)(0.8f * SR), 9600, SR, f0, 1.03f);
		float lvl = rms(r.L, (size_t)(0.8f * SR), (size_t)(1.4f * SR));
		bool pitched = std::fabs(1200.f * std::log2(got / f0)) < 3.f;
		check(pitched && lvl > 0.02f,
			std::string(STOPS[s].name) + " " + STOPS[s].foot + ": " + std::to_string((int)std::lround(got)) +
			" Hz (expected " + std::to_string((int)std::lround(f0)) + "), " + std::to_string(lvl).substr(0, 5) + " V rms");
		if (!dir.empty()) {
			std::string nm = std::string(DIV_NAME[div]) + "-" + STOPS[s].name + "-" + STOPS[s].foot;
			for (char& c : nm) if (c == '\'' || c == ' ') c = '_';
			wav(dir + "/stop-" + std::to_string(s) + "-" + nm + ".wav", r.L, r.R, SR);
		}
	}

	printf("== the pedal ==\n");
	{
		Run r; r.only({PD_BOURDON32});
		r.e.setKey(PEDAL, 36, true); r.go(2.f);
		float got = peakNear(r.L, (size_t)(1.f * SR), 48000, SR, 16.35f, 1.03f);
		check(std::fabs(1200.f * std::log2(got / 16.35f)) < 3.f, "Contra Bourdon 32' on the bottom C sounds 16.35 Hz (" + std::to_string(got) + ")");
	}

	printf("== couplers play the same pipe ==\n");
	{
		Run a; a.only({SW_GEDACKT8});
		a.e.setKey(SWELL, 60, true); a.go(1.f);
		Run b; b.only({SW_GEDACKT8}); b.e.coupler[SW_GT] = true;
		b.e.setKey(SWELL, 60, true); b.e.setKey(GREAT, 60, true); b.go(1.f);
		float la = rms(a.L, 24000, 48000), lb = rms(b.L, 24000, 48000);
		check(std::fabs(20.f * std::log10(lb / la)) < 0.1f && b.e.nActive == 1,
		      "Swell C4 held on both keyboards through Swell to Great is one pipe at one level");
		Run c; c.only({SW_GEDACKT8}); c.e.coupler[SW_GT] = true;
		c.e.setKey(GREAT, 60, true); c.go(1.f);
		check(rms(c.L, 24000, 48000) > 0.5f * la, "Swell to Great: the Great keyboard plays the Swell's Gedackt");
		Run d; d.only({SW_GEDACKT8});
		d.e.setKey(GREAT, 60, true); d.go(1.f);
		check(rms(d.L, 24000, 48000) < 1e-4f, "without the coupler the Great keyboard does not");
	}

	printf("== the céleste starts at tenor C ==\n");
	{
		Run a; a.only({SW_CELESTE8});
		a.e.setKey(SWELL, 47, true); a.go(0.5f);
		check(a.e.nActive == 0, "B2 on the Céleste alone is silent");
		Run b; b.only({SW_CELESTE8, SW_GAMBA8});
		b.e.setKey(SWELL, 69, true); b.go(3.f);
		// the two beat: the envelope of the sum rises and falls about 1.3 times a second
		std::vector<float> env;
		for (size_t i = 48000; i + 2400 < b.L.size(); i += 2400) env.push_back(rms(b.L, i, i + 2400));
		float mn = 1e9f, mx = 0;
		for (float v : env) { mn = std::min(mn, v); mx = std::max(mx, v); }
		check(mx > 1.5f * mn, "Gamba and Céleste at A4 beat (level " + std::to_string(mn).substr(0, 5) + " to " + std::to_string(mx).substr(0, 5) + ")");
	}

	printf("== speech ==\n");
	{
		float t[2];
		int keys[2] = {36, 84};
		for (int k = 0; k < 2; k++) {
			Run r; r.only({GT_PRINCIPAL8});
			r.e.setKey(GREAT, keys[k], true); r.go(1.5f);
			float fin = rms(r.L, 48000, 72000);
			size_t i = 0;
			while (i + 240 < r.L.size() && rms(r.L, i, i + 240) < 0.7f * fin) i += 48;
			t[k] = i / SR * 1000.f;
		}
		check(t[0] > 2.f * t[1], "Principal 8': C2 takes " + std::to_string((int)t[0]) + " ms to speak, C6 " + std::to_string((int)t[1]) + " ms");
	}

	printf("== release ==\n");
	{
		float t20[2];
		int keys[2] = {60, 36};
		for (int k = 0; k < 2; k++) {
			Run r; r.only({GT_PRINCIPAL8});
			r.e.setKey(GREAT, keys[k], true); r.go(1.f);
			float held = rms(r.L, r.L.size() - 4800, r.L.size());
			size_t off = r.L.size();
			r.e.setKey(GREAT, keys[k], false); r.go(1.5f);
			size_t i = off;
			while (i + 480 < r.L.size() && rms(r.L, i, i + 480) > 0.1f * held) i += 48;
			t20[k] = (i - off) / SR * 1000.f;
		}
		check(t20[0] > 80.f && t20[0] < 250.f && t20[1] > 2.5f * t20[0],
		      "Principal 8' released: 20 dB down after " + std::to_string((int)t20[0]) + " ms at C4, " + std::to_string((int)t20[1]) + " ms at C2");
	}

	{
		// a reed stops with its tongue: the Posaune 16' on the bottom pedal C
		// must be gone well before the Subbass 16' on the same key
		float t20[2];
		int st[2] = {PD_POSAUNE16, PD_SUBBASS16};
		for (int k = 0; k < 2; k++) {
			Run r; r.only({st[k]});
			r.e.setKey(PEDAL, 36, true); r.go(1.5f);
			float held = rms(r.L, r.L.size() - 9600, r.L.size()) + rms(r.R, r.R.size() - 9600, r.R.size());
			size_t off = r.L.size();
			r.e.setKey(PEDAL, 36, false); r.go(2.f);
			size_t i = off;
			while (i + 960 < r.L.size() && rms(r.L, i, i + 960) + rms(r.R, i, i + 960) > 0.1f * held) i += 48;
			t20[k] = (i - off) / SR * 1000.f;
		}
		check(t20[0] < 200.f && t20[1] > 2.f * t20[0], "Pedal C released: the Posaune 16' 20 dB down after " + std::to_string((int)t20[0]) +
		      " ms, the Subbass 16' after " + std::to_string((int)t20[1]) + " ms");
	}

	printf("== wind ==\n");
	{
		float sag[2];
		for (int w = 0; w < 2; w++) {
			Run r; r.only({GT_BOURDON16, GT_PRINCIPAL8, GT_OCTAVE4, GT_FIFTEENTH2, GT_MIXTURE, GT_TRUMPET8});
			r.e.wind = w ? 1.f : 0.f;
			const int ch[8] = {48, 52, 55, 60, 64, 67, 72, 76};
			for (int k : ch) r.e.setKey(GREAT, k, true);
			r.go(1.f);
			sag[w] = r.e.p[GREAT];
		}
		check(sag[0] > 0.999f && sag[1] < 0.97f && sag[1] > 0.85f,
		      "a full Great chord: regulated wind holds (" + std::to_string(sag[0]).substr(0, 5) + "), WIND 1 sags to " + std::to_string(sag[1]).substr(0, 5));
	}

	printf("== pipes that are not quite steady ==\n");
	{
		// averaged over five notes, since one note's three seconds of a 3 Hz
		// wavering is a noisy measurement
		float sd[2] = {0.f, 0.f};
		const int KEYS[5] = {48, 55, 60, 67, 72};
		for (int k = 0; k < 2; k++) {
			for (int key : KEYS) {
				Run r; r.only({GT_PRINCIPAL8}); r.e.irreg = k ? 1.f : 0.f;
				r.e.setKey(GREAT, key, true); r.go(4.f);
				// windows of exactly 25 periods, so a steady tone reads steady
				const size_t W = (size_t)std::lround(25.0 * SR / keyHz(key));
				std::vector<float> lv;
				for (size_t i = 48000; i + W < r.L.size(); i += W) lv.push_back(20.f * std::log10(rms(r.L, i, i + W)));
				float m = 0; for (float v : lv) m += v; m /= lv.size();
				float q = 0; for (float v : lv) q += (v - m) * (v - m);
				sd[k] += std::sqrt(q / lv.size()) / 5.f;
			}
		}
		check(sd[0] < 0.03f && sd[1] > 3.f * sd[0] && sd[1] < 1.f, "a held Principal 8' wavers " + std::to_string(sd[1]).substr(0, 4) +
		      " dB (in 25-period windows, five notes) at Natural, " + std::to_string(sd[0]).substr(0, 5) + " dB at Steady");
	}

	printf("== a chord's pipes do not start together ==\n");
	{
		float spread[2];
		for (int k = 0; k < 2; k++) {
			Run r; r.e.irreg = k ? 1.f : 0.f;
			r.only({GT_PRINCIPAL8, GT_OCTAVE4, SW_GEDACKT8, PD_SUBBASS16});
			r.e.coupler[SW_GT] = true; r.e.coupler[GT_PD] = true;
			for (int key : {48, 52, 55, 60}) r.e.setKey(GREAT, key, true);
			r.e.setKey(PEDAL, 36, true);
			r.go(0.01f);
			float lo = 1e9f, hi = 0.f;
			for (int i = 0; i < r.e.nActive; i++) { float d = r.e.pipes[r.e.active[i]].delay; lo = std::min(lo, d); hi = std::max(hi, d); }
			spread[k] = (hi - lo) * 1000.f;
		}
		check(spread[0] == 0.f && spread[1] > 5.f && spread[1] < 15.f, "a coupled chord's pipes start over " +
		      std::to_string(spread[1]).substr(0, 4) + " ms at Natural, together at Steady");
	}

	printf("== full organ ==\n");
	{
		Run r;
		for (int s = 0; s < NSTOP; s++) r.e.stop[s] = true;
		for (int c = 0; c < NCOUP; c++) r.e.coupler[c] = true;
		r.e.tremulant = false;
		const int gt[4] = {60, 64, 67, 72}, sw[4] = {55, 59, 62, 67};
		for (int k : gt) r.e.setKey(GREAT, k, true);
		for (int k : sw) r.e.setKey(SWELL, k, true);
		r.e.setKey(PEDAL, 36, true);
		r.go(0.5f);
		int n = (int)(4.f * SR);
		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < n; i++) { float l, rr; r.e.next(l, rr); r.L.push_back(l); r.R.push_back(rr); }
		double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		float pk = 0; for (float v : r.L) pk = std::max(pk, std::fabs(v));
		printf("    %d pipes, %.1f%% of one core, %.2f V rms, peak %.2f V\n", r.e.nActive, 100.0 * sec / 4.0,
		       rms(r.L, r.L.size() - 96000, r.L.size()), pk);
		check(sec / 4.0 < 0.10, "full organ, eight notes and the pedal with every coupler, under 10% of a core");
		if (!dir.empty()) {
			for (int k : gt) r.e.setKey(GREAT, k, false);
			for (int k : sw) r.e.setKey(SWELL, k, false);
			r.e.setKey(PEDAL, 36, false);
			r.go(1.f);
			wav(dir + "/full-organ.wav", r.L, r.R, SR);
		}
	}

	const char* PIECE[3] = {"piece-steady", "piece", "piece-old-organ"};
	for (int v = 0; v < 3 && !dir.empty(); v++) {
		printf("== a short piece (%s) ==\n", PIECE[v]);
		Run r;
		r.e.irreg = (float)v;
		r.only({SW_GEDACKT8, SW_GAMBA8, SW_CELESTE8, PD_SUBBASS16});
		r.e.tremulant = true; r.e.coupler[SW_PD] = false;
		const int prog[4][4] = {{48, 55, 60, 64}, {45, 52, 57, 60}, {41, 53, 57, 60}, {43, 50, 55, 59}};
		const int bass[4] = {48, 45, 41, 43};
		for (int bar = 0; bar < 4; bar++) {
			for (int k : prog[bar]) r.e.setKey(SWELL, k, true);
			r.e.setKey(PEDAL, bass[bar] - 12, true);
			r.go(1.9f);
			for (int k : prog[bar]) r.e.setKey(SWELL, k, false);
			r.e.setKey(PEDAL, bass[bar] - 12, false);
			r.go(0.1f);
		}
		r.only({GT_PRINCIPAL8, GT_OCTAVE4, GT_FIFTEENTH2, GT_MIXTURE, GT_BOURDON16, PD_SUBBASS16, PD_OCTAVE8, PD_POSAUNE16});
		r.e.tremulant = false; r.e.wind = 0.6f;
		for (int bar = 0; bar < 4; bar++) {
			for (int k : prog[bar]) r.e.setKey(GREAT, k + 12, true);
			r.e.setKey(PEDAL, bass[bar] - 12, true);
			r.go(1.9f);
			for (int k : prog[bar]) r.e.setKey(GREAT, k + 12, false);
			r.e.setKey(PEDAL, bass[bar] - 12, false);
			r.go(0.1f);
		}
		r.go(1.f);
		wav(dir + "/" + PIECE[v] + ".wav", r.L, r.R, SR);
	}
	if (!dir.empty()) {
		printf("== the progression on the Full organ preset's stops ==\n");
		Run r;
		r.only({GT_BOURDON16, GT_PRINCIPAL8, GT_ROHR8, GT_OCTAVE4, GT_FIFTEENTH2, GT_MIXTURE, GT_TRUMPET8,
		        SW_GEDACKT8, SW_GAMBA8, SW_PRINCIPAL4, SW_OBOE8, SW_TROMPETTE8,
		        PD_BOURDON32, PD_SUBBASS16, PD_OCTAVE8, PD_POSAUNE16});
		for (int c = 0; c < NCOUP; c++) r.e.coupler[c] = true;
		r.e.wind = 0.5f; r.e.master = std::pow(10.f, -4.f / 20.f);
		const int prog[4][4] = {{48, 55, 60, 64}, {45, 52, 57, 60}, {41, 53, 57, 60}, {43, 50, 55, 59}};
		const int bass[4] = {48, 45, 41, 43};
		for (int bar = 0; bar < 4; bar++) {
			for (int k : prog[bar]) r.e.setKey(GREAT, k + 12, true);
			r.e.setKey(PEDAL, bass[bar] - 12, true);
			r.go(1.9f);
			for (int k : prog[bar]) r.e.setKey(GREAT, k + 12, false);
			r.e.setKey(PEDAL, bass[bar] - 12, false);
			r.go(0.1f);
		}
		r.go(1.f);
		wav(dir + "/full-organ-progression.wav", r.L, r.R, SR);
	}
	printf(fails ? "FAILED (%d)\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}

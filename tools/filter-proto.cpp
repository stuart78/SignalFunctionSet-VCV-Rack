// Prototype for Pole, the stereo pole-pair filter (docs/pole-design.md).
// Free of Rack. Renders listening WAVs so the ideas can be judged before a
// module exists:
//
//   clang++ -std=c++11 -O2 tools/filter-proto.cpp -o /tmp/filter-proto
//   /tmp/filter-proto <outdir>
//
// Every file is normalised to -1 dBFS and the gain applied is printed, because
// level is a design question in its own right (series bandpasses spread apart
// lose a great deal).
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>

static const float SR = 48000.f;
static const float PI = 3.14159265358979f;

// --- one pole pair: a TPT state-variable filter, stable under modulation ---
// mode 0 = lowpass, 1 = bandpass, 2 = highpass, 3 = notch, continuous between.
struct Pair {
	float ic1 = 0.f, ic2 = 0.f;
	float process(float x, float f, float q, float mode) {
		f = std::min(std::max(f, 10.f), SR * 0.45f);
		float g = std::tan(PI * f / SR);
		float k = 1.f / q;
		float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
		float v3 = x - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		float lp = v2, bp = v1, hp = x - k * v1 - v2, nt = x - k * v1;
		float out[4] = {lp, bp, hp, nt};
		mode = std::min(std::max(mode, 0.f), 3.f);
		int i = std::min((int)mode, 2);
		float t = mode - i;
		return out[i] + (out[i + 1] - out[i]) * t;
	}
};

// --- four pairs, run both ways at once so topology can crossfade ---
struct Settings {
	float cutoff = 800.f;        // Hz, centre of the constellation
	float spread = 0.f;          // octaves between neighbouring pairs
	float pos[4] = {-1.5f, -0.5f, 0.5f, 1.5f};  // positions in units of spread
	float shiftHz = 0.f;         // linear shift added after the ratio layout
	float q = 4.f;
	float mode = 0.f;
	float topo = 0.f;            // 0 = series, 1 = parallel
	float drive = 0.f;           // saturation before each series stage
	int order[4] = {0, 1, 2, 3};
	bool autoGain = false;       // divide by the response's own peak
	float lag[4] = {0, 0, 0, 0}; // seconds, per pair, on log frequency
};

struct Filter4 {
	Pair ser[4], par[4];
	float lf[4] = {0, 0, 0, 0};  // lagged log2 frequency per pair
	bool init = false;
	float freqOf(const Settings& s, int i) {
		float target = std::log2(s.cutoff) + s.spread * s.pos[i];
		if (!init) lf[i] = target;
		if (s.lag[i] <= 0.f) lf[i] = target;
		else lf[i] += (target - lf[i]) * (1.f - std::exp(-1.f / (s.lag[i] * SR)));
		return std::exp2(lf[i]) + s.shiftHz;
	}
	float process(float x, const Settings& s) {
		float f[4];
		for (int i = 0; i < 4; i++) f[i] = freqOf(s, i);
		init = true;
		// In series, resonances that coincide MULTIPLY: four pairs of Q 8 at
		// one frequency peak at 8^4. So each pair shares its Q with the pairs
		// it overlaps (weighted by distance in units of its bandwidth), and the
		// set of them makes one peak of the Q asked for. Pulled apart, each
		// pair gets its whole Q back.
		float bw = 1.f / (s.q * std::log(2.f));     // bandwidth, octaves (approx.)
		float qs[4];
		for (int i = 0; i < 4; i++) {
			float cnt = 0.f;
			for (int j = 0; j < 4; j++) {
				float d = (std::log2(f[i]) - std::log2(f[j])) / bw;
				cnt += std::exp(-d * d);
			}
			qs[i] = std::pow(s.q, 1.f / cnt);
		}
		float y = x;
		for (int n = 0; n < 4; n++) {
			int i = s.order[n];
			if (s.drive > 0.f) y = std::tanh(y * s.drive) / s.drive;
			y = ser[i].process(y, f[i], std::max(qs[i], 0.5f), s.mode);
		}
		float p = 0.f;
		for (int i = 0; i < 4; i++) p += par[i].process(x, f[i], s.q, s.mode);
		p *= 0.25f;
		float out = y + (p - y) * s.topo;
		if (s.autoGain) {
			if (ctl-- <= 0) { ctl = 32; target = 1.f / peakOf(s, f, qs); }
			gain += (target - gain) * 0.002f;
			out *= gain;
		}
		return out;
	}

	// The analytic magnitude response on a log grid: what the screen would
	// draw, and its maximum is the level correction.
	int ctl = 0; float gain = 1.f, target = 1.f;
	struct C { float r, i; };
	static C mul(C a, C b) { return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r}; }
	static C div(C a, C b) { float d = b.r * b.r + b.i * b.i; return {(a.r * b.r + a.i * b.i) / d, (a.i * b.r - a.r * b.i) / d}; }
	static C pairH(float w, float fc, float q, float mode) {
		C s = {0.f, w / fc}, den = {1.f - (w / fc) * (w / fc), (w / fc) / q};
		C lp = div({1, 0}, den), bp = div(s, den), hp = div(mul(s, s), den);
		C nt = {1.f - bp.r / q, -bp.i / q};
		C o[4] = {lp, bp, hp, nt};
		mode = std::min(std::max(mode, 0.f), 3.f);
		int k = std::min((int)mode, 2); float t = mode - k;
		return {o[k].r + (o[k + 1].r - o[k].r) * t, o[k].i + (o[k + 1].i - o[k].i) * t};
	}
	float peakOf(const Settings& s, const float* f, const float* qs) {
		float pk = 1e-6f;
		for (int g = 0; g < 96; g++) {
			float w = 20.f * std::pow(1000.f, g / 95.f);
			C ser = {1, 0}, par = {0, 0};
			for (int i = 0; i < 4; i++) {
				ser = mul(ser, pairH(w, f[i], std::max(qs[i], 0.5f), s.mode));
				C h = pairH(w, f[i], s.q, s.mode); par.r += 0.25f * h.r; par.i += 0.25f * h.i;
			}
			C h = {ser.r + (par.r - ser.r) * s.topo, ser.i + (par.i - ser.i) * s.topo};
			pk = std::max(pk, std::sqrt(h.r * h.r + h.i * h.i));
		}
		return pk;
	}
};

// --- source: a polyBLEP sawtooth chord, or noise ---
struct Saw {
	float ph = 0.f, f;
	explicit Saw(float f_) : f(f_) {}
	static float blep(float t, float dt) {
		if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
		if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
		return 0.f;
	}
	float next() {
		float dt = f / SR;
		float y = 2.f * ph - 1.f - blep(ph, dt);
		ph += dt; if (ph >= 1.f) ph -= 1.f;
		return y;
	}
};
struct Chord {
	std::vector<Saw> s;
	Chord() { for (float hz : {65.41f, 98.0f, 130.81f, 164.81f}) s.emplace_back(hz); }
	float next() { float y = 0; for (auto& v : s) y += v.next(); return 0.25f * y; }
};
static uint32_t rng = 12345;
static float noise() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 8388608.f - 1.f; }

// --- WAV out ---
static void writeWav(const std::string& path, std::vector<float>& L, std::vector<float>& R) {
	float pk = 1e-9f;
	for (size_t i = 0; i < L.size(); i++) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
	float g = 0.891f / pk;
	FILE* fp = std::fopen(path.c_str(), "wb");
	if (!fp) { std::printf("cannot write %s\n", path.c_str()); return; }
	uint32_t n = (uint32_t)L.size(), data = n * 4, sr = (uint32_t)SR, br = sr * 4;
	uint16_t ch = 2, bits = 16, fmt = 1, align = 4; uint32_t fmtLen = 16, riff = 36 + data;
	std::fwrite("RIFF", 1, 4, fp); std::fwrite(&riff, 4, 1, fp); std::fwrite("WAVEfmt ", 1, 8, fp);
	std::fwrite(&fmtLen, 4, 1, fp); std::fwrite(&fmt, 2, 1, fp); std::fwrite(&ch, 2, 1, fp);
	std::fwrite(&sr, 4, 1, fp); std::fwrite(&br, 4, 1, fp); std::fwrite(&align, 2, 1, fp);
	std::fwrite(&bits, 2, 1, fp); std::fwrite("data", 1, 4, fp); std::fwrite(&data, 4, 1, fp);
	for (uint32_t i = 0; i < n; i++) {
		int16_t a = (int16_t)std::lrint(std::max(-1.f, std::min(1.f, L[i] * g)) * 32767.f);
		int16_t b = (int16_t)std::lrint(std::max(-1.f, std::min(1.f, R[i] * g)) * 32767.f);
		std::fwrite(&a, 2, 1, fp); std::fwrite(&b, 2, 1, fp);
	}
	std::fclose(fp);
	std::printf("  %-28s gain %+6.1f dB\n", path.substr(path.rfind('/') + 1).c_str(), 20.f * std::log10(g));
}

// Render: `set(t, sL, sR)` fills both channels' settings at time t (seconds).
static void render(const std::string& dir, const char* name, float secs, bool useNoise,
                   std::function<void(float, Settings&, Settings&)> set) {
	Chord c; Filter4 fl, fr; Settings sL, sR;
	size_t n = (size_t)(secs * SR);
	std::vector<float> L(n), R(n);
	for (size_t i = 0; i < n; i++) {
		float t = i / SR;
		set(t, sL, sR);
		float x = useNoise ? 0.5f * noise() : c.next();
		L[i] = fl.process(x, sL);
		R[i] = fr.process(x, sR);
	}
	writeWav(dir + "/" + name, L, R);
	// Level swing across the render: RMS in half-second blocks.
	float lo = 1e9f, hi = -1e9f;
	size_t blk = (size_t)(0.5f * SR);
	for (size_t b = 0; b + blk <= n; b += blk) {
		double e = 0; for (size_t i = b; i < b + blk; i++) e += 0.5 * (L[i] * L[i] + R[i] * R[i]);
		float db = 10.f * std::log10((float)(e / blk) + 1e-20f);
		lo = std::min(lo, db); hi = std::max(hi, db);
	}
	std::printf("  %-28s level swing %5.1f dB\n", "", hi - lo);
}

static float expSweep(float a, float b, float u) { return a * std::pow(b / a, u); }
static float tri(float t, float period) { float u = std::fmod(t / period, 1.f); return u < 0.5f ? 2 * u : 2 - 2 * u; }

int main(int argc, char** argv) {
	std::string dir = argc > 1 ? argv[1] : ".";
	std::printf("rendering to %s\n", dir.c_str());

	// 1. Gathered: four lowpass pairs at one frequency, series = one 8-pole filter.
	render(dir, "01-gathered-sweep.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = expSweep(80, 6000, tri(t, 8)); l.q = 3; l.mode = 0; l.spread = 0;
		r = l; });

	// 2. SPREAD from 0 to 1.5 octaves between neighbours, series.
	render(dir, "02a-spread-series-lp.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = 900; l.q = 8; l.mode = 0; l.spread = 1.5f * tri(t, 8); r = l; });
	render(dir, "02b-spread-series-bp.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = 900; l.q = 8; l.mode = 1; l.spread = 1.5f * tri(t, 8); r = l; });

	//    c, d: the same two with the level corrected from the response's peak.
	render(dir, "02c-spread-series-lp-auto.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = 900; l.q = 8; l.mode = 0; l.spread = 1.5f * tri(t, 8); l.autoGain = true; r = l; });
	render(dir, "02d-spread-series-bp-auto.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = 900; l.q = 8; l.mode = 1; l.spread = 1.5f * tri(t, 8); l.autoGain = true; r = l; });

	// 3. The same SPREAD sweep in parallel: the additive multifilter.
	render(dir, "03-spread-parallel-bp.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = 900; l.q = 8; l.mode = 1; l.topo = 1; l.spread = 1.5f * tri(t, 8); r = l; });

	// 4. Topology crossfade, series to parallel, at a fixed spread.
	render(dir, "04-series-to-parallel.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = 900; l.q = 8; l.mode = 1; l.spread = 0.8f; l.topo = tri(t, 8); r = l; });

	// 5. LAG. An envelope throws the cutoff up every half second. Without lag
	//    the pairs move together; with staggered lag a fast move fans them out.
	auto env = [](float t) { float u = std::fmod(t, 0.5f); return expSweep(150, 5000, std::exp(-u / 0.12f)); };
	render(dir, "05a-envelope-no-lag.wav", 6, false, [env](float t, Settings& l, Settings& r) {
		l.cutoff = env(t); l.q = 6; l.mode = 0; r = l; });
	render(dir, "05b-envelope-lag.wav", 6, false, [env](float t, Settings& l, Settings& r) {
		l.cutoff = env(t); l.q = 6; l.mode = 0;
		l.lag[0] = 0.f; l.lag[1] = 0.03f; l.lag[2] = 0.07f; l.lag[3] = 0.15f; r = l; });

	// 6. Stereo, related.
	//    a: R's constellation an octave ratio above L's (+0.3 oct), parallel bandpass.
	render(dir, "06a-stereo-offset-oct.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = expSweep(300, 2000, tri(t, 8)); l.q = 10; l.mode = 1; l.topo = 1; l.spread = 0.7f;
		r = l; r.cutoff = l.cutoff * std::exp2(0.3f); });
	//    b: R shifted by a fixed number of Hz instead: same move, ratios not kept.
	render(dir, "06b-stereo-offset-hz.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = expSweep(300, 2000, tri(t, 8)); l.q = 10; l.mode = 1; l.topo = 1; l.spread = 0.7f;
		r = l; r.shiftHz = 120.f; });
	//    c: mirrored. R moves opposite to L in log frequency round 1 kHz, and a
	//       mirrored lowpass is a highpass, so the mode is mirrored too.
	render(dir, "06c-stereo-mirror.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = expSweep(150, 6600, tri(t, 4)); l.q = 4; l.mode = 0; l.spread = 0.3f;
		r = l; r.cutoff = 1000.f * 1000.f / l.cutoff; r.mode = 2.f - l.mode; });

	// 7. Shift by ratio vs by Hz, pairs on a harmonic ladder (200 400 600 800).
	auto ladder = [](Settings& s) { s.cutoff = 200; s.spread = 1;
		s.pos[0] = 0; s.pos[1] = 1; s.pos[2] = std::log2(3.f); s.pos[3] = 2; s.q = 20; s.mode = 1; s.topo = 1; };
	render(dir, "07a-shift-octaves.wav", 8, true, [ladder](float t, Settings& l, Settings& r) {
		ladder(l); l.cutoff = 200 * std::exp2(1.2f * tri(t, 8)); r = l; });
	render(dir, "07b-shift-hz.wav", 8, true, [ladder](float t, Settings& l, Settings& r) {
		ladder(l); l.shiftHz = 260 * tri(t, 8); r = l; });

	// 8. Order. Linear stages commute, so order only exists with drive between
	//    them: L runs low pair first, R runs high pair first.
	render(dir, "08-order-with-drive.wav", 8, false, [](float t, Settings& l, Settings& r) {
		l.cutoff = 700; l.q = 12; l.mode = 1; l.spread = 0.9f; l.drive = 1.f + 7.f * tri(t, 8);
		r = l; r.order[0] = 3; r.order[1] = 2; r.order[2] = 1; r.order[3] = 0; });

	// Check: with no drive, order makes no difference (to rounding).
	{
		Chord c; Filter4 a, b; Settings sa, sb;
		sa.cutoff = 700; sa.q = 12; sa.mode = 1; sa.spread = 0.9f; sb = sa;
		sb.order[0] = 3; sb.order[1] = 2; sb.order[2] = 1; sb.order[3] = 0;
		double e = 0, d = 0;
		for (int i = 0; i < 48000; i++) { float x = c.next(), ya = a.process(x, sa), yb = b.process(x, sb);
			e += ya * ya; d += (ya - yb) * (ya - yb); }
		std::printf("order without drive: difference %.1f dB below signal\n", 10 * std::log10(e / std::max(d, 1e-30)));
	}
	return 0;
}

// =============================================================================
// Strata: a wavetable voice you travel through.
//
// Plays what WaveStack builds. A table is a VOLUME of single-cycle frames,
// X the column, Y the row, Z the layer, loaded from a WaveStack export (a WAV
// whose `wt3d` chunk records the grid) or from a folder of tables stacked as
// layers. The sound is one point in the volume, blended from the eight frames
// around it, and everything here is about where that point is and how it
// moves: X/Y/Z knobs and CVs place it, a clocked list of waypoints moves it
// with a glide whose travel time follows distance, and four shape macros bend
// the frame it reads. Design: docs/strata-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "dr_wav.h"
#include <osdialog.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>

static const int ST_N = 2048;                    // the engine's one frame size
static const int ST_LEVELS = 11;                 // mip levels: level k keeps harmonics up to 1024 >> k
// Each level holds at least 8 samples per cycle of its top harmonic. At 4, as
// first written, linear interpolation threw images 19 dB under that harmonic
// which folded back (-46 dB of alias at 440 Hz, -58 now).
static const int ST_LEN[ST_LEVELS] = {2048, 2048, 2048, 1024, 512, 256, 128, 128, 128, 128, 128};
static const int ST_MAXDIM = 16;                 // per axis
static const int ST_MAXFRAMES = 1024;            // in total (8 x 8 x 16), about 35 MB with the mips
static const int ST_MAXSTEPS = 16;
static const int ST_POLY = 16;
// The volume as a cube in world units, shared by the screen and the listeners:
// columns span x -1..1, rows z +1 (row 1, at the back) to -1, layers y -1
// (layer 1, at the bottom) to +1, so a layer added to the stack lands on top.
static const float ST_CUBE_D = 2.f, ST_CUBE_H = 2.f;
static inline float stWorldX(float px, int cols) { return -1.f + (px + 0.5f) * 2.f / (float)cols; }
static inline float stWorldZ(float py, int rows) { return rows > 1 ? (0.5f - py / (float)(rows - 1)) * ST_CUBE_D : 0.f; }
static inline float stWorldY(float pz, int layers) { return layers > 1 ? (pz / (float)(layers - 1) - 0.5f) * ST_CUBE_H : 0.f; }

// Band-limited corrections, LINEAR phase: a windowed-sinc BLEP for steps and
// its integral, a BLAMP, for changes of slope. Sync on a smooth frame and the
// WARP knee are slope changes, not steps, and a minBLEP cannot correct them:
// Rack's minimum-phase step lags the ideal one, so its integral ends 3.9
// samples of slope short of zero and truncating it leaves a step behind. A
// symmetric kernel's residuals return exactly to zero at both ends. The price
// is ST_BLZ samples of delay (0.33 ms at 48 kHz) on the voice.
static const int ST_BLZ = 16, ST_BLO = 32, ST_BLL = 64;   // zero crossings, oversampling, ring
struct StrataBlTables {
	float step[2 * ST_BLZ * ST_BLO + 1], ramp[2 * ST_BLZ * ST_BLO + 1];
	StrataBlTables() {
		const int n = 2 * ST_BLZ * ST_BLO + 1;
		const double fc = 0.9, dt = 1.0 / ST_BLO;             // cutoff as a fraction of Nyquist
		std::vector<double> k(n), b(n);
		double sum = 0.0;
		for (int i = 0; i < n; i++) {
			double u = (i - ST_BLZ * ST_BLO) * dt, x = M_PI * fc * u;
			double w = 2.0 * M_PI * i / (n - 1);
			double win = 0.42 - 0.5 * std::cos(w) + 0.08 * std::cos(2.0 * w);
			k[i] = (u == 0.0 ? 1.0 : std::sin(x) / x) * win;
			sum += k[i];
		}
		double acc = 0.0, racc = 0.0;
		for (int i = 0; i < n; i++) {
			acc += k[i] / sum;
			b[i] = acc - 0.5 * k[i] / sum;                    // the step, centred on its sample
			double u = (i - ST_BLZ * ST_BLO) * dt;
			step[i] = (float)(b[i] - (u >= 0.0 ? 1.0 : 0.0));      // a sample AT the step already has its new value
			if (i > 0) racc += 0.5 * (b[i] + b[i - 1]) * dt;
			ramp[i] = (float)(racc - (u > 0.0 ? u : 0.0));
		}
		// the ramp residual is odd about the centre and so ends at zero; take
		// out the integration's rounding so it ends there exactly
		float endR = ramp[n - 1];
		for (int i = 0; i < n; i++) ramp[i] -= endR * (float)i / (float)(n - 1);
	}
};
static const StrataBlTables& stBlTables() { static StrataBlTables t; return t; }

// FOLD: a triangle folder, the identity between -1 and 1, period 4 ...
static inline float stFold(float x) {
	float q = (x + 1.f) * 0.25f;
	return 4.f * std::fabs(q - std::floor(q + 0.5f)) - 1.f;
}
// FOLD's drive, exponential to 12x: at the old 4x the top of the knob folded
// each half-cycle about once and read as "not much more than the middle"
static inline float stFoldGain(float fold) { return std::pow(12.f, clamp(fold, 0.f, 1.f)); }
// ... and its antiderivative, which is periodic too (the triangle averages to
// zero), so it stays bounded however hard the folder is driven
static inline double stFoldF(double x) {
	double u = x + 1.0;
	u -= 4.0 * std::floor(u * 0.25);
	return u < 2.0 ? 0.5 * u * u - u : 3.0 * u - 0.5 * u * u - 4.0;
}

// The folder runs at twice the rate, through a halfband pair: a fold puts
// corners into the wave, and a bright frame's edge crosses more than a whole
// fold period in two samples, so its alias is genuinely near Nyquist and
// averaging alone (the antiderivative) bought only 7 dB of it. ST_HB taps a
// side at the odd offsets (the even ones of a halfband are zero), Kaiser.
static const int ST_HB = 12;
struct StrataHalfband {
	float c[ST_HB];
	StrataHalfband() {
		const double beta = 8.0, n = 2.0 * ST_HB;
		double sum = 0.0;
		auto i0 = [](double x) { double s = 1.0, t = 1.0; for (int k = 1; k < 30; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
		for (int i = 0; i < ST_HB; i++) {
			double k = 2 * i + 1, r = k / n;
			double win = i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta);
			c[i] = (float)(std::sin(M_PI * k / 2.0) / (M_PI * k / 2.0) * win);
			sum += c[i];
		}
		for (int i = 0; i < ST_HB; i++) c[i] = (float)(c[i] * 0.25 / sum);   // the odd taps carry half the gain
	}
};
static const StrataHalfband& stHalfband() { static StrataHalfband h; return h; }
// one x2 stage up: a sample in, two out (the even phase is the input, delayed)
struct StrataUp2 {
	float h[4 * ST_HB] = {};                          // history, written twice so it reads contiguously
	int pos = 0;
	void process(float x, float out[2]) {
		const StrataHalfband& hb = stHalfband();
		const int U = 2 * ST_HB;
		h[pos] = h[pos + U] = x;
		pos = (pos + 1) % U;
		const float* q = h + pos;                     // q[U - 1] is the newest
		float odd = 0.f;
		for (int i = 0; i < ST_HB; i++) odd += hb.c[i] * (q[ST_HB - 1 - i] + q[ST_HB + i]);
		out[0] = q[ST_HB - 1];
		out[1] = 2.f * odd;                           // the odd phase of a zero-stuffed interpolator
	}
};
// one x2 stage down: two samples in, one out
struct StrataDown2 {
	float h[8 * ST_HB] = {};
	int pos = 0;
	float process(float a, float b) {
		const StrataHalfband& hb = stHalfband();
		const int D = 4 * ST_HB;
		h[pos] = h[pos + D] = a; pos = (pos + 1) % D;
		h[pos] = h[pos + D] = b; pos = (pos + 1) % D;
		const float* q = h + pos + 1;                 // q[D - 2] is the newest
		int ctr = D - 2 - (2 * ST_HB - 1);            // an even sample, every tap inside
		float y = 0.5f * q[ctr];
		for (int i = 0; i < ST_HB; i++) y += hb.c[i] * (q[ctr - 1 - 2 * i] + q[ctr + 1 + 2 * i]);
		return y;
	}
};

struct StrataBl {
	float naive[ST_BLL] = {}, corr[ST_BLL] = {};
	int pos = 0;
	// a discontinuity at p in (-1, 0] samples from the current one: a step of
	// height h, or a change of slope of d per sample
	void add(float p, float h, float d) {
		const StrataBlTables& t = stBlTables();
		for (int j = -ST_BLZ; j <= ST_BLZ; j++) {
			float u = ((float)j - p + ST_BLZ) * ST_BLO;
			if (u < 0.f || u >= (float)(2 * ST_BLZ * ST_BLO)) continue;
			int i = (int)u; float f = u - (float)i;
			float r = 0.f;
			if (h != 0.f) r += h * (t.step[i] + (t.step[i + 1] - t.step[i]) * f);
			if (d != 0.f) r += d * (t.ramp[i] + (t.ramp[i + 1] - t.ramp[i]) * f);
			corr[(pos + j) & (ST_BLL - 1)] += r;
		}
	}
	// the naive sample in, the corrected one ST_BLZ samples ago out
	float process(float x) {
		naive[pos] = x;
		int o = (pos - ST_BLZ) & (ST_BLL - 1);
		float y = naive[o] + corr[o];
		corr[o] = 0.f;
		pos = (pos + 1) & (ST_BLL - 1);
		return y;
	}
};

// ── the table ────────────────────────────────────────────────────────────────
// Every frame band-limited per octave, as Wave's snapshots are: level k is the
// frame with every harmonic above 1024 >> k removed, so a voice reads the level
// whose top harmonic stays under Nyquist and nothing aliases. The short levels
// are stored short (the top harmonic still gets four samples a cycle).
struct StrataTable {
	int cols = 1, rows = 1, layers = 1, frames = 0;
	int per = 0;                                  // floats per frame, all levels
	int offs[ST_LEVELS] = {};
	std::vector<float> data;
	std::string name;
	std::vector<std::string> layerNames;
	StrataTable() {
		int o = 0;
		for (int k = 0; k < ST_LEVELS; k++) { offs[k] = o; o += ST_LEN[k]; }
		per = o;
	}
	const float* level(int f, int k) const { return data.data() + (size_t)f * per + offs[k]; }
	int index(int x, int y, int z) const { return (z * rows + y) * cols + x; }
};

// Conform: the load options that make unlike frames behave alike. Static,
// applied once, and skipped for whatever the file's `wt3d` flags say is done.
struct StrataConform {
	bool normalize = true, removeDC = true, align = false;
};

// One frame of any size in, its mip levels out.
struct StrataMipper {
	dsp::RealFFT* fftIn = nullptr;
	int nIn = 0;
	dsp::RealFFT* fftLev[ST_LEVELS] = {};
	std::vector<float> spec, work, tmp;
	StrataMipper() {
		for (int k = 0; k < ST_LEVELS; k++) {
			bool dup = false;
			for (int j = 0; j < k; j++) if (ST_LEN[j] == ST_LEN[k]) { fftLev[k] = fftLev[j]; dup = true; }
			if (!dup) fftLev[k] = new dsp::RealFFT(ST_LEN[k]);
		}
		work.assign(2 * ST_N, 0.f);
		tmp.assign(2 * ST_N, 0.f);
	}
	~StrataMipper() {
		delete fftIn;
		for (int k = 0; k < ST_LEVELS; k++) {
			bool dup = false;
			for (int j = 0; j < k; j++) if (fftLev[j] == fftLev[k]) dup = true;
			if (!dup) delete fftLev[k];
		}
	}
	StrataMipper(const StrataMipper&) = delete;
	StrataMipper& operator=(const StrataMipper&) = delete;

	// Spectrum of an n-sample frame in RealFFT's packing (bin 0 = DC, bin 1 =
	// Nyquist, then re/im pairs). RealFFT wants a multiple of 32; anything else
	// (rare: WaveStack writes powers of two) goes through a plain DFT.
	void analyse(const float* x, int n) {
		spec.assign(std::max(2 * n, 64), 0.f);
		if (n % 32 == 0 && n >= 32) {
			if (n != nIn) { delete fftIn; fftIn = new dsp::RealFFT(n); nIn = n; }
			std::vector<float> in(x, x + n);
			fftIn->rfft(in.data(), spec.data());
			return;
		}
		int H = std::min(n / 2 - 1, ST_N / 2);
		for (int i = 0; i < n; i++) spec[0] += x[i];
		for (int h = 1; h <= H; h++) {
			double re = 0, im = 0;
			for (int i = 0; i < n; i++) {
				double a = 2.0 * M_PI * h * i / n;
				re += x[i] * std::cos(a); im -= x[i] * std::sin(a);
			}
			spec[2 * h] = (float)re; spec[2 * h + 1] = (float)im;
		}
	}

	// Fill one frame's levels in `out` (per floats, StrataTable layout).
	void build(const float* x, int n, const StrataConform& c, float* out, const int* offs) {
		std::vector<float> frame(x, x + n);
		if (c.align) {
			// start at the rising zero crossing nearest the frame's start, so
			// frames do not click against each other as the point moves
			int best = 0, bestD = n;
			for (int i = 0; i < n; i++) {
				float a = frame[i], b = frame[(i + 1) % n];
				if (a <= 0.f && b > 0.f) { int d = std::min(i, n - i); if (d < bestD) { bestD = d; best = i + 1; } }
			}
			std::rotate(frame.begin(), frame.begin() + (best % n), frame.end());
		}
		analyse(frame.data(), n);
		int H = std::min(n / 2 - 1, ST_N / 2);
		for (int k = 0; k < ST_LEVELS; k++) {
			int L = ST_LEN[k];
			int maxBin = std::min({(ST_N / 2) >> k, L / 2 - 1, H});
			std::fill(work.begin(), work.begin() + L, 0.f);
			work[0] = c.removeDC ? 0.f : spec[0];
			for (int b = 1; b <= maxBin; b++) { work[2 * b] = spec[2 * b]; work[2 * b + 1] = spec[2 * b + 1]; }
			fftLev[k]->irfft(work.data(), tmp.data());
			float* dst = out + offs[k];
			float g = 1.f / (float)n;
			for (int i = 0; i < L; i++) dst[i] = tmp[i] * g;
		}
		if (c.normalize) {
			float pk = 0.f;
			for (int i = 0; i < ST_LEN[0]; i++) pk = std::max(pk, std::fabs(out[offs[0] + i]));
			if (pk > 1e-6f) {
				float g = 1.f / pk;
				for (int k = 0; k < ST_LEVELS; k++) for (int i = 0; i < ST_LEN[k]; i++) out[offs[k] + i] *= g;
			}
		}
	}
};

// ── reading files ────────────────────────────────────────────────────────────
struct StrataFile {
	std::vector<float> samples;                   // mono
	int frameSize = 0;
	int cols = 0, rows = 0, layers = 0;           // from `wt3d`, 0 when absent or inconsistent
	uint32_t flags = 0;
	std::vector<std::string> layerNames;
	std::string err;
};

static uint32_t stLE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// A WAV's samples through dr_wav, and its wavetable chunks by hand: `clm `
// (Serum), `srge` (Surge) and `wt3d` (the table's shape; spec in WaveStack's
// docs/wt3d.md). Only the first `wt3d` counts, and one that does not account
// for every sample is ignored, so the file still opens as one row. Layer
// names keep their places (empty ones too) and are dropped unless there is
// one per layer.
static StrataFile stReadWav(const std::string& path) {
	StrataFile f;
	std::ifstream in(path, std::ios::binary);
	if (!in) { f.err = "cannot open " + system::getFilename(path); return f; }
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) {
		f.err = system::getFilename(path) + " is not a WAV"; return f;
	}
	int hint = 0;
	uint32_t gfs = 0, gc = 0, gr = 0, gl = 0;
	bool seenGrid = false;
	for (size_t p = 12; p + 8 <= b.size();) {
		const uint8_t* id = b.data() + p;
		size_t len = stLE32(b.data() + p + 4), st = p + 8, end = std::min(st + len, b.size());
		const uint8_t* body = b.data() + st;
		size_t bl = end - st;
		if (!std::memcmp(id, "clm ", 4) && !hint) {
			std::string t((const char*)body, bl);
			size_t k = t.find("<!>");
			if (k != std::string::npos) hint = std::atoi(t.c_str() + k + 3);
		} else if (!std::memcmp(id, "srge", 4) && bl >= 8 && !hint) {
			hint = (int)stLE32(body + 4);
		} else if (!std::memcmp(id, "wt3d", 4) && !seenGrid) {
			seenGrid = true;
			if (bl >= 28 && stLE32(body) >= 1) {
				gfs = stLE32(body + 4); gc = stLE32(body + 8); gr = stLE32(body + 12); gl = stLE32(body + 16);
				f.flags = stLE32(body + 20);
				size_t n = std::min((size_t)stLE32(body + 24), bl - 28);
				// one NUL-terminated name per layer; tolerate a missing final NUL
				std::string cur;
				for (size_t i = 0; i < n; i++) {
					char c = (char)body[28 + i];
					if (c) cur += c; else { f.layerNames.push_back(cur); cur.clear(); }
				}
				if (n && body[28 + n - 1]) f.layerNames.push_back(cur);
			}
		}
		p = st + len + (len & 1);
	}
	unsigned ch = 0, sr = 0;
	drwav_uint64 frames = 0;
	float* pcm = drwav_open_memory_and_read_pcm_frames_f32(b.data(), b.size(), &ch, &sr, &frames, nullptr);
	if (!pcm || !ch) { f.err = "cannot read " + system::getFilename(path); return f; }
	f.samples.resize((size_t)frames);
	for (size_t i = 0; i < (size_t)frames; i++) {
		float s = 0.f;
		for (unsigned c = 0; c < ch; c++) s += pcm[i * ch + c];
		f.samples[i] = s / (float)ch;
	}
	drwav_free(pcm, nullptr);
	size_t n = f.samples.size();
	// frame size x cols x rows x layers, refusing anything that overflows
	auto fits = [n](uint64_t a, uint64_t b) { return a && b && a <= n / b; };
	uint64_t want = gfs;
	bool grid = gfs && fits(want, gc) && fits(want *= gc, gr) && fits(want *= gr, gl) && (want *= gl) == n;
	if (grid && gfs <= (uint32_t)INT32_MAX) {
		f.frameSize = (int)gfs; f.cols = (int)gc; f.rows = (int)gr; f.layers = (int)gl;
		if (f.layerNames.size() != gl) f.layerNames.clear();
	} else {
		f.frameSize = hint > 0 ? hint : ST_N;
		f.flags = 0;
		f.layerNames.clear();
	}
	if (f.frameSize <= 0 || n < (size_t)f.frameSize) { f.err = system::getFilename(path) + " holds less than one frame"; return f; }
	return f;
}

// ── the module ───────────────────────────────────────────────────────────────
struct StrataCellQ : ParamQuantity {
	int axis = 0;                                 // 0 x, 1 y, 2 z
	std::string getDisplayValueString() override;
};

struct Strata : Module {
	enum ParamId {
		FREQ_PARAM, FM_PARAM, X_PARAM, Y_PARAM, Z_PARAM, GLIDE_PARAM,
		WARP_PARAM, FOLD_PARAM, TILT_PARAM, SYNC_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT, FM_INPUT, X_INPUT, Y_INPUT, Z_INPUT, GLIDE_INPUT,
		WARP_INPUT, FOLD_INPUT, TILT_INPUT, SYNC_INPUT,
		HARD_INPUT, CLOCK_INPUT, RESET_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUT_OUTPUT, X_OUTPUT, Y_OUTPUT, Z_OUTPUT, STEP_OUTPUT, LEFT_OUTPUT, RIGHT_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	// the table, handed from the loader thread to the audio thread and retired
	// to the UI thread, so neither allocation nor freeing happens in process()
	StrataTable* table = nullptr;
	std::atomic<StrataTable*> incoming{nullptr};
	std::atomic<StrataTable*> retired{nullptr};
	std::thread loader;
	std::atomic<bool> loading{false};
	char loadError[160] = {};                     // read by the screen; a fixed buffer, so a read mid-write cannot crash
	std::vector<std::string> sources;             // what was loaded, saved with the patch
	StrataConform conform;
	bool squareUp = false;                        // a 1D table with a square frame count opens as a grid

	enum { WRAP_CLAMP, WRAP_WRAP };
	int wrap[3] = {WRAP_CLAMP, WRAP_CLAMP, WRAP_CLAMP};

	// the sequence
	struct Step { float x, y, z; };
	Step steps[ST_MAXSTEPS];
	int nSteps = 0, cur = 0;
	enum { GLIDE_RATE, GLIDE_TIME };
	int glideMode = GLIDE_RATE;
	enum { PATH_STRAIGHT, PATH_GRID };
	int pathMode = PATH_STRAIGHT;
	float gp[3] = {0.f, 0.f, 0.f};                // where the glide is
	float gv[3] = {0.f, 0.f, 0.f};                // time mode: per-axis velocity
	dsp::SchmittTrigger clockTrig, resetTrig;
	dsp::PulseGenerator stepPulse;

	struct Voice {
		double phase = 0.0;
		float hpState = 0.f;
		float dcX = 0.f, dcY = 0.f;                   // the output's DC blocker
		double foldX = 0.0;                           // the folder's last input, for its antiderivative
		StrataUp2 up1, up2;                           // the folder's x4: 1x to 2x, then 2x to 4x
		StrataDown2 dn1, dn2;
		dsp::SchmittTrigger hard;
		StrataBl bl;                                  // band-limits sync's restart and warp's corners
		float px = 0.f, py = 0.f, pz = 0.f;       // where it read, for the outputs and the screen
		float gL = 0.f, gR = 0.f;                     // its level at each listener, slewed
	};
	Voice voice[ST_POLY];
	int nVoices = 1;
	// The listeners: two points in the cube (world units, see ST_CUBE_*) that
	// the stereo mix is heard from. A voice is as loud at each as its distance
	// from it allows, so where it reads in the table is where it sits in the
	// image. A 1D or 2D table is a plane, and only the listeners' x and depth
	// count. Dragged on the screen; saved.
	float lis[2][3] = {{-0.7f, 0.f, -0.8f}, {0.7f, 0.f, -0.8f}};
	void resetListeners() { lis[0][0] = -0.7f; lis[0][1] = 0.f; lis[0][2] = -0.8f; lis[1][0] = 0.7f; lis[1][1] = 0.f; lis[1][2] = -0.8f; }
	// the screen's view, saved; the default is turned a little off square so
	// the table reads as a cube rather than as a flat face
	static constexpr float YAW0 = -0.45f, PITCH0 = 0.5f;
	float camYaw = YAW0, camPitch = PITCH0;
	float shownKnee = 0.5f, shownRatio = 1.f, shownFold = 0.f;   // the shape in force, for the screen

	Strata() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FREQ_PARAM, -4.f, 4.f, 0.f, "Frequency", " Hz", 2.f, dsp::FREQ_C4);
		configParam(FM_PARAM, -1.f, 1.f, 0.f, "FM depth (linear, through zero)", "%", 0.f, 100.f);
		configParam<StrataCellQ>(X_PARAM, 0.f, 1.f, 0.f, "X (column)");
		configParam<StrataCellQ>(Y_PARAM, 0.f, 1.f, 0.f, "Y (row)");
		configParam<StrataCellQ>(Z_PARAM, 0.f, 1.f, 0.f, "Z (layer)");
		for (int a = 0; a < 3; a++) dynamic_cast<StrataCellQ*>(paramQuantities[X_PARAM + a])->axis = a;
		configParam(GLIDE_PARAM, 0.f, 1.f, 0.3f, "Glide between steps", "%", 0.f, 100.f);
		configParam(WARP_PARAM, -1.f, 1.f, 0.f, "Warp (phase bend)", "%", 0.f, 100.f);
		configParam(FOLD_PARAM, 0.f, 1.f, 0.f, "Fold", "%", 0.f, 100.f);
		configParam(TILT_PARAM, -1.f, 1.f, 0.f, "Tilt (darker to brighter)", "%", 0.f, 100.f);
		configParam(SYNC_PARAM, 0.f, 1.f, 0.f, "Sync (internal ratio, 1x to 8x)", "x", 0.f, 7.f, 1.f);
		configInput(VOCT_INPUT, "V/OCT (poly)");
		configInput(FM_INPUT, "FM (poly)");
		configInput(X_INPUT, "X CV (poly, 1V per cell)");
		configInput(Y_INPUT, "Y CV (poly, 1V per cell)");
		configInput(Z_INPUT, "Z CV (poly, 1V per cell)");
		configInput(GLIDE_INPUT, "Glide CV");
		configInput(WARP_INPUT, "Warp CV");
		configInput(FOLD_INPUT, "Fold CV");
		configInput(TILT_INPUT, "Tilt CV");
		configInput(SYNC_INPUT, "Sync CV");
		configInput(HARD_INPUT, "Hard sync (poly): restarts the cycle");
		configInput(CLOCK_INPUT, "Clock: on to the next waypoint (click frames on the screen to set them)");
		configInput(RESET_INPUT, "Reset: back to the first waypoint");
		configOutput(OUT_OUTPUT, "Audio (poly)");
		configOutput(X_OUTPUT, "X position (poly, 1V per cell)");
		configOutput(Y_OUTPUT, "Y position (poly, 1V per cell)");
		configOutput(Z_OUTPUT, "Z position (poly, 1V per cell)");
		configOutput(STEP_OUTPUT, "Step: a trigger as each waypoint begins");
		configOutput(LEFT_OUTPUT, "Left: every voice, heard from the L listener");
		configOutput(RIGHT_OUTPUT, "Right: every voice, heard from the R listener");
		std::string err;
		sources = factorySources(3);
		delete incoming.exchange(buildFromFiles(sources, conform, squareUp, err));
	}
	~Strata() override {
		if (loader.joinable()) loader.join();
		delete table;
		delete incoming.exchange(nullptr);
		delete retired.exchange(nullptr);
	}

	// ── building tables ──────────────────────────────────────────────────
	// The factory tables are LAYERS, each named as a source ("factory:sync"),
	// so a factory table is a list of sources like any stack of files and a
	// factory layer can be added on top of anything with the same grid.
	// Built in the frequency domain, one inverse FFT a frame, in milliseconds.
	//   shapes     1D, 16 frames: sine, triangle, saw, square, narrow pulse
	//   harmonics  8 x 8: X adds harmonics, Y tilts odd-only to every one
	//   formant    8 x 8: the same with a resonant peak walking up the spectrum
	//   sync       8 x 8: a hard-synced saw, ratio 1 to 8.9 in reading order
	//   pulse      8 x 8: X narrows the pulse, Y opens the harmonics
	static std::vector<std::string> factorySources(int dims) {
		if (dims == 1) return {"factory:shapes"};
		if (dims == 2) return {"factory:harmonics"};
		return {"factory:harmonics", "factory:formant", "factory:sync", "factory:pulse"};
	}
	static bool isFactory(const std::string& p) { return p.compare(0, 8, "factory:") == 0; }
	static std::string sourceName(const std::string& p) {
		if (!isFactory(p)) return system::getStem(p);
		std::string k = p.substr(8);
		if (!k.empty()) k[0] = (char)std::toupper((unsigned char)k[0]);
		return k;
	}
	static bool factoryLayer(const std::string& path, StrataFile& f) {
		std::string kind = path.substr(8);
		dsp::RealFFT fft(ST_N);
		std::vector<float> spec(2 * ST_N), tmp(2 * ST_N);
		// a frame from a spectrum: `amp(h, re, im)` per harmonic up to H,
		// Lanczos-tapered so a truncated series does not ring, peak-normalised
		auto frame = [&](int H, std::function<void(int, float&, float&)> amp, float* out) {
			std::fill(spec.begin(), spec.end(), 0.f);
			H = std::min(H, ST_N / 2 - 1);
			for (int h = 1; h <= H; h++) {
				float re = 0.f, im = 0.f;
				amp(h, re, im);
				float x = (float)M_PI * h / (H + 1), sg = H > 4 ? std::sin(x) / x : 1.f;
				spec[2 * h] = re * sg; spec[2 * h + 1] = im * sg;
			}
			fft.irfft(spec.data(), tmp.data());
			float pk = 1e-9f;
			for (int i = 0; i < ST_N; i++) pk = std::max(pk, std::fabs(tmp[i]));
			for (int i = 0; i < ST_N; i++) out[i] = tmp[i] / pk;
		};
		f.frameSize = ST_N; f.layers = 1; f.flags = 0; f.err.clear();
		f.layerNames = {sourceName(path)};
		if (kind == "shapes") {
			f.cols = 16; f.rows = 1;
			std::vector<float> anchor(5 * ST_N);
			frame(1, [](int h, float& re, float& im) { if (h == 1) im = -1.f; }, &anchor[0]);
			frame(255, [](int h, float& re, float& im) { if (h % 2) im = -(((h - 1) / 2) % 2 ? -1.f : 1.f) / (float)(h * h); }, &anchor[ST_N]);
			frame(255, [](int h, float& re, float& im) { im = (h % 2 ? -1.f : 1.f) / (float)h; }, &anchor[2 * ST_N]);
			frame(255, [](int h, float& re, float& im) { re = std::sin((float)M_PI * h * 0.5f) / (float)h; }, &anchor[3 * ST_N]);
			frame(255, [](int h, float& re, float& im) { re = std::sin((float)M_PI * h * 0.12f) / (float)h; }, &anchor[4 * ST_N]);
			f.samples.assign((size_t)16 * ST_N, 0.f);
			for (int c = 0; c < 16; c++) {
				float pos = c * 4.f / 15.f;
				int a = std::min(3, (int)pos); float t = pos - a;
				for (int i = 0; i < ST_N; i++)
					f.samples[(size_t)c * ST_N + i] = anchor[a * ST_N + i] + (anchor[(a + 1) * ST_N + i] - anchor[a * ST_N + i]) * t;
			}
			return true;
		}
		f.cols = 8; f.rows = 8;
		f.samples.assign((size_t)64 * ST_N, 0.f);
		for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
			float* out = f.samples.data() + (size_t)(r * 8 + c) * ST_N;
			if (kind == "harmonics" || kind == "formant") {
				bool formant = kind == "formant";
				frame(1 + c * c * 2, [&](int h, float& re, float& im) {
					float a = 1.f / h;
					if (h % 2 == 0) a *= r / 7.f;
					if (formant) { float centre = 2.f + c * 3.f + r; a *= 0.3f + 2.f * std::exp(-std::pow((h - centre) / (1.5f + r * 0.3f), 2.f)); }
					im = -a;
				}, out);
			} else if (kind == "sync") {
				// drawn in time and band-limited: a saw restarted every cycle
				float ratio = 1.f + c + r / 8.f;
				for (int i = 0; i < ST_N; i++) { float p = ratio * i / ST_N; tmp[i] = 2.f * (p - std::floor(p)) - 1.f; }
				std::vector<float> x(tmp.begin(), tmp.begin() + ST_N), sp(2 * ST_N);
				fft.rfft(x.data(), sp.data());
				frame(300, [&](int h, float& re, float& im) { re = sp[2 * h]; im = sp[2 * h + 1]; }, out);
			} else if (kind == "pulse") {
				float w = 0.5f - c * (0.45f / 7.f);
				frame(4 << r, [&](int h, float& re, float& im) { re = std::sin((float)M_PI * h * w) / (float)h; }, out);
			} else {
				f.err = "no factory layer called " + kind;
				return false;
			}
		}
		return true;
	}
	void loadFactory(int dims) { load(factorySources(dims)); }
	// a table (or a factory layer) stacked on top of the one in force; refused,
	// with the table kept, when the grids differ
	void addLayer(const std::string& path) {
		if (sources.empty()) return;
		std::vector<std::string> s = sources;
		s.push_back(path);
		load(s);
	}
	void removeTopLayer() {
		if (sources.size() < 2) return;
		std::vector<std::string> s = sources;
		s.pop_back();
		load(s);
	}

	// Load one file (its own grid) or several (each a layer). Runs on a
	// thread; the table arrives in process() when it is built.
	void load(std::vector<std::string> paths) {
		if (paths.empty()) return;
		if (loader.joinable()) loader.join();
		loading = true;
		loadError[0] = 0;
		StrataConform c = conform;
		bool sq = squareUp;
		loader = std::thread([this, paths, c, sq]() {
			std::string err;
			StrataTable* t = buildFromFiles(paths, c, sq, err);
			if (t) { delete incoming.exchange(t); sources = paths; }
			else snprintf(loadError, sizeof(loadError), "%s", err.c_str());
			loading = false;
		});
	}
	void waitLoaded() { if (loader.joinable()) loader.join(); }

	static StrataTable* buildFromFiles(std::vector<std::string> paths, StrataConform c, bool sq, std::string& err) {
		// stacked in the order given, bottom layer first (a folder is sorted
		// by name before it gets here)
		std::vector<StrataFile> files;
		for (const std::string& p : paths) {
			StrataFile f;
			if (isFactory(p)) factoryLayer(p, f);
			else f = stReadWav(p);
			if (!f.err.empty()) { err = f.err; return nullptr; }
			if (!f.cols) {                         // no grid: one row, or square if asked
				int n = (int)(f.samples.size() / f.frameSize);
				f.samples.resize((size_t)n * f.frameSize);
				int r = (int)std::lround(std::sqrt((float)n));
				if (sq && r > 1 && r * r == n && r <= ST_MAXDIM) { f.cols = r; f.rows = r; }
				else { f.cols = n; f.rows = 1; }
				f.layers = 1;
			}
			files.push_back(std::move(f));
		}
		// one file is its own volume; several files are layers and must share a grid
		const StrataFile& a = files[0];
		int cols = a.cols, rows = a.rows, layers = 0;
		for (const StrataFile& f : files) {
			if (f.cols != cols || f.rows != rows) {
				err = "layers must share a grid (" + std::to_string(cols) + " x " + std::to_string(rows) + ")";
				return nullptr;
			}
			layers += f.layers;
		}
		if (cols > ST_MAXDIM * (rows == 1 ? ST_MAXDIM : 1) || rows > ST_MAXDIM || layers > ST_MAXDIM || cols * rows * layers > ST_MAXFRAMES) {
			err = "too large: Strata holds up to 1024 frames, 16 per axis";
			return nullptr;
		}
		// a long 1D table (Serum's 256 frames) is allowed past 16 columns, up to 256
		StrataTable* t = new StrataTable();
		t->cols = cols; t->rows = rows; t->layers = layers; t->frames = cols * rows * layers;
		t->data.assign((size_t)t->frames * t->per, 0.f);
		StrataMipper mip;
		int fi = 0;
		for (const StrataFile& f : files) {
			StrataConform fc = c;
			if (f.flags & 1) fc.normalize = false;
			if (f.flags & 2) fc.removeDC = false;
			if (f.flags & 4) fc.align = false;
			int n = f.cols * f.rows * f.layers;
			for (int i = 0; i < n; i++, fi++)
				mip.build(f.samples.data() + (size_t)i * f.frameSize, f.frameSize, fc, t->data.data() + (size_t)fi * t->per, t->offs);
			const std::string& src = paths[&f - files.data()];
			for (int l = 0; l < f.layers; l++)
				t->layerNames.push_back((int)f.layerNames.size() == f.layers ? f.layerNames[l]
				                        : f.layers == 1 ? sourceName(src) : sourceName(src) + " " + std::to_string(l + 1));
		}
		bool allFactory = true, oneDir = true;
		for (const std::string& p : paths) {
			allFactory = allFactory && isFactory(p);
			oneDir = oneDir && !isFactory(p) && system::getDirectory(p) == system::getDirectory(paths[0]);
		}
		t->name = allFactory ? "Factory" : paths.size() == 1 ? sourceName(paths[0])
		        : oneDir ? system::getFilename(system::getDirectory(paths[0])) : sourceName(paths[0]) + " stack";
		return t;
	}

	// ── the sequence ─────────────────────────────────────────────────────
	// GLIDE: 0 is a jump; above that, seconds per cell from 5 ms to 2 s. In rate
	// mode that is a speed, so a long move takes longer than a short one; in
	// time mode it is the time of every move.
	static float glideSecPerCell(float k) { return k <= 0.001f ? 0.f : 0.005f * std::pow(400.f, clamp(k, 0.f, 1.f)); }
	void aimGlide(float secPerCell) {
		if (glideMode != GLIDE_TIME || nSteps == 0) return;
		const Step& s = steps[cur];
		float d[3] = {s.x - gp[0], s.y - gp[1], s.z - gp[2]};
		// every move takes one glide time, whatever its length (and goes straight)
		for (int a = 0; a < 3; a++) gv[a] = secPerCell > 0.f ? d[a] / secPerCell : 0.f;
	}
	void stepGlide(float dt, float secPerCell) {
		if (nSteps == 0) return;
		const Step& s = steps[cur];
		float tgt[3] = {s.x, s.y, s.z};
		if (secPerCell <= 0.f) { for (int a = 0; a < 3; a++) gp[a] = tgt[a]; return; }
		if (glideMode == GLIDE_TIME) {
			for (int a = 0; a < 3; a++) {
				float d = tgt[a] - gp[a], v = gv[a] * dt;
				if (v == 0.f || std::fabs(v) >= std::fabs(d)) gp[a] = tgt[a];
				else gp[a] += v;
			}
			return;
		}
		float budget = dt / secPerCell;            // cells this sample
		if (pathMode == PATH_GRID) {
			for (int a = 0; a < 3 && budget > 0.f; a++) {
				float d = tgt[a] - gp[a];
				float m = std::min(std::fabs(d), budget);
				gp[a] += d >= 0.f ? m : -m;
				budget -= m;
			}
			return;
		}
		float d[3] = {tgt[0] - gp[0], tgt[1] - gp[1], tgt[2] - gp[2]};
		float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dist <= budget) { for (int a = 0; a < 3; a++) gp[a] = tgt[a]; return; }
		for (int a = 0; a < 3; a++) gp[a] += d[a] / dist * budget;
	}
	void addStep(float x, float y, float z) {
		if (nSteps >= ST_MAXSTEPS) return;
		steps[nSteps++] = {x, y, z};
	}

	// ── reading the volume ────────────────────────────────────────────────
	static inline float readLevel(const float* t, int L, float phase) {
		float fp = phase * L;
		int i0 = (int)fp;
		float f = fp - (float)i0;
		i0 &= (L - 1);
		int i1 = (i0 + 1) & (L - 1);
		return t[i0] + (t[i1] - t[i0]) * f;
	}
	// one axis: the two cells either side and the weight of the upper one
	static inline void axisCells(float p, int n, int mode, int& a, int& b, float& w) {
		if (n <= 1) { a = b = 0; w = 0.f; return; }
		if (mode == WRAP_WRAP) {
			p -= std::floor(p / n) * n;
			a = (int)p; w = p - a; b = (a + 1) % n;
			return;
		}
		p = clamp(p, 0.f, (float)(n - 1));
		a = (int)p; if (a >= n - 1) a = n - 2;
		w = p - a; b = a + 1;
	}

	void process(const ProcessArgs& args) override {
		if (StrataTable* t = incoming.exchange(nullptr)) {
			StrataTable* old = table;
			table = t;
			delete retired.exchange(old);          // only if the UI has not collected the last one: rare
		}
		const StrataTable* T = table;

		// ── the sequence ──
		float glideK = clamp(params[GLIDE_PARAM].getValue() + inputs[GLIDE_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float spc = glideSecPerCell(glideK);
		bool seqOn = nSteps > 0 && inputs[CLOCK_INPUT].isConnected();
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f) && nSteps > 0) {
			cur = 0;
			gp[0] = steps[0].x; gp[1] = steps[0].y; gp[2] = steps[0].z;
			stepPulse.trigger(1e-3f);
		}
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f) && nSteps > 0) {
			cur = (cur + 1) % nSteps;
			aimGlide(spc);
			stepPulse.trigger(1e-3f);
		}
		if (seqOn) stepGlide(args.sampleTime, spc);
		float sx = seqOn ? gp[0] : 0.f, sy = seqOn ? gp[1] : 0.f, sz = seqOn ? gp[2] : 0.f;

		// ── the shape macros ──
		float warp = clamp(params[WARP_PARAM].getValue() + inputs[WARP_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
		float fold = clamp(params[FOLD_PARAM].getValue() + inputs[FOLD_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float tilt = clamp(params[TILT_PARAM].getValue() + inputs[TILT_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
		float syncK = clamp(params[SYNC_PARAM].getValue() + inputs[SYNC_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float ratio = 1.f + 7.f * syncK;
		// WARP: the cycle's first half squeezed into [0, knee) or stretched,
		// the second half taking the rest. Its steepest slope, and the sync
		// ratio, raise the effective pitch the mip level must band-limit for.
		float knee = 0.5f - 0.45f * warp;
		float slope = 0.5f / std::min(knee, 1.f - knee);
		float hfMul = slope * ratio;
		float darken = tilt < 0.f ? -tilt * 3.f : 0.f; // octaves of mip bias
		float hiBoost = tilt > 0.f ? tilt * 2.f : 0.f;
		float hpA = 1.f - std::exp(-2.f * (float)M_PI * 1500.f * args.sampleTime);
		float foldGain = stFoldGain(fold);
		shownKnee = knee; shownRatio = ratio; shownFold = fold;
		// WARP spends unequal time either side of zero and FOLD is not
		// symmetric about it, so either can leave DC on the output; a 2 Hz
		// blocker takes it off (measured: warp 0.8 put DC 14 dB under the signal)
		float dcR = 1.f - 2.f * (float)M_PI * 2.f * args.sampleTime;

		int nch = std::max({1, inputs[VOCT_INPUT].getChannels(), inputs[X_INPUT].getChannels(),
		                    inputs[Y_INPUT].getChannels(), inputs[Z_INPUT].getChannels()});
		nch = std::min(nch, ST_POLY);
		nVoices = nch;
		float fBase = params[FREQ_PARAM].getValue();
		float fmDepth = params[FM_PARAM].getValue();
		float kx = params[X_PARAM].getValue(), ky = params[Y_PARAM].getValue(), kz = params[Z_PARAM].getValue();
		outputs[OUT_OUTPUT].setChannels(nch);
		bool mixing = outputs[LEFT_OUTPUT].isConnected() || outputs[RIGHT_OUTPUT].isConnected();
		float mixL = 0.f, mixR = 0.f;
		float gSlew = 1.f - std::exp(-args.sampleTime / 0.005f);
		outputs[X_OUTPUT].setChannels(nch); outputs[Y_OUTPUT].setChannels(nch); outputs[Z_OUTPUT].setChannels(nch);

		for (int c = 0; c < nch; c++) {
			Voice& v = voice[c];
			float freq = dsp::FREQ_C4 * dsp::exp2_taylor5(fBase + inputs[VOCT_INPUT].getPolyVoltage(c));
			if (inputs[FM_INPUT].isConnected()) freq *= 1.f + fmDepth * inputs[FM_INPUT].getPolyVoltage(c) * 0.2f;
			double inc = (double)freq * args.sampleTime;

			if (!T) {
				v.phase += inc; v.phase -= std::floor(v.phase);
				outputs[OUT_OUTPUT].setVoltage(0.f, c);
				continue;
			}
			float px = sx + kx * (T->cols - 1) + inputs[X_INPUT].getPolyVoltage(c);
			float py = sy + ky * (T->rows - 1) + inputs[Y_INPUT].getPolyVoltage(c);
			float pz = sz + kz * (T->layers - 1) + inputs[Z_INPUT].getPolyVoltage(c);
			int x0, x1, y0, y1, z0, z1; float wx, wy, wz;
			axisCells(px, T->cols, wrap[0], x0, x1, wx);
			axisCells(py, T->rows, wrap[1], y0, y1, wy);
			axisCells(pz, T->layers, wrap[2], z0, z1, wz);
			v.px = x0 + wx; v.py = y0 + wy; v.pz = z0 + wz;
			if (wrap[0] == WRAP_WRAP && x1 == 0 && T->cols > 1) v.px = x0 + wx;   // reads between the last and first column

			// mip level: the top harmonic, 1024 >> k, under Nyquist, with a
			// crossfade into the next level so a pitch sweep does not step
			float eff = (float)std::fabs(inc) * hfMul;
			// Level k is alias-free while k >= log2(2048 * eff). Reading
			// floor(that) + 1 and fading toward the next level keeps EVERY level
			// in the mix under Nyquist and the brightness continuous as the pitch
			// sweeps; the cost is that the top octave of harmonics fades out
			// before Nyquist. Half an octave of margin was tried first and the
			// harness measured the level being faded out aliasing at -36 dB.
			float lf = std::log2(std::max(eff * (float)ST_N, 1e-9f)) + 1.f + darken;
			lf = clamp(lf, 0.f, (float)(ST_LEVELS - 1));
			int k0 = (int)lf; float kf = lf - k0;
			int k1 = std::min(k0 + 1, ST_LEVELS - 1);

			int fx[2] = {x0, x1}, fy[2] = {y0, y1}, fz[2] = {z0, z1};
			float wxs[2] = {1.f - wx, wx}, wys[2] = {1.f - wy, wy}, wzs[2] = {1.f - wz, wz};
			// the blended frame at frame phase q: the eight corners at two mip levels
			auto frame = [&](float q) {
				float s = 0.f;
				for (int iz = 0; iz < 2; iz++) {
					if (wzs[iz] <= 0.f) continue;
					for (int iy = 0; iy < 2; iy++) {
						float wzy = wzs[iz] * wys[iy];
						if (wzy <= 0.f) continue;
						for (int ix = 0; ix < 2; ix++) {
							float w = wzy * wxs[ix];
							if (w <= 0.f) continue;
							int f = T->index(fx[ix], fy[iy], fz[iz]);
							float a = readLevel(T->level(f, k0), ST_LEN[k0], q);
							if (kf > 0.f) a += (readLevel(T->level(f, k1), ST_LEN[k1], q) - a) * kf;
							s += w * a;
						}
					}
				}
				return s;
			};
			// WARP: slave phase to frame phase, a slope of wLo before the knee and wHi after
			float wLo = 0.5f / knee, wHi = 0.5f / (1.f - knee);
			auto warpOf = [&](float p) { return p < knee ? p * wLo : 0.5f + (p - knee) * wHi; };
			auto slaveOf = [&](double ph) { float p = (float)ph * ratio; return p - std::floor(p); };
			// the frame's slope per unit of frame phase, across one table step
			float hq = 1.f / (float)ST_LEN[k0];
			auto dframe = [&](float q) {
				float u = q + hq, l = q - hq;
				return (frame(u - std::floor(u)) - frame(l - std::floor(l))) * (0.5f / hq);
			};
			// the output's slope per sample, from the slave's
			float dps = ratio * (float)inc;
			// HARD sync: the cycle restarts now, a step and a change of slope
			if (inputs[HARD_INPUT].isConnected() && v.hard.process(inputs[HARD_INPUT].getPolyVoltage(c), 0.1f, 1.f)) {
				float pb = slaveOf(v.phase), qb = warpOf(pb);
				float h = frame(0.f) - frame(qb);
				float d = inc > 0.0 ? (dframe(0.f) * wLo - dframe(qb) * (pb < knee ? wLo : wHi)) * dps : 0.f;
				v.phase = 0.0;
				v.bl.add(0.f, h, d);
			}
			// Everything else this sample does to the waveform's corners, each
			// at its exact sub-sample position: the slave wrapping and passing
			// the knee are changes of slope (only while WARP bends it), and the
			// master wrapping under a ratio that is not whole restarts the slave.
			// Reverse travel (through-zero FM) goes uncorrected.
			double a0 = v.phase;
			v.phase += inc;
			bool masterWrap = v.phase >= 1.0;
			if (inc > 0.0) {
				bool bent = knee != 0.5f;
				float dWrap = bent ? dframe(0.f) * (wLo - wHi) * dps : 0.f;
				float dKnee = bent ? dframe(0.5f) * (wHi - wLo) * dps : 0.f;
				// master phase runs a0 -> a0 + inc; segment 2 is after its wrap
				for (int seg = 0; seg < (masterWrap ? 2 : 1); seg++) {
					double m0 = seg ? 0.0 : a0, m1 = seg ? v.phase - 1.0 : std::min(v.phase, 1.0);
					double base = seg ? 1.0 : 0.0;
					double u0 = m0 * ratio, u1 = m1 * ratio;
					if (bent) {
						for (double m = std::floor(u0); m <= u1; m += 1.0) {
							double ev[2] = {m, m + knee};
							float dd[2] = {dWrap, dKnee};
							for (int e = 0; e < 2; e++) {
								if (!(ev[e] > u0 && ev[e] <= u1)) continue;
								float at = (float)((base + ev[e] / ratio - a0) / inc) - 1.f;
								v.bl.add(clamp(at, -0.999999f, 0.f), 0.f, dd[e]);
							}
						}
					}
				}
				if (masterWrap && ratio != std::floor(ratio)) {
					float at = (float)((1.0 - a0) / inc) - 1.f;
					float pb = ratio - std::floor(ratio), qb = warpOf(pb);
					float h = frame(0.f) - frame(qb);
					float d = (dframe(0.f) * wLo - dframe(qb) * (pb < knee ? wLo : wHi)) * dps;
					v.bl.add(clamp(at, -0.999999f, 0.f), h, d);
				}
			}
			v.phase -= std::floor(v.phase);
			float s = v.bl.process(frame(warpOf(slaveOf(v.phase))));
			// TILT up: a one-pole high shelf
			if (hiBoost > 0.f) { v.hpState += (s - v.hpState) * hpA; s += hiBoost * (s - v.hpState); }
			// FOLD at four times the rate, each 4x sample anti-aliased by the
			// folder's antiderivative too: the mean of the folder over the
			// straight line from the last input, exact however many folds an
			// edge crosses in between. Folding a bright frame plainly measured
			// -17 dB of alias at 110 Hz; at 2x the 12x top of the knob still
			// measured -27 dB at 440 Hz. The averaging is a gentle lowpass, so it
			// fades in over the first tenth of the knob; the halfband stages run
			// even at zero, where they are flat, so the latency never switches.
			// The 2x-to-4x stages run at the 2x rate: fed twice a sample.
			{
				float z2[2], z4[2], y2[2];
				v.up1.process(s, z2);
				float adaa = std::min(fold * 10.f, 1.f);
				for (int j = 0; j < 2; j++) {
					v.up2.process(z2[j], z4);
					for (int k = 0; k < 2; k++) {
						double x = (double)z4[k] * foldGain, dx = x - v.foldX;
						if (fold > 0.f) {
							float plain = stFold((float)x);
							float ad = std::fabs(dx) > 1e-5 ? (float)((stFoldF(x) - stFoldF(v.foldX)) / dx)
							                                : stFold((float)(0.5 * (x + v.foldX)));
							z4[k] = plain + (ad - plain) * adaa;
						}
						v.foldX = x;
					}
					y2[j] = v.dn2.process(z4[0], z4[1]);
				}
				s = v.dn1.process(y2[0], y2[1]);
			}
			float y = s - v.dcX + dcR * v.dcY;
			v.dcX = s; v.dcY = y;
			outputs[OUT_OUTPUT].setVoltage(5.f * y, c);
			if (mixing) {
				// level falls with distance, d in cube widths: -3.5 dB a third of
				// the way across, -10 dB at the far side
				float wx = stWorldX(v.px, T->cols), wy = stWorldY(v.pz, T->layers), wz = stWorldZ(v.py, T->rows);
				float g[2];
				for (int k = 0; k < 2; k++) {
					float dx = wx - lis[k][0], dy = T->layers > 1 ? wy - lis[k][1] : 0.f, dz = wz - lis[k][2];
					g[k] = 1.f / (1.f + 1.5f * (dx * dx + dy * dy + dz * dz));
				}
				v.gL += (g[0] - v.gL) * gSlew; v.gR += (g[1] - v.gR) * gSlew;
				mixL += 5.f * y * v.gL; mixR += 5.f * y * v.gR;
			}
			outputs[X_OUTPUT].setVoltage(v.px, c);
			outputs[Y_OUTPUT].setVoltage(v.py, c);
			outputs[Z_OUTPUT].setVoltage(v.pz, c);
		}
		outputs[STEP_OUTPUT].setVoltage(stepPulse.process(args.sampleTime) ? 10.f : 0.f);
		// a chord of sixteen sums far past a single voice's level; bend it rather than clip
		auto soft = [](float x) { float a = std::fabs(x); return a <= 6.f ? x : std::copysign(6.f + 4.f * (1.f - 4.f / (a - 2.f)), x); };
		outputs[LEFT_OUTPUT].setVoltage(soft(mixL));
		outputs[RIGHT_OUTPUT].setVoltage(soft(mixR));
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_t* src = json_array();
		for (const std::string& s : sources) json_array_append_new(src, json_string(s.c_str()));
		json_object_set_new(r, "sources", src);
		json_t* st = json_array();
		for (int i = 0; i < nSteps; i++) {
			json_t* p = json_array();
			json_array_append_new(p, json_real(steps[i].x));
			json_array_append_new(p, json_real(steps[i].y));
			json_array_append_new(p, json_real(steps[i].z));
			json_array_append_new(st, p);
		}
		json_object_set_new(r, "steps", st);
		json_object_set_new(r, "glideMode", json_integer(glideMode));
		json_object_set_new(r, "pathMode", json_integer(pathMode));
		json_object_set_new(r, "wrapX", json_integer(wrap[0]));
		json_object_set_new(r, "wrapY", json_integer(wrap[1]));
		json_object_set_new(r, "wrapZ", json_integer(wrap[2]));
		json_object_set_new(r, "normalize", json_boolean(conform.normalize));
		json_object_set_new(r, "removeDC", json_boolean(conform.removeDC));
		json_object_set_new(r, "align", json_boolean(conform.align));
		json_object_set_new(r, "squareUp", json_boolean(squareUp));
		json_object_set_new(r, "camYaw", json_real(camYaw));
		json_object_set_new(r, "camPitch", json_real(camPitch));
		json_t* li = json_array();
		for (int k = 0; k < 2; k++) {
			json_t* p = json_array();
			for (int a = 0; a < 3; a++) json_array_append_new(p, json_real(lis[k][a]));
			json_array_append_new(li, p);
		}
		json_object_set_new(r, "listeners", li);
		return r;
	}
	void dataFromJson(json_t* r) override {
		auto i = [&](const char* k, int& v, int lo, int hi) { if (json_t* j = json_object_get(r, k)) v = clamp((int)json_integer_value(j), lo, hi); };
		auto b = [&](const char* k, bool& v) { if (json_t* j = json_object_get(r, k)) v = json_boolean_value(j); };
		i("glideMode", glideMode, 0, 1); i("pathMode", pathMode, 0, 1);
		i("wrapX", wrap[0], 0, 1); i("wrapY", wrap[1], 0, 1); i("wrapZ", wrap[2], 0, 1);
		b("normalize", conform.normalize); b("removeDC", conform.removeDC); b("align", conform.align); b("squareUp", squareUp);
		if (json_t* j = json_object_get(r, "camYaw")) camYaw = (float)json_real_value(j);
		if (json_t* j = json_object_get(r, "camPitch")) camPitch = clamp((float)json_real_value(j), -1.5f, 1.5f);
		if (json_t* li = json_object_get(r, "listeners"))
			for (int k = 0; k < 2 && k < (int)json_array_size(li); k++)
				for (int a = 0; a < 3; a++)
					if (json_t* q = json_array_get(json_array_get(li, k), a)) lis[k][a] = clamp((float)json_real_value(q), -1.f, 1.f);
		nSteps = 0;
		if (json_t* st = json_object_get(r, "steps"))
			for (size_t k = 0; k < json_array_size(st) && nSteps < ST_MAXSTEPS; k++) {
				json_t* p = json_array_get(st, k);
				if (json_array_size(p) == 3) addStep((float)json_real_value(json_array_get(p, 0)), (float)json_real_value(json_array_get(p, 1)), (float)json_real_value(json_array_get(p, 2)));
			}
		cur = 0;
		std::vector<std::string> src;
		if (json_t* s = json_object_get(r, "sources"))
			for (size_t k = 0; k < json_array_size(s); k++) src.push_back(json_string_value(json_array_get(s, k)));
		if (!src.empty()) load(src);
	}
};

std::string StrataCellQ::getDisplayValueString() {
	Strata* m = dynamic_cast<Strata*>(module);
	int n = 1;
	if (m && m->table) n = axis == 0 ? m->table->cols : axis == 1 ? m->table->rows : m->table->layers;
	const char* what[3] = {"column", "row", "layer"};
	return string::f("%s %.2f of %d", what[axis], 1.f + getValue() * (n - 1), n);
}

// =============================================================================
// The screen: WaveStack's 3D view, ported from its Waterfall.svelte and
// camera.svelte.ts. The camera is a yaw about the vertical and a pitch down
// toward the table; world x is phase along each frame, y is level, z is depth.
// A layer is drawn as WaveStack draws a 2D table, columns side by side, rows
// receding with row 1 at the back, painted back to front with each frame
// hanging a curtain that hides what is behind it. Layers stack vertically,
// the one the point is in drawn solid and the rest faint, so the whole volume
// is in view at once. The point is the frame it reads, in orange, and the
// sequence is a path through the volume.
// =============================================================================
struct StrataView {
	float yaw = 0.f, pitch = 0.8f;
	float sx = 1.f, ox = 0.f, oy = 0.f;
	void rotate(float x, float y, float z, float& px, float& py, float& d) const {
		float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
		float x1 = x * cy + z * sy, z1 = -x * sy + z * cy;
		px = x1; py = y * cp + z1 * sp; d = z1 * cp - y * sp;
	}
	void project(float x, float y, float z, float& X, float& Y, float& d) const {
		float px, py; rotate(x, y, z, px, py, d);
		X = ox + px * sx; Y = oy - py * sx;
	}
};

static Strata* strataPreview() {
	static Strata* m = nullptr;
	if (m) return m;
	m = new Strata();
	m->params[Strata::X_PARAM].setValue(0.55f);
	m->params[Strata::Y_PARAM].setValue(0.4f);
	m->addStep(1, 6, 0); m->addStep(5, 2, 0); m->addStep(6, 6, 1); m->addStep(2, 3, 1);
	int64_t frame = 0;
	rack::engine::Module::ProcessArgs a = sfs::previewArgs();
	for (int i = 0; i < 16; i++) { a.frame = frame++; m->process(a); }
	return m;
}

// The table is drawn as a CUBE: columns span x -1..1, rows recede over the
// same 2 in depth, and the layers spread over the same 2 in height. Packed
// tight (one frame height apart) the layers read as one thick slab.
static const float A2 = 0.22f;                   // a frame's half-height, at most
static const float D = ST_CUBE_D;                // the rows' depth

struct StrataDisplay : OpaqueWidget {
	Strata* module = nullptr;
	std::shared_ptr<Font> font;
	Vec pressPos; float dragDist = 0.f; bool pressed = false;
	int dragLis = -1;                                  // the listener being dragged, or -1

	// where a listener is drawn: on the plane for a one-layer table, which is
	// how the mix hears it there
	void lisWorld(int k, float& x, float& y, float& z) const {
		const StrataTable* T = module->table;
		x = module->lis[k][0]; z = module->lis[k][2];
		y = T && T->layers > 1 ? module->lis[k][1] : 0.f;
	}
	int lisAt(Vec p) const {
		if (!module) return -1;
		StrataView V = view();
		for (int k = 1; k >= 0; k--) {
			float x, y, z, X, Y, d; lisWorld(k, x, y, z);
			V.project(x, y, z, X, Y, d);
			if (std::hypot(X - p.x, Y - p.y) < mm2px(2.6f)) return k;
		}
		return -1;
	}


	void frameGeom(const StrataTable& T, float c, float& x0, float& x1) const {
		float w = 2.f / T.cols, gap = 0.18f;
		x0 = -1.f + (c + gap / 2.f) * w; x1 = -1.f + (c + 1.f - gap / 2.f) * w;
	}
	float zRow(const StrataTable& T, float r) const { return T.rows > 1 ? (0.5f - r / (T.rows - 1)) * D : 0.f; }
	float layerGap(const StrataTable& T) const { return T.layers > 1 ? ST_CUBE_H / (T.layers - 1) : 0.f; }
	float yLayer(const StrataTable& T, float l) const { return stWorldY(l, T.layers); }   // layer 1 at the bottom
	// a frame's half-height: A2, shrunk once layers are close enough to collide
	float amp(const StrataTable& T) const { return T.layers > 1 ? std::min(A2, 0.32f * layerGap(T)) : A2; }

	// where the wave pane ends and the table begins
	float split() const { return std::round(box.size.x * 0.36f); }

	// The left pane: the wave each voice plays, large. The blended frame
	// faint behind, and in orange what WARP, SYNC and FOLD make of it over one
	// cycle, so the shape macros can be seen doing what they do. (TILT is a
	// filter and a mip bias, nothing a single cycle's outline shows.)
	void drawWave(NVGcontext* vg, const StrataTable& T, int vi, float bx, float by, float bw, float bh) {
		const Strata::Voice& v0 = module->voice[vi];
		int x0, x1, y0, y1, z0, z1; float wx, wy, wz;
		Strata::axisCells(v0.px, T.cols, Strata::WRAP_CLAMP, x0, x1, wx);
		Strata::axisCells(v0.py, T.rows, Strata::WRAP_CLAMP, y0, y1, wy);
		Strata::axisCells(v0.pz, T.layers, Strata::WRAP_CLAMP, z0, z1, wz);
		auto frame = [&](float q) {
			float s = 0.f;
			for (int c = 0; c < 8; c++) {
				float wgt = ((c & 1) ? wx : 1.f - wx) * ((c & 2) ? wy : 1.f - wy) * ((c & 4) ? wz : 1.f - wz);
				if (wgt <= 0.f) continue;
				s += wgt * Strata::readLevel(T.level(T.index((c & 1) ? x1 : x0, (c & 2) ? y1 : y0, (c & 4) ? z1 : z0), 3), ST_LEN[3], q);
			}
			return s;
		};
		float knee = module->shownKnee, ratio = module->shownRatio, fold = module->shownFold;
		float cy = by + bh / 2.f, amp = bh * 0.42f;
		float thick = bh > mm2px(20.f) ? 1.3f : bh > mm2px(10.f) ? 0.9f : 0.7f;
		// the zero line and the cycle's ends
		nvgBeginPath(vg); nvgMoveTo(vg, bx, cy); nvgLineTo(vg, bx + bw, cy);
		nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_DIM, 0.35f)); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		const int P = 240;
		nvgBeginPath(vg);
		for (int k = 0; k <= P; k++) {
			float t = (float)k / P, X = bx + t * bw, Y = cy - clamp(frame(t), -1.2f, 1.2f) * amp;
			if (k == 0) nvgMoveTo(vg, X, Y); else nvgLineTo(vg, X, Y);
		}
		nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_BLUE, 0.4f)); nvgStrokeWidth(vg, thick * 0.6f); nvgStroke(vg);
		nvgBeginPath(vg);
		for (int k = 0; k <= P; k++) {
			float t = (float)k / P;
			float p = t * ratio; p -= std::floor(p);
			if (k == P && ratio == std::floor(ratio)) p = 0.f;   // a whole ratio ends where it began
			float q = p < knee ? 0.5f * p / knee : 0.5f + 0.5f * (p - knee) / (1.f - knee);
			float s = frame(q);
			if (fold > 0.f) s = stFold(s * stFoldGain(fold));
			float X = bx + t * bw, Y = cy - clamp(s, -1.2f, 1.2f) * amp;
			if (k == 0) nvgMoveTo(vg, X, Y); else nvgLineTo(vg, X, Y);
		}
		nvgStrokeColor(vg, sfs::SCREEN_HOT); nvgStrokeWidth(vg, thick); nvgLineJoin(vg, NVG_ROUND); nvgStroke(vg);
	}
	// Poly: one wave per voice, each from its own point in the table. Two or
	// three stack as full-width rows (a wave reads best wide); from four on
	// they tile a grid, up to 4 x 4.
	void drawWaves(NVGcontext* vg, const StrataTable& T, float bx, float by, float bw, float bh) {
		int n = clamp(module->nVoices, 1, ST_POLY);
		if (n == 1) { drawWave(vg, T, 0, bx, by, bw, bh); return; }
		int cols = n <= 3 ? 1 : (int)std::ceil(std::sqrt((float)n)), rows = (n + cols - 1) / cols;
		float gap = mm2px(1.f), cw = (bw - gap * (cols - 1)) / cols, ch = (bh - gap * (rows - 1)) / rows;
		for (int i = 0; i < n; i++) {
			float cx = bx + (i % cols) * (cw + gap), cy = by + (i / cols) * (ch + gap);
			nvgBeginPath(vg); nvgRoundedRect(vg, cx, cy, cw, ch, mm2px(0.6f));
			nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_DIM, 0.25f)); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
			drawWave(vg, T, i, cx + mm2px(0.8f), cy + mm2px(0.8f), cw - mm2px(1.6f), ch - mm2px(1.6f));
			if (font && font->handle >= 0) {
				sfs::screenFont(vg, font);
				nvgFontSize(vg, std::min(mm2px(2.6f), ch * 0.3f));
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
				nvgFillColor(vg, sfs::SCREEN_DIM);
				nvgText(vg, cx + mm2px(0.8f), cy + mm2px(0.5f), string::f("%d", i + 1).c_str(), NULL);
			}
		}
	}

	StrataView view() const {
		StrataView V;
		V.yaw = module->camYaw; V.pitch = module->camPitch;
		const StrataTable* T = module->table;
		int L = T ? T->layers : 1;
		// a frame can overshoot its peak (a band-limited square rings) and the
		// point is drawn 1.15x tall, so the box is taken a little beyond the
		// frame height; fitted to it exactly, the bottom layer ran off the screen
		float a = T ? amp(*T) : A2;
		float yTop = (T ? yLayer(*T, L - 1) : 0.f) + 1.3f * a, yBot = (T ? yLayer(*T, 0) : 0.f) - 1.3f * a;
		float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
		for (float x : {-1.f, 1.f}) for (float y : {yBot, yTop}) for (float z : {-D / 2.f, D / 2.f}) {
			float px, py, d; V.rotate(x, y, z, px, py, d);
			mnx = std::min(mnx, px); mxx = std::max(mxx, px); mny = std::min(mny, py); mxy = std::max(mxy, py);
		}
		// the right-hand pane, clear of the header and the footer
		float pad = mm2px(2.f), top = mm2px(5.5f), bot = mm2px(5.5f);
		float x0 = split() + pad, w = box.size.x - pad - x0, h = box.size.y - top - bot;
		V.sx = std::min(w / (mxx - mnx), h / (mxy - mny));
		V.ox = x0 + w / 2.f - (mnx + mxx) / 2.f * V.sx;
		V.oy = top + h / 2.f + (mny + mxy) / 2.f * V.sx;
		return V;
	}

	// a frame's polyline: `pts` samples of mip level 3 (512 long)
	void frameLine(const StrataTable& T, int f, int pts, float x0, float x1, float yOff, float z, const StrataView& V, std::vector<float>& out) const {
		out.clear();
		const float* d = T.level(f, 3);
		for (int k = 0; k <= pts; k++) {
			float t = (float)k / pts;
			float v = d[std::min((int)(t * ST_LEN[3]), ST_LEN[3] - 1)];
			float X, Y, dd; V.project(x0 + t * (x1 - x0), yOff + v * amp(T), z, X, Y, dd);
			out.push_back(X); out.push_back(Y);
		}
	}

	void onButton(const ButtonEvent& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
			pressPos = e.pos; dragDist = 0.f; pressed = true;
			dragLis = lisAt(e.pos);                    // a listener first: the frames are under it
			e.consume(this);
		}
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT && e.action == GLFW_PRESS && module) {
			e.consume(this);
			Strata* m = module;
			ui::Menu* menu = createMenu();
			menu->addChild(createMenuLabel(string::f("%d of %d waypoints (CLOCK steps through them)", m->nSteps, ST_MAXSTEPS)));
			menu->addChild(createMenuItem("Remove the last waypoint", "", [=]() { if (m->nSteps > 0) { m->nSteps--; m->cur = 0; } }));
			menu->addChild(createMenuItem("Clear the waypoints", "", [=]() { m->nSteps = 0; m->cur = 0; }));
			menu->addChild(createMenuItem("Reset the listeners", "", [=]() { m->resetListeners(); }));
			menu->addChild(createMenuItem("Reset the view", "", [=]() { m->camYaw = Strata::YAW0; m->camPitch = Strata::PITCH0; }));
		}
	}
	void onDragMove(const DragMoveEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT) return;
		float z = getAbsoluteZoom();
		dragDist += std::hypot(e.mouseDelta.x, e.mouseDelta.y) / z;
		if (dragLis >= 0) {
			// across the floor by default, up and down with shift (3D tables
			// only): the inverse of the camera's yaw and pitch, so the listener
			// follows the pointer from any angle
			StrataView V = view();
			float a = e.mouseDelta.x / z / V.sx, b = -e.mouseDelta.y / z / V.sx;
			float cy = std::cos(V.yaw), sy = std::sin(V.yaw), cp = std::cos(V.pitch), sp = std::sin(V.pitch);
			float* L = module->lis[dragLis];
			bool lift = (APP->window->getMods() & RACK_MOD_MASK) == GLFW_MOD_SHIFT && module->table && module->table->layers > 1;
			if (lift) L[1] = clamp(L[1] + b / std::max(std::fabs(cp), 0.2f), -1.f, 1.f);
			else {
				float bb = b / (std::fabs(sp) < 0.2f ? std::copysign(0.2f, sp) : sp);
				L[0] = clamp(L[0] + a * cy - bb * sy, -1.f, 1.f);
				L[2] = clamp(L[2] + a * sy + bb * cy, -1.f, 1.f);
			}
			return;
		}
		if (dragDist < 3.f) return;
		module->camYaw += e.mouseDelta.x / z * 0.01f;
		module->camPitch = clamp(module->camPitch + e.mouseDelta.y / z * 0.01f, -1.5f, 1.5f);
	}
	// A click without a drag adds the frame under the pointer as the next step.
	void onDragEnd(const DragEndEvent& e) override {
		if (!module || !pressed || e.button != GLFW_MOUSE_BUTTON_LEFT) return;
		pressed = false;
		bool wasLis = dragLis >= 0;
		dragLis = -1;
		if (wasLis || dragDist >= 3.f || !module->table) return;
		const StrataTable& T = *module->table;
		StrataView V = view();
		float best = mm2px(4.f); int bx = -1, by = 0, bz = 0;
		for (int l = 0; l < T.layers; l++) for (int r = 0; r < T.rows; r++) for (int c = 0; c < T.cols; c++) {
			float x0, x1; frameGeom(T, c, x0, x1);
			float X, Y, d; V.project((x0 + x1) / 2.f, yLayer(T, l), zRow(T, r), X, Y, d);
			float dist = std::hypot(X - pressPos.x, Y - pressPos.y);
			if (dist < best) { best = dist; bx = c; by = r; bz = l; }
		}
		if (bx >= 0) module->addStep((float)bx, (float)by, (float)bz);
	}
	void onDoubleClick(const DoubleClickEvent& e) override {
		if (module) { module->camYaw = Strata::YAW0; module->camPitch = Strata::PITCH0; }
		e.consume(this);
	}

	void step() override {
		OpaqueWidget::step();
		if (module) delete module->retired.exchange(nullptr);
	}

	void draw(const DrawArgs& args) override {
		if (!module) { module = strataPreview(); draw(args); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, w, h, mm2px(1.f));
		nvgFillColor(vg, sfs::SCREEN_BG); nvgFill(vg);
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		const StrataTable* Tp = module->table;
		if (!Tp) return;
		const StrataTable& T = *Tp;
		nvgSave(vg); nvgScissor(vg, 0, 0, w, h);
		float sp = split();
		drawWaves(vg, T, mm2px(2.f), mm2px(5.5f), sp - mm2px(4.f), h - mm2px(11.f));
		nvgBeginPath(vg); nvgMoveTo(vg, sp, mm2px(2.f)); nvgLineTo(vg, sp, h - mm2px(2.f));
		nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_DIM, 0.3f)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
		StrataView V = view();
		// the layer the first voice is in is the solid one
		const Strata::Voice& v0 = module->voice[0];
		int curL = clamp((int)std::lround(v0.pz), 0, T.layers - 1);
		int pts = clamp(480 / T.cols, 16, 48);
		if (T.frames > 256) pts = std::min(pts, 20);

		struct Strip { int l, r; float depth; };
		std::vector<Strip> strips;
		for (int l = 0; l < T.layers; l++) for (int r = 0; r < T.rows; r++) {
			float X, Y, d; V.project(0.f, yLayer(T, l), zRow(T, r), X, Y, d);
			strips.push_back({l, r, d});
		}
		std::sort(strips.begin(), strips.end(), [](const Strip& a, const Strip& b) { return a.depth > b.depth; });
		float fd = strips.front().depth, nd = strips.back().depth;
		std::vector<float> line;
		float floorSign = module->camPitch >= 0.f ? -1.f : 1.f;
		for (const Strip& s : strips) {
			float yOff = yLayer(T, s.l), z = zRow(T, s.r);
			bool solid = s.l == curL;
			float near = fd == nd ? 1.f : (fd - s.depth) / (fd - nd);
			for (int c = 0; c < T.cols; c++) {
				float x0, x1; frameGeom(T, (float)c, x0, x1);
				frameLine(T, T.index(c, s.r, s.l), pts, x0, x1, yOff, z, V, line);
				// the curtain, down to this layer's floor
				float fX0, fY0, fX1, fY1, d;
				V.project(x1, yOff + floorSign * amp(T) * 1.05f, z, fX0, fY0, d);
				V.project(x0, yOff + floorSign * amp(T) * 1.05f, z, fX1, fY1, d);
				nvgBeginPath(vg);
				nvgMoveTo(vg, line[0], line[1]);
				for (size_t k = 2; k < line.size(); k += 2) nvgLineTo(vg, line[k], line[k + 1]);
				nvgLineTo(vg, fX0, fY0); nvgLineTo(vg, fX1, fY1); nvgClosePath(vg);
				nvgFillColor(vg, nvgTransRGBAf(sfs::SCREEN_BG, 0.95f)); nvgFill(vg);
				nvgBeginPath(vg);
				nvgMoveTo(vg, line[0], line[1]);
				for (size_t k = 2; k < line.size(); k += 2) nvgLineTo(vg, line[k], line[k + 1]);
				nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_BLUE, solid ? 0.3f + 0.6f * near : 0.12f + 0.12f * near));
				nvgStrokeWidth(vg, solid ? 0.7f : 0.5f); nvgStroke(vg);
			}
		}

		// the cube's edges, faint, so the volume reads as one
		if (T.layers > 1) {
			float a = amp(T), yT = yLayer(T, T.layers - 1) + a, yB = yLayer(T, 0) - a;
			float cx[2] = {-1.f, 1.f}, cy[2] = {yB, yT}, cz[2] = {-D / 2.f, D / 2.f};
			nvgBeginPath(vg);
			for (int e = 0; e < 12; e++) {
				int ax = e / 4, i = e % 4;               // the axis an edge runs along, and which of its four
				float p0[3], p1[3];
				for (int k = 0, b = 0; k < 3; k++) {
					if (k == ax) { p0[k] = 0.f; p1[k] = 1.f; continue; }
					p0[k] = p1[k] = (float)((i >> b++) & 1);
				}
				float X0, Y0, X1, Y1, d;
				V.project(cx[(int)p0[0]], cy[(int)p0[1]], cz[(int)p0[2]], X0, Y0, d);
				V.project(cx[(int)p1[0]], cy[(int)p1[1]], cz[(int)p1[2]], X1, Y1, d);
				nvgMoveTo(vg, X0, Y0); nvgLineTo(vg, X1, Y1);
			}
			nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_DIM, 0.3f)); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
		}

		// the sequence: a path through the volume, a dot on each step, the current one ringed
		if (module->nSteps > 0) {
			auto cellXY = [&](float x, float y, float z, float& X, float& Y) {
				float x0, x1; frameGeom(T, x, x0, x1);
				float d; V.project((x0 + x1) / 2.f, yLayer(T, z), zRow(T, y), X, Y, d);
			};
			nvgBeginPath(vg);
			for (int i = 0; i < module->nSteps; i++) {
				float X, Y; cellXY(module->steps[i].x, module->steps[i].y, module->steps[i].z, X, Y);
				if (i == 0) nvgMoveTo(vg, X, Y); else nvgLineTo(vg, X, Y);
			}
			if (module->nSteps > 2) nvgClosePath(vg);
			nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_TEXT, 0.45f)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			for (int i = 0; i < module->nSteps; i++) {
				float X, Y; cellXY(module->steps[i].x, module->steps[i].y, module->steps[i].z, X, Y);
				nvgBeginPath(vg); nvgCircle(vg, X, Y, mm2px(i == module->cur ? 0.9f : 0.6f));
				nvgFillColor(vg, i == module->cur ? sfs::SCREEN_HOT : sfs::SCREEN_TEXT); nvgFill(vg);
			}
		}

		// the points: the frame each voice reads, blended, in orange
		for (int vi = 0; vi < clamp(module->nVoices, 1, ST_POLY); vi++) {
			const Strata::Voice& v0 = module->voice[vi];
			int x0, x1, y0, y1, z0, z1; float wx, wy, wz;
			Strata::axisCells(v0.px, T.cols, Strata::WRAP_CLAMP, x0, x1, wx);
			Strata::axisCells(v0.py, T.rows, Strata::WRAP_CLAMP, y0, y1, wy);
			Strata::axisCells(v0.pz, T.layers, Strata::WRAP_CLAMP, z0, z1, wz);
			int P = 64;
			float gx0, gx1, hx0, hx1; frameGeom(T, (float)x0, gx0, gx1); frameGeom(T, (float)x1, hx0, hx1);
			float ax0 = gx0 + (hx0 - gx0) * wx, ax1 = gx1 + (hx1 - gx1) * wx;
			float yOff = yLayer(T, z0 + wz), z = zRow(T, y0 + wy);
			nvgBeginPath(vg);
			for (int k = 0; k <= P; k++) {
				float t = (float)k / P, s = 0.f;
				int idx = std::min((int)(t * ST_LEN[3]), ST_LEN[3] - 1);
				for (int c = 0; c < 8; c++) {
					float wgt = ((c & 1) ? wx : 1.f - wx) * ((c & 2) ? wy : 1.f - wy) * ((c & 4) ? wz : 1.f - wz);
					if (wgt <= 0.f) continue;
					s += wgt * T.level(T.index((c & 1) ? x1 : x0, (c & 2) ? y1 : y0, (c & 4) ? z1 : z0), 3)[idx];
				}
				float X, Y, d; V.project(ax0 + t * (ax1 - ax0), yOff + s * amp(T) * 1.15f, z, X, Y, d);
				if (k == 0) nvgMoveTo(vg, X, Y); else nvgLineTo(vg, X, Y);
			}
			nvgStrokeColor(vg, sfs::SCREEN_HOT); nvgStrokeWidth(vg, module->nVoices > 1 ? 0.9f : 1.2f); nvgStroke(vg);
		}

		// the listeners: a ring with its letter, and on a 3D table a line down
		// to the floor so its height can be read
		for (int k = 0; k < 2; k++) {
			float x, y, z, X, Y, d; lisWorld(k, x, y, z);
			V.project(x, y, z, X, Y, d);
			if (T.layers > 1) {
				float FX, FY; V.project(x, yLayer(T, 0) - amp(T), z, FX, FY, d);
				nvgBeginPath(vg); nvgMoveTo(vg, X, Y); nvgLineTo(vg, FX, FY);
				nvgStrokeColor(vg, nvgTransRGBAf(sfs::SCREEN_TEXT, 0.35f)); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
				nvgBeginPath(vg); nvgEllipse(vg, FX, FY, mm2px(0.8f), mm2px(0.35f));
				nvgFillColor(vg, nvgTransRGBAf(sfs::SCREEN_TEXT, 0.35f)); nvgFill(vg);
			}
			float r = mm2px(1.5f);
			nvgBeginPath(vg); nvgCircle(vg, X, Y, r);
			nvgFillColor(vg, nvgTransRGBAf(sfs::SCREEN_BG, 0.85f)); nvgFill(vg);
			nvgStrokeColor(vg, dragLis == k ? sfs::SCREEN_HOT : sfs::SCREEN_TEXT); nvgStrokeWidth(vg, 0.9f); nvgStroke(vg);
			if (font && font->handle >= 0) {
				sfs::screenFont(vg, font);
				nvgFontSize(vg, mm2px(2.2f));
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, sfs::SCREEN_TEXT);
				nvgText(vg, X, Y + mm2px(0.1f), k ? "R" : "L", NULL);
			}
		}

		// the header: what is loaded, and where the point is
		if (font && font->handle >= 0) {
			sfs::screenFont(vg, font);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			std::string head = module->loading ? "loading..." : T.name;
			nvgText(vg, mm2px(1.5f), mm2px(1.f), head.c_str(), NULL);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			std::string dims = string::f("%d x %d x %d", T.cols, T.rows, T.layers);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgText(vg, w - mm2px(1.5f), mm2px(1.f), dims.c_str(), NULL);
			std::string layer = curL < (int)T.layerNames.size() && !T.layerNames[curL].empty() ? T.layerNames[curL] : string::f("layer %d", curL + 1);
			std::string pos = string::f("%s   %.1f, %.1f", layer.c_str(), v0.px + 1.f, v0.py + 1.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
			nvgText(vg, mm2px(1.5f), h - mm2px(1.f), pos.c_str(), NULL);
			// the foot's right: an error, else what the drag or the waypoints are doing.
			// CLOCK steps through waypoints set on this screen, and nothing on
			// the panel says so, so the screen does
			std::string note;
			bool clk = module->inputs[Strata::CLOCK_INPUT].isConnected();
			if (dragLis >= 0) {
				const float* L = module->lis[dragLis];
				note = string::f("%s  x %.2f  depth %.2f", dragLis ? "R" : "L", L[0], L[2]);
				if (T.layers > 1) note += string::f("  height %.2f  (shift: height)", L[1]);
			} else if (module->nSteps == 0 && clk) note = "click frames to set waypoints for CLOCK";
			else if (module->nSteps > 0 && !clk) note = string::f("%d waypoints, waiting for CLOCK", module->nSteps);
			else if (module->nSteps > 0) note = string::f("waypoint %d of %d", module->cur + 1, module->nSteps);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
			if (module->loadError[0]) {
				nvgFillColor(vg, sfs::SCREEN_HOT);
				nvgText(vg, w - mm2px(1.5f), h - mm2px(1.f), module->loadError, NULL);
			} else if (!note.empty()) {
				nvgFillColor(vg, sfs::SCREEN_DIM);
				nvgText(vg, w - mm2px(1.5f), h - mm2px(1.f), note.c_str(), NULL);
			}
		}
		nvgRestore(vg);
	}
};

// =============================================================================
// Panel: 24HP, laid out from code until the designer's art arrives. Ten
// controls over their CVs (FREQ's CV is V/OCT, FM's is the FM input), and the
// transport row at the foot with the outputs on a plate.
// =============================================================================
static const float ST_PX[10] = {9.5f, 20.9f, 32.3f, 43.7f, 55.1f, 66.5f, 77.9f, 89.3f, 100.7f, 112.1f};
static const float ST_PY = 76.f, ST_PCV = 88.f, ST_FY = 113.f;
// the foot: three inputs, then seven outputs at 10.16 mm on the plate
static const float ST_OX[7] = {51.0f, 61.16f, 71.32f, 81.48f, 91.64f, 101.8f, 111.96f};

struct StrataWidget : ModuleWidget {
	StrataWidget(Strata* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/strata.svg")));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title(6.f, 8.f, "STRATA");

		StrataDisplay* disp = new StrataDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(3.0f, 11.0f));
		disp->box.size = mm2px(Vec(115.92f, 56.0f));
		addChild(disp);

		static const char* PN[10] = {"FREQ", "FM", "X", "Y", "Z", "GLIDE", "WARP", "FOLD", "TILT", "SYNC"};
		static const int PP[10] = {Strata::FREQ_PARAM, Strata::FM_PARAM, Strata::X_PARAM, Strata::Y_PARAM, Strata::Z_PARAM,
		                           Strata::GLIDE_PARAM, Strata::WARP_PARAM, Strata::FOLD_PARAM, Strata::TILT_PARAM, Strata::SYNC_PARAM};
		static const int PI[10] = {Strata::VOCT_INPUT, Strata::FM_INPUT, Strata::X_INPUT, Strata::Y_INPUT, Strata::Z_INPUT,
		                           Strata::GLIDE_INPUT, Strata::WARP_INPUT, Strata::FOLD_INPUT, Strata::TILT_INPUT, Strata::SYNC_INPUT};
		for (int i = 0; i < 10; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(ST_PX[i], ST_PY)), module, PP[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(ST_PX[i], ST_PCV)), module, PI[i]));
			lbl->pairDown(ST_PX[i], ST_PY, ST_PCV, PN[i]);
		}
		static const char* FN[3] = {"HARD", "CLOCK", "RESET"};
		static const int FI[3] = {Strata::HARD_INPUT, Strata::CLOCK_INPUT, Strata::RESET_INPUT};
		for (int i = 0; i < 3; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(ST_PX[i], ST_FY)), module, FI[i]));
			lbl->jack(ST_PX[i], ST_FY, FN[i]);
		}
		static const char* ON[7] = {"STEP", "X", "Y", "Z", "OUT", "L", "R"};
		static const int OI[7] = {Strata::STEP_OUTPUT, Strata::X_OUTPUT, Strata::Y_OUTPUT, Strata::Z_OUTPUT, Strata::OUT_OUTPUT,
		                          Strata::LEFT_OUTPUT, Strata::RIGHT_OUTPUT};
		for (int i = 0; i < 7; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(ST_OX[i], ST_FY)), module, OI[i]));
			lbl->jackOnPlate(ST_OX[i], ST_FY, ON[i]);
		}
	}

	void appendContextMenu(Menu* menu) override {
		Strata* m = dynamic_cast<Strata*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Load table...", "", [=]() {
			osdialog_filters* f = osdialog_filters_parse("WAV:wav,WAV");
			char* p = osdialog_file(OSDIALOG_OPEN, NULL, NULL, f);
			osdialog_filters_free(f);
			if (p) { m->load({p}); std::free(p); }
		}));
		menu->addChild(createMenuItem("Load a folder of tables as layers...", "", [=]() {
			char* p = osdialog_file(OSDIALOG_OPEN_DIR, NULL, NULL, NULL);
			if (!p) return;
			std::vector<std::string> files;
			for (const std::string& e : system::getEntries(p))
				if (string::lowercase(system::getExtension(e)) == ".wav") files.push_back(e);
			std::free(p);
			std::sort(files.begin(), files.end());   // name order, bottom layer first
			if (!files.empty()) m->load(files);
		}));
		menu->addChild(createSubmenuItem("Factory tables", "", [=](Menu* sub) {
			sub->addChild(createMenuItem("1D: sixteen shapes in a row", "", [=]() { m->loadFactory(1); }));
			sub->addChild(createMenuItem("2D: harmonics, 8 x 8", "", [=]() { m->loadFactory(2); }));
			sub->addChild(createMenuItem("3D: four 8 x 8 layers", "", [=]() { m->loadFactory(3); }));
		}));
		// a layer on top of the table in force: needs the same grid
		menu->addChild(createSubmenuItem("Add a layer on top", "", [=](Menu* sub) {
			sub->addChild(createMenuItem("From a WAV...", "", [=]() {
				osdialog_filters* f = osdialog_filters_parse("WAV:wav,WAV");
				char* p = osdialog_file(OSDIALOG_OPEN, NULL, NULL, f);
				osdialog_filters_free(f);
				if (p) { m->addLayer(p); std::free(p); }
			}));
			sub->addChild(new MenuSeparator);
			sub->addChild(createMenuLabel("Factory layers (8 x 8)"));
			for (const char* k : {"harmonics", "formant", "sync", "pulse"}) {
				std::string src = std::string("factory:") + k;
				sub->addChild(createMenuItem(Strata::sourceName(src), "", [=]() { m->addLayer(src); }));
			}
			sub->addChild(new MenuSeparator);
			sub->addChild(createMenuItem("Remove the top layer", "", [=]() { m->removeTopLayer(); }, m->sources.size() < 2));
		}));
		menu->addChild(createSubmenuItem("Conform at load", "", [=](Menu* sub) {
			sub->addChild(createBoolPtrMenuItem("Normalize each frame", "", &m->conform.normalize));
			sub->addChild(createBoolPtrMenuItem("Remove DC", "", &m->conform.removeDC));
			sub->addChild(createBoolPtrMenuItem("Align zero crossings", "", &m->conform.align));
			sub->addChild(createBoolPtrMenuItem("Open square 1D tables as grids", "", &m->squareUp));
			sub->addChild(createMenuLabel("Applies to the next load"));
		}));
		menu->addChild(createMenuLabel(string::f("Waypoints: %d (click frames on the screen; CLOCK steps through them)", m->nSteps)));
		menu->addChild(createMenuItem("Remove the last waypoint", "", [=]() { if (m->nSteps > 0) { m->nSteps--; m->cur = 0; } }, m->nSteps == 0));
		menu->addChild(createMenuItem("Clear the waypoints", "", [=]() { m->nSteps = 0; m->cur = 0; }, m->nSteps == 0));
		menu->addChild(createIndexPtrSubmenuItem("Glide", {"Rate (a long move takes longer)", "Time (every move the same)"}, &m->glideMode));
		menu->addChild(createIndexPtrSubmenuItem("Path", {"Straight through the volume", "Along the grid"}, &m->pathMode));
		menu->addChild(createMenuLabel("Edges"));
		menu->addChild(createIndexPtrSubmenuItem("X", {"Clamp", "Wrap"}, &m->wrap[0]));
		menu->addChild(createIndexPtrSubmenuItem("Y", {"Clamp", "Wrap"}, &m->wrap[1]));
		menu->addChild(createIndexPtrSubmenuItem("Z", {"Clamp", "Wrap"}, &m->wrap[2]));
		menu->addChild(createMenuLabel("Stereo"));
		menu->addChild(createMenuItem("Reset the listeners", "", [=]() { m->resetListeners(); }));
	}
};

Model* modelStrata = createModel<Strata, StrataWidget>("Strata");

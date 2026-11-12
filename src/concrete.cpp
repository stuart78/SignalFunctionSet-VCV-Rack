// =============================================================================
// Concrete: four lanes of samples on one tape loop.
//
// The loop is 1-8 bars long and has one playhead. Each lane holds clips: a
// window into a sample, placed at a bar position, played at its own speed. The
// speed is varispeed, as tape is: pitch and length change together, so a clip
// at half speed is an octave down and twice as long on the timeline. A clip
// may instead follow the tempo ("stays N bars long"), when its speed is worked
// out from the bar length every sample.
//
// What plays on a lane at the playhead is the clip that started most recently
// at or before it. A clip therefore runs until its audio ends, the next clip
// on the lane starts, or the loop ends -- and the loop end cuts it off, as the
// end of a tape loop would. Putting a clip down (a drop, a move, a recording)
// replaces what was under it and leaves the rest of whatever it landed on in
// place after it, the way a splice into tape does.
//
// Each lane also carries four drawn curves across the loop: VOL, FILTER (a DJ
// filter, lowpass below the centre, highpass above), PAN and PITCH (varispeed,
// an octave either way).
//
// Tempo: CLOCK and BAR when they are patched (clocks per bar are counted
// between BARs), BAR alone, or the BPM knob. A clock drives the playhead and a
// clock that stops stops the tape. Concrete In, on the left, records into the
// lanes. Each lane has its own stereo output beside the mix.
// Design: docs/concrete-design.md.
// =============================================================================
#include "plugin.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "concrete-messages.hpp"
#include "dr_wav.h"        // implementation lives in phase.cpp; headers only here
#include "dr_flac.h"       // implementation lives in dr_flac.cpp; headers only here
#include <osdialog.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

static const int CC_N = CONCRETE_LANES;
static const int CC_CLIPS = 32;           // clip slots per lane
static const int CC_SMPS = 160;           // sample slots
static const int CC_MAXBARS = 8;
static const int CC_RES = 256;            // curve samples per bar: a step ramps over 1/256 bar
static const int CC_CN = CC_RES * CC_MAXBARS;
enum CcCurve { CC_VOL, CC_FILT, CC_PAN, CC_PITCH, CC_NCURVE };
static const char* CC_CURVE_NAME[CC_NCURVE] = {"volume", "filter", "pan", "pitch"};
static const float CC_CURVE_DEF[CC_NCURVE] = {1.f, 0.f, 0.f, 0.f};
static const float CC_PITCH_SEMIS = 12.f; // the pitch curve's top and bottom, in semitones
static const float CC_FADE = 0.003f;      // s: every clip edge, cut and switch
static const float CC_VOLTS = 5.f;        // a full-scale sample, in volts
static const double CC_MAXSEC = 600.0;    // the longest sample loaded
static const double CC_TAKE_MAXSEC = 120.0;
static const int CC_PEAK = 256;           // frames per screen peak
static const float CC_MINSPEED = 0.125f, CC_MAXSPEED = 8.f;
static const int CC_CPB_OPT[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 96};
static const int CC_NCPB = 12;
static const NVGcolor CC_LANE_COL[CC_N] = {
	nvgRGB(0x00, 0x97, 0xDE),   // 1 blue
	nvgRGB(0x3F, 0xBF, 0x6F),   // 2 green
	nvgRGB(0xEC, 0x65, 0x2E),   // 3 orange
	nvgRGB(0x9B, 0x6B, 0xD6),   // 4 purple
};

// Rack's clamp() has float and int overloads only
template <typename T> static inline T ccClamp(T x, T a, T b) { return x < a ? a : (x > b ? b : x); }

static inline float ccSoftClip(float x) {   // linear to 6 V, asymptotic to 10 V
	float a = std::fabs(x);
	if (a <= 6.f) return x;
	return std::copysign(6.f + 4.f * (1.f - 4.f / (4.f + a - 6.f)), x);
}

// ── a sample: stereo frames, full scale 1 ────────────────────────────────────
struct ConcreteSample {
	std::vector<float> d;        // interleaved L R
	int64_t n = 0;
	float sr = 48000.f;
	std::string name, path;      // path: where it was loaded from ("" for a take)
	std::string stored;          // its copy in the patch storage ("" if none)
	bool take = false, missing = false, written = false;
	int uid = 0;
	std::vector<float> peak;     // max |x| per CC_PEAK frames
	void computePeaks() {
		int64_t nb = (n + CC_PEAK - 1) / CC_PEAK;
		peak.assign((size_t)nb, 0.f);
		for (int64_t i = 0; i < n; i++) {
			float a = std::max(std::fabs(d[2 * i]), std::fabs(d[2 * i + 1]));
			float& p = peak[(size_t)(i / CC_PEAK)];
			if (a > p) p = a;
		}
	}
};

static ConcreteSample* ccFromInterleaved(const float* raw, int64_t frames, unsigned ch, float sr) {
	if (!raw || frames <= 0 || ch == 0) return nullptr;
	frames = std::min(frames, (int64_t)(CC_MAXSEC * sr));
	ConcreteSample* s = new ConcreteSample();
	s->n = frames; s->sr = sr;
	s->d.resize((size_t)(2 * frames));
	for (int64_t i = 0; i < frames; i++) {
		float l = raw[i * ch], r = ch > 1 ? raw[i * ch + 1] : l;
		s->d[2 * i] = l; s->d[2 * i + 1] = r;
	}
	s->computePeaks();
	return s;
}

static ConcreteSample* ccLoadFile(const std::string& path) {
	std::string lp = string::lowercase(path);
	unsigned ch = 0, sr = 0;
	ConcreteSample* s = nullptr;
	if (string::endsWith(lp, ".flac")) {
		drflac_uint64 n = 0;
		float* p = drflac_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &n, NULL);
		if (!p) return nullptr;
		s = ccFromInterleaved(p, (int64_t)n, ch, (float)sr);
		drflac_free(p, NULL);
	} else {
		drwav_uint64 n = 0;
		float* p = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &n, NULL);
		if (!p) return nullptr;
		s = ccFromInterleaved(p, (int64_t)n, ch, (float)sr);
		drwav_free(p, NULL);
	}
	if (s) { s->path = path; s->name = system::getStem(path); }
	return s;
}

static bool ccIsAudio(const std::string& p) {
	std::string l = string::lowercase(p);
	return string::endsWith(l, ".wav") || string::endsWith(l, ".flac");
}

// ── a clip: a window into a sample, placed on a lane ────────────────────────
struct ConcreteClip {
	bool used = false;
	int smp = -1;
	float t0 = 0.f;              // bars
	double a = 0.0, b = 0.0;     // the window, frames
	float speed = 1.f;
	bool rev = false;
	float fit = 0.f;             // > 0: the clip stays this many bars long (speed follows the tempo)
	uint32_t gen = 0;            // changes whenever the clip's mapping does
};

// ── a recording buffer. Two per lane: at a loop end the take that has just
// finished waits for the UI thread while the next one is already recording.
enum { TK_FREE, TK_READY, TK_REC, TK_SEALED };
struct ConcreteTake {
	std::vector<float> buf;
	int64_t cap = 0;             // frames
	std::atomic<int64_t> n{0};
	std::atomic<int> state{TK_FREE};
	float t0 = 0.f, t1 = 0.f;    // bars
	float sr = 48000.f;
	uint32_t gen = 0;
	double freedAt = -1e9;       // UI clock
};

// ── what a lane is sounding ─────────────────────────────────────────────────
struct CcVoice {
	int kind = 0;                // 0 nothing, 1 clip, 2 finished take, 3 the input (recording)
	int idx = -1;
	uint32_t gen = 0, seek = 0;
	const float* d = nullptr;
	int64_t n = 0;
	double pos = 0.0, a = 0.0, b = 0.0, inc = 0.0;
	double dur1 = 0.0;           // seconds of the window at 1x
	float fit = 0.f, srS = 48000.f;
	bool rev = false, fin = false;
	float env = 0.f;             // fade in
	float out = 1.f;             // fade out, once it has been replaced
};

// Phase's DJ filter (a TPT state-variable filter: crossing the centre only
// swaps which output is read), with the coefficients shared by L and R.
struct CcFilter {
	float a1 = 0.f, a2 = 0.f, a3 = 0.f, k = 1.414f, pos = 0.f, wet = 0.f;
	float ic1[2] = {}, ic2[2] = {};
	void set(float p, float sr) {
		pos = p;
		wet = std::min(std::fabs(p) / 0.05f, 1.f);
		float f = p < 0.f ? 20000.f * std::pow(60.f / 20000.f, -p) : 20.f * std::pow(8000.f / 20.f, p);
		f = std::min(f, sr * 0.45f);
		float g = std::tan((float)M_PI * f / sr);
		k = 1.f / (0.707f + 0.6f * std::fabs(p));
		a1 = 1.f / (1.f + g * (g + k));
		a2 = g * a1;
		a3 = g * a2;
	}
	float proc(int c, float x) {
		float v3 = x - ic2[c];
		float v1 = a1 * ic1[c] + a2 * v3;
		float v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
		ic1[c] = 2.f * v1 - ic1[c];
		ic2[c] = 2.f * v2 - ic2[c];
		float y = pos < 0.f ? v2 : x - k * v1 - v2;
		return x + (y - x) * wet;
	}
};

static inline float ccHermite(float y0, float y1, float y2, float y3, float f) {
	float c1 = 0.5f * (y2 - y0);
	float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
	float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
	return ((c3 * f + c2) * f + c1) * f + y1;
}

struct Concrete : Module {
	enum ParamId { BPM_PARAM, LENGTH_PARAM, PLAY_PARAM, RESET_PARAM, PARAMS_LEN };
	enum InputId { CLOCK_INPUT, BAR_INPUT, PLAY_INPUT, RESET_INPUT, INPUTS_LEN };
	// the lane outputs follow the mix, so a patch saved before they existed keeps its mix cables
	enum OutputId { L_OUTPUT, R_OUTPUT, ENUMS(LANE_L_OUTPUT, CC_N), ENUMS(LANE_R_OUTPUT, CC_N), OUTPUTS_LEN };
	enum LightId { PLAY_LIGHT, LIGHTS_LEN };

	// ── the tape ────────────────────────────────────────────────────────
	// Clips and curves are written by the UI thread only; the audio thread
	// reads them. Samples are published by pointer and freed a second after
	// nothing refers to them, so a voice fading out never reads freed memory.
	ConcreteClip clip[CC_N][CC_CLIPS];
	std::atomic<ConcreteSample*> smp[CC_SMPS];
	double unrefSince[CC_SMPS];
	std::vector<std::pair<ConcreteSample*, double>> grave;
	float curve[CC_N][CC_NCURVE][CC_CN];      // what the audio thread reads, rendered from the points
	struct CcPt { float b, v; };              // a curve point: bar, value
	std::vector<CcPt> pts[CC_N][CC_NCURVE];   // UI thread only; sorted by bar
	ConcreteTake take[CC_N][2];
	uint32_t genCounter = 1;

	// ── the transport (audio thread; the screen reads it) ─────────────
	double base = 0.0, frac = 0.0, P = 0.0;   // bars: where the last edge put us, and since
	int barIdx = 0, clk = 0;
	int cpb = 16, cpbMeasured = 0, clkCount = 0;
	double spb = 2.0, spbSm = 2.0;            // seconds per bar: measured, slewed
	double clkPer = 0.0, barPer = 0.0;        // samples
	int64_t now = 0, lastClk = -1, lastBar = -1;
	uint32_t seekGen = 0;
	bool running = false, moving = false;
	int srcMode = 0;                          // 0 BPM knob, 1 CLOCK, 2 BAR
	int L = 4;
	float sr = 48000.f;
	std::atomic<float> seekReq{-1.f};

	CcVoice voice[CC_N], ghost[CC_N][2];
	float cs[CC_N][CC_NCURVE];
	CcFilter filt[CC_N];
	float panSet[CC_N], panL[CC_N], panR[CC_N];
	float pitchSet[CC_N] = {};
	double laneRate[CC_N] = {1.0, 1.0, 1.0, 1.0};  // the pitch curve, as a reading speed
	int ctlDiv = 0;
	int recK[CC_N] = {-1, -1, -1, -1};
	bool recNow[CC_N] = {};
	float laneL[CC_N] = {}, laneR[CC_N] = {};
	std::atomic<bool> inAttached{false};
	const ConcreteInMessage* inOverride = nullptr;    // a harness feeds Concrete In's message here

	dsp::SchmittTrigger clkTrig, barTrig, playTrig, rstTrig;
	dsp::BooleanTrigger rstBtn;

	// menu, saved
	bool snap = true, embed = false, resetOnPlay = false;
	// the screen's view: viewLen bars from viewStart, 0 = the whole loop
	float viewStart = 0.f, viewLen = 0.f;
	bool viewFollow = true;                   // a zoomed view pages with the playhead
	int cpbIdx = 7;                           // clocks per bar with BAR unpatched: 16
	int tab = 0;                              // screen: 0 clips, 1 VOL, 2 FILTER, 3 PAN, 4 PITCH
	int uid = 0, takeNo = 0;

	Concrete() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(BPM_PARAM, 30.f, 300.f, 120.f, "Tempo with CLOCK and BAR unpatched", " BPM");
		configParam(LENGTH_PARAM, 1.f, (float)CC_MAXBARS, 4.f, "Loop length", " bars");
		paramQuantities[LENGTH_PARAM]->snapEnabled = true;
		configSwitch(PLAY_PARAM, 0.f, 1.f, 0.f, "Play", {"Stopped", "Playing"});
		configButton(RESET_PARAM, "Reset to bar 1");
		configInput(CLOCK_INPUT, "Clock (drives the playhead; clocks per bar are counted between BARs)");
		configInput(BAR_INPUT, "Bar (moves the playhead to the nearest bar line)");
		configInput(PLAY_INPUT, "Play gate (high plays; overrides the button)");
		configInput(RESET_INPUT, "Reset");
		configOutput(L_OUTPUT, "Left mix");
		configOutput(R_OUTPUT, "Right mix");
		for (int l = 0; l < CC_N; l++) {
			configOutput(LANE_L_OUTPUT + l, string::f("Lane %d left", l + 1));
			configOutput(LANE_R_OUTPUT + l, string::f("Lane %d right", l + 1));
		}
		for (int i = 0; i < CC_SMPS; i++) { smp[i].store(nullptr); unrefSince[i] = 0.0; }
		for (int l = 0; l < CC_N; l++) { panSet[l] = 2.f; panL[l] = panR[l] = 1.f; }
		resetCurves();
		leftExpander.producerMessage = new ConcreteBackMessage();
		leftExpander.consumerMessage = new ConcreteBackMessage();
	}
	~Concrete() override {
		for (int i = 0; i < CC_SMPS; i++) delete smp[i].load();
		for (auto& g : grave) delete g.first;
		delete (ConcreteBackMessage*)leftExpander.producerMessage;
		delete (ConcreteBackMessage*)leftExpander.consumerMessage;
	}

	void resetCurves() {
		for (int l = 0; l < CC_N; l++)
			for (int c = 0; c < CC_NCURVE; c++) {
				pts[l][c].clear();
				renderCurve(l, c);
				cs[l][c] = CC_CURVE_DEF[c];
			}
	}
	// The curve is straight lines between its points, holding the first
	// point's value before it and the last's after it; no points is the
	// default. Two points on one bar make a step.
	void renderCurve(int l, int c) {
		const std::vector<CcPt>& P = pts[l][c];
		for (int i = 0; i < CC_CN; i++) {
			float b = (float)i / CC_RES, v;
			if (P.empty()) v = CC_CURVE_DEF[c];
			else if (b <= P.front().b) v = P.front().v;
			else if (b >= P.back().b) v = P.back().v;
			else {
				size_t k = 1;
				while (k < P.size() && P[k].b < b) k++;
				k = std::min(k, P.size() - 1);   // b is before the last point, so this never moves it
				const CcPt& p0 = P[k - 1];
				const CcPt& p1 = P[k];
				float d = p1.b - p0.b;
				v = d > 1e-6f ? p0.v + (p1.v - p0.v) * (b - p0.b) / d : p1.v;
			}
			curve[l][c][i] = v;
		}
	}

	// A bar holds two points at most, the value arriving and the value leaving.
	// Any between them have no effect on the curve, so they go.
	void pruneCurve(int l, int c) {
		std::vector<CcPt>& P = pts[l][c];
		for (size_t k = 1; k + 1 < P.size();) {
			if (P[k].b == P[k - 1].b && P[k].b == P[k + 1].b) P.erase(P.begin() + k);
			else k++;
		}
	}

	// ── clip arithmetic, shared by the audio thread and the screen ─────
	ConcreteSample* sampleOf(const ConcreteClip& c) const {
		return (c.smp >= 0 && c.smp < CC_SMPS) ? smp[c.smp].load(std::memory_order_acquire) : nullptr;
	}
	// the speed the clip plays at: its own, or the one that keeps it `fit` bars long
	double clipSpeed(const ConcreteClip& c, const ConcreteSample* s, double barSec) const {
		if (c.fit > 0.f && s && s->sr > 0.f && barSec > 0.0)
			return clamp((float)(((c.b - c.a) / s->sr) / (c.fit * barSec)), CC_MINSPEED, CC_MAXSPEED);
		return c.speed;
	}
	double clipBars(const ConcreteClip& c, double barSec) const {
		const ConcreteSample* s = sampleOf(c);
		if (!s || s->sr <= 0.f || barSec <= 0.0) return 0.0;
		return (c.b - c.a) / s->sr / clipSpeed(c, s, barSec) / barSec;
	}
	// where the clip stops sounding: its audio ends, the next clip starts, or the loop ends
	double clipEnd(int l, int j, double barSec, int loop) const {
		const ConcreteClip& c = clip[l][j];
		double e = std::min((double)loop, c.t0 + clipBars(c, barSec));
		for (int k = 0; k < CC_CLIPS; k++) {
			const ConcreteClip& o = clip[l][k];
			if (k == j || !o.used) continue;
			if (o.t0 > c.t0 || (o.t0 == c.t0 && k > j)) e = std::min(e, (double)o.t0);
		}
		return e;
	}

	// ── the transport ───────────────────────────────────────────────────
	// The first clock after a reset IS bar 1: it lands the playhead on the
	// downbeat rather than one clock past it.
	bool firstClk = true;
	void doReset() {
		base = frac = P = 0.0;
		barIdx = clk = 0;
		firstClk = true;
		seekGen++;
	}

	void onReset() override {
		clearAll();
		resetCurves();
		tab = 0;
		viewStart = viewLen = 0.f;
		doReset();
	}

	// the clip that should be sounding on lane l at the playhead
	int pickClip(int l) const {
		int best = -1;
		float bt = -1.f;
		for (int j = 0; j < CC_CLIPS; j++) {
			const ConcreteClip& c = clip[l][j];
			if (!c.used || c.t0 >= (float)L || c.t0 > (float)P + 1e-6f) continue;
			if (c.t0 > bt) { bt = c.t0; best = j; }
		}
		return best;
	}

	// Bars of tape read between bars t0 and t1, with the lane's pitch curve on
	// the reading speed: a voice started part way through a clip is where it would
	// have been had it played from the start.
	double pitchedBars(int l, double t0, double t1) const {
		if (t1 <= t0) return t1 - t0;
		const float* cv = curve[l][CC_PITCH];
		int ia = ccClamp((int)std::floor(t0 * CC_RES), 0, CC_CN - 1), ib = ccClamp((int)std::ceil(t1 * CC_RES), 0, CC_CN - 1);
		bool flat = true;
		for (int i = ia; i <= ib && flat; i++) flat = cv[i] == 0.f;
		if (flat) return t1 - t0;
		double sum = 0.0;
		for (int i = ia; i < ib; i++) {
			double a = std::max(t0, (double)i / CC_RES), b = std::min(t1, (double)(i + 1) / CC_RES);
			if (b <= a) continue;
			double f = (0.5 * (a + b) - (double)i / CC_RES) * CC_RES;
			double v = cv[i] + (cv[i + 1] - cv[i]) * f;
			sum += (b - a) * std::pow(2.0, v * CC_PITCH_SEMIS / 12.0);
		}
		return sum;
	}

	void startVoice(CcVoice& v, int l, int kind, int idx, uint32_t g) {
		v = CcVoice();
		v.kind = kind; v.idx = idx; v.gen = g; v.seek = seekGen;
		if (kind == 1) {
			const ConcreteClip& c = clip[l][idx];
			const ConcreteSample* s = sampleOf(c);
			if (!s || s->n < 2) { v.fin = true; return; }
			v.d = s->d.data(); v.n = s->n; v.srS = s->sr; v.rev = c.rev; v.fit = c.fit;
			v.a = ccClamp(c.a, 0.0, (double)(s->n - 1));
			v.b = ccClamp(c.b, v.a + 1.0, (double)s->n);
			v.dur1 = (v.b - v.a) / v.srS;
			double sp = clipSpeed(c, s, spbSm);
			double adv = pitchedBars(l, c.t0, P) * spbSm * v.srS * sp;
			if (adv < 0.0) adv = 0.0;
			if (adv >= v.b - v.a) { v.fin = true; return; }
			v.pos = v.rev ? v.b - adv : v.a + adv;
			v.inc = sp * v.srS / sr * (v.rev ? -1.0 : 1.0);
		} else if (kind == 2) {
			ConcreteTake& T = take[l][idx];
			int64_t n = T.n.load(std::memory_order_acquire);
			if (n < 2) { v.fin = true; return; }
			v.d = T.buf.data(); v.n = n; v.srS = T.sr;
			v.a = 0.0; v.b = (double)n;
			double adv = pitchedBars(l, T.t0, P) * spbSm * v.srS;
			if (adv < 0.0) adv = 0.0;
			if (adv >= v.b) { v.fin = true; return; }
			v.pos = adv;
			v.inc = v.srS / sr;
		}
	}

	// one sample of a voice, added into (xl, xr)
	void renderVoice(CcVoice& v, const ConcreteInMessage* in, int l, float fadeStep, float fadeN, float& xl, float& xr) {
		if (v.kind == 0 || v.fin) return;
		v.env = std::min(1.f, v.env + fadeStep);
		if (v.kind == 3) {
			if (!in) return;
			float g = v.env * v.out;
			xl += in->l[l] / CC_VOLTS * g;
			xr += in->r[l] / CC_VOLTS * g;
			return;
		}
		if (v.fit > 0.f && spbSm > 0.0) {
			double sp = clamp((float)(v.dur1 / (v.fit * spbSm)), CC_MINSPEED, CC_MAXSPEED);
			v.inc = sp * v.srS / sr * (v.rev ? -1.0 : 1.0);
		}
		double inc = v.inc * laneRate[l];
		double rem = (v.rev ? v.pos - v.a : v.b - v.pos) / std::fabs(inc);
		if (rem <= 0.0) { v.fin = true; return; }
		float gEnd = (float)std::min(1.0, rem / fadeN);
		float g = v.env * v.out * gEnd;
		int64_t i = (int64_t)std::floor(v.pos);
		float f = (float)(v.pos - (double)i);
		int64_t hi = v.n - 1;
		int64_t i0 = ccClamp(i - 1, (int64_t)0, hi), i1 = ccClamp(i, (int64_t)0, hi);
		int64_t i2 = ccClamp(i + 1, (int64_t)0, hi), i3 = ccClamp(i + 2, (int64_t)0, hi);
		const float* d = v.d;
		xl += g * ccHermite(d[2 * i0], d[2 * i1], d[2 * i2], d[2 * i3], f);
		xr += g * ccHermite(d[2 * i0 + 1], d[2 * i1 + 1], d[2 * i2 + 1], d[2 * i3 + 1], f);
		v.pos += inc;
	}

	void seal(int l, int k, float t1) {
		ConcreteTake& T = take[l][k];
		T.t1 = std::max(t1, T.t0);
		T.state.store(TK_SEALED, std::memory_order_release);
	}

	void process(const ProcessArgs& args) override {
		sr = args.sampleRate;
		float dt = args.sampleTime;
		now++;
		L = clamp((int)std::round(params[LENGTH_PARAM].getValue()), 1, CC_MAXBARS);

		// ── play, reset, seek ──
		bool run;
		if (inputs[PLAY_INPUT].isConnected()) { playTrig.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 1.f); run = playTrig.isHigh(); }
		else run = params[PLAY_PARAM].getValue() > 0.5f;
		if (run && !running && resetOnPlay) doReset();
		running = run;
		bool rIn = rstTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		bool rBt = rstBtn.process(params[RESET_PARAM].getValue() > 0.5f);
		double Pprev = P;
		if (rIn || rBt) doReset();
		float sk = seekReq.exchange(-1.f);
		if (sk >= 0.f) {
			barIdx = clamp((int)std::floor(sk), 0, L - 1);
			clk = clamp((int)std::round((sk - barIdx) * cpb), 0, cpb - 1);
			base = barIdx + (double)clk / cpb; frac = 0.0; P = base;
			seekGen++;
		}

		// ── tempo ──
		bool hasClk = inputs[CLOCK_INPUT].isConnected(), hasBar = inputs[BAR_INPUT].isConnected();
		bool clkRise = clkTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		bool barRise = barTrig.process(inputs[BAR_INPUT].getVoltage(), 0.1f, 1.f);
		bool barHigh = barTrig.isHigh();
		srcMode = hasClk ? 1 : hasBar ? 2 : 0;
		if (!hasClk) { lastClk = -1; clkPer = 0.0; clkCount = 0; }
		if (!hasBar) { lastBar = -1; barPer = 0.0; cpbMeasured = 0; }
		if (clkRise) {
			if (lastClk >= 0) clkPer = (double)(now - lastClk);
			lastClk = now;
			clkCount++;
		}
		if (barRise) {
			if (lastBar >= 0) {
				barPer = (double)(now - lastBar);
				if (hasClk && clkCount > 0) cpbMeasured = clamp(clkCount, 1, 96);
			}
			lastBar = now;
			clkCount = 0;
		}
		cpb = hasClk ? (cpbMeasured > 0 ? cpbMeasured : CC_CPB_OPT[clamp(cpbIdx, 0, CC_NCPB - 1)]) : 16;
		double knobSpb = 240.0 / clamp(params[BPM_PARAM].getValue(), 30.f, 300.f);
		double target = knobSpb;
		if (hasClk && clkPer > 0.0) target = clkPer * cpb / sr;
		else if (!hasClk && hasBar && barPer > 0.0) target = barPer / sr;
		spb = ccClamp(target, 0.25, 60.0);
		if (std::fabs(spb - spbSm) > 0.25 * spbSm) spbSm = spb;
		else spbSm += (spb - spbSm) * std::min(1.0, (double)dt / 0.05);

		// a clock that stops stops the tape
		bool stalled = false;
		if (hasClk) stalled = lastClk < 0 || (double)(now - lastClk) > (clkPer > 0.0 ? 1.5 * clkPer : 2.0 * sr) + 0.002 * sr;
		else if (hasBar) stalled = lastBar < 0 || (double)(now - lastBar) > (barPer > 0.0 ? 1.5 * barPer : 8.0 * sr);

		// ── the playhead ──
		if (running) {
			if (hasBar && barRise) {
				// BAR moves the playhead to the nearest bar line
				int r = (int)std::floor(base + frac + 0.5);
				if (r >= L) r = 0;
				barIdx = r; clk = 0; base = r; frac = 0.0;
			}
			if (hasClk && clkRise) {
				// a clock on a BAR is the bar's own downbeat, already placed
				if (firstClk || barRise || barHigh) firstClk = false;
				else {
					if (++clk >= cpb) { clk -= cpb; barIdx++; }
					if (barIdx >= L) barIdx %= L;
				}
				base = barIdx + (double)clk / cpb; frac = 0.0;
			}
			if (!stalled) {
				frac += dt / spbSm;
				if (hasClk) frac = std::min(frac, 1.0 / cpb - 1e-9);
				else if (hasBar) frac = std::min(frac, 1.0 - 1e-9);
				else { base += frac; frac = 0.0; }
			}
			double pos = base + frac;
			if (pos >= L) {
				double w = std::floor(pos / L) * L;
				base -= w;
				barIdx = (int)std::floor(base);
				if (!hasClk) clk = 0;
			}
			if (barIdx >= L || barIdx < 0) barIdx = ((barIdx % L) + L) % L;
			P = base + frac;
		}
		moving = running && !stalled;
		bool jumped = P < Pprev - 1e-9;
		if (jumped) seekGen++;

		// ── recording ──
		const ConcreteInMessage* in = inOverride;
		if (!in && leftExpander.module && leftExpander.module->model == modelConcreteIn)
			in = (const ConcreteInMessage*)leftExpander.module->rightExpander.consumerMessage;
		inAttached.store(in != nullptr, std::memory_order_relaxed);
		for (int l = 0; l < CC_N; l++) {
			bool want = in && moving && in->rec[l] && in->on[l];
			int k = recK[l];
			if (k >= 0) {
				ConcreteTake& T = take[l][k];
				if (jumped) seal(l, k, (float)std::min((double)L, Pprev + dt / spbSm));
				else if (!want || T.n.load(std::memory_order_relaxed) >= T.cap) seal(l, k, (float)P);
				if (T.state.load(std::memory_order_relaxed) == TK_SEALED) k = -1;
			}
			if (k < 0 && want) {
				for (int kk = 0; kk < 2 && k < 0; kk++) {
					ConcreteTake& T = take[l][kk];
					int e = TK_READY;
					if (T.cap > 0 && T.state.compare_exchange_strong(e, TK_REC)) {
						T.t0 = (float)P; T.t1 = (float)P; T.sr = sr; T.gen = ++genRec;
						T.n.store(0, std::memory_order_relaxed);
						k = kk;
					}
				}
			}
			if (k >= 0) {
				ConcreteTake& T = take[l][k];
				int64_t n = T.n.load(std::memory_order_relaxed);
				if (n < T.cap) {
					T.buf[2 * n] = in->l[l] / CC_VOLTS;
					T.buf[2 * n + 1] = in->r[l] / CC_VOLTS;
					T.n.store(n + 1, std::memory_order_release);
					T.t1 = (float)P;
				}
			}
			recK[l] = k;
			recNow[l] = k >= 0;
		}

		// ── the lanes ──
		float fadeN = CC_FADE * sr, fadeStep = 1.f / fadeN;
		float slew = std::min(1.f, dt / 0.005f);
		bool ctl = (ctlDiv++ & 7) == 0;
		float sumL = 0.f, sumR = 0.f;
		double ci = P * CC_RES;
		int i0 = clamp((int)ci, 0, CC_CN - 1), i1 = std::min(i0 + 1, CC_CN - 1);
		float cf = (float)(ci - (double)i0);
		for (int l = 0; l < CC_N; l++) {
			int kind = 0, idx = -1;
			uint32_t g = 0;
			if (moving) {
				if (recK[l] >= 0) kind = 3;
				else {
					for (int k = 0; k < 2 && !kind; k++) {
						ConcreteTake& T = take[l][k];
						if (T.state.load(std::memory_order_acquire) == TK_SEALED && P >= T.t0 && P < T.t1) {
							kind = 2; idx = k; g = T.gen;
						}
					}
					if (!kind) {
						int j = pickClip(l);
						if (j >= 0) { kind = 1; idx = j; g = clip[l][j].gen; }
					}
				}
			}
			CcVoice& v = voice[l];
			if (kind != v.kind || idx != v.idx || g != v.gen || seekGen != v.seek) {
				if (v.kind != 0 && !v.fin) {
					// fade the old one out in whichever ghost is free, or the quieter
					auto busy = [](const CcVoice& q) { return q.kind != 0 && !q.fin; };
					int q = !busy(ghost[l][0]) ? 0 : !busy(ghost[l][1]) ? 1 : ghost[l][0].out < ghost[l][1].out ? 0 : 1;
					ghost[l][q] = v;
				}
				startVoice(v, l, kind, idx, g);
			}
			float xl = 0.f, xr = 0.f;
			renderVoice(v, in, l, fadeStep, fadeN, xl, xr);
			for (int q = 0; q < 2; q++) {
				CcVoice& gh = ghost[l][q];
				if (gh.kind == 0 || gh.fin) continue;
				gh.out -= fadeStep;
				if (gh.out <= 0.f) { gh.kind = 0; continue; }
				renderVoice(gh, in, l, fadeStep, fadeN, xl, xr);
			}

			// the curves
			for (int c = 0; c < CC_NCURVE; c++) {
				float t = curve[l][c][i0] + (curve[l][c][i1] - curve[l][c][i0]) * cf;
				cs[l][c] += (t - cs[l][c]) * slew;
			}
			float vol = cs[l][CC_VOL] * cs[l][CC_VOL];
			if (ctl && std::fabs(cs[l][CC_FILT] - filt[l].pos) > 1e-4f) filt[l].set(cs[l][CC_FILT], sr);
			if (ctl && cs[l][CC_PITCH] != pitchSet[l]) {
				pitchSet[l] = cs[l][CC_PITCH];
				laneRate[l] = std::pow(2.0, pitchSet[l] * CC_PITCH_SEMIS / 12.0);
			}
			float p = cs[l][CC_PAN];
			if (ctl && std::fabs(p - panSet[l]) > 1e-4f) {
				panSet[l] = p;
				panL[l] = p > 0.f ? std::cos(p * (float)M_PI * 0.5f) : 1.f;
				panR[l] = p < 0.f ? std::cos(-p * (float)M_PI * 0.5f) : 1.f;
			}
			xl *= vol * CC_VOLTS; xr *= vol * CC_VOLTS;
			if (filt[l].wet > 0.f) { xl = filt[l].proc(0, xl); xr = filt[l].proc(1, xr); }
			else { filt[l].ic1[0] = filt[l].ic1[1] = filt[l].ic2[0] = filt[l].ic2[1] = 0.f; }
			xl *= panL[l]; xr *= panR[l];
			laneL[l] = xl; laneR[l] = xr;
			outputs[LANE_L_OUTPUT + l].setVoltage(xl);
			outputs[LANE_R_OUTPUT + l].setVoltage(xr);
			sumL += xl; sumR += xr;
		}
		outputs[L_OUTPUT].setVoltage(ccSoftClip(sumL));
		outputs[R_OUTPUT].setVoltage(ccSoftClip(sumR));
		lights[PLAY_LIGHT].setBrightness(running ? 1.f : 0.f);

		// ── the expander ──
		if (leftExpander.module && leftExpander.module->model == modelConcreteIn) {
			auto* m = (ConcreteBackMessage*)leftExpander.producerMessage;
			for (int l = 0; l < CC_N; l++) m->recording[l] = recNow[l];
			m->running = moving;
			leftExpander.requestMessageFlip();
		}
	}
	uint32_t genRec = 0;

	// =====================================================================
	// The UI thread: placing and editing clips, finishing takes, keeping the
	// recording buffers sized, freeing what nothing refers to. Called every
	// frame from the widget.
	// =====================================================================
	int publish(ConcreteSample* s) {
		for (int i = 0; i < CC_SMPS; i++) {
			if (smp[i].load() == nullptr) {
				unrefSince[i] = 0.0;
				smp[i].store(s, std::memory_order_release);
				return i;
			}
		}
		return -1;
	}
	int freeClip(int l) const {
		for (int j = 0; j < CC_CLIPS; j++) if (!clip[l][j].used) return j;
		return -1;
	}
	void touch(ConcreteClip& c) { c.gen = ++genCounter; }

	// move the clip's start later by d bars, keeping its speed
	void trimFront(ConcreteClip& c, double d) {
		const ConcreteSample* s = sampleOf(c);
		if (!s) return;
		double f = d * spbSm * s->sr * clipSpeed(c, s, spbSm);
		if (c.rev) c.b -= f; else c.a += f;
		if (c.fit > 0.f) c.fit = std::max(0.01f, c.fit - (float)d);
		c.t0 += (float)d;
	}

	// Put clip `j` down on lane l: what it covers is replaced, and anything it
	// landed in the middle of carries on after it.
	void splice(int l, int j) {
		// the new clip's own end, ignoring what it is about to replace
		const ConcreteClip& nc = clip[l][j];
		double t0 = nc.t0, t1 = t0 + clipBars(nc, spbSm);
		const double eps = 1e-5;
		for (int k = 0; k < CC_CLIPS; k++) {
			ConcreteClip& c = clip[l][k];
			if (k == j || !c.used) continue;
			// where c stops sounding, not counting the new clip as its successor
			double cs0 = c.t0;
			double ce = std::min((double)CC_MAXBARS, cs0 + clipBars(c, spbSm));
			for (int o = 0; o < CC_CLIPS; o++) {
				const ConcreteClip& oc = clip[l][o];
				if (o == k || o == j || !oc.used) continue;
				if (oc.t0 > cs0 || (oc.t0 == cs0 && o > k)) ce = std::min(ce, (double)oc.t0);
			}
			if (ce <= t0 + eps || cs0 >= t1 - eps) continue;
			bool startsIn = cs0 >= t0 - eps, endsOut = ce > t1 + eps;
			if (startsIn && !endsOut) { c.used = false; continue; }
			if (startsIn) { trimFront(c, t1 - cs0); touch(c); continue; }
			if (endsOut) {
				int f = freeClip(l);
				if (f < 0) continue;
				ConcreteClip tail = c;
				trimFront(tail, t1 - cs0);
				tail.used = false;
				clip[l][f] = tail;
				touch(clip[l][f]);
				clip[l][f].used = true;
			}
		}
	}

	int placeClip(int l, const ConcreteClip& c) {
		int j = freeClip(l);
		if (j < 0) return -1;
		clip[l][j] = c;
		clip[l][j].used = false;
		touch(clip[l][j]);
		clip[l][j].used = true;
		splice(l, j);
		return j;
	}

	// Load audio files onto lane l from bar t0, one after another.
	int loadOnto(int l, float t0, const std::vector<std::string>& paths) {
		int placed = 0;
		for (const std::string& p : paths) {
			if (!ccIsAudio(p) || t0 >= CC_MAXBARS) continue;
			ConcreteSample* s = ccLoadFile(p);
			if (!s) { WARN("Concrete: could not load \"%s\"", p.c_str()); continue; }
			s->uid = ++uid;
			int si = publish(s);
			if (si < 0) { delete s; continue; }
			ConcreteClip c;
			c.smp = si; c.t0 = t0; c.a = 0.0; c.b = (double)s->n;
			if (placeClip(l, c) < 0) continue;
			placed++;
			double len = (double)s->n / s->sr / spbSm;
			t0 = (float)(t0 + len);
			if (snap) t0 = std::ceil(t0 * gridDiv() - 1e-4f) / gridDiv();
		}
		return placed;
	}

	float gridDiv() const { return (float)std::max(1, cpb); }

	void commitTake(int l, int k, double t) {
		ConcreteTake& T = take[l][k];
		int64_t n = T.n.load(std::memory_order_acquire);
		if (n >= (int64_t)(0.01f * T.sr)) {
			ConcreteSample* s = new ConcreteSample();
			s->d.assign(T.buf.begin(), T.buf.begin() + 2 * n);
			s->n = n; s->sr = T.sr; s->take = true;
			s->name = string::f("Take %d", ++takeNo);
			s->uid = ++uid;
			s->computePeaks();
			int si = publish(s);
			if (si < 0) delete s;
			else {
				ConcreteClip c;
				c.smp = si; c.t0 = T.t0; c.a = 0.0; c.b = (double)n;
				if (placeClip(l, c) < 0) WARN("Concrete: lane %d is full, the take was dropped", l + 1);
			}
		}
		T.freedAt = t;
		T.state.store(TK_FREE, std::memory_order_release);
	}

	void uiService() {
		double t = system::getTime();
		for (int l = 0; l < CC_N; l++)
			for (int k = 0; k < 2; k++)
				if (take[l][k].state.load(std::memory_order_acquire) == TK_SEALED) commitTake(l, k, t);

		// Recording buffers, while Concrete In is attached: one ready per lane at
		// all times, so a punch-in or a loop end always has somewhere to go.
		int len = clamp((int)std::round(params[LENGTH_PARAM].getValue()), 1, CC_MAXBARS);
		int64_t need = (int64_t)(std::min(CC_TAKE_MAXSEC, len * spbSm * 1.25 + 2.0) * sr);
		bool att = inAttached.load();
		for (int l = 0; l < CC_N; l++) {
			int ready = 0;
			for (int k = 0; k < 2; k++) if (take[l][k].state.load() == TK_READY) ready++;
			for (int k = 0; k < 2; k++) {
				ConcreteTake& T = take[l][k];
				int st = T.state.load();
				if (att && st == TK_READY && T.cap < need) {
					int e = TK_READY;
					if (T.state.compare_exchange_strong(e, TK_FREE)) { ready--; T.freedAt = t - 1.0; st = TK_FREE; }
				}
				// a buffer a voice may still be reading is left alone for a moment
				if (st != TK_FREE || t - T.freedAt < 0.25) continue;
				if (att && ready == 0) {
					if (T.cap < need) { T.buf.assign((size_t)(2 * need), 0.f); T.cap = need; }
					T.n.store(0);
					T.state.store(TK_READY, std::memory_order_release);
					ready++;
				} else if (T.cap > 0) {
					std::vector<float>().swap(T.buf);
					T.cap = 0;
				}
			}
			if (!att) {
				for (int k = 0; k < 2; k++) {
					int e = TK_READY;
					if (take[l][k].state.compare_exchange_strong(e, TK_FREE)) take[l][k].freedAt = t;
				}
			}
		}

		// samples nothing refers to, after a second
		bool ref[CC_SMPS] = {};
		for (int l = 0; l < CC_N; l++)
			for (int j = 0; j < CC_CLIPS; j++)
				if (clip[l][j].used && clip[l][j].smp >= 0 && clip[l][j].smp < CC_SMPS) ref[clip[l][j].smp] = true;
		for (int i = 0; i < CC_SMPS; i++) {
			ConcreteSample* s = smp[i].load();
			if (!s) continue;
			if (ref[i]) { unrefSince[i] = 0.0; continue; }
			if (unrefSince[i] == 0.0) unrefSince[i] = t;
			else if (t - unrefSince[i] > 1.0) { smp[i].store(nullptr); delete s; unrefSince[i] = 0.0; }
		}
		for (size_t g = 0; g < grave.size();) {
			if (t - grave[g].second > 1.0) { delete grave[g].first; grave.erase(grave.begin() + g); }
			else g++;
		}
	}

	void clearAll() {
		double t = system::getTime();
		for (int l = 0; l < CC_N; l++) for (int j = 0; j < CC_CLIPS; j++) clip[l][j].used = false;
		for (int i = 0; i < CC_SMPS; i++) {
			ConcreteSample* s = smp[i].exchange(nullptr);
			if (s) grave.push_back({s, t});
			unrefSince[i] = 0.0;
		}
		seekGen++;
	}

	// ── saving ──────────────────────────────────────────────────────────
	// A take has no file behind it, so it is written into the patch's storage
	// directory, which Rack carries with the patch. A loaded sample is copied
	// there too when "Keep copies of loaded samples in the patch" is on.
	void writeStorage(ConcreteSample* s) {
		std::string dir = createPatchStorageDirectory();
		if (dir.empty()) return;
		if (s->take) {
			std::string name = string::f("concrete-take-%d.wav", s->uid);
			drwav_data_format fmt;
			fmt.container = drwav_container_riff;
			fmt.format = DR_WAVE_FORMAT_PCM;
			fmt.channels = 2;
			fmt.sampleRate = (drwav_uint32)s->sr;
			fmt.bitsPerSample = 16;
			drwav w;
			if (!drwav_init_file_write(&w, system::join(dir, name).c_str(), &fmt, NULL)) return;
			std::vector<int16_t> pcm((size_t)(2 * s->n));
			for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)clamp(s->d[i] * 32767.f, -32768.f, 32767.f);
			drwav_write_pcm_frames(&w, (drwav_uint64)s->n, pcm.data());
			drwav_uninit(&w);
			s->stored = name; s->written = true;
		} else if (!s->path.empty() && system::isFile(s->path)) {
			std::string name = string::f("concrete-%d-%s", s->uid, system::getFilename(s->path).c_str());
			std::string dst = system::join(dir, name);
			if (system::isFile(dst) || system::copy(s->path, dst)) { s->stored = name; s->written = true; }
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "snap", json_boolean(snap));
		json_object_set_new(root, "embed", json_boolean(embed));
		json_object_set_new(root, "resetOnPlay", json_boolean(resetOnPlay));
		json_object_set_new(root, "cpbIdx", json_integer(cpbIdx));
		json_object_set_new(root, "tab", json_integer(tab));
		json_object_set_new(root, "viewStart", json_real(viewStart));
		json_object_set_new(root, "viewLen", json_real(viewLen));
		json_object_set_new(root, "viewFollow", json_boolean(viewFollow));
		json_object_set_new(root, "uid", json_integer(uid));
		json_object_set_new(root, "takeNo", json_integer(takeNo));
		json_object_set_new(root, "pos", json_real(P));

		bool ref[CC_SMPS] = {};
		for (int l = 0; l < CC_N; l++)
			for (int j = 0; j < CC_CLIPS; j++)
				if (clip[l][j].used && clip[l][j].smp >= 0) ref[clip[l][j].smp] = true;
		std::vector<std::string> keep;
		json_t* sa = json_array();
		for (int i = 0; i < CC_SMPS; i++) {
			ConcreteSample* s = smp[i].load();
			if (!s || !ref[i]) continue;
			if (!s->missing) {
				if ((s->take || embed) && !s->written) writeStorage(s);
				// copies go when the option does, unless the original has gone too
				if (!s->take && !embed && s->written && system::isFile(s->path)) { s->stored = ""; s->written = false; }
			}
			if (!s->stored.empty()) keep.push_back(s->stored);
			json_t* o = json_object();
			json_object_set_new(o, "slot", json_integer(i));
			json_object_set_new(o, "uid", json_integer(s->uid));
			json_object_set_new(o, "name", json_string(s->name.c_str()));
			json_object_set_new(o, "path", json_string(s->path.c_str()));
			json_object_set_new(o, "stored", json_string(s->stored.c_str()));
			json_object_set_new(o, "take", json_boolean(s->take));
			json_object_set_new(o, "sr", json_real(s->sr));
			json_array_append_new(sa, o);
		}
		json_object_set_new(root, "samples", sa);

		json_t* la = json_array();
		for (int l = 0; l < CC_N; l++) {
			json_t* ca = json_array();
			for (int j = 0; j < CC_CLIPS; j++) {
				const ConcreteClip& c = clip[l][j];
				if (!c.used) continue;
				json_t* o = json_object();
				json_object_set_new(o, "s", json_integer(c.smp));
				json_object_set_new(o, "t0", json_real(c.t0));
				json_object_set_new(o, "a", json_real(c.a));
				json_object_set_new(o, "b", json_real(c.b));
				json_object_set_new(o, "speed", json_real(c.speed));
				if (c.rev) json_object_set_new(o, "rev", json_true());
				if (c.fit > 0.f) json_object_set_new(o, "fit", json_real(c.fit));
				json_array_append_new(ca, o);
			}
			json_array_append_new(la, ca);
		}
		json_object_set_new(root, "lanes", la);

		// the points, as [bar, value, bar, value ...] per curve
		json_t* cv = json_array();
		for (int l = 0; l < CC_N; l++) {
			json_t* lc = json_array();
			for (int c = 0; c < CC_NCURVE; c++) {
				json_t* pa = json_array();
				for (const CcPt& q : pts[l][c]) { json_array_append_new(pa, json_real(q.b)); json_array_append_new(pa, json_real(q.v)); }
				json_array_append_new(lc, pa);
			}
			json_array_append_new(cv, lc);
		}
		json_object_set_new(root, "points", cv);

		// storage files nothing refers to any more
		std::string dir = getPatchStorageDirectory();
		if (!dir.empty() && system::isDirectory(dir)) {
			for (const std::string& e : system::getEntries(dir)) {
				std::string fn = system::getFilename(e);
				if (!string::startsWith(fn, "concrete-")) continue;
				if (std::find(keep.begin(), keep.end(), fn) == keep.end()) system::remove(e);
			}
		}
		return root;
	}

	void dataFromJson(json_t* root) override {
		clearAll();
		resetCurves();
		if (json_t* j = json_object_get(root, "snap")) snap = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "embed")) embed = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "resetOnPlay")) resetOnPlay = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "cpbIdx")) cpbIdx = clamp((int)json_integer_value(j), 0, CC_NCPB - 1);
		if (json_t* j = json_object_get(root, "tab")) tab = clamp((int)json_integer_value(j), 0, CC_NCURVE);
		if (json_t* j = json_object_get(root, "viewStart")) viewStart = (float)json_number_value(j);
		if (json_t* j = json_object_get(root, "viewLen")) viewLen = (float)json_number_value(j);
		if (json_t* j = json_object_get(root, "viewFollow")) viewFollow = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "uid")) uid = (int)json_integer_value(j);
		if (json_t* j = json_object_get(root, "takeNo")) takeNo = (int)json_integer_value(j);

		int map[CC_SMPS];
		for (int i = 0; i < CC_SMPS; i++) map[i] = -1;
		std::string dir = getPatchStorageDirectory();
		json_t* sa = json_object_get(root, "samples");
		for (size_t q = 0; sa && q < json_array_size(sa); q++) {
			json_t* o = json_array_get(sa, q);
			int slot = (int)json_integer_value(json_object_get(o, "slot"));
			auto str = [&](const char* k) { json_t* j = json_object_get(o, k); return std::string(j && json_string_value(j) ? json_string_value(j) : ""); };
			std::string stored = str("stored"), path = str("path");
			ConcreteSample* s = nullptr;
			if (!stored.empty() && !dir.empty() && system::isFile(system::join(dir, stored))) {
				s = ccLoadFile(system::join(dir, stored));
				if (s) { s->stored = stored; s->written = true; }
			}
			if (!s && !path.empty() && system::isFile(path)) s = ccLoadFile(path);
			if (!s) {
				s = new ConcreteSample();
				s->missing = true;
				if (json_t* j = json_object_get(o, "sr")) s->sr = (float)json_number_value(j);
			}
			s->name = str("name"); s->path = path;
			s->take = json_boolean_value(json_object_get(o, "take"));
			s->uid = (int)json_integer_value(json_object_get(o, "uid"));
			int si = publish(s);
			if (si < 0) { delete s; continue; }
			if (slot >= 0 && slot < CC_SMPS) map[slot] = si;
		}

		json_t* la = json_object_get(root, "lanes");
		for (int l = 0; la && l < CC_N && l < (int)json_array_size(la); l++) {
			json_t* ca = json_array_get(la, l);
			for (size_t q = 0; q < json_array_size(ca); q++) {
				json_t* o = json_array_get(ca, q);
				int s = (int)json_integer_value(json_object_get(o, "s"));
				int j = freeClip(l);
				if (j < 0 || s < 0 || s >= CC_SMPS || map[s] < 0) continue;
				ConcreteClip c;
				c.smp = map[s];
				c.t0 = (float)json_number_value(json_object_get(o, "t0"));
				c.a = json_number_value(json_object_get(o, "a"));
				c.b = json_number_value(json_object_get(o, "b"));
				c.speed = clamp((float)json_number_value(json_object_get(o, "speed")), CC_MINSPEED, CC_MAXSPEED);
				c.rev = json_boolean_value(json_object_get(o, "rev"));
				if (json_t* f = json_object_get(o, "fit")) c.fit = (float)json_number_value(f);
				c.gen = ++genCounter;
				clip[l][j] = c;
				clip[l][j].used = true;
			}
		}

		json_t* cv = json_object_get(root, "points");
		for (int l = 0; cv && l < CC_N && l < (int)json_array_size(cv); l++) {
			json_t* lc = json_array_get(cv, l);
			for (int c = 0; c < CC_NCURVE && c < (int)json_array_size(lc); c++) {
				json_t* pa = json_array_get(lc, c);
				for (size_t q = 0; q + 1 < json_array_size(pa); q += 2)
					pts[l][c].push_back({(float)json_number_value(json_array_get(pa, q)), (float)json_number_value(json_array_get(pa, q + 1))});
				std::stable_sort(pts[l][c].begin(), pts[l][c].end(), [](const CcPt& a, const CcPt& b) { return a.b < b.b; });
				pruneCurve(l, c);
				renderCurve(l, c);
			}
		}
		if (json_t* j = json_object_get(root, "pos")) {
			double p = json_number_value(j);
			if (p > 0.0) seekReq.store((float)p);
		}
	}
};

// =============================================================================
// Something to look at in the browser: four lanes of synthesised clips with a
// curve drawn, two seconds into playing.
// =============================================================================
static ConcreteSample* ccSynth(int kind, float sr) {
	float secs = kind == 0 ? 4.f : kind == 1 ? 2.f : 4.f;
	int64_t n = (int64_t)(secs * sr);
	std::vector<float> raw((size_t)(2 * n));
	uint32_t rng = 12345u + kind;
	auto noise = [&]() { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 8388608.f - 1.f; };
	for (int64_t i = 0; i < n; i++) {
		float t = (float)i / sr, x = 0.f;
		if (kind == 0) {        // drums: kick on the beat, a tick on the off-beat
			float tb = std::fmod(t, 0.5f), to = std::fmod(t + 0.25f, 0.5f);
			x = std::sin(2.f * (float)M_PI * (50.f * tb + 40.f * (1.f - std::exp(-tb * 30.f)) / 30.f)) * std::exp(-tb * 9.f)
			  + 0.3f * noise() * std::exp(-to * 60.f);
		} else if (kind == 1) { // bass: an ostinato of eighths
			static const float NOTE[4] = {55.f, 65.41f, 73.42f, 49.f};
			float te = std::fmod(t, 0.25f);
			float f = NOTE[(int)(t / 0.5f) % 4];
			x = 0.6f * (2.f * std::fmod(f * t, 1.f) - 1.f) * std::exp(-te * 6.f);
		} else if (kind == 2) { // pad: a chord that swells
			x = 0.25f * (std::sin(2.f * (float)M_PI * 220.f * t) + std::sin(2.f * (float)M_PI * 277.2f * t)
			           + std::sin(2.f * (float)M_PI * 329.6f * t)) * std::sin((float)M_PI * t / secs);
		} else {                // texture: noise in bursts
			x = 0.4f * noise() * (0.5f + 0.5f * std::sin(2.f * (float)M_PI * 1.5f * t));
		}
		raw[2 * i] = x; raw[2 * i + 1] = x;
	}
	ConcreteSample* s = ccFromInterleaved(raw.data(), n, 2, sr);
	static const char* NAME[4] = {"drums", "bass", "pad", "texture"};
	s->name = NAME[kind];
	return s;
}

static Concrete* concretePreview() {
	static Concrete* m = nullptr;
	if (m) return m;
	m = new Concrete();
	m->params[Concrete::PLAY_PARAM].setValue(1.f);
	int si[4];
	for (int k = 0; k < 4; k++) si[k] = m->publish(ccSynth(k, sfs::PREVIEW_SR));
	auto put = [&](int l, int k, float t0, float speed, bool rev) {
		ConcreteSample* s = m->smp[si[k]].load();
		ConcreteClip c; c.smp = si[k]; c.t0 = t0; c.a = 0.0; c.b = (double)s->n; c.speed = speed; c.rev = rev;
		m->placeClip(l, c);
	};
	put(0, 0, 0.f, 1.f, false); put(0, 0, 2.f, 1.f, false);
	for (int b = 0; b < 4; b++) put(1, 1, (float)b, 1.f, false);
	put(2, 2, 0.f, 0.5f, false);
	put(3, 3, 1.f, 1.f, false); put(3, 3, 3.f, 1.f, true);
	m->pts[2][CC_VOL] = {{0.f, 0.35f}, {4.f, 1.f}};
	m->pts[3][CC_PAN] = {{0.f, 0.f}, {1.f, 0.8f}, {2.f, 0.f}, {3.f, -0.8f}, {4.f, 0.f}};
	m->pts[1][CC_FILT] = {{0.f, -0.5f}, {2.f, 0.f}};
	m->renderCurve(2, CC_VOL); m->renderCurve(3, CC_PAN); m->renderCurve(1, CC_FILT);
	int64_t frame = 0;
	sfs::previewRun(*m, 1.3f, frame);
	return m;
}

// =============================================================================
// The screen: four lanes across the loop. CLIPS edits the clips; VOL, FILTER
// and PAN draw that curve on each lane.
// =============================================================================
static const float CC_DESIGN_W = 600.f;
// The view is a whole number of bars: 1, 2, 4 or the whole loop, on bar lines.
static const int CC_VIEWS[3] = {1, 2, 4};
static const float CC_ZX = 290.f, CC_ZW = 24.f;   // the view buttons, design units
static const float CC_TABW = 50.f;                // a tab and its gap
static const float CC_PT_XDEAD = 2.5f;            // mm sideways before a dragged curve point leaves its bar

struct ConcreteDisplay : OpaqueWidget {
	Concrete* module = nullptr;
	std::shared_ptr<Font> font;

	enum Drag { D_NONE, D_MOVE, D_TRIML, D_TRIMR, D_SPEED, D_POINT, D_PAN };
	double panStart = 0.0;
	int dPt = -1;                 // the curve point being dragged
	float dPtBar = 0.f;           // the bar it was on when grabbed
	Vec dPtGrab;                  // from the pointer to the point, when grabbed
	bool dPtXFree = false;        // the drag has gone far enough sideways to move it along the bar line
	float scrollAcc = 0.f, panAcc = 0.f;
	int drag = D_NONE, dLane = -1, dClip = -1, dToLane = -1;
	Vec dPos, dStart;
	ConcreteClip dOrig;
	double dOrigLen = 0.0;
	Vec hover = Vec(-1, -1);
	bool hovering = false;
	int pendLane = -1;
	float pendT0 = 0.f;
	int selLane = -1, selClip = -1;   // the clip last clicked, until the screen loses the selection
	bool delHeld = false;             // a Delete press removed the selected clip and is still down
	bool dCopy = false;               // this drag is a Ctrl (Cmd) copy
	double dFine = 0.0;               // how far a trim has been dragged, in pixels, after the slowdown
	int modsOverride = -1;            // a harness stands in for the keyboard here

	// geometry, in pixels
	float sc() const { return box.size.x / CC_DESIGN_W; }
	float x0() const { return 32.f * sc(); }
	float x1() const { return box.size.x - 8.f * sc(); }
	float ovTop() const { return 24.f * sc(); }        // the overview strip: the whole loop
	float ovBot() const { return 33.f * sc(); }
	float rulerTop() const { return 35.f * sc(); }
	float lanesTop() const { return 50.f * sc(); }
	float laneH() const { return (box.size.y - 6.f * sc() - lanesTop()) / CC_N; }
	float laneY(int l) const { return lanesTop() + laneH() * l; }
	int loopLen() const { return module ? module->L : 4; }
	// ZOOM: the lanes show vLen() bars from vStart(); the overview strip
	// always shows the whole loop
	double vLen() const {
		double L = loopLen(), v = module ? std::round(module->viewLen) : 0.0;
		return (v < 1.0 || v >= L) ? L : v;
	}
	double vStart() const { return module ? ccClamp(std::round((double)module->viewStart), 0.0, loopLen() - vLen()) : 0.0; }
	bool zoomed() const { return vLen() < loopLen() - 1e-6; }
	float xOf(double bar) const { return x0() + (float)((bar - vStart()) / vLen()) * (x1() - x0()); }
	double barOf(float x) const { return vStart() + (double)(x - x0()) / (x1() - x0()) * vLen(); }
	float ovX(double bar) const { return x0() + (float)(bar / loopLen()) * (x1() - x0()); }
	double holdUntil = 0.0;                   // a hand on the view stops it following, for a while
	// whole bars, inside the loop
	void setView(double start, double len) {
		if (!module) return;
		double L = loopLen();
		len = ccClamp(std::round(len), 1.0, L);
		start = ccClamp(std::round(start), 0.0, L - len);
		module->viewLen = len >= L ? 0.f : (float)len;
		module->viewStart = (float)start;
	}
	// show n bars: the page of n that holds `bar`
	void viewBars(int n, double bar) {
		setView(std::floor(bar / n) * n, n);
	}
	// the next view length in (dir < 0) or out (dir > 0), keeping `bar` in view
	void zoomStep(int dir, double bar) {
		int L = loopLen(), cur = (int)vLen(), next = cur;
		if (dir < 0) { for (int k = 2; k >= 0; k--) if (CC_VIEWS[k] < cur) { next = CC_VIEWS[k]; break; } }
		else { next = L; for (int k = 0; k < 3; k++) if (CC_VIEWS[k] > cur && CC_VIEWS[k] < L) { next = CC_VIEWS[k]; break; } }
		viewBars(next, bar);
		holdUntil = system::getTime() + 3.0;
	}
	int beatsPerBar() const {
		int g = module ? module->cpb : 16;
		return (g % 4 == 0) ? 4 : (g % 3 == 0) ? 3 : 4;
	}
	int laneAt(float y) const {
		if (y < lanesTop()) return -1;
		int l = (int)((y - lanesTop()) / laneH());
		return (l >= 0 && l < CC_N) ? l : -1;
	}
	int modsNow() const { return modsOverride >= 0 ? modsOverride : (APP->window->getMods() & RACK_MOD_MASK); }
	// Ctrl (Cmd) inverts the snap while dragging, except on a copy, where Ctrl is
	// what asked for the copy
	bool snapNow() const {
		if (!module) return false;
		bool invert = (modsNow() & RACK_MOD_CTRL) && !dCopy;
		return module->snap != invert;
	}
	double snapBar(double b) const {
		if (!snapNow() || !module) return b;
		float g = module->gridDiv();
		return std::round(b * g) / g;
	}
	// How much a trim is slowed, as on a Rack knob: Ctrl (Cmd) 1/16, Ctrl+Shift 1/256
	int fineNow() const {
		int mods = modsNow();
		return mods == (RACK_MOD_CTRL | GLFW_MOD_SHIFT) ? 256 : mods == RACK_MOD_CTRL ? 16 : 1;
	}
	// a slowed trim leaves the grid, or it could not land between two grid lines
	double snapEdge(double b) const { return fineNow() > 1 ? b : snapBar(b); }
	bool selValid() const {
		return module && module->tab == 0 && selLane >= 0 && selClip >= 0 && module->clip[selLane][selClip].used;
	}

	// the clip under a point, and which part of it
	enum Part { P_NONE, P_BODY, P_LEFT, P_RIGHT };
	int clipAt(Vec p, int& lane, int& part) const {
		lane = laneAt(p.y); part = P_NONE;
		if (!module || lane < 0 || p.x < x0() || p.x > x1()) return -1;
		Concrete* m = module;
		int best = -1;
		float bt = -1.f;
		for (int j = 0; j < CC_CLIPS; j++) {
			const ConcreteClip& c = m->clip[lane][j];
			if (!c.used || c.t0 >= m->L) continue;
			float xa = xOf(c.t0), xb = xOf(m->clipEnd(lane, j, m->spbSm, m->L));
			xb = std::max(xb, xa + 2.f);
			if (p.x < xa - 1.f || p.x > xb + 1.f) continue;
			if (c.t0 > bt) { bt = c.t0; best = j; }
		}
		if (best < 0) return -1;
		const ConcreteClip& c = m->clip[lane][best];
		float xa = xOf(c.t0), xb = std::max(xOf(m->clipEnd(lane, best, m->spbSm, m->L)), xa + 2.f);
		float edge = std::min(mm2px(1.6f), (xb - xa) * 0.25f);
		part = p.x - xa < edge ? P_LEFT : xb - p.x < edge ? P_RIGHT : P_BODY;
		return best;
	}

	// ── curve points ──────────────────────────────────────────────────
	float curveTop(int l) const { return laneY(l) + 4.f * sc(); }
	float curveBot(int l) const { return laneY(l) + laneH() - 4.f * sc(); }
	float yOfV(int l, int c, float v) const {
		float yT = curveTop(l), yB = curveBot(l);
		return c == CC_VOL ? yB - v * (yB - yT) : yB - (v + 1.f) * 0.5f * (yB - yT);
	}
	float vOfY(int l, int c, float y) const {
		float u = clamp((curveBot(l) - y) / (curveBot(l) - curveTop(l)), 0.f, 1.f);
		float v = c == CC_VOL ? u : u * 2.f - 1.f;
		if (c != CC_VOL && std::fabs(v) < 0.04f) v = 0.f;     // a detent at the centre
		if (c == CC_VOL && v > 0.97f) v = 1.f;                // and at unity
		// pitch lands on semitones while the snap is on
		if (c == CC_PITCH && snapNow()) v = std::round(v * CC_PITCH_SEMIS) / CC_PITCH_SEMIS;
		return v;
	}
	// the point of the open curve under p, or -1
	int ptAt(int l, Vec p) const {
		if (!module || module->tab == 0 || l < 0) return -1;
		int c = module->tab - 1;
		const auto& P = module->pts[l][c];
		float r = mm2px(1.6f);
		int best = -1;
		float bd = r * r;
		for (int k = 0; k < (int)P.size(); k++) {
			float dx = xOf(P[k].b) - p.x, dy = yOfV(l, c, P[k].v) - p.y, d = dx * dx + dy * dy;
			if (d <= bd) { bd = d; best = k; }
		}
		return best;
	}
	// keepBar moves the point straight up or down, on the bar it was grabbed on
	void movePoint(int l, int k, Vec p, bool keepBar) {
		Concrete* m = module;
		int c = m->tab - 1;
		if (l < 0 || l >= CC_N || c < 0 || c >= CC_NCURVE) return;   // the CLIPS tab has no curve
		auto& P = m->pts[l][c];
		if (k < 0 || k >= (int)P.size()) return;
		double lo = k > 0 ? P[k - 1].b : 0.0, hi = k + 1 < (int)P.size() ? P[k + 1].b : (double)CC_MAXBARS;
		double b = keepBar ? (double)dPtBar : snapBar(barOf(clamp(p.x, x0(), x1())));
		P[k].b = (float)ccClamp(b, lo, hi);
		P[k].v = vOfY(l, c, p.y);
		m->renderCurve(l, c);
	}
	int addPoint(int l, Vec p) {
		Concrete* m = module;
		int c = m->tab - 1;
		auto& P = m->pts[l][c];
		float b = (float)ccClamp(snapBar(barOf(clamp(p.x, x0(), x1()))), 0.0, (double)CC_MAXBARS);
		int k = 0;
		while (k < (int)P.size() && P[k].b <= b) k++;
		P.insert(P.begin() + k, {b, vOfY(l, c, p.y)});
		m->renderCurve(l, c);
		return k;
	}
	// the nearer point of the two on the bar a click at p would land on, or -1
	int fullBarPoint(int l, Vec p) const {
		int c = module->tab - 1;
		const auto& P = module->pts[l][c];
		float b = (float)ccClamp(snapBar(barOf(clamp(p.x, x0(), x1()))), 0.0, (double)CC_MAXBARS);
		for (int k = 0; k + 1 < (int)P.size(); k++)
			if (P[k].b == b && P[k + 1].b == b)
				return std::fabs(yOfV(l, c, P[k].v) - p.y) <= std::fabs(yOfV(l, c, P[k + 1].v) - p.y) ? k : k + 1;
		return -1;
	}
	void deletePoint(int l, int k) {
		Concrete* m = module;
		int c = m->tab - 1;
		if (l < 0 || l >= CC_N || c < 0 || c >= CC_NCURVE) return;   // the CLIPS tab has no curve
		auto& P = m->pts[l][c];
		if (k < 0 || k >= (int)P.size()) return;
		P.erase(P.begin() + k);
		m->renderCurve(l, c);
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS) { OpaqueWidget::onButton(e); return; }
		Concrete* m = module;
		Vec p = e.pos;
		// tabs
		if (p.y < 22.f * sc() && p.x < 8.f * sc() + (CC_NCURVE + 1) * CC_TABW * sc()) {
			if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
				int t = (int)((p.x - 8.f * sc()) / (CC_TABW * sc()));
				if (t >= 0 && t <= CC_NCURVE) m->tab = t;
			}
			e.consume(this);
			return;
		}
		// the view: 1, 2 or 4 bars, or the whole loop; the page with the playhead
		if (p.y < 22.f * sc() && p.x >= CC_ZX * sc() && p.x < (CC_ZX + 4 * CC_ZW) * sc()) {
			if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
				int b = (int)((p.x - CC_ZX * sc()) / (CC_ZW * sc()));
				if (b >= 0 && b < 3 && CC_VIEWS[b] < loopLen()) viewBars(CC_VIEWS[b], m->P);
				else if (b == 3) setView(0.0, loopLen());
				holdUntil = 0.0;
			}
			e.consume(this);
			return;
		}
		// the overview: drag the window along the loop
		if (p.y >= ovTop() - 1.f && p.y < ovBot() + 1.f && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (zoomed()) {
				double b = (double)(p.x - x0()) / (x1() - x0()) * loopLen();
				setView(std::floor(b - vLen() * 0.5 + 0.5), vLen());
				holdUntil = system::getTime() + 3.0;
				dPos = dStart = p;
				drag = D_PAN;
				panStart = vStart();
			}
			e.consume(this);
			return;
		}
		// the ruler: move the playhead
		if (p.y >= rulerTop() && p.y < lanesTop() && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			double b = ccClamp(snapBar(barOf(p.x)), 0.0, (double)m->L - 1e-4);
			m->seekReq.store((float)b);
			e.consume(this);
			return;
		}
		int lane, part;
		int j = clipAt(p, lane, part);
		if (lane < 0) { OpaqueWidget::onButton(e); return; }
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			int k = ptAt(lane, p);
			if (m->tab > 0 && k >= 0) pointMenu(lane, k);
			else if (m->tab == 0 && j >= 0) clipMenu(lane, j);
			else if (m->tab == 0) laneMenu(lane, (float)ccClamp(snapBar(barOf(p.x)), 0.0, (double)CC_MAXBARS - 0.01));
			else curveMenu(lane);
			e.consume(this);
			return;
		}
		if (e.button != GLFW_MOUSE_BUTTON_LEFT) return;
		dPos = dStart = p;
		dLane = dToLane = lane;
		if (m->tab == 0) {
			selLane = j >= 0 ? lane : -1;
			selClip = j;
			if (j < 0) { e.consume(this); return; }
			dClip = j;
			dFine = 0.0;
			dCopy = false;
			dOrig = m->clip[lane][j];
			dOrigLen = m->clipBars(dOrig, m->spbSm);
			bool shift = (e.mods & RACK_MOD_MASK) == GLFW_MOD_SHIFT;
			drag = part == P_LEFT ? D_TRIML : part == P_RIGHT ? (shift ? D_SPEED : D_TRIMR) : D_MOVE;
			// Ctrl (Cmd) on the body drags a copy and leaves the original where
			// it is. The copy is not spliced until it is let go, as a move is.
			if (drag == D_MOVE && (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL) {
				int k = m->freeClip(lane);
				if (k >= 0) {
					m->clip[lane][k] = dOrig;
					m->clip[lane][k].used = false;
					m->touch(m->clip[lane][k]);
					m->clip[lane][k].used = true;
					dClip = k;
					selClip = k;
					dCopy = true;
				}
			}
		} else {
			// a point under the pointer is picked up; anywhere else puts one down,
			// unless its bar already has two, when the nearer of those is picked up
			int k = ptAt(lane, p);
			if (k < 0) k = fullBarPoint(lane, p);
			if (k < 0) k = addPoint(lane, p);
			drag = D_POINT;
			dPt = k;
			{
				int c = m->tab - 1;
				const auto& P = m->pts[lane][c];
				dPtBar = P[k].b;
				dPtGrab = Vec(xOf(P[k].b), yOfV(lane, c, P[k].v)).minus(p);
				dPtXFree = false;
			}
		}
		e.consume(this);
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (!module || drag == D_NONE) return;
		Concrete* m = module;
		Vec delta = e.mouseDelta.div(getAbsoluteZoom());
		dPos = dPos.plus(delta);
		dFine += (double)delta.x / fineNow();
		if (drag == D_POINT) {
			// A point stays on its bar until the drag has clearly gone sideways, so
			// a hand moving up does not also move it along. Shift holds it there.
			if (std::fabs(dPos.x - dStart.x) > mm2px(CC_PT_XDEAD)) dPtXFree = true;
			movePoint(dLane, dPt, dPos.plus(dPtGrab), !dPtXFree || (modsNow() & GLFW_MOD_SHIFT));
			return;
		}
		if (drag == D_PAN) {
			setView(std::round(panStart + (double)(dPos.x - dStart.x) / (x1() - x0()) * loopLen()), vLen());
			holdUntil = system::getTime() + 3.0;
			return;
		}
		if (dClip < 0) return;
		ConcreteClip& c = m->clip[dLane][dClip];
		if (!c.used) { drag = D_NONE; return; }
		const ConcreteSample* s = m->sampleOf(c);
		double db = (double)(dPos.x - dStart.x) / (x1() - x0()) * vLen();
		double dbEdge = dFine / (x1() - x0()) * vLen();
		double spb = m->spbSm;
		if (drag == D_MOVE) {
			c.t0 = (float)ccClamp(snapBar(dOrig.t0 + db), 0.0, (double)CC_MAXBARS - 0.01);
			int l = laneAt(dPos.y);
			if (l >= 0) dToLane = l;
		} else if (drag == D_TRIML && s) {
			double nt = ccClamp(snapEdge(dOrig.t0 + dbEdge), 0.0, dOrig.t0 + dOrigLen - 0.01);
			double d = nt - dOrig.t0;
			double f = d * spb * s->sr * m->clipSpeed(dOrig, s, spb);
			double na = dOrig.a, nb = dOrig.b;
			if (dOrig.rev) nb = ccClamp(dOrig.b - f, dOrig.a + 64.0, (double)s->n);
			else na = ccClamp(dOrig.a + f, 0.0, dOrig.b - 64.0);
			double used = (dOrig.rev ? dOrig.b - nb : na - dOrig.a) / (s->sr * m->clipSpeed(dOrig, s, spb) * spb);
			c.a = na; c.b = nb;
			c.t0 = (float)(dOrig.t0 + used);
			if (dOrig.fit > 0.f) c.fit = std::max(0.01f, dOrig.fit - (float)used);
		} else if (drag == D_TRIMR && s) {
			double ne = std::max(dOrig.t0 + 0.01, snapEdge(dOrig.t0 + dOrigLen + dbEdge));
			double d = ne - (dOrig.t0 + dOrigLen);
			double f = d * spb * s->sr * m->clipSpeed(dOrig, s, spb);
			if (dOrig.rev) { c.a = ccClamp(dOrig.a - f, 0.0, dOrig.b - 64.0); c.b = dOrig.b; }
			else { c.b = ccClamp(dOrig.b + f, dOrig.a + 64.0, (double)s->n); c.a = dOrig.a; }
			if (dOrig.fit > 0.f) c.fit = std::max(0.01f, (float)((c.b - c.a) / s->sr / m->clipSpeed(dOrig, s, spb) / spb));
		} else if (drag == D_SPEED && s) {
			// varispeed: the window stays, its length on the tape changes
			double ne = std::max(dOrig.t0 + 0.01, snapEdge(dOrig.t0 + dOrigLen + dbEdge));
			double lb = ne - dOrig.t0;
			if (dOrig.fit > 0.f) c.fit = (float)lb;
			else c.speed = clamp((float)(((dOrig.b - dOrig.a) / s->sr) / (lb * spb)), CC_MINSPEED, CC_MAXSPEED);
		}
		m->touch(c);
	}

	void onDragEnd(const DragEndEvent& e) override {
		if (module && drag == D_MOVE && dClip >= 0) {
			Concrete* m = module;
			int j = dClip, l = dLane;
			if (dToLane != dLane) {
				int k = m->freeClip(dToLane);
				if (k >= 0) {
					ConcreteClip c = m->clip[dLane][dClip];
					m->clip[dLane][dClip].used = false;
					c.used = false;
					m->clip[dToLane][k] = c;
					m->touch(m->clip[dToLane][k]);
					m->clip[dToLane][k].used = true;
					j = k; l = dToLane;
				}
			}
			m->splice(l, j);
			selLane = l; selClip = j;
		}
		if (module && drag == D_POINT && module->tab > 0) {
			module->pruneCurve(dLane, module->tab - 1);
			module->renderCurve(dLane, module->tab - 1);
		}
		drag = D_NONE; dClip = -1; dPt = -1; dCopy = false;
	}

	// double-click a point to remove it
	void onDoubleClick(const DoubleClickEvent& e) override {
		if (!module || module->tab == 0) return;
		int l = laneAt(hover.y);
		int k = ptAt(l, hover);
		if (k >= 0) { deletePoint(l, k); e.consume(this); }
	}

	void onHover(const HoverEvent& e) override { hover = e.pos; hovering = true; OpaqueWidget::onHover(e); }
	void onLeave(const LeaveEvent& e) override { hovering = false; OpaqueWidget::onLeave(e); }

	static bool isDeleteKey(int key) { return key == GLFW_KEY_DELETE || key == GLFW_KEY_BACKSPACE; }

	// Rack sends keys to the selected widget before anything else, so while a
	// clip is selected Delete reaches it wherever the pointer has gone. The
	// repeats of the press that removed it stop here too.
	void onSelectKey(const SelectKeyEvent& e) override {
		if (!isDeleteKey(e.key)) return;
		if (e.action == GLFW_PRESS) {
			delHeld = selValid();
			if (delHeld) { module->clip[selLane][selClip].used = false; selLane = selClip = -1; }
		}
		if (delHeld) e.consume(this);
		if (e.action == GLFW_RELEASE) delHeld = false;
	}
	void onDeselect(const DeselectEvent& e) override { selLane = selClip = -1; delHeld = false; }

	// Delete with nothing selected: whatever is under the pointer. Called by
	// ConcreteWidget, because ModuleWidget acts on this key before its children.
	void deleteUnder(Vec p) {
		if (!module) return;
		int lane, part;
		if (module->tab == 0) {
			int j = clipAt(p, lane, part);
			if (j >= 0) module->clip[lane][j].used = false;
		} else {
			lane = laneAt(p.y);
			deletePoint(lane, ptAt(lane, p));
		}
	}

	// Ctrl (Cmd) + scroll: up and down step the view through 1/2/4 bars and the
	// whole loop, about the bar under the pointer; sideways moves it a bar.
	// Plain scroll is left to Rack, which scrolls the rack with it.
	void onHoverScroll(const HoverScrollEvent& e) override {
		if (!module || (APP->window->getMods() & RACK_MOD_MASK) != RACK_MOD_CTRL || e.pos.y < ovTop()) {
			OpaqueWidget::onHoverScroll(e);
			return;
		}
		scrollAcc += e.scrollDelta.y;
		panAcc += e.scrollDelta.x;
		double bar = barOf(clamp(e.pos.x, x0(), x1()));
		if (scrollAcc > 30.f) { zoomStep(-1, bar); scrollAcc = 0.f; }
		else if (scrollAcc < -30.f) { zoomStep(1, bar); scrollAcc = 0.f; }
		if (std::fabs(panAcc) > 30.f) {
			setView(vStart() + (panAcc > 0.f ? -1.0 : 1.0), vLen());
			panAcc = 0.f;
			holdUntil = system::getTime() + 3.0;
		}
		e.consume(this);
	}

	void onPathDrop(const PathDropEvent& e) override {
		if (!module) return;
		int l = laneAt(e.pos.y);
		if (l < 0) l = 0;
		float t0 = (float)ccClamp(snapBar(barOf(e.pos.x)), 0.0, (double)CC_MAXBARS - 0.01);
		if (module->loadOnto(l, t0, e.paths) > 0) e.consume(this);
	}

	void step() override {
		OpaqueWidget::step();
		// a zoomed view turns the page when the playhead leaves it
		if (module && module->viewFollow && module->moving && zoomed() && drag == D_NONE && system::getTime() > holdUntil) {
			double P = module->P, vs = vStart(), vl = vLen();
			if (P < vs || P >= vs + vl) viewBars((int)vl, P);
		}
		if (pendLane >= 0 && module) {
			int l = pendLane;
			pendLane = -1;
			char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, osdialog_filters_parse("Audio:wav,flac"));
			if (path) { module->loadOnto(l, pendT0, {std::string(path)}); std::free(path); }
		}
	}

	// ── menus ───────────────────────────────────────────────────────────
	void clipMenu(int l, int j) {
		Concrete* m = module;
		Menu* menu = createMenu();
		const ConcreteClip& c0 = m->clip[l][j];
		const ConcreteSample* s = m->sampleOf(c0);
		std::string nm = s ? s->name : "";
		if (s && s->missing) nm += " (missing)";
		menu->addChild(createMenuLabel(nm));
		if (s && s->sr > 0.f)
			menu->addChild(createMenuLabel(string::f("%.2f s of audio at x%.2f, %.2f bars",
				(c0.b - c0.a) / s->sr, m->clipSpeed(c0, s, m->spbSm), m->clipBars(c0, m->spbSm))));
		auto ref = [=]() -> ConcreteClip& { return m->clip[l][j]; };
		menu->addChild(createSubmenuItem("Speed", "", [=](Menu* sub) {
			static const float SP[] = {0.25f, 0.5f, 1.f, 2.f, 4.f};
			for (float v : SP)
				sub->addChild(createCheckMenuItem(string::f("x%g", v), "",
					[=]() { return ref().fit <= 0.f && std::fabs(ref().speed - v) < 1e-4f; },
					[=]() { ConcreteClip& c = ref(); c.fit = 0.f; c.speed = v; m->touch(c); }));
			sub->addChild(createMenuItem("An octave down", "", [=]() { ConcreteClip& c = ref(); c.fit = 0.f; c.speed = std::max(CC_MINSPEED, c.speed * 0.5f); m->touch(c); }));
			sub->addChild(createMenuItem("An octave up", "", [=]() { ConcreteClip& c = ref(); c.fit = 0.f; c.speed = std::min(CC_MAXSPEED, c.speed * 2.f); m->touch(c); }));
			sub->addChild(createMenuItem("A semitone down", "", [=]() { ConcreteClip& c = ref(); c.fit = 0.f; c.speed = std::max(CC_MINSPEED, c.speed / 1.0594631f); m->touch(c); }));
			sub->addChild(createMenuItem("A semitone up", "", [=]() { ConcreteClip& c = ref(); c.fit = 0.f; c.speed = std::min(CC_MAXSPEED, c.speed * 1.0594631f); m->touch(c); }));
		}));
		menu->addChild(createCheckMenuItem("Reverse", "", [=]() { return ref().rev; },
			[=]() { ConcreteClip& c = ref(); c.rev = !c.rev; m->touch(c); }));
		menu->addChild(createCheckMenuItem("Follow the tempo (stays the same number of bars)", "",
			[=]() { return ref().fit > 0.f; },
			[=]() {
				ConcreteClip& c = ref();
				if (c.fit > 0.f) { c.speed = (float)m->clipSpeed(c, m->sampleOf(c), m->spbSm); c.fit = 0.f; }
				else {
					double b = m->clipBars(c, m->spbSm);
					float g = m->gridDiv();
					if (m->snap) b = std::max(1.0 / g, std::round(b * g) / g);
					c.fit = (float)b;
				}
				m->touch(c);
			}));
		menu->addChild(createMenuItem("Restore the whole sample", "", [=]() {
			ConcreteClip& c = ref();
			const ConcreteSample* sm = m->sampleOf(c);
			if (sm && sm->n > 0) { c.a = 0.0; c.b = (double)sm->n; m->touch(c); }
		}));
		menu->addChild(createMenuItem("Duplicate after it", "", [=]() {
			ConcreteClip c = ref();
			double t = c.t0 + m->clipBars(c, m->spbSm);
			float g = m->gridDiv();
			if (m->snap) t = std::ceil(t * g - 1e-4) / g;
			if (t >= CC_MAXBARS) return;
			c.t0 = (float)t;
			m->placeClip(l, c);
		}));
		menu->addChild(createMenuItem("Delete", "Del", [=]() { ref().used = false; }));
	}

	void laneMenu(int l, float t0) {
		Concrete* m = module;
		Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("Lane %d", l + 1)));
		menu->addChild(createMenuItem(string::f("Load a sample at bar %.2f...", t0 + 1.f), "", [=]() {
			ConcreteDisplay* self = this;
			self->pendLane = l; self->pendT0 = t0;
		}));
		menu->addChild(createMenuItem("Clear this lane", "", [=]() {
			for (int j = 0; j < CC_CLIPS; j++) m->clip[l][j].used = false;
		}));
	}

	void curveMenu(int l) {
		Concrete* m = module;
		int c = m->tab - 1;
		if (l < 0 || l >= CC_N || c < 0 || c >= CC_NCURVE) return;   // the CLIPS tab has no curve
		Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("Lane %d %s", l + 1, CC_CURVE_NAME[c])));
		menu->addChild(createMenuLabel("Click to add a point, drag to move it (Shift: up and down only), double-click to remove it"));
		menu->addChild(createMenuItem("Reset this curve", "", [=]() { m->pts[l][c].clear(); m->renderCurve(l, c); }));
		menu->addChild(createMenuItem("Copy it to every lane", "", [=]() {
			for (int k = 0; k < CC_N; k++) if (k != l) { m->pts[k][c] = m->pts[l][c]; m->renderCurve(k, c); }
		}));
		menu->addChild(createMenuItem("Reset all of this lane's curves", "", [=]() {
			for (int k = 0; k < CC_NCURVE; k++) { m->pts[l][k].clear(); m->renderCurve(l, k); }
		}));
	}

	static std::string pitchText(float v) {
		float st = v * CC_PITCH_SEMIS;
		return std::fabs(st - std::round(st)) < 0.005f ? string::f("%+d st", (int)std::round(st)) : string::f("%+.2f st", st);
	}

	void pointMenu(int l, int k) {
		Concrete* m = module;
		int c = m->tab - 1;
		const auto& P = m->pts[l][c];
		if (k < 0 || k >= (int)P.size()) return;
		Menu* menu = createMenu();
		float v = P[k].v;
		std::string val = c == CC_PITCH ? pitchText(v)
		                : c == CC_VOL ? (v < 1e-3f ? "-inf dB" : string::f("%+.1f dB", 40.f * std::log10(v)))
		                : c == CC_PAN ? (std::fabs(v) < 1e-3f ? "centre" : string::f("%.0f%% %s", std::fabs(v) * 100.f, v < 0.f ? "left" : "right"))
		                : (std::fabs(v) < 0.05f ? "off" : string::f("%s %.0f%%", v < 0.f ? "lowpass" : "highpass", std::fabs(v) * 100.f));
		menu->addChild(createMenuLabel(string::f("Bar %.2f: %s", P[k].b + 1.f, val.c_str())));
		menu->addChild(createMenuItem("Remove this point", "double-click", [=]() { deletePoint(l, k); }));
	}

	// ── drawing ─────────────────────────────────────────────────────────
	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, mm2px(1.27f));
		nvgFillColor(args.vg, sfs::SCREEN_BG);
		nvgFill(args.vg);
	}

	// the filled envelope of a stretch of audio between screen columns xa..xb
	void drawWave(NVGcontext* vg, Concrete* m, const float* d, int64_t n, const std::vector<float>* peak,
	              double a, double b, bool rev, double framesPerBar, double t0, float xa, float xb,
	              float yMid, float hh, NVGcolor col) {
		// only the part in view, so a zoomed clip is drawn at the zoom's detail
		xa = std::max(xa, x0()); xb = std::min(xb, x1());
		if (n < 2 || xb - xa < 1.f) return;
		float step = std::max(1.f, (xb - xa) / 300.f);
		int cols = (int)((xb - xa) / step) + 1;
		std::vector<float> amp((size_t)cols, 0.f);
		for (int k = 0; k < cols; k++) {
			float x = xa + k * step;
			double ba = barOf(x) - t0, bb = barOf(x + step) - t0;
			double fa = ba * framesPerBar, fb = bb * framesPerBar;
			double sa = rev ? b - fb : a + fa, sb = rev ? b - fa : a + fb;
			int64_t ia = ccClamp((int64_t)std::floor(sa), (int64_t)0, n - 1), ib = ccClamp((int64_t)std::ceil(sb), (int64_t)0, n - 1);
			float pk = 0.f;
			if (peak && !peak->empty() && ib - ia >= 2 * CC_PEAK) {
				for (int64_t q = ia / CC_PEAK; q <= ib / CC_PEAK && q < (int64_t)peak->size(); q++) pk = std::max(pk, (*peak)[(size_t)q]);
			} else {
				int64_t st = std::max((int64_t)1, (ib - ia) / 48);
				for (int64_t q = ia; q <= ib; q += st) pk = std::max(pk, std::max(std::fabs(d[2 * q]), std::fabs(d[2 * q + 1])));
			}
			amp[(size_t)k] = std::min(1.f, pk);
		}
		nvgBeginPath(vg);
		for (int k = 0; k < cols; k++) {
			float x = std::min(xb, xa + k * step), y = yMid - amp[(size_t)k] * hh;
			if (k == 0) nvgMoveTo(vg, x, y); else nvgLineTo(vg, x, y);
		}
		for (int k = cols - 1; k >= 0; k--) nvgLineTo(vg, std::min(xb, xa + k * step), yMid + amp[(size_t)k] * hh);
		nvgClosePath(vg);
		nvgFillColor(vg, col);
		nvgFill(vg);
	}

	// While a trim is being dragged: where the edge is in the sample, and with
	// the drag slowed, the audio round it at the scale the mouse is moving, so the
	// wave slides under the edge line one pixel per pixel. Kept audio is bright,
	// trimmed audio dim.
	void drawTrim(NVGcontext* vg, Concrete* m, bool hasFont) {
		if ((drag != D_TRIML && drag != D_TRIMR) || dClip < 0 || dLane < 0) return;
		const ConcreteClip& c = m->clip[dLane][dClip];
		const ConcreteSample* sm = m->sampleOf(c);
		if (!c.used || !sm || sm->missing || sm->n < 2 || sm->sr <= 0.f) return;
		float s = sc(), y = laneY(dLane), h = laneH();
		bool left = drag == D_TRIML;
		double edge = left == c.rev ? c.b : c.a;
		double t = c.t0 + (left ? 0.0 : m->clipBars(c, m->spbSm));
		int fine = fineNow();
		float top = y + 2.f * s, bot = y + h - 2.f * s, ex = xOf(t);
		nvgSave(vg);
		nvgScissor(vg, x0(), y, x1() - x0(), h);
		float lw = 0.f, lx = ex;
		if (fine > 1) {
			lw = 170.f * s;
			lx = clamp(ex - lw * 0.5f, x0(), x1() - lw);
			nvgBeginPath(vg);
			nvgRect(vg, lx, top, lw, bot - top);
			nvgFillColor(vg, sfs::SCREEN_BG);
			nvgFill(vg);
			double perPx = m->spbSm * sm->sr * m->clipSpeed(c, sm, m->spbSm) * vLen() / (x1() - x0()) / fine;
			double dir = c.rev ? -1.0 : 1.0;
			int cols = std::max(2, (int)lw);
			std::vector<float> lo((size_t)cols), hi((size_t)cols);
			for (int k = 0; k < cols; k++) {
				double fa = edge + dir * perPx * (lx + k - ex), fb = edge + dir * perPx * (lx + k + 1 - ex);
				if (fa > fb) std::swap(fa, fb);
				float mn = 1.f, mx = -1.f;
				if (fb >= 0.0 && fa < (double)(sm->n - 1)) {
					int64_t ia = ccClamp((int64_t)std::floor(fa), (int64_t)0, sm->n - 2), ib = ccClamp((int64_t)std::ceil(fb), ia + 1, sm->n - 1);
					int64_t st = std::max((int64_t)1, (ib - ia) / 32);
					for (int64_t q = ia; q <= ib; q += st) {
						float v = 0.5f * (sm->d[2 * q] + sm->d[2 * q + 1]);
						mn = std::min(mn, v); mx = std::max(mx, v);
					}
				}
				if (mn > mx) mn = mx = 0.f;
				lo[(size_t)k] = mn; hi[(size_t)k] = mx;
			}
			float mid = (top + bot) * 0.5f, hh = (bot - top) * 0.5f - 1.f;
			int split = clamp((int)(ex - lx), 0, cols - 1);
			for (int side = 0; side < 2; side++) {
				int ka = side ? split : 0, kb = side ? cols - 1 : split;
				if (kb <= ka) continue;
				nvgBeginPath(vg);
				for (int k = ka; k <= kb; k++) {
					float yy = mid - clamp(hi[(size_t)k], -1.f, 1.f) * hh - 0.5f;
					if (k == ka) nvgMoveTo(vg, lx + k, yy); else nvgLineTo(vg, lx + k, yy);
				}
				for (int k = kb; k >= ka; k--) nvgLineTo(vg, lx + k, mid - clamp(lo[(size_t)k], -1.f, 1.f) * hh + 0.5f);
				nvgClosePath(vg);
				bool kept = left == (side == 1);
				nvgFillColor(vg, nvgTransRGBAf(CC_LANE_COL[dLane], kept ? 1.f : 0.28f));
				nvgFill(vg);
			}
			nvgBeginPath(vg);
			nvgRect(vg, lx + 0.5f, top + 0.5f, lw - 1.f, bot - top - 1.f);
			nvgStrokeColor(vg, sfs::SCREEN_TEXT);
			nvgStrokeWidth(vg, 0.8f);
			nvgStroke(vg);
		}
		nvgBeginPath(vg);
		nvgRect(vg, ex - 0.5f, top, 1.f, bot - top);
		nvgFillColor(vg, sfs::SCREEN_TEXT);
		nvgFill(vg);
		if (hasFont) {
			std::string txt = string::f("%s %.4f s", left ? "START" : "END", edge / sm->sr);
			if (fine > 1) txt += string::f("  1/%d", fine);
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			float tw = nvgTextBounds(vg, 0.f, 0.f, txt.c_str(), NULL, NULL);
			float tx = fine > 1 ? lx + 3.f * s : clamp(left ? ex + 3.f * s : ex - tw - 3.f * s, x0() + 1.f, x1() - tw - 1.f);
			nvgBeginPath(vg);
			nvgRect(vg, tx - 1.5f * s, top + 0.5f, tw + 3.f * s, 9.f * s);
			nvgFillColor(vg, sfs::SCREEN_BG);
			nvgFill(vg);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			nvgText(vg, tx, top + 1.f * s, txt.c_str(), NULL);
		}
		nvgRestore(vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) return;
		if (!module) { module = concretePreview(); drawLayer(args, layer); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0) font = sfs::screenFontFace();
		bool hasFont = font && font->handle >= 0;
		Concrete* m = module;
		float s = sc(), W = box.size.x;
		int Lp = m->L;
		bool curves = m->tab > 0;

		// header: tabs, then the tempo and the position
		static const char* TAB[CC_NCURVE + 1] = {"CLIPS", "VOL", "FILTER", "PAN", "PITCH"};
		for (int t = 0; t <= CC_NCURVE; t++) {
			float x = 8.f * s + t * CC_TABW * s;
			nvgBeginPath(vg);
			nvgRect(vg, x, 5.f * s, (CC_TABW - 5.f) * s, 15.f * s);
			nvgFillColor(vg, t == m->tab ? sfs::SCREEN_DEEP : sfs::SCREEN_PURP);
			nvgFill(vg);
			if (hasFont) {
				sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, t == m->tab ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM);
				nvgText(vg, x + (CC_TABW - 5.f) * 0.5f * s, 12.5f * s, TAB[t], NULL);
			}
		}
		if (hasFont) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			std::string tempo = m->srcMode == 0 ? string::f("%.1f BPM", 240.0 / m->spbSm)
			                  : string::f("%.2f s/bar  %s", m->spbSm, m->srcMode == 1 ? string::f("CLK %d", m->cpb).c_str() : "BAR");
			bool anyRec = false;
			for (int l = 0; l < CC_N; l++) anyRec |= m->recNow[l];
			std::string pos = string::f("%d.%02d / %d", (int)m->P + 1, (int)((m->P - std::floor(m->P)) * 100.0), Lp);
			nvgFillColor(vg, sfs::SCREEN_DIM);
			nvgText(vg, W - 8.f * s, 12.5f * s, (tempo + "    " + pos).c_str(), NULL);
			if (anyRec) {
				nvgFillColor(vg, nvgRGB(0xE0, 0x30, 0x30));
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
				nvgText(vg, 8.f * s + (CC_NCURVE + 1) * CC_TABW * s + 4.f * s, 12.5f * s, "REC", NULL);
			}
		}

		// the view: 1, 2 or 4 bars, or the whole loop
		static const char* ZB[4] = {"1", "2", "4", "ALL"};
		for (int b = 0; b < 4; b++) {
			float x = (CC_ZX + b * CC_ZW) * s, w = (CC_ZW - 3.f) * s;
			bool on = b < 3 ? (zoomed() && (int)vLen() == CC_VIEWS[b]) : !zoomed();
			bool live = b == 3 || CC_VIEWS[b] < Lp;
			nvgBeginPath(vg);
			nvgRect(vg, x, 5.f * s, w, 15.f * s);
			nvgFillColor(vg, on ? sfs::SCREEN_DEEP : sfs::SCREEN_PURP);
			nvgFill(vg);
			if (hasFont) {
				sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, on ? sfs::SCREEN_TEXT : live ? sfs::SCREEN_DIM : sfs::SCREEN_PMID);
				nvgText(vg, x + w * 0.5f, 12.5f * s, ZB[b], NULL);
			}
		}
		if (hasFont) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_PMID);
			nvgText(vg, CC_ZX * s - 3.f * s, 12.5f * s, "BARS", NULL);
		}

		// the overview: every lane's clips across the whole loop, the view, the playhead
		{
			float oT = ovTop(), oB = ovBot(), oh = (oB - oT) / CC_N;
			nvgBeginPath(vg);
			nvgRect(vg, x0(), oT, x1() - x0(), oB - oT);
			nvgFillColor(vg, nvgRGB(0x22, 0x22, 0x3A));
			nvgFill(vg);
			for (int l = 0; l < CC_N; l++)
				for (int j = 0; j < CC_CLIPS; j++) {
					const ConcreteClip& c = m->clip[l][j];
					if (!c.used || c.t0 >= Lp) continue;
					float xa = ovX(c.t0), xb = std::max(ovX(m->clipEnd(l, j, m->spbSm, Lp)), xa + 1.f);
					nvgBeginPath(vg);
					nvgRect(vg, xa, oT + oh * l + 0.3f, xb - xa, oh - 0.6f);
					nvgFillColor(vg, nvgTransRGBAf(CC_LANE_COL[l], 0.75f));
					nvgFill(vg);
				}
			if (zoomed()) {
				nvgBeginPath(vg);
				nvgRect(vg, ovX(vStart()), oT - 0.5f, ovX(vStart() + vLen()) - ovX(vStart()), oB - oT + 1.f);
				nvgStrokeColor(vg, sfs::SCREEN_TEXT);
				nvgStrokeWidth(vg, 1.f);
				nvgStroke(vg);
			}
			nvgBeginPath(vg);
			nvgRect(vg, ovX(m->P) - 0.5f, oT, 1.f, oB - oT);
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			nvgFill(vg);
		}

		// ruler and the beat grid: every second beat shaded across the lanes,
		// a line on every beat and a brighter one on every bar; the ruler
		// ticks the clock, taller on the beat, with the beats numbered
		// once there is room for them
		float rT = rulerTop(), lT = lanesTop(), bot = laneY(CC_N);
		nvgSave(vg);
		nvgScissor(vg, x0() - 1.f, rT, x1() - x0() + 2.f, bot - rT);
		int beats = beatsPerBar();
		double vs = vStart(), ve = vStart() + vLen();
		for (int q = (int)std::floor(vs * beats); q < (int)std::ceil(ve * beats); q++) {
			if (q % 2 == 0) continue;
			float xa = xOf((double)q / beats), xb = xOf((double)(q + 1) / beats);
			nvgBeginPath(vg);
			nvgRect(vg, xa, lT, xb - xa, bot - lT);
			nvgFillColor(vg, nvgRGB(0x20, 0x20, 0x3C));
			nvgFill(vg);
		}
		float g = m->gridDiv();
		int every = std::max(1, (int)std::ceil(g * vLen() * 3.f * s / (x1() - x0())));
		int iA = std::max(0, (int)std::floor(vs * g)), iB = (int)std::ceil(ve * g);
		iA -= iA % every;
		int perBeat = ((int)g % beats == 0) ? (int)g / beats : 0;
		for (int i = iA; i <= iB; i += every) {
			bool barLine = (i % (int)g) == 0;
			bool beat = !barLine && perBeat > 0 && (i % perBeat) == 0;
			float x = xOf((double)i / g);
			nvgBeginPath(vg);
			if (barLine) nvgRect(vg, x - 0.6f, rT + 2.f * s, 1.2f, bot - rT - 2.f * s);
			else if (beat) nvgRect(vg, x - 0.5f, lT - 8.f * s, 1.f, bot - lT + 8.f * s);
			else nvgRect(vg, x - 0.5f, lT - 4.f * s, 1.f, 4.f * s);
			nvgFillColor(vg, barLine ? nvgRGB(0x60, 0x60, 0x86) : beat ? nvgRGB(0x34, 0x34, 0x54) : sfs::SCREEN_PURP);
			nvgFill(vg);
		}
		if (hasFont) {
			sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, sfs::SCREEN_TEXT);
			for (int b = 0; b < Lp; b++) nvgText(vg, xOf(b) + 2.5f * s, rT + 6.f * s, string::f("%d", b + 1).c_str(), NULL);
			float beatW = (x1() - x0()) / (float)(vLen() * beats);
			if (beatW > 36.f * s) {
				nvgFillColor(vg, sfs::SCREEN_PMID);
				for (int q = (int)std::floor(vs * beats); q < (int)std::ceil(ve * beats); q++)
					if (q % beats) nvgText(vg, xOf((double)q / beats) + 2.5f * s, rT + 6.f * s, string::f("%d.%d", q / beats + 1, q % beats + 1).c_str(), NULL);
			}
		}
		nvgRestore(vg);

		// lanes
		int hl = -1, hpart = P_NONE;
		int hj = (hovering && m->tab == 0 && drag == D_NONE) ? clipAt(hover, hl, hpart) : -1;
		for (int l = 0; l < CC_N; l++) {
			float y = laneY(l), h = laneH();
			NVGcolor lc = CC_LANE_COL[l];
			nvgBeginPath(vg);
			nvgRect(vg, x0(), y + h - 0.7f * s, x1() - x0(), 0.7f * s);
			nvgFillColor(vg, sfs::SCREEN_PURP);
			nvgFill(vg);
			nvgBeginPath(vg);
			nvgRect(vg, 8.f * s, y + 3.f * s, 3.f * s, h - 6.f * s);
			nvgFillColor(vg, lc);
			nvgFill(vg);
			if (hasFont) {
				sfs::screenFont(vg, font);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, m->recNow[l] ? nvgRGB(0xE0, 0x30, 0x30) : sfs::SCREEN_TEXT);
				nvgText(vg, 21.f * s, y + h * 0.5f, string::f("%d", l + 1).c_str(), NULL);
			}

			nvgSave(vg);
			nvgScissor(vg, x0(), y, x1() - x0(), h);
			float dim = curves ? 0.35f : 1.f;
			float yMid = y + h * 0.5f, hh = h * 0.5f - 4.f * s;
			// clips, earliest first so a later one is drawn over the end of it
			int order[CC_CLIPS], no = 0;
			for (int j = 0; j < CC_CLIPS; j++) if (m->clip[l][j].used && m->clip[l][j].t0 < Lp) order[no++] = j;
			std::sort(order, order + no, [&](int p, int q) { return m->clip[l][p].t0 < m->clip[l][q].t0; });
			for (int q = 0; q < no; q++) {
				int j = order[q];
				const ConcreteClip& c = m->clip[l][j];
				const ConcreteSample* sm = m->sampleOf(c);
				double e = m->clipEnd(l, j, m->spbSm, Lp);
				float xa = xOf(c.t0), xb = std::max(xOf(e), xa + 2.f);
				bool hot = (j == hj && l == hl) || (drag != D_NONE && dClip == j && dLane == l);
				nvgBeginPath(vg);
				nvgRect(vg, xa, y + 2.f * s, xb - xa, h - 4.f * s);
				nvgFillColor(vg, nvgTransRGBAf(lc, 0.16f * dim));
				nvgFill(vg);
				if (sm && !sm->missing && sm->n > 1) {
					double sp = m->clipSpeed(c, sm, m->spbSm);
					drawWave(vg, m, sm->d.data(), sm->n, &sm->peak, c.a, c.b, c.rev, m->spbSm * sm->sr * sp, c.t0,
					         xa, xb, yMid, hh, nvgTransRGBAf(lc, 0.85f * dim));
				}
				nvgBeginPath(vg);
				nvgRect(vg, xa + 0.5f, y + 2.5f * s, xb - xa - 1.f, h - 5.f * s);
				bool sel = selValid() && l == selLane && j == selClip;
				nvgStrokeColor(vg, sel || hot ? nvgTransRGBAf(sfs::SCREEN_TEXT, (sel ? 1.f : 0.9f) * dim) : nvgTransRGBAf(lc, 0.7f * dim));
				nvgStrokeWidth(vg, sel ? 1.8f : hot ? 1.2f : 0.8f);
				nvgStroke(vg);
				if (hot && hpart != P_BODY && hpart != P_NONE && drag == D_NONE) {
					float ex = hpart == P_LEFT ? xa : xb - 2.f * s;
					nvgBeginPath(vg);
					nvgRect(vg, ex, y + 2.f * s, 2.f * s, h - 4.f * s);
					nvgFillColor(vg, sfs::SCREEN_TEXT);
					nvgFill(vg);
				}
				// a clip that starts off the left of a zoomed view keeps its name in sight
				float xt = std::max(xa, x0());
				if (hasFont && xb - xt > 14.f * s) {
					sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
					nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
					std::string label = sm ? sm->name : "?";
					if (sm && sm->missing) label += " missing";
					double sp = m->clipSpeed(c, sm, m->spbSm);
					if (c.fit > 0.f) label += string::f("  FIT %g", std::round(c.fit * 100.f) / 100.f);
					else if (std::fabs(sp - 1.0) > 1e-3) label += string::f("  x%.2f", sp);
					if (c.rev) label += "  REV";
					nvgSave(vg);
					nvgIntersectScissor(vg, xt, y, xb - xt - 2.f * s, h);
					nvgFillColor(vg, nvgTransRGBAf(sm && sm->missing ? nvgRGB(0xE0, 0x50, 0x50) : sfs::SCREEN_TEXT, dim));
					nvgText(vg, xt + 3.f * s, y + 4.f * s, label.c_str(), NULL);
					nvgRestore(vg);
				}
			}
			// takes being recorded, or waiting to become clips
			for (int k = 0; k < 2; k++) {
				ConcreteTake& T = m->take[l][k];
				int st = T.state.load();
				if (st != TK_REC && st != TK_SEALED) continue;
				int64_t n = T.n.load();
				float xa = xOf(T.t0), xb = std::max(xOf(st == TK_REC ? m->P : T.t1), xa + 1.f);
				NVGcolor rc = nvgRGB(0xE0, 0x30, 0x30);
				nvgBeginPath(vg);
				nvgRect(vg, xa, y + 2.f * s, xb - xa, h - 4.f * s);
				nvgFillColor(vg, nvgTransRGBAf(rc, 0.18f));
				nvgFill(vg);
				drawWave(vg, m, T.buf.data(), n, nullptr, 0.0, (double)n, false, m->spbSm * T.sr, T.t0, xa, xb, yMid, hh, nvgTransRGBAf(rc, 0.85f));
			}
			// the curve
			if (curves) {
				int c = m->tab - 1;
				float yT = curveTop(l), yB = curveBot(l);
				auto yOf = [&](float v) { return yOfV(l, c, v); };
				float yZero = c == CC_VOL ? yB : (yT + yB) * 0.5f;
				// drawn from the points themselves, so the line goes through every one
				const auto& Pc = m->pts[l][c];
				float vFirst = Pc.empty() ? CC_CURVE_DEF[c] : Pc.front().v, vLast = Pc.empty() ? CC_CURVE_DEF[c] : Pc.back().v;
				nvgBeginPath(vg);
				nvgMoveTo(vg, xOf(0), yZero);
				nvgLineTo(vg, xOf(0), yOf(vFirst));
				for (const auto& q : Pc) nvgLineTo(vg, xOf(q.b), yOf(q.v));
				nvgLineTo(vg, xOf(CC_MAXBARS), yOf(vLast));
				nvgLineTo(vg, xOf(CC_MAXBARS), yZero);
				nvgClosePath(vg);
				nvgFillColor(vg, nvgTransRGBAf(lc, 0.25f));
				nvgFill(vg);
				if (c != CC_VOL) {
					nvgBeginPath(vg);
					nvgRect(vg, x0(), yZero - 0.4f, x1() - x0(), 0.8f);
					nvgFillColor(vg, sfs::SCREEN_PMID);
					nvgFill(vg);
				}
				nvgBeginPath(vg);
				nvgMoveTo(vg, xOf(0), yOf(vFirst));
				for (const auto& q : Pc) nvgLineTo(vg, xOf(q.b), yOf(q.v));
				nvgLineTo(vg, xOf(CC_MAXBARS), yOf(vLast));
				nvgStrokeColor(vg, lc);
				nvgStrokeWidth(vg, 1.4f);
				nvgStroke(vg);
				// the points
				int hk = hovering && drag == D_NONE && laneAt(hover.y) == l ? ptAt(l, hover) : -1;
				const auto& Pp = m->pts[l][c];
				for (int k = 0; k < (int)Pp.size(); k++) {
					if (Pp[k].b > Lp + 1e-4f) continue;
					bool hot = k == hk || (drag == D_POINT && dLane == l && dPt == k);
					nvgBeginPath(vg);
					nvgCircle(vg, xOf(Pp[k].b), yOf(Pp[k].v), (hot ? 4.f : 3.f) * s);
					nvgFillColor(vg, hot ? sfs::SCREEN_TEXT : lc);
					nvgFill(vg);
					nvgStrokeColor(vg, sfs::SCREEN_BG);
					nvgStrokeWidth(vg, 1.f);
					nvgStroke(vg);
					if (hasFont && c == CC_PITCH && drag == D_POINT && dLane == l && dPt == k) {
						float px = xOf(Pp[k].b);
						bool toLeft = px > x1() - 40.f * s;
						sfs::screenFont(vg, font, sfs::TYPE_SCREEN_SMALL);
						nvgTextAlign(vg, (toLeft ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT) | NVG_ALIGN_MIDDLE);
						nvgFillColor(vg, sfs::SCREEN_TEXT);
						nvgText(vg, px + (toLeft ? -6.f : 6.f) * s, clamp(yOf(Pp[k].v), y + 6.f * s, y + h - 6.f * s), pitchText(Pp[k].v).c_str(), NULL);
					}
				}
			}
			nvgRestore(vg);
		}

		// the playhead
		float px = xOf(m->P);
		if (px >= x0() - 1.f && px <= x1() + 1.f) {
			nvgBeginPath(vg);
			nvgRect(vg, px - 0.6f, rT, 1.2f, bot - rT);
			nvgFillColor(vg, m->moving ? sfs::SCREEN_TEXT : sfs::SCREEN_DIM);
			nvgFill(vg);
		}
		drawTrim(vg, m, hasFont);
		Widget::drawLayer(args, layer);
	}
};

// =============================================================================
// Panel: 32HP, code-drawn until the Figma pass. The screen across the top;
// under it the tempo (BPM, or CLOCK under it), the loop LENGTH (BAR under it),
// PLAY and RESET over their jacks, then each lane's output, L over R, and the
// mix on its own plate at the right.
// =============================================================================
static const float CC_SX = 5.08f, CC_SY = 10.16f, CC_SW = 152.4f, CC_SH = 83.82f;
static const float CC_Y_CTL = 104.14f, CC_Y_JACK = 119.38f;
static const float CC_C1 = 12.7f, CC_C2 = 27.94f, CC_C3 = 43.18f, CC_C4 = 55.88f;
static const float CC_X_LANE = 96.52f, CC_X_LPITCH = 12.7f, CC_X_MIX = 151.13f, CC_X_LR = 86.36f;

struct ConcreteWidget : ModuleWidget {
	ConcreteDisplay* screen = nullptr;

	// ModuleWidget::onHoverKey removes the module on Delete or Backspace, press
	// or repeat, before it offers the key to its children. Over the screen those
	// keys mean a clip or a point, so they stop here.
	void onHoverKey(const HoverKeyEvent& e) override {
		if (screen && ConcreteDisplay::isDeleteKey(e.key) && screen->box.contains(e.pos)) {
			if (e.action == GLFW_PRESS) screen->deleteUnder(e.pos.minus(screen->box.pos));
			e.consume(this);
			return;
		}
		ModuleWidget::onHoverKey(e);
	}

	ConcreteWidget(Concrete* module) {
		setModule(module);
		setPanel(sfs::themedPanel("concrete"));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("CONCRÈTE");

		ConcreteDisplay* disp = new ConcreteDisplay();
		disp->module = module;
		screen = disp;
		disp->box.pos = mm2px(Vec(CC_SX, CC_SY));
		disp->box.size = mm2px(Vec(CC_SW, CC_SH));
		addChild(disp);

		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(CC_C1, CC_Y_CTL)), module, Concrete::BPM_PARAM));
		lbl->rogan(CC_C1, CC_Y_CTL, "BPM");
		addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(CC_C2, CC_Y_CTL)), module, Concrete::LENGTH_PARAM));
		lbl->rogan(CC_C2, CC_Y_CTL, "BARS");
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(mm2px(Vec(CC_C3, CC_Y_CTL)), module, Concrete::PLAY_PARAM, Concrete::PLAY_LIGHT));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CC_C3, CC_Y_JACK)), module, Concrete::PLAY_INPUT));
		// on the knobs' baseline, so the row of labels reads as one line
		lbl->add(CC_C3, CC_Y_CTL - sfs::LABEL_GAP_ROGAN, "PLAY");
		lbl->link(CC_C3, CC_Y_CTL, CC_C3, CC_Y_JACK);
		addParam(createParamCentered<VCVButton>(mm2px(Vec(CC_C4, CC_Y_CTL)), module, Concrete::RESET_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CC_C4, CC_Y_JACK)), module, Concrete::RESET_INPUT));
		lbl->add(CC_C4, CC_Y_CTL - sfs::LABEL_GAP_ROGAN, "RESET");
		lbl->link(CC_C4, CC_Y_CTL, CC_C4, CC_Y_JACK);
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CC_C1, CC_Y_JACK)), module, Concrete::CLOCK_INPUT));
		lbl->jack(CC_C1, CC_Y_JACK, "CLOCK");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CC_C2, CC_Y_JACK)), module, Concrete::BAR_INPUT));
		lbl->jack(CC_C2, CC_Y_JACK, "BAR");

		// the outputs: L on the control row, R on the jack row
		lbl->add(CC_X_LR, CC_Y_CTL + 0.83f, "L");
		lbl->add(CC_X_LR, CC_Y_JACK + 0.83f, "R");
		for (int l = 0; l < CC_N; l++) {
			float x = CC_X_LANE + CC_X_LPITCH * l;
			addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(x, CC_Y_CTL)), module, Concrete::LANE_L_OUTPUT + l));
			addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(x, CC_Y_JACK)), module, Concrete::LANE_R_OUTPUT + l));
			lbl->jackOnPlate(x, CC_Y_CTL, string::f("%d", l + 1));
		}
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(CC_X_MIX, CC_Y_CTL)), module, Concrete::L_OUTPUT));
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(CC_X_MIX, CC_Y_JACK)), module, Concrete::R_OUTPUT));
		lbl->jackOnPlate(CC_X_MIX, CC_Y_CTL, "MIX");
	}

	void step() override {
		ModuleWidget::step();
		if (Concrete* m = dynamic_cast<Concrete*>(module)) m->uiService();
	}

	void appendContextMenu(Menu* menu) override {
		Concrete* m = dynamic_cast<Concrete*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Snap to the clock grid (Ctrl/Cmd while dragging inverts)", "", &m->snap));
		std::vector<std::string> opts;
		for (int i = 0; i < CC_NCPB; i++) opts.push_back(string::f("%d", CC_CPB_OPT[i]));
		menu->addChild(createIndexSubmenuItem("Clocks per bar with BAR unpatched", opts,
			[=]() { return m->cpbIdx; }, [=](size_t i) { m->cpbIdx = (int)i; }));
		menu->addChild(createBoolPtrMenuItem("Reset when PLAY starts", "", &m->resetOnPlay));
		menu->addChild(createBoolPtrMenuItem("Zoomed view follows the playhead", "", &m->viewFollow));
		menu->addChild(createBoolPtrMenuItem("Keep copies of loaded samples in the patch", "", &m->embed));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Clear every lane", "", [=]() {
			for (int l = 0; l < CC_N; l++) for (int j = 0; j < CC_CLIPS; j++) m->clip[l][j].used = false;
		}));
		menu->addChild(createMenuItem("Reset every curve", "", [=]() { m->resetCurves(); }));
	}
};

Model* modelConcrete = createModel<Concrete, ConcreteWidget>("Concrete");

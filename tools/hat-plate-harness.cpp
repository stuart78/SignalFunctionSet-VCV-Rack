// A hi-hat as two plates that touch. Prototype for Kit, standalone (no Rack).
//
// Kit's membrane cannot be a hat however it is set (tools/kit-voice-harness.py:
// 92-96% of a "hat" below 500 Hz). Three things are missing, and this tests
// whether supplying them physically gets there:
//   1. contact time: a stick on bronze is ~0.1-0.3 ms, not 1 ms
//   2. density: a plate has hundreds of modes above 2 kHz, a membrane bank 40
//   3. nonlinearity: the plates COLLIDE. Every collision is an impulsive force
//      into both plates, broadband, so energy is carried from the low modes
//      that ring to the high ones that sizzle. That is the cascade a linear
//      bank cannot make, and it is the one thing the pedal controls.
//
//   clang++ -std=c++11 -O2 tools/hat-plate-harness.cpp -o /tmp/hat && /tmp/hat [outdir]
//
// The defaults are round Q1, settled by ear on 2026-09-23. Above 5 kHz the
// plate is a statistical field (band energies heard as noise), because the
// ~3000 modes a real plate has there are not affordable and not resolvable.
// Every knob overrides from the environment (see main()). The model, what each
// listening round taught and what is left for Kit: docs/kit-hat-design.md.
//
// Units are normalised: modal mass 1, displacement in "gap units" (the contact
// geometry is stated in the same unit, and the strike is scaled so a normal hit
// moves the rim about one of them).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <complex>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <functional>

static const float SR = 48000.f;
static const float DT = 1.f / SR;
static float hfLoss = 0.f;     // T60 ~ f^-hfLoss
// Knobs for the listening rounds, all from the environment (see main()).
static float gP = 1.5f;       // mode grid exponent: lower = denser
static float gFJ = 0.03f;     // random mode detune, fraction
static int   gNC = 8;         // contact points
static float gWarp = 0.2f;     // per-point gap irregularity, gap units
static float gK3 = 3e6f;       // cubic rim stiffening (the plate's own nonlinearity)
static float gTM = 0.f;       // pitch rise with amplitude (tension modulation)
static float gF2 = 220.f;     // the body's first bending mode, top plate (bottom +9%)
static float gFX = 5000.f;    // explicit modes stop here; above it the plate is a field
static int   gSEA = 1;        // the statistical field on/off, for A/B
static float gDENS = 0.1f;    // modal density of the real plate, modes/Hz
static float gLEAK = 150.f;    // explicit -> field transfer, 1/s at a full hit
static float gCASC = 40.f;    // band -> next band cascade, 1/s at a full hit
static float gSEAG = 2.5f;     // field output trim
static float gTMAX = 2.5f;    // the clutch: no mode rings longer than this (s)
static float gCORR = 0.6f;     // field noise shared between L and R, 0..1
static float gSPREAD = 1.f;   // share of the leak landing across every band at once (vs band 0 then cascading)
static float gSLOPE = 0.f;    // that spread's tilt: energy per Hz ~ f^gSLOPE
static float gNLD = 1000.f;      // loss that grows with amplitude, 1/s at a full hit (the cascade's dissipation)
static float gNLDF = 1.f;     // share of that loss the field takes directly (0 = it follows the body through the leak)
static float gRTX = 3.f;     // radiation tilt for the explicit modes (-1 = same as the field)
typedef std::complex<float> cf;

static uint32_t rng = 0x9e3779b9u;
static float frand() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) * (1.f / 16777216.f); }

// ── one plate ───────────────────────────────────────────────────────────────
// Modes on an (m, n) grid, m nodal diameters and n nodal circles, with
// Chladni's law generalised, f ~ (m + 2n)^p (p = 2 for a flat plate, lower
// for a cymbal's profile). A modes with m > 0 is a degenerate pair on a
// perfect disc; a hammered, lathed cymbal is not one, so each pair splits a
// fraction of a percent, and that splitting is much of what "metal" sounds like.
struct Plate {
	struct Mode { float f, w, m, sig; cf pole; cf s; float phase; int mm, nn; float rad; };
	std::vector<Mode> modes;
	// shape at a point, precomputed per point
	std::vector<float> phiStrike, phiMic[2];
	std::vector<std::vector<float>> phiContact;   // [point][mode]

	static float shape(const Mode& M, float r, float th) {
		// radial: ~0 at the clamped centre for m > 0, an antinode at the free
		// rim, n oscillations between. Not a Bessel solution -- a cymbal's
		// profile is not a flat plate's either -- but it puts the right things
		// at the rim, which is where the contact and the stick are.
		float rr = std::pow(r, (float)std::min(M.mm, 3) * 0.5f + 0.5f);
		float rad = std::cos((float)M_PI * (M.nn + 0.5f) * (1.f - r));
		return rr * rad * std::cos(M.mm * th + M.phase);
	}

	void build(float f2, float p, float fmax, float t60at1k, float split, float massScale) {
		modes.clear();
		for (int n = 0; n < 40; n++)
			for (int m = 0; m < 80; m++) {
				if (m == 0 && n == 0) continue;           // rigid body, held by the clutch
				if (m == 1 && n == 0) continue;           // rocking, ditto
				float k = m + 2.f * n;
				float f = f2 * std::pow(k / 2.f, p);
				if (f > fmax) continue;
				int copies = m > 0 ? 2 : 1;
				for (int c = 0; c < copies; c++) {
					Mode M;
					M.mm = m; M.nn = n;
					float det = (c == 0 ? -1.f : 1.f) * split * (0.3f + frand());
					M.f = f * (1.f + (m > 0 ? det : 0.f)) * (1.f + 2.f * gFJ * (frand() - 0.5f));
					M.w = 2.f * (float)M_PI * M.f;
					M.m = massScale;
					M.phase = c * (float)M_PI * 0.5f + 0.3f * frand();
					// Loss rises with frequency, gently: a cymbal's highs outlast
					// a drum's by far, which is why a hat can sizzle for a second.
					// The clutch felts and the rod hold the plate at its centre, and
					// they take the low modes hardest: an open hat stays bright as
					// it dies, where a free plate sinks into its own body.
					float t60 = std::min(gTMAX, t60at1k * std::pow(M.f / 1000.f, -hfLoss));
					float sig = 6.9078f / t60;
					M.sig = sig;
					M.pole = std::exp(cf(-sig * DT, -M.w * DT));
					M.s = 0.f;
					modes.push_back(M);
				}
			}
	}
	void points(float rs, float ths, const float* micR, const float* micT,
	            const std::vector<float>& cth) {
		size_t N = modes.size();
		phiStrike.resize(N);
		for (int c = 0; c < 2; c++) phiMic[c].resize(N);
		phiContact.assign(cth.size(), std::vector<float>(N));
		for (size_t k = 0; k < N; k++) {
			phiStrike[k] = shape(modes[k], rs, ths);
			for (int c = 0; c < 2; c++) phiMic[c][k] = shape(modes[k], micR[c], micT[c]);
			for (size_t j = 0; j < cth.size(); j++) phiContact[j][k] = shape(modes[k], 1.f, cth[j]);
		}
	}
	void clear() { for (auto& M : modes) M.s = 0.f; }   // (the hat clears its field in run())
	// PRESSING. A plate held against another along its rim is damped there in
	// proportion to how hard it is held and how much each mode moves at the
	// rim -- the same closed form Kit's MUFFLE uses. Done in the modes, where
	// it is exact at any strength; as a dashpot in the contact it is an
	// explicit stiff term across three hundred modes and it blows up.
	// pen0(th) is the preload round the whole rim: the clutch presses the
	// plates together all the way round, harder on the side the tilt closes.
	template <typename F>
	void press(F pen0, float D) {
		const int NA = 16;
		for (size_t k = 0; k < modes.size(); k++) {
			float add = 0.f;
			for (int a = 0; a < NA; a++) {
				float th = 2.f * (float)M_PI * a / NA, p0 = pen0(th);
				if (p0 > 0.f) { float ph = shape(modes[k], 1.f, th); add += p0 * ph * ph * (3.f / NA); }
			}
			modes[k].pole = std::exp(cf(-(modes[k].sig + D * add) * DT, -modes[k].w * DT));
		}
	}
	float sumPhi2(int j) const { float a = 0; for (size_t k = 0; k < modes.size(); k++) a += phiContact[j][k] * phiContact[j][k] / modes[k].m; return a; }
	// displacement at contact point j: x = -Im(s)/w
	float disp(int j) const {
		float x = 0.f; const float* ph = phiContact[j].data();
		for (size_t k = 0; k < modes.size(); k++) x -= ph[k] * modes[k].s.imag() / modes[k].w;
		return x;
	}
	float vel(int j) const {
		float v = 0.f; const float* ph = phiContact[j].data();
		for (size_t k = 0; k < modes.size(); k++) v += ph[k] * modes[k].s.real();
		return v;
	}
	void push(const std::vector<float>& phi, float F) {
		for (size_t k = 0; k < modes.size(); k++) modes[k].s += cf(F * phi[k] * DT / modes[k].m, 0.f);
	}
	void step() { for (auto& M : modes) M.s *= M.pole; }
	// With the frequencies raised by a fraction `up` this sample: a plate that
	// is moving hard is stiffer, so every mode goes sharp and glides back down
	// as it settles. That glide smears the partials the way a bell never is.
	void step(float up) {
		if (up <= 0.f) { step(); return; }
		for (auto& M : modes) {
			float th = M.w * up * DT;
			M.s *= M.pole * cf(1.f - 0.5f * th * th, -th);
		}
	}
	// Radiated: modal velocity, weighted by radiation efficiency. Everything
	// audible is below a thin bronze plate's coincidence frequency (~23 kHz),
	// where efficiency climbs with frequency -- the plate is its own high-pass.
	float out(int c, float radTilt) const {
		float o = 0.f; const float* ph = phiMic[c].data();
		for (size_t k = 0; k < modes.size(); k++)
			o += ph[k] * modes[k].s.real() * std::pow(modes[k].f / 1000.f, radTilt);
		return o;
	}
};

// ── the hat ─────────────────────────────────────────────────────────────────
struct Hat {
	Plate top, bot;
	std::vector<float> cth, gap0, warp;
	float ampS = 0.f;          // smoothed rim amplitude^2, for the pitch rise
	std::vector<float> radW[2][2];            // [plate][mic] radiation-weighted phi
	float pedal = 0.f;        // 0 closed hard ... 1 wide open
	float kc = 4.0e6f;        // Hertz contact stiffness
	float lam = 1.0f;         // Hunt-Crossley damping, as a fraction of the stable limit
	float pressD = 400.f;     // pressing damping per unit preload
	float strikeScale = 1.f;  // set by calibrate(): a vel-1 hit moves the rim 1 unit
	float cmax = 0.f;         // the explicit dashpot's stability limit
	float tilt = 0.6f;        // the bottom plate is tilted: gaps differ round the rim
	float radTilt = 0.75f;
	float stickMs = 0.35f;
	bool contact = true;
	// stick
	int stickN = 0, stickI = 0; float stickAmp = 0.f;
	std::vector<float> cprev;
	long contacts = 0;

	void build(float stickR) {
		rng = 0x9e3779b9u;
		float fx = gSEA ? gFX : 18000.f;
		top.build(gF2, gP, fx, 2.2f, 0.004f, 1.f);
		bot.build(gF2 * 1.09f, gP, fx, 2.0f, 0.005f, 1.4f);
		cth.clear(); warp.clear();
		if (gNC <= 3) for (int j = 0; j < gNC; j++) cth.push_back(-0.5f + 0.5f * j);   // one side of the rim
		else for (int j = 0; j < gNC; j++) cth.push_back(2.f * (float)M_PI * (j + 0.3f * frand()) / gNC);
		// a real pair of hats is not flat: each point meets its partner at its own gap
		for (int j = 0; j < gNC; j++) warp.push_back(gWarp * (2.f * frand() - 1.f));
		float micR[2] = {0.55f, 0.55f}, micT[2] = {0.9f, 2.3f};
		top.points(stickR, 0.2f, micR, micT, cth);
		bot.points(0.5f, 0.f, micR, micT, cth);   // bottom plate is never struck
		for (int c = 0; c < 2; c++) {
			radW[0][c].resize(top.modes.size()); radW[1][c].resize(bot.modes.size());
			// Below coincidence a thin plate radiates its low modes very poorly,
			// so the body is heard mostly through the noise it makes, not as
			// partials. Referenced to gFX so the field and the top explicit
			// modes meet at the same level whatever the tilt.
			float rx = gRTX >= 0.f ? gRTX : radTilt;
			float ref = std::pow(gFX / 1000.f, radTilt - rx);
			for (size_t k = 0; k < top.modes.size(); k++) radW[0][c][k] = top.phiMic[c][k] * std::pow(top.modes[k].f / 1000.f, rx) * ref;
			for (size_t k = 0; k < bot.modes.size(); k++) radW[1][c][k] = 0.7f * bot.phiMic[c][k] * std::pow(bot.modes[k].f / 1000.f, rx) * ref;
		}
		cprev.assign(cth.size(), 0.f);
	}
	// Gap at point j, in gap units: negative = pressed together (preload).
	// Closed hard is about half a unit of preload; open is a few units, and the
	// tilt means one side closes first -- half-open is one side touching.
	float gapTh(float th) const {
		// Most of the travel is spent where the plates just touch on the tilted
		// side and rattle -- that is where a hat lives. Pressed hard only at the
		// bottom, clear of each other only at the top.
		float g = pedal < 0.15f ? -0.5f + 0.35f * pedal / 0.15f             // firm to light
		        : pedal < 0.85f ? -0.15f + 0.45f * (pedal - 0.15f) / 0.7f    // touching: the rattle
		        : 0.3f + 2.7f * (pedal - 0.85f) / 0.15f;                     // apart
		return g + tilt * (1.f - std::cos(th));
	}
	float gapAt(int j) const { return gapTh(cth[j]) + warp[j]; }
	static float hertz(float pen, float k) { return pen > 0.f ? k * pen * std::sqrt(pen) : 0.f; }

	// ── THE FIELD ─────────────────────────────────────────────────────────
	// Above gFX a real plate has more modes than bandwidth: they overlap, no
	// ear can separate them, and thousands of them ringing at random phases
	// ARE band-limited noise. So above gFX the plate is its energy per band
	// (statistical energy analysis): fed by the collisions and by the top
	// explicit modes leaking up, cascading band to band -- which is why the
	// highs arrive late -- and drained by damping and by the pedal's pressing.
	static const int NB = 9;
	float bf[NB], bw[NB], bE[NB], bSig[NB], bSigP[NB], bOut[NB], bNorm[NB], bPend[NB];
	struct BQ { float b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		float run(float x) { float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; return y; } };
	BQ bq[NB][2];
	uint32_t nz[2] = {0x1234567u, 0x7654321u};
	float phiRim2 = 0.25f, phiStr2 = 0.25f, phiMic2 = 0.25f, eRef = 1.f;
	int ctlN = 0;
	bool calibrating = false;
	float bPkE[16] = {0}; long bPkT[16] = {0}; long clock = 0;
	float common[16] = {0};
	float hpX[32] = {0}, hpY[32] = {0}, hpA = 0.5f;
	float white(int c) { uint32_t& r = nz[c]; r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (r >> 8) * (2.f / 16777216.f) - 1.f; }
	void buildField() {
		float rc = 1.f / (2.f * (float)M_PI * gFX); hpA = rc / (rc + DT);
		for (int b = 0; b < NB; b++) {
			bf[b] = gFX * std::pow(2.f, (b + 0.5f) * 0.25f);
			bw[b] = gFX * (std::pow(2.f, (b + 1) * 0.25f) - std::pow(2.f, b * 0.25f));
			bE[b] = 0.f; bPend[b] = 0.f;
			float t60 = std::min(gTMAX, 2.2f * std::pow(bf[b] / 1000.f, -hfLoss));
			bSig[b] = bSigP[b] = 6.9078f / t60;
			// RBJ band-pass, 0 dB peak, on white noise; variance measured once
			float w = 2 * (float)M_PI * std::min(bf[b], 0.45f * SR) / SR, Q = bf[b] / bw[b];
			float al = std::sin(w) / (2 * Q), cw = std::cos(w), a0 = 1 + al;
			for (int c = 0; c < 2; c++) { BQ& q = bq[b][c]; q = BQ(); q.b0 = al / a0; q.b1 = 0; q.b2 = -al / a0; q.a1 = -2 * cw / a0; q.a2 = (1 - al) / a0; }
			BQ t = bq[b][0]; uint32_t sv = nz[0]; double v = 0;
			for (int i = 0; i < 48000; i++) { float y = t.run(white(0)); v += y * y; }
			nz[0] = sv;
			bNorm[b] = 1.f / std::sqrt((float)(v / 48000));
			// a mode's time-averaged v^2 is E/m; through the mic's shape and the
			// radiation tilt, a band of energy E reads as this rms
			bOut[b] = std::sqrt(phiMic2 * std::pow(bf[b] / 1000.f, 2.f * radTilt));
		}
	}
	float plateE(const Plate& P, float fmin) const {
		double e = 0; for (auto& M : P.modes) if (M.f >= fmin) e += 0.5 * M.m * std::norm(M.s); return (float)e;
	}
	void drain(Plate& P, float fmin, float frac) {
		float g = std::sqrt(std::max(0.f, 1.f - frac));
		for (auto& M : P.modes) if (M.f >= fmin) M.s *= g;
	}
	// an impulse J at a point, incoherent part, into every band: n_b modes of
	// mean shape^2 each take J^2 phi^2 / 2m
	void injectImpulse(float J, float phi2, float m, float T) {
		for (int b = 0; b < NB; b++) {
			float w = 1.f;
			if (T > 0.f) { float x = bf[b] * T; w = std::fabs(x) < 1e-3f ? 1.f : std::fabs(std::sin((float)M_PI * x) / ((float)M_PI * x) / (1.f - x * x + 1e-6f)); }
			bPend[b] += 0.5f * J * J * phi2 * gDENS * bw[b] * w * w / m;
		}
	}
	void fieldControl(int n) {
		float dt = n * DT;
		float eAll = plateE(top, 0.f) + plateE(bot, 0.f);
		for (int b = 0; b < NB; b++) eAll += bE[b];
		float nl = eAll / eRef;                       // 1 at a full hit
		// the top octave of explicit modes leaks up into the first band
		float lo = gFX * 0.5f;
		float eHi = plateE(top, lo) + plateE(bot, lo);
		if (eHi > 0.f) {
			float frac = std::min(0.5f, gLEAK * nl * dt);
			drain(top, lo, frac); drain(bot, lo, frac);
			// The cascade in a real plate fills the top in a few milliseconds and
			// then holds a steady shape. Walked band by band it took ~90 ms, and a
			// band of noise climbing for 90 ms is heard as a pitch bend.
			float e = eHi * frac;
			bE[0] += e * (1.f - gSPREAD);
			if (gSPREAD > 0.f) {
				float tot = 0.f; for (int b = 0; b < NB; b++) tot += bw[b] * std::pow(bf[b] / gFX, gSLOPE);
				for (int b = 0; b < NB; b++) bE[b] += e * gSPREAD * bw[b] * std::pow(bf[b] / gFX, gSLOPE) / tot;
			}
		}
		// Loss that grows with amplitude: in a real plate the upward cascade
		// ends in dissipation, and it runs far harder when the plate is loud.
		// A hit therefore falls fast and then settles into a long tail --
		// the shape of a real open hat, where fixed loss gives a straight line.
		if (gNLD > 0.f && !calibrating) {
			float g = std::exp(-gNLD * nl * dt);
			for (auto& M : top.modes) M.s *= g;
			for (auto& M : bot.modes) M.s *= g;
			float gf = std::exp(-gNLD * gNLDF * nl * dt);
			for (int b = 0; b < NB; b++) bE[b] *= gf * gf;
		}
		// cascade, then loss
		for (int b = NB - 1; b >= 0; b--) {
			// the top band passes its share on past 18 kHz, where it is lost;
			// without an exit it collected the whole cascade and peaked last
			float J = std::min(0.5f, gCASC * nl * dt) * bE[b]; bE[b] -= J; if (b < NB - 1) bE[b + 1] += J;
		}
		for (int b = 0; b < NB; b++) {
			bE[b] += bPend[b]; bPend[b] = 0.f;
			bE[b] *= std::exp(-2.f * bSigP[b] * dt);
			if (bE[b] > bPkE[b]) { bPkE[b] = bE[b]; bPkT[b] = clock; }
		}
		clock += n;
	}
	float fieldOut(int c) {
		float o = 0.f;
		// a close pair hears one small object: much of the field is common to both
		float sh = std::sqrt(gCORR), own = std::sqrt(1.f - gCORR);
		if (c == 0) for (int b = 0; b < NB; b++) common[b] = white(0);
		for (int b = 0; b < NB; b++) if (bE[b] > 0.f) o += bq[b][c].run(sh * common[b] + own * white(1)) * bNorm[b] * bOut[b] * std::sqrt(bE[b]);
		return o * gSEAG;
	}

	void strike(float vel, float contactMs) {
		stickN = std::max(2, (int)(contactMs * 0.001f * SR));
		stickI = 0; stickAmp = vel * strikeScale;
		if (gSEA) injectImpulse(stickAmp, phiStr2, 1.f, contactMs * 0.001f);
	}
	void setPedal(float p) {
		pedal = p;
		auto pen0 = [this](float th) { return std::max(0.f, -gapTh(th)); };
		top.press(pen0, pressD); bot.press(pen0, pressD * 0.7f);
		// the field is pressed too: the mean extra loss of the top explicit modes
		float add = 0.f; int cnt = 0;
		for (auto& M : top.modes) if (M.f > gFX * 0.5f) { add += -std::log(std::abs(M.pole)) / DT - M.sig; cnt++; }
		add = cnt ? add / cnt : 0.f;
		for (int b = 0; b < NB; b++) bSigP[b] = bSig[b] + add;
	}
	void calibrate() {
		calibrating = true;
		buildField();
		bool was = contact; contact = false; strikeScale = 1.f;
		top.clear(); bot.clear(); strike(1.f, 0.2f);
		float mx = 0; for (int i = 0; i < 4800; i++) { float o[2]; process(o); mx = std::max(mx, std::fabs(top.disp(0))); }
		strikeScale = 1.f / mx; contact = was; top.clear(); bot.clear();
		// field constants from the explicit modes' own shapes
		auto m2 = [](const std::vector<float>& v) { double a = 0; for (float x : v) a += x * x; return v.empty() ? 0.25f : (float)(a / v.size()); };
		phiStr2 = m2(top.phiStrike); phiMic2 = m2(top.phiMic[0]); phiRim2 = m2(top.phiContact[0]);
		buildField();
		// the energy a full hit leaves in the plate, for the nonlinearity's scale
		strike(1.f, 0.2f); for (int i = 0; i < 48; i++) { float o[2]; process(o); }
		eRef = std::max(1e-9f, plateE(top, 0.f));
		top.clear(); bot.clear(); for (int b = 0; b < NB; b++) bE[b] = bPend[b] = 0.f;
		calibrating = false;
		float s2 = 0; for (size_t j = 0; j < cth.size(); j++) s2 = std::max(s2, top.sumPhi2(j) + bot.sumPhi2(j));
		cmax = 0.5f / (DT * s2);
	}
	void process(float* o) {
		// stick: raised-cosine force, integral = vel (normalised)
		if (stickI < stickN) {
			float ph = (stickI + 0.5f) / stickN;
			float F = stickAmp * 2.f / (stickN * DT) * 0.5f * (1.f - std::cos(2.f * (float)M_PI * ph));
			top.push(top.phiStrike, -F);   // downward onto the bottom plate
			stickI++;
		}
		float a2 = 0.f;
		for (size_t j = 0; j < cth.size(); j++) {
			float xt = top.disp(j), xb = bot.disp(j);
			a2 += xt * xt + xb * xb;
			// The plate's own nonlinearity, as a stiffening spring at the rim:
			// a force in x^3 couples every mode to every other, so a hit that
			// lands in the low modes leaks upward over the first milliseconds.
			if (gK3 > 0.f) {
				top.push(top.phiContact[j], -gK3 * xt * xt * xt);
				bot.push(bot.phiContact[j], -gK3 * xb * xb * xb);
			}
			if (contact) {
				// penetration: top moving DOWN (negative) into bottom
				float dx = xb - xt;
				float g  = gapAt(j);
				float pen = dx - g, pen0 = -g;
				float dv = bot.vel(j) - top.vel(j);
				// dynamic force about the pressed rest state, so a closed hat at
				// rest has zero net force and zero motion
				float c = pen > 0.f ? std::min(kc * pen * std::sqrt(pen) * 1e-3f, cmax * lam) : 0.f;
				float F = hertz(pen, kc) + c * dv - hertz(pen0, kc);
				if (pen > 0.f) contacts++;
				// repulsive: the top plate is pushed UP, the bottom DOWN
				if (F != 0.f) { top.push(top.phiContact[j], F); bot.push(bot.phiContact[j], -F); }
				// a collision is an impulse into BOTH plates, so into the field twice
				// Only the part of the force that changes faster than the field's
				// cutoff reaches it: a pedal's steady pressure is not a sound, a
				// Hertz impact's sharp onset is. One-pole high-pass at gFX.
				if (gSEA) {
					float hp = hpA * (hpY[j] + F - hpX[j]); hpX[j] = F; hpY[j] = hp;
					if (hp != 0.f) { float J = hp * DT; injectImpulse(J, phiRim2, 1.f, 0.f); injectImpulse(J, phiRim2, 1.4f, 0.f); }
				}
			}
		}
		a2 /= (2.f * cth.size());
		ampS += (a2 - ampS) * 0.02f;     // ~1 ms
		float up = gTM * ampS;
		top.step(up); bot.step(up);
		if (gSEA && ++ctlN >= 16) { fieldControl(ctlN); ctlN = 0; }
		for (int c = 0; c < 2; c++) {
			float s = gSEA ? fieldOut(c) : 0.f;
			for (size_t k = 0; k < top.modes.size(); k++) s += radW[0][c][k] * top.modes[k].s.real();
			for (size_t k = 0; k < bot.modes.size(); k++) s += radW[1][c][k] * bot.modes[k].s.real();
			o[c] = s;
		}
	}
};

// ── measuring ───────────────────────────────────────────────────────────────
static void bands(const std::vector<float>& x, int a, int b, double* e, double& cen) {
	// DFT energy in bands by direct evaluation on a log grid (slow, fine)
	static const float edges[6] = {0, 500, 2000, 6000, 12000, 24000};
	for (int i = 0; i < 5; i++) e[i] = 0;
	double tot = 0, wsum = 0;
	int N = b - a;
	for (float f = 60.f; f < 22000.f; f *= 1.03f) {
		double re = 0, im = 0, w = 2 * M_PI * f / SR;
		for (int n = 0; n < N; n++) {
			double win = 0.5 - 0.5 * std::cos(2 * M_PI * n / N);
			re += x[a + n] * win * std::cos(w * n); im += x[a + n] * win * std::sin(w * n);
		}
		double p = (re * re + im * im) * f * 0.03;   // per-Hz on a log grid
		for (int i = 0; i < 5; i++) if (f >= edges[i] && f < edges[i + 1]) e[i] += p;
		tot += p; wsum += p * f;
	}
	for (int i = 0; i < 5; i++) e[i] = tot > 0 ? 100 * e[i] / tot : 0;
	cen = tot > 0 ? wsum / tot : 0;
}
static void wav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R) {
	FILE* f = fopen(path.c_str(), "wb"); if (!f) return;
	float pk = 1e-9f; for (size_t i = 0; i < L.size(); i++) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
	float g = 0.7f / pk;
	uint32_t n = L.size(), bytes = n * 4, sr = 48000, br = sr * 4; uint16_t ch = 2, ba = 4, bps = 16, fmt = 1;
	uint32_t sz = 36 + bytes, sixteen = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&sixteen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
	fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
	for (uint32_t i = 0; i < n; i++) { int16_t a = (int16_t)(L[i] * g * 32767), b = (int16_t)(R[i] * g * 32767); fwrite(&a, 2, 1, f); fwrite(&b, 2, 1, f); }
	fclose(f);
}

struct Hat; static Hat* gHat = nullptr;
struct Result { double e[3][5], cen[3]; float t20, t40, t60, peak; long contacts; double us; float tLo, tHi, rise, flat, fEarly, fLate, cEarly, cLate, corr, db[7], tonal; };
static Result measure(const std::vector<float>& l, const std::vector<float>& r);
static Result run(Hat& h, float pedal, float vel, float contactMs, float secs,
                  std::vector<float>* L = nullptr, std::vector<float>* R = nullptr) {
	h.setPedal(pedal); for (int b = 0; b < Hat::NB; b++) { h.bE[b] = h.bPend[b] = 0.f; h.bPkE[b] = 0.f; h.bPkT[b] = 0; } h.clock = 0; h.top.clear(); h.bot.clear(); h.contacts = 0;
	int N = (int)(secs * SR);
	std::vector<float> l(N), r(N);
	h.strike(vel, contactMs);
	auto t0 = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < N; i++) { float o[2]; h.process(o); l[i] = o[0]; r[i] = o[1]; }
	auto t1 = std::chrono::high_resolution_clock::now();
	Result res = measure(l, r);
	res.us = std::chrono::duration<double, std::micro>(t1 - t0).count() / N;
	res.contacts = h.contacts;
	if (L) *L = l; if (R) *R = r;
	return res;
}
// Everything measured from a render alone, so another engine (src/kit-hat.hpp,
// through tools/kit-hat-check.cpp) is judged by exactly the same yardstick.
static Result measure(const std::vector<float>& l, const std::vector<float>& r) {
	int N = (int)l.size();
	Result res; res.us = 0; res.contacts = 0;
	// envelope in 5 ms windows
	float pk = 0; std::vector<float> env;
	for (int i = 0; i + 240 <= N; i += 240) { double s = 0; for (int j = 0; j < 240; j++) s += l[i + j] * l[i + j]; env.push_back(std::sqrt(s / 240)); pk = std::max(pk, env.back()); }
	res.peak = pk;
	res.t20 = res.t40 = res.t60 = -1;
	for (size_t i = 0; i < env.size(); i++) {
		float db = 20 * std::log10(env[i] / pk + 1e-12f);
		// last window above each threshold
		if (db > -20) res.t20 = (i + 1) * 0.005f;
		if (db > -40) res.t40 = (i + 1) * 0.005f;
		if (db > -60) res.t60 = (i + 1) * 0.005f;
	}
	int W[3][2] = {{0, (int)(0.03f * SR)}, {(int)(0.03f * SR), (int)(0.12f * SR)}, {(int)(0.12f * SR), (int)(0.4f * SR)}};
	for (int w = 0; w < 3; w++) {
		int a = W[w][0], b = std::min(W[w][1], N);
		if (b - a < 256) { for (int i = 0; i < 5; i++) res.e[w][i] = 0; res.cen[w] = 0; continue; }
		bands(l, a, b, res.e[w], res.cen[w]);
	}
	// BLOOM: when the highs peak against when the lows do. A bell's highs
	// arrive with the stick; a cymbal's arrive late, carried up by the
	// nonlinearity. 1 ms RMS envelopes of a 5 kHz high-pass and a 3 kHz low-pass
	// (2-pole each, RBJ).
	auto biq = [&](bool hp, float fc, std::vector<float>& y) {
		float w = 2 * M_PI * fc / SR, al = std::sin(w) / (2 * 0.7071f), c = std::cos(w);
		float b0 = hp ? (1 + c) / 2 : (1 - c) / 2, b1 = hp ? -(1 + c) : 1 - c, b2 = b0;
		float a0 = 1 + al, a1 = -2 * c, a2 = 1 - al;
		float x1 = 0, x2 = 0, y1 = 0, y2 = 0; y.resize(N);
		for (int i = 0; i < N; i++) { float x = l[i], o = (b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0; x2 = x1; x1 = x; y2 = y1; y1 = o; y[i] = o; }
	};
	auto tpk = [&](const std::vector<float>& y) {
		int M = std::min(N, (int)(0.1f * SR)); float best = 0; int bi = 0;
		for (int i = 0; i + 48 <= M; i += 48) { double s = 0; for (int j = 0; j < 48; j++) s += y[i + j] * y[i + j]; if (s > best) { best = s; bi = i; } }
		return (bi + 24) / SR * 1000.f;
	};
	std::vector<float> hp, lp; biq(true, 5000.f, hp); biq(false, 3000.f, lp);
	res.tHi = tpk(hp); res.tLo = tpk(lp);
	{	// 10-90% rise of the broadband 1 ms envelope
		std::vector<float> e; float m = 0;
		for (int i = 0; i + 48 <= std::min(N, (int)(0.1f * SR)); i += 48) { double s = 0; for (int j = 0; j < 48; j++) s += l[i + j] * l[i + j]; e.push_back(std::sqrt(s)); m = std::max(m, e.back()); }
		int a = -1, b = -1; for (size_t i = 0; i < e.size(); i++) { if (a < 0 && e[i] > 0.1f * m) a = i; if (b < 0 && e[i] > 0.9f * m) b = i; }
		res.rise = (b - a) * 1.f;
	}
	{	// FLATNESS 2-16 kHz, 20-120 ms: 1 = noise, near 0 = a few clean partials
		int a = (int)(0.02f * SR), n = std::min((int)(0.1f * SR), N - a);
		double lg = 0, ar = 0; int cnt = 0;
		for (float f = 2000.f; f < 16000.f; f += 10.f) {
			double re = 0, im = 0, w = 2 * M_PI * f / SR;
			for (int k = 0; k < n; k++) { double win = 0.5 - 0.5 * std::cos(2 * M_PI * k / n); re += l[a + k] * win * std::cos(w * k); im += l[a + k] * win * std::sin(w * k); }
			double p = re * re + im * im + 1e-30; lg += std::log(p); ar += p; cnt++;
		}
		res.flat = (float)(std::exp(lg / cnt) / (ar / cnt));
		// the same, 500 Hz - 5 kHz: where the body's partials would squelch
		lg = 0; ar = 0; cnt = 0;
		for (float f = 500.f; f < 5000.f; f += 10.f) {
			double re = 0, im = 0, w = 2 * M_PI * f / SR;
			for (int k = 0; k < n; k++) { double win = 0.5 - 0.5 * std::cos(2 * M_PI * k / n); re += l[a + k] * win * std::cos(w * k); im += l[a + k] * win * std::sin(w * k); }
			double p = re * re + im * im + 1e-30; lg += std::log(p); ar += p; cnt++;
		}
		res.tonal = (float)(std::exp(lg / cnt) / (ar / cnt));
	}
	{	// DECAY SHAPE: 10 ms RMS level at fixed times, dB below the peak window
		const float ts[7] = {0.01f, 0.025f, 0.05f, 0.1f, 0.2f, 0.4f, 0.8f};
		float pk = 0; for (int i = 0; i + 480 <= std::min(N, (int)(0.1f * SR)); i += 48) { double q = 0; for (int j = 0; j < 480; j++) q += l[i + j] * l[i + j]; pk = std::max(pk, (float)q); }
		for (int t = 0; t < 7; t++) {
			int i = (int)(ts[t] * SR); if (i + 480 > N) { res.db[t] = -99; continue; }
			double q = 0; for (int j = 0; j < 480; j++) q += l[i + j] * l[i + j];
			res.db[t] = 10.f * std::log10((float)q / pk + 1e-12f);
		}
	}
	{	// GLIDE: the strongest partial 150-3000 Hz early (10-60 ms) against late
		// (300-400 ms). A gong's modes start sharp and fall; a hat's should not.
		auto pkf = [&](int a, int n, float flo, float fhi) {
			if (a + n > N) return 0.f;
			double best = 0; float bf = 0;
			for (float f = flo; f < fhi; f += 1.f) {
				double re = 0, im = 0, w = 2 * M_PI * f / SR;
				for (int k = 0; k < n; k++) { double win = 0.5 - 0.5 * std::cos(2 * M_PI * k / n); re += l[a + k] * win * std::cos(w * k); im += l[a + k] * win * std::sin(w * k); }
				double p = re * re + im * im; if (p > best) { best = p; bf = f; }
			}
			return bf;
		};
		// the same partial, followed: search late only near where it was early
		// SINKING: the spectral centroid 100 Hz-18 kHz early against late
		auto cen = [&](int a, int n) {
			if (a + n > N) return 0.f;
			double pw = 0, pf = 0;
			for (float f = 100.f; f < 18000.f; f *= 1.02f) {
				double re = 0, im = 0, w = 2 * M_PI * f / SR;
				for (int k = 0; k < n; k += 1) { double win = 0.5 - 0.5 * std::cos(2 * M_PI * k / n); re += l[a + k] * win * std::cos(w * k); im += l[a + k] * win * std::sin(w * k); }
				double p = (re * re + im * im) * f; pw += p; pf += p * f;
			}
			return (float)(pf / std::max(1e-30, pw));
		};
		res.cEarly = cen((int)(0.02f * SR), (int)(0.05f * SR));
		res.cLate  = cen((int)(0.6f * SR), (int)(0.05f * SR));
		{ double a = 0, b = 0, c = 0; int n = std::min(N, (int)(0.5f * SR)); for (int i = 0; i < n; i++) { a += l[i] * r[i]; b += l[i] * l[i]; c += r[i] * r[i]; } res.corr = (float)(a / std::sqrt(b * c + 1e-30)); }
		res.fEarly = pkf((int)(0.01f * SR), (int)(0.05f * SR), 150.f, 3000.f);
		res.fLate  = pkf((int)(0.30f * SR), (int)(0.10f * SR), res.fEarly * 0.97f, res.fEarly * 1.03f);
	}
	return res;
}
static void report(const char* name, const Result& r) {
	printf("== %-22s peak %.3g  t-20 %.3fs t-40 %.3fs t-60 %.3fs  contacts %ld  %.2f us/sample\n",
	       name, r.peak, r.t20, r.t40, r.t60, r.contacts, r.us);
	printf("   bloom: lows peak %.0f ms, highs peak %.0f ms, rise %.0f ms   flatness 2-16k %.3f   glide %.0f Hz -> %.0f Hz (%+.0f cents)\n",
	       r.tLo, r.tHi, r.rise, r.flat, r.fEarly, r.fLate, r.fEarly > 0 && r.fLate > 0 ? 1200.0 * std::log2(r.fLate / r.fEarly) : 0.0);
	printf("   decay dB at 10/25/50/100/200/400/800 ms: %.0f %.0f %.0f %.0f %.0f %.0f %.0f   flatness 0.5-5k %.3f\n",
	       r.db[0], r.db[1], r.db[2], r.db[3], r.db[4], r.db[5], r.db[6], r.tonal);
	if (gSEA && gHat) { printf("   field band peaks (ms):"); for (int b = 0; b < Hat::NB; b++) printf(" %.0f", gHat->bPkT[b] * 1000.f / SR); printf("\n"); }
	if (r.cLate > 0) printf("   centroid %.0f Hz at 20 ms, %.0f Hz at 600 ms (%.1f octaves down)   L/R correlation %.2f\n",
	       r.cEarly, r.cLate, std::log2(r.cEarly / r.cLate), r.corr);
	const char* wn[3] = {"  0- 30ms", " 30-120ms", "120-400ms"};
	for (int w = 0; w < 3; w++)
		printf("   %s centroid %6.0f Hz   <500 %3.0f%%  0.5-2k %3.0f%%  2-6k %3.0f%%  6-12k %3.0f%%  >12k %3.0f%%\n",
		       wn[w], r.cen[w], r.e[w][0], r.e[w][1], r.e[w][2], r.e[w][3], r.e[w][4]);
}

int main(int argc, char** argv) {
	std::string out = argc > 1 ? argv[1] : ".";
	Hat h; gHat = &h;
	if (getenv("RT")) h.radTilt = atof(getenv("RT"));
	if (getenv("HF")) hfLoss = atof(getenv("HF"));
	if (getenv("P"))  gP = atof(getenv("P"));
	if (getenv("FJ")) gFJ = atof(getenv("FJ"));
	if (getenv("NC")) gNC = atoi(getenv("NC"));
	if (getenv("WARP")) gWarp = atof(getenv("WARP"));
	if (getenv("K3")) gK3 = atof(getenv("K3"));
	if (getenv("TM")) gTM = atof(getenv("TM"));
	if (getenv("F2")) gF2 = atof(getenv("F2"));
	if (getenv("FX")) gFX = atof(getenv("FX"));
	if (getenv("SEA")) gSEA = atoi(getenv("SEA"));
	if (getenv("DENS")) gDENS = atof(getenv("DENS"));
	if (getenv("LEAK")) gLEAK = atof(getenv("LEAK"));
	if (getenv("CASC")) gCASC = atof(getenv("CASC"));
	if (getenv("SEAG")) gSEAG = atof(getenv("SEAG"));
	if (getenv("TMAX")) gTMAX = atof(getenv("TMAX"));
	if (getenv("CORR")) gCORR = atof(getenv("CORR"));
	if (getenv("SPREAD")) gSPREAD = atof(getenv("SPREAD"));
	if (getenv("SLOPE")) gSLOPE = atof(getenv("SLOPE"));
	if (getenv("NLD")) gNLD = atof(getenv("NLD"));
	if (getenv("NLDF")) gNLDF = atof(getenv("NLDF"));
	if (getenv("RTX")) gRTX = atof(getenv("RTX"));
	if (getenv("SM")) h.stickMs = atof(getenv("SM"));
	if (getenv("TILT")) h.tilt = atof(getenv("TILT"));
	h.build(0.85f);
	printf("modes: top %zu, bottom %zu\n", h.top.modes.size(), h.bot.modes.size());
	h.calibrate();
	printf("strike scale %.4g, dashpot limit %.4g\n", h.strikeScale, h.cmax);
	if (getenv("KC")) h.kc = atof(getenv("KC"));
	if (getenv("PD")) h.pressD = atof(getenv("PD"));
	struct Case { const char* name; float pedal; bool contact; float secs; };
	Case cases[] = {
		{"plate alone (no contact)", 1.0f, false, 2.0f},
		{"closed", 0.0f, true, 1.0f},
		{"closed loose", 0.15f, true, 1.0f},
		{"half open", 0.35f, true, 2.0f},
		{"mostly open", 0.55f, true, 2.0f},
		{"open", 1.0f, true, 2.5f},
	};
	for (auto& c : cases) {
		h.contact = c.contact;
		std::vector<float> L, R;
		Result r = run(h, c.pedal, 1.f, h.stickMs, c.secs, &L, &R);
		report(c.name, r);
		std::string fn = out + "/hat-" + std::string(c.name) + ".wav";
		for (auto& ch : fn) if (ch == ' ' || ch == '(' || ch == ')') ch = '_';
		wav(fn, L, R);
	}
	// ── listening: a groove and a pedal sweep ──────────────────────────────
	// The groove plays through ONE hat whose state carries over, so a hit meets
	// plates that are still moving, and the pedal closing on an open hat chokes
	// it the way a foot does. Pedal changes are slewed (a foot is not a switch).
	auto groove = [&](const char* name, int steps, float stepSec,
	                  std::function<void(int, float&, float&)> at) {
		h.contact = true; h.top.clear(); h.bot.clear(); for (int b = 0; b < Hat::NB; b++) h.bE[b] = h.bPend[b] = 0.f;
		int per = (int)(stepSec * SR), N = steps * per + (int)(1.5f * SR);
		std::vector<float> L(N), R(N);
		float ped = 0.f, pedT = 0.f; int pedCount = 0;
		h.setPedal(0.f);
		for (int i = 0; i < N; i++) {
			if (i % per == 0 && i / per < steps) {
				float vel = 0.f; at(i / per, pedT, vel);
				if (vel > 0.f) h.strike(vel, h.stickMs);
			}
			if (std::fabs(ped - pedT) > 1e-4f && ++pedCount >= 32) {   // control rate
				pedCount = 0; ped += (pedT - ped) * std::min(1.f, 32.f / (0.02f * SR)); h.setPedal(ped);
			}
			float o[2]; h.process(o); L[i] = o[0]; R[i] = o[1];
		}
		wav(out + "/hat-" + name + ".wav", L, R);
		printf("wrote hat-%s.wav\n", name);
	};
	// eighths at 110 BPM, accents on the beat, open on the "and" of 4, closed by the foot on 1
	groove("groove", 32, 60.f / 110.f / 2.f, [](int i, float& ped, float& vel) {
		int s = i % 8;
		vel = (s % 2 == 0) ? 1.f : 0.55f;
		ped = (s == 7) ? 0.45f : 0.f;
	});
	// sixteen hits, the pedal lifting from closed to open and back
	groove("pedal-sweep", 32, 0.2f, [](int i, float& ped, float& vel) {
		float t = i / 31.f; ped = 0.8f * std::sin((float)M_PI * t); vel = 0.8f;
	});
	// sixteenths, velocity from ghost to accent, closed
	groove("velocity", 16, 0.14f, [](int i, float& ped, float& vel) {
		ped = 0.05f; vel = 0.15f + 0.85f * i / 15.f;
	});
	return 0;
}

#pragma once
// Garamut: a slit drum, modelled mode by mode. Free of Rack, so it can be
// measured and rendered on its own (tools/garamut-proto.cpp).
//
// A slit drum is a hollow column with a long slit cut into one side. The wall on
// either side of the slit is a LIP. Makers thin the two lips by different amounts,
// so each lip has its own note. The sound has three parts:
//
//   1. Each lip is a plate held along three edges (the two ends of the slit and the
//      line where it joins the rest of the wall) and free along the slit. It is
//      modelled as 8 modes: 4 along the slit (a beam clamped at both ends) times 2
//      across the lip (a beam clamped at one edge and free at the other).
//   2. The air inside and the air in the slit form a Helmholtz resonator. The lips
//      push air in and out of the cavity, and the slit's air moves in response.
//      Below the Helmholtz frequency the slit's flow cancels the lips' flow, as in a
//      bass-reflex speaker, so the column radiates little there.
//   3. Higher air modes run up the column's height (closed at both ends). Their
//      frequencies come from the cross-section's area along the height, so the pear
//      shape makes them inharmonic.
//
// The air is driven by the lips and does not push back on them. A study of a
// Nigerian slit log gong found its resonances were two mechanical modes and one
// Helmholtz mode with little energy passing between them, and the beating heard
// from interference between them (APS March Meeting 2006, B42.2).
//
// Every material constant, the lip proportions, the curvature factor and the slit
// end correction are ESTIMATES chosen to land in the range real slit drums play in
// (roughly 80 Hz to 900 Hz), not measurements of one instrument.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace garamut {

static const int   NMA = 6;          // modes along a lip
static const int   NMC = 3;          // and across it
static const int   NM = NMA * NMC;   // modes per lip
static const int   NT = 3;           // air modes up the column
static const int   NCH = 17;         // 16 polyphonic channels, and one for the mouse
static const int   NMAL = 2 * NCH;   // a mallet per channel per side
static const int   SUB = 8;          // substeps while a mallet is in contact
static const float C_AIR = 343.f;
static const float RHO_AIR = 1.2f;
static const double PI_D = 3.14159265358979323846;

// ── materials, from wood to metal ───────────────────────────────────────────
// Wood is orthotropic: with the grain along the slit it is about ten times stiffer
// along the lip than across it. Metal is the same in every direction. `thick`
// scales the lip's thickness: metal slit drums are made from thinner wall than a
// hollowed log, and without it a steel lip would sound about two octaves above a
// cedar one of the same size. `surfE` is the stiffness a mallet meets on the face.
struct MatNode {
	const char* name;
	float rho, EL, ET, nu;
	float eta;        // loss factor at 250 Hz
	float etaSlope;   // eta rises as f^slope
	float thick;
	float surfE;
};
static const int NMAT = 4;
static const MatNode MATS[NMAT] = {
	{"Cedar",      380.f,  8.0e9f,  0.60e9f, 0.30f, 0.0090f, 0.30f, 1.00f, 0.4e9f},
	{"Hardwood",   800.f, 14.0e9f,  1.20e9f, 0.30f, 0.0060f, 0.30f, 1.00f, 1.2e9f},
	{"Aluminium", 2700.f, 70.0e9f, 70.0e9f, 0.33f, 0.0006f, 0.10f, 0.28f, 70e9f},
	{"Steel",     7850.f, 200e9f,  200e9f,  0.29f, 0.0003f, 0.10f, 0.20f, 200e9f},
};
// What holding the drum and radiating sound remove, on top of the material's loss.
// It sets how long a metal drum rings, since its own loss is tiny.
static const float ETA_MOUNT = 0.0016f;

// ── mallets, from soft to hard ──────────────────────────────────────────────
// A mass meeting the lip through a Hertzian contact, F = k c^alpha. For rubber,
// wood and brass k comes from the two surfaces' stiffness and the tip radius, so a
// hard mallet on a wooden lip is still limited by the wood. Felt is a compliant
// pad whose own stiffness sets k, with alpha 2.5 as in piano hammer felt.
struct MalletNode {
	const char* name;
	float mass;     // kg
	float E;        // Pa, 0 = use k directly
	float k;        // N/m^alpha when E is 0
	float tipR;     // m
	float alpha;
};
static const int NMALLET = 4;
static const MalletNode MALLETS[NMALLET] = {
	{"Felt",   0.12f, 0.f,    3.7e8f, 0.020f, 2.5f},
	{"Rubber", 0.10f, 4.0e7f, 0.f,    0.020f, 1.5f},
	{"Wood",   0.05f, 1.0e10f,0.f,    0.008f, 1.5f},
	{"Brass",  0.08f, 1.0e11f,0.f,    0.008f, 1.5f},
};

// ── beam functions ──────────────────────────────────────────────────────────
// Roots of cos(b) cosh(b) = 1 (clamped at both ends) and = -1 (clamped-free),
// found by Newton at startup, with sigma = (cosh b - cos b) / (sinh b - sin b) for
// the first and (cosh b + cos b) / (sinh b + sin b) for the second.
// Sigma has to be exact to many digits: at b = 20, sinh b is 4e8, and an error of
// 1e-6 in sigma leaves the shape 400 off zero at the clamped end.
struct Beams {
	double ccBeta[NMA], ccSigma[NMA], cfBeta[NMC], cfSigma[NMC];
	static double root(double guess, double rhs) {
		double b = guess;
		for (int i = 0; i < 60; i++) {
			double f = std::cos(b) * std::cosh(b) - rhs;
			double d = -std::sin(b) * std::cosh(b) + std::cos(b) * std::sinh(b);
			b -= f / d;
		}
		return b;
	}
	Beams() {
		for (int m = 0; m < NMA; m++) {
			double b = root((m + 1.5) * PI_D, 1.0);
			ccBeta[m] = b; ccSigma[m] = (std::cosh(b) - std::cos(b)) / (std::sinh(b) - std::sin(b));
		}
		for (int n = 0; n < NMC; n++) {
			double b = root(n == 0 ? 1.875 : (n + 0.5) * PI_D, -1.0);
			cfBeta[n] = b; cfSigma[n] = (std::cosh(b) + std::cos(b)) / (std::sinh(b) + std::sin(b));
		}
	}
};
static const Beams& beams() { static Beams B; return B; }

// The shape, written so that it stays exact where cosh and sinh are huge: cosh - sigma
// sinh is evaluated as (1 - sigma) e^x / 2 + (1 + sigma) e^-x / 2.
static inline double beamShape(double beta, double sigma, double x) {
	double bx = beta * x;
	double ex = std::exp(bx), emx = std::exp(-bx);
	return 0.5 * (1.0 - sigma) * ex + 0.5 * (1.0 + sigma) * emx - std::cos(bx) + sigma * std::sin(bx);
}

// ── the shape of the column ─────────────────────────────────────────────────
// SHAPE runs square column (0) to round column (0.5) to pear (1). `sq` is how
// square the cross-section is, `pear` how far the profile has bulged.
static inline void shapeParts(float shape, float& sq, float& pear) {
	sq = std::max(0.f, std::min(1.f, 1.f - 2.f * shape));
	pear = std::max(0.f, std::min(1.f, 2.f * shape - 1.f));
}
// Outer radius at height eta (0 bottom, 1 top), as a fraction of R.
static inline float profileR(float eta, float pear) {
	float b = (eta - 0.3f) / 0.24f;
	float bulge = std::exp(-b * b);
	float t = std::max(0.f, std::min(1.f, (eta - 0.5f) / 0.45f));
	float neck = t * t * (3.f - 2.f * t);
	return 1.f + pear * (0.32f * bulge - 0.42f * neck);
}
static const float WALL = 0.12f;     // the wall, as a fraction of the local radius
static inline float areaFactor(float eta, float sq, float pear) {
	float ri = (1.f - WALL) * profileR(eta, pear);
	return ri * ri * (float)(PI_D + (4.0 - PI_D) * sq);
}
// Where the slit is: centred a little low on a pear, so it stays on the body.
static inline void slitSpan(float slit, float pear, float& e0, float& e1) {
	float lf = 0.30f + 0.45f * slit;
	float c = 0.5f - 0.08f * pear;
	e0 = c - 0.5f * lf; e1 = c + 0.5f * lf;
}

// ── air modes up the column ─────────────────────────────────────────────────
// Webster's horn equation on the area profile, closed at both ends, as a chain of
// 32 cells. Solved once per tabulated shape at startup.
static const int WN = 32;
static const int SHAPE_TAB = 129;
struct TubeTab {
	float omega[SHAPE_TAB][NT];         // dimensionless: c = 1, H = 1
	float psi[SHAPE_TAB][NT][WN];       // pressure shape, max |psi| = 1
	float vol[SHAPE_TAB];               // integral of area / R^2 over height
	float wNorm[SHAPE_TAB][NMA];        // warped clamped-clamped beam: 1 / sqrt(int X^2)
	float wInt[SHAPE_TAB][NMA];         // and int X, after normalising
};

static inline float warpGamma(float pear) { return 1.f / (1.f + 0.4f * pear); }

static void sturmSolve(const double* d, const double* e, int n, int k, double lo, double hi, double& out) {
	// k-th smallest eigenvalue (0-based) of a symmetric tridiagonal matrix
	for (int it = 0; it < 80; it++) {
		double mid = 0.5 * (lo + hi);
		int cnt = 0;
		double q = d[0] - mid;
		if (q < 0) cnt++;
		for (int i = 1; i < n; i++) {
			if (std::fabs(q) < 1e-300) q = 1e-300;
			q = d[i] - mid - e[i - 1] * e[i - 1] / q;
			if (q < 0) cnt++;
		}
		if (cnt > k) hi = mid; else lo = mid;
	}
	out = 0.5 * (lo + hi);
}

static void buildTubeTab(TubeTab& T) {
	for (int s = 0; s < SHAPE_TAB; s++) {
		float shape = (float)s / (SHAPE_TAB - 1);
		float sq, pear; shapeParts(shape, sq, pear);
		double S[WN], C[WN], a[WN], d[WN], e[WN];
		double dx = 1.0 / WN, vol = 0;
		for (int i = 0; i < WN; i++) {
			S[i] = areaFactor((i + 0.5f) / WN, sq, pear);
			C[i] = S[i] * dx;
			vol += S[i] * dx;
		}
		for (int i = 0; i < WN - 1; i++) a[i] = 0.5 * (S[i] + S[i + 1]) / dx;
		for (int i = 0; i < WN; i++) {
			double w = (i > 0 ? a[i - 1] : 0) + (i < WN - 1 ? a[i] : 0);
			d[i] = w / C[i];
			if (i < WN - 1) e[i] = -a[i] / std::sqrt(C[i] * C[i + 1]);
		}
		T.vol[s] = (float)vol;
		double hi = 0;
		for (int i = 0; i < WN; i++) hi = std::max(hi, d[i] + 2 * std::fabs(i < WN - 1 ? e[i] : 0) + 2 * std::fabs(i > 0 ? e[i - 1] : 0));
		for (int j = 0; j < NT; j++) {
			double lam;
			sturmSolve(d, e, WN, j + 1, 0.0, hi, lam);   // skip the zero mode
			T.omega[s][j] = (float)std::sqrt(std::max(lam, 0.0));
			// inverse iteration for the vector
			double x[WN], cp[WN], dp[WN];
			for (int i = 0; i < WN; i++) x[i] = 1.0 + 0.01 * i;
			double sh = lam + 1e-9 * (1 + lam);
			for (int rep = 0; rep < 4; rep++) {
				// Thomas on (A - sh I) y = x
				double b0 = d[0] - sh;
				cp[0] = e[0] / b0; dp[0] = x[0] / b0;
				for (int i = 1; i < WN; i++) {
					double m = (d[i] - sh) - e[i - 1] * cp[i - 1];
					if (std::fabs(m) < 1e-300) m = 1e-300;
					cp[i] = i < WN - 1 ? e[i] / m : 0;
					dp[i] = (x[i] - e[i - 1] * dp[i - 1]) / m;
				}
				x[WN - 1] = dp[WN - 1];
				for (int i = WN - 2; i >= 0; i--) x[i] = dp[i] - cp[i] * x[i + 1];
				double nn = 0; for (int i = 0; i < WN; i++) nn += x[i] * x[i];
				nn = std::sqrt(nn); for (int i = 0; i < WN; i++) x[i] /= nn;
			}
			double mx = 1e-30;
			double p[WN];
			for (int i = 0; i < WN; i++) { p[i] = x[i] / std::sqrt(C[i]); mx = std::max(mx, std::fabs(p[i])); }
			for (int i = 0; i < WN; i++) T.psi[s][j][i] = (float)(p[i] / mx);
		}
		// the lip's mode shapes along the slit, warped toward the wider part on a pear
		double g = warpGamma(pear);
		const Beams& B = beams();
		for (int m = 0; m < NMA; m++) {
			const int Q = 512;
			double s2 = 0, s1 = 0;
			for (int q = 0; q < Q; q++) {
				double eta = (q + 0.5) / Q;
				double X = beamShape(B.ccBeta[m], B.ccSigma[m], std::pow(eta, g));
				s2 += X * X / Q; s1 += X / Q;
			}
			double nrm = 1.0 / std::sqrt(s2);
			T.wNorm[s][m] = (float)nrm;
			T.wInt[s][m] = (float)(s1 * nrm);
		}
	}
}

static const TubeTab& tubeTab() {
	static TubeTab* T = nullptr;
	if (!T) { T = new TubeTab(); buildTubeTab(*T); }
	return *T;
}

// ── settings ────────────────────────────────────────────────────────────────
struct Params {
	float size = 0.5f;       // 0..1: column height 0.3 m .. 2.5 m
	float shape = 0.5f;      // 0 square column, 0.5 round, 1 pear
	float material = 0.33f;  // 0 cedar .. 1 steel
	float slit = 0.5f;       // 0..1: slit length and width
	float mallet = 0.5f;     // 0 felt .. 1 brass
	float interval = 5.f;    // semitones, right lip above left
	float decay = 1.f;       // ring-time multiplier
	float voct = 0.f;        // transposes the whole drum
	bool operator==(const Params& o) const {
		return size == o.size && shape == o.shape && material == o.material && slit == o.slit
		    && mallet == o.mallet && interval == o.interval && decay == o.decay && voct == o.voct;
	}
};

static inline float heightOf(float size) { return 0.3f * std::pow(2.5f / 0.3f, size); }

// Blend two table entries in log space for positive quantities.
static inline float lerpLog(float a, float b, float t) { return std::exp(std::log(a) + (std::log(b) - std::log(a)) * t); }

struct MatNow { float rho, EL, ET, nu, eta, etaSlope, thick, surfE; };
static inline MatNow materialAt(float u) {
	u = std::max(0.f, std::min(1.f, u)) * (NMAT - 1);
	int i = std::min((int)u, NMAT - 2);
	float t = u - i;
	const MatNode& A = MATS[i]; const MatNode& B = MATS[i + 1];
	MatNow m;
	m.rho = lerpLog(A.rho, B.rho, t);
	m.EL = lerpLog(A.EL, B.EL, t);
	m.ET = lerpLog(A.ET, B.ET, t);
	m.nu = A.nu + (B.nu - A.nu) * t;
	m.eta = lerpLog(A.eta, B.eta, t);
	m.etaSlope = A.etaSlope + (B.etaSlope - A.etaSlope) * t;
	m.thick = lerpLog(A.thick, B.thick, t);
	m.surfE = lerpLog(A.surfE, B.surfE, t);
	return m;
}

// Contact stiffness for a mallet node against a lip surface.
static inline float nodeK(const MalletNode& N, float surfE) {
	if (N.E <= 0.f) return N.k;
	float Es = 1.f / ((1.f - 0.09f) / N.E + (1.f - 0.09f) / surfE);
	return (4.f / 3.f) * Es * std::sqrt(N.tipR);
}
struct MalletNow { float mass = 0.05f, k = 1e8f, alpha = 1.5f, tipR = 0.01f; };
// k at a tip radius scaled by s: Hertz k goes as sqrt(tip radius). Felt, whose k is
// its own, scales the same way (a bigger felt head of the same felt).
static inline float rescaleK(float u, float surfE, float s) {
	u = std::max(0.f, std::min(1.f, u)) * (NMALLET - 1);
	int i = std::min((int)u, NMALLET - 2);
	float t = u - i;
	float ka = nodeK(MALLETS[i], surfE), kb = nodeK(MALLETS[i + 1], surfE);
	return lerpLog(ka, kb, t) * std::sqrt(s);
}
static inline MalletNow malletAt(float u, float surfE) {
	u = std::max(0.f, std::min(1.f, u)) * (NMALLET - 1);
	int i = std::min((int)u, NMALLET - 2);
	float t = u - i;
	const MalletNode& A = MALLETS[i]; const MalletNode& B = MALLETS[i + 1];
	MalletNow m;
	m.mass = lerpLog(A.mass, B.mass, t);
	m.k = lerpLog(nodeK(A, surfE), nodeK(B, surfE), t);
	m.alpha = A.alpha + (B.alpha - A.alpha) * t;
	m.tipR = lerpLog(A.tipR, B.tipR, t);
	return m;
}

static const float LIP_RUN = 1.1f;  // a lip's length, against the slit's
static const float OUTK = 15.5f;     // volts per unit; calibrated in tools/garamut-proto.cpp
static const float TUBE_K = 0.35f;   // the column's air modes against the Helmholtz mode
// The mallet's own radiation: a rigid sphere of volume (4/3) pi r^3 whose
// acceleration changes radiates p ~ V (da/dt) / c, against the lips' p ~ dU/dt.
static const float MALLET_K = 4.18879f / 343.f;

// ── one resonator ───────────────────────────────────────────────────────────
// A damped oscillator driven by a force. y advances by rotation. Im(y) is the
// displacement and omega Re(y) the velocity, so a force impulse J, which changes
// the velocity by J / M, adds J / (M omega) to Re(y). (Adding it to Im(y) moves
// the lip instantly instead, and every strike then creates energy.)
struct Mode {
	float yr = 0.f, yi = 0.f;     // state
	float pr = 1.f, pi = 0.f;     // per-sample rotation
	float sr = 1.f, si = 0.f;     // per-substep rotation
	float omega = 1.f;            // rad/s
	float inj = 0.f;              // 1 / (M omega)
	inline void rotate() { float r = yr * pr - yi * pi; yi = yr * pi + yi * pr; yr = r; }
	inline void rotateSub() { float r = yr * sr - yi * si; yi = yr * si + yi * sr; yr = r; }
};

struct Mallet {
	bool active = false;
	float tipR = 0.01f;
	float lastF = 0.f;
	int side = 0;
	float p = 0.f, v = 0.f;        // position and velocity into the lip, m and m/s
	float mass = 0.05f, k = 1e8f, alpha = 1.5f;
	float phi[NM] = {};            // mode shapes at the strike point, this lip
	float phiX[NM] = {};           // and at the same height on the other lip
	float age = 0.f;
	float y = 0.5f;                // for the screen
	float peakF = 0.f;
	float contactT = 0.f;          // seconds in contact so far
};

// ── a roll ──────────────────────────────────────────────────────────────────
// Strokes for as long as a gate is high, from two hands taking turns. The first
// stroke lands on the gate's rising edge. Each later one is 1 / rate after the
// last, give or take 6%; the second hand lands 8% lighter than the first and a
// little off its spot, and each stroke's velocity varies by 5%. Those numbers are
// estimates of a steady hand roll, not measurements.
struct Roller {
	bool was = false;
	float wait = 0.f;          // seconds to the next stroke
	int hand = 0;
	uint32_t rng = 0x2545F491u;

	float uni() { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.f; }   // 0..1

	// Advance by dt. Returns true when a stroke lands; velMul and dy describe it.
	bool step(bool high, float rate, float dt, float& velMul, float& dy) {
		bool fire = false;
		if (high && !was) { fire = true; hand = 0; wait = 0.f; }
		else if (high) {
			wait -= dt;
			if (wait <= 0.f) { fire = true; hand ^= 1; }
		}
		was = high;
		if (!fire) return false;
		float r = std::max(0.5f, rate);
		wait += (1.f + 0.12f * (uni() - 0.5f)) / r;
		if (wait < 0.f) wait = 0.f;
		velMul = (hand ? 0.92f : 1.f) * (1.f + 0.1f * (uni() - 0.5f));
		dy = (hand ? 0.025f : -0.015f) + 0.02f * (uni() - 0.5f);
		return true;
	}
	void reset() { was = false; wait = 0.f; hand = 0; }
};

// ── the drum ────────────────────────────────────────────────────────────────
struct Drum {
	// read by the screen
	float lipF0[2] = {0, 0};
	float fH = 0.f, fTube[NT] = {};
	float modeF[2][NM] = {};
	float lipLevel[2] = {0, 0};      // smoothed displacement, for drawing
	float airLevel = 0.f;
	float contactMs = 0.f;           // the last strike's contact time
	float H = 1.f, R = 0.15f, slitW = 0.02f, slit0 = 0.3f, slit1 = 0.7f;
	float sq = 0.f, pear = 0.f;
	int mn[NM][2];                   // (m, n) of each mode

	Mode lip[2][NM];
	Mode air;                        // Helmholtz
	Mode tube[NT];
	Mallet mal[NMAL];

	float srate = 48000.f, T = 1.f / 48000.f;
	float uw[2][NM] = {};            // net volume swept per unit displacement, m^2
	float dip[2][NM] = {};           // the part that radiates without cancelling
	float tubeW[NT] = {};
	float airQ = 10.f;
	float gain = 1.f, gainT = 1.f;
	float lipCouple = 0.06f;         // force reaching the other lip through the ends
	float quiet = 0.f;
	bool asleep = true;

	// what update() worked from
	Params last;
	bool built = false;
	float shapeG = 1.f;              // the warp exponent for the lips
	float wNorm[NMA] = {}, wInt[NMA] = {};
	float baseOmega[2][NM] = {}, baseZeta[2][NM] = {};
	float baseMass[2] = {1, 1};
	float baseAir = 1.f, baseTube[NT] = {};
	float pitchK = 1.f;
	MalletNow malNow;
	float malScale = 1.f;            // the mallet's size against the one at SIZE 0.5 (SIZE only)
	float pMalletPeak = 0.f;         // for the harness
	float malLp = 0.f, malLpK = 1.f;

	Drum() {
		int k = 0;
		for (int n = 0; n < NMC; n++) for (int m = 0; m < NMA; m++) { mn[k][0] = m; mn[k][1] = n; k++; }
		tubeTab();
	}

	void setSampleRate(float sr) {
		srate = sr; T = 1.f / sr; built = false;
		malLpK = 1.f - std::exp(-2.f * (float)PI_D * std::min(12000.f, 0.4f * sr) / sr);
	}

	// The (m, n) lip mode's omega^2 = plate * h^2 + shell. The plate term is
	// bending; the shell term is the stiffness a curved lip gains from its curvature.
	static void lipTerms(int m, int n, float L, float b, float kappa, const MatNow& M, float& plate, float& shell) {
		float ky = (float)beams().ccBeta[m] / L, kx = (float)beams().cfBeta[n] / b;
		float ky2 = ky * ky, kx2 = kx * kx;
		float D = 1.f / (12.f * M.rho * (1.f - M.nu * M.nu));
		float Exy = std::sqrt(M.EL * M.ET);
		plate = D * (M.EL * ky2 * ky2 + M.ET * kx2 * kx2 + 2.f * 0.6f * Exy * ky2 * kx2);
		// A slit cylinder can bend without stretching much, so only a small part
		// (0.05) of a closed shell's membrane stiffness is kept.
		float k2 = ky2 + kx2;
		shell = 0.05f * (Exy / M.rho) * kappa * kappa * (ky2 * ky2) / (k2 * k2);
	}
	static float lipOmega(int m, int n, float h, float L, float b, float kappa, const MatNow& M) {
		float a, c; lipTerms(m, n, L, b, kappa, M, a, c);
		return std::sqrt(a * h * h + c);
	}

	void update(const Params& P) {
		if (built && P == last) return;
		bool geo = !built || P.size != last.size || P.shape != last.shape || P.material != last.material
		        || P.slit != last.slit || P.interval != last.interval || P.decay != last.decay;
		bool mal = !built || P.mallet != last.mallet || P.material != last.material;
		bool pitch = geo || P.voct != last.voct;   // and everything that scales the mallet
		last = P;
		if (geo) buildGeometry(P);
		MatNow M = materialAt(P.material);
		if (mal || pitch) {
			pitchK = std::exp2(P.voct);
			malNow = malletAt(P.mallet, M.surfE);
			// A player chooses a mallet to suit the drum. Its mass scales with the drum's
			// volume and its tip with the drum's height, so the strike stands in the same
			// proportion to the lip at every SIZE (the reference is SIZE 0.5).
			//
			// V/OCT and TUNE keep the lip's mass and make it stiffer (omega^2 goes as
			// pitch^2), so the mallet keeps its mass and its contact stiffens by the
			// same factor. The collision then keeps its proportions in time as well:
			// without that, a felt mallet on a drum an octave up stays on the lip for
			// several of its periods and the drum is 30 dB quieter.
			malScale = H / heightOf(0.5f);
			float s = malScale;
			malNow.mass *= s * s * s;
			malNow.tipR *= s;
			malNow.k = rescaleK(P.mallet, M.surfE, s) * pitchK * pitchK;
			setRotations();
		}
		built = true;
	}

	void buildGeometry(const Params& P) {
		const TubeTab& TT = tubeTab();
		MatNow M = materialAt(P.material);
		shapeParts(P.shape, sq, pear);
		H = heightOf(P.size);
		R = 0.17f * H;
		// table lookup on shape
		float sp = std::max(0.f, std::min(1.f, P.shape)) * (SHAPE_TAB - 1);
		int si = std::min((int)sp, SHAPE_TAB - 2);
		float st = sp - si;
		slitSpan(P.slit, pear, slit0, slit1);
		float L = (slit1 - slit0) * H * LIP_RUN;         // the lip runs a little past the slit's ends
		// the profile over the slit
		float rbar = 0.f;
		for (int i = 0; i < 16; i++) rbar += profileR(slit0 + (slit1 - slit0) * (i + 0.5f) / 16.f, pear) / 16.f;
		rbar *= R;
		slitW = R * (0.05f + 0.25f * P.slit);
		float b = rbar * (0.9f + 0.1f * sq) - 0.5f * slitW;
		float kappa = (1.f - sq) / rbar;
		float h0 = 0.045f * R * M.thick;
		for (int m = 0; m < NMA; m++) {
			wNorm[m] = TT.wNorm[si][m] + (TT.wNorm[si + 1][m] - TT.wNorm[si][m]) * st;
			wInt[m]  = TT.wInt[si][m]  + (TT.wInt[si + 1][m]  - TT.wInt[si][m])  * st;
		}
		shapeG = warpGamma(pear);
		// right lip thickness so its fundamental sits `interval` above the left's
		float target = std::exp2(P.interval / 12.f);
		float a0, c0; lipTerms(0, 0, L, b, kappa, M, a0, c0);
		float w02 = a0 * h0 * h0 + c0;
		// omega^2 = a h^2 + c, solved for h. Where the curvature alone already puts the
		// lip above the target the interval cannot be reached; the lip is then as thin
		// as it is allowed to be.
		float hR = std::sqrt(std::max(target * target * w02 - c0, 0.04f * a0 * h0 * h0) / a0);
		float hs[2] = {h0, hR};
		// Cross-lip integral of the cantilever, normalised: 2 sigma / beta.
		float CF_INT[NMC];
		for (int n = 0; n < NMC; n++) CF_INT[n] = (float)(2.0 * beams().cfSigma[n] / beams().cfBeta[n]);
		for (int s = 0; s < 2; s++) {
			baseMass[s] = M.rho * hs[s] * L * b;
			for (int k = 0; k < NM; k++) {
				int m = mn[k][0], n = mn[k][1];
				float w = lipOmega(m, n, hs[s], L, b, kappa, M);
				baseOmega[s][k] = w;
				float f = w / (2.f * (float)PI_D);
				float eta = M.eta * std::pow(f / 250.f, M.etaSlope) + ETA_MOUNT;
				baseZeta[s][k] = 0.5f * eta / std::max(0.05f, P.decay);
				uw[s][k] = wInt[m] * CF_INT[n] * L * b;
				// a small part that radiates without cancelling, rising with frequency
				float kl = w * L / C_AIR;
				dip[s][k] = 0.08f * L * b * std::min(1.f, 0.5f * kl) * ((m + n) % 2 ? -1.f : 1.f);
			}
		}
		// Helmholtz
		float vol = (TT.vol[si] + (TT.vol[si + 1] - TT.vol[si]) * st) * R * R * H;
		float Ls = (slit1 - slit0) * H;
		float A = Ls * slitW;
		float tw = WALL * rbar;
		float delta = (2.f * slitW / (float)PI_D) * (1.f + std::log(std::max(1.5f, Ls / slitW)));
		float Leff = tw + delta;
		baseAir = C_AIR * std::sqrt(A / (vol * Leff));
		airQ = 6.f + 20.f * P.slit;
		// tube modes
		for (int j = 0; j < NT; j++) {
			float om = TT.omega[si][j] + (TT.omega[si + 1][j] - TT.omega[si][j]) * st;
			baseTube[j] = om * C_AIR / H;
			// coupling: mean pressure shape over the slit
			float acc = 0.f;
			for (int i = 0; i < 16; i++) {
				float eta = slit0 + (slit1 - slit0) * (i + 0.5f) / 16.f;
				float x = eta * WN - 0.5f;
				int i0 = std::max(0, std::min(WN - 2, (int)std::floor(x)));
				float fr = std::max(0.f, std::min(1.f, x - i0));
				float a0 = TT.psi[si][j][i0] + (TT.psi[si][j][i0 + 1] - TT.psi[si][j][i0]) * fr;
				float a1 = TT.psi[si + 1][j][i0] + (TT.psi[si + 1][j][i0 + 1] - TT.psi[si + 1][j][i0]) * fr;
				acc += (a0 + (a1 - a0) * st) / 16.f;
			}
			tubeW[j] = acc;
		}
	}

	void setRotations() {
		for (int s = 0; s < 2; s++) for (int k = 0; k < NM; k++) {
			float w = baseOmega[s][k] * pitchK;
			setMode(lip[s][k], w, baseZeta[s][k]);
			lip[s][k].inj = 1.f / (baseMass[s] * w);
			modeF[s][k] = w / (2.f * (float)PI_D);
		}
		lipF0[0] = modeF[0][0]; lipF0[1] = modeF[1][0];
		// Level: the left lip's fundamental, struck at its antinode by a unit impulse,
		// radiates the same whatever the size, shape, slit, material and pitch.
		float phiMax = 1.6f * 2.f;
		float w0 = baseOmega[0][0] * pitchK;
		float meff = baseMass[0] / (phiMax * phiMax);
		(void)meff;
		// The mallet's mass scales with the drum (see update()), and the impulse it
		// gives scales with its mass, so that scale is divided back out.
		float share = malScale * malScale * malScale;
		float taper = 1.f;
		gainT = taper * baseMass[0] / std::max(1e-12f, (std::fabs(uw[0][0]) + std::fabs(dip[0][0])) * phiMax * w0 * share);
		if (!built) gain = gainT;
		float wa = baseAir * pitchK;
		setMode(air, wa, 0.5f / airQ);
		air.inj = 1.f / wa;
		fH = wa / (2.f * (float)PI_D);
		for (int j = 0; j < NT; j++) {
			float w = baseTube[j] * pitchK;
			setMode(tube[j], w, 0.5f / 30.f);
			tube[j].inj = 1.f / w;
			fTube[j] = w / (2.f * (float)PI_D);
		}
	}

	void setMode(Mode& M, float w, float zeta) {
		float nyq = 0.45f * 2.f * (float)PI_D * srate;
		M.omega = w;
		float ww = std::min(w, nyq);
		float dec = std::exp(-zeta * w * T);
		M.pr = dec * std::cos(ww * T); M.pi = dec * std::sin(ww * T);
		float ds = std::exp(-zeta * w * T / SUB);
		M.sr = ds * std::cos(ww * T / SUB); M.si = ds * std::sin(ww * T / SUB);
		if (w > nyq) { M.pr *= 0.f; M.pi *= 0.f; M.sr = 0.f; M.si = 0.f; }   // above Nyquist: silent
	}

	// The lip's mode shapes at height y (0 bottom of the slit, 1 top), at the
	// strike line just in from the free edge.
	// y runs along the slit; the lip runs a little past it at both ends (LIP_RUN).
	void shapesAt(float y, float* phi) const {
		static const float XI = 0.92f;
		const Beams& B = beams();
		float z[NMC];
		for (int n = 0; n < NMC; n++) z[n] = (float)beamShape(B.cfBeta[n], B.cfSigma[n], XI);
		double lipPos = 0.5 + (std::max(0.0, std::min(1.0, (double)y)) - 0.5) / LIP_RUN;
		double e = std::pow(lipPos, (double)shapeG);
		for (int k = 0; k < NM; k++) {
			int m = mn[k][0], n = mn[k][1];
			phi[k] = (float)beamShape(B.ccBeta[m], B.ccSigma[m], e) * wNorm[m] * z[n];
		}
	}

	// A strike. side 0 = the left lip, 1 = the right. vel 0..1.
	void strike(int ch, int side, float y, float vel) {
		Mallet& M = mal[(ch % NCH) * 2 + (side & 1)];
		M.active = true;
		M.side = side & 1;
		M.y = std::max(0.f, std::min(1.f, y));
		shapesAt(M.y, M.phi);
		for (int k = 0; k < NM; k++) M.phiX[k] = M.phi[k];
		M.mass = malNow.mass; M.k = malNow.k; M.alpha = malNow.alpha; M.tipR = malNow.tipR;
		M.lastF = 0.f;
		float u = 0.f;
		for (int k = 0; k < NM; k++) u += M.phi[k] * lip[M.side][k].yi;
		M.p = u;
		M.v = 0.2f + 3.8f * std::max(0.f, std::min(1.f, vel));
		M.age = 0.f;
		M.peakF = 0.f;
		M.contactT = 0.f;
		asleep = false;
		quiet = 0.f;
	}

	bool anyActive() const {
		for (int i = 0; i < NMAL; i++) if (mal[i].active) return true;
		return false;
	}

	// One sample. Returns mid (the whole drum) and side (left lip minus right).
	void process(float& outL, float& outR) {
		// Asleep, nothing is sounding, so the output gain takes its new value at once.
		// (It glides only while the drum rings; leaving it where it was let the first
		// strike after a change in silence play at the old level.)
		if (asleep) { gain = gainT; outL = outR = 0.f; return; }
		bool contact = anyActive();
		float pMal = 0.f;
		if (contact) {
			float dt = T / SUB;
			for (int s = 0; s < SUB; s++) {
				float f[2][NM] = {};
				for (int i = 0; i < NMAL; i++) {
					Mallet& M = mal[i];
					if (!M.active) continue;
					const Mode* L = lip[M.side];
					float u = 0.f;
					for (int k = 0; k < NM; k++) u += M.phi[k] * L[k].yi;
					M.p += M.v * dt;
					float c = M.p - u;
					float F = 0.f;
					if (c > 0.f) {
						F = M.k * std::pow(c, M.alpha);
						M.v -= F / M.mass * dt;
						M.peakF = std::max(M.peakF, F);
						M.contactT += dt;
					}
					// The mallet radiates as it is stopped: a small rigid sphere whose
					// acceleration changes, heard as the click of a hard stick.
					pMal += MALLET_K * M.tipR * M.tipR * M.tipR * (F - M.lastF) / (dt * M.mass) / SUB;
					M.lastF = F;
					M.age += dt;
					if ((c < -0.002f && M.v < 0.f) || M.age > 0.05f) {
						M.active = false;
						contactMs = 1000.f * M.contactT;
						continue;
					}
					// positive displacement is inward, the way the mallet pushes
					for (int k = 0; k < NM; k++) {
						f[M.side][k] += F * M.phi[k];
						f[1 - M.side][k] += lipCouple * F * M.phiX[k];
					}
				}
				for (int sd = 0; sd < 2; sd++) for (int k = 0; k < NM; k++) {
					Mode& m = lip[sd][k];
					m.yr += f[sd][k] * dt * m.inj;
					m.rotateSub();
				}
			}
		} else {
			for (int sd = 0; sd < 2; sd++) for (int k = 0; k < NM; k++) lip[sd][k].rotate();
		}

		// U is the volume per second the lips push into the cavity. What leaves the
		// drum is the slit's outward flow minus U, so its pressure is the time
		// derivative of that. The lips' part is -uw * acceleration.
		float U = 0.f, pLip[2] = {0, 0};
		for (int sd = 0; sd < 2; sd++) for (int k = 0; k < NM; k++) {
			const Mode& m = lip[sd][k];
			float vel = m.omega * m.yr;
			float acc = -m.omega * m.omega * m.yi;
			U += uw[sd][k] * vel;
			pLip[sd] -= (uw[sd][k] + dip[sd][k]) * acc;
		}
		// The slit's flow follows U through the Helmholtz resonance: all of it below
		// the resonance, none of it well above. The column's air modes follow U
		// weighted by their pressure where the slit is.
		air.yr += U * air.omega * air.omega * T * air.inj;
		air.rotate();
		float pAir = air.omega * air.yr;
		float pTube = 0.f;
		for (int j = 0; j < NT; j++) {
			Mode& t = tube[j];
			t.yr += tubeW[j] * U * t.omega * t.omega * T * t.inj;
			t.rotate();
			pTube += TUBE_K * tubeW[j] * t.omega * t.yr;
		}
		// low-passed at 12 kHz: the derivative is taken at the substep rate
		malLp += (pMal - malLp) * malLpK;
		pMalletPeak = std::max(pMalletPeak, std::fabs(malLp * gain * OUTK));
		float mid = pLip[0] + pLip[1] + pAir + pTube + malLp;
		float side = 0.5f * (pLip[0] - pLip[1]);
		gain += (gainT - gain) * 0.002f;
		float g = gain * OUTK;
		outL = (mid + side) * g;
		outR = (mid - side) * g;
		// A value that is not finite would otherwise stay in the resonators and the
		// click filter for good. Clear the drum and carry on from silence.
		if (!std::isfinite(outL) || !std::isfinite(outR)) {
			reset();
			malLp = 0.f;
			outL = outR = 0.f;
			return;
		}

		// for the screen: how loud each lip and the air are, in volts
		for (int sd = 0; sd < 2; sd++) lipLevel[sd] += (std::fabs(pLip[sd]) * g - lipLevel[sd]) * 0.002f;
		airLevel += (std::fabs(pAir + pTube) * g - airLevel) * 0.002f;

		// sleep once everything has rung out
		if (!contact && std::fabs(outL) + std::fabs(outR) < 2e-5f) {
			quiet += T;
			if (quiet > 0.25f) sleepAll();
		} else quiet = 0.f;
	}

	void sleepAll() {
		asleep = true;
		for (int sd = 0; sd < 2; sd++) for (int k = 0; k < NM; k++) lip[sd][k].yr = lip[sd][k].yi = 0.f;
		air.yr = air.yi = 0.f;
		for (int j = 0; j < NT; j++) tube[j].yr = tube[j].yi = 0.f;
	}

	void reset() {
		sleepAll();
		for (int i = 0; i < NMAL; i++) mal[i].active = false;
	}

};

} // namespace garamut

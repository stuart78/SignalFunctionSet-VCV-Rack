#pragma once
// A hi-hat as two plates that touch, for Kit. The model is round Q1 of
// tools/hat-plate-harness.cpp, settled by ear on 2026-09-23; the harness is the
// reference and docs/kit-hat-design.md has the physics and why each part is
// there. This is the same model with fixed-size arrays laid out so every loop
// over the modes vectorises, and with the sample rate taken from the host.
//
// Two modal plates collide at eight points round a warped, tilted rim; the
// pedal sets the gap. Above 5 kHz each plate has more modes than bandwidth, so
// there the plate is a statistical field -- energy per quarter-octave band,
// heard as band-passed noise -- fed by the collisions and by the top explicit
// modes, cascading upward, and drained by a loss that grows with amplitude.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace sfs {

// Four floats as one register. GCC and Clang both take this on every target
// Rack builds for (x86 SSE, ARM NEON); the loops below are written in it
// because left to itself the vectoriser went across blocks instead of points.
typedef float hat4 __attribute__((vector_size(16)));
static inline hat4 hat4load(const float* p) { hat4 v; std::memcpy(&v, p, sizeof v); return v; }
static inline void hat4store(float* p, hat4 v) { std::memcpy(p, &v, sizeof v); }
static inline hat4 hat4splat(float x) { hat4 v = {x, x, x, x}; return v; }

struct Hat {
	static const int MAXM = 192;   // explicit modes per plate (150 / 133 at Q1)
	static const int NC = 8;       // contact points round the rim
	static const int NA = 16;      // angles the pressing is integrated over
	static const int NB = 9;       // field bands, quarter octaves from 5 kHz

	// ── Q1 constants ──────────────────────────────────────────────────────
	static constexpr float F2   = 220.f;    // top plate's first bending mode at 14"
	static constexpr float P    = 1.5f;     // mode grid exponent, f ~ (m + 2n)^P
	static constexpr float FJ   = 0.03f;    // random detune
	static constexpr float FX   = 5000.f;   // explicit modes stop, the field starts
	static constexpr float WARP = 0.2f;     // gap irregularity round the rim
	static constexpr float K3   = 3e6f;     // cubic rim stiffening
	static constexpr float KC   = 4.0e6f;   // Hertz contact stiffness
	static constexpr float PRESSD = 400.f;  // pressing damping per unit preload
	static constexpr float TILT = 0.6f;     // the bottom plate's tilt, in gap units
	static constexpr float DENS = 0.1f;     // the real plate's modal density, per Hz
	static constexpr float LEAK = 150.f;    // explicit -> field, 1/s at a full hit
	static constexpr float CASC = 40.f;     // band -> band, 1/s at a full hit
	static constexpr float NLD  = 1000.f;   // amplitude-dependent loss, 1/s at a full hit
	static constexpr float SEAG = 2.5f;     // field level
	static constexpr float CORR = 0.6f;     // field shared between L and R
	static constexpr float RTX  = 3.f;      // explicit modes' radiation tilt

	// ── set by Kit at control rate ────────────────────────────────────────
	float fscale  = 1.f;     // SIZE and TENSION: every frequency scales by this
	float ring    = 2.5f;    // DECAY: the clutch -- no mode rings longer (s)
	float radTilt = 0.75f;   // TONE: the field's radiation tilt
	float muffle  = 0.f;     // MUFFLE: a hand on the top plate
	float pedalT  = 0.f;     // PEDAL, 0 closed hard .. 1 wide open (slewed here)
	float strikeR = 0.85f, strikeAng = 0.2f;
	float outGain = 1.f;

	float sr = 48000.f, dt = 1.f / 48000.f;
	bool  ready = false;

	struct Plate {
		int n = 0;
		float mass = 1.f, invM = 1.f;
		int   mi[MAXM], ni[MAXM];
		float ph[MAXM], fref[MAXM], t60a[MAXM];
		float f[MAXM], w[MAXM], sigA[MAXM];   // sigA: pressing + hand, 1/s
		float pr[MAXM], pim[MAXM], re[MAXM], im[MAXM];
		float phiS[MAXM], phiM[2][MAXM], phiC[NC][MAXM], phiCw[NC][MAXM];
		float rim2[NA][MAXM], radW[2][MAXM], hi[MAXM];
		// The contact shapes again, blocked four modes at a time: block b holds
		// modes 4b..4b+3 for all eight points. One pass over these gives all
		// eight displacements (or velocities), or applies all eight forces --
		// where the [point][mode] layout took a pass per point per job, which
		// was 64 passes a sample and most of Kit's CPU. Padding modes are zero.
		hat4 cB[MAXM / 4][NC], cwB[MAXM / 4][NC];
		// G[j][i] = sum over modes of phiC[j] * phiC[i]: how much a unit push at
		// point i moves point j's velocity. It lets the contacts be applied in
		// ONE pass while still seeing each other in order, as the sequential
		// model does.
		float G[NC][NC];
	};
	Plate top, bot;
	float cth[NC], warp[NC];
	float gapJ[NC] = {0};    // each point's gap for the pedal in force; set in control()

	// field
	float bf[NB], bw[NB], bE[NB], bSig[NB], bSigP[NB], bOut[NB], bNorm[NB], bPend[NB], cInj[NB];
	struct BQ { float b0 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		inline float run(float x) { float y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; return y; } };
	BQ bq[NB][2];
	uint32_t nz[2] = {0x1234567u, 0x7654321u};
	float phiRim2 = 0.25f, phiStr2 = 0.25f, phiMic2 = 0.25f;
	float bAmp[NB] = {0};    // a band's output gain, sqrt(energy) in; set per control block
	float eRef0 = 1.f, strikeScale0 = 1.f, cmax = 0.f;
	float hpX[NC] = {0}, hpY[NC] = {0}, hpA = 0.5f;
	int   ctlN = 0;
	bool  calibrating = false, contactOn = true;
	float eAll = 0.f;        // last total energy, for quiet detection
	// stick
	int   stickN = 0, stickI = 0; float stickAmp = 0.f;
	// what control() last applied
	float ped = 0.f, pedDone = -1.f, mufDone = -1.f, fsDone = -1.f, ringDone = -1.f,
	      toneDone = -1.f, srDone = -1.f, saDone = -9.f;

	uint32_t rng = 0x9e3779b9u;
	float frand() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) * (1.f / 16777216.f); }
	float white(int c) { uint32_t& r = nz[c]; r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (r >> 8) * (2.f / 16777216.f) - 1.f; }

	// Radial ~0 at the clamped centre for m > 0, an antinode at the free rim, n
	// oscillations between. Not a Bessel solution (a cymbal is not a flat
	// plate) but it puts the right things at the rim, where the contact is.
	static float shape(int m, int n, float ph, float r, float th) {
		float rr = std::pow(r, (float)std::min(m, 3) * 0.5f + 0.5f);
		float rad = std::cos((float)M_PI * (n + 0.5f) * (1.f - r));
		return rr * rad * std::cos(m * th + ph);
	}

	// ── building (not per sample) ─────────────────────────────────────────
	void buildPlate(Plate& Pl, float f2, float t60at1k, float split, float mass) {
		Pl.n = 0; Pl.mass = mass; Pl.invM = 1.f / mass;
		for (int n = 0; n < 40; n++)
			for (int m = 0; m < 80; m++) {
				if (m == 0 && n == 0) continue;   // rigid body, held by the clutch
				if (m == 1 && n == 0) continue;   // rocking, ditto
				float fk = f2 * std::pow((m + 2.f * n) / 2.f, P);
				if (fk > FX) continue;
				int copies = m > 0 ? 2 : 1;
				for (int c = 0; c < copies; c++) {
					float det = (c == 0 ? -1.f : 1.f) * split * (0.3f + frand());
					float fr = fk * (1.f + (m > 0 ? det : 0.f)) * (1.f + 2.f * FJ * (frand() - 0.5f));
					float phs = c * (float)M_PI * 0.5f + 0.3f * frand();
					if (Pl.n >= MAXM) continue;
					int k = Pl.n++;
					Pl.mi[k] = m; Pl.ni[k] = n; Pl.ph[k] = phs; Pl.fref[k] = fr; Pl.t60a[k] = t60at1k;
					Pl.re[k] = Pl.im[k] = 0.f; Pl.sigA[k] = 0.f;
				}
			}
	}
	void shapes(Plate& Pl, float rs, float ths) {
		static const float micR[2] = {0.55f, 0.55f}, micT[2] = {0.9f, 2.3f};
		std::memset(Pl.cB, 0, sizeof(Pl.cB)); std::memset(Pl.cwB, 0, sizeof(Pl.cwB));
		for (int k = 0; k < Pl.n; k++) {
			Pl.phiS[k] = shape(Pl.mi[k], Pl.ni[k], Pl.ph[k], rs, ths);
			for (int c = 0; c < 2; c++) Pl.phiM[c][k] = shape(Pl.mi[k], Pl.ni[k], Pl.ph[k], micR[c], micT[c]);
			for (int j = 0; j < NC; j++) Pl.phiC[j][k] = shape(Pl.mi[k], Pl.ni[k], Pl.ph[k], 1.f, cth[j]);
			for (int a = 0; a < NA; a++) {
				float s = shape(Pl.mi[k], Pl.ni[k], Pl.ph[k], 1.f, 2.f * (float)M_PI * a / NA);
				Pl.rim2[a][k] = s * s;
			}
			for (int j = 0; j < NC; j++) Pl.cB[k >> 2][j][k & 3] = Pl.phiC[j][k];   // lane k&3
		}
		for (int j = 0; j < NC; j++)
			for (int i = 0; i < NC; i++) {
				double g = 0;
				for (int k = 0; k < Pl.n; k++) g += (double)Pl.phiC[j][k] * Pl.phiC[i][k];
				Pl.G[j][i] = (float)g;
			}
	}
	// Frequencies, poles and everything that follows from them. fs = the
	// frequency scale in force; mute anything the sample rate cannot carry.
	void modesFor(Plate& Pl, float fs, float radGain) {
		float rx = RTX, ref = std::pow(FX / 1000.f, radTilt - rx);
		float cap = 0.45f * sr;
		for (int k = 0; k < Pl.n; k++) {
			float fk = Pl.fref[k] * fs;
			bool on = fk < cap;
			Pl.f[k] = fk;
			Pl.w[k] = 2.f * (float)M_PI * fk;
			float sig = 6.9078f / std::min(ring, Pl.t60a[k]) + Pl.sigA[k];
			float g = std::exp(-sig * dt);
			Pl.pr[k]  = on ? g * std::cos(Pl.w[k] * dt) : 0.f;
			Pl.pim[k] = on ? -g * std::sin(Pl.w[k] * dt) : 0.f;
			for (int j = 0; j < NC; j++) Pl.cwB[k >> 2][j][k & 3] = Pl.phiCw[j][k] = Pl.phiC[j][k] / Pl.w[k];
			for (int c = 0; c < 2; c++) Pl.radW[c][k] = on ? radGain * Pl.phiM[c][k] * std::pow(fk / 1000.f, rx) * ref : 0.f;
			Pl.hi[k] = fk >= FX * 0.5f ? 1.f : 0.f;
		}
	}
	void buildField() {
		float rc = 1.f / (2.f * (float)M_PI * FX); hpA = rc / (rc + dt);
		for (int b = 0; b < NB; b++) {
			bf[b] = FX * std::pow(2.f, (b + 0.5f) * 0.25f);
			bw[b] = FX * (std::pow(2.f, (b + 1) * 0.25f) - std::pow(2.f, b * 0.25f));
			float w = 2 * (float)M_PI * std::min(bf[b], 0.45f * sr) / sr, Q = bf[b] / bw[b];
			float al = std::sin(w) / (2 * Q), cw = std::cos(w), a0 = 1 + al;
			for (int c = 0; c < 2; c++) {
				BQ& q = bq[b][c]; q = BQ();
				q.b0 = al / a0; q.b2 = -al / a0; q.a1 = -2 * cw / a0; q.a2 = (1 - al) / a0;
			}
			// the noise's variance through this band, measured once
			BQ t = bq[b][0]; uint32_t sv = nz[0]; double v = 0;
			int N = (int)sr;
			for (int i = 0; i < N; i++) { float y = t.run(white(0)); v += y * y; }
			nz[0] = sv;
			bNorm[b] = 1.f / std::sqrt((float)(v / N));
			cInj[b] = 0.5f * phiRim2 * DENS * bw[b] * (1.f + 1.f / 1.4f);
		}
		fieldGains();
	}
	void fieldGains() {
		for (int b = 0; b < NB; b++) {
			bSig[b] = 6.9078f / std::min(ring, 2.2f);
			bOut[b] = std::sqrt(phiMic2 * std::pow(bf[b] / 1000.f, 2.f * radTilt));
		}
		pressField();
	}
	void pressField() {
		// the field is pressed too: the mean extra loss of the top explicit modes
		float add = 0.f; int cnt = 0;
		for (int k = 0; k < top.n; k++) if (top.f[k] > FX * 0.5f) { add += top.sigA[k]; cnt++; }
		add = cnt ? add / cnt : 0.f;
		for (int b = 0; b < NB; b++) bSigP[b] = bSig[b] + add;
	}
	void clearState() {
		for (Plate* Pl : {&top, &bot}) for (int k = 0; k < Pl->n; k++) Pl->re[k] = Pl->im[k] = 0.f;
		for (int b = 0; b < NB; b++) { bE[b] = bPend[b] = bAmp[b] = 0.f; bq[b][0].x1 = bq[b][0].x2 = bq[b][0].y1 = bq[b][0].y2 = 0.f; bq[b][1] = bq[b][0]; }
		for (int b = 0; b < NB; b++) { BQ& q = bq[b][1]; q.x1 = q.x2 = q.y1 = q.y2 = 0.f; }
		for (int j = 0; j < NC; j++) hpX[j] = hpY[j] = 0.f;
		stickI = stickN = 0; eAll = 0.f; ctlN = 0;
	}

	// Build everything and calibrate, as the harness does: a vel-1 hit is
	// scaled to move the rim one gap unit, and the energy it leaves is what the
	// amplitude-dependent terms call "a full hit". Heavy (~15 ms): call it off
	// the audio thread, or from onSampleRateChange.
	void init(float sampleRate) {
		sr = sampleRate; dt = 1.f / sr;
		rng = 0x9e3779b9u;
		buildPlate(top, F2, 2.2f, 0.004f, 1.f);
		buildPlate(bot, F2 * 1.09f, 2.0f, 0.005f, 1.4f);
		for (int j = 0; j < NC; j++) cth[j] = 2.f * (float)M_PI * (j + 0.3f * frand()) / NC;
		for (int j = 0; j < NC; j++) warp[j] = WARP * (2.f * frand() - 1.f);
		shapes(top, 0.85f, 0.2f);
		shapes(bot, 0.5f, 0.f);                  // never struck
		float fs0 = fscale; fscale = 1.f;
		modesFor(top, 1.f, 1.f); modesFor(bot, 1.f, 0.7f);
		phiRim2 = phiStr2 = phiMic2 = 0.25f;
		buildField();
		float s2 = 0.f;
		for (int j = 0; j < NC; j++) {
			float a = 0.f;
			for (int k = 0; k < top.n; k++) a += top.phiC[j][k] * top.phiC[j][k] * top.invM;
			for (int k = 0; k < bot.n; k++) a += bot.phiC[j][k] * bot.phiC[j][k] * bot.invM;
			s2 = std::max(s2, a);
		}
		cmax = 0.5f / (dt * s2);

		calibrating = true; contactOn = false;
		clearState(); strikeScale0 = 1.f; eRef0 = 1.f;
		strikeRaw(1.f, 0.2f);
		float mx = 0.f, o[2];
		int n48 = (int)(0.1f * sr);
		for (int i = 0; i < n48; i++) { process(o, false); mx = std::max(mx, std::fabs(disp(top, 0))); }
		strikeScale0 = 1.f / std::max(mx, 1e-12f);
		auto m2 = [](const float* v, int n) { double a = 0; for (int i = 0; i < n; i++) a += v[i] * v[i]; return n ? (float)(a / n) : 0.25f; };
		phiStr2 = m2(top.phiS, top.n); phiMic2 = m2(top.phiM[0], top.n); phiRim2 = m2(top.phiC[0], top.n);
		clearState();
		buildField();
		strikeRaw(strikeScale0, 0.2f);
		for (int i = 0; i < (int)(0.001f * sr); i++) process(o, false);
		eRef0 = std::max(1e-9f, plateE(top, false));
		clearState();
		calibrating = false; contactOn = true;

		fscale = fs0;
		fsDone = ringDone = toneDone = mufDone = pedDone = -1.f; saDone = -9.f;
		srDone = sr;
		ready = true;
		control(0, true);
	}

	// ── control rate ──────────────────────────────────────────────────────
	// Cheap unless something moved. `n` samples since the last call.
	void control(int n, bool force = false) {
		ped += (pedalT - ped) * std::min(1.f, n / (0.02f * sr));
		if (force) ped = pedalT;
		bool press = force || std::fabs(ped - pedDone) > 1e-4f || std::fabs(muffle - mufDone) > 1e-4f;
		bool modes = press || std::fabs(fscale - fsDone) > 1e-5f || std::fabs(ring - ringDone) > 1e-4f
		          || std::fabs(radTilt - toneDone) > 1e-4f;
		if (std::fabs(strikeR - saDone) > 1e-3f || force) {
			for (int k = 0; k < top.n; k++) top.phiS[k] = shape(top.mi[k], top.ni[k], top.ph[k], strikeR, strikeAng);
			saDone = strikeR;
		}
		if (press) {
			pressModes(top, PRESSD, true); pressModes(bot, PRESSD * 0.7f, false);
			gaps();
			pedDone = ped; mufDone = muffle;
		}
		if (modes) {
			modesFor(top, fscale, 1.f); modesFor(bot, fscale, 0.7f);
			fieldGains();
			fsDone = fscale; ringDone = ring; toneDone = radTilt;
		}
	}
	float gapG() const {
		// Most of the pedal's travel is where the plates just touch on the
		// tilted side and rattle: pressed hard only at the bottom, apart at the top.
		return ped < 0.15f ? -0.5f + 0.35f * ped / 0.15f
		     : ped < 0.85f ? -0.15f + 0.45f * (ped - 0.15f) / 0.7f
		     : 0.3f + 2.7f * (ped - 0.85f) / 0.15f;
	}
	float gapAt(int j) const { return gapG() + TILT * (1.f - std::cos(cth[j])) + warp[j]; }
	// Only the pedal moves the gaps, so they are worked out when it does.
	void gaps() { float g = gapG(); for (int j = 0; j < NC; j++) gapJ[j] = g + TILT * (1.f - std::cos(cth[j])) + warp[j]; }
	void pressModes(Plate& Pl, float D, bool isTop) {
		float g = gapG(), pen0[NA];
		for (int a = 0; a < NA; a++) pen0[a] = std::max(0.f, -(g + TILT * (1.f - std::cos(2.f * (float)M_PI * a / NA))));
		for (int k = 0; k < Pl.n; k++) {
			float add = 0.f;
			for (int a = 0; a < NA; a++) add += pen0[a] * Pl.rim2[a][k];
			add *= 3.f / NA;
			// a hand on the top plate: pressing at one place on its rim
			if (isTop && muffle > 0.f) add += muffle * 1.5f * Pl.rim2[4][k];
			Pl.sigA[k] = D * add;
		}
	}

	// ── the hit ───────────────────────────────────────────────────────────
	void injectImpulse(float J, float phi2, float T) {
		for (int b = 0; b < NB; b++) {
			float w = 1.f;
			if (T > 0.f) { float x = bf[b] * T; w = std::fabs(x) < 1e-3f ? 1.f : std::fabs(std::sin((float)M_PI * x) / ((float)M_PI * x) / (1.f - x * x + 1e-6f)); }
			bPend[b] += 0.5f * J * J * phi2 * DENS * bw[b] * w * w;
		}
	}
	void strikeRaw(float amp, float ms) {
		stickN = std::max(2, (int)(ms * 0.001f * sr));
		stickI = 0; stickAmp = amp;
		injectImpulse(stickAmp, phiStr2, ms * 0.001f);
	}
	// Kit's strike: approach speed (0.4 + 9 vel), beater hardness, weight.
	// The hardest beater is Q1's 0.35 ms stick; a soft one is three times that.
	void strike(float speed, float hard, float weight) {
		if (!ready) return;
		float ms = 0.35f * std::pow(3.f, 1.f - hard) * std::sqrt(weight);
		strikeRaw(speed / 6.f * std::sqrt(weight) * strikeScale0 * fscale, ms);
	}

	// ── per sample ────────────────────────────────────────────────────────
	static inline float disp(const Plate& Pl, int j) {
		float x = 0.f; const float* a = Pl.phiCw[j]; const float* im = Pl.im;
		for (int k = 0; k < Pl.n; k++) x -= a[k] * im[k];
		return x;
	}
	static inline float vel(const Plate& Pl, int j) {
		float v = 0.f; const float* a = Pl.phiC[j]; const float* re = Pl.re;
		for (int k = 0; k < Pl.n; k++) v += a[k] * re[k];
		return v;
	}
	inline void push(Plate& Pl, const float* phi, float F) {
		float s = F * dt * Pl.invM; float* re = Pl.re;
		for (int k = 0; k < Pl.n; k++) re[k] += s * phi[k];
	}
	// All eight points at once: x[j] = sum over modes of B[mode][j] * v[mode].
	// Eight accumulators, one per point, each four modes wide.
	static inline void dot8(const hat4 (*B)[NC], const float* v, int n, float* x) {
		hat4 acc[NC];
		for (int j = 0; j < NC; j++) acc[j] = hat4splat(0.f);
		int nb = (n + 3) >> 2;
		for (int b = 0; b < nb; b++) {
			hat4 vb = hat4load(v + 4 * b);
			for (int j = 0; j < NC; j++) acc[j] += B[b][j] * vb;
		}
		for (int j = 0; j < NC; j++) x[j] = (acc[j][0] + acc[j][1]) + (acc[j][2] + acc[j][3]);
	}
	// Apply a force at each of the eight points in one pass over the modes.
	inline void push8(Plate& Pl, const float* F) {
		hat4 s[NC];
		for (int j = 0; j < NC; j++) s[j] = hat4splat(F[j] * dt * Pl.invM);
		int nb = (Pl.n + 3) >> 2;
		for (int b = 0; b < nb; b++) {
			hat4 a = Pl.cB[b][0] * s[0];
			for (int j = 1; j < NC; j++) a += Pl.cB[b][j] * s[j];
			hat4store(Pl.re + 4 * b, hat4load(Pl.re + 4 * b) + a);
		}
	}
	static inline void step(Plate& Pl) {
		float* re = Pl.re; float* im = Pl.im; const float* pr = Pl.pr; const float* pi = Pl.pim;
		for (int k = 0; k < Pl.n; k++) {
			float a = re[k], b = im[k];
			re[k] = a * pr[k] - b * pi[k];
			im[k] = a * pi[k] + b * pr[k];
		}
	}
	static inline float plateE(const Plate& Pl, bool hiOnly) {
		float e = 0.f;
		for (int k = 0; k < Pl.n; k++) e += (hiOnly ? Pl.hi[k] : 1.f) * (Pl.re[k] * Pl.re[k] + Pl.im[k] * Pl.im[k]);
		return 0.5f * Pl.mass * e;
	}
	static inline void scale(Plate& Pl, float g, bool hiOnly) {
		for (int k = 0; k < Pl.n; k++) { float s = hiOnly ? (Pl.hi[k] > 0.f ? g : 1.f) : g; Pl.re[k] *= s; Pl.im[k] *= s; }
	}
	static inline float hertz(float pen) { return pen > 0.f ? KC * pen * std::sqrt(pen) : 0.f; }

	void fieldControl(int n) {
		float t = n * dt;
		float eRef = eRef0 * fscale * fscale;
		eAll = plateE(top, false) + plateE(bot, false);
		for (int b = 0; b < NB; b++) eAll += bE[b];
		float nl = eAll / eRef;
		// the top octave of explicit modes leaks up, landing across every band
		// at once (walked band by band it was a pitch bend)
		float eHi = plateE(top, true) + plateE(bot, true);
		if (eHi > 0.f) {
			float frac = std::min(0.5f, LEAK * nl * t);
			float g = std::sqrt(std::max(0.f, 1.f - frac));
			scale(top, g, true); scale(bot, g, true);
			float e = eHi * frac, tot = 0.f;
			for (int b = 0; b < NB; b++) tot += bw[b];
			for (int b = 0; b < NB; b++) bE[b] += e * bw[b] / tot;
		}
		// loss that grows with amplitude: fast while loud, then the long tail
		if (!calibrating) {
			float g = std::exp(-NLD * nl * t);
			scale(top, g, false); scale(bot, g, false);
			for (int b = 0; b < NB; b++) bE[b] *= g * g;
		}
		float cr = std::min(0.5f, CASC * nl * t);
		for (int b = NB - 1; b >= 0; b--) {
			float J = cr * bE[b]; bE[b] -= J; if (b < NB - 1) bE[b + 1] += J;
		}
		for (int b = 0; b < NB; b++) {
			bE[b] += bPend[b]; bPend[b] = 0.f;
			bE[b] *= std::exp(-2.f * bSigP[b] * t);
			// bE only changes here, so its square root need not be taken per sample
			bAmp[b] = bE[b] > 0.f ? bNorm[b] * bOut[b] * std::sqrt(bE[b]) * SEAG : 0.f;
		}
	}

	// out[0..1]; mono puts channel 0 in both.
	void process(float* out, bool stereo) {
		if (stickI < stickN) {
			float p = (stickI + 0.5f) / stickN;
			float F = stickAmp * 2.f / (stickN * dt) * 0.5f * (1.f - std::cos(2.f * (float)M_PI * p));
			push(top, top.phiS, -F);          // downward, onto the bottom plate
			stickI++;
		}
		// ── the rim: the plate's own stiffening, and the two plates meeting ──
		// Written as one gather (every point's displacement), a sequential
		// decision per point, and one scatter (every point's force). The
		// sequential model let point j see the velocity changes of the forces
		// already applied this sample (the stiffening at points 0..j, contacts
		// at 0..j-1); G reproduces exactly that, so this is the same model
		// with a quarter of the memory traffic.
		float xt[NC], xb[NC];
		dot8(top.cwB, top.im, top.n, xt);
		dot8(bot.cwB, bot.im, bot.n, xb);
		float kt[NC], kb[NC], ct[NC] = {0.f}, cb[NC] = {0.f};
		bool any = false;
		for (int j = 0; j < NC; j++) {
			xt[j] = -xt[j]; xb[j] = -xb[j];
			// Below a rim movement of 0.003 gap units the stiffening is under
			// 0.002% of the linear restoring force; skipping it there lets a
			// quiet, open hat skip the scatter altogether.
			kt[j] = std::fabs(xt[j]) > 0.003f ? -K3 * xt[j] * xt[j] * xt[j] : 0.f;
			kb[j] = std::fabs(xb[j]) > 0.003f ? -K3 * xb[j] * xb[j] * xb[j] : 0.f;
			any = any || kt[j] != 0.f || kb[j] != 0.f;
		}
		if (contactOn) {
			float g[NC], pen[NC];
			bool touching = false;
			for (int j = 0; j < NC; j++) {
				g[j] = gapJ[j]; pen[j] = (xb[j] - xt[j]) - g[j];
				touching = touching || pen[j] > 0.f;
			}
			float vt[NC], vb[NC];
			if (touching) { dot8(top.cB, top.re, top.n, vt); dot8(bot.cB, bot.re, bot.n, vb); }
			const float st = dt * top.invM, sb = dt * bot.invM;
			for (int j = 0; j < NC; j++) {
				float pen0 = -g[j], F;
				if (pen[j] > 0.f) {
					float at = 0.f, ab = 0.f;
					for (int i = 0; i <= j; i++) { at += top.G[j][i] * kt[i]; ab += bot.G[j][i] * kb[i]; }
					for (int i = 0; i < j; i++)  { at += top.G[j][i] * ct[i]; ab += bot.G[j][i] * cb[i]; }
					float dv = (vb[j] + sb * ab) - (vt[j] + st * at);
					float c = std::min(KC * pen[j] * std::sqrt(pen[j]) * 1e-3f, cmax);
					F = hertz(pen[j]) + c * dv - hertz(pen0);
				} else F = -hertz(pen0);
				// dynamic force about the pressed rest state; repulsive
				ct[j] = F; cb[j] = -F;
				any = any || F != 0.f;
				// only what changes faster than the field's cutoff reaches it
				float hp = hpA * (hpY[j] + F - hpX[j]); hpX[j] = F; hpY[j] = hp;
				if (hp != 0.f) { float J = hp * dt, J2 = J * J; for (int b = 0; b < NB; b++) bPend[b] += J2 * cInj[b]; }
			}
		}
		if (any) {
			float ft[NC], fb[NC];
			for (int j = 0; j < NC; j++) { ft[j] = kt[j] + ct[j]; fb[j] = kb[j] + cb[j]; }
			push8(top, ft); push8(bot, fb);
		}
		step(top); step(bot);
		if (++ctlN >= 16) { fieldControl(ctlN); ctlN = 0; }

		float sh = std::sqrt(CORR), own = std::sqrt(1.f - CORR);
		int nch = stereo ? 2 : 1;
		float common[NB];
		for (int b = 0; b < NB; b++) common[b] = white(0);
		for (int c = 0; c < nch; c++) {
			float s = 0.f;
			for (int b = 0; b < NB; b++)
				if (bAmp[b] > 0.f) s += bq[b][c].run(sh * common[b] + own * white(1)) * bAmp[b];
			const float* wt = top.radW[c]; const float* wb = bot.radW[c];
			for (int k = 0; k < top.n; k++) s += wt[k] * top.re[k];
			for (int k = 0; k < bot.n; k++) s += wb[k] * bot.re[k];
			out[c] = s * outGain;
		}
		if (!stereo) out[1] = out[0];
	}

	// Everything in it, against a full hit: Kit's quiet test.
	float level() const { return eAll / std::max(1e-12f, eRef0 * fscale * fscale); }
	void clear() { clearState(); }
};

} // namespace sfs

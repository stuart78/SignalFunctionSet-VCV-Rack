#include "plugin.hpp"
#include <cmath>
#include <vector>

// SWING — Double-pendulum chaos LFO.
//
// A normalized double pendulum (m1=m2=1, l1=l2=1) is integrated with RK4 at
// audio rate. Its motion is read three ways:
//   * X / Y          — the lower bob (tip) position, bipolar +/-5V
//   * 6 SECTOR CVs    — distance-from-center, morph-crossfaded across the six
//                       60-degree wedges by the tip's angular position
//   * 6 GATE outs     — retriggerable holds fired as the tip sweeps across each
//                       of the six boundary rays (high while loitering, drops
//                       once the tip is clear)
//
// Controls: SPEED (sim time scale), CHAOS (energy target — tame swing -> full
// tumble), plus ragdoll drag of either bob on the display to relaunch motion.

static const int NUM_SECTORS = 6;
static const float DEG = (float) (M_PI / 180.0);

// Pendulum lengths / masses (normalized).
static const float L1 = 1.f;
static const float L2 = 1.f;
static const float M1 = 1.f;
static const float M2 = 1.f;
static const float GRAV = 9.81f;
static const float REACH = L1 + L2;   // max tip radius

// double clamp (rack::clamp only has int/float overloads).
static inline double clampd(double x, double lo, double hi) {
	return x < lo ? lo : (x > hi ? hi : x);
}

// Wrap an angle to (-pi, pi].
static inline float wrapPi(float a) {
	while (a > (float) M_PI) a -= (float) (2.0 * M_PI);
	while (a <= -(float) M_PI) a += (float) (2.0 * M_PI);
	return a;
}


struct Swing : Module {
	enum ParamId {
		SPEED_PARAM,
		CHAOS_PARAM,
		GRAVITY_PARAM,
		MODE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		SPEED_INPUT,
		CHAOS_INPUT,
		GRAVITY_INPUT,
		MODE_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		X_OUTPUT,
		Y_OUTPUT,
		RADIUS_OUTPUT,
		ANGLE_OUTPUT,
		ENUMS(SECTOR_OUTPUT, NUM_SECTORS),
		ENUMS(GATE_OUTPUT, NUM_SECTORS),
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	// --- Modes ---------------------------------------------------------------
	// Each mode produces a single "tracked point" (drives X/Y/RADIUS/ANGLE/
	// sectors) plus a set of "gate points" whose ray crossings fire the gates.
	enum Mode { MODE_PENDULUM = 0, MODE_GRAVWELL = 1, MODE_BILLIARDS = 2, NUM_MODES = 3 };
	int mode = MODE_PENDULUM;
	int prevMode = MODE_PENDULUM;

	// --- Gravity Well state --------------------------------------------------
	// N planets orbit the center (inner faster); a rocket is N-body pulled
	// between them. CHAOS sets planet count + launch energy.
	static const int MAX_PLANETS = 5;
	int   gwPlanetCount = 3;
	double gwPlanetAng[MAX_PLANETS]  = {};   // current orbital angle
	double gwPlanetRad[MAX_PLANETS]  = {};   // orbital radius (fixed per planet)
	double gwPlanetRate[MAX_PLANETS] = {};   // angular velocity (inner faster)
	double gwPlanetMass[MAX_PLANETS] = {};
	double gwRx = 0.6, gwRy = 0.0;           // rocket position (pendulum coords)
	double gwVx = 0.0, gwVy = 0.8;           // rocket velocity
	bool   gwInit = false;

	// --- Billiards state -----------------------------------------------------
	// Elastic balls in a circle. Ball 0 = cue (heavier, the tracked point).
	// CHAOS sets the count of non-cue balls. KE is normalized to stay alive.
	static const int MAX_BALLS = 9;          // cue + up to 8
	int    bzCount = 1;                       // active ball count (incl. cue)
	double bzX[MAX_BALLS]  = {};
	double bzY[MAX_BALLS]  = {};
	double bzVx[MAX_BALLS] = {};
	double bzVy[MAX_BALLS] = {};
	double bzMass[MAX_BALLS] = {};
	bool   bzInit = false;
	static constexpr double BALL_R   = 0.10;   // collision radius (pendulum units)
	static constexpr double CUE_MASS = 2.5;
	// Slingshot drag (cue): pull back, release launches opposite the drag.
	bool   bzAiming = false;
	double bzAimX = 0.0, bzAimY = 0.0;         // current drag point (pendulum coords)

	// Tracked point (filled by the active mode each sample; pendulum coords).
	float trackedX = 0.f, trackedY = 0.f;
	// Gate points for this sample (filled by the active mode).
	int   gatePtCount = 0;
	float gatePtX[MAX_BALLS] = {};
	float gatePtY[MAX_BALLS] = {};

	// --- Pendulum state (analytic double pendulum) ---
	double th1 = 2.6, th2 = 2.7;   // angles from downward vertical
	double w1 = 0.0,  w2 = 0.0;    // angular velocities
	double energyLP = 4.0;         // low-passed kinetic proxy for the servo

	// --- Drag (ragdoll) state, written by the display widget ---
	enum DragJoint { DRAG_NONE = 0, DRAG_ELBOW = 1, DRAG_TIP = 2 };
	int dragJoint = DRAG_NONE;
	float dragTargetX = 0.f, dragTargetY = 0.f;   // pendulum coords (+y down)
	double prevDragTh1 = 0.0, prevDragTh2 = 0.0;
	double w1Smooth = 0.0, w2Smooth = 0.0;

	// --- Centre of gravity: the direction (radians, 0 = straight down) the
	// gravity torque pulls toward. Rotating it physically reorients the pendulum;
	// CV-spinning it drags the whole thing around the circle. ---
	double gravAngle = 0.0;
	float gwSunPull = 0.5f;   // GW: central pull magnitude 0..1 (from GRAV knob)

	// --- Gate / sector engine ---
	// Per gate-point continuous-angle trackers so multiple points (billiards
	// balls) can each fire ray crossings independently.
	double gpUnwrapped[MAX_BALLS] = {};
	float  gpPrevPhi[MAX_BALLS]   = {};
	int    gpLastBucket[MAX_BALLS] = {};
	bool   gpInit[MAX_BALLS] = {};
	float gateTimer[NUM_SECTORS] = {};
	float sectorOut[NUM_SECTORS] = {};
	bool initialized = false;

	// --- Exposed for the display (UI thread reads these; benign races) ---
	float dispB1x = 0.f, dispB1y = 0.f;   // elbow, pendulum coords
	float dispB2x = 0.f, dispB2y = 0.f;   // tip
	float dispGateFlash[NUM_SECTORS] = {};

	// --- Options (context menu, persisted) ---
	float gateHoldSec = 0.06f;       // retriggerable gate hold time
	int trailLength = 90;            // trail ring-buffer length (frames); 0 = off

	Swing() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SPEED_PARAM, 0.f, 1.f, 0.4f, "Speed", "");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.6f, "Chaos", "");
		configParam(GRAVITY_PARAM, -1.f, 1.f, 0.f, "Gravity direction", " deg", 0.f, 180.f);
		configSwitch(MODE_PARAM, 0.f, 2.f, 0.f, "Mode",
			{"Pendulum", "Gravity Well", "Billiards"});
		paramQuantities[MODE_PARAM]->snapEnabled = true;
		configInput(SPEED_INPUT, "Speed CV");
		configInput(CHAOS_INPUT, "Chaos CV");
		configInput(GRAVITY_INPUT, "Gravity direction CV");
		configInput(MODE_INPUT, "Mode CV (0-10V -> 3 modes)");
		configOutput(X_OUTPUT, "Tip X");
		configOutput(Y_OUTPUT, "Tip Y");
		configOutput(RADIUS_OUTPUT, "Radius (folded -5V .. +5V extended)");
		configOutput(ANGLE_OUTPUT, "Angle from gravity (±180° -> ±5V)");
		for (int i = 0; i < NUM_SECTORS; i++) {
			configOutput(SECTOR_OUTPUT + i, string::f("Sector %d distance", i + 1));
			configOutput(GATE_OUTPUT + i, string::f("Ray %d gate", i + 1));
		}
	}

	// Forces (angular accelerations) of the analytic double pendulum.
	// Angular accelerations via the mass-matrix form. Gravity pulls toward
	// `gravAngle` (a movable centre of gravity): the gravity torques use
	// (theta - gravAngle), while the inertial/Coriolis terms depend only on the
	// angle difference and are unaffected. gravAngle = 0 -> straight down.
	void accel(double a1, double a2, double v1, double v2, double& o1, double& o2) {
		double d = a1 - a2;
		double cd = std::cos(d), sd = std::sin(d);
		double M11 = (M1 + M2) * L1;
		double M12 = M2 * L2 * cd;
		double M21 = L1 * cd;
		double M22 = L2;
		double R1 = -M2 * L2 * v2 * v2 * sd - (M1 + M2) * GRAV * std::sin(a1 - gravAngle);
		double R2 =  L1 * v1 * v1 * sd       - GRAV * std::sin(a2 - gravAngle);
		double det = M11 * M22 - M12 * M21;
		if (std::abs(det) < 1e-9) det = (det < 0 ? -1e-9 : 1e-9);
		o1 = (R1 * M22 - M12 * R2) / det;
		o2 = (M11 * R2 - M21 * R1) / det;
	}

	void rk4Step(double h) {
		double k1a1, k1a2; accel(th1, th2, w1, w2, k1a1, k1a2);
		double k1t1 = w1, k1t2 = w2;

		double k2a1, k2a2; accel(th1 + 0.5 * h * k1t1, th2 + 0.5 * h * k1t2,
		                         w1 + 0.5 * h * k1a1, w2 + 0.5 * h * k1a2, k2a1, k2a2);
		double k2t1 = w1 + 0.5 * h * k1a1, k2t2 = w2 + 0.5 * h * k1a2;

		double k3a1, k3a2; accel(th1 + 0.5 * h * k2t1, th2 + 0.5 * h * k2t2,
		                         w1 + 0.5 * h * k2a1, w2 + 0.5 * h * k2a2, k3a1, k3a2);
		double k3t1 = w1 + 0.5 * h * k2a1, k3t2 = w2 + 0.5 * h * k2a2;

		double k4a1, k4a2; accel(th1 + h * k3t1, th2 + h * k3t2,
		                         w1 + h * k3a1, w2 + h * k3a2, k4a1, k4a2);
		double k4t1 = w1 + h * k3a1, k4t2 = w2 + h * k3a2;

		th1 += (h / 6.0) * (k1t1 + 2 * k2t1 + 2 * k3t1 + k4t1);
		th2 += (h / 6.0) * (k1t2 + 2 * k2t2 + 2 * k3t2 + k4t2);
		w1  += (h / 6.0) * (k1a1 + 2 * k2a1 + 2 * k3a1 + k4a1);
		w2  += (h / 6.0) * (k1a2 + 2 * k2a2 + 2 * k3a2 + k4a2);
	}

	void relaunch() {
		// Throw it back up into the chaotic regime.
		th1 = 1.5 + 2.0 * (random::uniform() - 0.5) * M_PI;
		th2 = 1.5 + 2.0 * (random::uniform() - 0.5) * M_PI;
		w1 = (random::uniform() - 0.5) * 4.0;
		w2 = (random::uniform() - 0.5) * 4.0;
	}

	// Two-link inverse kinematics: angles that place the tip at (tx,ty).
	// Picks the elbow side nearest the current pose for continuity.
	void ikTip(double tx, double ty, double& outTh1, double& outTh2) {
		double dist = std::sqrt(tx * tx + ty * ty);
		dist = clampd(dist, (double) std::abs(L1 - L2) + 1e-3, (double) REACH - 1e-3);
		// Rescale target onto the reachable circle so trig stays valid.
		double ang = std::atan2(tx, ty);          // direction from down-vertical
		double cosB = (dist * dist + L1 * L1 - L2 * L2) / (2 * L1 * dist);
		cosB = clampd(cosB, -1.0, 1.0);
		double beta = std::acos(cosB);

		double cand1 = ang - beta;
		double cand2 = ang + beta;
		// Choose the candidate whose resulting th1 is closest to current.
		double pick = (std::abs(wrapPi(cand1 - th1)) <= std::abs(wrapPi(cand2 - th1)))
		            ? cand1 : cand2;
		outTh1 = pick;
		double b1x = L1 * std::sin(pick), b1y = L1 * std::cos(pick);
		outTh2 = std::atan2(tx - b1x, ty - b1y);
	}

	void process(const ProcessArgs& args) override {
		// --- Time scale ---
		float speed = params[SPEED_PARAM].getValue();
		if (inputs[SPEED_INPUT].isConnected())
			speed += inputs[SPEED_INPUT].getVoltage() / 10.f;   // ~1 oct / 10V handled below
		float speedOct = clamp(speed, -0.5f, 1.5f) * 9.f - 3.f;
		float speedMult = std::pow(2.f, speedOct);
		double h = args.sampleTime * speedMult;

		float chaos = params[CHAOS_PARAM].getValue();
		if (inputs[CHAOS_INPUT].isConnected())
			chaos += inputs[CHAOS_INPUT].getVoltage() / 10.f;
		chaos = clamp(chaos, 0.f, 1.f);

		// GRAVITY knob is mode-dependent:
		//  - Pendulum / Billiards: DIRECTION. Knob -1..1 -> -180..180 deg; CV
		//    adds 36 deg/V, so a 0-10V phasor spins gravity once around.
		//  - Gravity Well: MAGNITUDE of the central sun's pull (shown as a
		//    center circle sized by the amount).
		float grav = params[GRAVITY_PARAM].getValue();
		gravAngle = (double) grav * M_PI;
		if (inputs[GRAVITY_INPUT].isConnected())
			gravAngle += inputs[GRAVITY_INPUT].getVoltage() * (M_PI / 5.0);
		// GW pull: full knob sweep -1..+1 -> 0..1; CV nudges at 0.1/V.
		gwSunPull = clamp((grav + 1.f) * 0.5f
			+ (inputs[GRAVITY_INPUT].isConnected() ? inputs[GRAVITY_INPUT].getVoltage() * 0.1f : 0.f),
			0.f, 1.f);

		// --- MODE select (knob 0..2 + CV; 0-10V spans the three modes) ---
		int modeRaw = (int) std::round(params[MODE_PARAM].getValue());
		if (inputs[MODE_INPUT].isConnected())
			modeRaw += (int) std::floor(inputs[MODE_INPUT].getVoltage() / (10.f / 3.f) + 0.001f);
		mode = clamp(modeRaw, 0, (int) NUM_MODES - 1);
		if (mode != prevMode) { initMode(mode); prevMode = mode; }

		// Substep so integrators stay stable when SPEED pushes h large.
		int nsub = 1;
		while (h / nsub > 0.004 && nsub < 16) nsub++;
		double hsub = h / nsub;

		gatePtCount = 0;

		// ---- Mode physics: fill trackedX/Y + gate points + display state ----
		if (mode == MODE_PENDULUM) {
			if (dragJoint == DRAG_NONE) {
				for (int s = 0; s < nsub; s++) rk4Step(hsub);
				// Energy servo: hold a kinetic target set by CHAOS.
				double instE = w1 * w1 + w2 * w2;
				energyLP += (instE - energyLP) * 0.0008;
				if (energyLP < 1e-4) energyLP = 1e-4;
				double targetE = 0.3 + 13.7 * chaos;
				double f = std::pow(targetE / energyLP, 0.6 * h);
				f = clampd(f, 0.985, 1.015);
				w1 *= f; w2 *= f;
				w1 = clampd(w1, -60.0, 60.0);
				w2 = clampd(w2, -60.0, 60.0);
				w1Smooth = w1; w2Smooth = w2;
			}
			else {
				double tx = dragTargetX, ty = dragTargetY;
				if (dragJoint == DRAG_TIP) {
					double nt1, nt2;
					ikTip(tx, ty, nt1, nt2);
					double dv1 = wrapPi(nt1 - prevDragTh1) / std::max(h, 1e-6);
					double dv2 = wrapPi(nt2 - prevDragTh2) / std::max(h, 1e-6);
					w1Smooth += (dv1 - w1Smooth) * 0.05;
					w2Smooth += (dv2 - w2Smooth) * 0.05;
					th1 = nt1; th2 = nt2;
					w1 = clampd(w1Smooth, -40.0, 40.0);
					w2 = clampd(w2Smooth, -40.0, 40.0);
					prevDragTh1 = nt1; prevDragTh2 = nt2;
				}
				else {
					double nt1 = std::atan2(tx, ty);
					double dv1 = wrapPi(nt1 - prevDragTh1) / std::max(h, 1e-6);
					w1Smooth += (dv1 - w1Smooth) * 0.05;
					for (int s = 0; s < nsub; s++) {
						rk4Step(hsub);
						th1 = nt1;
						w1 = clampd(w1Smooth, -40.0, 40.0);
					}
					prevDragTh1 = nt1;
				}
			}
			float b1x = L1 * std::sin((float) th1);
			float b1y = L1 * std::cos((float) th1);
			float b2x = b1x + L2 * std::sin((float) th2);
			float b2y = b1y + L2 * std::cos((float) th2);
			dispB1x = b1x; dispB1y = b1y;
			dispB2x = b2x; dispB2y = b2y;
			trackedX = b2x; trackedY = b2y;
			gatePtX[0] = b2x; gatePtY[0] = b2y; gatePtCount = 1;
		}
		else if (mode == MODE_GRAVWELL) {
			stepGravWell(hsub, nsub, chaos);
			trackedX = (float) gwRx; trackedY = (float) gwRy;
			dispB1x = 0.f; dispB1y = 0.f;          // no arm in this mode
			dispB2x = trackedX; dispB2y = trackedY;
			gatePtX[0] = trackedX; gatePtY[0] = trackedY; gatePtCount = 1;
		}
		else { // MODE_BILLIARDS
			stepBilliards(hsub, nsub, chaos);
			trackedX = (float) bzX[0]; trackedY = (float) bzY[0];
			dispB1x = 0.f; dispB1y = 0.f;
			dispB2x = trackedX; dispB2y = trackedY;
			for (int i = 0; i < bzCount && i < MAX_BALLS; i++) {
				gatePtX[i] = (float) bzX[i];
				gatePtY[i] = (float) bzY[i];
			}
			gatePtCount = bzCount;
		}

		// ---- Shared output tail (driven by the tracked point) ----
		float tx = trackedX, ty = trackedY;
		outputs[X_OUTPUT].setVoltage(clamp(tx / REACH * 5.f, -5.f, 5.f));
		outputs[Y_OUTPUT].setVoltage(clamp(-ty / REACH * 5.f, -5.f, 5.f));

		float radius = std::sqrt(tx * tx + ty * ty);        // 0..REACH
		float phi = std::atan2(tx, ty);                     // 0 at straight down

		outputs[RADIUS_OUTPUT].setVoltage(clamp(radius / REACH * 10.f - 5.f, -5.f, 5.f));
		float angFromGrav = wrapPi(phi - (float) gravAngle);
		outputs[ANGLE_OUTPUT].setVoltage(clamp(angFromGrav / (float) M_PI * 5.f, -5.f, 5.f));

		// Sector morph from the tracked point.
		float radV = radius / REACH * 10.f;
		for (int i = 0; i < NUM_SECTORS; i++) {
			float center = (i * 60.f + 30.f) * DEG;
			float dd = std::abs(wrapPi(phi - center)) / (60.f * DEG);
			float wgt = clamp(1.f - dd, 0.f, 1.f);
			float target = wgt * radV;
			sectorOut[i] += (target - sectorOut[i]) * 0.02f;
			outputs[SECTOR_OUTPUT + i].setVoltage(sectorOut[i]);
		}

		// Ray-cross gates: every gate point fires the ray it crosses. Each point
		// keeps its own continuous-angle tracker so crossings never alias.
		for (int p = 0; p < gatePtCount && p < MAX_BALLS; p++) {
			float pphi = std::atan2(gatePtX[p], gatePtY[p]);
			if (!gpInit[p]) {
				gpPrevPhi[p] = pphi;
				gpUnwrapped[p] = pphi;
				gpLastBucket[p] = (int) std::floor(gpUnwrapped[p] / (60.0 * DEG));
				gpInit[p] = true;
			}
			gpUnwrapped[p] += wrapPi(pphi - gpPrevPhi[p]);
			gpPrevPhi[p] = pphi;
			int bucket = (int) std::floor(gpUnwrapped[p] / (60.0 * DEG));
			if (bucket != gpLastBucket[p]) {
				int dir = (bucket > gpLastBucket[p]) ? 1 : -1;
				for (int b = gpLastBucket[p]; b != bucket; b += dir) {
					int boundary = (dir > 0) ? (b + 1) : b;
					int ray = ((boundary % NUM_SECTORS) + NUM_SECTORS) % NUM_SECTORS;
					gateTimer[ray] = gateHoldSec;
					dispGateFlash[ray] = 1.f;
				}
				gpLastBucket[p] = bucket;
			}
		}

		for (int i = 0; i < NUM_SECTORS; i++) {
			if (gateTimer[i] > 0.f) gateTimer[i] -= args.sampleTime;
			outputs[GATE_OUTPUT + i].setVoltage(gateTimer[i] > 0.f ? 10.f : 0.f);
			if (dispGateFlash[i] > 0.f) dispGateFlash[i] -= args.sampleTime * 6.f;
		}
	}

	// ---- Mode (re)initialization ----
	void initMode(int m) {
		// Reset all gate-point trackers so crossings re-seed cleanly.
		for (int p = 0; p < MAX_BALLS; p++) gpInit[p] = false;
		if (m == MODE_GRAVWELL) initGravWell();
		else if (m == MODE_BILLIARDS) initBilliards();
	}

	// ===================== Gravity Well =====================
	void initGravWell() {
		gwPlanetCount = 2 + (int) std::round(params[CHAOS_PARAM].getValue() * 3.f);  // 2..5
		gwPlanetCount = clamp(gwPlanetCount, 2, MAX_PLANETS);
		for (int i = 0; i < MAX_PLANETS; i++) {
			float t = (i + 1) / (float)(MAX_PLANETS + 1);
			gwPlanetRad[i]  = 0.55 + 1.55 * t;                 // wider spread; outers sit further out
			gwPlanetRate[i] = 0.9 / (0.4 + gwPlanetRad[i]);    // inner faster
			// Mass grows with orbital radius: inner planets light, outer planets
			// heavy. The big outer worlds yank the orbit hard on each flyby for
			// chaotic, unpredictable arcs (the spring still prevents escape/capture).
			gwPlanetMass[i] = 0.4 + 2.6 * t + 0.4 * random::uniform();
			gwPlanetAng[i]  = random::uniform() * 2.0 * M_PI;
		}
		// Start in a circular spring orbit: for F=-K*r, circular speed v = r*sqrt(K).
		double r0 = 0.85;
		double K0 = 2.2 + 5.0 * (double) gwSunPull;
		gwRx = r0; gwRy = 0.0;
		double vCirc = r0 * std::sqrt(K0);
		gwVx = 0.0; gwVy = vCirc;
		gwInit = true;
	}

	void stepGravWell(double hsub, int nsub, float chaos) {
		if (!gwInit) initGravWell();
		gwPlanetCount = 2 + (int) std::round(chaos * 3.f);
		gwPlanetCount = clamp(gwPlanetCount, 2, MAX_PLANETS);
		const double G = 0.18;
		// Central force is a SPRING (linear, F = -k*r toward center), not 1/r^2.
		// A spring gives stable bounded ellipses at ANY energy: it can't pull the
		// rocket into the center (force -> 0 there) and can't let it escape (force
		// grows with distance). This sidesteps the knife-edge orbital balance of
		// Newtonian gravity. GRAV sets the spring stiffness (orbit size/speed).
		// Spring stiffness from GRAV. Floor raised so even at GRAV=0 the central
		// pull is strong enough to reclaim the rocket from a heavy planet —
		// prevents planet capture at low grav / high chaos.
		const double K = 2.2 + 5.0 * (double) gwSunPull;   // stiffness from GRAV knob
		const double soft2 = 0.18 * 0.18;
		const double PG = G * 2.2;
		// Per-planet acceleration cap: keeps the sharp flyby kick but BOUNDS the
		// well so a planet can never trap the rocket into a tight bound orbit.
		// Set near the spring's max pull so the spring always wins eventually.
		const double PLANET_ACC_CAP = 9.0;
		for (int s = 0; s < nsub; s++) {
			for (int i = 0; i < gwPlanetCount; i++)
				gwPlanetAng[i] += gwPlanetRate[i] * hsub;
			double ax = -K * gwRx;     // central spring
			double ay = -K * gwRy;
			for (int i = 0; i < gwPlanetCount; i++) {
				double px = gwPlanetRad[i] * std::cos(gwPlanetAng[i]);
				double py = gwPlanetRad[i] * std::sin(gwPlanetAng[i]);
				double dx = px - gwRx, dy = py - gwRy;
				double r2 = dx * dx + dy * dy + soft2;
				double inv = 1.0 / (r2 * std::sqrt(r2));
				double pax = PG * gwPlanetMass[i] * dx * inv;
				double pay = PG * gwPlanetMass[i] * dy * inv;
				// Clamp this planet's acceleration magnitude.
				double pa2 = pax * pax + pay * pay;
				if (pa2 > PLANET_ACC_CAP * PLANET_ACC_CAP) {
					double s2 = PLANET_ACC_CAP / std::sqrt(pa2);
					pax *= s2; pay *= s2;
				}
				ax += pax; ay += pay;
			}
			gwVx += ax * hsub; gwVy += ay * hsub;
			gwRx += gwVx * hsub; gwRy += gwVy * hsub;
			// Safety rim: a violent flyby could in principle overshoot; reflect
			// so the rocket always stays in the visible disc.
			double d2 = gwRx * gwRx + gwRy * gwRy;
			double WALL = REACH * 1.02;
			if (d2 > WALL * WALL) {
				double d = std::sqrt(d2);
				double nx = gwRx / d, ny = gwRy / d;
				double vn = gwVx * nx + gwVy * ny;
				if (vn > 0.0) { gwVx -= 2.0 * vn * nx; gwVy -= 2.0 * vn * ny; }
				gwRx = nx * WALL; gwRy = ny * WALL;
			}
		}
		// Very light energy governor: only nudges back when the orbit drifts far
		// outside a generous band, so heavy-planet flybys are free to throw the
		// orbit around. Without this the perturbations would slowly pump energy.
		double e = gwVx * gwVx + gwVy * gwVy + K * (gwRx * gwRx + gwRy * gwRy);
		double targetE = K * 0.85 * 0.85 * (1.0 + 0.8 * chaos);
		double ratio = e / std::max(targetE, 1e-6);
		// Only correct when >2x or <0.5x the target — leave the chaotic middle alone.
		if (ratio > 2.0 || ratio < 0.5) {
			double f = std::sqrt(targetE / e);
			f = clampd(f, 0.995, 1.005);
			gwVx *= f; gwVy *= f;
			gwRx *= f; gwRy *= f;
		}
	}

	// ===================== Billiards =====================
	void initBilliards() {
		bzCount = 1 + (int) std::round(params[CHAOS_PARAM].getValue() * 8.f);  // cue + 0..8
		bzCount = clamp(bzCount, 1, MAX_BALLS);
		for (int i = 0; i < MAX_BALLS; i++) {
			double a = random::uniform() * 2.0 * M_PI;
			double r = 0.2 + 0.7 * random::uniform();
			bzX[i] = r * std::cos(a);
			bzY[i] = r * std::sin(a);
			bzMass[i] = (i == 0) ? CUE_MASS : 1.0;
			// small random initial velocities; cue gets a bit more
			double va = random::uniform() * 2.0 * M_PI;
			double vs = (i == 0) ? 0.9 : 0.4 * random::uniform();
			bzVx[i] = vs * std::cos(va);
			bzVy[i] = vs * std::sin(va);
		}
		bzInit = true;
	}

	double billiardsKE() {
		double e = 0.0;
		for (int i = 0; i < bzCount; i++)
			e += 0.5 * bzMass[i] * (bzVx[i] * bzVx[i] + bzVy[i] * bzVy[i]);
		return e;
	}

	void stepBilliards(double hsub, int nsub, float chaos) {
		if (!bzInit) initBilliards();
		int want = 1 + (int) std::round(chaos * 8.f);
		want = clamp(want, 1, MAX_BALLS);
		if (want != bzCount) {
			// Grow/shrink the active set without disturbing existing balls.
			if (want > bzCount) {
				for (int i = bzCount; i < want; i++) {
					double a = random::uniform() * 2.0 * M_PI;
					double r = 0.2 + 0.7 * random::uniform();
					bzX[i] = r * std::cos(a); bzY[i] = r * std::sin(a);
					bzMass[i] = 1.0;
					double va = random::uniform() * 2.0 * M_PI, vs = 0.4 * random::uniform();
					bzVx[i] = vs * std::cos(va); bzVy[i] = vs * std::sin(va);
				}
			}
			bzCount = want;
			for (int p = 0; p < MAX_BALLS; p++) gpInit[p] = false;
		}

		// Aiming (slingshot): hold the cue still at its spot while the user drags;
		// release applies an impulse opposite to the drag (handled in onDragEnd).
		double keBefore = billiardsKE();
		double wallR = REACH - BALL_R;

		// Table tilt: GRAV direction adds a constant acceleration so balls pool
		// toward one side of the circle (changes which rays get hit most).
		const double TILT = 1.1;
		double gx = std::sin(gravAngle) * TILT;
		double gy = std::cos(gravAngle) * TILT;

		for (int s = 0; s < nsub; s++) {
			for (int i = 0; i < bzCount; i++) {
				if (i == 0 && bzAiming) continue;        // cue frozen while aiming
				bzVx[i] += gx * hsub;                    // table tilt
				bzVy[i] += gy * hsub;
				bzX[i] += bzVx[i] * hsub;
				bzY[i] += bzVy[i] * hsub;
				// Wall: reflect about the inward normal, clamp inside.
				double d2 = bzX[i] * bzX[i] + bzY[i] * bzY[i];
				if (d2 > wallR * wallR) {
					double d = std::sqrt(d2);
					double nx = bzX[i] / d, ny = bzY[i] / d;
					double vn = bzVx[i] * nx + bzVy[i] * ny;
					if (vn > 0.0) { bzVx[i] -= 2.0 * vn * nx; bzVy[i] -= 2.0 * vn * ny; }
					bzX[i] = nx * wallR; bzY[i] = ny * wallR;
				}
			}
			// Ball-ball elastic collisions (equal-radius, mass-weighted).
			for (int i = 0; i < bzCount; i++) {
				for (int j = i + 1; j < bzCount; j++) {
					double dx = bzX[j] - bzX[i], dy = bzY[j] - bzY[i];
					double d2 = dx * dx + dy * dy;
					double minD = 2.0 * BALL_R;
					if (d2 > 1e-12 && d2 < minD * minD) {
						double d = std::sqrt(d2);
						double nx = dx / d, ny = dy / d;
						// separate overlap proportionally to inverse mass
						double overlap = minD - d;
						double im1 = 1.0 / bzMass[i], im2 = 1.0 / bzMass[j];
						double imsum = im1 + im2;
						bzX[i] -= nx * overlap * (im1 / imsum);
						bzY[i] -= ny * overlap * (im1 / imsum);
						bzX[j] += nx * overlap * (im2 / imsum);
						bzY[j] += ny * overlap * (im2 / imsum);
						// relative velocity along normal
						double rvx = bzVx[j] - bzVx[i], rvy = bzVy[j] - bzVy[i];
						double vn = rvx * nx + rvy * ny;
						if (vn < 0.0) {
							double imp = -2.0 * vn / imsum;   // elastic
							bzVx[i] -= imp * im1 * nx; bzVy[i] -= imp * im1 * ny;
							bzVx[j] += imp * im2 * nx; bzVy[j] += imp * im2 * ny;
						}
					}
				}
			}
		}

		// KE normalization: elastic collisions conserve energy; this corrects
		// integrator drift so motion neither dies nor blows up. Skip while aiming.
		if (!bzAiming) {
			double keAfter = billiardsKE();
			if (keAfter > 1e-6 && keBefore > 1e-6) {
				double f = std::sqrt(keBefore / keAfter);
				f = clampd(f, 0.98, 1.02);
				for (int i = 0; i < bzCount; i++) { bzVx[i] *= f; bzVy[i] *= f; }
			}
		}
	}

	// Apply the slingshot launch (called from the display on drag release).
	void billiardsLaunch(double dragX, double dragY) {
		// Launch opposite the drag, power proportional to drag distance.
		double dx = bzX[0] - dragX, dy = bzY[0] - dragY;
		double power = std::sqrt(dx * dx + dy * dy) * 2.5;
		double d = std::sqrt(dx * dx + dy * dy);
		if (d > 1e-6) { bzVx[0] = dx / d * power; bzVy[0] = dy / d * power; }
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "th1", json_real(th1));
		json_object_set_new(root, "th2", json_real(th2));
		json_object_set_new(root, "w1", json_real(w1));
		json_object_set_new(root, "w2", json_real(w2));
		json_object_set_new(root, "gateHoldSec", json_real(gateHoldSec));
		json_object_set_new(root, "trailLength", json_integer(trailLength));
		json_object_set_new(root, "mode", json_integer(mode));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "th1")) th1 = json_number_value(j);
		if (json_t* j = json_object_get(root, "th2")) th2 = json_number_value(j);
		if (json_t* j = json_object_get(root, "w1")) w1 = json_number_value(j);
		if (json_t* j = json_object_get(root, "w2")) w2 = json_number_value(j);
		if (json_t* j = json_object_get(root, "gateHoldSec")) gateHoldSec = json_number_value(j);
		if (json_t* j = json_object_get(root, "trailLength")) trailLength = (int) json_integer_value(j);
		if (json_t* j = json_object_get(root, "mode")) {
			mode = clamp((int) json_integer_value(j), 0, (int) NUM_MODES - 1);
			prevMode = mode;
			initMode(mode);
		}
	}
};


// ---------------------------------------------------------------------------
// Display: circular field, hex overlay, two tumbling arms, fading tip + elbow
// trails, gate-ray flashes, sector morph readout. Ragdoll drag on either bob.
// ---------------------------------------------------------------------------
struct SwingDisplay : OpaqueWidget {
	Swing* module = nullptr;
	std::shared_ptr<Font> font;
	// Trails are UI-only: sampled once per frame, never persisted.
	std::vector<Vec> tipTrail, elbowTrail;

	// Draw the rocket centered at (cx,cy), nose pointed along headingRad
	// (screen angle, y-down), scaled so its long axis ~= sizePx. The shape is
	// the res/rocket.svg path baked directly into nanovg beziers (viewBox
	// 28.8 x 50.84, art nose pointing toward -y), so there's no file to load.
	void drawRocket(NVGcontext* vg, float cx, float cy, float headingRad, float sizePx) {
		const float W = 28.8f, H = 50.84f;
		float scale = sizePx / H;
		nvgSave(vg);
		nvgTranslate(vg, cx, cy);
		nvgRotate(vg, headingRad + (float) M_PI / 2.f);  // nose (-y) -> heading
		nvgScale(vg, scale, scale);
		nvgTranslate(vg, -W * 0.5f, -H * 0.5f);
		nvgBeginPath(vg);
		nvgMoveTo(vg, 21.26f, 35.34f);
		nvgBezierTo(vg, 21.48f, 33.28f, 21.60f, 31.08f, 21.60f, 28.80f);
		nvgBezierTo(vg, 21.60f, 16.87f, 16.80f,  0.00f, 14.40f,  0.00f);
		nvgBezierTo(vg, 12.00f,  0.00f,  7.20f, 16.87f,  7.20f, 28.80f);
		nvgBezierTo(vg,  7.20f, 31.08f,  7.32f, 33.27f,  7.54f, 35.34f);
		nvgBezierTo(vg,  3.05f, 37.78f,  0.00f, 42.53f,  0.00f, 48.00f);
		nvgBezierTo(vg,  0.00f, 48.69f,  0.05f, 49.37f,  0.14f, 50.03f);
		nvgBezierTo(vg,  0.26f, 50.88f,  1.38f, 51.14f,  1.87f, 50.44f);
		nvgBezierTo(vg,  3.86f, 47.57f,  6.43f, 45.38f,  9.35f, 44.20f);
		nvgBezierTo(vg,  9.60f, 48.00f,  9.60f, 45.60f, 14.40f, 45.60f);
		nvgBezierTo(vg, 19.20f, 45.60f, 19.20f, 48.00f, 19.45f, 44.20f);
		nvgBezierTo(vg, 22.37f, 45.39f, 24.94f, 47.57f, 26.93f, 50.44f);
		nvgBezierTo(vg, 27.42f, 51.14f, 28.54f, 50.88f, 28.66f, 50.03f);
		nvgBezierTo(vg, 28.75f, 49.37f, 28.80f, 48.69f, 28.80f, 48.00f);
		nvgBezierTo(vg, 28.80f, 42.53f, 25.75f, 37.78f, 21.26f, 35.34f);
		nvgClosePath(vg);
		nvgFillColor(vg, nvgRGBA(0xEC, 0x65, 0x2E, 0xFF));
		nvgFill(vg);
		nvgRestore(vg);
	}

	static const int DRAG_GRAV = 99;   // dragging the gravity marker on the rim
	int dragging = Swing::DRAG_NONE;
	Vec dragPos;   // widget-local cursor, kept current through the drag

	// Set the GRAVITY param so the centre of gravity points at the cursor.
	void setGravFromPos(Vec p) {
		float mx, my; toModel(p, mx, my);
		float ang = std::atan2(mx, my);              // 0 = down, matches field
		if (module) module->params[Swing::GRAVITY_PARAM].setValue(clamp(ang / (float) M_PI, -1.f, 1.f));
	}

	float radiusPx() { return std::min(box.size.x, box.size.y) * 0.5f - 6.f; }
	Vec centerPx()   { return Vec(box.size.x * 0.5f, box.size.y * 0.5f); }
	float ppu()      { return radiusPx() / REACH; }   // pixels per pendulum unit

	// pendulum coords (+y down) -> widget pixels
	Vec toPx(float x, float y) {
		Vec c = centerPx();
		return Vec(c.x + x * ppu(), c.y + y * ppu());
	}
	// widget pixels -> pendulum coords
	void toModel(Vec p, float& x, float& y) {
		Vec c = centerPx();
		x = (p.x - c.x) / ppu();
		y = (p.y - c.y) / ppu();
	}

	static const int DRAG_CUE = 98;   // billiards slingshot aim

	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			dragPos = e.pos;

			// Billiards: click near the cue starts a slingshot aim.
			if (module->mode == Swing::MODE_BILLIARDS) {
				Vec cue = toPx(module->trackedX, module->trackedY);
				if (e.pos.minus(cue).norm() < 14.f) {
					dragging = DRAG_CUE;
					module->bzAiming = true;
					float mx, my; toModel(e.pos, mx, my);
					module->bzAimX = mx; module->bzAimY = my;
					e.consume(this);
					return;
				}
			}

			// Pendulum: grab a joint (ragdoll).
			if (module->mode == Swing::MODE_PENDULUM) {
				Vec tip = toPx(module->dispB2x, module->dispB2y);
				Vec elbow = toPx(module->dispB1x, module->dispB1y);
				float dTip = e.pos.minus(tip).norm();
				float dElbow = e.pos.minus(elbow).norm();
				float thresh = 11.f;
				int joint = Swing::DRAG_NONE;
				if (dTip <= dElbow && dTip < thresh) joint = Swing::DRAG_TIP;
				else if (dElbow < thresh) joint = Swing::DRAG_ELBOW;
				if (joint != Swing::DRAG_NONE) {
					dragging = joint;
					float mx, my; toModel(e.pos, mx, my);
					module->dragTargetX = mx; module->dragTargetY = my;
					module->prevDragTh1 = module->th1;
					module->prevDragTh2 = module->th2;
					module->w1Smooth = 0.0; module->w2Smooth = 0.0;
					module->dragJoint = joint;
					e.consume(this);
					return;
				}
			}

			// Outer rim (all modes): grab the gravity marker and spin gravity.
			if (e.pos.minus(centerPx()).norm() > radiusPx() * 0.7f) {
				dragging = DRAG_GRAV;
				setGravFromPos(e.pos);
				e.consume(this);
				return;
			}
		}
		OpaqueWidget::onButton(e);
	}

	void onDragStart(const DragStartEvent& e) override { OpaqueWidget::onDragStart(e); }

	void onDragMove(const DragMoveEvent& e) override {
		if (module && dragging != Swing::DRAG_NONE) {
			float zoom = getAbsoluteZoom();
			if (zoom <= 0.f) zoom = 1.f;
			dragPos = dragPos.plus(e.mouseDelta.div(zoom));
			if (dragging == DRAG_GRAV) {
				setGravFromPos(dragPos);
			} else if (dragging == DRAG_CUE) {
				float mx, my; toModel(dragPos, mx, my);
				module->bzAimX = mx; module->bzAimY = my;
			} else {
				float mx, my; toModel(dragPos, mx, my);
				module->dragTargetX = mx; module->dragTargetY = my;
			}
		}
		OpaqueWidget::onDragMove(e);
	}

	void onDragEnd(const DragEndEvent& e) override {
		if (module && dragging != Swing::DRAG_NONE) {
			if (dragging == DRAG_CUE) {
				module->billiardsLaunch(module->bzAimX, module->bzAimY);
				module->bzAiming = false;
			} else if (dragging != DRAG_GRAV) {
				module->dragJoint = Swing::DRAG_NONE;   // release -> let it fly
			}
			dragging = Swing::DRAG_NONE;
		}
		OpaqueWidget::onDragEnd(e);
	}

	void pushTrail() {
		if (!module) return;
		int len = module->trailLength;
		if (len <= 0) { tipTrail.clear(); elbowTrail.clear(); return; }
		tipTrail.push_back(toPx(module->dispB2x, module->dispB2y));
		elbowTrail.push_back(toPx(module->dispB1x, module->dispB1y));
		while ((int) tipTrail.size() > len) tipTrail.erase(tipTrail.begin());
		while ((int) elbowTrail.size() > len) elbowTrail.erase(elbowTrail.begin());
	}

	void drawTrail(NVGcontext* vg, std::vector<Vec>& trail, NVGcolor col) {
		int n = (int) trail.size();
		for (int i = 1; i < n; i++) {
			float a = (float) i / n;          // newer = brighter
			nvgBeginPath(vg);
			nvgMoveTo(vg, trail[i - 1].x, trail[i - 1].y);
			nvgLineTo(vg, trail[i].x, trail[i].y);
			nvgStrokeColor(vg, nvgRGBAf(col.r, col.g, col.b, a * a * 0.7f));
			nvgStrokeWidth(vg, 1.2f);
			nvgStroke(vg);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!module) {
			// Browser-preview: pendulum mid-swing, two sector arcs lit, trails
			// faked as straight segments.
			NVGcontext* vg = args.vg;
			const NVGcolor COL_BLUE   = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
			const NVGcolor COL_ORANGE = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
			const NVGcolor COL_GRID   = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);
			const NVGcolor COL_TIP    = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
			const NVGcolor COL_ELBOW  = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
			Vec c = centerPx();
			float R = radiusPx();
			// Outer circle
			nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, R);
			nvgStrokeColor(vg, COL_GRID); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			// Six rays
			for (int k = 0; k < NUM_SECTORS; k++) {
				float a = k * 60.f * DEG;
				float ex = c.x + std::sin(a) * R;
				float ey = c.y + std::cos(a) * R;
				nvgBeginPath(vg); nvgMoveTo(vg, c.x, c.y); nvgLineTo(vg, ex, ey);
				nvgStrokeColor(vg, COL_GRID); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			}
			// Two sector arcs lit
			const float sectorLevels[NUM_SECTORS] = {0.f, 0.8f, 0.f, 0.f, 0.5f, 0.f};
			for (int i = 0; i < NUM_SECTORS; i++) {
				if (sectorLevels[i] < 0.01f) continue;
				float a0 = (i * 60.f) * DEG, a1 = ((i + 1) * 60.f) * DEG;
				nvgBeginPath(vg);
				int seg = 10;
				for (int s = 0; s <= seg; s++) {
					float a = a0 + (a1 - a0) * s / seg;
					float rr = R - 3.f;
					float px = c.x + std::sin(a) * rr;
					float py = c.y + std::cos(a) * rr;
					if (s == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
				}
				nvgStrokeColor(vg, nvgRGBAf(COL_BLUE.r, COL_BLUE.g, COL_BLUE.b, sectorLevels[i]));
				nvgStrokeWidth(vg, 2.5f); nvgStroke(vg);
			}
			// Pendulum: pose at angles theta1=+45°, theta2=-30° (looks swung)
			float th1 = 0.78f, th2 = -0.52f;
			float ppuV = ppu();
			float b1x = c.x + std::sin(th1) * L1 * ppuV;
			float b1y = c.y + std::cos(th1) * L1 * ppuV;
			float b2x = b1x + std::sin(th1 + th2) * L2 * ppuV;
			float b2y = b1y + std::cos(th1 + th2) * L2 * ppuV;
			// Trail fakes
			for (int i = 0; i < 12; i++) {
				float t = i / 11.f;
				float a = t * t * 0.6f;
				float tip_t1 = th1 - (1.f - t) * 0.6f;
				float tip_t2 = th2 - (1.f - t) * 0.4f;
				float tipx = c.x + std::sin(tip_t1) * L1 * ppuV
					+ std::sin(tip_t1 + tip_t2) * L2 * ppuV;
				float tipy = c.y + std::cos(tip_t1) * L1 * ppuV
					+ std::cos(tip_t1 + tip_t2) * L2 * ppuV;
				if (i > 0) {
					nvgBeginPath(vg); nvgMoveTo(vg, c.x, c.y);
					float prev_t1 = th1 - (1.f - (i-1)/11.f) * 0.6f;
					float prev_t2 = th2 - (1.f - (i-1)/11.f) * 0.4f;
					float ptipx = c.x + std::sin(prev_t1) * L1 * ppuV
						+ std::sin(prev_t1 + prev_t2) * L2 * ppuV;
					float ptipy = c.y + std::cos(prev_t1) * L1 * ppuV
						+ std::cos(prev_t1 + prev_t2) * L2 * ppuV;
					nvgBeginPath(vg); nvgMoveTo(vg, ptipx, ptipy); nvgLineTo(vg, tipx, tipy);
					nvgStrokeColor(vg, nvgRGBAf(COL_TIP.r, COL_TIP.g, COL_TIP.b, a * 0.7f));
					nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
				}
			}
			// Arms
			nvgBeginPath(vg);
			nvgMoveTo(vg, c.x, c.y); nvgLineTo(vg, b1x, b1y); nvgLineTo(vg, b2x, b2y);
			nvgStrokeColor(vg, nvgRGBA(0xC0, 0xC8, 0xD0, 0xFF));
			nvgStrokeWidth(vg, 2.f); nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
			// Bobs
			nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, 2.5f);
			nvgFillColor(vg, COL_GRID); nvgFill(vg);
			nvgBeginPath(vg); nvgCircle(vg, b1x, b1y, 4.f);
			nvgFillColor(vg, COL_ELBOW); nvgFill(vg);
			nvgBeginPath(vg); nvgCircle(vg, b2x, b2y, 5.f);
			nvgFillColor(vg, COL_TIP); nvgFill(vg);
			return;
		}
		NVGcontext* vg = args.vg;

		const NVGcolor COL_BLUE   = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
		const NVGcolor COL_ORANGE = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
		const NVGcolor COL_GRID   = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);
		const NVGcolor COL_TIP    = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
		const NVGcolor COL_ELBOW  = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);

		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));

		Vec c = centerPx();
		float R = radiusPx();

		// Outer circle
		nvgBeginPath(vg);
		nvgCircle(vg, c.x, c.y, R);
		nvgStrokeColor(vg, COL_GRID);
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);

		// Six boundary rays + gate flash highlight (fixed to the panel jacks)
		for (int k = 0; k < NUM_SECTORS; k++) {
			float a = k * 60.f * DEG;
			float ex = c.x + std::sin(a) * R;
			float ey = c.y + std::cos(a) * R;
			float flash = clamp(module->dispGateFlash[k], 0.f, 1.f);
			nvgBeginPath(vg);
			nvgMoveTo(vg, c.x, c.y);
			nvgLineTo(vg, ex, ey);
			NVGcolor rc = nvgRGBAf(
				COL_GRID.r + (COL_ORANGE.r - COL_GRID.r) * flash,
				COL_GRID.g + (COL_ORANGE.g - COL_GRID.g) * flash,
				COL_GRID.b + (COL_ORANGE.b - COL_GRID.b) * flash,
				1.f);
			nvgStrokeColor(vg, rc);
			nvgStrokeWidth(vg, 1.f + flash * 1.5f);
			nvgStroke(vg);
		}

		// Sector morph arcs near the rim — brightness = that sector's CV level
		for (int i = 0; i < NUM_SECTORS; i++) {
			float lvl = clamp(module->sectorOut[i] / 10.f, 0.f, 1.f);
			if (lvl < 0.01f) continue;
			float a0 = (i * 60.f) * DEG;
			float a1 = ((i + 1) * 60.f) * DEG;
			nvgBeginPath(vg);
			int seg = 10;
			for (int s = 0; s <= seg; s++) {
				float a = a0 + (a1 - a0) * s / seg;
				float rr = R - 3.f;
				float px = c.x + std::sin(a) * rr;
				float py = c.y + std::cos(a) * rr;
				if (s == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
			}
			nvgStrokeColor(vg, nvgRGBAf(COL_BLUE.r, COL_BLUE.g, COL_BLUE.b, lvl));
			nvgStrokeWidth(vg, 2.5f);
			nvgStroke(vg);
		}

		// Index numbers tying the field to the radial jacks: gate id on each ray
		// (near the rim), sector id at each wedge centre.
		if (font && font->handle >= 0) {
			nvgFontFaceId(vg, font->handle);
			nvgFontSize(vg, 9.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			for (int k = 0; k < NUM_SECTORS; k++) {
				float ag = k * 60.f * DEG;
				float gx = c.x + std::sin(ag) * (R - 11.f);
				float gy = c.y + std::cos(ag) * (R - 11.f);
				nvgFillColor(vg, nvgRGBA(0x55, 0x55, 0x72, 0xFF));
				nvgText(vg, gx, gy, string::f("%d", k + 1).c_str(), NULL);

				float as = (k * 60.f + 30.f) * DEG;
				float sx = c.x + std::sin(as) * (R * 0.62f);
				float sy = c.y + std::cos(as) * (R * 0.62f);
				nvgFillColor(vg, nvgRGBA(0x3a, 0x6a, 0x8a, 0xFF));
				nvgText(vg, sx, sy, string::f("%d", k + 1).c_str(), NULL);
			}
		}

		// Gravity marker. In Gravity Well it's the central SUN, a filled circle
		// at the origin sized by the pull amount. In the other modes it's a rim
		// dot showing the direction gravity pulls.
		{
			NVGcolor gold = nvgRGBA(0xD0, 0xA8, 0x50, 0xFF);
			if (module->mode == Swing::MODE_GRAVWELL) {
				float sunR = 4.f + module->gwSunPull * 12.f;   // 4..16 px by pull
				// soft halo
				nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, sunR + 3.f);
				nvgFillColor(vg, nvgRGBAf(gold.r, gold.g, gold.b, 0.18f)); nvgFill(vg);
				nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, sunR);
				nvgFillColor(vg, gold); nvgFill(vg);
			} else {
				float g = (float) module->gravAngle;
				float mx = c.x + std::sin(g) * R;
				float my = c.y + std::cos(g) * R;
				float ix = c.x + std::sin(g) * (R - 9.f);
				float iy = c.y + std::cos(g) * (R - 9.f);
				nvgBeginPath(vg);
				nvgMoveTo(vg, ix, iy); nvgLineTo(vg, mx, my);
				nvgStrokeColor(vg, nvgRGBAf(gold.r, gold.g, gold.b, 0.5f));
				nvgStrokeWidth(vg, 1.2f);
				nvgStroke(vg);
				nvgBeginPath(vg); nvgCircle(vg, mx, my, 3.2f);
				nvgFillColor(vg, gold); nvgFill(vg);
			}
		}

		// Trails
		pushTrail();
		drawTrail(vg, elbowTrail, COL_ELBOW);
		drawTrail(vg, tipTrail, COL_TIP);

		// --- Per-mode body rendering ---
		if (module->mode == Swing::MODE_PENDULUM) {
			Vec b1 = toPx(module->dispB1x, module->dispB1y);
			Vec b2 = toPx(module->dispB2x, module->dispB2y);
			nvgBeginPath(vg);
			nvgMoveTo(vg, c.x, c.y);
			nvgLineTo(vg, b1.x, b1.y);
			nvgLineTo(vg, b2.x, b2.y);
			nvgStrokeColor(vg, nvgRGBA(0xC0, 0xC8, 0xD0, 0xFF));
			nvgStrokeWidth(vg, 2.f);
			nvgLineCap(vg, NVG_ROUND);
			nvgStroke(vg);
			nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, 2.5f);
			nvgFillColor(vg, COL_GRID); nvgFill(vg);
			nvgBeginPath(vg); nvgCircle(vg, b1.x, b1.y, 4.f);
			nvgFillColor(vg, COL_ELBOW); nvgFill(vg);
			nvgBeginPath(vg); nvgCircle(vg, b2.x, b2.y, 5.f);
			nvgFillColor(vg, COL_TIP); nvgFill(vg);
		}
		else if (module->mode == Swing::MODE_GRAVWELL) {
			// Planets (gold) on their orbits, rocket = tracked tip (orange).
			NVGcolor gold = nvgRGBA(0xD0, 0xA8, 0x50, 0xFF);
			for (int i = 0; i < module->gwPlanetCount; i++) {
				float px = (float)(module->gwPlanetRad[i] * std::cos(module->gwPlanetAng[i]));
				float py = (float)(module->gwPlanetRad[i] * std::sin(module->gwPlanetAng[i]));
				Vec p = toPx(px, py);
				float rad = 2.5f + 3.5f * (float) module->gwPlanetMass[i];
				// faint orbit ring
				nvgBeginPath(vg);
				nvgCircle(vg, c.x, c.y, (float)(module->gwPlanetRad[i]) * ppu());
				nvgStrokeColor(vg, nvgRGBAf(gold.r, gold.g, gold.b, 0.12f));
				nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
				nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, rad);
				nvgFillColor(vg, gold); nvgFill(vg);
			}
			Vec r = toPx(module->trackedX, module->trackedY);
			// Orange rocket, nose pointed along its velocity heading.
			float hx = (float) module->gwVx, hy = (float) module->gwVy;
			float heading = (std::fabs(hx) + std::fabs(hy) > 1e-6f)
				? std::atan2(hy, hx) : -(float) M_PI / 2.f;
			drawRocket(vg, r.x, r.y, heading, 16.f);
		}
		else { // MODE_BILLIARDS
			for (int i = 0; i < module->bzCount && i < Swing::MAX_BALLS; i++) {
				Vec p = toPx((float) module->bzX[i], (float) module->bzY[i]);
				bool cue = (i == 0);
				float rad = (float)(Swing::BALL_R * ppu()) * (cue ? 1.15f : 1.f);
				nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, rad);
				nvgFillColor(vg, cue ? COL_TIP : COL_ELBOW); nvgFill(vg);
			}
			// Slingshot aim line while dragging the cue.
			if (module->bzAiming) {
				Vec cue = toPx(module->trackedX, module->trackedY);
				Vec aim = toPx((float) module->bzAimX, (float) module->bzAimY);
				nvgBeginPath(vg);
				nvgMoveTo(vg, cue.x, cue.y); nvgLineTo(vg, aim.x, aim.y);
				nvgStrokeColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.6f));
				nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
				// launch direction (opposite the drag), dashed-ish preview
				Vec opp(2.f * cue.x - aim.x, 2.f * cue.y - aim.y);
				nvgBeginPath(vg);
				nvgMoveTo(vg, cue.x, cue.y); nvgLineTo(vg, opp.x, opp.y);
				nvgStrokeColor(vg, nvgRGBAf(COL_TIP.r, COL_TIP.g, COL_TIP.b, 0.5f));
				nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			}
		}

		OpaqueWidget::drawLayer(args, layer);
	}
};


// Panel labels drawn in code — VCV's SVG renderer ignores <text>, so SFS panels
// carry their labels as the display widget / nanovg text rather than in the SVG.
struct SwingLabels : Widget {
	std::shared_ptr<Font> font;

	void label(NVGcontext* vg, float mmx, float mmy, const char* s, float px, int align) {
		Vec p = mm2px(Vec(mmx, mmy));
		nvgFontFaceId(vg, font->handle);
		nvgFontSize(vg, px);
		nvgTextAlign(vg, align);
		nvgText(vg, p.x, p.y, s, NULL);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font || font->handle < 0) return;

		nvgFillColor(vg, nvgRGB(0x23, 0x1f, 0x20));
		int C = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE;

		// Left control column (pot label sits just above each pot at x=11)
		label(vg, 11.f, 10.f, "SPEED", 6.f, C);
		label(vg, 11.f, 36.f, "CHAOS", 6.f, C);
		label(vg, 11.f, 62.f, "GRAV",  6.f, C);
		label(vg, 11.f, 88.f, "MODE",  6.f, C);

		// Right output column (label above each jack at x=141)
		label(vg, 141.f, 14.f, "X",     6.f, C);
		label(vg, 141.f, 40.f, "Y",     6.f, C);
		label(vg, 141.f, 66.f, "RAD",   6.f, C);
		label(vg, 141.f, 92.f, "ANG",   6.f, C);

		// Ring legend (inner = gates, outer = sector CVs)
		nvgFillColor(vg, nvgRGB(0x60, 0x60, 0x72));
		label(vg, 76.f, 12.5f, "INNER GATE  ·  OUTER SECT", 5.2f, C);

		// Wordmark
		nvgFillColor(vg, nvgRGB(0x23, 0x1f, 0x20));
		label(vg, 76.f, 119.f, "SWING", 12.f, C);

		Widget::draw(args);
	}
};


struct SwingWidget : ModuleWidget {
	SwingWidget(Swing* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/swing.svg")));

		SwingLabels* labels = new SwingLabels();
		labels->box.pos = Vec(0, 0);
		labels->box.size = box.size;
		addChild(labels);

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Centred circular display (centred at 76,64 on the 30HP panel)
		const float CX = 76.f, CY = 64.f;
		const float Rg = 38.f, Rs = 49.f;   // gate ring, sector ring radii
		SwingDisplay* display = new SwingDisplay();
		display->module = module;
		display->box.pos = mm2px(Vec(CX - 30.f, CY - 30.f));
		display->box.size = mm2px(Vec(60.f, 60.f));
		addChild(display);

		// Radial jack mandala: gates on the inner ring at each ray angle, sector
		// CVs on the outer ring at each wedge centre — aligned with the field.
		for (int k = 0; k < NUM_SECTORS; k++) {
			float ag = k * 60.f * DEG;
			float as = (k * 60.f + 30.f) * DEG;
			Vec gp(CX + std::sin(ag) * Rg, CY + std::cos(ag) * Rg);
			Vec sp(CX + std::sin(as) * Rs, CY + std::cos(as) * Rs);
			addOutput(createOutputCentered<PJ301MPort>(mm2px(gp), module, Swing::GATE_OUTPUT + k));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(sp), module, Swing::SECTOR_OUTPUT + k));
		}

		// Left column: 4 pot + CV-jack pairs (SPEED / CHAOS / GRAV / MODE).
		const float colL = 11.f;
		struct LCtl { int param; int input; float py; float jy; };
		const LCtl lc[4] = {
			{Swing::SPEED_PARAM,   Swing::SPEED_INPUT,   16.f, 27.f},
			{Swing::CHAOS_PARAM,   Swing::CHAOS_INPUT,   42.f, 53.f},
			{Swing::GRAVITY_PARAM, Swing::GRAVITY_INPUT, 68.f, 79.f},
			{Swing::MODE_PARAM,    Swing::MODE_INPUT,    94.f, 105.f},
		};
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(colL, lc[i].py)), module, lc[i].param));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colL, lc[i].jy)), module, lc[i].input));
		}

		// Right column: 4 output jacks (X / Y / RADIUS / ANGLE).
		const float colR = 141.f;
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colR, 20.f)), module, Swing::X_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colR, 46.f)), module, Swing::Y_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colR, 72.f)), module, Swing::RADIUS_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colR, 98.f)), module, Swing::ANGLE_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Swing* module = dynamic_cast<Swing*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Relaunch (kick)", "", [=]() { module->relaunch(); }));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Gate hold"));
		struct HoldOpt { const char* name; float sec; };
		static const HoldOpt holds[3] = {{"Tight (30ms)", 0.03f}, {"Medium (60ms)", 0.06f}, {"Gluey (120ms)", 0.12f}};
		for (int i = 0; i < 3; i++) {
			float sec = holds[i].sec;
			menu->addChild(createCheckMenuItem(holds[i].name, "",
				[=]() { return std::abs(module->gateHoldSec - sec) < 1e-4f; },
				[=]() { module->gateHoldSec = sec; }));
		}

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Trail length"));
		struct TrailOpt { const char* name; int len; };
		static const TrailOpt trails[4] = {{"Off", 0}, {"Short", 40}, {"Medium", 90}, {"Long", 180}};
		for (int i = 0; i < 4; i++) {
			int len = trails[i].len;
			menu->addChild(createCheckMenuItem(trails[i].name, "",
				[=]() { return module->trailLength == len; },
				[=]() { module->trailLength = len; }));
		}
	}
};


Model* modelSwing = createModel<Swing, SwingWidget>("Swing");

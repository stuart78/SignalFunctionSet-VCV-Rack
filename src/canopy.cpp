// CANOPY -- drops falling through leaves onto a steel tongue drum. See docs/canopy-design.md.
//
// The drops are real objects: spheres under gravity in a 3D volume, bouncing off
// leaves the user places and off the drum's domed top. Every landing on the drum
// makes a sound, because the body carries it: a tongue under the drop rings
// fully, the ones near it less, and the shell knocks. How it sounds comes from the
// hit: how fast it was falling, what it is made of, how big it is, and where it
// landed. The screen draws what the physics is doing and has no state of its own
// beyond the camera, so nothing drawn can disagree with what is heard.
//
// The mode tables are ESTIMATES. Little has been published on measured tongue drum
// partials; CP_RATIO starts from one uncited set and is there to be tuned by ear.
#include "plugin.hpp"
#include "root-cv.hpp"
#include "fastmath.hpp"
#include "panel-style.hpp"
#include "preview.hpp"
#include "scale-bus.hpp"
#include "scales.hpp"
#include "waveguide.hpp"   // sfs::softClip
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

static const int CP_MAXN = 32;             // voices: the tenor pan has 29 notes
static const int CP_MAXT = 16;             // tongues and strings, at most
static const int CP_MODES = 6;             // per tongue
static const int CP_CONTACTS = 3;          // hits in progress on one tongue
static const int CP_SHELL_CONTACTS = 6;
static const int CP_MAXDROPS = 128;
static const int CP_MAXLEAVES = 12;
static const int CP_SHAPE_N = 33;          // points along a tongue in the shape table

// The world, in drum radii: the top is a shallow dome over the disc of radius 1,
// and drops start at y = CP_H. Gravity is chosen for the look of the fall, about
// 0.8 s from the top, and the physics runs at a fixed 2 kHz whatever the sample rate.
static const float CP_H = 2.6f;
static const float CP_G = 8.f;
static const float CP_DT = 1.f / 2000.f;
static const float CP_DOME = 0.11f;        // height of the dome's middle above its rim
static const float CP_V0 = 6.f;            // a fall from the top, about: the reference speed
static const float CP_R0 = 0.06f;          // the reference drop radius
static const float CP_WATER_MIN = 0.024f;  // water smaller than this stays whole
static const float CP_MIN_STRIKE = 0.15f;  // slower than this and a hit makes no sound
static const int   CP_FORM_TAB = 512;      // points round a leaf's outline
static const float CP_LEAF_TH = 0.012f;    // half-thickness
static const float CP_LEAF_I = 1.5f;       // how little a hit bends a leaf
static const float CP_LEAF_BEND = 0.5f;    // radians, at most
// A draught toward the middle of the column: it bends a thrown drop back over the
// drum, where a wall would make it bounce. Nothing in the column is pushed until
// it is past CP_DRAUGHT_R.
static const float CP_DRAUGHT_R = 0.86f;
static const float CP_DRAUGHT_K = 30.f;

static inline float cpDomeY(float x, float z) { return CP_DOME * (1.f - x * x - z * z); }

// ── the zither (2026-11) ──────────────────────────────────────────────────────
// A board under graded strings that run toward the viewer, low and long at the
// left, high and short at the right. The units are the drum's (its radius is 1).
// The board leans toward the viewer, so a drop resting on two strings rolls down
// them and falls off the front.
static const int   CP_NINST = 9;            // instruments TYPE chooses between
static const int   CP_ZMODES = 12;           // harmonics per string
static const int   CP_VMODES = 12;           // the most modes any instrument's voice has
static const float CP_Z_SP = 0.1f;           // string spacing: a drop wider than this rests on two
static const float CP_Z_BACK = -0.78f;       // the back bridge
static const float CP_Z_FRONT0 = 0.80f;      // the front bridge, at the lowest string
static const float CP_Z_FRONT1 = 0.30f;      // and at the highest
static const float CP_Z_H = 0.05f;           // the strings' height above the board
static const float CP_Z_R = 0.006f;          // a string's radius
static const float CP_Z_SLOPE = 0.12f;       // the board falls this much per unit toward the viewer
static const float CP_Z_EDGE = 0.10f;        // board beyond the outer strings and the front bridge
static const float CP_Z_B = 0.00015f;        // stiffness: partial n is n * sqrt(1 + B n^2) times the fundamental
static const float CP_Z_T60 = 6.f;           // seconds, the fundamental of a string at 220 Hz
static const float CP_Z_DAMP = 0.0015f;      // per sample at 48 kHz: what a resting drop of unit mass removes at an antinode
// Rolling sustains a string rather than hissing on it: each harmonic is pushed
// along in step with its own motion, so it grows smoothly until the damping of
// the drop and the string's own loss hold it, the way an EBow sustains a string.
// A trace of noise only gets a silent harmonic moving.
static const float CP_Z_PUMP = 2e-4f;        // how hard rolling sustains a string
static const float CP_Z_SEED = 5e-4f;        // the noise that starts it
static inline float cpBoardY(float z) { return -CP_Z_SLOPE * z; }

// ── the steel pan (2026-11) ──────────────────────────────────────────────────
// A low-C tenor pan, C4 to E6. The playing surface is a bowl sunk into the top
// of the drum, hung tilted a little toward the player, so drops roll down across
// the notes to the low point. Each note is a pad hammered into the bowl and
// tuned so its first three partials are the fundamental, the octave and the
// twelfth; the partials above them are not tuned. A hit near a pad's edge brings
// the upper partials out, as on a real pan. These ratios, levels and ring times
// are estimates, not measurements of one instrument.
static const int   CP_PMODES = 6;
static const float CP_PAN_RATIO[CP_PMODES] = {1.f, 2.f, 3.f, 4.15f, 5.4f, 6.8f};
static const float CP_PAN_DB[CP_PMODES]    = {0.f, -4.f, -10.f, -24.f, -28.f, -32.f};
static const float CP_PAN_T60[CP_PMODES]   = {1.8f, 1.2f, 0.8f, 0.35f, 0.3f, 0.25f};   // seconds at 440 Hz
static const float CP_PAN_D = 0.42f;         // the bowl's depth below its rim
static const float CP_PAN_T = 0.12f;         // the tilt: the rim falls this much toward the player
static const int   CP_PAN_SPAN = 28;         // semitones, C4 to E6
static const float CP_PAN_SKIRT = 0.2f;      // the skirt below the rim, short enough to stay inside the view
// A pan stick is light and rubber-tipped, and bounces straight off a pad: the
// contact is a fraction of the drum's, or every hit would lose its octave.
static const float CP_PAN_CONTACT = 0.3f;
// A drop rolling across the pan. Every pad is a raised, separately tuned patch of
// the bowl with a groove round it, so a drop crossing into or out of one knocks
// it; and the hammered steel of a pad is not smooth, so rolling over it is a run of
// small bumps, each a soft tap of that pad.
static const float CP_ROLL_EDGE  = 0.2f;     // a groove crossed, against a strike at the same speed
static const float CP_ROLL_BUMP  = 0.06f;    // one bump in the steel
static const float CP_ROLL_PITCH = 0.015f;   // how far apart the bumps are, on average
static inline float cpPanY(float x, float z) { return CP_PAN_D * (x * x + z * z - 1.f) - CP_PAN_T * z; }

// ── the singing bowls (2026-11) ──────────────────────────────────────────────
// One bowl per note of the scale, each on a cushion on a low table, the largest
// (the lowest note) in the middle. A bowl's partials are inharmonic, and each is a
// PAIR a little apart, because no hammered bowl is perfectly round: the pair beats,
// which is the slow pulsing of a real bowl, and where it is struck sets how deep.
// Ratios, levels and ring times are estimates from published measurements of
// Himalayan bowls, not of one bowl.
static const int   CP_BPART = 5;
static const int   CP_BMODES = 2 * CP_BPART;
static const float CP_BOWL_RATIO[CP_BPART] = {1.f, 2.72f, 5.12f, 8.21f, 11.92f};
static const float CP_BOWL_DB[CP_BPART]    = {0.f, -6.f, -12.f, -18.f, -24.f};
static const float CP_BOWL_T60[CP_BPART]   = {22.f, 12.f, 7.f, 4.5f, 3.f};   // seconds at 220 Hz
static const float CP_BOWL_SPLIT = 0.0015f;  // how far apart a pair is, of its frequency
static const float CP_BOWL_EVAP = 180.f;     // seconds for a full bowl of water to dry out
static const float CP_BOWL_FLOOR = -0.14f;   // the table the cushions sit on
static const float CP_BOWL_TABLE = 1.25f;    // the table's radius
static const float CP_BOWL_CONTACT = 0.5f;   // a drop on bronze, against the table's contact times

// The shape, after hammered Himalayan bowls, in units of the bowl's mouth radius:
// a flat foot, a wall that swells to its widest a little over half way up, and a
// mouth that turns back in, finished with a rounded lip. Heights are above the foot.
static const float CP_BOWL_HT    = 0.95f;    // the height
static const float CP_BOWL_BELLY = 1.10f;    // the widest radius
static const float CP_BOWL_FOOT  = 0.62f;    // the flat foot's radius
static const float CP_BOWL_UB    = 0.55f;    // where it is widest, as a fraction of the height
static const float CP_BOWL_WALL  = 0.05f;    // the wall's thickness
static const float CP_BOWL_BASE  = 0.08f;    // the floor's thickness
static inline float cpBowlOuter(float h) {
	float u = clamp(h / CP_BOWL_HT, 0.f, 1.f);
	if (u < CP_BOWL_UB) return CP_BOWL_FOOT + (CP_BOWL_BELLY - CP_BOWL_FOOT) * std::sin(0.5f * (float)M_PI * u / CP_BOWL_UB);
	float v = (u - CP_BOWL_UB) / (1.f - CP_BOWL_UB);
	return CP_BOWL_BELLY - (CP_BOWL_BELLY - 1.f) * v * v;
}
static inline float cpBowlInner(float h) { return cpBowlOuter(h) - CP_BOWL_WALL; }
// The wall's cross-section as a polygon in (radius, height), the axis at radius 0:
// the underside, up the outside, over the lip, down the inside, across the floor
// to the axis. Anticlockwise, so each edge's outward normal is (dh, -dr).
// Segment kinds: what part of the bowl a drop is touching.
enum { CPB_OUTSIDE, CPB_LIP, CPB_INSIDE };
struct CpBowlSection {
	static const int K = 16, L = 6, N = 2 * K + L + 3;
	float r[N], h[N];
	int kind(int seg) const { return seg <= K ? CPB_OUTSIDE : seg <= K + L ? CPB_LIP : CPB_INSIDE; }
};
// ── the wind chimes (2026-11) ──────────────────────────────────────────────
// Tubes hanging from a ring, one per note of the scale over one octave, with a
// clapper on a cord in the middle and a sail hanging under it. Every piece is a
// real pendulum: a drop that strikes a tube or lands on the sail sets it swinging,
// the clapper knocks the tubes and the tubes knock each other, so one drop can set
// off a run of notes. A tube rings as a free-free beam, whose partials are
// 1 : 2.757 : 5.404 : 8.933 : 13.34 (exact for a uniform bar); where along it a
// hit lands sets how much of each it gets. The ring times are estimates for
// aluminium tube.
static const int   CP_CMODES = 5;
static const float CP_CH_RATIO[CP_CMODES] = {1.f, 2.757f, 5.404f, 8.933f, 13.344f};
static const float CP_CH_DB[CP_CMODES]    = {0.f, -3.f, -7.f, -13.f, -19.f};
static const float CP_CH_T60[CP_CMODES]   = {9.f, 5.f, 3.f, 1.8f, 1.2f};   // seconds at 440 Hz
static const float CP_CH_HUB  = 0.78f;    // the ring the tubes hang from, below the lowest leaves
static const float CP_CH_RING = 0.48f;    // its radius
static const float CP_CH_LEN  = 1.05f;    // the longest tube
static const float CP_CH_TR   = 0.042f;   // a tube's radius
static const float CP_CH_STR  = 0.06f;    // the cord from the ring to a tube's top
static const float CP_CH_CLAP = 0.50f;    // how far below the ring the clapper hangs
static const float CP_CH_CLR  = 0.335f;   // the clapper's radius: a swing of 0.2 rad reaches a tube
static const float CP_CH_SAIL = 1.0f;     // how far below the ring the sail hangs
static const float CP_CH_SR   = 0.3f;     // the sail's radius
static const float CP_CH_ST   = 0.012f;   // its half-thickness
static const float CP_CH_GAIN = 1.5f;     // a tube's level against the drum's tongues

// ── the handpan (2026-11) ─────────────────────────────────────────────────
// Two steel shells joined at the rim, the top one a shallow dome: a raised note in
// the middle (the ding) and eight tone fields round it, each a shallow dent. The
// fields rise in pitch in a zigzag, alternating left and right from the one nearest
// the player, as most makers lay them out; the first is the first scale note at least
// a fifth above the ding, as on a D Kurd (D3 | A3 Bb3 C4 D4 E4 F4 G4 A4). Each note is
// tuned to its fundamental, its octave and the fifth above that, like a steel pan's,
// but a handpan rings for longer. The shell is convex, so drops run off it.
static const float CP_HP_H0   = -0.05f;   // the rim's height
static const float CP_HP_DOME = 0.30f;    // how far the top rises above the rim
static const float CP_HP_DR   = 0.22f;    // the ding's radius
static const float CP_HP_DH   = 0.06f;    // and how far it stands up
static const float CP_HP_RING = 0.62f;    // the tone fields' circle
static const float CP_HP_DENT = 0.025f;   // how deep a field is dented
static const float CP_HP_UNDER = 0.22f;   // the bottom shell, below the rim
static const int   CP_HP_FIELDS = 8;

// ── the bonang (2026-11) ──────────────────────────────────────────────────
// Javanese kettle gongs in two rows on cords across a low wooden frame. A kettle has
// a raised boss in the middle, a flat top round it and a rounded shoulder down to its
// side. The boss gives the note; the flat top gives a muted, brighter knock with less
// of the fundamental; the shoulder less again. The rows follow SCALE, near row an
// octave below the far one, both rising left to right (on a real bonang the kettles
// are arranged by the player and the piece, so this is one arrangement of several).
// No measured partial ratios for bonang kettles have been published: these are
// estimates for a small tuned kettle gong, a strong fundamental (a near pair, since
// no hand-forged kettle is perfectly round) and weaker inharmonic partials above.
static const int   CP_BNMODES = 6;
static const float CP_BN_RATIO[CP_BNMODES] = {1.f, 1.003f, 2.02f, 2.94f, 4.21f, 5.63f};
static const float CP_BN_DB[CP_BNMODES]    = {0.f, -3.f, -14.f, -18.f, -24.f, -28.f};
static const float CP_BN_T60[CP_BNMODES]   = {3.2f, 3.2f, 1.2f, 0.8f, 0.5f, 0.35f};   // seconds at 440 Hz
static const float CP_BN_BASE = -0.30f;   // the cords the kettles sit on
static const float CP_BN_KR   = 0.17f;    // the lowest kettle's radius, at most
static const float CP_BN_KH   = 0.17f;    // a kettle's top above the cords, at that size
static const float CP_BN_BOSS = 0.33f;    // the boss's radius, of the kettle's
static const float CP_BN_BH   = 0.38f;    // and its height, of the kettle's radius
static const float CP_BN_ROWZ = 0.21f;    // the rows, either side of the middle
static const float CP_BN_W    = 0.96f;    // the frame's half-width
static const float CP_BN_D    = 0.44f;    // and half-depth

// ── the marimba and the balafon (2026-11) ─────────────────────────────────
// Wooden bars over resonators. A marimba is a keyboard laid in a circle, so it fills
// the round view: two octaves from "Lowest note", the naturals as spokes round an
// outer ring rising clockwise from the front, the sharps raised on an inner ring
// between the naturals they lie between, as they lie behind them on a straight
// marimba. (A straight two-octave keyboard is twice as wide as it is deep and left
// most of the view empty.) Every bar is undercut so its first overtone is two octaves
// up and its second a little under ten times the fundamental (1 : 4 : 9.9); a note
// outside SCALE is damped, as the steel pan's are by default. A balafon is one row,
// the scale's own notes, on gourds whose membranes (traditionally spider-egg silk)
// buzz when the bar above is struck hard; its bars are tuned less exactly, so its
// overtones are estimates. Where a drop lands along a bar sets the partials by the
// bar's own mode shapes: the middle has no second mode, and near the ends all of
// them sound. A bar's length goes as one over the square root of its note.
static const int   CP_XMODES = 3;
static const float CP_MAR_RATIO[CP_XMODES] = {1.f, 3.99f, 9.9f};
static const float CP_BAL_RATIO[CP_XMODES] = {1.f, 2.9f, 6.2f};
static const float CP_X_DB[CP_XMODES]      = {0.f, -9.f, -17.f};
static const float CP_X_T60[CP_XMODES]     = {1.6f, 0.45f, 0.18f};   // seconds at 440 Hz
static const float CP_X_Y    = 0.0f;      // the naturals' tops
static const float CP_X_RAISE = 0.05f;    // how much higher the sharps sit
static const float CP_X_TH   = 0.045f;    // a bar's thickness
static const float CP_X_LEN  = 0.86f;     // the lowest bar's length
static const float CP_X_W    = 0.96f;     // half the keyboard's width
static const float CP_X_GAP  = 0.014f;    // between bars
static const float CP_X_BUZZ = 0.25f;     // the level, in a voice's own units, above which a membrane buzzes:
                                          // a soft touch (0.09) stays clean, a medium hit (0.35) buzzes

// A rigid pendulum swinging freely about a pivot. Small swings are two angles: ax
// moves its lower end toward +x and az toward +z. Impulses at any point turn into
// swing through the arm from the pivot, so a hit off the line under the pivot (a
// drop on the edge of the sail) swings it as well.
struct CpPend {
	float px = 0.f, py = 0.f, pz = 0.f;
	float ax = 0.f, az = 0.f, wx = 0.f, wz = 0.f;
	float I = 1.f, w2 = 1.f, damp = 0.2f;     // inertia, gravity's pull (omega squared), damping per second
	void dir(float d[3]) const {
		float x = std::tan(ax), z = std::tan(az), l = 1.f / std::sqrt(1.f + x * x + z * z);
		d[0] = x * l; d[1] = -l; d[2] = z * l;
	}
	void at(float s, float p[3]) const { float d[3]; dir(d); p[0] = px + s * d[0]; p[1] = py + s * d[1]; p[2] = pz + s * d[2]; }
	// the velocity of the point at r from the pivot: omega x r, omega = (-wz, 0, wx)
	void vel(const float r[3], float v[3]) const {
		float ox = -wz, oz = wx;
		v[0] = -oz * r[1]; v[1] = oz * r[0] - ox * r[2]; v[2] = ox * r[1];
	}
	// how easily the point at r moves along n: 1 / its effective mass
	float give(const float r[3], const float n[3]) const {
		float cx = r[1] * n[2] - r[2] * n[1], cz = r[0] * n[1] - r[1] * n[0];
		return (cx * cx + cz * cz) / I;
	}
	void push(const float r[3], const float J[3]) {
		float Lx = r[1] * J[2] - r[2] * J[1], Lz = r[0] * J[1] - r[1] * J[0];
		wz -= Lx / I; wx += Lz / I;
	}
	// move the point at r by `by` along the horizontal part of n
	void shift(const float r[3], const float n[3], float by) {
		float s = std::max(-r[1], 0.05f);
		ax = clamp(ax + by * n[0] / s, -0.9f, 0.9f); az = clamp(az + by * n[2] / s, -0.9f, 0.9f);
	}
	// At the end of its travel (it would be hitting the ring) it stops dead: an angle
	// held at the limit with its speed kept would report a velocity it does not
	// have, and fling whatever touched it.
	void step(float dt) {
		wx += (-w2 * std::sin(ax) - damp * wx) * dt; wz += (-w2 * std::sin(az) - damp * wz) * dt;
		wx = clamp(wx, -8.f, 8.f); wz = clamp(wz, -8.f, 8.f);
		ax += wx * dt; az += wz * dt;
		if (ax > 0.9f) { ax = 0.9f; wx = std::min(wx, 0.f); } else if (ax < -0.9f) { ax = -0.9f; wx = std::max(wx, 0.f); }
		if (az > 0.9f) { az = 0.9f; wz = std::min(wz, 0.f); } else if (az < -0.9f) { az = -0.9f; wz = std::max(wz, 0.f); }
	}
};
// A free-free beam's mode n, at u along it (0 one end, 1 the other), 1 at the ends.
static inline float cpBeamShape(int n, float u) {
	static const double BL[5] = {4.73004, 7.85320, 10.99561, 14.13717, 17.27876};
	double b = BL[clamp(n, 0, 4)], x = b * (double)clamp(u, 0.f, 1.f);
	double sg = (std::cosh(b) - std::cos(b)) / (std::sinh(b) - std::sin(b));
	return (float)(0.5 * (std::cosh(x) + std::cos(x) - sg * (std::sinh(x) + std::sin(x))));
}
// The nearest point of segment a-b to p, as a fraction along it.
static inline float cpSegNear(const float a[3], const float b[3], const float p[3], float q[3]) {
	float e[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
	float u = clamp(((p[0] - a[0]) * e[0] + (p[1] - a[1]) * e[1] + (p[2] - a[2]) * e[2]) / (e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + 1e-12f), 0.f, 1.f);
	for (int i = 0; i < 3; i++) q[i] = a[i] + u * e[i];
	return u;
}
// The nearest points of two segments, as fractions along each.
static inline void cpSegSeg(const float a0[3], const float a1[3], const float b0[3], const float b1[3], float& s, float& t) {
	float d1[3], d2[3], r[3];
	for (int i = 0; i < 3; i++) { d1[i] = a1[i] - a0[i]; d2[i] = b1[i] - b0[i]; r[i] = a0[i] - b0[i]; }
	float a = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2], e = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2];
	float f = d2[0] * r[0] + d2[1] * r[1] + d2[2] * r[2], c = d1[0] * r[0] + d1[1] * r[1] + d1[2] * r[2];
	float b = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2], den = a * e - b * b;
	s = den > 1e-9f ? clamp((b * f - c * e) / den, 0.f, 1.f) : 0.f;
	t = (b * s + f) / std::max(e, 1e-9f);
	if (t < 0.f) { t = 0.f; s = clamp(-c / std::max(a, 1e-9f), 0.f, 1.f); }
	else if (t > 1.f) { t = 1.f; s = clamp((b - c) / std::max(a, 1e-9f), 0.f, 1.f); }
}

static const CpBowlSection& cpBowlSection() {
	static const CpBowlSection S = [] {
		CpBowlSection c;
		const int K = CpBowlSection::K, L = CpBowlSection::L;
		int i = 0;
		c.r[i] = 0.f; c.h[i] = 0.f; i++;
		for (int k = 0; k <= K; k++) { float h = CP_BOWL_HT * (float)k / (float)K; c.r[i] = cpBowlOuter(h); c.h[i] = h; i++; }
		for (int j = 1; j <= L; j++) {
			float a = (float)M_PI * (float)j / (float)L;
			c.r[i] = 1.f - 0.5f * CP_BOWL_WALL + 0.5f * CP_BOWL_WALL * std::cos(a);
			c.h[i] = CP_BOWL_HT + 0.5f * CP_BOWL_WALL * std::sin(a);
			i++;
		}
		for (int k = 1; k <= K; k++) {
			float h = CP_BOWL_HT - (CP_BOWL_HT - CP_BOWL_BASE) * (float)k / (float)K;
			c.r[i] = cpBowlInner(h); c.h[i] = h; i++;
		}
		c.r[i] = 0.f; c.h[i] = CP_BOWL_BASE; i++;
		return c;
	}();
	return S;
}

static const float CP_RATIO[CP_MODES] = {1.000f, 2.005f, 2.998f, 3.520f, 4.015f, 5.980f};
static const float CP_DB[CP_MODES]    = {0.f, -9.f, -16.f, -24.f, -22.f, -30.f};
static const float CP_T60[CP_MODES]   = {5.0f, 2.2f, 1.1f, 0.5f, 0.6f, 0.25f};  // seconds at 220 Hz
// the clamped-free beam's roots, and sigma = (cosh + cos) / (sinh + sin) at each
static const double CP_BETA[CP_MODES]  = {1.875104, 4.694091, 7.854757, 10.995541, 14.137168, 17.278760};
static const double CP_SIGMA[CP_MODES] = {0.734096, 1.018467, 0.999224, 1.000034, 0.999999, 1.000000};

enum CanopyMaterial { TM_WATER, TM_RUBBER, TM_WOOD, TM_GLASS, TM_STEEL, TM_MIX, TM_COUNT };
struct CanopyMat {
	const char* name;
	float density;    // relative to water: mass, and so the weight of the strike
	float bounce;     // coefficient of restitution on the drum
	float friction;   // Coulomb: tangential speed lost, per unit of speed into the surface
	float contactMs;  // contact time at the reference size and speed
};
// The contact times are longer than a real marble's (a steel ball on steel is
// tens of microseconds), because that would sound the same for every material.
static const CanopyMat CP_MAT[TM_MIX] = {
	{"Water",  1.0f, 0.05f, 0.06f, 4.0f},
	{"Rubber", 1.1f, 0.78f, 0.16f, 3.0f},
	{"Wood",   0.7f, 0.55f, 0.13f, 1.2f},
	{"Glass",  2.5f, 0.80f, 0.08f, 0.45f},
	{"Steel",  7.8f, 0.85f, 0.08f, 0.3f},
};

// What a leaf is made of, and so what it does to what lands on it. A leaf is held
// by its stem and bends under a hit, then springs back, so it can throw a drop lying
// on it. A drop's bounce is multiplied by `give`; friction is the average of the two
// surfaces; `splash` scales how readily water breaks up on it; `hz` and `zeta` are how
// it bends. A living leaf is waxy, thin and springy: little friction, a moderate bounce
// and a quick, damped return. Felt sags and stays down, rubber and steel keep flapping,
// glass barely moves.
enum CanopyLeafKind { TP_LEAF, TP_FELT, TP_RUBBER, TP_WOOD, TP_GLASS, TP_STEEL, TP_MIX, TP_COUNT };
struct CanopyLeafMat { const char* name; float give, friction, splash, hz, zeta; };
static const CanopyLeafMat CP_LEAF_MAT[TP_MIX] = {
	{"Leaf",   0.40f, 0.05f, 0.5f,  6.f, 0.22f},
	{"Felt",   0.10f, 0.16f, 0.0f,  3.f, 0.70f},
	{"Rubber", 1.00f, 0.16f, 0.6f,  4.f, 0.10f},
	{"Wood",   0.50f, 0.10f, 0.7f,  8.f, 0.18f},
	{"Glass",  0.70f, 0.04f, 1.0f, 40.f, 0.40f},
	{"Steel",  0.95f, 0.05f, 1.0f, 11.f, 0.04f},
};

// The outlines, chosen from the menu. A form is a distance from a centre point at each
// angle, in units of the leaf's half-length, so a maple's lobes can point where they
// point and a monstera's cuts can run in toward the middle. The physics reads the same
// table, so a drop falls through a cut. `flat` scales the fold: a broad leaf folded
// as hard as a narrow one would close up. `palmate` veins start at the centre.
enum CanopyLeafForm { LF_BEECH, LF_MAPLE, LF_MONSTERA, LF_GINKGO, LF_OAK, LF_WILLOW, LF_MIXED, LF_COUNT = LF_MIXED };
static const char* const CP_FORM_NAME[LF_COUNT + 1] = {"Beech", "Maple", "Monstera", "Ginkgo", "Oak", "Willow", "All forms in turn"};
static const float CP_FORM_C[LF_COUNT]    = {-0.15f, -0.32f, -0.30f, -0.92f, 0.f, 0.f};   // the centre, along the leaf
// Half-length in drum radii, for a 45 cm drum, so the forms are the right size against the
// drum and each other. In cm tip to stem: beech 10, maple 12, monstera 30, ginkgo 6, oak 15,
// willow 8. A monstera has cuts only once it is mature, and 30 cm is the small end of that.
// On a 30 cm drum, the commonest size, one such leaf is as wide as the drum.
// Divided by how many half-lengths each outline spans.
static const float CP_FORM_LEN[LF_COUNT]  = {0.222f, 0.31f, 0.717f, 0.155f, 0.333f, 0.178f};
static const float CP_SLOT_SIZE[12] = {1.f, 0.92f, 1.08f, 0.96f, 1.12f, 0.88f, 1.04f, 0.94f, 1.1f, 0.9f, 1.02f, 0.98f};
static const float CP_FORM_FLAT[LF_COUNT] = {1.f, 0.35f, 0.40f, 0.30f, 0.8f, 1.2f};
static const bool  CP_FORM_PALM[LF_COUNT] = {false, true, false, true, false, false};
static const int   CP_FORM_NS[LF_COUNT]   = {28, 64, 72, 40, 48, 24};   // facets round the leaf
static const int   CP_FORM_NR[LF_COUNT]   = {4, 3, 3, 3, 3, 3};         // and out from the centre

// Each leaf has its own values for how much it arches, folds along the midrib and twists, how
// its midrib sweeps to one side, and how wide it is. Fixed per slot, so a patch keeps
// its leaves. The physics reads the same numbers as the drawing.
struct CanopyLeafShape { float arch, fold, twist, sweep, width; };
static inline CanopyLeafShape canopyLeafShape(int i) {
	static const float R[12][5] = {
		{0.20f, 0.20f,  0.10f,  0.08f, 1.00f}, {0.14f, 0.26f, -0.14f, -0.10f, 0.92f},
		{0.24f, 0.16f,  0.05f, -0.05f, 1.08f}, {0.17f, 0.22f, -0.08f,  0.12f, 0.96f},
		{0.12f, 0.28f,  0.14f,  0.05f, 0.88f}, {0.22f, 0.18f, -0.12f, -0.12f, 1.04f},
		{0.18f, 0.24f,  0.08f,  0.10f, 1.00f}, {0.26f, 0.15f, -0.06f, -0.06f, 0.94f},
		{0.15f, 0.20f,  0.12f, -0.09f, 1.06f}, {0.20f, 0.26f, -0.10f,  0.06f, 0.90f},
		{0.13f, 0.18f,  0.06f,  0.11f, 1.02f}, {0.23f, 0.22f, -0.14f, -0.04f, 0.98f},
	};
	const float* r = R[((i % 12) + 12) % 12];
	CanopyLeafShape S; S.arch = r[0]; S.fold = r[1]; S.twist = r[2]; S.sweep = r[3]; S.width = r[4];
	return S;
}

struct CanopyDrop {
	bool  alive = false;
	bool  droplet = false;      // a piece of a drop that has broken
	bool  original = false;     // the drop as it first appeared, or what is left of it on a leaf
	int   mat = TM_GLASS;
	float x = 0.f, y = 0.f, z = 0.f;
	float vx = 0.f, vy = 0.f, vz = 0.f;
	float r = CP_R0;
	float mass = 1.f;
	float age = 0.f;
	float still = 0.f;          // seconds barely moving on a surface
	bool  landed = false;       // has struck the drum
	bool  onTongue = false;     // has landed on a tongue
	bool  touched = false;      // has touched a leaf
	bool  released = false;     // the hoop and the draught no longer act on it, so it leaves
	float fade = 1.f;           // the pan: a drop that has settled in the bowl fades out
	int   pad = -2;             // the pan: the pad it is rolling on, -1 a groove, -2 not rolling
	float air = 1.f;            // seconds since it last touched a leaf
	float bump = 0.f;           // the pan: how far it rolls before the next bump in the steel
};

struct CanopyLeaf {
	float cx = 0.f, cy = 1.f, cz = 0.f;   // the centre
	float ax = 0.4f, ay = 0.f, az = 0.f;  // from the centre to one end
};
// What the physics reads: the plank's own axes, worked out once when it moves.
struct CanopyLeafLive {
	float cx = 0.f, cy = 0.f, cz = 0.f;
	float ax = 1.f, ay = 0.f, az = 0.f, L = 0.4f;   // unit vector along the length, half-length
	float bx = 0.f, by = 0.f, bz = 1.f;             // unit vector across, always level
	float nx = 0.f, ny = 1.f, nz = 0.f;             // the upward normal
	int   mat = TP_LEAF;
	int   form = LF_MONSTERA;
	float inertia = 1.5f;      // how little a hit bends it: a bigger leaf is harder to bend
	CanopyLeafShape shape = {0.f, 0.f, 0.f, 0.f, 1.f};
};

static inline void canopyMakeLeaf(CanopyLeafLive& K, float cx, float cy, float cz, float ax, float ay, float az) {
	K.cx = cx; K.cy = cy; K.cz = cz;
	float L = std::max(std::sqrt(ax * ax + ay * ay + az * az), 1e-3f);
	K.L = L;
	K.ax = ax / L; K.ay = ay / L; K.az = az / L;
	// across = up x along
	float bx = K.az, bz = -K.ax;
	float bl = std::sqrt(bx * bx + bz * bz);
	if (bl < 1e-4f) { bx = 0.f; bz = 1.f; bl = 1.f; }   // standing on end
	K.bx = bx / bl; K.by = 0.f; K.bz = bz / bl;
	// normal = along x across, turned to face up
	float nx = K.ay * K.bz - K.az * K.by;
	float ny = K.az * K.bx - K.ax * K.bz;
	float nz = K.ax * K.by - K.ay * K.bx;
	if (ny < 0.f) { nx = -nx; ny = -ny; nz = -nz; }
	K.nx = nx; K.ny = ny; K.nz = nz;
}

struct Canopy : Module {
	enum ParamId {
		RATE_PARAM, MATERIAL_PARAM, SIZE_PARAM, SCATTER_PARAM,
		DRIFT_PARAM, SPEED_PARAM, TILT_PARAM, LEAF_PARAM,
		DECAY_PARAM, TONE_PARAM, ROOT_PARAM, SCALE_PARAM,
		DENSITY_PARAM,
		INSTRUMENT_PARAM,            // appended 2026-11: params serialise by index
		PARAMS_LEN
	};
	enum InputId {
		RATE_INPUT, MATERIAL_INPUT, SIZE_INPUT, SCATTER_INPUT,
		DRIFT_INPUT, SPEED_INPUT, TILT_INPUT, LEAF_INPUT,
		DECAY_INPUT, TONE_INPUT, ROOT_INPUT, SCALE_INPUT,
		DENSITY_INPUT,
		VOCT_INPUT, GATE_INPUT, VEL_INPUT, CLOCK_INPUT, X_INPUT, Z_INPUT,
		INSTRUMENT_INPUT,            // appended 2026-11
		INPUTS_LEN
	};
	enum OutputId { POLY_OUTPUT, L_OUTPUT, R_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	// ── the drops ─────────────────────────────────────────────────────────
	CanopyDrop drop[CP_MAXDROPS];
	float physAcc = 0.f;
	float nextDrop = 0.f;         // how long until the next drop, in gaps at the present RATE, so turning it up acts at once
	uint32_t rng = 0x12345u;
	dsp::SchmittTrigger clockTrig;
	dsp::SchmittTrigger gateTrig[CP_MAXN];
	bool  evenly = false;         // saved: drops fall evenly rather than at random
	float rateK = 1.f, scatterK = 1.f, sizeK = CP_R0;
	float colX = 0.f, colZ = 0.f, colR = 0.95f;   // the column drops fall through
	int   matK = TM_MIX, padK = TP_LEAF;
	int   instrument = 0;         // what TYPE and its CV ask for: 0 the tongue drum, 1 the zither
	float switchFade = 1.f;       // the output fades out before the instrument changes, and back in
	int   instNow = 0;            // the instrument the voices and layout are set up for
	CpPend clap;                  // the wind chimes' clapper and sail, on one cord
	int   chimeSig = -1;
	float tiltC[CP_CMODES] = {1.f, 1.f, 1.f, 1.f, 1.f};
	float tiltBn[CP_BNMODES] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
	float tiltX[CP_XMODES] = {1.f, 1.f, 1.f};
	int   barSig = -1;
	float tiltZ[CP_ZMODES] = {};  // TONE, on the zither's harmonics
	float zDampK = CP_Z_DAMP;
	uint32_t zrng = 0x9E3779B9u;  // the rubbing noise's own, so the drops fall as they would anyway
	enum PanTuning { PAN_CHROMATIC, PAN_SCALE, PAN_DAMPED };
	int   panTuning = PAN_DAMPED; // saved
	int   panSig = -1;            // what the pan was laid out for
	float tiltP[CP_PMODES] = {};  // TONE, on the pan's partials
	float tiltB[CP_BPART] = {};   // TONE, on the bowls' partials
	int   bowlSig = -1;           // what the bowls were laid out for
	int   polyMap[16] = {};       // which voice each POLY channel carries, when there are more than 16
	long  strikes = 0, spawned = 0, landed = 0, onTongue = 0, touched = 0;   // counted, for the harness

	// ── the leaves ───────────────────────────────────────────────────────
	// The UI thread edits `leaf` and publishes it; the audio thread moves each
	// plank round the published place by DRIFT and TILT, and that is what the
	// physics and the screen both read.
	CanopyLeaf leaf[CP_MAXLEAVES];
	float formR[LF_COUNT][CP_FORM_TAB] = {};   // the outlines
	float formMax[LF_COUNT] = {};
	int   leafForm = LF_MONSTERA;       // saved: what the menu chose
	CanopyLeaf home[2][CP_MAXLEAVES];
	std::atomic<int> homeBank{0};
	CanopyLeafLive live[CP_MAXLEAVES];
	int nLive = 0;
	static const int DRIFT_AXES = 5;      // across, along, height, swing, pitch
	float driftPh[CP_MAXLEAVES][DRIFT_AXES] = {};
	float driftRatio[CP_MAXLEAVES][DRIFT_AXES] = {};
	float driftK = 0.f, tiltK = 0.f, speedK = 0.1f;
	int   leavesK = 3;
	float bend[CP_MAXLEAVES] = {}, bendV[CP_MAXLEAVES] = {};   // how far each leaf is bent down from its stem
	float leafFlash[CP_MAXLEAVES] = {};                        // for the screen: 1 just after a hit, falling to 0

	// ── the tongues ───────────────────────────────────────────────────────
	// One voice per tongue, or per string on the zither: the arrays are sized for
	// the zither's twelve harmonics, and the drum uses the first CP_MODES of them.
	struct Contact { float ph = 2.f, dph = 0.f, amp = 0.f; float g[CP_VMODES] = {}; };
	struct TongueVoice {
		float re[CP_VMODES] = {}, im[CP_VMODES] = {}, c[CP_VMODES] = {}, s[CP_VMODES] = {}, lv[CP_VMODES] = {};
		float ex[CP_VMODES] = {};
		// the zither: what drops resting on the string do to it, from the last physics step
		bool  zTouch = false;
		float zDamp[CP_ZMODES] = {};     // per-sample factor on each harmonic
		float zRub[CP_ZMODES] = {};      // how hard the rolling rubs each harmonic
		float zNoise = 0.f;
		// the steel pan: a pad's half-sizes along and across its radius, its
		// direction from the middle, which ring, whether SCALE damps it, and the
		// pads whose partials meet its own, which ring with it
		float pa = 0.f, pb = 0.f, pth = 0.f;
		int   ring = 0;
		bool  damped = false;
		int   sym[6] = {-1, -1, -1, -1, -1, -1};
		int   nsym = 0;
		// a singing bowl: its mouth's radius, the rim's height, how deep it is, the
		// direction its pairs are split along, how much water is in it (of its
		// depth) and what the coefficients were worked out for
		float bR = 0.f, bY = 0.f, bD = 0.f, bAx = 0.f;
		// a wind chime: the tube as a pendulum hanging from the ring (`len` is its length)
		CpPend pend;
		// a marimba or balafon bar: its half-width, the height of its top, and the
		// balafon gourd's membrane: a band-passed noise for its rattle
		float xw = 0.05f, xy = 0.f, buzzN = 0.f, buzzB = 0.f;
		float water = 0.f, waterCoef = -1.f;
		Contact contact[CP_CONTACTS];
		float freq = 0.f, semis = 0.f;    // what the coefficients were worked out for
		float tx = 0.f, tz = 0.f;         // the free end
		float ux = 0.f, uz = 1.f;         // from the free end toward the clamp
		float len = 0.4f, w = 0.14f;      // length, half-width
		float panL = 0.7071f, panR = 0.7071f;
		float out = 0.f;
		float env = 0.f;                  // for the screen
		bool  awake = false, fading = false;
	} tv[CP_MAXN];
	int   nT = 10;                 // tongues in play: the scale's notes, over two octaves when there is room
	int   layoutN = 0;
	float shape[CP_MODES][CP_SHAPE_N] = {};
	float tilt[CP_MODES] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
	Contact shellHit[CP_SHELL_CONTACTS];
	float bodyRe[2] = {}, bodyIm[2] = {}, bodyC[2] = {1.f, 1.f}, bodyS[2] = {};
	float decayK = 1.f, coefDecay = -1.f, coefSr = 0.f;
	int   ctl = 0;
	int   regOct = -1;            // saved: octave of the lowest tongue's root from C4
	int   rootCvMode = sfs::ROOT_CV_VOCT;
	int   root = 0;
	sfs::BusScale sc;

	// ── the output limiter, as Carillon's: 1 ms look-ahead, then soft clip ─────
	static const int LOOK = 48;
	float limBuf[2][LOOK] = {};
	int   limHead = 0;
	float limGain = 1.f, limDt = 0.f, limAtk = 1.f, limRel = 0.f;

	// ── the camera, saved ───────────────────────────────────────────────────
	float camYaw = 0.f, camTilt = 0.62f;

	Canopy() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate", " drops/s", 240.f, 0.05f);
		std::vector<std::string> mats;
		for (int i = 0; i < TM_MIX; i++) mats.push_back(CP_MAT[i].name);
		mats.push_back("Mix (a random material per drop)");
		configSwitch(MATERIAL_PARAM, 0.f, (float)(TM_COUNT - 1), (float)TM_MIX, "Drop material", mats);
		configParam(SIZE_PARAM, 0.f, 1.f, 0.46f, "Drop size (diameter, on a 45 cm drum)", " mm", 3.143f, 15.75f);
		configParam(SCATTER_PARAM, 0.f, 1.f, 1.f, "Scatter (width of the column the drops fall through)", "% of the drum", 0.f, 100.f);
		configParam(DRIFT_PARAM, 0.f, 1.f, 0.2f, "Drift (how far each leaf moves from where it was placed)", "%", 0.f, 100.f);
		configParam(SPEED_PARAM, 0.f, 1.f, 0.35f, "Drift speed", " Hz", 75.f, 0.02f);
		configParam(TILT_PARAM, 0.f, 1.f, 0.2f, "Tilt (how far each leaf turns and rocks as it drifts)", "%", 0.f, 100.f);
		std::vector<std::string> pads;
		for (int i = 0; i < TP_MIX; i++) pads.push_back(CP_LEAF_MAT[i].name);
		pads.push_back("Mix (a different material per leaf)");
		configSwitch(LEAF_PARAM, 0.f, (float)(TP_COUNT - 1), (float)TP_LEAF, "Leaf material", pads);
		configParam(DENSITY_PARAM, 0.f, (float)CP_MAXLEAVES, 3.f, "Density (number of leaves)");
		paramQuantities[DENSITY_PARAM]->snapEnabled = true;
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay (ring time)", "x", 16.f, 0.25f);
		configParam(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone (level of the overtones)", "%", 0.f, 200.f, -100.f);
		configSwitch(ROOT_PARAM, 0.f, 11.f, 0.f, "Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
		std::vector<std::string> names;
		for (int i = 0; i < sfs::NUM_SCALES; i++) names.push_back(sfs::SCALES[i].longName);
		// Pentatonic Minor rather than the bottom of the list: a tongue drum a
		// semitone apart does not sound like the instrument.
		configSwitch(SCALE_PARAM, 0.f, (float)(sfs::NUM_SCALES - 1), 4.f, "Scale", names);
		configInput(RATE_INPUT, "Rate CV (each volt doubles the drops per second)");
		configInput(MATERIAL_INPUT, "Drop material CV (1 V per material)");
		configInput(SIZE_INPUT, "Drop size CV (+-5V = full range)");
		configInput(SCATTER_INPUT, "Scatter CV (+-5V = full range)");
		configInput(DRIFT_INPUT, "Drift CV (+-5V = full range)");
		configInput(SPEED_INPUT, "Drift speed CV (+-5V = full range)");
		configInput(TILT_INPUT, "Tilt CV (+-5V = full range)");
		configInput(LEAF_INPUT, "Leaf material CV (1 V per material)");
		configInput(DENSITY_INPUT, "Density CV (1 V per leaf)");
		configInput(DECAY_INPUT, "Decay CV (+-5V = full range)");
		configInput(TONE_INPUT, "Tone CV (+-5V = full range)");
		configInput(ROOT_INPUT, sfs::rootCvName(sfs::ROOT_CV_VOCT));
		configInput(SCALE_INPUT, "Scale CV (1V per scale, or Key's scale bus)");
		configInput(VOCT_INPUT, "V/OCT (polyphonic, selects the nearest tongue; a note out of range moves by octaves into range)");
		configInput(GATE_INPUT, "Gate (polyphonic, strikes a tongue with no drop; with V/OCT unpatched, channel N strikes tongue N)");
		configInput(VEL_INPUT, "Velocity (polyphonic, 0-10 V sets how hard a gate strikes; 70% when unpatched)");
		configInput(CLOCK_INPUT, "Clock (each pulse releases one drop; RATE has no effect while this is patched)");
		configInput(X_INPUT, "Column X (+-5V moves the column from the left edge to the right, narrowing it to stay over the drum)");
		configSwitch(INSTRUMENT_PARAM, 0.f, (float)(CP_NINST - 1), 0.f, "Instrument", {"Tongue drum", "Zither", "Steel pan (tenor)", "Singing bowls", "Wind chimes", "Handpan", "Bonang", "Marimba", "Balafon"});
		configInput(INSTRUMENT_INPUT, "Instrument CV (1 V per instrument, added to TYPE)");
		configInput(Z_INPUT, "Column Z (+-5V moves the column from the back edge to the front, narrowing it to stay over the drum)");
		configOutput(POLY_OUTPUT, "Tongues (polyphonic, one channel per tongue, lowest note first)");
		configOutput(L_OUTPUT, "Mix left");
		configOutput(R_OUTPUT, "Mix right");
		buildShape();
		buildForms();
		rng ^= random::u32() | 1u;
		for (int i = 0; i < CP_MAXLEAVES; i++)
			for (int a = 0; a < DRIFT_AXES; a++) {
				driftPh[i][a] = uni();
				driftRatio[i][a] = 0.6f + 0.9f * uni();
			}
		resetLeaves();
		layoutTongues(sc.size > 0 ? tongueCount(sc.size) : 10);
	}

	// Half an outline as points from the tip round to the stem, across >= 0.
	struct FormPt { float a, y; };
	void formFromOutline(int f, const std::vector<FormPt>& half) {
		std::vector<FormPt> loop(half);
		for (int i = (int)half.size() - 2; i >= 1; i--) loop.push_back({half[i].a, -half[i].y});
		const float c = CP_FORM_C[f];
		for (int i = 0; i < CP_FORM_TAB; i++) {
			float th = -(float)M_PI + 2.f * (float)M_PI * (float)i / (float)CP_FORM_TAB;
			float dx = std::cos(th), dy = std::sin(th), best = 0.f;
			for (size_t k = 0; k < loop.size(); k++) {
				const FormPt& A = loop[k]; const FormPt& B = loop[(k + 1) % loop.size()];
				float ex = B.a - A.a, ey = B.y - A.y;
				float det = dx * -ey + dy * ex;
				if (std::fabs(det) < 1e-7f) continue;
				float ox = A.a - c, oy = A.y;
				float t = (ox * -ey + oy * ex) / det;
				float q = (dx * oy - dy * ox) / det;
				if (t > 0.f && q >= 0.f && q <= 1.f) best = std::max(best, t);
			}
			formR[f][i] = best;
		}
	}
	void buildForms() {
		std::vector<FormPt> beech, willow, oak;
		for (int i = 64; i >= 0; i--) {
			float t = (float)i / 64.f, a = 2.f * t - 1.f;
			// widest about a third of the way along, rounded at the stem, drawn out to a
			// point; the power rises along the leaf, which makes the tip concave
			beech.push_back({a, 0.56f * std::pow(std::max(std::sin((float)M_PI * std::pow(t, 0.72f)), 0.f), 0.7f + 0.9f * t)});
			willow.push_back({a, 0.2f * std::pow(std::max(std::sin((float)M_PI * std::pow(t, 0.85f)), 0.f), 1.1f)});
			float env = std::pow(std::max(std::sin((float)M_PI * std::pow(t, 0.9f)), 0.f), 0.75f);
			oak.push_back({a, 0.56f * env * (0.56f + 0.44f * std::pow(std::fabs(std::cos((float)M_PI * 3.5f * t)), 0.6f))});
		}
		formFromOutline(LF_BEECH, beech);
		formFromOutline(LF_WILLOW, willow);
		formFromOutline(LF_OAK, oak);
		// five lobes spreading from near the stem, the middle one longest, and each with a tooth
		formFromOutline(LF_MAPLE, {
			{1.00f, 0.00f}, {0.70f, 0.13f}, {0.76f, 0.30f}, {0.46f, 0.25f}, {0.14f, 0.14f},
			{0.34f, 0.52f}, {0.22f, 0.56f}, {0.52f, 0.96f}, {0.14f, 0.76f}, {-0.02f, 0.80f},
			{-0.12f, 0.36f}, {-0.30f, 0.46f}, {-0.48f, 0.84f}, {-0.64f, 0.46f}, {-0.76f, 0.36f},
			{-0.66f, 0.14f}, {-0.72f, 0.00f}});
		// a heart, with cuts running in from the edge most of the way to the middle
		formFromOutline(LF_MONSTERA, {
			{1.00f, 0.00f}, {0.86f, 0.28f}, {0.62f, 0.56f}, {0.28f, 0.80f}, {-0.14f, 0.90f},
			{-0.56f, 0.80f}, {-0.84f, 0.50f}, {-0.86f, 0.22f}, {-0.70f, 0.06f}, {-0.62f, 0.00f}});
		const float cuts[5] = {0.60f, 1.03f, 1.46f, 1.89f, 2.32f};    // radians from the tip
		for (int i = 0; i < CP_FORM_TAB; i++) {
			float th = std::fabs(-(float)M_PI + 2.f * (float)M_PI * (float)i / (float)CP_FORM_TAB);
			float keep = 1.f;
			for (float at : cuts) {
				float d = (th - at) / 0.05f;
				keep -= 0.70f * std::exp(-d * d * d * d);
			}
			formR[LF_MONSTERA][i] *= std::max(keep, 0.22f);
		}
		// a fan from the stem, with a notch in the middle of its outer edge
		for (int i = 0; i < CP_FORM_TAB; i++) {
			float th = std::fabs(-(float)M_PI + 2.f * (float)M_PI * (float)i / (float)CP_FORM_TAB);
			float edge = 1.0f - 0.5f * (th / 0.88f) * (th / 0.88f);
			float n = th / 0.07f;
			float notch = 1.f - 0.30f * std::exp(-n * n);
			float side = clamp((0.88f - th) / 0.06f, 0.f, 1.f);       // closes at the fan's sides
			formR[LF_GINKGO][i] = std::max(0.05f, 1.72f * edge * notch * side);
		}
		for (int f = 0; f < LF_COUNT; f++) {
			formMax[f] = 0.f;
			for (int i = 0; i < CP_FORM_TAB; i++) formMax[f] = std::max(formMax[f], formR[f][i]);
		}
	}
	// How far the edge is from the centre, at an angle from the tip.
	float formRadius(int f, float th) const {
		const float* tab = formR[clamp(f, 0, LF_COUNT - 1)];
		float p = (th + (float)M_PI) / (2.f * (float)M_PI) * (float)CP_FORM_TAB;
		int i0 = (int)std::floor(p);
		float fr = p - (float)i0;
		i0 = ((i0 % CP_FORM_TAB) + CP_FORM_TAB) % CP_FORM_TAB;
		return tab[i0] + (tab[(i0 + 1) % CP_FORM_TAB] - tab[i0]) * fr;
	}
	// Height of the surface over the leaf's flat plane, per unit of half-length, at `u`
	// along (-1 stem, 1 tip) and `y` across: it arches down toward both ends, folds up
	// from the midrib to the edges, and twists along its length.
	static float leafLift(const CanopyLeafShape& S, int form, float u, float y) {
		return -S.arch * u * u + S.fold * CP_FORM_FLAT[clamp(form, 0, LF_COUNT - 1)] * std::fabs(y) + S.twist * u * y;
	}

	// Mode k of a clamped-free beam at xi along it, 0 at the clamp and 1 at the
	// free end, normalised so the free end is 1. A hit near the clamp barely moves
	// the fundamental and gives the overtones relatively more.
	void buildShape() {
		for (int m = 0; m < CP_MODES; m++) {
			double b = CP_BETA[m], sg = CP_SIGMA[m];
			double tipV = std::cosh(b) - std::cos(b) - sg * (std::sinh(b) - std::sin(b));
			for (int i = 0; i < CP_SHAPE_N; i++) {
				double x = b * (double)i / (double)(CP_SHAPE_N - 1);
				double v = std::cosh(x) - std::cos(x) - sg * (std::sinh(x) - std::sin(x));
				shape[m][i] = (float)std::fabs(v / tipV);
			}
		}
	}

	void onReset() override {
		evenly = false; regOct = -1; leafForm = LF_MONSTERA; panTuning = PAN_DAMPED;
		camYaw = 0.f; camTilt = 0.62f;
		for (CanopyDrop& d : drop) d.alive = false;
		resetLeaves();
	}

	// Twelve places, each stem higher and further out than its tip. Heights follow a
	// low-discrepancy sequence and bearings the golden angle, so however many are in
	// play, the first N are spread through the column rather than stacked at the top.
	void resetLeaves() {
		for (int i = 0; i < CP_MAXLEAVES; i++) {
			CanopyLeaf& P = leaf[i];
			float vdc = 0.f, f = 0.5f;
			for (int n = i + 1; n > 0; n >>= 1, f *= 0.5f) if (n & 1) vdc += f;
			float th = 2.4f * (float)i + 0.6f;
			float rad = 0.34f + 0.2f * (float)((i * 5) % 3) * 0.5f;
			P.cx = rad * std::cos(th); P.cz = rad * std::sin(th);
			P.cy = 0.85f + 1.4f * vdc;
			float half = 0.38f + 0.05f * (float)((i * 7) % 3);
			// pointing round the column and a little inward, tip down
			float dir = th + 2.2f;
			P.ax = half * std::cos(dir); P.az = half * std::sin(dir);
			P.ay = -half * 0.24f;
		}
		publishLeaves();
	}

	// UI thread, or the patch loading.
	void publishLeaves() {
		int bank = 1 - homeBank.load();
		for (int i = 0; i < CP_MAXLEAVES; i++) home[bank][i] = leaf[i];
		homeBank.store(bank);
	}

	// Each leaf moves around where it was placed: two sines per axis at rates no two
	// leaves share, so it never quite repeats, and DRIFT and TILT set how far it can go.
	void updateLive(float dt) {
		int bank = homeBank.load();
		int n = clamp(leavesK, 0, CP_MAXLEAVES);
		float hz = speedK * dt;
		for (int i = 0; i < n; i++) {
			const CanopyLeaf& P = home[bank][i];
			float v[DRIFT_AXES];
			for (int a = 0; a < DRIFT_AXES; a++) {
				float& ph = driftPh[i][a];
				ph += hz * driftRatio[i][a];
				if (ph >= 1.f) ph -= 1.f;
				float second = ph * 2.37f;
				second -= std::floor(second);
				v[a] = 0.65f * SFS_SIN2PI(ph) + 0.35f * SFS_SIN2PI(second);
			}
			float cx = P.cx + driftK * 0.42f * v[0];
			float cz = P.cz + driftK * 0.42f * v[1];
			float cy = clamp(P.cy + driftK * 0.22f * v[2], 0.25f, 2.4f);
			// the stored axis is only a direction: a leaf's size comes from its form
			const int form = clamp(leafForm == LF_MIXED ? i % LF_COUNT : leafForm, 0, LF_COUNT - 1);
			const float L = CP_FORM_LEN[form] * CP_SLOT_SIZE[i % 12];
			float pl = std::max(std::sqrt(P.ax * P.ax + P.ay * P.ay + P.az * P.az), 1e-3f);
			float sw = tiltK * 1.1f * v[3], csw = std::cos(sw), ssw = std::sin(sw);
			float ax = (P.ax * csw - P.az * ssw) / pl;
			float az = (P.ax * ssw + P.az * csw) / pl;
			float ay = P.ay / pl + tiltK * 0.22f * v[4];
			canopyMakeLeaf(live[i], cx, cy, cz, ax * L, ay * L, az * L);
			const float big = L / 0.43f;
			live[i].inertia = CP_LEAF_I * big * big * big;
			live[i].mat = padK == TP_MIX ? i % TP_MIX : padK;
			live[i].shape = canopyLeafShape(i);
			live[i].form = form;
		}
		nLive = n;
	}

	// ── random ───────────────────────────────────────────────────────────
	uint32_t next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
	float uni() { return (float)(next() >> 8) * (1.f / 16777216.f); }
	float bi() { return uni() * 2.f - 1.f; }

	// ── a drop ─────────────────────────────────────────────────────────────
	CanopyDrop* freeDrop() {
		CanopyDrop* oldest = &drop[0];
		for (CanopyDrop& d : drop) {
			if (!d.alive) return &d;
			if (d.age > oldest->age) oldest = &d;
		}
		return oldest;
	}
	void setMaterial(CanopyDrop& d, int m, float radius) {
		d.mat = m; d.r = radius;
		float q = radius / CP_R0;
		d.mass = CP_MAT[m].density * q * q * q;
	}
	int chooseMaterial() { return clamp(matK == TM_MIX ? (int)(uni() * TM_MIX) : matK, 0, TM_MIX - 1); }
	void spawn(float x, float z) {
		CanopyDrop& d = *freeDrop();
		d = CanopyDrop();
		d.alive = true;
		d.original = true;
		spawned++;
		setMaterial(d, chooseMaterial(), sizeK);
		d.x = x; d.y = CP_H; d.z = z;
	}
	// Water breaks into pieces about half its radius, and a piece still big enough breaks
	// again when it lands: big to medium to small. A thrown piece needs less speed to
	// break than a falling drop, because it has only fallen as far as it was thrown.
	static bool canBreak(const CanopyDrop& d, float speed) {
		return d.mat == TM_WATER && d.r >= CP_WATER_MIN && speed > (d.droplet ? 0.8f : 1.5f);
	}
	void splash(const CanopyDrop& from, float speed) {
		int n = 2 + (int)(uni() * 3.f);
		for (int i = 0; i < n; i++) {
			CanopyDrop& d = *freeDrop();
			float a = uni() * 2.f * (float)M_PI;
			float out = speed * (0.08f + 0.14f * uni());
			d = CanopyDrop();
			d.alive = true; d.droplet = true;
			setMaterial(d, TM_WATER, from.r * (0.45f + 0.2f * uni()));
			d.x = from.x; d.y = from.y + from.r; d.z = from.z;
			d.vx = out * std::cos(a); d.vz = out * std::sin(a);
			d.vy = speed * (0.12f + 0.2f * uni());
		}
	}

	// ── the tongues on the drum ───────────────────────────────────────────
	// A pentatonic scale over two octaves is ten, the common drum: the lowest note
	// is the big tongue in the middle and the other nine go round it. A scale with
	// more than eight notes gets one octave, and there are never more than sixteen.
	static int tongueCount(int scaleSize) {
		int s = std::max(scaleSize, 1);
		return clamp(s <= 8 ? s * 2 : s, 3, CP_MAXT);
	}
	// Going round from the front, the notes rise up alternate sides to the back.
	void layoutTongues(int n) {
		layoutN = n;
		int ring = n - 1;
		TongueVoice& D = tv[0];
		D.tx = 0.f; D.tz = 0.24f; D.ux = 0.f; D.uz = -1.f; D.len = 0.54f; D.w = 0.17f;
		D.panL = D.panR = 0.7071f;
		float w = std::min(0.15f, 0.72f * (float)M_PI / (float)ring * 0.82f);
		for (int r = 1; r < n; r++) {
			int q = r - 1;
			int j = q == 0 ? 0 : ((q + 1) / 2) * (q % 2 == 1 ? -1 : 1);
			float a = 2.f * (float)M_PI * (float)j / (float)ring;
			float sa = std::sin(a), ca = std::cos(a);
			TongueVoice& T = tv[r];
			T.w = w;
			// a higher tongue is shorter, as cutting one longer lowers its note
			T.len = clamp(0.40f * std::pow(2.f, -T.semis / 42.f), 0.20f, 0.40f);
			float tipR = 0.90f - T.len;
			T.tx = sa * tipR; T.tz = ca * tipR;
			T.ux = sa; T.uz = ca;
			float pan = sa * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
			T.panL = std::cos(th); T.panR = std::sin(th);
		}
	}

	// ── the strings on the zither ─────────────────────────────────────────────
	// As many as the drum would have tongues, a fixed distance apart, so a drop
	// wider than the spacing rests on two of them. Each runs from the back bridge
	// toward the viewer; the low ones are the longest, the high ones the shortest.
	// tx is the string's x, tz the back bridge, len its length toward the front.
	void layoutStrings(int n) {
		layoutN = n;
		float half = 0.5f * CP_Z_SP * (float)(n - 1);
		for (int t = 0; t < CP_MAXN; t++) {
			TongueVoice& T = tv[t];
			float k = n > 1 ? (float)std::min(t, n - 1) / (float)(n - 1) : 0.f;
			T.tx = -half + CP_Z_SP * (float)t;
			T.tz = CP_Z_BACK;
			T.ux = 0.f; T.uz = 1.f;
			T.len = CP_Z_FRONT0 + (CP_Z_FRONT1 - CP_Z_FRONT0) * k - CP_Z_BACK;
			T.w = CP_Z_R;
			float pan = (2.f * k - 1.f) * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
			T.panL = std::cos(th); T.panR = std::sin(th);
		}
	}
	// The column and the draught are laid out on the drum, a disc of radius 1.
	// On the zither they are scaled onto the string bed, which is narrower than
	// the drum and runs from the back bridge toward the viewer: x by sx, z by sz
	// about zc.
	void bedScale(float& sx, float& sz, float& zc) const {
		sx = 0.5f * CP_Z_SP * (float)(nT - 1) + 0.04f;
		sz = 0.62f;
		zc = -0.14f;
	}
	// where the front edge of the board is, at x
	float zitherFront(float x) const {
		float half = 0.5f * CP_Z_SP * (float)(nT - 1);
		float k = half > 0.f ? clamp((x + half) / (2.f * half), 0.f, 1.f) : 0.f;
		return CP_Z_FRONT0 + (CP_Z_FRONT1 - CP_Z_FRONT0) * k + CP_Z_EDGE;
	}
	// A string is fixed at both ends, so harmonic n moves as sin(n pi xi) along it:
	// a hit at the middle gives only the odd harmonics, one near an end all of them.
	void startContactZ(int t, float A, float n, float xi) {
		TongueVoice& T = tv[t];
		int slot = 0;
		float oldest = -1.f;
		for (int i = 0; i < CP_CONTACTS; i++) if (T.contact[i].ph > oldest) { oldest = T.contact[i].ph; slot = i; }
		Contact& C = T.contact[slot];
		C.ph = 0.f;
		C.dph = 1.f / n;
		C.amp = A * 2.f / n;
		for (int m = 0; m < CP_ZMODES; m++)
			C.g[m] = std::sin((float)(m + 1) * (float)M_PI * clamp(xi, 0.02f, 0.98f)) * tiltZ[m];
		T.awake = true;
	}
	// a drop striking string t at xi (0 the back bridge, 1 the front); the board hears it too
	void hitString(int t, float xi, float speed, int mat, float radius, float mass) {
		const CanopyMat& M = CP_MAT[mat];
		float q = radius / CP_R0;
		float tc = 0.001f * M.contactMs * q * SFS_POWABS(speed / CP_V0, -0.2f);
		float n = std::max(tc * coefSr, 2.f);
		float A = SFS_POWABS(mass, 0.35f) * (speed / CP_V0);
		startContactZ(t, A, n, xi);
		knockBody(A * 0.15f, n);
		strikes++;
	}
	// a drop landing on the board between the strings: a knock, and the nearest
	// strings ring faintly through it
	void hitBoard(float x, float z, float speed, int mat, float radius, float mass) {
		const CanopyMat& M = CP_MAT[mat];
		float q = radius / CP_R0;
		float tc = 0.001f * M.contactMs * q * SFS_POWABS(speed / CP_V0, -0.2f);
		float n = std::max(tc * coefSr, 2.f);
		float A = SFS_POWABS(mass, 0.35f) * (speed / CP_V0);
		for (int t = 0; t < nT; t++) {
			float g = 0.06f * SFS_EXP(-std::fabs(x - tv[t].tx) / 0.08f);
			if (g < 0.01f) continue;
			startContactZ(t, A * g, n, clamp((z - tv[t].tz) / tv[t].len, 0.f, 1.f));
		}
		knockBody(A, n);
		strikes++;
	}
	void knockBody(float A, float n) {
		int slot = 0; float oldest = -1.f;
		for (int i = 0; i < CP_SHELL_CONTACTS; i++) if (shellHit[i].ph > oldest) { oldest = shellHit[i].ph; slot = i; }
		Contact& S = shellHit[slot];
		S.ph = 0.f; S.dph = 1.f / n;
		S.amp = A * 2.f / n;
	}
	// A drop resting or rolling on a string touches it where it sits: the
	// harmonics that move most there are damped, as a finger touching a string
	// damps them, and rolling rubs the string, more for a heavier, faster drop.
	void touchString(TongueVoice& T, float xi, float mass, float speed) {
		if (!T.zTouch) {
			T.zTouch = true;
			for (int m = 0; m < CP_ZMODES; m++) { T.zDamp[m] = 1.f; T.zRub[m] = 0.f; }
		}
		T.awake = true;
		float w = std::sqrt(mass);
		float rub = SFS_POWABS(mass, 0.35f) * std::min(speed / CP_V0, 1.f);
		for (int m = 0; m < CP_ZMODES; m++) {
			float sn = std::sin((float)(m + 1) * (float)M_PI * clamp(xi, 0.f, 1.f));
			T.zDamp[m] *= std::max(1.f - zDampK * w * sn * sn, 0.9f);
			T.zRub[m] += rub * sn * tiltZ[m];
		}
	}

	// ── the singing bowls ─────────────────────────────────────────────────────
	// One bowl per note of the scale from ROOT, over one octave ("Lowest note"
	// sets which; C3 by default), and a scale of more than eight notes is thinned
	// evenly to eight. A bowl's note falls as its mouth widens (frequency goes as
	// 1/D^2 for a given wall), so the bowls run from the largest, in the middle and
	// lowest, to small ones on taller stands round it.
	int bowlUnder(float x, float z) const {
		int best = -1; float bq = 1.f;
		for (int t = 0; t < nT; t++) {
			const TongueVoice& T = tv[t];
			float q = ((x - T.tx) * (x - T.tx) + (z - T.tz) * (z - T.tz)) / (T.bR * T.bR);
			if (q < bq) { bq = q; best = t; }
		}
		return best;
	}
	int bowlSignature() const {
		int h = root * 131 + (regOct + 3) * 17 + sc.size * 3;
		for (int k = 0; k < sc.size; k++) h = h * 31 + (int)std::round(sc.intervals[k] * 100.f);
		return h;
	}
	void layoutBowls() {
		int size = std::max(sc.size, 1);
		int n = clamp(size, 3, 8);
		nT = n; layoutN = n;
		// the cushions are not all the same thickness
		static const float CT[8] = {0.06f, 0.045f, 0.07f, 0.05f, 0.065f, 0.045f, 0.06f, 0.05f};
		bowlTop = CP_BOWL_FLOOR;
		float lowest = 0.f, outer = 0.f;
		for (int i = 0; i < n; i++) {
			int k = size <= 8 ? i : (int)std::round((float)i * (float)size / (float)n);
			TongueVoice& T = tv[i];
			T.semis = 12.f * (float)regOct + (float)root + sc.degree(k);
			if (i == 0) lowest = T.semis;
			T.bR = 0.30f * std::pow(2.f, -(T.semis - lowest) / 24.f);
			T.bD = CP_BOWL_HT * T.bR;
			T.bY = CP_BOWL_FLOOR + CT[i] + T.bD;
			bowlTop = std::max(bowlTop, T.bY);
			T.bAx = 1.3f * (float)i;
			if (i > 0) outer = std::max(outer, T.bR);
		}
		// one ring round the middle bowl, far enough out that no two bellies touch
		float gap = 0.05f, ring = CP_BOWL_BELLY * (tv[0].bR + outer) + gap;
		if (n > 2) ring = std::max(ring, (2.f * CP_BOWL_BELLY * outer + gap) / (2.f * std::sin((float)M_PI / (float)(n - 1))));
		for (int i = 0; i < n; i++) {
			TongueVoice& T = tv[i];
			if (i == 0) { T.tx = 0.f; T.tz = 0.f; }
			else {
				float a = 2.f * (float)M_PI * (float)(i - 1) / (float)(n - 1) + 0.4f;
				T.tx = ring * std::sin(a); T.tz = ring * std::cos(a);
			}
			T.water = 0.f; T.waterCoef = -1.f;
			float pan = clamp(T.tx, -1.f, 1.f) * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
			T.panL = std::cos(th); T.panR = std::sin(th);
		}
	}
	float bowlTop = 0.f;     // the highest rim
	// the hollow of the bowl (x, y, z) is in, or -1
	int bowlHollow(float x, float y, float z) const {
		for (int t = 0; t < nT; t++) {
			const TongueVoice& T = tv[t];
			float h = (y - (T.bY - T.bD)) / T.bR;
			if (h < CP_BOWL_BASE || h > CP_BOWL_HT) continue;
			float dx = x - T.tx, dz = z - T.tz;
			if (std::sqrt(dx * dx + dz * dz) < cpBowlInner(h) * T.bR) return t;
		}
		return -1;
	}
	// the water's height in a bowl, above its foot, in mouth radii
	static float waterH(float fill) { return CP_BOWL_BASE + fill * (0.75f * CP_BOWL_HT - CP_BOWL_BASE); }
	// Water in a bowl lowers its note and shortens its ring, as it does in a real
	// one: at 90% full, about 10% lower. The pairs keep their split.
	void bowlCoefs(TongueVoice& T, int t, float sr) {
		float fill = T.water;
		float f = std::min(dsp::FREQ_C4 * std::pow(2.f, T.semis / 12.f) * (1.f - 0.12f * fill * std::sqrt(fill)), sr * 0.45f);
		T.freq = f;
		T.waterCoef = fill;
		float rShort = std::sqrt(220.f / f);
		for (int k = 0; k < CP_BPART; k++) {
			float sp = CP_BOWL_SPLIT * (1.f + 0.6f * std::sin(2.3f * (float)t + 1.7f * (float)k));
			for (int j = 0; j < 2; j++) {
				int m = 2 * k + j;
				float fm = f * CP_BOWL_RATIO[k] * (1.f + (j ? 0.5f : -0.5f) * sp);
				if (fm >= sr * 0.45f) { T.lv[m] = 0.f; T.c[m] = 1.f; T.s[m] = 0.f; continue; }
				float t60 = std::max(CP_BOWL_T60[k] * decayK * clamp(rShort, 0.5f, 1.6f) * (1.f - 0.5f * fill), 0.02f);
				float dec1 = std::pow(10.f, -3.f / (t60 * sr));
				float w = 2.f * (float)M_PI * fm / sr;
				T.c[m] = dec1 * std::cos(w); T.s[m] = dec1 * std::sin(w);
				// 0.65: a bowl rings for tens of seconds, so its tails stack, and this
				// keeps the set level with the drum
				T.lv[m] = 0.65f * std::pow(10.f, CP_BOWL_DB[k] / 20.f);
			}
		}
	}
	// How much a point excites each partial: more toward the rim (rrel 1) than low
	// in the bowl, and between the pair by where round the bowl it is, against the
	// direction the bowl's pairs are split along. A hit on that line rings one of
	// the pair and does not beat; one between rings both and beats deepest.
	void bowlShape(const TongueVoice& T, float rrel, float phi, float g[CP_BMODES]) const {
		float r = clamp(rrel, 0.f, 1.f);
		for (int k = 0; k < CP_BPART; k++) {
			float rad = std::pow(r, (float)(k + 1)), a = (float)(k + 2) * (phi - T.bAx);
			g[2 * k] = rad * std::cos(a);
			g[2 * k + 1] = rad * std::sin(a);
		}
	}
	void hitBowlAt(int t, float rrel, float phi, float speed, int mat, float radius, float mass) {
		const CanopyMat& M = CP_MAT[mat];
		float q = radius / CP_R0;
		float tc = 0.001f * CP_BOWL_CONTACT * M.contactMs * q * SFS_POWABS(speed / CP_V0, -0.2f);
		float n = std::max(tc * coefSr, 2.f);
		float A = SFS_POWABS(mass, 0.35f) * (speed / CP_V0) * (1.f - 0.6f * tv[t].water);
		TongueVoice& T = tv[t];
		int slot = 0;
		float oldest = -1.f;
		for (int i = 0; i < CP_CONTACTS; i++) if (T.contact[i].ph > oldest) { oldest = T.contact[i].ph; slot = i; }
		Contact& C = T.contact[slot];
		C.ph = 0.f; C.dph = 1.f / n; C.amp = A * 2.f / n;
		float g[CP_BMODES];
		bowlShape(T, rrel, phi, g);
		for (int m = 0; m < CP_BMODES; m++) C.g[m] = g[m] * tiltB[m / 2];
		T.awake = true;
		strikes++;
	}
	// A drop rolling round inside a bowl sustains it, as a ball circling a bowl does,
	// through the zither's sustain: what it touches is damped a little, the rest is
	// pushed along in step with itself.
	void touchBowl(TongueVoice& T, float rrel, float phi, float mass, float speed) {
		if (!T.zTouch) {
			T.zTouch = true;
			for (int m = 0; m < CP_BMODES; m++) { T.zDamp[m] = 1.f; T.zRub[m] = 0.f; }
		}
		T.awake = true;
		float w = std::sqrt(mass);
		float rub = SFS_POWABS(mass, 0.35f) * std::min(speed / CP_V0, 1.f);
		float g[CP_BMODES];
		bowlShape(T, rrel, phi, g);
		for (int m = 0; m < CP_BMODES; m++) {
			T.zDamp[m] *= std::max(1.f - 0.25f * zDampK * w * g[m] * g[m], 0.9f);
			T.zRub[m] += rub * std::fabs(g[m]) * tiltB[m / 2];
		}
	}
	// One step of a drop against the bowls and the table. Returns false if it has
	// gone. A bowl is its real wall, a surface of revolution, so a drop can strike
	// it outside or in, run round inside it, or land on the lip, where nothing holds
	// it: the lip is round, and a drop on it rolls off one side or the other.
	bool stepBowls(CanopyDrop& d, bool& slow) {
		const CanopyMat& M = CP_MAT[d.mat];
		if (d.released) {
			d.fade -= CP_DT / 0.4f;
			if (d.fade <= 0.f) { d.alive = false; return false; }
		}
		// the table under the cushions: silent, and water soaks into its cloth
		if (d.y - d.r < CP_BOWL_FLOOR && d.y > CP_BOWL_FLOOR - 0.15f && d.vy < 0.f
		    && d.x * d.x + d.z * d.z < CP_BOWL_TABLE * CP_BOWL_TABLE) {
			if (d.mat == TM_WATER) { d.alive = false; return false; }
			float vn = d.vy;
			d.y = CP_BOWL_FLOOR + d.r;
			reflect(d, 0.f, 1.f, 0.f, vn, 0.4f * M.bounce, M.friction, false);
			if (-vn < 0.6f) slow = true;
		}
		const CpBowlSection& S = cpBowlSection();
		for (int t = 0; t < nT; t++) {
			TongueVoice& T = tv[t];
			const float R = T.bR, base = T.bY - T.bD;
			float dx = d.x - T.tx, dz = d.z - T.tz;
			float rho = std::sqrt(dx * dx + dz * dz);
			if (rho > CP_BOWL_BELLY * R + d.r || d.y < base - d.r || d.y > T.bY + 0.5f * CP_BOWL_WALL * R + d.r) continue;
			float ux = 1.f, uz = 0.f;
			if (rho > 1e-5f) { ux = dx / rho; uz = dz / rho; }
			float phi = std::atan2(dx, dz);
			// water: a drop that reaches the water's surface in the hollow stays there
			const float pr = rho / R, ph = (d.y - base) / R, rad = d.r / R;
			if (d.mat == TM_WATER && ph > CP_BOWL_BASE && ph < CP_BOWL_HT && pr < cpBowlInner(ph)
			    && ph - rad <= waterH(T.water) && T.water > 0.01f) {
				float speed = std::fabs(d.vy);
				if (speed >= CP_MIN_STRIKE && !d.released) hitBowlAt(t, 0.3f, phi, speed * 0.3f, d.mat, d.r, d.mass);
				addWater(T, d);
				return false;
			}
			if (T.water > 0.01f && ph < waterH(T.water) && pr < cpBowlInner(ph)) {
				d.vx *= 0.995f; d.vy *= 0.99f; d.vz *= 0.995f;      // sinking through water
			}
			// the nearest point of the wall's section, and whether the drop is in the wall
			float best = 1e9f, qr = 0.f, qh = 0.f; int seg = 0; bool in = false;
			for (int i = 0; i + 1 < CpBowlSection::N; i++) {
				float ar = S.r[i], ah = S.h[i], er = S.r[i + 1] - ar, eh = S.h[i + 1] - ah;
				float u = clamp(((pr - ar) * er + (ph - ah) * eh) / (er * er + eh * eh + 1e-12f), 0.f, 1.f);
				float cr = ar + u * er, ch = ah + u * eh;
				float dd = (pr - cr) * (pr - cr) + (ph - ch) * (ph - ch);
				if (dd < best) { best = dd; qr = cr; qh = ch; seg = i; }
			}
			for (int i = 0; i < CpBowlSection::N; i++) {
				int j = (i + 1) % CpBowlSection::N;
				if ((S.h[i] > ph) != (S.h[j] > ph) && pr < S.r[i] + (ph - S.h[i]) * (S.r[j] - S.r[i]) / (S.h[j] - S.h[i])) in = !in;
			}
			best = std::sqrt(best);
			if (!in && best >= rad) continue;
			float nr, nh;
			if (best > 1e-6f) { nr = (pr - qr) / best; nh = (ph - qh) / best; if (in) { nr = -nr; nh = -nh; } }
			else {
				float er = S.r[seg + 1] - S.r[seg], eh = S.h[seg + 1] - S.h[seg], el = std::sqrt(er * er + eh * eh) + 1e-12f;
				nr = eh / el; nh = -er / el;
			}
			float rNew = std::max((qr + nr * rad) * R, 0.f);
			d.x = T.tx + ux * rNew; d.z = T.tz + uz * rNew;
			d.y = base + (qh + nh * rad) * R;
			float nx = nr * ux, nz = nr * uz;
			float vn = d.vx * nx + d.vy * nh + d.vz * nz;
			if (vn >= 0.f) continue;
			float speed = -vn;
			const int kind = S.kind(seg);
			// the wall rings most at the lip and least at the foot
			float rrel = 0.3f + 0.7f * clamp(qh / CP_BOWL_HT, 0.f, 1.f);
			if (speed >= CP_MIN_STRIKE && !d.released) {
				hitBowlAt(t, rrel, phi, speed, d.mat, d.r, d.mass);
				if (d.original) {
					if (!d.landed) { d.landed = true; landed++; }
					if (!d.onTongue) { d.onTongue = true; onTongue++; }
				}
			}
			if (d.mat == TM_WATER) {
				// inside, it runs down and stays; on the lip or the outside it breaks up
				if (kind == CPB_INSIDE) { addWater(T, d); return false; }
				if (canBreak(d, speed)) splash(d, speed);
				d.alive = false;
				return false;
			}
			const bool lip = kind == CPB_LIP;
			reflect(d, nx, nh, nz, vn, M.bounce * 0.85f, lip ? 0.f : M.friction, speed > 0.8f);
			if (lip && nh > 0.9f) {
				// on top of the lip: off whichever side it is on, as a round edge does
				float side = qr - (1.f - 0.5f * CP_BOWL_WALL);
				float sgn = side > 1e-4f ? 1.f : side < -1e-4f ? -1.f : (uni() < 0.5f ? -1.f : 1.f);
				d.vx += sgn * 0.1f * ux; d.vz += sgn * 0.1f * uz;
			}
			if (speed < 0.6f) slow = true;
			// rolling round inside: it sustains the bowl
			if (kind == CPB_INSIDE && speed < 0.5f && !d.released) {
				float sp = std::sqrt(d.vx * d.vx + d.vy * d.vy + d.vz * d.vz);
				touchBowl(T, rrel, phi, d.mass, sp);
			}
		}
		return true;
	}
	// A water drop that stays in a bowl raises its level. It adds far more than its
	// real volume, or the change in pitch would take an hour to hear.
	void addWater(TongueVoice& T, CanopyDrop& d) {
		float q = d.r / CP_R0, k = CP_R0 / T.bR;
		T.water = std::min(0.9f, T.water + 0.6f * q * q * q * k * k);
		if (d.original && !d.landed) { d.landed = true; landed++; }
		d.alive = false;
	}

	// ── the wind chimes ──────────────────────────────────────────────────────
	// One tube per note of the scale over one octave, from "Lowest note" an octave
	// up (C4 by default), at most eight, round the ring in order of pitch. A tube's
	// note goes as 1/length^2, so an octave is a tube 0.71 as long.
	void layoutChimes() {
		int size = std::max(sc.size, 1);
		int n = clamp(size, 3, 8);
		nT = n; layoutN = n;
		float lowest = 0.f;
		for (int i = 0; i < n; i++) {
			int k = size <= 8 ? i : (int)std::round((float)i * (float)size / (float)n);
			TongueVoice& T = tv[i];
			T.semis = 12.f * (float)(regOct + 1) + (float)root + sc.degree(k);
			if (i == 0) lowest = T.semis;
			T.len = CP_CH_LEN * std::pow(2.f, -(T.semis - lowest) / 24.f);
			float a = 2.f * (float)M_PI * (float)i / (float)n + 0.3f;
			T.tx = CP_CH_RING * std::sin(a); T.tz = CP_CH_RING * std::cos(a);
			CpPend& P = T.pend;
			P = CpPend();
			P.px = T.tx; P.py = CP_CH_HUB; P.pz = T.tz;
			// a uniform tube on its cord: its mass goes with its length
			float M = 12.f * T.len / CP_CH_LEN, c = CP_CH_STR + 0.5f * T.len;
			P.I = M * (T.len * T.len / 12.f + c * c);
			P.w2 = CP_G * M * c / P.I;
			P.damp = 0.25f;
			float pan = clamp(T.tx / CP_CH_RING, -1.f, 1.f) * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
			T.panL = std::cos(th); T.panR = std::sin(th);
		}
		clap = CpPend();
		clap.py = CP_CH_HUB;
		// a wooden clapper and a thin sail under it
		const float Mc = 8.f, Ms = 2.f;
		clap.I = Mc * CP_CH_CLAP * CP_CH_CLAP + Ms * CP_CH_SAIL * CP_CH_SAIL;
		clap.w2 = CP_G * (Mc * CP_CH_CLAP + Ms * CP_CH_SAIL) / clap.I;
		clap.damp = 0.3f;
	}
	void chimeCoefs(TongueVoice& T, float sr) {
		float f = std::min(dsp::FREQ_C4 * std::pow(2.f, T.semis / 12.f), sr * 0.45f);
		T.freq = f;
		float rShort = std::sqrt(440.f / f);
		for (int m = 0; m < CP_VMODES; m++) { T.lv[m] = 0.f; T.c[m] = 1.f; T.s[m] = 0.f; }
		for (int m = 0; m < CP_CMODES; m++) {
			float fm = f * CP_CH_RATIO[m];
			if (fm >= sr * 0.45f) continue;
			float t60 = std::max(CP_CH_T60[m] * decayK * clamp(rShort, 0.5f, 1.8f), 0.02f);
			float dec1 = std::pow(10.f, -3.f / (t60 * sr));
			float w = 2.f * (float)M_PI * fm / sr;
			T.c[m] = dec1 * std::cos(w); T.s[m] = dec1 * std::sin(w);
			T.lv[m] = CP_CH_GAIN * std::pow(10.f, CP_CH_DB[m] / 20.f);
		}
	}
	// A hit at u along the tube (0 its top, 1 its bottom): each partial gets what its
	// mode shape has there, so a hit near a node of the fundamental is mostly overtones.
	void hitChime(int t, float u, float speed, float contactMs, float radius, float mass) {
		TongueVoice& T = tv[t];
		float tc = 0.001f * contactMs * (radius / CP_R0) * SFS_POWABS(std::max(speed, 0.05f) / CP_V0, -0.2f);
		float n = std::max(tc * coefSr, 2.f);
		float A = SFS_POWABS(mass, 0.35f) * (speed / CP_V0);
		int slot = 0;
		float oldest = -1.f;
		for (int i = 0; i < CP_CONTACTS; i++) if (T.contact[i].ph > oldest) { oldest = T.contact[i].ph; slot = i; }
		Contact& C = T.contact[slot];
		C.ph = 0.f; C.dph = 1.f / n; C.amp = A * 2.f / n;
		for (int m = 0; m < CP_VMODES; m++) C.g[m] = m < CP_CMODES ? cpBeamShape(m, u) * tiltC[m] : 0.f;
		T.awake = true;
		strikes++;
	}
	// Two pendulums meeting at c, n pointing from B to A. Returns how fast they
	// closed, 0 if they were already parting.
	static float pendMeet(CpPend& A, CpPend& B, const float c[3], const float n[3], float e) {
		float rA[3] = {c[0] - A.px, c[1] - A.py, c[2] - A.pz}, rB[3] = {c[0] - B.px, c[1] - B.py, c[2] - B.pz};
		float vA[3], vB[3];
		A.vel(rA, vA); B.vel(rB, vB);
		float vn = (vA[0] - vB[0]) * n[0] + (vA[1] - vB[1]) * n[1] + (vA[2] - vB[2]) * n[2];
		if (vn >= 0.f) return 0.f;
		float k = A.give(rA, n) + B.give(rB, n);
		if (k < 1e-9f) return 0.f;
		float j = -(1.f + e) * vn / k;
		float J[3] = {j * n[0], j * n[1], j * n[2]}, Jn[3] = {-J[0], -J[1], -J[2]};
		A.push(rA, J); B.push(rB, Jn);
		return -vn;
	}
	// A drop meeting a pendulum at c, n pointing from the pendulum to the drop.
	float dropMeet(CanopyDrop& d, CpPend& P, const float c[3], const float n[3], float e) {
		float r[3] = {c[0] - P.px, c[1] - P.py, c[2] - P.pz}, v[3];
		P.vel(r, v);
		float vn = (d.vx - v[0]) * n[0] + (d.vy - v[1]) * n[1] + (d.vz - v[2]) * n[2];
		if (vn >= 0.f) return 0.f;
		if (-vn < 0.5f) e = 0.f;                     // too slow to leave: it rests or rolls
		float j = -(1.f + e) * vn / (1.f / std::max(d.mass, 1e-4f) + P.give(r, n));
		d.vx += j / d.mass * n[0]; d.vy += j / d.mass * n[1]; d.vz += j / d.mass * n[2];
		float Jn[3] = {-j * n[0], -j * n[1], -j * n[2]};
		P.push(r, Jn);
		// a little sliding friction against the surface
		float vt[3] = {d.vx - v[0], d.vy - v[1], d.vz - v[2]};
		float vtn = vt[0] * n[0] + vt[1] * n[1] + vt[2] * n[2];
		float keep = 1.f - 0.1f * CP_MAT[d.mat].friction;
		d.vx = v[0] + (vt[0] - vtn * n[0]) * keep + vtn * n[0];
		d.vy = v[1] + (vt[1] - vtn * n[1]) * keep + vtn * n[1];
		d.vz = v[2] + (vt[2] - vtn * n[2]) * keep + vtn * n[2];
		return -vn;
	}
	// The pendulums swing, the clapper meets the tubes and the tubes meet each other.
	void stepChimes(float dt) {
		clap.step(dt);
		for (int t = 0; t < nT; t++) tv[t].pend.step(dt);
		const CanopyMat& W = CP_MAT[TM_WOOD];
		float C[3]; clap.at(CP_CH_CLAP, C);
		for (int t = 0; t < nT; t++) {
			TongueVoice& T = tv[t];
			float a[3], b[3], q[3];
			T.pend.at(CP_CH_STR, a); T.pend.at(CP_CH_STR + T.len, b);
			float u = cpSegNear(a, b, C, q);
			float n[3] = {C[0] - q[0], C[1] - q[1], C[2] - q[2]};
			float dist = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), reach = CP_CH_CLR + CP_CH_TR;
			if (dist >= reach || dist < 1e-6f) continue;
			for (float& x : n) x /= dist;
			float c[3] = {q[0] + n[0] * CP_CH_TR, q[1] + n[1] * CP_CH_TR, q[2] + n[2] * CP_CH_TR};
			float rc[3] = {c[0] - clap.px, c[1] - clap.py, c[2] - clap.pz}, rt[3] = {c[0] - T.pend.px, c[1] - T.pend.py, c[2] - T.pend.pz};
			float pen = reach - dist;
			clap.shift(rc, n, 0.5f * pen);
			float nn[3] = {-n[0], -n[1], -n[2]};
			T.pend.shift(rt, nn, 0.5f * pen);
			float sp = pendMeet(clap, T.pend, c, n, 0.7f);
			// the clapper is the chimes' own voice: a heavy wooden puck, 30 drops' weight
			if (sp > 0.02f) hitChime(t, u, sp, W.contactMs, CP_R0 * 1.5f, 30.f);
		}
		for (int i = 0; i < nT; i++) for (int k = i + 1; k < nT; k++) {
			TongueVoice& A = tv[i]; TongueVoice& B = tv[k];
			float a0[3], a1[3], b0[3], b1[3];
			A.pend.at(CP_CH_STR, a0); A.pend.at(CP_CH_STR + A.len, a1);
			B.pend.at(CP_CH_STR, b0); B.pend.at(CP_CH_STR + B.len, b1);
			float sa, sb;
			cpSegSeg(a0, a1, b0, b1, sa, sb);
			float pa[3], pb[3], n[3];
			for (int j = 0; j < 3; j++) { pa[j] = a0[j] + sa * (a1[j] - a0[j]); pb[j] = b0[j] + sb * (b1[j] - b0[j]); n[j] = pa[j] - pb[j]; }
			float dist = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (dist >= 2.f * CP_CH_TR || dist < 1e-6f) continue;
			for (float& x : n) x /= dist;
			float c[3] = {0.5f * (pa[0] + pb[0]), 0.5f * (pa[1] + pb[1]), 0.5f * (pa[2] + pb[2])};
			float ra[3] = {c[0] - A.pend.px, c[1] - A.pend.py, c[2] - A.pend.pz}, rb[3] = {c[0] - B.pend.px, c[1] - B.pend.py, c[2] - B.pend.pz};
			float pen = 2.f * CP_CH_TR - dist, nn[3] = {-n[0], -n[1], -n[2]};
			A.pend.shift(ra, n, 0.5f * pen); B.pend.shift(rb, nn, 0.5f * pen);
			float sp = pendMeet(A.pend, B.pend, c, n, 0.6f);
			if (sp > 0.02f) {
				// metal on metal: a short contact
				hitChime(i, sa, sp, 0.3f, CP_R0, 3.f);
				hitChime(k, sb, sp, 0.3f, CP_R0, 3.f);
			}
		}
	}
	// One step of a drop against the chimes. Returns false if it has gone.
	bool dropChimes(CanopyDrop& d, bool& slow) {
		const CanopyMat& M = CP_MAT[d.mat];
		float X[3] = {d.x, d.y, d.z};
		// the tubes
		for (int t = 0; t < nT; t++) {
			TongueVoice& T = tv[t];
			float a[3], b[3], q[3];
			T.pend.at(CP_CH_STR, a); T.pend.at(CP_CH_STR + T.len, b);
			float u = cpSegNear(a, b, X, q);
			float n[3] = {X[0] - q[0], X[1] - q[1], X[2] - q[2]};
			float dist = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), reach = d.r + CP_CH_TR;
			if (dist >= reach || dist < 1e-6f) continue;
			for (float& x : n) x /= dist;
			d.x = q[0] + n[0] * reach; d.y = q[1] + n[1] * reach; d.z = q[2] + n[2] * reach;
			float c[3] = {q[0] + n[0] * CP_CH_TR, q[1] + n[1] * CP_CH_TR, q[2] + n[2] * CP_CH_TR};
			float sp = dropMeet(d, T.pend, c, n, M.bounce);
			if (sp >= CP_MIN_STRIKE && !d.released) {
				hitChime(t, u, sp, M.contactMs, d.r, d.mass);
				if (d.original) {
					if (!d.landed) { d.landed = true; landed++; }
					if (!d.onTongue) { d.onTongue = true; onTongue++; }
				}
			}
			if (d.mat == TM_WATER && sp > 0.f) {
				if (canBreak(d, sp)) splash(d, sp);
				d.alive = false;
				return false;
			}
			if (sp > 0.f && sp < 0.6f) slow = true;
		}
		// the clapper, a wooden ball, and the sail, a thin disc under it: both silent,
		// both swing the cord they hang from
		{
			float C[3]; clap.at(CP_CH_CLAP, C);
			float n[3] = {d.x - C[0], d.y - C[1], d.z - C[2]};
			float dist = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), reach = d.r + CP_CH_CLR;
			if (dist < reach && dist > 1e-6f) {
				for (float& x : n) x /= dist;
				d.x = C[0] + n[0] * reach; d.y = C[1] + n[1] * reach; d.z = C[2] + n[2] * reach;
				float c[3] = {C[0] + n[0] * CP_CH_CLR, C[1] + n[1] * CP_CH_CLR, C[2] + n[2] * CP_CH_CLR};
				float sp = dropMeet(d, clap, c, n, M.bounce * 0.7f);
				if (d.mat == TM_WATER && sp > 0.f) { if (canBreak(d, sp)) splash(d, sp); d.alive = false; return false; }
				if (sp > 0.f && sp < 0.6f) slow = true;
			}
		}
		{
			float S[3], up[3];
			clap.at(CP_CH_SAIL, S); clap.dir(up);
			for (float& x : up) x = -x;
			float rel[3] = {d.x - S[0], d.y - S[1], d.z - S[2]};
			float h = rel[0] * up[0] + rel[1] * up[1] + rel[2] * up[2];
			float rad[3] = {rel[0] - h * up[0], rel[1] - h * up[1], rel[2] - h * up[2]};
			float rho = std::sqrt(rad[0] * rad[0] + rad[1] * rad[1] + rad[2] * rad[2]);
			if (rho < CP_CH_SR && std::fabs(h) < d.r + CP_CH_ST) {
				float sg = h >= 0.f ? 1.f : -1.f;
				float n[3] = {sg * up[0], sg * up[1], sg * up[2]};
				float to = sg * (d.r + CP_CH_ST) - h;
				d.x += up[0] * to; d.y += up[1] * to; d.z += up[2] * to;
				float c[3] = {S[0] + rad[0] + sg * CP_CH_ST * up[0], S[1] + rad[1] + sg * CP_CH_ST * up[1], S[2] + rad[2] + sg * CP_CH_ST * up[2]};
				float sp = dropMeet(d, clap, c, n, M.bounce * 0.5f);
				if (d.mat == TM_WATER && sp > 0.f) { if (canBreak(d, sp)) splash(d, sp); d.alive = false; return false; }
				if (sp > 0.f && sp < 0.6f) slow = true;
			}
		}
		return true;
	}

	// ── the pads on the steel pan ─────────────────────────────────────────────
	// A low-C tenor pan: C4 to E6, 29 notes. The lowest octave is the outer ring,
	// the next the middle ring, the last five sit in the centre. Round each ring
	// the notes go in fifths, so a note's neighbours are a fifth and a fourth from
	// it and its octave lies straight inward. Tuned to the scale, each ring holds
	// the scale's notes in order instead.
	float surfaceAt(float x, float z) const {
		if (instNow == 3) {
			// the floor of a bowl through its mouth, the lip round it, else the table
			for (int t = 0; t < nT; t++) {
				const TongueVoice& T = tv[t];
				float rho = std::sqrt((x - T.tx) * (x - T.tx) + (z - T.tz) * (z - T.tz)) / T.bR;
				if (rho < 1.f - CP_BOWL_WALL) return T.bY - T.bD + CP_BOWL_BASE * T.bR;
				if (rho < CP_BOWL_BELLY) return T.bY;
			}
			return CP_BOWL_FLOOR;
		}
		if (instNow == 4) return CP_CH_HUB - CP_CH_SAIL - 0.05f;     // nothing below the chimes: the sail's level
		if (instNow == 5) return (x * x + z * z) <= 1.f ? handY(x, z) : CP_HP_H0 - CP_HP_UNDER;
		if (instNow == 6) {
			for (int t = 0; t < nT; t++) {
				const TongueVoice& T = tv[t];
				float r = std::sqrt((x - T.tx) * (x - T.tx) + (z - T.tz) * (z - T.tz));
				if (r < T.bR) return kettleY(T, r / T.bR);
			}
			return CP_BN_BASE;
		}
		if (instNow == 7 || instNow == 8) {
			float u;
			int t = barAt(x, z, u);
			return t >= 0 ? tv[t].xy : CP_X_Y - 0.5f;
		}
		return instNow == 1 ? cpBoardY(z) : instNow == 2 ? cpPanY(x, z) : cpDomeY(x, z);
	}
	int panSignature() const {
		int h = panTuning * 7919 + root * 131 + (regOct + 3) * 17 + sc.size * 3;
		for (int k = 0; k < sc.size; k++) h = h * 31 + (int)std::round(sc.intervals[k] * 100.f);
		return h;
	}
	void layoutPan() {
		struct Pad { float semis; int ring; float ang; bool damped; };
		Pad pads[CP_MAXN];
		int n = 0;
		bool inKey[12] = {};
		if (sc.size <= 0) for (bool& b : inKey) b = true;
		for (int k = 0; k < sc.size; k++) inKey[(((int)std::round(sc.intervals[k]) + root) % 12 + 12) % 12] = true;
		const float TAU = 2.f * (float)M_PI;
		if (panTuning == PAN_SCALE) {
			int sz = std::max(sc.size, 1);
			for (int k = 0; n < CP_MAXN; k++) {
				float above = sc.degree(k);                  // above the root
				if (above > (float)CP_PAN_SPAN + 0.5f) break;
				int ring = std::min((int)std::floor(above / 12.f + 1e-3f), 2);
				pads[n++] = {(float)root + above, ring, TAU * (float)(k % sz) / (float)sz, false};
			}
		}
		else {
			for (int st = 0; st <= CP_PAN_SPAN && n < CP_MAXN; st++) {
				int pc = st % 12;
				pads[n++] = {(float)st, std::min(st / 12, 2), TAU * (float)((pc * 7) % 12) / 12.f,
				             panTuning == PAN_DAMPED && !inKey[pc]};
			}
		}
		nT = n; layoutN = n;
		// the rings: outer 0.64 to 0.98 of the radius, middle 0.34 to 0.62, the centre within 0.32
		static const float RC[3] = {0.81f, 0.48f, 0.17f}, HALF[3] = {0.165f, 0.135f, 0.13f};
		int cnt[3] = {};
		for (int i = 0; i < n; i++) cnt[pads[i].ring]++;
		for (int i = 0; i < n; i++) {
			TongueVoice& T = tv[i];
			const Pad& P = pads[i];
			int rg = P.ring;
			float rc = (rg == 2 && cnt[2] == 1) ? 0.f : RC[rg];
			// the smallest gap to a pad in the same ring sets how wide it may be
			float gap = TAU;
			for (int j = 0; j < n; j++) {
				if (j == i || pads[j].ring != rg) continue;
				float d = std::fabs(pads[j].ang - P.ang);
				d = std::min(d, TAU - d);
				gap = std::min(gap, d);
			}
			T.semis = 12.f * (float)(regOct + 1) + P.semis;   // "Lowest note" moves the pan by octaves; C4 by default
			T.ring = rg; T.damped = P.damped; T.pth = P.ang;
			T.tx = rc * std::sin(P.ang); T.tz = rc * std::cos(P.ang);   // C at the front, toward the player
			T.pa = HALF[rg] * 0.9f;
			T.pb = rc > 0.f ? std::min(rc * gap * 0.5f * 0.86f, HALF[rg] * 1.4f) : T.pa;
			float pan = clamp(T.tx, -1.f, 1.f) * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
			T.panL = std::cos(th); T.panR = std::sin(th);
		}
		// pads whose partials meet: a fifth (3f against 2f), an octave, a twelfth
		for (int i = 0; i < n; i++) {
			TongueVoice& T = tv[i];
			T.nsym = 0;
			for (int j = 0; j < n && T.nsym < 6; j++) {
				if (j == i) continue;
				float d = std::fabs(tv[j].semis - T.semis);
				if (std::fabs(d - 7.02f) < 0.25f || std::fabs(d - 12.f) < 0.25f || std::fabs(d - 19.02f) < 0.25f)
					T.sym[T.nsym++] = j;
			}
		}
	}
	// ── the marimba and the balafon ──────────────────────────────────────────
	bool balafon() const { return instNow == 8; }
	void layoutBars() {
		const float TAU = 0.f; (void)TAU;
		if (!balafon()) {
			// two octaves from "Lowest note": 15 naturals across, the sharps between and behind
			bool inKey[12] = {};
			if (sc.size <= 0) for (bool& b : inKey) b = true;
			for (int k = 0; k < sc.size; k++) inKey[(((int)std::round(sc.intervals[k]) + root) % 12 + 12) % 12] = true;
			static const bool SHARP[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
			// the naturals round the circle, one place left empty at the front between
			// the lowest and the highest; the sharps between their two naturals
			int nat = 0;
			for (int st = 0; st <= 24; st++) if (!SHARP[st % 12]) nat++;
			const float step = 2.f * (float)M_PI / (float)(nat + 1);
			const float R_NAT = 0.42f, R_SH = 0.24f, LMAX = 0.56f;
			int n = 0, ni = 0;
			for (int st = 0; st <= 24 && n < CP_MAXN; st++) {
				TongueVoice& T = tv[n++];
				bool sh = SHARP[st % 12];
				T.semis = 12.f * (float)regOct + (float)st;
				T.damped = !inKey[st % 12];
				// angle 0 is toward the player; the notes rise clockwise from just past it
				float ang = step * ((float)ni + (sh ? 0.f : 0.5f)) + 0.5f * step;
				if (!sh) ni++;
				T.len = LMAX * std::pow(2.f, -(float)st / 24.f) * (sh ? 0.72f : 1.f);
				T.xw = sh ? 0.045f : 0.068f;
				float r0 = sh ? R_SH : R_NAT;
				T.ux = -std::sin(ang); T.uz = std::cos(ang);         // outward along the spoke
				T.tx = T.ux * (r0 + 0.5f * T.len); T.tz = T.uz * (r0 + 0.5f * T.len);
				T.xy = CP_X_Y + (sh ? CP_X_RAISE : 0.f);
				T.ring = sh ? 1 : 0;
				float pan = clamp(T.tx, -1.f, 1.f) * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
				T.panL = std::cos(th); T.panR = std::sin(th);
			}
			nT = n; layoutN = n;
			return;
		}
		// the balafon: the scale's notes in one row, two octaves of a scale of up to eight
		int n = tongueCount(sc.size);
		nT = n; layoutN = n;
		float pitch = 2.f * CP_X_W / (float)n;
		for (int i = 0; i < n; i++) {
			TongueVoice& T = tv[i];
			T.semis = 12.f * (float)regOct + (float)root + sc.degree(i);
			T.damped = false; T.ring = 0;
			T.len = CP_X_LEN * std::pow(2.f, -(T.semis - tv[0].semis) / 24.f);
			if (i == 0) T.len = CP_X_LEN;
			T.xw = 0.5f * pitch - 0.5f * CP_X_GAP;
			T.tx = -CP_X_W + pitch * ((float)i + 0.5f);
			T.xy = CP_X_Y;
			T.tz = 0.42f - 0.5f * T.len;
			T.ux = 0.f; T.uz = 1.f;                               // along the bar, toward the player
			float pan = clamp(T.tx / CP_X_W, -1.f, 1.f) * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
			T.panL = std::cos(th); T.panR = std::sin(th);
		}
	}
	void barCoefs(TongueVoice& T, float sr) {
		float f = std::min(dsp::FREQ_C4 * std::pow(2.f, T.semis / 12.f), sr * 0.45f);
		T.freq = f;
		float rShort = std::sqrt(440.f / f);
		const float* RATIO = balafon() ? CP_BAL_RATIO : CP_MAR_RATIO;
		for (int m = 0; m < CP_VMODES; m++) { T.lv[m] = 0.f; T.c[m] = 1.f; T.s[m] = 0.f; }
		for (int m = 0; m < CP_XMODES; m++) {
			float fm = f * RATIO[m];
			if (fm >= sr * 0.45f) continue;
			// a damped bar: a hand on it, so it knocks and is gone
			float t60 = std::max(CP_X_T60[m] * decayK * clamp(rShort, 0.5f, 2.2f) / (T.damped ? 8.f : 1.f), 0.02f);
			float dec1 = std::pow(10.f, -3.f / (t60 * sr));
			float w = 2.f * (float)M_PI * fm / sr;
			T.c[m] = dec1 * std::cos(w); T.s[m] = dec1 * std::sin(w);
			T.lv[m] = (balafon() ? 0.85f : 1.6f) * std::pow(10.f, CP_X_DB[m] / 20.f) * (T.damped ? 0.3f : 1.f);
		}
		T.buzzN = 0.f; T.buzzB = 0.f;
	}
	// a hit at u along the bar (0 its far end, 1 its near one): each mode gets what the
	// free bar's mode shape has there, so the middle has no second mode
	void hitBar(int t, float u, float speed, int mat, float radius, float mass) {
		const CanopyMat& M = CP_MAT[mat];
		TongueVoice& T = tv[t];
		float tc = 0.001f * M.contactMs * (radius / CP_R0) * SFS_POWABS(std::max(speed, 0.05f) / CP_V0, -0.2f);
		float n = std::max(tc * coefSr, 2.f);
		float A = SFS_POWABS(mass, 0.35f) * (speed / CP_V0);
		int slot = 0;
		float oldest = -1.f;
		for (int i = 0; i < CP_CONTACTS; i++) if (T.contact[i].ph > oldest) { oldest = T.contact[i].ph; slot = i; }
		Contact& C = T.contact[slot];
		C.ph = 0.f; C.dph = 1.f / n; C.amp = A * 2.f / n;
		// the bar's modes in order are the free beam's first, second and third
		for (int m = 0; m < CP_VMODES; m++) C.g[m] = m < CP_XMODES ? cpBeamShape(m, u) * tiltX[m] : 0.f;
		T.awake = true;
		strikes++;
	}
	// the bar under (x, z), the higher first where two overlap, or -1
	int barAt(float x, float z, float& u) const {
		int best = -1; float top = -9.f;
		for (int t = 0; t < nT; t++) {
			const TongueVoice& T = tv[t];
			float rx = x - T.tx, rz = z - T.tz;
			float along = rx * T.ux + rz * T.uz, across = rx * T.uz - rz * T.ux;
			if (std::fabs(across) > T.xw || std::fabs(along) > 0.5f * T.len) continue;
			if (T.xy > top) { top = T.xy; best = t; u = along / T.len + 0.5f; }
		}
		return best;
	}
	bool stepBars(CanopyDrop& d, bool& slow) {
		const CanopyMat& M = CP_MAT[d.mat];
		if (d.released) {
			d.fade -= CP_DT / 0.4f;
			if (d.fade <= 0.f) { d.alive = false; return false; }
		}
		float u = 0.5f;
		int t = barAt(d.x, d.z, u);
		if (t < 0) return true;                      // between the bars: it falls through
		const TongueVoice& T = tv[t];
		if (d.y - d.r > T.xy || d.y < T.xy - CP_X_TH - d.r) return true;
		if (d.vy >= 0.f) return true;
		float vn = d.vy, speed = -vn;
		d.y = T.xy + d.r;
		if (speed >= CP_MIN_STRIKE && !d.released) {
			hitBar(t, u, speed, d.mat, d.r, d.mass);
			if (d.original) {
				if (!d.landed) { d.landed = true; landed++; }
				if (!T.damped && !d.onTongue) { d.onTongue = true; onTongue++; }
			}
		}
		if (d.mat == TM_WATER) { if (canBreak(d, speed)) splash(d, speed); d.alive = false; return false; }
		reflect(d, 0.f, 1.f, 0.f, vn, M.bounce, M.friction, speed > 0.8f);
		if (speed < 0.6f) slow = true;
		return true;
	}

	// The bonang and the bars have gaps a drop thrown by a leaf can fall through,
	// where the drum is solid. In the last stretch of its fall, a drop in flight is
	// drawn sideways toward the nearest kettle or bar, the nearer the stronger.
	void steer(CanopyDrop& d) {
		float best = 1e9f, gx = 0.f, gz = 0.f, top = 0.f;
		for (int t = 0; t < nT; t++) {
			const TongueVoice& T = tv[t];
			float px, pz, y;
			if (instNow == 6) { px = T.tx; pz = T.tz; y = T.bY; }
			else {
				float a = clamp((d.x - T.tx) * T.ux + (d.z - T.tz) * T.uz, -0.4f * T.len, 0.4f * T.len);
				px = T.tx + a * T.ux; pz = T.tz + a * T.uz; y = T.xy;
			}
			float dd = (d.x - px) * (d.x - px) + (d.z - pz) * (d.z - pz);
			if (dd < best) { best = dd; gx = px; gz = pz; top = y; }
		}
		float h = d.y - top;
		if (best > 0.09f || h < 0.f || h > 0.7f) return;
		// a pull toward it of 84 per second per unit of distance at the target's height,
		// fading to nothing 0.7 above it
		float k = 84.f * (1.f - h / 0.7f) * CP_DT;
		d.vx += (gx - d.x) * k; d.vz += (gz - d.z) * k;
	}

	// ── the bonang ───────────────────────────────────────────────────────────
	// The near row the scale over one octave from "Lowest note" an octave up (C4 by
	// default), the far row the octave above; at most seven a row, as a pelog bonang
	// has. bR is a kettle's radius and bY the height of its top.
	void layoutBonang() {
		int size = std::max(sc.size, 1);
		int per = clamp(size, 3, 7);
		nT = 2 * per; layoutN = nT;
		float lowest = 0.f;
		for (int i = 0; i < nT; i++) {
			int row = i / per, j = i % per;
			int k = size <= 7 ? j : (int)std::round((float)j * (float)size / (float)per);
			TongueVoice& T = tv[i];
			T.semis = 12.f * (float)(regOct + 1 + row) + (float)root + sc.degree(k);
			if (i == 0) lowest = T.semis;
			// as wide as the row leaves room for, the higher a little smaller
			float span0 = 2.f * (CP_BN_W - 0.16f) / (float)per;
			T.bR = std::min(CP_BN_KR, 0.48f * span0) * std::pow(2.f, -(T.semis - lowest) / 48.f);
			T.bD = CP_BN_KH * T.bR / CP_BN_KR;
			T.bY = CP_BN_BASE + T.bD;
			T.bAx = 0.9f * (float)i;
			float span = 2.f * (CP_BN_W - 0.16f);
			T.tx = -0.5f * span + span * ((float)j + 0.5f) / (float)per;
			T.tz = row == 0 ? CP_BN_ROWZ : -CP_BN_ROWZ;
			float pan = clamp(T.tx / CP_BN_W, -1.f, 1.f) * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
			T.panL = std::cos(th); T.panR = std::sin(th);
		}
	}
	void bonangCoefs(TongueVoice& T, float sr) {
		float f = std::min(dsp::FREQ_C4 * std::pow(2.f, T.semis / 12.f), sr * 0.45f);
		T.freq = f;
		float rShort = std::sqrt(440.f / f);
		for (int m = 0; m < CP_VMODES; m++) { T.lv[m] = 0.f; T.c[m] = 1.f; T.s[m] = 0.f; }
		for (int m = 0; m < CP_BNMODES; m++) {
			float fm = f * CP_BN_RATIO[m];
			if (fm >= sr * 0.45f) continue;
			float t60 = std::max(CP_BN_T60[m] * decayK * clamp(rShort, 0.5f, 1.8f), 0.02f);
			float dec1 = std::pow(10.f, -3.f / (t60 * sr));
			float w = 2.f * (float)M_PI * fm / sr;
			T.c[m] = dec1 * std::cos(w); T.s[m] = dec1 * std::sin(w);
			T.lv[m] = 1.6f * std::pow(10.f, CP_BN_DB[m] / 20.f);
		}
	}
	// a kettle's surface at u of its radius out: the boss, the flat top, the shoulder
	static float kettleY(const TongueVoice& T, float u) {
		if (u < CP_BN_BOSS) return T.bY + CP_BN_BH * T.bR * 0.5f * (1.f + std::cos((float)M_PI * u / CP_BN_BOSS));
		if (u < 0.8f) return T.bY;
		float v = std::min((u - 0.8f) / 0.2f, 1.f);
		return T.bY - 0.3f * T.bR * (1.f - std::sqrt(std::max(1.f - v * v, 0.f)));
	}
	// The hit, by where on the kettle: the boss is all fundamental, the top and the
	// shoulder progressively less of it and more of what is above.
	void hitKettle(int t, float u, float speed, int mat, float radius, float mass) {
		const CanopyMat& M = CP_MAT[mat];
		TongueVoice& T = tv[t];
		float tc = 0.001f * M.contactMs * (radius / CP_R0) * SFS_POWABS(std::max(speed, 0.05f) / CP_V0, -0.2f);
		float n = std::max(tc * coefSr, 2.f);
		float A = SFS_POWABS(mass, 0.35f) * (speed / CP_V0);
		float off = clamp((u - 0.5f * CP_BN_BOSS) / (0.8f - 0.5f * CP_BN_BOSS), 0.f, 1.f);   // 0 on the boss, 1 from the top's edge out
		int slot = 0;
		float oldest = -1.f;
		for (int i = 0; i < CP_CONTACTS; i++) if (T.contact[i].ph > oldest) { oldest = T.contact[i].ph; slot = i; }
		Contact& C = T.contact[slot];
		C.ph = 0.f; C.dph = 1.f / n; C.amp = A * 2.f / n;
		static const float BOSS[CP_BNMODES] = {1.f, 1.f, 0.45f, 0.3f, 0.2f, 0.15f};
		static const float EDGE[CP_BNMODES] = {0.04f, 0.05f, 0.9f, 0.9f, 0.8f, 0.7f};
		for (int m = 0; m < CP_VMODES; m++) C.g[m] = m < CP_BNMODES ? (BOSS[m] + (EDGE[m] - BOSS[m]) * off) * tiltBn[m] : 0.f;
		T.awake = true;
		strikes++;
	}
	// One step of a drop against the bonang: the kettles, their sides, and the cords
	// and frame under them, which are silent.
	bool stepBonang(CanopyDrop& d, bool& slow) {
		const CanopyMat& M = CP_MAT[d.mat];
		if (d.released) {
			d.fade -= CP_DT / 0.4f;
			if (d.fade <= 0.f) { d.alive = false; return false; }
		}
		for (int t = 0; t < nT; t++) {
			TongueVoice& T = tv[t];
			float dx = d.x - T.tx, dz = d.z - T.tz, r = std::sqrt(dx * dx + dz * dz);
			if (r > T.bR + d.r) continue;
			float ux = r > 1e-5f ? dx / r : 1.f, uz = r > 1e-5f ? dz / r : 0.f;
			const float shoulder = T.bY - 0.3f * T.bR;
			if (r >= T.bR) {
				// against the side
				if (d.y > shoulder || d.y < CP_BN_BASE) continue;
				float vn = d.vx * ux + d.vz * uz;
				d.x = T.tx + ux * (T.bR + d.r); d.z = T.tz + uz * (T.bR + d.r);
				if (vn < 0.f) {
					if (-vn >= CP_MIN_STRIKE && !d.released) hitKettle(t, 1.f, -vn, d.mat, d.r, d.mass);
					if (d.mat == TM_WATER) { if (canBreak(d, -vn)) splash(d, -vn); d.alive = false; return false; }
					reflect(d, ux, 0.f, uz, vn, M.bounce, M.friction, false);
				}
				continue;
			}
			float u = r / T.bR;
			float surface = kettleY(T, u);
			if (d.y - d.r > surface || d.y < surface - 0.2f) continue;
			const float e = 0.004f;
			float sp = kettleY(T, std::min((r + e) / T.bR, 1.f)), sm = kettleY(T, std::max(r - e, 0.f) / T.bR);
			float slope = (sp - sm) / (2.f * e);
			float nl = 1.f / std::sqrt(1.f + slope * slope);
			float nx = -slope * nl * ux, nz = -slope * nl * uz;
			float vn = d.vx * nx + d.vy * nl + d.vz * nz;
			if (vn >= 0.f) continue;
			float speed = -vn;
			d.y = surface + d.r;
			if (speed >= CP_MIN_STRIKE && !d.released) {
				hitKettle(t, u, speed, d.mat, d.r, d.mass);
				if (d.original) {
					if (!d.landed) { d.landed = true; landed++; }
					if (u < 0.8f && !d.onTongue) { d.onTongue = true; onTongue++; }
				}
			}
			if (d.mat == TM_WATER) { if (canBreak(d, speed)) splash(d, speed); d.alive = false; return false; }
			reflect(d, nx, nl, nz, vn, M.bounce, M.friction, speed > 0.8f);
			if (speed < 0.6f) slow = true;
		}
		// the cords and the frame's floor
		if (std::fabs(d.x) < CP_BN_W && std::fabs(d.z) < CP_BN_D && d.y - d.r < CP_BN_BASE && d.y > CP_BN_BASE - 0.15f && d.vy < 0.f) {
			if (d.mat == TM_WATER) { d.alive = false; return false; }
			float vn = d.vy;
			d.y = CP_BN_BASE + d.r;
			reflect(d, 0.f, 1.f, 0.f, vn, 0.35f * M.bounce, M.friction, false);
			// down among the cords it has nothing left to play: it fades where it lands
			d.released = true;
		}
		return true;
	}

	// the handpan: the ding is voice 0, the fields 1 to 8 in order of pitch
	void layoutHand() {
		int size = std::max(sc.size, 1);
		nT = 1 + CP_HP_FIELDS; layoutN = nT;
		float base = 12.f * (float)regOct + (float)root;            // C3 by default
		int k = 0;
		while (sc.degree(k) < 7.f - 1e-3f && k < 4 * size) k++;     // the first note a fifth up
		for (int i = 0; i < nT; i++) {
			TongueVoice& T = tv[i];
			T.ring = i == 0 ? 2 : 0; T.damped = false;
			if (i == 0) {
				T.semis = base;
				T.tx = 0.f; T.tz = 0.f; T.pth = 0.f; T.pa = T.pb = CP_HP_DR;
			}
			else {
				T.semis = base + sc.degree(k + i - 1);
				int f = i - 1;
				float ang = (0.3927f + 0.7854f * (float)(f / 2)) * (f % 2 == 0 ? -1.f : 1.f);   // 22.5 + 45 degrees a step
				T.pth = ang;
				T.tx = CP_HP_RING * std::sin(ang); T.tz = CP_HP_RING * std::cos(ang);
				float kk = std::pow(2.f, -(T.semis - tv[1].semis) / 30.f);
				if (i == 1) kk = 1.f;
				T.pa = 0.19f * kk; T.pb = 0.16f * kk;      // a lower note is a bigger field
			}
			float pan = clamp(T.tx, -1.f, 1.f) * 0.75f, th = (pan + 1.f) * 0.25f * (float)M_PI;
			T.panL = std::cos(th); T.panR = std::sin(th);
		}
		for (int i = 0; i < nT; i++) {
			TongueVoice& T = tv[i];
			T.nsym = 0;
			for (int j = 0; j < nT && T.nsym < 6; j++) {
				if (j == i) continue;
				float d = std::fabs(tv[j].semis - T.semis);
				if (std::fabs(d - 7.02f) < 0.25f || std::fabs(d - 12.f) < 0.25f || std::fabs(d - 19.02f) < 0.25f)
					T.sym[T.nsym++] = j;
			}
		}
	}
	float handY(float x, float z) const {
		float r2 = x * x + z * z;
		float y = CP_HP_H0 + CP_HP_DOME * (1.f - std::min(r2, 1.f));
		if (r2 < CP_HP_DR * CP_HP_DR) y += CP_HP_DH * (1.f - r2 / (CP_HP_DR * CP_HP_DR));
		for (int t = 1; t < nT; t++) {
			const TongueVoice& T = tv[t];
			float rx = x - T.tx, rz = z - T.tz;
			float ux = std::sin(T.pth), uz = std::cos(T.pth);
			float ur = rx * ux + rz * uz, ut = rx * uz - rz * ux;
			float q2 = (ur / T.pa) * (ur / T.pa) + (ut / T.pb) * (ut / T.pb);
			if (q2 < 1.f) y -= CP_HP_DENT * (1.f - q2);
		}
		return y;
	}
	// One step of a drop against the handpan. The shell is convex, so a drop rolls
	// across the fields and off the edge; past the rim it falls away.
	bool stepHand(CanopyDrop& d, bool& slow) {
		const CanopyMat& M = CP_MAT[d.mat];
		if (d.x * d.x + d.z * d.z > 1.f) return true;
		float surface = handY(d.x, d.z);
		if (d.y - d.r > surface || d.y < surface - 0.3f) return true;
		const float e = 0.004f;
		float gx = (handY(d.x + e, d.z) - handY(d.x - e, d.z)) / (2.f * e);
		float gz = (handY(d.x, d.z + e) - handY(d.x, d.z - e)) / (2.f * e);
		float nl = 1.f / std::sqrt(1.f + gx * gx + gz * gz);
		float nx = -gx * nl, nz = -gz * nl;
		float vn = d.vx * nx + d.vy * nl + d.vz * nz;
		if (vn >= 0.f) return true;
		float speed = -vn;
		d.y = surface + d.r;
		if (speed >= CP_MIN_STRIKE && !d.released) {
			float rho;
			int t = padAt(d.x, d.z, rho);
			hitPanAt(t, rho, speed, d.mat, d.r, d.mass);
			d.pad = -2;
			if (d.original) {
				if (!d.landed) { d.landed = true; landed++; }
				if (rho <= 1.f && !d.onTongue) { d.onTongue = true; onTongue++; }
			}
		}
		if (d.mat == TM_WATER) {
			if (canBreak(d, speed)) splash(d, speed);
			d.alive = false;
			return false;
		}
		reflect(d, nx, nl, nz, vn, M.bounce, M.friction, speed > 0.8f);
		if (speed < 0.6f) slow = true;
		if (speed < 0.5f) rollPan(d);
		return true;
	}
	void panCoefs(TongueVoice& T, float sr) {
		float f = std::min(dsp::FREQ_C4 * std::pow(2.f, T.semis / 12.f), sr * 0.45f);
		T.freq = f;
		float rShort = std::sqrt(440.f / f);
		for (int m = 0; m < CP_PMODES; m++) {
			float fm = f * CP_PAN_RATIO[m];
			if (fm >= sr * 0.45f) { T.lv[m] = 0.f; T.c[m] = 1.f; T.s[m] = 0.f; continue; }
			// a damped note: a hand resting on it, so it thuds and is gone
			float t60 = std::max(CP_PAN_T60[m] * decayK * clamp(rShort, 0.5f, 1.6f) / (T.damped ? 8.f : 1.f), 0.02f);
			if (instNow == 5) t60 *= 2.6f;          // a handpan sustains far longer than a pan
			float dec1 = std::pow(10.f, -3.f / (t60 * sr));
			float w = 2.f * (float)M_PI * fm / sr;
			T.c[m] = dec1 * std::cos(w); T.s[m] = dec1 * std::sin(w);
			// 1.5: level with the drum at the default settings, where SCALE damps over half the pads
			T.lv[m] = 1.5f * std::pow(10.f, CP_PAN_DB[m] / 20.f) * (T.damped ? 0.3f : 1.f);
			// a handpan rings far longer, so its tails stack: half the pan's level
			if (instNow == 5) T.lv[m] *= 0.5f * (m == 1 ? 1.25f : m == 2 ? 1.5f : 0.8f);
		}
	}
	// rho: where on the pad, 0 its middle, 1 its edge
	void startContactP(int t, float A, float n, float rho) {
		TongueVoice& T = tv[t];
		int slot = 0;
		float oldest = -1.f;
		for (int i = 0; i < CP_CONTACTS; i++) if (T.contact[i].ph > oldest) { oldest = T.contact[i].ph; slot = i; }
		Contact& C = T.contact[slot];
		C.ph = 0.f;
		C.dph = 1.f / n;
		C.amp = A * 2.f / n;
		float r = clamp(rho, 0.f, 1.f);
		float g[CP_PMODES] = {0.35f + 0.65f * std::cos(r * 1.5708f), 0.3f + 0.7f * r, 0.25f + 0.75f * r * r,
		                      0.15f + 0.85f * r, 0.15f + 0.85f * r, 0.15f + 0.85f * r};
		for (int m = 0; m < CP_PMODES; m++) C.g[m] = g[m] * tiltP[m];
		T.awake = true;
	}
	// which pad is under (x, z), and how far out on it (rho > 1 is off it)
	int padAt(float x, float z, float& rho) const {
		int best = -1; rho = 1e9f;
		for (int t = 0; t < nT; t++) {
			const TongueVoice& T = tv[t];
			float rx = x - T.tx, rz = z - T.tz;
			float ux = std::sin(T.pth), uz = std::cos(T.pth);          // outward along the radius
			float ur = rx * ux + rz * uz, ut = rx * uz - rz * ux;
			float q = std::sqrt((ur / T.pa) * (ur / T.pa) + (ut / T.pb) * (ut / T.pb));
			if (q < rho) { rho = q; best = t; }
		}
		return best;
	}
	// The hit. On a pad: that note, and the pads whose partials meet its own ring
	// with it, which is the pan's shimmer. Between pads: the nearest rings a little.
	void hitPanAt(int t, float rho, float speed, int mat, float radius, float mass) {
		const CanopyMat& M = CP_MAT[mat];
		float q = radius / CP_R0;
		float tc = 0.001f * CP_PAN_CONTACT * M.contactMs * q * SFS_POWABS(speed / CP_V0, -0.2f);
		float n = std::max(tc * coefSr, 2.f);
		float A = SFS_POWABS(mass, 0.35f) * (speed / CP_V0);
		if (t >= 0 && rho <= 1.f) {
			startContactP(t, A, n, rho);
			for (int k = 0; k < tv[t].nsym; k++) startContactP(tv[t].sym[k], A * 0.07f, n, 0.5f);
			knockBody(A * 0.2f, n);
		}
		else {
			if (t >= 0) {
				float g = 0.25f * SFS_EXP(-(rho - 1.f) / 0.5f);
				if (g > 0.02f) startContactP(t, A * g, n, 1.f);
			}
			knockBody(A, n);
		}
		strikes++;
	}

	// `over` is how much of the overtones this hit gives the tongue: a tongue rung
	// through the body gets little, or their inharmonic partials pile up into a wash.
	void startContact(int t, float A, float n, float over, float xi) {
		TongueVoice& T = tv[t];
		int slot = 0;
		float oldest = -1.f;
		for (int i = 0; i < CP_CONTACTS; i++) if (T.contact[i].ph > oldest) { oldest = T.contact[i].ph; slot = i; }
		Contact& C = T.contact[slot];
		C.ph = 0.f;
		C.dph = 1.f / n;
		C.amp = A * 2.f / n;              // a raised cosine of area A
		float pos = clamp(xi, 0.f, 1.f) * (float)(CP_SHAPE_N - 1);
		int i0 = std::min((int)pos, CP_SHAPE_N - 2);
		float fr = pos - (float)i0;
		for (int m = 0; m < CP_MODES; m++) {
			float s = shape[m][i0] + (shape[m][i0 + 1] - shape[m][i0]) * fr;
			C.g[m] = (0.15f + 0.85f * s) * tilt[m] * (m == 0 ? 1.f : over);
		}
		T.awake = true;
	}

	// The hit. Harder and heavier is louder, and contact time follows Hertz: with mass
	// proportional to r^3, (m^2 / (r v))^(1/5) reduces to r * v^(-1/5), so material and
	// size set the brightness far more than the speed does.
	//
	// The top is one piece of steel, so every landing sounds. The tongue under the drop
	// receives the whole hit; off a tongue, the nearest ones receive part of it through
	// the body and the shell sounds louder; on one, its neighbours ring faintly.
	void hitDrum(float x, float z, float speed, int mat, float radius, float mass, bool& wasOnTongue) {
		const CanopyMat& M = CP_MAT[mat];
		float q = radius / CP_R0;
		float tc = 0.001f * M.contactMs * q * SFS_POWABS(speed / CP_V0, -0.2f);
		float n = std::max(tc * coefSr, 2.f);
		float A = SFS_POWABS(mass, 0.35f) * (speed / CP_V0);

		int on = -1;
		float onXi = 0.f;
		float dist[CP_MAXN] = {}, xiAt[CP_MAXN] = {};
		for (int t = 0; t < nT; t++) {
			const TongueVoice& T = tv[t];
			float px = x - T.tx, pz = z - T.tz;
			float a = px * T.ux + pz * T.uz;
			float c = px * T.uz - pz * T.ux;
			float da = std::max(std::max(-a, a - T.len), 0.f);
			float dc = std::max(std::fabs(c) - T.w, 0.f);
			dist[t] = std::sqrt(da * da + dc * dc);
			xiAt[t] = 1.f - clamp(a / T.len, 0.f, 1.f);
			if (dist[t] <= 0.f && on < 0) { on = t; onXi = xiAt[t]; }
		}
		for (int t = 0; t < nT; t++) {
			float g;
			if (t == on) g = 1.f;
			else if (on >= 0) g = 0.08f * SFS_EXP(-dist[t] / 0.3f);
			else g = 0.26f * SFS_EXP(-dist[t] / 0.14f);
			if (g < 0.025f) continue;
			startContact(t, A * g, n, t == on ? 1.f : 0.25f, t == on ? onXi : 0.6f);
		}
		// the shell
		int slot = 0; float oldest = -1.f;
		for (int i = 0; i < CP_SHELL_CONTACTS; i++) if (shellHit[i].ph > oldest) { oldest = shellHit[i].ph; slot = i; }
		Contact& S = shellHit[slot];
		S.ph = 0.f; S.dph = 1.f / n;
		S.amp = A * (on >= 0 ? 0.3f : 1.f) * 2.f / n;
		strikes++;
		wasOnTongue = on >= 0;
	}

	// A drop that has settled, or has been in play a long time, is let go: it rolls down
	// the dome and off the rim, and it is removed once it has fallen out of the view.
	void release(CanopyDrop& d) {
		if (d.released) return;
		d.released = true;
		d.still = 0.f;
		if (instNow == 1) { d.vz += 0.5f; return; }    // the zither: off the front
		if (instNow == 2 || instNow == 3 || instNow >= 6) return;      // the pan, the bowls, the bonang, the bars: it fades where it lies
		float rad = std::sqrt(d.x * d.x + d.z * d.z);
		if (rad > 1e-3f) { d.vx += 0.5f * d.x / rad; d.vz += 0.5f * d.z / rad; }
		else d.vx += 0.5f;
	}

	// Coulomb friction: it removes speed in proportion to how hard the drop presses,
	// so a drop lying on a slope still rolls off it. `share` is the part of the normal
	// impulse a rigid surface would give that the drop actually gets: 1 off the drum,
	// less off a leaf that gives way under it (see the leaf contact in stepDrop). The
	// bounce used is returned, 0 when the drop was too slow to leave the surface.
	float reflect(CanopyDrop& d, float nx, float ny, float nz, float vn, float e, float mu, bool jitter, float share = 1.f) {
		float tx = d.vx - vn * nx, ty = d.vy - vn * ny, tz = d.vz - vn * nz;
		float vt = std::sqrt(tx * tx + ty * ty + tz * tz);
		bool rolling = -vn < 0.5f;             // too slow to leave the surface
		float bounce = rolling ? 0.f : e;
		// a round thing rolls: it keeps most of what sliding friction would remove
		float grip = rolling ? mu * 0.15f : mu;
		float keep = vt > 1e-5f ? std::max(1.f - grip * (1.f + bounce) * share * -vn / vt, 0.f) : 0.f;
		// share < 1: the normal speed changes by (1 + bounce) * share of the closing speed
		float out = share == 1.f ? -vn * bounce : -vn * ((1.f + bounce) * share - 1.f);
		d.vx = tx * keep + out * nx;
		d.vy = ty * keep + out * ny;
		d.vz = tz * keep + out * nz;
		if (jitter) {
			// nothing lands perfectly flat, and this is what sends a bounce to a neighbour
			float j = 0.02f * -vn;
			d.vx += j * bi(); d.vz += j * bi();
		}
		return bounce;
	}

	// One step of one drop against the planks and the drum.
	void stepDrop(CanopyDrop& d) {
		d.age += CP_DT;
		d.air += CP_DT;
		d.vy -= CP_G * CP_DT;
		float rad2 = d.x * d.x + d.z * d.z;
		float surface = surfaceAt(d.x, d.z);
		// the draught, once it is out past the edge of the column's middle
		if (instNow == 1) {
			// the same, in the string bed's own proportions, and only on a drop
			// in the air: one rolling down the strings must be left to roll off
			float sx, sz, zc; bedScale(sx, sz, zc);
			float u = d.x / sx, v = (d.z - zc) / sz, rn2 = u * u + v * v;
			if (!d.released && rn2 > CP_DRAUGHT_R * CP_DRAUGHT_R && d.y - d.r > surface + CP_Z_H + 0.05f) {
				float rn = std::sqrt(rn2);
				float low = 1.f + 3.f * clamp(1.f - d.y, 0.f, 1.f);
				float pull = low * CP_DRAUGHT_K * (rn - CP_DRAUGHT_R) * CP_DT / rn;
				d.vx -= u * sx * pull; d.vz -= v * sz * pull;
			}
		}
		// over the bowls only in the air above their rims, so it can never press a drop
		// against a rim
		else if (instNow == 6 || instNow == 8) {
			// over the bonang and the balafon the draught is their shape, a long oval, and
			// acts only on drops in flight above them
			const bool bars = instNow >= 7;
			const float W = bars ? CP_X_W : CP_BN_W, D = bars ? 0.55f : CP_BN_D + 0.05f, z0 = bars ? 0.1f : 0.f;
			const float above = bars ? CP_X_Y + 0.3f : CP_BN_BASE + 0.35f;
			float u = d.x / W, v = (d.z - z0) / D, rn2 = u * u + v * v;
			if (!d.released && d.air > 0.05f && rn2 > 0.7f * 0.7f && d.y > above) {
				float rn = std::sqrt(rn2);
				float pull = 2.f * CP_DRAUGHT_K * (rn - 0.7f) * CP_DT / rn;
				d.vx -= u * W * pull; d.vz -= v * D * pull;
			}
		}
		else if (instNow == 4) {
			// Over the chimes the draught brings a drop a leaf has thrown back over the
			// tubes: it checks a drop flying outward and draws one beyond the ring back
			// in. Only in flight: acting on a drop lying on a leaf it held the drop
			// against the leaf, where drops piled up by the hundred.
			if (!d.released && d.air > 0.05f && rad2 > 0.25f * CP_CH_RING * CP_CH_RING && d.y > CP_CH_HUB) {
				float rad = std::sqrt(rad2);
				float vr = (d.vx * d.x + d.vz * d.z) / rad;
				float k = (vr > 0.f ? std::min(8.f * CP_DT, 1.f) * vr : 0.f) / rad;
				if (rad > CP_CH_RING) k += 2.f * CP_DRAUGHT_K * (rad - CP_CH_RING) * CP_DT / rad;
				d.vx -= d.x * k; d.vz -= d.z * k;
			}
		}
		else if (!d.released && rad2 > CP_DRAUGHT_R * CP_DRAUGHT_R && d.y > (instNow == 3 ? bowlTop + 0.1f : instNow == 5 ? std::max(surface, CP_HP_H0 + 0.05f) : surface)) {
			float rad = std::sqrt(rad2);
			// stronger in the last stretch above the drum, where there is least time left to turn
			float low = 1.f + 3.f * clamp(1.f - d.y, 0.f, 1.f);
			float pull = low * CP_DRAUGHT_K * (rad - CP_DRAUGHT_R) * CP_DT / rad;
			d.vx -= d.x * pull; d.vz -= d.z * pull;
		}
		if (instNow >= 6 && !d.released && d.air > 0.05f && d.vy < 0.f) steer(d);
		d.x += d.vx * CP_DT; d.y += d.vy * CP_DT; d.z += d.vz * CP_DT;
		const CanopyMat& M = CP_MAT[d.mat];

		bool slow = false, wedged = false;
		for (int i = 0; i < nLive; i++) {
			const CanopyLeafLive& K = live[i];
			float rx = d.x - K.cx, ry = d.y - K.cy, rz = d.z - K.cz;
			const float reach = d.r + CP_LEAF_TH;
			const float rawA = rx * K.ax + ry * K.ay + rz * K.az;
			const float centre = CP_FORM_C[clamp(K.form, 0, LF_COUNT - 1)];
			const float xRaw = rawA / K.L - centre;
			const float uRaw = clamp(centre + xRaw, -2.f, 2.f);
			const float off = K.shape.sweep * uRaw * uRaw;       // where the midrib has swept to
			const float yRaw = ((rx * K.bx + rz * K.bz) / K.L - off) / K.shape.width;
			const float R = std::sqrt(xRaw * xRaw + yRaw * yRaw);
			if (R > formMax[clamp(K.form, 0, LF_COUNT - 1)] + (reach + 0.05f) / K.L) continue;
			const float rr = formRadius(K.form, std::atan2(yRaw, xRaw));
			const bool inside = R <= rr;
			const float toEdge = inside || R < 1e-5f ? 1.f : rr / R;
			const float px = xRaw * toEdge, py = yRaw * toEdge;  // the point on the leaf nearest
			const float u = centre + px;
			const float s = (u + 1.f) * K.L;                     // along from the stem
			const float lift = K.L * leafLift(K.shape, K.form, u, py) - s * bend[i];
			const float h = rx * K.nx + ry * K.ny + rz * K.nz - lift;
			if (std::fabs(h) > reach + 0.05f) continue;
			float nx, ny, nz, pen;
			if (inside) {
				// on the face: the surface's own slope, so a drop runs down the fold and off the tip
				if (std::fabs(h) >= reach) continue;
				float dA = -2.f * K.shape.arch * u + K.shape.twist * py - bend[i];
				const float flat = CP_FORM_FLAT[clamp(K.form, 0, LF_COUNT - 1)];
				float dB = (py >= 0.f ? K.shape.fold * flat : -K.shape.fold * flat) + K.shape.twist * u;
				nx = K.nx - K.ax * dA - K.bx * dB;
				ny = K.ny - K.ay * dA;
				nz = K.nz - K.az * dA - K.bz * dB;
				float nl = (h >= 0.f ? 1.f : -1.f) / std::sqrt(nx * nx + ny * ny + nz * nz);
				nx *= nl; ny *= nl; nz *= nl;
				pen = reach - std::fabs(h);
			}
			else {
				// over the edge: that point of it
				float la = u * K.L, lb = (off + py * K.shape.width) * K.L;
				float qx = rx - la * K.ax - lb * K.bx - K.nx * lift;
				float qy = ry - la * K.ay - K.ny * lift;
				float qz = rz - la * K.az - lb * K.bz - K.nz * lift;
				float dist2 = qx * qx + qy * qy + qz * qz;
				if (dist2 >= reach * reach || dist2 < 1e-10f) continue;
				float dist = std::sqrt(dist2);
				nx = qx / dist; ny = qy / dist; nz = qz / dist;
				pen = reach - dist;
			}
			d.x += nx * pen; d.y += ny * pen; d.z += nz * pen;
			// the surface here is moving, so everything is relative to it
			float sv = -s * bendV[i];
			float sx = K.nx * sv, sy = K.ny * sv, sz = K.nz * sv;
			float vn = (d.vx - sx) * nx + (d.vy - sy) * ny + (d.vz - sz) * nz;
			if (vn >= 0.f) continue;
			float speed = -vn;
			const CanopyLeafMat& Pd = CP_LEAF_MAT[K.mat];
			float e = M.bounce * Pd.give;
			const float mu = 0.5f * (M.friction + Pd.friction);
			float along = nx * K.nx + ny * K.ny + nz * K.nz;
			// The drop bounces as if the leaf were rigid, and the leaf is pushed by the
			// impulse a rigid leaf would have given. A leaf is held by its stem, so
			// momentum need not balance between the two, but energy must, and for a
			// drop heavy against the leaf (steel at full SIZE is 48 times the reference
			// mass) that push gave the leaf more than the drop lost: the leaf then threw
			// the next drop harder still, and drops went thousands of drum radii up. So
			// the leaf's motion takes at most what the drop gave up plus what it already
			// had, and the drop's bounce is left as it was. Where the drop would gain
			// more than the leaf holds (a heavy drop meeting a leaf on its way up), the
			// contact is solved as a collision of the two instead: the leaf's mass at
			// this point is inertia / (s * along)^2, and the drop gets the share of the
			// impulse that leaves the pair separating at the bounce.
			const float kick = d.mass * (1.f + e) * speed * along * s / K.inertia;
			CanopyDrop t = d;
			t.vx -= sx; t.vy -= sy; t.vz -= sz;
			reflect(t, nx, ny, nz, vn, e, mu, false);
			t.vx += sx; t.vy += sy; t.vz += sz;
			// twice the energies, in the drop's mass units: the leaf's is inertia x bendV^2
			const float ed0 = d.mass * (d.vx * d.vx + d.vy * d.vy + d.vz * d.vz);
			const float ed1 = d.mass * (t.vx * t.vx + t.vy * t.vy + t.vz * t.vz);
			const float room = ed0 + K.inertia * bendV[i] * bendV[i] - ed1;
			const float nb = bendV[i] + kick;
			float share = 1.f;   // of the rigid leaf's push, what the leaf gets
			if (K.inertia * nb * nb > room) {
				share = -1.f;
				if (room >= 0.f && kick != 0.f) {
					const float a = (std::copysign(std::sqrt(room / K.inertia), kick) - bendV[i]) / kick;
					if (a >= 0.f && a <= 1.f) share = a;
				}
			}
			if (share == 1.f) {
				d.vx -= sx; d.vy -= sy; d.vz -= sz;
				reflect(d, nx, ny, nz, vn, e, mu, speed > 0.8f);
				d.vx += sx; d.vy += sy; d.vz += sz;
				// and the drop pushes back
				bendV[i] = nb;
			}
			else if (share >= 0.f) {
				d.vx -= sx; d.vy -= sy; d.vz -= sz;
				reflect(d, nx, ny, nz, vn, e, mu, speed > 0.8f);
				d.vx += sx; d.vy += sy; d.vz += sz;
				bendV[i] += share * kick;
			}
			else {
				const float k = d.mass * s * s * along * along / K.inertia;
				const float part = 1.f / (1.f + k);
				d.vx -= sx; d.vy -= sy; d.vz -= sz;
				const float b = reflect(d, nx, ny, nz, vn, e, mu, speed > 0.8f, part);
				d.vx += sx; d.vy += sy; d.vz += sz;
				bendV[i] += d.mass * (1.f + b) * part * speed * along * s / K.inertia;
			}
			if (speed > 0.6f) leafFlash[i] = std::max(leafFlash[i], clamp(speed / 2.5f, 0.35f, 1.f));
			if (canBreak(d, speed * Pd.splash * 0.75f)) {
				// what is left on the leaf is one of the pieces
				splash(d, speed);
				setMaterial(d, TM_WATER, d.r * 0.6f);
				d.droplet = true;
			}
			if (speed < 0.6f) wedged = true;
			if (!d.touched) { d.touched = true; if (d.original) touched++; }
			d.air = 0.f;
		}

		if (instNow == 1) {
			if (!stepZither(d, slow)) return;
		}
		else if (instNow == 2) {
			if (!stepPan(d, slow)) return;
		}
		else if (instNow == 3) {
			if (!stepBowls(d, slow)) return;
		}
		else if (instNow == 4) {
			if (!dropChimes(d, slow)) return;
		}
		else if (instNow == 5) {
			if (!stepHand(d, slow)) return;
		}
		else if (instNow == 6) {
			if (!stepBonang(d, slow)) return;
		}
		else if (instNow == 7 || instNow == 8) {
			if (!stepBars(d, slow)) return;
		}
		else {
		rad2 = d.x * d.x + d.z * d.z;
		surface = cpDomeY(d.x, d.z);
		if (rad2 <= 1.f && d.y - d.r <= surface && d.y > surface - 0.2f && d.vy < 0.f) {
			// the dome's normal
			float nx = 2.f * CP_DOME * d.x, nz = 2.f * CP_DOME * d.z;
			float nl = 1.f / std::sqrt(1.f + nx * nx + nz * nz);
			nx *= nl; nz *= nl;
			float vn = d.vx * nx + d.vy * nl + d.vz * nz;
			if (vn < 0.f) {
				float speed = -vn;
				d.y = surface + d.r;
				if (speed >= CP_MIN_STRIKE) {
					bool on = false;
					hitDrum(d.x, d.z, speed, d.mat, d.r, d.mass, on);
					if (d.original) {
						if (!d.landed) { d.landed = true; landed++; }
						if (on && !d.onTongue) { d.onTongue = true; onTongue++; }
					}
				}
				if (d.mat == TM_WATER) {
					if (canBreak(d, speed)) splash(d, speed);
					d.alive = false;
					return;
				}
				reflect(d, nx, nl, nz, vn, M.bounce, M.friction, speed > 0.8f);
				if (speed < 0.6f) slow = true;
			}
		}
		// the hoop round the rim turns back a drop rolling off
		if (!d.released && rad2 > 0.92f && d.y < 0.1f && d.y > -0.1f) {
			float rad = std::sqrt(rad2);
			float vr = (d.vx * d.x + d.vz * d.z) / rad;
			if (vr > 0.f) {
				d.vx -= 1.4f * vr * d.x / rad; d.vz -= 1.4f * vr * d.z / rad;
				d.x *= 0.96f / rad; d.z *= 0.96f / rad;
			}
		}
		}
		if ((slow || wedged) && d.vx * d.vx + d.vy * d.vy + d.vz * d.vz < 0.25f) {
			d.still += CP_DT;
			if (d.still > 1.f && wedged) {
				// held in a dip or between two leaves: a random push frees it
				d.vx += 0.7f * bi(); d.vz += 0.7f * bi(); d.vy += 0.4f;
				d.still = 0.f;
			}
			else if (d.still > 1.5f) release(d);
		}
		else if (d.still > 0.f) d.still = std::max(0.f, d.still - CP_DT * 0.25f);
		if (d.age > 15.f) release(d);
		if (d.y < -1.8f || d.age > 40.f) d.alive = false;
	}

	// One step of a drop against the zither's strings, then its board. Returns
	// false if the drop has gone (water breaks on what it hits).
	bool stepZither(CanopyDrop& d, bool& slow) {
		const CanopyMat& M = CP_MAT[d.mat];
		const float s = CP_Z_SLOPE, sn = 1.f / std::sqrt(1.f + s * s);
		for (int t = 0; t < nT; t++) {
			TongueVoice& T = tv[t];
			float dx = d.x - T.tx;
			float reach = d.r + CP_Z_R;
			if (std::fabs(dx) >= reach) continue;
			float a = d.z - T.tz;                       // along, from the back bridge
			if (a < 0.f || a > T.len) continue;
			// the string runs down the board's slope, so the way out from it is
			// square to that slope, not to the vertical
			float dy = d.y - (cpBoardY(d.z) + CP_Z_H);
			float qx = dx, qy = dy * sn * sn, qz = dy * s * sn * sn;
			float dist2 = qx * qx + qy * qy + qz * qz;
			if (dist2 >= reach * reach || dist2 < 1e-12f) continue;
			float dist = std::sqrt(dist2);
			float nx = qx / dist, ny = qy / dist, nz = qz / dist;
			float pen = reach - dist;
			d.x += nx * pen; d.y += ny * pen; d.z += nz * pen;
			float vn = d.vx * nx + d.vy * ny + d.vz * nz;
			float xi = a / T.len;
			if (vn < 0.f) {
				float speed = -vn;
				if (speed >= CP_MIN_STRIKE) {
					hitString(t, xi, speed, d.mat, d.r, d.mass);
					if (d.original) {
						if (!d.landed) { d.landed = true; landed++; }
						if (!d.onTongue) { d.onTongue = true; onTongue++; }
					}
				}
				if (d.mat == TM_WATER) {
					if (canBreak(d, speed)) splash(d, speed);
					d.alive = false;
					return false;
				}
				// a taut string throws a drop back as well as a tongue does, and grips it less
				reflect(d, nx, ny, nz, vn, M.bounce, M.friction * 0.4f, speed > 0.8f);
			}
			if (vn > -0.5f) touchString(T, xi, d.mass, std::fabs(d.vz) + std::fabs(d.vx));
		}
		// the board, between and beyond the strings
		float half = 0.5f * CP_Z_SP * (float)(nT - 1);
		bool over = std::fabs(d.x) <= half + CP_Z_EDGE && d.z >= CP_Z_BACK - CP_Z_EDGE - 0.08f
		            && d.z <= zitherFront(d.x);
		float yb = cpBoardY(d.z);
		if (over && d.y - d.r <= yb && d.y > yb - 0.2f) {
			float nx = 0.f, ny = sn, nz = s * sn;
			float vn = d.vx * nx + d.vy * ny + d.vz * nz;
			if (vn < 0.f) {
				float speed = -vn;
				d.y = yb + d.r;
				if (speed >= CP_MIN_STRIKE) {
					hitBoard(d.x, d.z, speed, d.mat, d.r, d.mass);
					if (d.original && !d.landed) { d.landed = true; landed++; }
				}
				if (d.mat == TM_WATER) {
					if (canBreak(d, speed)) splash(d, speed);
					d.alive = false;
					return false;
				}
				reflect(d, nx, ny, nz, vn, M.bounce * 0.7f, M.friction, speed > 0.8f);
				if (speed < 0.6f) slow = true;
			}
		}
		return true;
	}

	// One step of a drop against the steel pan's bowl. Returns false if it has gone.
	// The bowl is concave, so a drop rolls down across the pads to the low point;
	// one that has settled there fades out rather than piling up.
	bool stepPan(CanopyDrop& d, bool& slow) {
		const CanopyMat& M = CP_MAT[d.mat];
		if (d.released) {
			d.fade -= CP_DT / 0.4f;
			if (d.fade <= 0.f) { d.alive = false; return false; }
		}
		float rad2 = d.x * d.x + d.z * d.z;
		if (rad2 > 1.f) return true;                 // outside the rim: past the skirt and away
		float surface = cpPanY(d.x, d.z);
		if (d.y - d.r > surface || d.y < surface - 0.3f) return true;
		float nx = -2.f * CP_PAN_D * d.x, nz = -(2.f * CP_PAN_D * d.z - CP_PAN_T);
		float nl = 1.f / std::sqrt(1.f + nx * nx + nz * nz);
		nx *= nl; nz *= nl;
		float vn = d.vx * nx + d.vy * nl + d.vz * nz;
		if (vn >= 0.f) return true;
		float speed = -vn;
		d.y = surface + d.r;
		if (speed >= CP_MIN_STRIKE && !d.released) {
			float rho;
			int t = padAt(d.x, d.z, rho);
			hitPanAt(t, rho, speed, d.mat, d.r, d.mass);
			d.pad = -2;                                // it struck: rolling starts afresh
			if (d.original) {
				if (!d.landed) { d.landed = true; landed++; }
				if (rho <= 1.f && !d.onTongue) { d.onTongue = true; onTongue++; }
			}
		}
		if (d.mat == TM_WATER) {
			if (canBreak(d, speed)) splash(d, speed);
			d.alive = false;
			return false;
		}
		reflect(d, nx, nl, nz, vn, M.bounce, M.friction, speed > 0.8f);
		if (speed < 0.6f) slow = true;
		if (speed < 0.5f) rollPan(d);
		return true;
	}
	void rollPan(CanopyDrop& d) {
		float vt = std::sqrt(d.vx * d.vx + d.vy * d.vy + d.vz * d.vz);
		if (vt < 0.03f) return;
		float rho;
		int t = padAt(d.x, d.z, rho);
		int pad = (t >= 0 && rho <= 1.f) ? t : -1;
		// A rolling contact lasts half a cycle of the pad it is on: long enough to round
		// off everything above the note, which a strike's short contact does not, and
		// no longer, or it would round off the note too. A short one is a tick.
		auto soft = [&](int t) { return std::max(0.5f / std::max(tv[t].freq, 20.f) * coefSr, 2.f); };
		float A = SFS_POWABS(d.mass, 0.35f) * (vt / CP_V0) * d.fade;
		if (pad != d.pad) {
			// over the groove: onto a pad, or off the one it was on
			if (d.pad != -2) {
				if (pad >= 0) startContactP(pad, A * CP_ROLL_EDGE, soft(pad), 1.f);
				else if (d.pad >= 0) startContactP(d.pad, A * CP_ROLL_EDGE * 0.6f, soft(d.pad), 1.f);
			}
			d.pad = pad;
			d.bump = CP_ROLL_PITCH * (0.5f + uni());
		}
		else if (pad >= 0) {
			d.bump -= vt * CP_DT;
			if (d.bump <= 0.f) {
				d.bump = CP_ROLL_PITCH * (0.5f + uni());
				startContactP(pad, A * CP_ROLL_BUMP * (0.3f + 0.7f * uni()), soft(pad), rho);
			}
		}
	}

	void physics(float dt) {
		// the zither's touches are gathered afresh every step
		if (instNow == 1 || instNow == 3)
			for (int t = 0; t < CP_MAXN; t++) tv[t].zTouch = false;
		for (int i = 0; i < nLive; i++) {
			const CanopyLeafMat& Pd = CP_LEAF_MAT[live[i].mat];
			float w = 2.f * (float)M_PI * Pd.hz;
			bendV[i] += (-w * w * bend[i] - 2.f * Pd.zeta * w * bendV[i]) * dt;
			bend[i] += bendV[i] * dt;
			leafFlash[i] *= 1.f - dt / 0.12f;
			if (std::fabs(bend[i]) > CP_LEAF_BEND) {
				bend[i] = std::copysign(CP_LEAF_BEND, bend[i]);
				bendV[i] = 0.f;
			}
		}
		if (!inputs[CLOCK_INPUT].isConnected()) {
			nextDrop -= dt * rateK;
			if (nextDrop <= 0.f) {
				spawnNow();
				nextDrop = evenly ? 1.f : -std::log(std::max(uni(), 1e-4f));
			}
		}
		if (instNow == 4) stepChimes(dt);
		for (CanopyDrop& d : drop) if (d.alive) stepDrop(d);
	}

	// Anywhere in the column above the drum.
	void spawnNow() {
		if (instNow == 3) {
			// Each bowl under the column is as likely as any other, whatever its size,
			// so the large one in the middle does not take most of the drops. The drop
			// falls somewhere over the bowl's mouth, a little wider, so some catch the
			// rim and some miss.
			int cand[CP_MAXN], nc = 0;
			for (int t = 0; t < nT; t++) {
				float dx = tv[t].tx - colX, dz = tv[t].tz - colZ;
				if (std::sqrt(dx * dx + dz * dz) <= colR + 0.5f * tv[t].bR) cand[nc++] = t;
			}
			if (nc > 0) {
				int t = cand[std::min((int)(uni() * (float)nc), nc - 1)];
				float a = uni() * 2.f * (float)M_PI, rr = 1.15f * tv[t].bR * std::sqrt(uni());
				spawn(tv[t].tx + rr * std::sin(a), tv[t].tz + rr * std::cos(a));
				return;
			}
		}
		float a = uni() * 2.f * (float)M_PI, rr = colR * std::sqrt(uni());
		if (instNow == 7 || instNow == 8) {
			// over a bar, each as likely as any other
			int t = std::min((int)(uni() * (float)nT), nT - 1);
			const TongueVoice& T = tv[t];
			float along = 0.5f * T.len * (2.f * uni() - 1.f), across = 1.1f * T.xw * (2.f * uni() - 1.f);
			spawn(T.tx + along * T.ux + across * T.uz, T.tz + along * T.uz - across * T.ux);
			return;
		}
		if (instNow == 6) {
			// over a kettle, each as likely as any other, a little wider than it, so
			// some drops catch the shoulder and some fall to the cords
			int t = std::min((int)(uni() * (float)nT), nT - 1);
			float ra = 1.05f * tv[t].bR * std::sqrt(uni());
			spawn(tv[t].tx + ra * std::cos(a), tv[t].tz + ra * std::sin(a));
			return;
		}
		if (instNow == 4) {
			// over the ring of tubes and a little past it, so a drop meets a tube, the
			// clapper or the sail, or falls through
			float k = (CP_CH_RING + 0.12f) / 0.95f;
			spawn((colX + rr * std::cos(a)) * k, (colZ + rr * std::sin(a)) * k);
			return;
		}
		if (instNow == 1) {
			float sx, sz, zc; bedScale(sx, sz, zc);
			spawn((colX + rr * std::cos(a)) * sx, zc + (colZ + rr * std::sin(a)) * sz);
			return;
		}
		spawn(colX + rr * std::cos(a), colZ + rr * std::sin(a));
	}

	// ── a direct strike, from the GATE input ──────────────────────────────
	int tongueForNote(float semisFromC4) const {
		float base = 12.f * (float)regOct + (float)root;
		float lo = base + tv[0].semis, hi = base + tv[nT - 1].semis;
		float s = semisFromC4;
		while (s < lo - 0.5f) s += 12.f;
		while (s > hi + 0.5f) s -= 12.f;
		int best = 0; float bd = 1e9f;
		for (int t = 0; t < nT; t++) {
			float d = std::fabs(s - (base + tv[t].semis));
			if (d < bd) { bd = d; best = t; }
		}
		return best;
	}
	int padForNote(float semisFromC4) const {
		float lo = 1e9f, hi = -1e9f;
		for (int t = 0; t < nT; t++) { lo = std::min(lo, tv[t].semis); hi = std::max(hi, tv[t].semis); }
		float s = semisFromC4;
		while (s < lo - 0.5f) s += 12.f;
		while (s > hi + 0.5f) s -= 12.f;
		int best = 0; float bd = 1e9f;
		for (int t = 0; t < nT; t++) {
			float d = std::fabs(s - tv[t].semis) + (tv[t].damped ? 0.6f : 0.f);
			if (d < bd) { bd = d; best = t; }
		}
		return best;
	}
	void gates() {
		const int gch = inputs[GATE_INPUT].getChannels();
		// all of them every sample: a Rack trigger first read high does not fire
		for (int c = 0; c < CP_MAXN; c++) {
			float g = c < gch ? inputs[GATE_INPUT].getVoltage(c) : 0.f;
			if (!gateTrig[c].process(g, 0.1f, 1.f)) continue;
			// the drum and the zither keep each note relative to ROOT; the rest keep it
			// from C4, and are looked up by it
			int t = inputs[VOCT_INPUT].isConnected()
				? (instNow >= 2 ? padForNote(inputs[VOCT_INPUT].getPolyVoltage(c) * 12.f) : tongueForNote(inputs[VOCT_INPUT].getPolyVoltage(c) * 12.f))
				: c % nT;
			float vel = inputs[VEL_INPUT].isConnected() ? clamp(inputs[VEL_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f) : 0.7f;
			if (vel <= 0.f) continue;
			if (instNow == 7 || instNow == 8) {
				// a mallet a little off the middle, where a player strikes a bar
				int m = chooseMaterial();
				float q = sizeK / CP_R0;
				hitBar(t, 0.42f, std::max(vel * CP_V0, CP_MIN_STRIKE), m, sizeK, CP_MAT[m].density * q * q * q);
				continue;
			}
			if (instNow == 6) {
				// on the boss, as a player strikes it
				int m = chooseMaterial();
				float q = sizeK / CP_R0;
				hitKettle(t, 0.1f, std::max(vel * CP_V0, CP_MIN_STRIKE), m, sizeK, CP_MAT[m].density * q * q * q);
				continue;
			}
			if (instNow == 4) {
				// struck below its middle, where the clapper meets it
				int m = chooseMaterial();
				float q = sizeK / CP_R0;
				hitChime(t, 0.62f, std::max(vel * CP_V0, CP_MIN_STRIKE), CP_MAT[m].contactMs, sizeK, CP_MAT[m].density * q * q * q);
				continue;
			}
			if (instNow == 3) {
				// struck on the rim, a little off the line its pairs are split along, so it beats
				int m = chooseMaterial();
				float q = sizeK / CP_R0;
				hitBowlAt(t, 1.f, tv[t].bAx + 0.3f, std::max(vel * CP_V0, CP_MIN_STRIKE), m, sizeK, CP_MAT[m].density * q * q * q);
				continue;
			}
			if (instNow == 2 || instNow == 5) {
				// struck a little off the pad's middle; with V/OCT the nearest pad SCALE has not damped
				int m = chooseMaterial();
				float q = sizeK / CP_R0;
				hitPanAt(t, 0.3f, std::max(vel * CP_V0, CP_MIN_STRIKE), m, sizeK, CP_MAT[m].density * q * q * q);
				continue;
			}
			if (instNow == 1) {
				// plucked near the front bridge, where all its harmonics move
				int m = chooseMaterial();
				float q = sizeK / CP_R0;
				hitString(t, 0.86f, std::max(vel * CP_V0, CP_MIN_STRIKE), m, sizeK, CP_MAT[m].density * q * q * q);
				continue;
			}
			const TongueVoice& T = tv[t];
			// a mallet on the free end, three quarters of the way out
			float x = T.tx + T.ux * T.len * 0.25f, z = T.tz + T.uz * T.len * 0.25f;
			int m = chooseMaterial();
			float q = sizeK / CP_R0;
			bool on = false;
			hitDrum(x, z, std::max(vel * CP_V0, CP_MIN_STRIKE), m, sizeK, CP_MAT[m].density * q * q * q, on);
		}
	}

	// ── the knobs and the key, every 32 samples ──────────────────────────────
	void control(float sr) {
		rateK = 0.05f * SFS_POWABS(240.f, params[RATE_PARAM].getValue());
		if (inputs[RATE_INPUT].isConnected()) rateK *= SFS_EXP2(clamp(inputs[RATE_INPUT].getVoltage(), -10.f, 10.f));
		matK = clamp((int)std::round(params[MATERIAL_PARAM].getValue() + (inputs[MATERIAL_INPUT].isConnected() ? inputs[MATERIAL_INPUT].getVoltage() : 0.f)), 0, TM_COUNT - 1);
		padK = clamp((int)std::round(params[LEAF_PARAM].getValue() + (inputs[LEAF_INPUT].isConnected() ? inputs[LEAF_INPUT].getVoltage() : 0.f)), 0, TP_COUNT - 1);
		leavesK = clamp((int)std::round(params[DENSITY_PARAM].getValue() + (inputs[DENSITY_INPUT].isConnected() ? inputs[DENSITY_INPUT].getVoltage() : 0.f)), 0, CP_MAXLEAVES);
		float size = clamp(params[SIZE_PARAM].getValue() + inputs[SIZE_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		sizeK = 0.035f * SFS_POWABS(3.143f, size);
		scatterK = clamp(params[SCATTER_PARAM].getValue() + inputs[SCATTER_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		driftK = clamp(params[DRIFT_PARAM].getValue() + inputs[DRIFT_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		tiltK = clamp(params[TILT_PARAM].getValue() + inputs[TILT_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		speedK = 0.02f * SFS_POWABS(75.f, clamp(params[SPEED_PARAM].getValue() + inputs[SPEED_INPUT].getVoltage() * 0.2f, 0.f, 1.f));
		// X and Z move the column, which narrows to stay over the drum: at full SCATTER it
		// is already as wide as the drum and would otherwise have nowhere to go.
		colX = inputs[X_INPUT].isConnected() ? inputs[X_INPUT].getVoltage() * 0.19f : 0.f;
		colZ = inputs[Z_INPUT].isConnected() ? inputs[Z_INPUT].getVoltage() * 0.19f : 0.f;
		float off = std::sqrt(colX * colX + colZ * colZ);
		if (off > 0.95f) { colX *= 0.95f / off; colZ *= 0.95f / off; off = 0.95f; }
		colR = std::min(0.95f * scatterK, 0.95f - off);
		float dec = clamp(params[DECAY_PARAM].getValue() + inputs[DECAY_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		decayK = 0.25f * SFS_POWABS(16.f, dec);
		float tone = clamp(params[TONE_PARAM].getValue() + inputs[TONE_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		for (int m = 0; m < CP_MODES; m++) tilt[m] = SFS_POWABS(CP_RATIO[m], (tone - 0.5f) * 3.f);
		// the zither's harmonics fall away as 1/n at the middle of TONE, as a plucked
		// string's do, and TONE leans that either way
		for (int m = 0; m < CP_ZMODES; m++) tiltZ[m] = SFS_POWABS((float)(m + 1), (tone - 0.5f) * 2.5f - 0.5f);
		for (int m = 0; m < CP_PMODES; m++) tiltP[m] = SFS_POWABS(CP_PAN_RATIO[m], (tone - 0.5f) * 3.f);
		for (int k = 0; k < CP_BPART; k++) tiltB[k] = SFS_POWABS(CP_BOWL_RATIO[k], (tone - 0.5f) * 3.f);
		for (int m = 0; m < CP_CMODES; m++) tiltC[m] = SFS_POWABS(CP_CH_RATIO[m], (tone - 0.5f) * 3.f);
		for (int m = 0; m < CP_BNMODES; m++) tiltBn[m] = SFS_POWABS(CP_BN_RATIO[m], (tone - 0.5f) * 3.f);
		for (int m = 0; m < CP_XMODES; m++) tiltX[m] = SFS_POWABS(CP_MAR_RATIO[m], (tone - 0.5f) * 3.f);
		zDampK = CP_Z_DAMP * 48000.f / sr;

		int r = (int)std::round(params[ROOT_PARAM].getValue());
		if (inputs[ROOT_INPUT].isConnected()) r += (int)std::round(sfs::rootCvSemis(inputs[ROOT_INPUT].getVoltage(), rootCvMode));
		root = ((r % 12) + 12) % 12;
		sc = sfs::busResolve(inputs[SCALE_INPUT], (int)std::round(params[SCALE_PARAM].getValue()));

		instrument = clamp((int)std::round(params[INSTRUMENT_PARAM].getValue()
		             + (inputs[INSTRUMENT_INPUT].isConnected() ? inputs[INSTRUMENT_INPUT].getVoltage() : 0.f)), 0, CP_NINST - 1);
		// the change waits until the output has faded out, or it would click
		bool switched = instrument != instNow && switchFade <= 0.f;
		if (switched) {
			// a new instrument starts silent: nothing carries over from the old one
			instNow = instrument;
			for (TongueVoice& T : tv) {
				for (int m = 0; m < CP_VMODES; m++) { T.re[m] = T.im[m] = T.ex[m] = 0.f; }
				for (Contact& C : T.contact) C.ph = 2.f;
				T.awake = T.fading = T.zTouch = false; T.env = 0.f; T.out = 0.f;
			}
		}
		bool all = sr != coefSr || decayK != coefDecay || switched;
		coefSr = sr; coefDecay = decayK;
		if (instNow == 3) {
			// the bowls lay themselves out from ROOT and SCALE; water dries out of them
			int sig = bowlSignature();
			if (sig != bowlSig || switched) { layoutBowls(); bowlSig = sig; all = true; }
			float dry = 32.f / (sr * CP_BOWL_EVAP);
			for (int t = 0; t < nT; t++) {
				TongueVoice& T = tv[t];
				T.water = std::max(0.f, T.water - dry);
				if (all || std::fabs(T.water - T.waterCoef) > 0.003f) bowlCoefs(T, t, sr);
			}
		}
		else if (instNow == 4) {
			int sig = bowlSignature();
			if (sig != chimeSig || switched) { layoutChimes(); chimeSig = sig; all = true; }
			panSig = -1; bowlSig = -1;
			if (all) for (int t = 0; t < nT; t++) chimeCoefs(tv[t], sr);
		}
		else if (instNow == 7 || instNow == 8) {
			int sig = bowlSignature() * 3 + instNow;
			if (sig != barSig || switched) { layoutBars(); barSig = sig; all = true; }
			panSig = -1; bowlSig = -1;
			if (all) for (int t = 0; t < nT; t++) barCoefs(tv[t], sr);
		}
		else if (instNow == 6) {
			int sig = bowlSignature();
			if (sig != chimeSig || switched) { layoutBonang(); chimeSig = sig; all = true; }
			panSig = -1; bowlSig = -1;
			if (all) for (int t = 0; t < nT; t++) bonangCoefs(tv[t], sr);
		}
		else if (instNow == 5) {
			int sig = bowlSignature();
			if (sig != panSig || switched) { layoutHand(); panSig = sig; all = true; }
			bowlSig = -1;
			if (all) for (int t = 0; t < nT; t++) panCoefs(tv[t], sr);
		}
		else if (instNow == 2) {
			// the pan lays out its own pads, from the tuning menu, ROOT and SCALE
			int sig = panSignature();
			if (sig != panSig || switched) { layoutPan(); panSig = sig; all = true; }
			bowlSig = -1;
			if (all) for (int t = 0; t < nT; t++) panCoefs(tv[t], sr);
		}
		else {
		nT = tongueCount(sc.size);
		panSig = -1; bowlSig = -1;
		bool moved = nT != layoutN || switched;
		for (int t = 0; t < CP_MAXN; t++) {
			float semis = sc.degree(t);
			float f = dsp::FREQ_C4 * std::pow(2.f, (float)regOct + ((float)root + semis) / 12.f);
			f = std::min(f, sr * 0.45f);
			TongueVoice& T = tv[t];
			if (T.semis != semis) moved = true;
			T.semis = semis;
			if (!all && f == T.freq) continue;
			T.freq = f;
			float rShort = std::sqrt(220.f / f);
			if (instNow == 1) {
				// a string: harmonics, stretched a little by stiffness, the higher ones dying sooner
				for (int m = 0; m < CP_ZMODES; m++) {
					float n1 = (float)(m + 1);
					float fm = f * n1 * std::sqrt(1.f + CP_Z_B * n1 * n1);
					if (fm >= sr * 0.45f) { T.lv[m] = 0.f; T.c[m] = 1.f; T.s[m] = 0.f; continue; }
					float t60 = std::max(CP_Z_T60 * decayK * clamp(rShort, 0.5f, 1.6f) / (1.f + 0.03f * (float)(m * m)), 0.02f);
					float dec1 = std::pow(10.f, -3.f / (t60 * sr));
					float w = 2.f * (float)M_PI * fm / sr;
					T.c[m] = dec1 * std::cos(w); T.s[m] = dec1 * std::sin(w);
					T.lv[m] = 1.f;
				}
				continue;
			}
			for (int m = 0; m < CP_MODES; m++) {
				float fm = f * CP_RATIO[m];
				if (fm >= sr * 0.45f) { T.lv[m] = 0.f; T.c[m] = 1.f; T.s[m] = 0.f; continue; }
				float t60 = std::max(CP_T60[m] * decayK * clamp(rShort, 0.5f, 1.6f), 0.02f);
				float dec1 = std::pow(10.f, -3.f / (t60 * sr));
				float w = 2.f * (float)M_PI * fm / sr;
				T.c[m] = dec1 * std::cos(w); T.s[m] = dec1 * std::sin(w);
				T.lv[m] = std::pow(10.f, CP_DB[m] / 20.f);
			}
		}
		if (moved) { if (instNow == 1) layoutStrings(nT); else layoutTongues(nT); }
		}
		if (all) {
			static const float BF[2] = {190.f, 410.f}, BT[2] = {0.22f, 0.10f};
			for (int b = 0; b < 2; b++) {
				float dec1 = std::pow(10.f, -3.f / (BT[b] * sr));
				float w = 2.f * (float)M_PI * BF[b] / sr;
				bodyC[b] = dec1 * std::cos(w); bodyS[b] = dec1 * std::sin(w);
			}
		}
	}

	void process(const ProcessArgs& args) override {
		if ((ctl & 31) == 0) control(args.sampleRate);
		if ((ctl & 127) == 0) updateLive(args.sampleTime * 128.f);
		ctl++;

		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) spawnNow();
		gates();
		physAcc += args.sampleTime;
		while (physAcc >= CP_DT) { physAcc -= CP_DT; physics(CP_DT); }

		float mixL = 0.f, mixR = 0.f;
		const bool zith = instNow == 1;
		const int nm = zith ? CP_ZMODES : (instNow == 2 || instNow == 5) ? CP_PMODES : instNow == 3 ? CP_BMODES : instNow == 4 ? CP_CMODES : instNow == 6 ? CP_BNMODES : instNow >= 7 ? CP_XMODES : CP_MODES;
		// A bowl rings for tens of seconds, so the same push builds far further on
		// it than on a string, and the bowls get a quarter of it.
		const float pumpK = instNow == 3 ? 0.25f * CP_Z_PUMP : CP_Z_PUMP;
		for (int t = 0; t < CP_MAXN; t++) {
			TongueVoice& T = tv[t];
			if (!T.awake) { T.out = 0.f; continue; }
			bool touching = false;
			for (int i = 0; i < CP_CONTACTS; i++) {
				Contact& C = T.contact[i];
				if (C.ph >= 1.f) continue;
				touching = true;
				float f = 0.5f - 0.5f * SFS_COS2PI(C.ph);
				float af = f * C.amp;
				for (int m = 0; m < nm; m++) T.ex[m] += af * C.g[m];
				C.ph += C.dph;
			}
			const bool held = (zith || instNow == 3) && T.zTouch;
			if (held) {
				// a trace of noise, so a silent harmonic has something to grow from
				zrng ^= zrng << 13; zrng ^= zrng >> 17; zrng ^= zrng << 5;
				float wn = (float)(zrng >> 8) * (2.f / 16777216.f) - 1.f;
				T.zNoise += (wn - T.zNoise) * 0.35f;
				float rn = T.zNoise * CP_Z_SEED;
				for (int m = 0; m < nm; m++) T.ex[m] += rn * T.zRub[m];
				touching = true;
			}
			float y = 0.f;
			for (int m = 0; m < nm; m++) {
				float re = T.re[m] * T.c[m] - T.im[m] * T.s[m] + T.ex[m];
				float im = T.re[m] * T.s[m] + T.im[m] * T.c[m];
				if (held) {
					// damped where the drop sits, and pushed along in step with itself
					float amp = std::sqrt(re * re + im * im);
					float g = T.zDamp[m] + pumpK * T.zRub[m] / (amp + 1e-3f);
					re *= g; im *= g;
				}
				T.re[m] = re; T.im[m] = im;
				T.ex[m] = 0.f;
				y += im * T.lv[m];
			}
			if (instNow == 8) {
				// The gourd's membrane: once the air in the gourd moves hard enough, the
				// membrane slaps against the rim of its hole on the outward half of each
				// cycle, which flattens that half of the fundamental: a buzz made of the
				// note's own harmonics, as a kazoo's is, with a trace of rattle. Below
				// the threshold it is silent.
				// by how hard the bar itself moves, whatever its level in the mix
				float a0 = T.im[0] * 1.1f;
				float over = a0 - CP_X_BUZZ;
				if (over > 0.f) {
					zrng ^= zrng << 13; zrng ^= zrng >> 17; zrng ^= zrng << 5;
					float wn = (float)(zrng >> 8) * (2.f / 16777216.f) - 1.f;
					T.buzzN += (wn - T.buzzN) * 0.5f;
					T.buzzB += (T.buzzN - T.buzzB) * 0.1f;
					y += (-0.85f * over + (T.buzzN - T.buzzB) * over * 0.35f) * T.lv[0] / 1.1f;
				}
			}
			T.out = y;
			T.env += (std::fabs(T.out) - T.env) * (std::fabs(T.out) > T.env ? 0.02f : 0.0005f);
			mixL += T.out * T.panL;
			mixR += T.out * T.panR;
			// Once it is down by 47 dB the rest of the tail is only adding to the wash
			// under whatever rings next, so it fades out over about 50 ms.
			if (!touching && (args.frame & 255) == 0) {
				float e = 0.f;
				for (int m = 0; m < nm; m++) e += T.re[m] * T.re[m] + T.im[m] * T.im[m];
				T.fading = e < 2e-5f;
				if (e < 1e-8f) { T.awake = false; T.fading = false; T.env = 0.f; for (int m = 0; m < nm; m++) { T.re[m] = 0.f; T.im[m] = 0.f; } }
			}
			if (T.fading) { for (int m = 0; m < nm; m++) { T.re[m] *= 0.9996f; T.im[m] *= 0.9996f; } }
		}
		// the shell, knocked by every hit and heard through both sides
		float force = 0.f;
		for (int i = 0; i < CP_SHELL_CONTACTS; i++) {
			Contact& C = shellHit[i];
			if (C.ph >= 1.f) continue;
			force += (0.5f - 0.5f * SFS_COS2PI(C.ph)) * C.amp;
			C.ph += C.dph;
		}
		float body = 0.f;
		for (int b = 0; b < 2; b++) {
			float re = bodyRe[b] * bodyC[b] - bodyIm[b] * bodyS[b] + force;
			float im = bodyRe[b] * bodyS[b] + bodyIm[b] * bodyC[b];
			bodyRe[b] = re; bodyIm[b] = im;
			body += im;
		}
		body = body * 0.1f;

		static const float OUT = 7.5f;
		float l = (mixL + body) * OUT, r = (mixR + body) * OUT;
		// 5 ms out before a change of instrument, and 5 ms back in after it
		if (instrument != instNow || switchFade < 1.f) {
			float step = args.sampleTime / 0.005f;
			switchFade = instrument != instNow ? std::max(switchFade - step, 0.f) : std::min(switchFade + step, 1.f);
			l *= switchFade; r *= switchFade;
		}
		float dl = limBuf[0][limHead], dr = limBuf[1][limHead];
		limBuf[0][limHead] = l; limBuf[1][limHead] = r;
		limHead = (limHead + 1) % LOOK;
		float pk = 0.f;
		for (int k = 0; k < LOOK; k++) pk = std::max(pk, std::max(std::fabs(limBuf[0][k]), std::fabs(limBuf[1][k])));
		float want = pk > 6.f ? 6.f / pk : 1.f;
		if (args.sampleTime != limDt) {
			limDt = args.sampleTime;
			limAtk = 1.f - std::exp(-limDt / 0.0003f);
			limRel = 1.f - std::exp(-limDt / 0.35f);
		}
		limGain += (want - limGain) * (want < limGain ? limAtk : limRel);
		outputs[L_OUTPUT].setVoltage(sfs::softClip(dl * limGain));
		outputs[R_OUTPUT].setVoltage(sfs::softClip(dr * limGain));
		if (nT <= 16) {
			outputs[POLY_OUTPUT].setChannels(nT);
			for (int t = 0; t < nT; t++) outputs[POLY_OUTPUT].setVoltage(sfs::softClip(tv[t].out * OUT * 0.6f), t);
		}
		else {
			// more notes than a cable has channels (the pan): the 16 loudest, chosen
			// again every 256 samples, each keeping its channel while it stays in
			if ((args.frame & 255) == 0) {
				bool keep[CP_MAXN] = {};
				int order[CP_MAXN];
				for (int t = 0; t < nT; t++) order[t] = t;
				std::sort(order, order + nT, [&](int a, int b) { return tv[a].env > tv[b].env; });
				for (int k = 0; k < 16; k++) keep[order[k]] = true;
				bool used[CP_MAXN] = {};
				for (int c = 0; c < 16; c++) if (polyMap[c] < nT && keep[polyMap[c]] && !used[polyMap[c]]) used[polyMap[c]] = true; else polyMap[c] = -1;
				int k = 0;
				for (int c = 0; c < 16; c++) {
					if (polyMap[c] >= 0) continue;
					while (k < 16 && used[order[k]]) k++;
					if (k < 16) { polyMap[c] = order[k]; used[order[k]] = true; }
				}
			}
			outputs[POLY_OUTPUT].setChannels(16);
			for (int c = 0; c < 16; c++)
				outputs[POLY_OUTPUT].setVoltage(polyMap[c] >= 0 && polyMap[c] < nT ? sfs::softClip(tv[polyMap[c]].out * OUT * 0.6f) : 0.f, c);
		}
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		sfs::rootCvToJson(r, rootCvMode);
		json_object_set_new(r, "evenly", json_boolean(evenly));
		json_object_set_new(r, "panTuning", json_integer(panTuning));
		json_object_set_new(r, "leafForm", json_integer(leafForm));
		json_object_set_new(r, "regOct", json_integer(regOct));
		json_object_set_new(r, "camYaw", json_real(camYaw));
		json_object_set_new(r, "camTilt", json_real(camTilt));
		json_t* a = json_array();
		for (int i = 0; i < CP_MAXLEAVES; i++) {
			const CanopyLeaf& P = leaf[i];
			json_t* p = json_array();
			for (float v : {P.cx, P.cy, P.cz, P.ax, P.ay, P.az}) json_array_append_new(p, json_real(v));
			json_array_append_new(a, p);
		}
		json_object_set_new(r, "leaves", a);
		return r;
	}

	void dataFromJson(json_t* r) override {
		sfs::rootCvFromJson(r, this, ROOT_INPUT, rootCvMode);
		if (json_t* j = json_object_get(r, "evenly")) evenly = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "panTuning")) panTuning = clamp((int)json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(r, "leafForm")) leafForm = clamp((int)json_integer_value(j), 0, (int)LF_MIXED);
		if (json_t* j = json_object_get(r, "regOct")) regOct = clamp((int)json_integer_value(j), -2, 0);
		if (json_t* j = json_object_get(r, "camYaw")) camYaw = (float)json_real_value(j);
		if (json_t* j = json_object_get(r, "camTilt")) camTilt = clamp((float)json_real_value(j), 0.15f, 1.4f);
		if (json_t* a = json_object_get(r, "leaves")) {
			int n = std::min((int)json_array_size(a), CP_MAXLEAVES);
			for (int i = 0; i < n; i++) {
				json_t* p = json_array_get(a, i);
				float v[6] = {};
				for (int k = 0; k < 6; k++) v[k] = (float)json_real_value(json_array_get(p, k));
				CanopyLeaf& P = leaf[i];
				P.cx = v[0]; P.cy = v[1]; P.cz = v[2];
				P.ax = v[3]; P.ay = v[4]; P.az = v[5];
			}
			publishLeaves();
		}
	}
};
const int Canopy::DRIFT_AXES;
const int Canopy::LOOK;

static Canopy* canopyPreview() {
	static Canopy* m = nullptr;
	if (m) return m;
	m = new Canopy();
	m->params[Canopy::RATE_PARAM].setValue(0.62f);
	int64_t frame = 0;
	sfs::previewRun(*m, 3.2f, frame);
	return m;
}

// ── the view ─────────────────────────────────────────────────────────────
// A perspective camera on an orbit round the middle of the volume.
struct CanopyView {
	float w, h, cy, sy, cp, sp, S;
	static constexpr float DIST = 8.0f;
	static constexpr float MIDY = 1.24f;
	CanopyView(float w_, float h_, float yaw, float tilt) : w(w_), h(h_) {
		cy = std::cos(yaw); sy = std::sin(yaw);
		cp = std::cos(tilt); sp = std::sin(tilt);
		// fitted to the volume at the default camera, so orbiting does not change the scale
		S = std::min(w / 2.4f, h / 3.42f);
	}
	struct Pt { float x, y, depth, scale; };
	Pt project(float x, float y, float z) const {
		float X = x * cy - z * sy;
		float Z = x * sy + z * cy;
		float Y = y - MIDY;
		float yc = Y * cp - Z * sp;      // up on screen
		float zc = Y * sp + Z * cp;      // toward the camera
		float k = DIST / std::max(DIST - zc, 0.2f);
		Pt p;
		p.x = w * 0.5f + X * S * k;
		p.y = h * 0.5f - yc * S * k;
		p.depth = zc;
		p.scale = S * k;
		return p;
	}
	// The ray through a screen point, in world coordinates.
	void ray(float px, float py, float o[3], float d[3]) const {
		float X = (px - w * 0.5f) / S, yc = (h * 0.5f - py) / S;
		// camera at (0, 0, DIST) in view space, looking toward -z, with the screen at z = 0
		float vx = X, vy = yc, vz = -DIST;
		fromView(0.f, 0.f, DIST, o); o[1] += MIDY;
		fromView(vx, vy, vz, d);
	}
	void fromView(float X, float yc, float zc, float out[3]) const {
		float Y = yc * cp + zc * sp;
		float Z = -yc * sp + zc * cp;
		out[0] = X * cy + Z * sy;
		out[1] = Y;
		out[2] = -X * sy + Z * cy;
	}
	// Where the ray meets the level plane y = planeY.
	bool onLevel(float px, float py, float planeY, float& x, float& z) const {
		float o[3], d[3];
		ray(px, py, o, d);
		if (std::fabs(d[1]) < 1e-5f) return false;
		float t = (planeY - o[1]) / d[1];
		if (t <= 0.f) return false;
		x = o[0] + d[0] * t; z = o[2] + d[2] * t;
		return true;
	}
	// Where the ray meets the plane through (px, py, pz) that faces the camera.
	void onFacing(float mx, float my, const float p[3], float out[3]) const {
		float o[3], d[3], f[3];
		ray(mx, my, o, d);
		fromView(0.f, 0.f, -1.f, f);
		float den = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
		float t = ((p[0] - o[0]) * f[0] + (p[1] - o[1]) * f[1] + (p[2] - o[2]) * f[2]) / den;
		for (int i = 0; i < 3; i++) out[i] = o[i] + d[i] * t;
	}
};
constexpr float CanopyView::DIST;
constexpr float CanopyView::MIDY;

static const NVGcolor CP_INK  = nvgRGB(0x23, 0x1F, 0x20);
static const NVGcolor CP_SOFT = nvgRGB(0x8A, 0x8A, 0x96);
static const NVGcolor CP_LINE = nvgRGB(0xC4, 0xC4, 0xCC);
static const NVGcolor CP_HOT  = nvgRGB(0xEC, 0x65, 0x2E);
// The drawing is ink on the faceplate, so it has a dark-panel version: light ink
// on the dark faceplate, the drum a shade lighter than the faceplate so it still
// reads as an object, the leaves darkened, and the drops kept as "heavier is
// more ink", which on a dark faceplate means lighter. The light values are the
// ones the drawing has always used.
struct CpPal { NVGcolor ink, line, top, shell, shadeLeaf, shadeDrop; bool dark; };
static inline CpPal cpPal() {
	if (sfs::dark())
		return {sfs::theme().ink, nvgRGB(0x58, 0x58, 0x60), nvgRGB(0x4A, 0x4A, 0x50), nvgRGB(0x2C, 0x2C, 0x30),
		        nvgRGBA(0x00, 0x00, 0x00, 0x30), nvgRGBA(0x00, 0x00, 0x00, 0x50), true};
	return {CP_INK, CP_LINE, nvgRGB(0xE8, 0xE8, 0xEC), nvgRGB(0xD0, 0xD0, 0xD6),
	        nvgRGBA(0x23, 0x1F, 0x20, 0x0E), nvgRGBA(0x23, 0x1F, 0x20, 0x20), false};
}

struct CanopyDisplay : OpaqueWidget {
	Canopy* module = nullptr;
	enum Grab { NONE, BODY, HEIGHT, END_A, END_B, ORBIT };
	Grab grab = NONE;
	int  grabbed = -1;
	int  hover = -1;
	Vec  lastPos;
	float grabX = 0.f, grabZ = 0.f;   // where on the leaf it was picked up

	CanopyView view() const { return CanopyView(box.size.x, box.size.y, module->camYaw, module->camTilt); }

	// a point on a leaf's surface: `u` along it (-1 stem, 1 tip), `y` across, in half-lengths
	CanopyView::Pt leafAt(const CanopyView& V, const CanopyLeafLive& K, float bent, float u, float y, float world[3] = nullptr) const {
		float la = u * K.L;
		float lb = (K.shape.sweep * u * u + y * K.shape.width) * K.L;
		float lift = K.L * Canopy::leafLift(K.shape, K.form, u, y) - (u + 1.f) * K.L * bent;
		float x = K.cx + K.ax * la + K.bx * lb + K.nx * lift;
		float yy = K.cy + K.ay * la + K.ny * lift;
		float z = K.cz + K.az * la + K.bz * lb + K.nz * lift;
		if (world) { world[0] = x; world[1] = yy; world[2] = z; }
		return V.project(x, yy, z);
	}
	// a point `rho` of the way out from the centre to the edge, at an angle from the tip
	CanopyView::Pt leafPolar(const CanopyView& V, const CanopyLeafLive& K, float bent, float th, float rho, float world[3] = nullptr) const {
		float r = rho * module->formRadius(K.form, th);
		return leafAt(V, K, bent, CP_FORM_C[clamp(K.form, 0, LF_COUNT - 1)] + r * std::cos(th), r * std::sin(th), world);
	}
	static bool inPoly(const CanopyView::Pt* q, int n, float x, float y) {
		bool in = false;
		for (int i = 0, j = n - 1; i < n; j = i++)
			if ((q[i].y > y) != (q[j].y > y) && x < (q[j].x - q[i].x) * (y - q[i].y) / (q[j].y - q[i].y) + q[i].x) in = !in;
		return in;
	}
	// the leaf under a point, nearest the camera first; which end, if near one
	int leafUnder(Vec p, Grab* end = nullptr) const {
		CanopyView V = view();
		const Canopy& M = *module;
		int best = -1; float bestDepth = -1e9f;
		for (int i = 0; i < M.nLive; i++) {
			const CanopyLeafLive& K = M.live[i];
			CanopyView::Pt q[32];
			for (int k = 0; k < 32; k++) q[k] = leafPolar(V, K, M.bend[i], -(float)M_PI + 2.f * (float)M_PI * (float)k / 32.f, 1.f);
			CanopyView::Pt ca = V.project(K.cx + K.ax * K.L, K.cy + K.ay * K.L, K.cz + K.az * K.L);
			CanopyView::Pt cb = V.project(K.cx - K.ax * K.L, K.cy - K.ay * K.L, K.cz - K.az * K.L);
			float da = std::hypot(ca.x - p.x, ca.y - p.y), db = std::hypot(cb.x - p.x, cb.y - p.y);
			bool nearEnd = std::min(da, db) < 9.f;
			if (!inPoly(q, 32, p.x, p.y) && !nearEnd) continue;
			CanopyView::Pt c = V.project(K.cx, K.cy, K.cz);
			if (c.depth > bestDepth) {
				bestDepth = c.depth; best = i;
				if (end) *end = !nearEnd ? BODY : (da < db ? END_A : END_B);
			}
		}
		return best;
	}

	void onButton(const ButtonEvent& e) override {
		OpaqueWidget::onButton(e);
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) return;
		lastPos = e.pos;
		grab = ORBIT; grabbed = -1;
		Grab end = BODY;
		int i = leafUnder(e.pos, &end);
		if (i >= 0) {
			grabbed = i;
			grab = end;
			if (end == BODY && (e.mods & GLFW_MOD_SHIFT)) grab = HEIGHT;
			if (end == BODY) {
				float x = 0.f, z = 0.f;
				view().onLevel(e.pos.x, e.pos.y, module->leaf[i].cy, x, z);
				grabX = module->leaf[i].cx - x; grabZ = module->leaf[i].cz - z;
			}
		}
		e.consume(this);
	}
	void onHover(const HoverEvent& e) override {
		OpaqueWidget::onHover(e);
		lastPos = e.pos;
		if (module) hover = leafUnder(e.pos);
		e.consume(this);
	}
	void onLeave(const LeaveEvent& e) override { hover = -1; }
	void onDragHover(const DragHoverEvent& e) override {
		if (!module || grab == NONE || grab == ORBIT) return;
		Vec p(clamp(e.pos.x, 0.f, box.size.x), clamp(e.pos.y, 0.f, box.size.y));
		CanopyView V = view();
		if (grabbed >= 0 && grabbed < CP_MAXLEAVES) {
			CanopyLeaf& P = module->leaf[grabbed];
			if (grab == BODY) {
				float x, z;
				if (V.onLevel(p.x, p.y, P.cy, x, z)) { P.cx = clamp(x + grabX, -0.95f, 0.95f); P.cz = clamp(z + grabZ, -0.95f, 0.95f); }
			}
			else if (grab == HEIGHT) {
				P.cy = clamp(P.cy - (p.y - lastPos.y) / V.S, 0.3f, 2.4f);
			}
			else {
				// the end steers the leaf; its size comes from its form
				float sgn = grab == END_A ? 1.f : -1.f;
				float pl = std::max(std::sqrt(P.ax * P.ax + P.ay * P.ay + P.az * P.az), 1e-3f);
				float Ln = module->live[grabbed].L;
				float at[3] = {P.cx + sgn * P.ax / pl * Ln, P.cy + sgn * P.ay / pl * Ln, P.cz + sgn * P.az / pl * Ln};
				float hit[3];
				V.onFacing(p.x, p.y, at, hit);
				float ax = sgn * (hit[0] - P.cx), ay = sgn * (hit[1] - P.cy), az = sgn * (hit[2] - P.cz);
				float nl = std::sqrt(ax * ax + ay * ay + az * az);
				if (nl > 1e-3f) { P.ax = ax / nl; P.ay = ay / nl; P.az = az / nl; }
			}
			module->publishLeaves();
		}
		lastPos = p;
	}
	void onDragMove(const DragMoveEvent& e) override {
		if (!module || grab != ORBIT || e.button != GLFW_MOUSE_BUTTON_LEFT) return;
		Vec d = e.mouseDelta.div(APP->scene->rackScroll->getZoom());
		module->camYaw += d.x * 0.012f;
		module->camTilt = clamp(module->camTilt + d.y * 0.008f, 0.15f, 1.4f);
	}
	void onDragEnd(const DragEndEvent& e) override { grab = NONE; grabbed = -1; }
	// ── drawing ──────────────────────────────────────────────────────────
	// Two convex shells welded at the rim, a shape like a flying saucer, the top one
	// a plain dome with a hoop round it and the tongues cut into it as thin lines.
	void circleOn(NVGcontext* vg, const CanopyView& V, float r, float y) {
		nvgBeginPath(vg);
		for (int i = 0; i <= 72; i++) {
			float a = 2.f * (float)M_PI * (float)i / 72.f;
			CanopyView::Pt p = V.project(r * std::cos(a), y, r * std::sin(a));
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgClosePath(vg);
	}
	CanopyView::Pt onTongue(const CanopyView& V, const Canopy::TongueVoice& T, float a, float c, float dip) const {
		float x = T.tx + T.ux * a + T.uz * c, z = T.tz + T.uz * a - T.ux * c;
		return V.project(x, cpDomeY(x, z) - dip, z);
	}
	// the cut: up one side, round the free end, down the other
	void tonguePath(NVGcontext* vg, const CanopyView& V, const Canopy::TongueVoice& T, float dip) {
		nvgBeginPath(vg);
		CanopyView::Pt p = onTongue(V, T, T.len, -T.w, 0.f);
		nvgMoveTo(vg, p.x, p.y);
		for (int i = 0; i <= 12; i++) {
			float th = (float)M_PI * (float)i / 12.f;
			float a = T.w - T.w * std::sin(th), c = -T.w * std::cos(th);
			CanopyView::Pt q = onTongue(V, T, a, c, dip);
			nvgLineTo(vg, q.x, q.y);
		}
		p = onTongue(V, T, T.len, T.w, 0.f);
		nvgLineTo(vg, p.x, p.y);
	}
	void drawDrum(NVGcontext* vg, const CanopyView& V) {
		const Canopy& M = *module;
		const CpPal P = cpPal();
		// the lower shell, hanging under the rim
		static const float LOW_R[4] = {0.98f, 0.86f, 0.62f, 0.34f}, LOW_Y[4] = {-0.05f, -0.2f, -0.34f, -0.40f};
		nvgFillColor(vg, P.shell);
		for (int k = 0; k < 4; k++) { circleOn(vg, V, LOW_R[k], LOW_Y[k]); nvgFill(vg); }
		for (int k = 0; k < 3; k++) {
			for (int i = 0; i < 48; i++) {
				float a0 = 2.f * (float)M_PI * (float)i / 48.f, a1 = 2.f * (float)M_PI * (float)(i + 1) / 48.f;
				CanopyView::Pt p0 = V.project(LOW_R[k] * std::cos(a0), LOW_Y[k], LOW_R[k] * std::sin(a0));
				CanopyView::Pt p1 = V.project(LOW_R[k] * std::cos(a1), LOW_Y[k], LOW_R[k] * std::sin(a1));
				CanopyView::Pt q0 = V.project(LOW_R[k + 1] * std::cos(a0), LOW_Y[k + 1], LOW_R[k + 1] * std::sin(a0));
				CanopyView::Pt q1 = V.project(LOW_R[k + 1] * std::cos(a1), LOW_Y[k + 1], LOW_R[k + 1] * std::sin(a1));
				nvgBeginPath(vg); nvgMoveTo(vg, p0.x, p0.y); nvgLineTo(vg, p1.x, p1.y); nvgLineTo(vg, q1.x, q1.y); nvgLineTo(vg, q0.x, q0.y); nvgClosePath(vg);
				nvgFill(vg);
			}
		}
		circleOn(vg, V, 1.f, 0.f);
		nvgFillColor(vg, P.top);
		nvgFill(vg);

		for (int t = 0; t < M.nT; t++) {
			const Canopy::TongueVoice& T = M.tv[t];
			float glow = clamp(T.env * 1.3f, 0.f, 1.f);
			float dip = clamp(T.env * 0.05f, 0.f, 0.035f);
			// ringing, a tongue goes from grey where it joins the drum to orange at its free end
			CanopyView::Pt gRoot = onTongue(V, T, T.len, 0.f, 0.f), gTip = onTongue(V, T, 0.f, 0.f, dip);
			if (glow > 0.01f) {
				tonguePath(vg, V, T, dip);
				nvgClosePath(vg);
				nvgFillPaint(vg, nvgLinearGradient(vg, gRoot.x, gRoot.y, gTip.x, gTip.y,
					nvgRGBAf(0.62f, 0.62f, 0.65f, 0.55f * glow), nvgRGBAf(CP_HOT.r, CP_HOT.g, CP_HOT.b, 0.75f * glow)));
				nvgFill(vg);
			}
			NVGcolor ink = nvgLerpRGBA(P.ink, CP_HOT, glow);
			NVGpaint line = nvgLinearGradient(vg, gRoot.x, gRoot.y, gTip.x, gTip.y, P.ink, ink);
			tonguePath(vg, V, T, dip);
			nvgStrokePaint(vg, line); nvgStrokeWidth(vg, 0.9f);
			nvgStroke(vg);
			// the tuning slits, hanging from the round end at lengths that differ
			nvgStrokeWidth(vg, 0.7f);
			for (int k = 0; k < 3; k++) {
				float c = T.w * (-0.5f + 0.5f * (float)k);
				float a0 = T.w - std::sqrt(std::max(T.w * T.w - c * c, 0.f));
				float reach = 0.34f + 0.26f * (float)(((t + 1) * (k + 2) * 7) % 5) / 4.f;
				float a1 = a0 + T.len * reach;
				CanopyView::Pt p0 = onTongue(V, T, a0, c, dip), p1 = onTongue(V, T, a1, c, dip * 0.5f);
				nvgBeginPath(vg); nvgMoveTo(vg, p0.x, p0.y); nvgLineTo(vg, p1.x, p1.y); nvgStroke(vg);
			}
			if (t == 0) {
				// the big tongue's relief holes
				for (float s : {-1.f, 1.f}) {
					CanopyView::Pt h = onTongue(V, T, T.len, s * T.w, 0.f);
					nvgBeginPath(vg); nvgCircle(vg, h.x, h.y, 1.5f);
					nvgFillColor(vg, P.ink); nvgFill(vg);
				}
			}
		}
		// the hoop
		circleOn(vg, V, 1.f, 0.f);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 2.2f);
		nvgStroke(vg);
	}
	// The zither: a board leaning toward the viewer, a rose round the sound hole,
	// two bridges, tuning pins behind the back one, and the strings, which go from
	// ink to orange at the front while they ring, and thicken with it.
	void drawZither(NVGcontext* vg, const CanopyView& V) {
		const Canopy& M = *module;
		const CpPal P = cpPal();
		const int n = M.nT;
		const float half = 0.5f * CP_Z_SP * (float)(n - 1);
		const float xL = -half - CP_Z_EDGE, xR = half + CP_Z_EDGE;
		const float zB = CP_Z_BACK - CP_Z_EDGE - 0.08f;
		const float zFL = M.zitherFront(xL), zFR = M.zitherFront(xR);
		const float TH = 0.07f;                         // the board's thickness
		// round joins: at the board's sharp corners a mitred outline overshoots them
		nvgSave(vg);
		nvgLineJoin(vg, NVG_ROUND);
		nvgLineCap(vg, NVG_ROUND);
		const float cx[4] = {xL, xR, xR, xL}, cz[4] = {zB, zB, zFR, zFL};
		auto pt = [&](int i, float dy) { return V.project(cx[i], cpBoardY(cz[i]) + dy, cz[i]); };
		// the sides that face the camera, then the top over them: a side facing
		// away is behind the board, and its lower edge would show through
		float view[3];
		V.fromView(0.f, 0.f, 1.f, view);
		nvgFillColor(vg, P.shell);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f);
		for (int i = 0; i < 4; i++) {
			int j = (i + 1) % 4;
			// the corners run round clockwise seen from above, so (ez, -ex) points out
			float ex = cx[j] - cx[i], ez = cz[j] - cz[i];
			if (ez * view[0] - ex * view[2] <= 0.f) continue;
			CanopyView::Pt a = pt(i, 0.f), b = pt(j, 0.f), c = pt(j, -TH), d = pt(i, -TH);
			nvgBeginPath(vg); nvgMoveTo(vg, a.x, a.y); nvgLineTo(vg, b.x, b.y); nvgLineTo(vg, c.x, c.y); nvgLineTo(vg, d.x, d.y); nvgClosePath(vg);
			nvgFill(vg); nvgStroke(vg);
		}
		nvgBeginPath(vg);
		for (int i = 0; i < 4; i++) { CanopyView::Pt a = pt(i, 0.f); if (i == 0) nvgMoveTo(vg, a.x, a.y); else nvgLineTo(vg, a.x, a.y); }
		nvgClosePath(vg);
		nvgFillColor(vg, P.top); nvgFill(vg);
		nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
		// the sound hole and its rose
		for (int k = 0; k < 2; k++) {
			float rr = k == 0 ? 0.13f : 0.17f;
			nvgBeginPath(vg);
			for (int i = 0; i <= 40; i++) {
				float a = 2.f * (float)M_PI * (float)i / 40.f;
				float x = rr * std::cos(a), z = -0.30f + rr * std::sin(a);
				CanopyView::Pt q = V.project(x, cpBoardY(z) + 0.001f, z);
				if (i == 0) nvgMoveTo(vg, q.x, q.y); else nvgLineTo(vg, q.x, q.y);
			}
			nvgClosePath(vg);
			if (k == 0) { nvgFillColor(vg, P.shell); nvgFill(vg); }
			nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		}
		// tuning pins
		for (int t = 0; t < n; t++) {
			float z = CP_Z_BACK - 0.06f;
			CanopyView::Pt q = V.project(M.tv[t].tx, cpBoardY(z) + 0.01f, z);
			nvgBeginPath(vg); nvgCircle(vg, q.x, q.y, 1.4f);
			nvgFillColor(vg, P.ink); nvgFill(vg);
		}
		// the bridges, under the strings
		auto bridge = [&](float x0, float z0, float x1, float z1) {
			CanopyView::Pt a = V.project(x0, cpBoardY(z0) + CP_Z_H, z0), b = V.project(x1, cpBoardY(z1) + CP_Z_H, z1);
			CanopyView::Pt a0 = V.project(x0, cpBoardY(z0), z0), b0 = V.project(x1, cpBoardY(z1), z1);
			nvgBeginPath(vg); nvgMoveTo(vg, a.x, a.y); nvgLineTo(vg, b.x, b.y); nvgLineTo(vg, b0.x, b0.y); nvgLineTo(vg, a0.x, a0.y); nvgClosePath(vg);
			nvgFillColor(vg, P.shell); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		};
		bridge(-half - 0.04f, CP_Z_BACK, half + 0.04f, CP_Z_BACK);
		bridge(-half - 0.04f, CP_Z_FRONT0, half + 0.04f, CP_Z_FRONT1);
		// the strings
		for (int t = 0; t < n; t++) {
			const Canopy::TongueVoice& T = M.tv[t];
			float glow = clamp(T.env * 1.3f, 0.f, 1.f);
			float z1 = T.tz + T.len;
			CanopyView::Pt a = V.project(T.tx, cpBoardY(T.tz) + CP_Z_H, T.tz);
			CanopyView::Pt b = V.project(T.tx, cpBoardY(z1) + CP_Z_H, z1);
			NVGcolor hot = nvgLerpRGBA(P.ink, CP_HOT, glow);
			nvgBeginPath(vg); nvgMoveTo(vg, a.x, a.y); nvgLineTo(vg, b.x, b.y);
			nvgStrokePaint(vg, nvgLinearGradient(vg, a.x, a.y, b.x, b.y, P.ink, hot));
			nvgStrokeWidth(vg, 0.7f + 1.6f * glow + 0.25f * (float)(n - 1 - t) / (float)std::max(n - 1, 1));
			nvgStroke(vg);
		}
		nvgRestore(vg);
	}

	// The singing bowls, farthest first: each on a post to the floor, its outside,
	// the inside seen over the rim, any water in it, and the rim, which goes from
	// ink to orange while the bowl rings.
	// The run of the NS steps round a ring that pass `ok`, as a start and a count of
	// steps, so a visible surface can be filled as ONE polygon. Filled as many thin
	// strips, the anti-aliased edge of every strip shows as a seam.
	static void cpSpan(int NS, const std::function<bool(int)>& ok, int& start, int& count) {
		start = 0; count = 0;
		int any = -1;
		for (int i = 0; i < NS; i++) if (ok(i)) { any = i; if (!ok((i + NS - 1) % NS)) { start = i; break; } }
		if (any < 0) return;
		if (ok((start + NS - 1) % NS) && start == 0 && ok(0)) {
			bool all = true;
			for (int i = 0; i < NS; i++) all = all && ok(i);
			if (all) { count = NS; return; }
		}
		while (count < NS && ok((start + count) % NS)) count++;
	}
	// a drop down in the pan's bowl, below its rim, is drawn with the pan
	bool inPan(const CanopyDrop& d) const {
		return module->instNow == 2 && d.x * d.x + d.z * d.z <= 1.f && d.y < -CP_PAN_T * d.z;
	}
	// The cords the bars hang on: through each bar at its nodes, 0.224 of its length
	// in from either end, where the fundamental does not move.
	void drawBarFrame(NVGcontext* vg, const CanopyView& V) {
		const Canopy& M = *module;
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgLineJoin(vg, NVG_ROUND); nvgLineCap(vg, NVG_ROUND);
		nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 1.f);
		for (int row = 0; row < 2; row++) for (int end = 0; end < 2; end++) {
			nvgBeginPath(vg);
			bool first = true;
			for (int t = 0; t < M.nT; t++) {
				const Canopy::TongueVoice& T = M.tv[t];
				if (T.ring != row) continue;
				float a = (end ? 1.f : -1.f) * (0.5f - 0.224f) * T.len;
				CanopyView::Pt p = V.project(T.tx + a * T.ux, T.xy - CP_X_TH, T.tz + a * T.uz);
				if (first) { nvgMoveTo(vg, p.x, p.y); first = false; } else nvgLineTo(vg, p.x, p.y);
			}
			nvgStroke(vg);
		}
		nvgRestore(vg);
	}
	// A bar, with its resonator tube (marimba) or gourd (balafon) under its outer
	// half. The faces of the block that face the camera are drawn, then the top,
	// which turns orange while it rings; a bar SCALE has damped is drawn faint.
	void drawBar(NVGcontext* vg, const CanopyView& V, int t) {
		const Canopy& M = *module;
		const Canopy::TongueVoice& T = M.tv[t];
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgLineJoin(vg, NVG_ROUND); nvgLineCap(vg, NVG_ROUND);
		float view[3];
		V.fromView(0.f, 0.f, 1.f, view);
		const float glow = clamp(T.env * 1.3f, 0.f, 1.f);
		const float yt = T.xy, yb = T.xy - CP_X_TH;
		const float k = T.len / CP_X_LEN;
		// along the bar (ux, uz) and across it (uz, -ux)
		auto at = [&](float along, float across, float y) {
			return V.project(T.tx + along * T.ux + across * T.uz, y, T.tz + along * T.uz - across * T.ux);
		};
		const float hl = 0.5f * T.len, hw = T.xw, under = 0.25f * T.len;
		const NVGcolor edge = T.damped ? P.line : P.ink;
		if (M.instNow == 8) {
			// the gourd, its membrane hole facing out
			float gr = 0.85f * T.xw + 0.06f * k;
			float gx = T.tx + under * T.ux, gz = T.tz + under * T.uz;
			CanopyView::Pt c = V.project(gx, yb - 0.06f - gr, gz);
			nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, gr * c.scale);
			nvgFillColor(vg, P.shell); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
			CanopyView::Pt h = V.project(gx, yb - 0.06f - gr, gz + 0.8f * gr);
			nvgBeginPath(vg); nvgEllipse(vg, h.x, h.y, 0.32f * gr * h.scale, 0.22f * gr * h.scale);
			nvgFillColor(vg, P.line); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
		}
		else {
			// the resonator tube, a quarter wavelength long, so the low ones hang deepest
			float rr = 0.75f * T.xw, depth = 0.16f + 0.42f * k * k;
			float rx = T.tx + under * T.ux, rz = T.tz + under * T.uz;
			CanopyView::Pt a = V.project(rx, yb - 0.02f, rz), b = V.project(rx, yb - 0.02f - depth, rz);
			float wa = rr * a.scale, wb = rr * b.scale;
			nvgBeginPath(vg);
			nvgMoveTo(vg, a.x - wa, a.y); nvgLineTo(vg, b.x - wb, b.y);
			nvgBezierTo(vg, b.x - wb, b.y + 0.5f * wb * V.sp, b.x + wb, b.y + 0.5f * wb * V.sp, b.x + wb, b.y);
			nvgLineTo(vg, a.x + wa, a.y); nvgClosePath(vg);
			nvgFillColor(vg, P.shell); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		}
		auto quad = [&](CanopyView::Pt a, CanopyView::Pt b, CanopyView::Pt c, CanopyView::Pt d, NVGcolor fill) {
			nvgBeginPath(vg); nvgMoveTo(vg, a.x, a.y); nvgLineTo(vg, b.x, b.y); nvgLineTo(vg, c.x, c.y); nvgLineTo(vg, d.x, d.y); nvgClosePath(vg);
			nvgFillColor(vg, fill); nvgFill(vg);
			nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		};
		// which end and which side face the camera
		float endS = T.ux * view[0] + T.uz * view[2] > 0.f ? 1.f : -1.f;
		float sideS = T.uz * view[0] - T.ux * view[2] > 0.f ? 1.f : -1.f;
		quad(at(-hl, sideS * hw, yt), at(hl, sideS * hw, yt), at(hl, sideS * hw, yb), at(-hl, sideS * hw, yb), P.shell);
		quad(at(endS * hl, -hw, yt), at(endS * hl, hw, yt), at(endS * hl, hw, yb), at(endS * hl, -hw, yb), P.shell);
		NVGcolor top = T.damped ? nvgLerpRGBA(P.top, P.shell, 0.6f) : P.top;
		quad(at(-hl, -hw, yt), at(hl, -hw, yt), at(hl, hw, yt), at(-hl, hw, yt), nvgLerpRGBA(top, CP_HOT, glow * 0.8f));
		nvgRestore(vg);
	}
	// The bonang's frame: a low wooden box whose top is level with the cords, its
	// front board, and two cords along each row that the kettles sit on.
	void drawBonangFrame(NVGcontext* vg, const CanopyView& V) {
		const Canopy& M = *module;
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgLineJoin(vg, NVG_ROUND); nvgLineCap(vg, NVG_ROUND);
		const float W = CP_BN_W, D = CP_BN_D, y0 = CP_BN_BASE, y1 = CP_BN_BASE - 0.16f;
		auto P3 = [&](float x, float y, float z) { return V.project(x, y, z); };
		// the inside floor, seen through the top
		CanopyView::Pt a = P3(-W, y0, -D), b = P3(W, y0, -D), c = P3(W, y0, D), dd = P3(-W, y0, D);
		nvgBeginPath(vg); nvgMoveTo(vg, a.x, a.y); nvgLineTo(vg, b.x, b.y); nvgLineTo(vg, c.x, c.y); nvgLineTo(vg, dd.x, dd.y); nvgClosePath(vg);
		nvgFillColor(vg, P.shell); nvgFill(vg);
		// the cords
		nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.8f);
		for (float z : {CP_BN_ROWZ, -CP_BN_ROWZ}) for (float off : {-0.06f, 0.06f}) {
			CanopyView::Pt p = P3(-W, y0 + 0.004f, z + off), q = P3(W, y0 + 0.004f, z + off);
			nvgBeginPath(vg); nvgMoveTo(vg, p.x, p.y); nvgLineTo(vg, q.x, q.y); nvgStroke(vg);
		}
		// the front and the visible end, as boards down from the rim
		float view[3];
		V.fromView(0.f, 0.f, 1.f, view);
		auto board = [&](float xa, float za, float xb, float zb) {
			CanopyView::Pt p0 = P3(xa, y0, za), p1 = P3(xb, y0, zb), q1 = P3(xb, y1, zb), q0 = P3(xa, y1, za);
			nvgBeginPath(vg); nvgMoveTo(vg, p0.x, p0.y); nvgLineTo(vg, p1.x, p1.y); nvgLineTo(vg, q1.x, q1.y); nvgLineTo(vg, q0.x, q0.y); nvgClosePath(vg);
			nvgFillColor(vg, P.shell); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		};
		if (view[2] > 0.f) board(-W, D, W, D); else board(W, -D, -W, -D);
		if (view[0] > 0.f) board(W, D, W, -D); else board(-W, -D, -W, D);
		// the rim of the box
		nvgBeginPath(vg); nvgMoveTo(vg, a.x, a.y); nvgLineTo(vg, b.x, b.y); nvgLineTo(vg, c.x, c.y); nvgLineTo(vg, dd.x, dd.y); nvgClosePath(vg);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 1.4f); nvgStroke(vg);
		(void)M;
		nvgRestore(vg);
	}
	// A kettle: its side facing the camera, the shoulder, the flat top and the boss,
	// which turns orange while it rings.
	void drawKettle(NVGcontext* vg, const CanopyView& V, int t) {
		const Canopy& M = *module;
		const Canopy::TongueVoice& T = M.tv[t];
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgLineJoin(vg, NVG_ROUND); nvgLineCap(vg, NVG_ROUND);
		const float TAU = 2.f * (float)M_PI;
		const int NS = 48;
		float view[3];
		V.fromView(0.f, 0.f, 1.f, view);
		auto at = [&](float rad, float y, float a) { return V.project(T.tx + rad * std::sin(a), y, T.tz + rad * std::cos(a)); };
		auto facing = [&](float a) { return std::sin(a) * view[0] + std::cos(a) * view[2] > 0.f; };
		auto ring = [&](float rad, float y) {
			nvgBeginPath(vg);
			for (int i = 0; i < NS; i++) { CanopyView::Pt p = at(rad, y, TAU * (float)i / NS); if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
			nvgClosePath(vg);
		};
		const float R = T.bR, shoulder = T.bY - 0.3f * R;
		const float glow = clamp(T.env * 1.3f, 0.f, 1.f);
		int st, cnt;
		cpSpan(NS, [&](int i) { return facing(TAU * ((float)i + 0.5f) / NS); }, st, cnt);
		if (cnt > 0) {
			nvgBeginPath(vg);
			for (int j = 0; j <= cnt; j++) { CanopyView::Pt p = at(R, CP_BN_BASE, TAU * (float)(st + j) / NS); if (j == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
			for (int j = cnt; j >= 0; j--) { CanopyView::Pt p = at(R, shoulder, TAU * (float)(st + j) / NS); nvgLineTo(vg, p.x, p.y); }
			nvgClosePath(vg);
			nvgFillColor(vg, P.shell); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		}
		ring(R, shoulder);
		nvgFillColor(vg, P.shell); nvgFill(vg);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		ring(0.8f * R, T.bY);
		nvgFillColor(vg, P.top); nvgFill(vg);
		nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
		// the boss: its foot, then its crown a little higher
		ring(CP_BN_BOSS * R, T.bY);
		nvgFillColor(vg, nvgLerpRGBA(P.shell, CP_HOT, glow * 0.85f)); nvgFill(vg);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		ring(0.55f * CP_BN_BOSS * R, T.bY + 0.75f * CP_BN_BH * R);
		nvgFillColor(vg, nvgLerpRGBA(P.top, CP_HOT, glow * 0.85f)); nvgFill(vg);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
		nvgRestore(vg);
	}
	// The handpan. The shell is convex, so what can be seen of it is what faces the
	// camera: the bottom shell down to where it turns away, the top as one outline
	// (the hull of the dome as seen, which is the rim from above and the crest of
	// the dome from low down), and of each note only the part on the near side of
	// the crest. Drawn any other way, notes on the far slope showed through.
	void drawHand(NVGcontext* vg, const CanopyView& V) {
		const Canopy& M = *module;
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgLineJoin(vg, NVG_ROUND); nvgLineCap(vg, NVG_ROUND);
		const int NS = 72;
		const float TAU = 2.f * (float)M_PI;
		float view[3];
		V.fromView(0.f, 0.f, 1.f, view);
		auto ringAt = [&](float rad, float y, float a) { return V.project(rad * std::sin(a), y, rad * std::cos(a)); };
		// the bottom shell: from the rim curving in and down; at each angle, how far
		// round it before it faces away from the camera
		auto under = [&](float u, float& rad, float& y) {
			float q = u * 0.5f * (float)M_PI;
			rad = 1.f - 0.42f * (1.f - std::cos(q)); y = CP_HP_H0 - CP_HP_UNDER * std::sin(q);
		};
		auto faces = [&](float a, float u) {
			float q = u * 0.5f * (float)M_PI;
			float nr = CP_HP_UNDER * std::cos(q), ny = -0.42f * std::sin(q);     // outward normal in (radius, height)
			return nr * (std::sin(a) * view[0] + std::cos(a) * view[2]) + ny * view[1];
		};
		float uLow[NS + 1];
		for (int i = 0; i <= NS; i++) {
			float a = TAU * (float)i / NS;
			uLow[i] = -1.f;
			for (int k = 0; k <= 24; k++) { float u = (float)k / 24.f; if (faces(a, u) > 0.f) uLow[i] = u; else break; }
		}
		{
			int st, cnt;
			cpSpan(NS, [&](int i) { return uLow[i] >= 0.f && uLow[(i + 1) % NS] >= 0.f; }, st, cnt);
			if (cnt > 0) {
				auto low = [&](int j) { float rad, y; under(uLow[(st + j) % NS], rad, y); return ringAt(rad, y, TAU * (float)(st + j) / NS); };
				nvgBeginPath(vg);
				for (int j = 0; j <= cnt; j++) { CanopyView::Pt p = ringAt(1.f, CP_HP_H0, TAU * (float)(st + j) / NS); if (j == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
				for (int j = cnt; j >= 0; j--) { CanopyView::Pt p = low(j); nvgLineTo(vg, p.x, p.y); }
				nvgClosePath(vg);
				nvgFillColor(vg, P.shell); nvgFill(vg);
				nvgBeginPath(vg);
				for (int j = 0; j <= cnt; j++) { CanopyView::Pt p = low(j); if (j == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
				nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
			}
		}
		// the top: the convex hull of the dome as the camera sees it
		{
			std::vector<Vec> pts;
			pts.reserve(11 * 48 + 1);
			for (int k = 0; k <= 10; k++) {
				float rad = 1.f - 0.1f * (float)k;
				for (int i = 0; i < 48; i++) {
					float a = TAU * (float)i / 48.f, x = rad * std::sin(a), z = rad * std::cos(a);
					CanopyView::Pt p = V.project(x, M.handY(x, z), z);
					pts.push_back(Vec(p.x, p.y));
				}
			}
			std::sort(pts.begin(), pts.end(), [](const Vec& a, const Vec& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
			auto cross = [](const Vec& o, const Vec& a, const Vec& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
			std::vector<Vec> h(2 * pts.size());
			size_t kk = 0;
			for (size_t i = 0; i < pts.size(); i++) { while (kk >= 2 && cross(h[kk - 2], h[kk - 1], pts[i]) <= 0.f) kk--; h[kk++] = pts[i]; }
			for (size_t i = pts.size() - 1, t = kk + 1; i > 0; i--) { while (kk >= t && cross(h[kk - 2], h[kk - 1], pts[i - 1]) <= 0.f) kk--; h[kk++] = pts[i - 1]; }
			if (kk > 3) {
				nvgBeginPath(vg);
				for (size_t i = 0; i + 1 < kk; i++) { if (i == 0) nvgMoveTo(vg, h[i].x, h[i].y); else nvgLineTo(vg, h[i].x, h[i].y); }
				nvgClosePath(vg);
				nvgFillColor(vg, P.top); nvgFill(vg);
				nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 1.8f); nvgStroke(vg);
			}
		}
		// the notes, each only where its surface faces the camera
		auto facesTop = [&](float x, float z) {
			const float e = 0.004f;
			float gx = (M.handY(x + e, z) - M.handY(x - e, z)) / (2.f * e), gz = (M.handY(x, z + e) - M.handY(x, z - e)) / (2.f * e);
			return -gx * view[0] + view[1] - gz * view[2] > 0.f;
		};
		static const char* NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		std::shared_ptr<Font> font = sfs::panelFont();
		auto outline = [&](const Canopy::TongueVoice& T, float scale, bool& any, bool& all) {
			float ux = std::sin(T.pth), uz = std::cos(T.pth);
			nvgBeginPath(vg);
			bool first = true; any = false; all = true;
			for (int k = 0; k < 36; k++) {
				float ph = TAU * (float)k / 36.f;
				float ur = scale * T.pa * std::cos(ph), ut = scale * T.pb * std::sin(ph);
				float x = T.tx + ur * ux + ut * uz, z = T.tz + ur * uz - ut * ux;
				if (!facesTop(x, z)) { all = false; continue; }
				any = true;
				CanopyView::Pt p = V.project(x, M.handY(x, z) + 0.003f, z);
				if (first) { nvgMoveTo(vg, p.x, p.y); first = false; } else nvgLineTo(vg, p.x, p.y);
			}
			if (all) nvgClosePath(vg);
		};
		for (int t = 0; t < M.nT; t++) {
			const Canopy::TongueVoice& T = M.tv[t];
			float glow = clamp(T.env * 1.3f, 0.f, 1.f);
			bool any, all;
			outline(T, 1.f, any, all);
			if (!any) continue;
			nvgFillColor(vg, nvgLerpRGBA(t == 0 ? P.shell : P.top, CP_HOT, glow * 0.8f)); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, t == 0 ? 1.1f : 0.9f); nvgStroke(vg);
			if (t > 0) {
				// the dimple in the middle of a field
				bool a2, l2;
				outline(T, 0.3f, a2, l2);
				if (a2) { nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg); }
			}
			float ux = std::sin(T.pth), uz = std::cos(T.pth);
			float lx = t == 0 ? 0.f : T.tx + 0.62f * T.pa * ux, lz = t == 0 ? 0.f : T.tz + 0.62f * T.pa * uz;
			if (font && font->handle >= 0 && all && facesTop(lx, lz)) {
				// the name sits toward the rim on a field, in the middle of the ding
				CanopyView::Pt c = V.project(lx, M.handY(lx, lz), lz);
				int pc = (((int)std::round(T.semis)) % 12 + 12) % 12;
				nvgFontFaceId(vg, font->handle);
				nvgFontSize(vg, clamp(T.pa * c.scale * 0.42f, 5.f, 9.f));
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, P.ink);
				nvgText(vg, c.x, c.y, NN[pc], NULL);
			}
		}
		nvgRestore(vg);
	}
	// The wind chimes' ring, the cords it hangs by, and the cord from each tube's top.
	void drawChimeFrame(NVGcontext* vg, const CanopyView& V) {
		const Canopy& M = *module;
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgLineCap(vg, NVG_ROUND); nvgLineJoin(vg, NVG_ROUND);
		const float hook = CP_CH_HUB + 0.32f;
		CanopyView::Pt H = V.project(0.f, hook, 0.f), top = V.project(0.f, CP_H, 0.f);
		nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.8f);
		nvgBeginPath(vg); nvgMoveTo(vg, H.x, H.y); nvgLineTo(vg, top.x, top.y); nvgStroke(vg);
		for (int k = 0; k < 3; k++) {
			float a = 2.f * (float)M_PI * (float)k / 3.f + 0.3f;
			CanopyView::Pt q = V.project(CP_CH_RING * std::sin(a), CP_CH_HUB, CP_CH_RING * std::cos(a));
			nvgBeginPath(vg); nvgMoveTo(vg, H.x, H.y); nvgLineTo(vg, q.x, q.y); nvgStroke(vg);
		}
		// the clapper's cord, from the middle of the ring
		{
			float c[3]; M.clap.at(CP_CH_SAIL, c);
			CanopyView::Pt a = V.project(0.f, CP_CH_HUB, 0.f), b = V.project(c[0], c[1], c[2]);
			nvgBeginPath(vg); nvgMoveTo(vg, H.x, H.y); nvgLineTo(vg, a.x, a.y); nvgLineTo(vg, b.x, b.y); nvgStroke(vg);
		}
		nvgBeginPath(vg);
		for (int i = 0; i < 72; i++) {
			float a = 2.f * (float)M_PI * (float)i / 72.f;
			CanopyView::Pt p = V.project(CP_CH_RING * std::sin(a), CP_CH_HUB, CP_CH_RING * std::cos(a));
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgClosePath(vg);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 1.6f); nvgStroke(vg);
		nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.7f);
		for (int t = 0; t < M.nT; t++) {
			float a[3]; M.tv[t].pend.at(CP_CH_STR, a);
			CanopyView::Pt p = V.project(M.tv[t].pend.px, CP_CH_HUB, M.tv[t].pend.pz), q = V.project(a[0], a[1], a[2]);
			nvgBeginPath(vg); nvgMoveTo(vg, p.x, p.y); nvgLineTo(vg, q.x, q.y); nvgStroke(vg);
		}
		nvgRestore(vg);
	}
	// A tube, as a cylinder seen side on: two edges, the open ends as ellipses, filled
	// and turning toward orange while it rings. Index nT is the clapper and the sail.
	void drawChimePiece(NVGcontext* vg, const CanopyView& V, int t) {
		const Canopy& M = *module;
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgLineCap(vg, NVG_ROUND); nvgLineJoin(vg, NVG_ROUND);
		auto disc = [&](const float c[3], const float up[3], float rad, int n) {
			// a circle of radius rad round c, in the plane whose normal is up
			float ex[3] = {1.f, 0.f, 0.f};
			float dt = ex[0] * up[0] + ex[1] * up[1] + ex[2] * up[2];
			float e1[3] = {ex[0] - dt * up[0], ex[1] - dt * up[1], ex[2] - dt * up[2]};
			float l = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
			for (float& x : e1) x /= l;
			float e2[3] = {up[1] * e1[2] - up[2] * e1[1], up[2] * e1[0] - up[0] * e1[2], up[0] * e1[1] - up[1] * e1[0]};
			nvgBeginPath(vg);
			for (int i = 0; i < n; i++) {
				float a = 2.f * (float)M_PI * (float)i / (float)n, ca = std::cos(a) * rad, sa = std::sin(a) * rad;
				CanopyView::Pt p = V.project(c[0] + ca * e1[0] + sa * e2[0], c[1] + ca * e1[1] + sa * e2[1], c[2] + ca * e1[2] + sa * e2[2]);
				if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
			}
			nvgClosePath(vg);
		};
		if (t >= M.nT) {
			float C[3], S[3], d[3], up[3];
			M.clap.at(CP_CH_CLAP, C); M.clap.at(CP_CH_SAIL, S); M.clap.dir(d);
			for (int i = 0; i < 3; i++) up[i] = -d[i];
			disc(S, up, CP_CH_SR, 40);
			nvgFillColor(vg, P.shell); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
			// the clapper: a wooden puck, its side and then its top
			const float hh = 0.045f;
			float lo[3] = {C[0] - d[0] * hh, C[1] - d[1] * hh, C[2] - d[2] * hh}, hi[3] = {C[0] + d[0] * hh, C[1] + d[1] * hh, C[2] + d[2] * hh};
			disc(hi, up, CP_CH_CLR, 40);
			nvgFillColor(vg, P.shell); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
			disc(lo, up, CP_CH_CLR, 40);
			nvgFillColor(vg, P.top); nvgFill(vg);
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
			nvgRestore(vg);
			return;
		}
		const Canopy::TongueVoice& T = M.tv[t];
		float a[3], b[3], d[3];
		T.pend.at(CP_CH_STR, a); T.pend.at(CP_CH_STR + T.len, b); T.pend.dir(d);
		CanopyView::Pt pa = V.project(a[0], a[1], a[2]), pb = V.project(b[0], b[1], b[2]);
		float dx = pb.x - pa.x, dy = pb.y - pa.y, dl = std::sqrt(dx * dx + dy * dy) + 1e-6f;
		float nx = -dy / dl, ny = dx / dl, wa = CP_CH_TR * pa.scale, wb = CP_CH_TR * pb.scale;
		const float glow = clamp(T.env * 1.3f, 0.f, 1.f);
		NVGcolor fill = nvgLerpRGBA(P.shell, CP_HOT, glow * 0.85f);
		// the lower end, then the side, then the top's opening over it
		float up[3] = {-d[0], -d[1], -d[2]};
		disc(b, up, CP_CH_TR, 16);
		nvgFillColor(vg, fill); nvgFill(vg);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		nvgBeginPath(vg);
		nvgMoveTo(vg, pa.x + nx * wa, pa.y + ny * wa); nvgLineTo(vg, pb.x + nx * wb, pb.y + ny * wb);
		nvgLineTo(vg, pb.x - nx * wb, pb.y - ny * wb); nvgLineTo(vg, pa.x - nx * wa, pa.y - ny * wa);
		nvgClosePath(vg);
		nvgFillColor(vg, fill); nvgFill(vg);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f);
		nvgBeginPath(vg); nvgMoveTo(vg, pa.x + nx * wa, pa.y + ny * wa); nvgLineTo(vg, pb.x + nx * wb, pb.y + ny * wb); nvgStroke(vg);
		nvgBeginPath(vg); nvgMoveTo(vg, pa.x - nx * wa, pa.y - ny * wa); nvgLineTo(vg, pb.x - nx * wb, pb.y - ny * wb); nvgStroke(vg);
		// a highlight line down the tube, so it reads as round metal
		nvgStrokeColor(vg, nvgTransRGBA(P.top, 0xC0)); nvgStrokeWidth(vg, std::max(0.6f, 0.25f * wa));
		nvgBeginPath(vg); nvgMoveTo(vg, pa.x + nx * wa * 0.35f, pa.y + ny * wa * 0.35f); nvgLineTo(vg, pb.x + nx * wb * 0.35f, pb.y + ny * wb * 0.35f); nvgStroke(vg);
		disc(a, up, CP_CH_TR, 16);
		nvgFillColor(vg, P.top); nvgFill(vg);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		nvgRestore(vg);
	}
	// The table the bowls stand on, as its edge.
	void drawTable(NVGcontext* vg, const CanopyView& V) {
		const CpPal P = cpPal();
		nvgBeginPath(vg);
		for (int i = 0; i < 72; i++) {
			float a = 2.f * (float)M_PI * (float)i / 72.f;
			CanopyView::Pt p = V.project(CP_BOWL_TABLE * std::sin(a), CP_BOWL_FLOOR, CP_BOWL_TABLE * std::cos(a));
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgClosePath(vg);
		nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
	}
	// One bowl: its cushion, the inside seen through the mouth with any water and
	// drops in it, then the outside facing the camera, which hides the near part of
	// the inside, and the lip, which turns orange as the bowl rings.
	void drawBowl(NVGcontext* vg, const CanopyView& V, int t) {
		const Canopy& M = *module;
		const Canopy::TongueVoice& T = M.tv[t];
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgLineJoin(vg, NVG_ROUND); nvgLineCap(vg, NVG_ROUND);
		const float TAU = 2.f * (float)M_PI;
		const int NS = 96, NH = 12;
		const float R = T.bR, base = T.bY - T.bD;
		float view[3];
		V.fromView(0.f, 0.f, 1.f, view);
		auto at = [&](float rad, float y, float a) { return V.project(T.tx + rad * std::sin(a), y, T.tz + rad * std::cos(a)); };
		auto facing = [&](float a) { return std::sin(a) * view[0] + std::cos(a) * view[2] > 0.f; };
		auto ring = [&](float rad, float y) {
			nvgBeginPath(vg);
			for (int i = 0; i < NS; i++) { CanopyView::Pt p = at(rad, y, TAU * (float)i / NS); if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
			nvgClosePath(vg);
		};
		// the front half of a ring, from where it turns toward the camera to where it turns away
		auto frontArc = [&](float rad, float y) {
			nvgBeginPath(vg);
			bool on = false;
			for (int i = 0; i <= 2 * NS; i++) {
				float a = TAU * (float)i / NS;
				if (!facing(a)) { if (on && i > NS) break; on = false; continue; }
				CanopyView::Pt p = at(rad, y, a);
				if (!on) { nvgMoveTo(vg, p.x, p.y); on = true; } else nvgLineTo(vg, p.x, p.y);
			}
		};
		auto band = [&](float r0, float y0, float r1, float y1, NVGcolor fill) {
			// the side of a solid of revolution between two rings, where it faces the camera
			int st, cnt;
			cpSpan(NS, [&](int i) { return facing(TAU * ((float)i + 0.5f) / NS); }, st, cnt);
			if (cnt == 0) return;
			nvgBeginPath(vg);
			for (int j = 0; j <= cnt; j++) { CanopyView::Pt p = at(r0, y0, TAU * (float)(st + j) / NS); if (j == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
			for (int j = cnt; j >= 0; j--) { CanopyView::Pt p = at(r1, y1, TAU * (float)(st + j) / NS); nvgLineTo(vg, p.x, p.y); }
			nvgClosePath(vg);
			nvgFillColor(vg, fill); nvgFill(vg);
		};
		const float glow = clamp(T.env * 1.3f, 0.f, 1.f);
		// the cushion: a ring stuffed round, the bowl's foot sitting in it
		const NVGcolor cush = P.line;
		const float cr = 0.84f * R, cm = 0.5f * (CP_BOWL_FLOOR + base);
		band(0.72f * cr, CP_BOWL_FLOOR, cr, cm, cush);
		band(cr, cm, 0.8f * cr, base + 0.01f, cush);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.6f);
		frontArc(cr, cm); nvgStroke(vg);
		// the inside through the mouth
		const float mouth = (1.f - CP_BOWL_WALL) * R, lipY = T.bY;
		ring(mouth, lipY);
		nvgFillColor(vg, P.top); nvgFill(vg);
		float floorY = base + CP_BOWL_BASE * R;
		ring(cpBowlInner(CP_BOWL_BASE) * R, floorY);
		nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
		if (T.water > 0.01f) {
			float hw = Canopy::waterH(T.water);
			ring(cpBowlInner(hw) * R, base + hw * R);
			nvgFillColor(vg, nvgTransRGBA(sfs::SCREEN_BLUE, 0x50)); nvgFill(vg);
			nvgStrokeColor(vg, nvgTransRGBA(sfs::SCREEN_BLUE, 0xA0)); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
		}
		for (const CanopyDrop& d : M.drop) {
			if (!d.alive || M.bowlHollow(d.x, d.y, d.z) != t) continue;
			// its shadow on the floor, then the drop
			float dx = d.x - T.tx, dz = d.z - T.tz;
			if (dx * dx + dz * dz < cpBowlInner(CP_BOWL_BASE) * cpBowlInner(CP_BOWL_BASE) * R * R) {
				CanopyView::Pt p = V.project(d.x, floorY, d.z);
				nvgBeginPath(vg); nvgEllipse(vg, p.x, p.y, d.r * p.scale, d.r * p.scale * V.sp);
				nvgFillColor(vg, P.shadeDrop); nvgFill(vg);
			}
			drawDrop(vg, V, d);
		}
		// The outside, only where it faces the camera: seen from above, the underside
		// of the belly faces away and is hidden, so at each angle the wall is drawn
		// from where it turns toward the camera up to the lip, and that lower edge,
		// round the front, is the bowl's outline.
		auto facesAt = [&](float a, float h) {
			const float e = 0.01f;
			float slope = (cpBowlOuter(h + e) - cpBowlOuter(h - e)) / (2.f * e);   // dr/dh
			return (std::sin(a) * view[0] + std::cos(a) * view[2]) - slope * view[1];
		};
		// the lowest height at angle a where the wall faces the camera, or -1 if none does
		auto lowest = [&](float a) {
			float prev = facesAt(a, 0.f);
			if (prev > 0.f) return 0.f;
			for (int k = 1; k <= 4 * NH; k++) {
				float h = CP_BOWL_HT * (float)k / (4 * NH), f = facesAt(a, h);
				if (f > 0.f) return h - CP_BOWL_HT / (4 * NH) * f / (f - prev);
				prev = f;
			}
			return -1.f;
		};
		float hLow[NS + 1];
		for (int i = 0; i <= NS; i++) hLow[i] = lowest(TAU * (float)i / NS);
		int st, cnt;
		cpSpan(NS, [&](int i) { return hLow[i] >= 0.f && hLow[(i + 1) % NS] >= 0.f; }, st, cnt);
		if (cnt > 0) {
			auto low = [&](int j) {
				int i = (st + j) % NS;
				float h = hLow[i];
				return at(cpBowlOuter(h) * R, base + h * R, TAU * (float)(st + j) / NS);
			};
			// the wall: up from its lower edge round the front, back along the lip
			nvgBeginPath(vg);
			for (int j = 0; j <= cnt; j++) { CanopyView::Pt p = low(j); if (j == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
			for (int j = cnt; j >= 0; j--) { CanopyView::Pt p = at(R, lipY, TAU * (float)(st + j) / NS); nvgLineTo(vg, p.x, p.y); }
			nvgClosePath(vg);
			nvgFillColor(vg, P.shell); nvgFill(vg);
			nvgBeginPath(vg);
			for (int j = 0; j <= cnt; j++) { CanopyView::Pt p = low(j); if (j == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
			nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		}
		// the two lines turned into the wall under the lip
		nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.6f);
		for (float h : {0.88f * CP_BOWL_HT, 0.83f * CP_BOWL_HT}) { frontArc(cpBowlOuter(h) * R, base + h * R); nvgStroke(vg); }
		// the lip
		ring(mouth, lipY);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
		ring(R, lipY);
		nvgStrokeColor(vg, nvgLerpRGBA(P.ink, CP_HOT, glow)); nvgStrokeWidth(vg, 1.4f + 1.2f * glow); nvgStroke(vg);
		nvgRestore(vg);
	}

	// The steel pan: its skirt, the bowl inside the rim with grooves between the
	// rings, and the pads with their notes painted on, as real pans have them.
	// A ringing pad fills toward orange; one SCALE has damped is drawn faint.
	void drawPan(NVGcontext* vg, const CanopyView& V) {
		const Canopy& M = *module;
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgLineJoin(vg, NVG_ROUND); nvgLineCap(vg, NVG_ROUND);
		const int NS = 72;
		const float TAU = 2.f * (float)M_PI;
		float view[3];
		V.fromView(0.f, 0.f, 1.f, view);
		auto rim = [&](float a, float dy) {
			float x = std::sin(a), z = std::cos(a);
			return V.project(x, -CP_PAN_T * z + dy, z);
		};
		auto facing = [&](float a) { return std::sin(a) * view[0] + std::cos(a) * view[2] > 0.f; };
		// the bowl inside the rim
		nvgBeginPath(vg);
		for (int i = 0; i < NS; i++) { CanopyView::Pt p = rim(TAU * (float)i / NS, 0.f); if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
		nvgClosePath(vg);
		nvgFillColor(vg, P.top); nvgFill(vg);
		// the grooves between the rings
		nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.7f);
		for (float rr : {0.63f, 0.33f}) {
			nvgBeginPath(vg);
			for (int i = 0; i <= NS; i++) {
				float a = TAU * (float)i / NS, x = rr * std::sin(a), z = rr * std::cos(a);
				CanopyView::Pt p = V.project(x, cpPanY(x, z) + 0.002f, z);
				if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
			}
			nvgStroke(vg);
		}
		// the pads
		static const char* NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		std::shared_ptr<Font> font = sfs::panelFont();
		for (int t = 0; t < M.nT; t++) {
			const Canopy::TongueVoice& T = M.tv[t];
			float glow = clamp(T.env * 1.3f, 0.f, 1.f);
			float ux = std::sin(T.pth), uz = std::cos(T.pth);
			nvgBeginPath(vg);
			for (int k = 0; k < 32; k++) {
				float ph = TAU * (float)k / 32.f;
				float ur = T.pa * std::cos(ph), ut = T.pb * std::sin(ph);
				float x = T.tx + ur * ux + ut * uz, z = T.tz + ur * uz - ut * ux;
				CanopyView::Pt p = V.project(x, cpPanY(x, z) + 0.003f, z);
				if (k == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
			}
			nvgClosePath(vg);
			if (glow > 0.01f) { nvgFillColor(vg, nvgLerpRGBA(P.top, CP_HOT, glow * 0.8f)); nvgFill(vg); }
			nvgStrokeColor(vg, T.damped ? P.line : P.ink);
			nvgStrokeWidth(vg, T.damped ? 0.6f : 0.9f);
			nvgStroke(vg);
			if (font && font->handle >= 0) {
				CanopyView::Pt c = V.project(T.tx, cpPanY(T.tx, T.tz), T.tz);
				int pc = (((int)std::round(T.semis)) % 12 + 12) % 12;
				nvgFontFaceId(vg, font->handle);
				nvgFontSize(vg, clamp(T.pa * c.scale * 0.55f, 5.f, 9.f));
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, T.damped ? P.line : P.ink);
				nvgText(vg, c.x, c.y, NN[pc], NULL);
			}
		}
		// the drops down in the bowl, then the skirt in front of them, which hides
		// the near part of the bowl and anything in it below the rim
		for (const CanopyDrop& d : M.drop) if (d.alive && inPan(d)) drawDrop(vg, V, d);
		{
			int st, cnt;
			cpSpan(NS, [&](int i) { return facing(TAU * ((float)i + 0.5f) / NS); }, st, cnt);
			if (cnt > 0) {
				// one polygon down the front of the skirt, its lower edge, and its two ends
				nvgBeginPath(vg);
				for (int j = 0; j <= cnt; j++) { CanopyView::Pt p = rim(TAU * (float)(st + j) / NS, 0.f); if (j == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
				for (int j = cnt; j >= 0; j--) { CanopyView::Pt p = rim(TAU * (float)(st + j) / NS, -CP_PAN_SKIRT); nvgLineTo(vg, p.x, p.y); }
				nvgClosePath(vg);
				nvgFillColor(vg, P.shell); nvgFill(vg);
				nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f);
				nvgBeginPath(vg);
				for (int j = 0; j <= cnt; j++) { CanopyView::Pt p = rim(TAU * (float)(st + j) / NS, -CP_PAN_SKIRT); if (j == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
				nvgStroke(vg);
				if (cnt < NS) for (int j : {0, cnt}) {
					CanopyView::Pt e0 = rim(TAU * (float)(st + j) / NS, 0.f), e1 = rim(TAU * (float)(st + j) / NS, -CP_PAN_SKIRT);
					nvgBeginPath(vg); nvgMoveTo(vg, e0.x, e0.y); nvgLineTo(vg, e1.x, e1.y); nvgStroke(vg);
				}
			}
		}
		// the rim
		nvgBeginPath(vg);
		for (int i = 0; i < NS; i++) { CanopyView::Pt p = rim(TAU * (float)i / NS, 0.f); if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y); }
		nvgClosePath(vg);
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 2.2f); nvgStroke(vg);
		nvgRestore(vg);
	}

	// The surface as small flat facets, each shaded by which way it faces, with the
	// outline, midrib and veins on top. The underside, when it shows, is darker.
	void drawLeaf(NVGcontext* vg, const CanopyView& V, const CanopyLeafLive& K, float bent, float flash, bool hot) {
		static const unsigned char SHADE[TP_MIX] = {0xCC, 0xA8, 0x58, 0xE4, 0xFF, 0x96};
		const int form = clamp(K.form, 0, LF_COUNT - 1);
		const int NS = CP_FORM_NS[form], NR = CP_FORM_NR[form];
		const float base = (float)SHADE[clamp(K.mat, 0, TP_MIX - 1)];
		const float alpha = K.mat == TP_GLASS ? 0.55f : 1.f;
		const CpPal pal = cpPal();
		static const int MS = 72, MR = 4;
		CanopyView::Pt P[MS + 1][MR + 1];
		float W[MS + 1][MR + 1][3];
		for (int a = 0; a <= NS; a++) for (int b = 0; b <= NR; b++)
			P[a][b] = leafPolar(V, K, bent, -(float)M_PI + 2.f * (float)M_PI * (float)a / (float)NS, (float)b / (float)NR, W[a][b]);
		float view[3];
		V.fromView(0.f, 0.f, 1.f, view);
		struct Facet { float depth; int a, b; };
		Facet F[MS * MR];
		for (int a = 0; a < NS; a++) for (int b = 0; b < NR; b++) {
			F[a * NR + b].depth = 0.25f * (P[a][b].depth + P[a + 1][b].depth + P[a][b + 1].depth + P[a + 1][b + 1].depth);
			F[a * NR + b].a = a; F[a * NR + b].b = b;
		}
		const int nF = NS * NR;
		std::sort(F, F + nF, [](const Facet& x, const Facet& y) { return x.depth < y.depth; });
		for (int fi = 0; fi < nF; fi++) {
			const Facet& f = F[fi];
			const float* p0 = W[f.a][f.b + 1]; const float* p1 = W[f.a + 1][f.b + 1]; const float* p2 = W[f.a][f.b];
			float ux = p1[0] - p0[0], uy = p1[1] - p0[1], uz = p1[2] - p0[2];
			float vx = p2[0] - p0[0], vy = p2[1] - p0[1], vz = p2[2] - p0[2];
			float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (nl < 1e-8f) continue;
			nx /= nl; ny /= nl; nz /= nl;
			if (ny < 0.f) { nx = -nx; ny = -ny; nz = -nz; }     // the upper side's normal
			bool under = nx * view[0] + ny * view[1] + nz * view[2] < 0.f;
			float lit = clamp(0.62f * nx + 0.35f * nz + 0.7f * ny, 0.f, 1.f);
			float k = (under ? 0.62f : 0.78f) + 0.26f * lit;
			float sh = clamp(base * k, 0.f, 255.f) / 255.f;
			if (pal.dark) sh = 0.16f + 0.42f * sh;
			// a hit turns it green, by as much as the hit was hard
			NVGcolor col = nvgRGBAf(sh + (0.30f * k - sh) * flash, sh + (0.72f * k - sh) * flash, sh + (0.32f * k - sh) * flash, alpha + (1.f - alpha) * flash);
			const CanopyView::Pt& A = P[f.a][f.b]; const CanopyView::Pt& B = P[f.a + 1][f.b];
			const CanopyView::Pt& C = P[f.a + 1][f.b + 1]; const CanopyView::Pt& D = P[f.a][f.b + 1];
			nvgBeginPath(vg);
			nvgMoveTo(vg, A.x, A.y); nvgLineTo(vg, B.x, B.y); nvgLineTo(vg, C.x, C.y); nvgLineTo(vg, D.x, D.y); nvgClosePath(vg);
			nvgFillColor(vg, col); nvgFill(vg);
			if (alpha >= 1.f) { nvgStrokeColor(vg, col); nvgStrokeWidth(vg, 0.5f); nvgStroke(vg); }     // closes the seams between facets
		}
		const bool darkLeaf = pal.dark || base < 0x70;
		const NVGcolor ink = pal.ink;
		const float centre = CP_FORM_C[clamp(form, 0, LF_COUNT - 1)];

		// the veins: to the point of each lobe where it has them, evenly otherwise
		float tips[10]; int nTips = 0;
		if (form == LF_BEECH || form == LF_WILLOW) {
			for (int k = 0; k < 5; k++) tips[nTips++] = 0.42f + 0.46f * (float)k;
		}
		else if (form == LF_GINKGO) {
			for (float t : {0.22f, 0.48f, 0.74f, 0.98f}) tips[nTips++] = t;
		}
		else {
			const float* tab = module->formR[clamp(form, 0, LF_COUNT - 1)];
			for (int i = CP_FORM_TAB / 2 + 8; i < CP_FORM_TAB - 8 && nTips < 10; i++) {
				bool peak = tab[i] > tab[i - 1] && tab[i] >= tab[i + 1] && tab[i] > tab[i - 12] && tab[i] > tab[i + 12] && tab[i] > 0.3f;
				if (peak) tips[nTips++] = -(float)M_PI + 2.f * (float)M_PI * (float)i / (float)CP_FORM_TAB;
			}
		}
		nvgStrokeColor(vg, darkLeaf ? nvgRGBA(0xE8, 0xE8, 0xEC, 0x90) : nvgRGBA(0x23, 0x1F, 0x20, 0x70));
		nvgStrokeWidth(vg, 0.5f);
		for (int side = -1; side <= 1; side += 2) {
			for (int k = 0; k < nTips; k++) {
				float th = tips[k];
				float r = 0.88f * module->formRadius(form, (float)side * th);
				float endU = centre + r * std::cos(th), endY = (float)side * r * std::sin(th);
				float startU = CP_FORM_PALM[form] ? centre : centre + 0.4f * r * std::cos(th) - 0.12f;
				nvgBeginPath(vg);
				for (int i = 0; i <= 5; i++) {
					float q = (float)i / 5.f;
					// bowed toward the tip, as a real side vein is
					float uu = startU + (endU - startU) * q + (CP_FORM_PALM[form] ? 0.f : 0.08f * q * (1.f - q));
					CanopyView::Pt p = leafAt(V, K, bent, uu, endY * q);
					if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
				}
				nvgStroke(vg);
			}
		}
		const float baseU = centre - module->formRadius(form, (float)M_PI - 1e-3f);
		const float tipU = form == LF_GINKGO ? centre + 0.5f : centre + module->formRadius(form, 0.f) * 0.98f;
		// the midrib, which stops at the leaf
		nvgBeginPath(vg);
		for (int i = 0; i <= 16; i++) {
			CanopyView::Pt p = leafAt(V, K, bent, baseU + (tipU - baseU) * (float)i / 16.f, 0.f);
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgStrokeColor(vg, darkLeaf ? nvgRGB(0xD8, 0xD8, 0xDC) : ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		// the stem: heavier, short, and curving away to one side, so it reads as a stem
		// and not as more of the midrib
		nvgBeginPath(vg);
		for (int i = 0; i <= 6; i++) {
			float q = (float)i / 6.f;
			CanopyView::Pt p = leafAt(V, K, bent, baseU - 0.17f * q, 0.09f * q * q);
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgStrokeColor(vg, ink); nvgStrokeWidth(vg, 1.3f); nvgStroke(vg);
		// the outline
		nvgBeginPath(vg);
		for (int i = 0; i < 200; i++) {
			CanopyView::Pt p = leafPolar(V, K, bent, -(float)M_PI + 2.f * (float)M_PI * (float)i / 200.f, 1.f);
			if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
		}
		nvgClosePath(vg);
		nvgStrokeColor(vg, ink); nvgStrokeWidth(vg, hot ? 1.2f : 0.8f); nvgStroke(vg);
		if (hot) {
			for (float u : {baseU, tipU}) {
				CanopyView::Pt c = leafAt(V, K, bent, u, 0.f);
				nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, 2.5f);
				nvgFillColor(vg, ink); nvgFill(vg);
			}
		}
	}
	// A plain circle, darker for heavier. Water is only an outline.
	void drawDrop(NVGcontext* vg, const CanopyView& V, const CanopyDrop& d) {
		static const unsigned char SHADE[TM_MIX] = {0xFF, 0xA8, 0xD8, 0x6C, 0x23};
		CanopyView::Pt p = V.project(d.x, d.y, d.z);
		float r = std::max(d.r * p.scale, 1.0f);
		nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, r);
		unsigned char sh = SHADE[clamp(d.mat, 0, TM_MIX - 1)];
		const CpPal P = cpPal();
		if (P.dark) sh = (unsigned char)(0x3A + (255 - sh) * 0.72f);
		// a drop settled in the pan fades out where it lies
		if (d.fade < 1.f) nvgGlobalAlpha(vg, clamp(d.fade, 0.f, 1.f));
		if (d.mat != TM_WATER) { nvgFillColor(vg, nvgRGB(sh, sh, sh)); nvgFill(vg); }
		nvgStrokeColor(vg, P.ink); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		if (d.fade < 1.f) nvgGlobalAlpha(vg, 1.f);
	}

	void draw(const DrawArgs& args) override {
		if (!module) { module = canopyPreview(); draw(args); module = nullptr; return; }
		NVGcontext* vg = args.vg;
		CanopyView V = view();
		const Canopy& M = *module;
		const CpPal P = cpPal();
		nvgSave(vg);
		nvgScissor(vg, 0, 0, box.size.x, box.size.y);

		// the column the drops fall through, where X and Z have put it
		{
			float rr = std::max(M.colR, 0.03f);
			// on the zither the column is scaled onto the string bed, as the drops are
			float sx = 1.f, sz = 1.f, zc = 0.f;
			if (M.instNow == 1) M.bedScale(sx, sz, zc);
			auto colAt = [&](float a, float& x, float& z) {
				x = (M.colX + rr * std::cos(a)) * sx; z = zc + (M.colZ + rr * std::sin(a)) * sz;
			};
			nvgStrokeColor(vg, P.line); nvgStrokeWidth(vg, 0.7f);
			nvgBeginPath(vg);
			float xMin = 1e9f, xMax = -1e9f, aMin = 0.f, aMax = 0.f;
			for (int i = 0; i <= 48; i++) {
				float a = 2.f * (float)M_PI * (float)i / 48.f;
				float cx, cz; colAt(a, cx, cz);
				CanopyView::Pt p = V.project(cx, CP_H, cz);
				if (i == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
				if (p.x < xMin) { xMin = p.x; aMin = a; }
				if (p.x > xMax) { xMax = p.x; aMax = a; }
			}
			nvgStroke(vg);
			for (float a : {aMin, aMax}) {
				float x, z; colAt(a, x, z);
				CanopyView::Pt p = V.project(x, CP_H, z), q = V.project(x, M.surfaceAt(x, z), z);
				nvgBeginPath(vg); nvgMoveTo(vg, p.x, p.y); nvgLineTo(vg, q.x, q.y); nvgStroke(vg);
			}
		}

		// a drop that has rolled off the back falls behind the drum
		const float drumDepth = V.project(0.f, 0.f, 0.f).depth;
		auto behindDrum = [&](const CanopyDrop& d) {
			// in the pan's sunken bowl a drop is below the rim and still in front of it
			if (M.instNow == 2 && d.x * d.x + d.z * d.z <= 1.f) return false;
			// the bowls stand on a table, and nothing gets behind that
			if (M.instNow == 3) return d.y < CP_BOWL_FLOOR - 0.05f && V.project(d.x, d.y, d.z).depth < drumDepth;
			// below the handpan's rim and behind it
			if (M.instNow == 5) return d.y < CP_HP_H0 && V.project(d.x, d.y, d.z).depth < drumDepth;
			if (M.instNow == 6) return d.y < CP_BN_BASE - 0.05f && V.project(d.x, d.y, d.z).depth < drumDepth;
			if (M.instNow >= 7) return false;
			return d.y < 0.f && V.project(d.x, d.y, d.z).depth < drumDepth;
		};
		for (const CanopyDrop& d : M.drop) if (d.alive && behindDrum(d)) drawDrop(vg, V, d);

		if (M.instNow == 1) drawZither(vg, V);
		else if (M.instNow == 2) drawPan(vg, V);
		else if (M.instNow == 3) drawTable(vg, V);     // the bowls are drawn among the drops, by depth
		else if (M.instNow == 4) drawChimeFrame(vg, V);
		else if (M.instNow == 5) drawHand(vg, V);
		else if (M.instNow == 6) drawBonangFrame(vg, V);
		else if (M.instNow >= 7) drawBarFrame(vg, V);
		else drawDrum(vg, V);

		// shadows on the dome, straight down, so a height can be read (the chimes hang
		// over nothing, so they have none)
		nvgFillColor(vg, P.shadeLeaf);
		for (int i = 0; i < (M.instNow == 4 ? 0 : M.nLive); i++) {
			const CanopyLeafLive& K = M.live[i];
			nvgBeginPath(vg);
			for (int k = 0; k < 28; k++) {
				float wd[3];
				leafPolar(V, K, M.bend[i], -(float)M_PI + 2.f * (float)M_PI * (float)k / 28.f, 1.f, wd);
				if (M.instNow == 1) {
					// kept on the board, as the drum keeps them inside its rim
					float xb = 0.5f * CP_Z_SP * (float)(M.nT - 1) + CP_Z_EDGE;
					wd[0] = clamp(wd[0], -xb, xb);
					wd[2] = clamp(wd[2], CP_Z_BACK - CP_Z_EDGE - 0.08f, M.zitherFront(wd[0]));
				}
				else {
					float rad = std::sqrt(wd[0] * wd[0] + wd[2] * wd[2]);
					if (rad > 0.97f) { wd[0] *= 0.97f / rad; wd[2] *= 0.97f / rad; }
				}
				// on the bowls' table, where the bowls drawn over it hide part of it
				float sy = M.instNow == 3 ? CP_BOWL_FLOOR : M.surfaceAt(wd[0], wd[2]);
				CanopyView::Pt p = V.project(wd[0], sy, wd[2]);
				if (k == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
			}
			nvgClosePath(vg);
			nvgFill(vg);
		}
		for (const CanopyDrop& d : M.drop) {
			if (!d.alive || M.instNow == 4) continue;
			if (M.instNow == 1) {
				float xb = 0.5f * CP_Z_SP * (float)(M.nT - 1) + CP_Z_EDGE;
				if (std::fabs(d.x) > xb || d.z < CP_Z_BACK - CP_Z_EDGE - 0.08f || d.z > M.zitherFront(d.x)) continue;
			}
			else if (d.x * d.x + d.z * d.z > 0.85f) continue;
			float s = M.surfaceAt(d.x, d.z);
			// over a bowl the bowl draws it; in the pan's bowl the pan does
			if (M.instNow == 3 && s != CP_BOWL_FLOOR) continue;
			if (inPan(d)) continue;
			if (d.y < s) continue;
			CanopyView::Pt p = V.project(d.x, s, d.z);
			float r = d.r * p.scale;
			nvgBeginPath(vg);
			nvgEllipse(vg, p.x, p.y, r, r * V.sp);
			nvgFillColor(vg, P.shadeDrop);
			nvgFill(vg);
		}

		// leaves and drops together, farthest first
		struct Item { float depth; int kind, i; };
		std::vector<Item> items;
		items.reserve(CP_MAXLEAVES + CP_MAXDROPS);
		for (int i = 0; i < M.nLive; i++)
			items.push_back({V.project(M.live[i].cx, M.live[i].cy, M.live[i].cz).depth, 0, i});
		for (int i = 0; i < CP_MAXDROPS; i++) {
			const CanopyDrop& d = M.drop[i];
			if (!d.alive || behindDrum(d) || inPan(d)) continue;
			if (M.instNow == 3 && M.bowlHollow(d.x, d.y, d.z) >= 0) continue;    // drawn with its bowl
			items.push_back({V.project(d.x, d.y, d.z).depth, 1, i});
		}
		if (M.instNow == 3)
			for (int t = 0; t < M.nT; t++)
				items.push_back({V.project(M.tv[t].tx, M.tv[t].bY - 0.5f * M.tv[t].bD, M.tv[t].tz).depth, 2, t});
		if (M.instNow == 6)
			for (int t = 0; t < M.nT; t++)
				items.push_back({V.project(M.tv[t].tx, M.tv[t].bY - 0.5f * M.tv[t].bD, M.tv[t].tz).depth, 2, t});
		if (M.instNow >= 7)
			for (int t = 0; t < M.nT; t++)
				items.push_back({V.project(M.tv[t].tx, M.tv[t].xy - CP_X_TH, M.tv[t].tz).depth, 2, t});
		if (M.instNow == 4) {
			for (int t = 0; t < M.nT; t++) {
				float c[3]; M.tv[t].pend.at(CP_CH_STR + 0.5f * M.tv[t].len, c);
				items.push_back({V.project(c[0], c[1], c[2]).depth, 2, t});
			}
			float c[3]; M.clap.at(CP_CH_CLAP, c);
			items.push_back({V.project(c[0], c[1], c[2]).depth, 2, M.nT});
		}
		std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.depth < b.depth; });
		for (const Item& it : items) {
			if (it.kind == 0) drawLeaf(vg, V, M.live[it.i], M.bend[it.i], M.leafFlash[it.i], it.i == hover || it.i == grabbed);
			else if (it.kind == 2) {
				if (M.instNow == 4) drawChimePiece(vg, V, it.i);
				else if (M.instNow == 6) drawKettle(vg, V, it.i);
				else if (M.instNow >= 7) drawBar(vg, V, it.i);
				else drawBowl(vg, V, it.i);
			}
			else drawDrop(vg, V, M.drop[it.i]);
		}
		nvgRestore(vg);
	}
};

// A tight grid, so the screen gets the room. The foot is one row of jacks, inputs at the
// left as on the other modules, a gap, then the outputs under the last three columns
// of the controls. The screen fills everything left of the controls.
static const float CP_FX[9] = {7.62f, 19.05f, 30.48f, 41.91f, 53.34f, 64.77f, 111.76f, 123.19f, 134.62f};
static const float CP_PX[5] = {88.9f, 100.33f, 111.76f, 123.19f, 134.62f};
static const float CP_PY[3] = {24.13f, 55.88f, 87.63f};
static const float CP_CV = 10.16f;        // from a pot down to its jack
static const float CP_HEAD = 10.16f;      // from a pot up to its group's heading
static const float CP_Y_FOOT = 119.38f;
static const float CP_DISP_X = 2.f, CP_DISP_Y = 11.f, CP_DISP_W = 80.f, CP_DISP_H = 98.f;

struct CanopyWidget : ModuleWidget {
	CanopyWidget(Canopy* module) {
		setModule(module);
		setPanel(sfs::themedPanel("canopy"));
		sfs::PanelLabels* lbl = new sfs::PanelLabels();
		lbl->box.size = box.size;
		addChild(lbl);
		lbl->title("CANOPY");

		CanopyDisplay* disp = new CanopyDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(CP_DISP_X, CP_DISP_Y));
		disp->box.size = mm2px(Vec(CP_DISP_W, CP_DISP_H));
		addChild(disp);

		// each group's heading, in a rule across the five columns
		static const char* HEAD[3] = {"DROPS", "LEAVES", "INSTRUMENT"};
		for (int g = 0; g < 3; g++) {
			float y = CP_PY[g] - CP_HEAD;
			lbl->add(CP_PX[2], y, HEAD[g]);
			lbl->divider(CP_PX[0] - 5.f, y - 0.85f, CP_PX[2] - 8.f, y - 0.85f);
			lbl->divider(CP_PX[2] + 8.f, y - 0.85f, CP_PX[4] + 5.f, y - 0.85f);
		}

		// written out so tools/panel_reticules.py can read them
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[0], CP_PY[0])), module, Canopy::RATE_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[0], CP_PY[0] + CP_CV)), module, Canopy::RATE_INPUT));
		lbl->pairDown(CP_PX[0], CP_PY[0], CP_PY[0] + CP_CV, "RATE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[1], CP_PY[0])), module, Canopy::MATERIAL_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[1], CP_PY[0] + CP_CV)), module, Canopy::MATERIAL_INPUT));
		lbl->pairDown(CP_PX[1], CP_PY[0], CP_PY[0] + CP_CV, "MATERIAL");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[2], CP_PY[0])), module, Canopy::SIZE_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[2], CP_PY[0] + CP_CV)), module, Canopy::SIZE_INPUT));
		lbl->pairDown(CP_PX[2], CP_PY[0], CP_PY[0] + CP_CV, "SIZE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[3], CP_PY[0])), module, Canopy::SCATTER_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[3], CP_PY[0] + CP_CV)), module, Canopy::SCATTER_INPUT));
		lbl->pairDown(CP_PX[3], CP_PY[0], CP_PY[0] + CP_CV, "SCATTER");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[0], CP_PY[1])), module, Canopy::DENSITY_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[0], CP_PY[1] + CP_CV)), module, Canopy::DENSITY_INPUT));
		lbl->pairDown(CP_PX[0], CP_PY[1], CP_PY[1] + CP_CV, "DENSITY");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[1], CP_PY[1])), module, Canopy::LEAF_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[1], CP_PY[1] + CP_CV)), module, Canopy::LEAF_INPUT));
		lbl->pairDown(CP_PX[1], CP_PY[1], CP_PY[1] + CP_CV, "LEAF");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[2], CP_PY[1])), module, Canopy::DRIFT_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[2], CP_PY[1] + CP_CV)), module, Canopy::DRIFT_INPUT));
		lbl->pairDown(CP_PX[2], CP_PY[1], CP_PY[1] + CP_CV, "DRIFT");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[3], CP_PY[1])), module, Canopy::SPEED_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[3], CP_PY[1] + CP_CV)), module, Canopy::SPEED_INPUT));
		lbl->pairDown(CP_PX[3], CP_PY[1], CP_PY[1] + CP_CV, "SPEED");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[4], CP_PY[1])), module, Canopy::TILT_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[4], CP_PY[1] + CP_CV)), module, Canopy::TILT_INPUT));
		lbl->pairDown(CP_PX[4], CP_PY[1], CP_PY[1] + CP_CV, "TILT");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[0], CP_PY[2])), module, Canopy::DECAY_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[0], CP_PY[2] + CP_CV)), module, Canopy::DECAY_INPUT));
		lbl->pairDown(CP_PX[0], CP_PY[2], CP_PY[2] + CP_CV, "DECAY");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[1], CP_PY[2])), module, Canopy::TONE_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[1], CP_PY[2] + CP_CV)), module, Canopy::TONE_INPUT));
		lbl->pairDown(CP_PX[1], CP_PY[2], CP_PY[2] + CP_CV, "TONE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[2], CP_PY[2])), module, Canopy::ROOT_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[2], CP_PY[2] + CP_CV)), module, Canopy::ROOT_INPUT));
		lbl->pairDown(CP_PX[2], CP_PY[2], CP_PY[2] + CP_CV, "ROOT");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[3], CP_PY[2])), module, Canopy::SCALE_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[3], CP_PY[2] + CP_CV)), module, Canopy::SCALE_INPUT));
		lbl->pairDown(CP_PX[3], CP_PY[2], CP_PY[2] + CP_CV, "SCALE");
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CP_PX[4], CP_PY[2])), module, Canopy::INSTRUMENT_PARAM));
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_PX[4], CP_PY[2] + CP_CV)), module, Canopy::INSTRUMENT_INPUT));
		lbl->pairDown(CP_PX[4], CP_PY[2], CP_PY[2] + CP_CV, "TYPE");

		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_FX[0], CP_Y_FOOT)), module, Canopy::VOCT_INPUT));
		lbl->jack(CP_FX[0], CP_Y_FOOT, "V/OCT");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_FX[1], CP_Y_FOOT)), module, Canopy::GATE_INPUT));
		lbl->jack(CP_FX[1], CP_Y_FOOT, "GATE");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_FX[2], CP_Y_FOOT)), module, Canopy::VEL_INPUT));
		lbl->jack(CP_FX[2], CP_Y_FOOT, "VEL");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_FX[3], CP_Y_FOOT)), module, Canopy::CLOCK_INPUT));
		lbl->jack(CP_FX[3], CP_Y_FOOT, "CLOCK");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_FX[4], CP_Y_FOOT)), module, Canopy::X_INPUT));
		lbl->jack(CP_FX[4], CP_Y_FOOT, "X");
		addInput(createInputCentered<sfs::Jack>(mm2px(Vec(CP_FX[5], CP_Y_FOOT)), module, Canopy::Z_INPUT));
		lbl->jack(CP_FX[5], CP_Y_FOOT, "Z");

		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(CP_FX[6], CP_Y_FOOT)), module, Canopy::POLY_OUTPUT));
		lbl->jackOnPlate(CP_FX[6], CP_Y_FOOT, "POLY");
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(CP_FX[7], CP_Y_FOOT)), module, Canopy::L_OUTPUT));
		lbl->jackOnPlate(CP_FX[7], CP_Y_FOOT, "LEFT");
		addOutput(createOutputCentered<sfs::Jack>(mm2px(Vec(CP_FX[8], CP_Y_FOOT)), module, Canopy::R_OUTPUT));
		lbl->jackOnPlate(CP_FX[8], CP_Y_FOOT, "RIGHT");
	}

	void appendContextMenu(Menu* menu) override {
		Canopy* m = dynamic_cast<Canopy*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		sfs::rootCvMenu(menu, m, Canopy::ROOT_INPUT, m->rootCvMode);
		menu->addChild(createIndexPtrSubmenuItem("Steel pan tuning",
			{"Chromatic", "Tuned to the scale", "Chromatic, notes outside the scale damped"}, &m->panTuning));
		menu->addChild(createIndexSubmenuItem("Lowest note", {"C2 to B2", "C3 to B3", "C4 to B4"},
			[=]() { return (size_t)(m->regOct + 2); },
			[=](size_t v) { m->regOct = (int)v - 2; }));
		{
			std::vector<std::string> forms(CP_FORM_NAME, CP_FORM_NAME + LF_COUNT + 1);
			menu->addChild(createIndexPtrSubmenuItem("Leaf form", forms, &m->leafForm));
		}
		menu->addChild(createBoolPtrMenuItem("Evenly spaced drops (off: random spacing)", "", &m->evenly));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Drag a leaf to move it. Shift-drag to change its height. Drag an end to tilt it."));
		menu->addChild(createMenuLabel("Drag empty space to turn the view."));
		menu->addChild(createMenuItem("Reset leaf positions", "", [=]() { m->resetLeaves(); }));
		menu->addChild(createMenuItem("Reset view", "", [=]() { m->camYaw = 0.f; m->camTilt = 0.62f; }));
	}
};

Model* modelCanopy = createModel<Canopy, CanopyWidget>("Canopy");

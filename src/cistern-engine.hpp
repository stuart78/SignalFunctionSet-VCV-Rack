#pragma once
// =============================================================================
// Cistern's acoustic engine: a real space, modelled from its geometry.
// Free of Rack, so tools/cistern-harness.cpp can measure it on its own.
//
// Three layers (docs/cistern-design.md):
//   1. early reflections TRACED from the geometry on a worker, as a sparse tap
//      set per ear in three render bands, the direct sound added exactly
//   2. a late tail from a 16-line FDN whose per-band decay is the room's own
//      (Eyring, with ISO 9613-1 air absorption)
//   3. a handover at the mixing time, its level calibrated offline
// =============================================================================
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cistern {

static const int NB = 6;                                   // octave bands
static const double BAND_HZ[NB] = {125, 250, 500, 1000, 2000, 4000};
static const double C_SOUND = 343.0;                       // m/s at 20 C

// ── materials: absorption per band, and how much of a reflection scatters ──
struct Material { const char* name; double a[NB]; double scatter; };
enum MatId { M_SEALED_CONCRETE, M_ROUGH_CONCRETE, M_STONE, M_PLASTER, M_WOOD, M_GLASS, M_BRICK, M_MARBLE, M_PAINTED,
             M_ROUGH_STONE, M_PEWS, M_CARPET, M_FURNISHED, M_COFFERED, M_LATH, M_WATER, M_BOARD_CONCRETE, M_OPEN, M_COUNT };
static const Material MATERIALS[M_COUNT] = {
	// smooth, sealed concrete: what the cistern's 45 s at low frequency needs
	{"sealed concrete", {0.005, 0.006, 0.008, 0.010, 0.012, 0.015}, 0.10},
	{"rough concrete",  {0.010, 0.020, 0.040, 0.060, 0.080, 0.100}, 0.30},
	{"stone",           {0.020, 0.020, 0.030, 0.040, 0.050, 0.050}, 0.25},
	{"plaster",         {0.010, 0.020, 0.020, 0.030, 0.040, 0.050}, 0.15},
	{"wood",            {0.150, 0.110, 0.100, 0.070, 0.060, 0.070}, 0.40},
	{"glass",           {0.350, 0.250, 0.180, 0.120, 0.070, 0.040}, 0.05},
	{"brick",           {0.030, 0.030, 0.030, 0.040, 0.050, 0.070}, 0.30},
	{"marble",          {0.010, 0.010, 0.010, 0.015, 0.020, 0.020}, 0.10},
	{"painted masonry", {0.010, 0.010, 0.020, 0.020, 0.020, 0.030}, 0.20},
	// Aggregates. A converted scan is smoothed to half a metre, so what a room
	// is FURNISHED with (pews, carpets, cases, visitors, the depth of a coffer)
	// is gone from its geometry; these put it back as the surface it sits on.
	// Unoccupied, and estimates for the building type, not measurements.
	{"rough stone",     {0.030, 0.040, 0.050, 0.060, 0.070, 0.080}, 0.40},
	{"floor with pews", {0.100, 0.140, 0.170, 0.190, 0.200, 0.200}, 0.60},
	{"carpet on stone", {0.050, 0.100, 0.300, 0.500, 0.600, 0.650}, 0.20},
	{"furnished floor", {0.120, 0.160, 0.220, 0.260, 0.280, 0.300}, 0.60},
	{"coffered concrete", {0.040, 0.050, 0.060, 0.070, 0.080, 0.090}, 0.60},
	{"plaster on lath", {0.140, 0.100, 0.060, 0.050, 0.040, 0.030}, 0.30},   // a vault or ceiling that flexes absorbs low frequencies
	{"water",           {0.008, 0.008, 0.013, 0.015, 0.020, 0.025}, 0.00},
	{"board-formed concrete", {0.012, 0.014, 0.017, 0.018, 0.022, 0.027}, 0.40},   // set so Houston lands its measured 15-17 s
	{"opening",         {1.000, 1.000, 1.000, 1.000, 1.000, 1.000}, 0.00},
};

// ── air: ISO 9613-1, dB per metre, and the energy attenuation m (1/m) ──────
inline double airDbPerM(double f, double tempC, double rhPct, double pKPa = 101.325) {
	const double T = tempC + 273.15, T0 = 293.15, T01 = 273.16, pr = 101.325, pa = pKPa;
	double C = -6.8346 * std::pow(T01 / T, 1.261) + 4.6151;
	double h = rhPct * std::pow(10.0, C) * pr / pa;        // molar water vapour, %
	double frO = pa / pr * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
	double frN = pa / pr * std::pow(T / T0, -0.5) * (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(T / T0, -1.0 / 3.0) - 1.0)));
	return 8.686 * f * f * (1.84e-11 * (pr / pa) * std::sqrt(T / T0)
		+ std::pow(T / T0, -2.5) * (0.01275 * std::exp(-2239.1 / T) / (frO + f * f / frO)
		                          + 0.1068 * std::exp(-3352.0 / T) / (frN + f * f / frN)));
}
inline double airM(double f, double tempC, double rhPct) { return airDbPerM(f, tempC, rhPct) / 4.3429; }

// ── geometry ───────────────────────────────────────────────────────────────
struct V3 {
	double x = 0, y = 0, z = 0;
	V3() {}
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
	V3 operator+(const V3& o) const { return V3(x + o.x, y + o.y, z + o.z); }
	V3 operator-(const V3& o) const { return V3(x - o.x, y - o.y, z - o.z); }
	V3 operator*(double k) const { return V3(x * k, y * k, z * k); }
	double dot(const V3& o) const { return x * o.x + y * o.y + z * o.z; }
	double len() const { return std::sqrt(dot(*this)); }
	V3 norm() const { double l = len(); return l > 0 ? *this * (1.0 / l) : *this; }
	V3 cross(const V3& o) const { return V3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x); }
	double at(int a) const { return a == 0 ? x : a == 1 ? y : z; }
};
// a plane n.p = d (floor, ceiling, flat walls)
struct Plane { V3 n; double d; int mat; };
// a vertical cylinder; `wall` = the room is INSIDE it, else it is a column
struct Cyl { double x, z, r, y0, y1; int mat; bool wall; };
// a triangle of a scanned room (tools/cistern-convert.cpp): a, edges, and the
// face normal, which points into the air
struct Tri { V3 a, e1, e2, n; int mat; };
struct BNode { float lo[3], hi[3]; int first, count; };   // count > 0: a leaf of tris [first, first+count); else children at first, first+1

struct Space {
	const char* name = "";
	// How much of each reflection scatters, for the whole space, when >= 0 (else
	// each material's own). The tables' coefficients are for flat panels, but at
	// room scale what scatters is the build: form joints, pitting, a wall that is
	// not quite a cylinder. It decides how hard a round room focuses (round 3).
	double rough = -1;
	std::vector<Plane> planes;
	std::vector<Cyl> cyls;
	double volume = 0;
	double area[M_COUNT] = {};                          // m² of each material
	V3 lo, hi;                                          // bounds, for the screen and positions
	V3 src, lis;                                        // default positions
	// column lookup: a uniform grid over xz, so a ray tests only nearby columns
	double cell = 0; int gx = 0, gz = 0;
	std::vector<std::vector<int>> grid;
	// a scanned room: its surface, and a grid of where the air is (positions)
	bool mesh = false;
	bool clean = false;                                   // built from dimensions, not scanned: drawn as its own edges
	float foldDeg = 12.f;                                 // a drawn edge: two faces meeting at more than this
	std::vector<Tri> drawTris;                            // faces drawn but traced analytically (a built room's floor, lid, walls)
	std::vector<Tri> tris;
	std::vector<BNode> bvh;
	V3 glo; double gcell = 0; int ggx = 0, ggy = 0, ggz = 0;
	std::vector<uint8_t> gbits;
	bool airAt(int x, int y, int z) const {
		if (x < 0 || y < 0 || z < 0 || x >= ggx || y >= ggy || z >= ggz) return false;
		size_t i = (size_t)x + (size_t)ggx * ((size_t)y + (size_t)ggy * z);
		return (gbits[i >> 3] >> (i & 7)) & 1;
	}
	// can a source or the listener stand here
	bool inside(const V3& p) const {
		if (mesh) return airAt((int)std::floor((p.x - glo.x) / gcell), (int)std::floor((p.y - glo.y) / gcell), (int)std::floor((p.z - glo.z) / gcell));
		for (const Cyl& k : cyls) {
			double r = std::hypot(p.x - k.x, p.z - k.z);
			if (k.wall ? r > k.r * 0.97 : r < k.r + 0.2) return false;
		}
		return p.y > lo.y + 0.1 && p.y < hi.y - 0.1;
	}
	void buildGrid(double c) {
		cell = c; gx = (int)std::ceil((hi.x - lo.x) / c) + 1; gz = (int)std::ceil((hi.z - lo.z) / c) + 1;
		grid.assign((size_t)gx * gz, {});
		for (int i = 0; i < (int)cyls.size(); i++) {
			const Cyl& k = cyls[i];
			if (k.wall) continue;
			int x0 = (int)((k.x - k.r - lo.x) / c), x1 = (int)((k.x + k.r - lo.x) / c);
			int z0 = (int)((k.z - k.r - lo.z) / c), z1 = (int)((k.z + k.r - lo.z) / c);
			for (int a = std::max(0, x0); a <= std::min(gx - 1, x1); a++)
				for (int b = std::max(0, z0); b <= std::min(gz - 1, z1); b++) grid[(size_t)a * gz + b].push_back(i);
		}
	}
	double surface() const { double s = 0; for (int m = 0; m < M_COUNT; m++) s += area[m]; return s; }
	double meanFreePath() const { return 4.0 * volume / surface(); }
};

// Eyring with the air term, per band: the decay the late tail must have
inline void decayTimes(const Space& s, double tempC, double rh, double t60[NB]) {
	double S = s.surface();
	for (int b = 0; b < NB; b++) {
		double A = 0; for (int m = 0; m < M_COUNT; m++) A += s.area[m] * MATERIALS[m].a[b];
		double abar = std::min(A / S, 0.99);
		double air = 4.0 * airM(BAND_HZ[b], tempC, rh) * s.volume;
		t60[b] = 0.161 * s.volume / (-S * std::log(1.0 - abar) + air);
	}
}
// the same at 8 kHz, with the walls as at 4 kHz: up there the air decides it,
// and the tail's filter needs a target past its top octave or it holds 4 kHz's
// decay all the way to Nyquist
inline double decay8k(const Space& s, double tempC, double rh) {
	double S = s.surface(), A = 0;
	for (int m = 0; m < M_COUNT; m++) A += s.area[m] * MATERIALS[m].a[NB - 1];
	double abar = std::min(A / S, 0.99);
	return 0.161 * s.volume / (-S * std::log(1.0 - abar) + 4.0 * airM(8000, tempC, rh) * s.volume);
}

// ── the ray tracer ─────────────────────────────────────────────────────────
struct Rng { uint64_t s; explicit Rng(uint64_t x) : s(x * 2654435761u + 1) {} double u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (s >> 11) * (1.0 / 9007199254740992.0); } };
inline V3 randDir(Rng& r) {
	double z = 2 * r.u() - 1, a = 2 * M_PI * r.u(), q = std::sqrt(std::max(0.0, 1 - z * z));
	return V3(q * std::cos(a), z, q * std::sin(a));
}
// A Lambertian (cosine-weighted) direction about the normal: the normal plus a
// point on the WHOLE unit sphere. Flipping that point into the hemisphere first,
// as the first version did, crowds reflections towards the normal; in a
// shoebox it put the mean free path 14% past 4V/S, where a diffuse field must
// sit exactly.
inline V3 lambert(Rng& r, const V3& n) {
	for (;;) { V3 d = n + randDir(r); double l = d.len(); if (l > 1e-6) return d * (1.0 / l); }
}
// nearest surface along o + t d, t > eps: returns t, the normal, the material
inline bool intersect(const Space& s, const V3& o, const V3& d, double& tHit, V3& nHit, int& mat) {
	tHit = 1e30; bool any = false;
	for (const Plane& p : s.planes) {
		double den = p.n.dot(d);
		if (den >= -1e-12) continue;                        // only surfaces faced from inside
		double t = (p.d - p.n.dot(o)) / den;
		if (t > 1e-6 && t < tHit) { tHit = t; nHit = p.n; mat = p.mat; any = true; }
	}
	auto cyl = [&](const Cyl& k) {
		double ox = o.x - k.x, oz = o.z - k.z;
		double a = d.x * d.x + d.z * d.z; if (a < 1e-14) return;
		double b = ox * d.x + oz * d.z, c = ox * ox + oz * oz - k.r * k.r;
		double disc = b * b - a * c; if (disc < 0) return;
		double sq = std::sqrt(disc);
		double t = k.wall ? (-b + sq) / a : (-b - sq) / a;   // a wall is left from inside, a column entered from out
		if (!(t > 1e-6 && t < tHit)) return;
		double y = o.y + t * d.y; if (y < k.y0 || y > k.y1) return;
		V3 p(ox + t * d.x, 0, oz + t * d.z);
		V3 n = (p * (1.0 / k.r)); if (k.wall) n = n * -1.0;   // pointing into the room
		tHit = t; nHit = n; mat = k.mat; any = true;
	};
	for (const Cyl& k : s.cyls) if (k.wall) cyl(k);
	// columns: walk the grid cells the segment (up to the current hit) passes through
	if (!s.grid.empty()) {
		double tEnd = std::min(tHit, 200.0);
		int steps = (int)(tEnd * std::sqrt(d.x * d.x + d.z * d.z) / (s.cell * 0.5)) + 2;
		int last = -1;
		static thread_local std::vector<int> seen; seen.clear();
		for (int i = 0; i <= steps; i++) {
			double t = tEnd * i / steps;
			int cx = (int)((o.x + t * d.x - s.lo.x) / s.cell), cz = (int)((o.z + t * d.z - s.lo.z) / s.cell);
			if (cx < 0 || cz < 0 || cx >= s.gx || cz >= s.gz) continue;
			int id = cx * s.gz + cz; if (id == last) continue; last = id;
			for (int dx = -1; dx <= 1; dx++) for (int dz = -1; dz <= 1; dz++) {
				int ax = cx + dx, az = cz + dz;
				if (ax < 0 || az < 0 || ax >= s.gx || az >= s.gz) continue;
				for (int ci : s.grid[(size_t)ax * s.gz + az])
					if (std::find(seen.begin(), seen.end(), ci) == seen.end()) { seen.push_back(ci); cyl(s.cyls[ci]); }
			}
		}
	}
	// a scanned room: down its BVH
	if (!s.bvh.empty()) {
		double inv[3] = {1.0 / (d.x != 0 ? d.x : 1e-30), 1.0 / (d.y != 0 ? d.y : 1e-30), 1.0 / (d.z != 0 ? d.z : 1e-30)};
		int stack[96], sp = 0;
		stack[sp++] = 0;
		while (sp) {
			const BNode& nd = s.bvh[stack[--sp]];
			double t0 = 1e-6, t1 = tHit;
			bool miss = false;
			for (int a = 0; a < 3 && !miss; a++) {
				double oa = o.at(a), ta = (nd.lo[a] - oa) * inv[a], tb = (nd.hi[a] - oa) * inv[a];
				if (ta > tb) std::swap(ta, tb);
				t0 = std::max(t0, ta); t1 = std::min(t1, tb);
				miss = t0 > t1;
			}
			if (miss) continue;
			if (nd.count == 0) { if (sp < 94) { stack[sp++] = nd.first; stack[sp++] = nd.first + 1; } continue; }
			for (int i = nd.first; i < nd.first + nd.count; i++) {
				const Tri& T = s.tris[i];
				V3 p = d.cross(T.e2);
				double det = T.e1.dot(p);
				if (std::fabs(det) < 1e-14) continue;
				double id = 1.0 / det;
				V3 tv = o - T.a;
				double u = tv.dot(p) * id;
				if (u < 0 || u > 1) continue;
				V3 q = tv.cross(T.e1);
				double v = d.dot(q) * id;
				if (v < 0 || u + v > 1) continue;
				double t = T.e2.dot(q) * id;
				if (t > 1e-6 && t < tHit) {
					tHit = t; mat = T.mat; any = true;
					nHit = T.n.dot(d) > 0 ? T.n * -1.0 : T.n;    // facing the ray, whatever the winding
				}
			}
		}
	}
	return any;
}

// ── scanned rooms: loading, and the BVH ────────────────────────────────────
inline void buildBVH(Space& s) {
	int n = (int)s.tris.size();
	std::vector<int> idx(n);
	std::vector<V3> cen(n);
	for (int i = 0; i < n; i++) { idx[i] = i; const Tri& T = s.tris[i]; cen[i] = T.a + (T.e1 + T.e2) * (1.0 / 3); }
	s.bvh.clear(); s.bvh.reserve(2 * n / 3 + 2);
	s.bvh.push_back(BNode());
	std::function<void(int, int, int)> rec = [&](int node, int first, int count) {
		float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
		double clo[3] = {1e30, 1e30, 1e30}, chi[3] = {-1e30, -1e30, -1e30};
		for (int k = first; k < first + count; k++) {
			const Tri& T = s.tris[idx[k]];
			V3 v[3] = {T.a, T.a + T.e1, T.a + T.e2};
			for (const V3& p : v) for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], (float)p.at(a)); hi[a] = std::max(hi[a], (float)p.at(a)); }
			for (int a = 0; a < 3; a++) { clo[a] = std::min(clo[a], cen[idx[k]].at(a)); chi[a] = std::max(chi[a], cen[idx[k]].at(a)); }
		}
		for (int a = 0; a < 3; a++) { s.bvh[node].lo[a] = lo[a] - 1e-4f; s.bvh[node].hi[a] = hi[a] + 1e-4f; }
		if (count <= 4) { s.bvh[node].first = first; s.bvh[node].count = count; return; }
		int ax = 0; for (int a = 1; a < 3; a++) if (chi[a] - clo[a] > chi[ax] - clo[ax]) ax = a;
		int half = count / 2;
		std::nth_element(idx.begin() + first, idx.begin() + first + half, idx.begin() + first + count,
		                 [&](int a, int b) { return cen[a].at(ax) < cen[b].at(ax); });
		int c = (int)s.bvh.size();
		s.bvh.push_back(BNode()); s.bvh.push_back(BNode());
		s.bvh[node].first = c; s.bvh[node].count = 0;
		rec(c, first, half);
		rec(c + 1, first + half, count - half);
	};
	if (n) rec(0, 0, n);
	std::vector<Tri> sorted(n);
	for (int i = 0; i < n; i++) sorted[i] = s.tris[idx[i]];
	s.tris.swap(sorted);
}

// A space file from tools/cistern-convert.cpp. `clsMat` maps its face classes
// (floor, wall, ceiling, opening) to materials.
inline bool loadSpace(const std::string& path, const int clsMat[4], Space& s, std::string& err) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) { err = "cannot open " + path; return false; }
	std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> b((size_t)std::max(0L, sz));
	size_t got = std::fread(b.data(), 1, b.size(), f);
	std::fclose(f);
	size_t at = 0;
	auto need = [&](size_t n) { return at + n <= got; };
	auto rd = [&](void* dst, size_t n) { if (need(n)) std::memcpy(dst, b.data() + at, n); at += n; };
	auto f32 = [&]() { float x = 0; rd(&x, 4); return (double)x; };
	auto u32 = [&]() { uint32_t x = 0; rd(&x, 4); return x; };
	if (got < 8 || std::memcmp(b.data(), "CSP1", 4) != 0) { err = "not a space file"; return false; }
	at = 4;
	uint32_t nv = u32(), nt = u32();
	V3 vlo, vhi;
	vlo.x = f32(); vlo.y = f32(); vlo.z = f32(); vhi.x = f32(); vhi.y = f32(); vhi.z = f32();
	if (!need((size_t)nv * 6 + (size_t)nt * 13)) { err = "truncated"; return false; }
	std::vector<V3> v(nv);
	for (uint32_t i = 0; i < nv; i++) {
		uint16_t q[3]; rd(q, 6);
		v[i] = V3(vlo.x + q[0] / 65535.0 * (vhi.x - vlo.x), vlo.y + q[1] / 65535.0 * (vhi.y - vlo.y), vlo.z + q[2] / 65535.0 * (vhi.z - vlo.z));
	}
	std::vector<uint32_t> t((size_t)nt * 3);
	rd(t.data(), t.size() * 4);
	std::vector<uint8_t> cls(nt);
	rd(cls.data(), nt);
	s.tris.clear(); s.tris.reserve(nt);
	for (int m = 0; m < M_COUNT; m++) s.area[m] = 0;
	for (uint32_t i = 0; i < nt; i++) {
		uint32_t a = t[i * 3], c1 = t[i * 3 + 1], c2 = t[i * 3 + 2];
		if (a >= nv || c1 >= nv || c2 >= nv) { err = "bad index"; return false; }
		Tri T; T.a = v[a]; T.e1 = v[c1] - v[a]; T.e2 = v[c2] - v[a];
		V3 n = T.e1.cross(T.e2);
		double ar = 0.5 * n.len();
		if (ar <= 0) continue;
		T.n = n.norm();
		T.mat = clsMat[std::min<int>(cls[i], 3)];
		s.area[T.mat] += ar;
		s.tris.push_back(T);
	}
	s.volume = f32();
	s.src.x = f32(); s.src.y = f32(); s.src.z = f32();
	s.lis.x = f32(); s.lis.y = f32(); s.lis.z = f32();
	s.glo.x = f32(); s.glo.y = f32(); s.glo.z = f32(); s.gcell = f32();
	s.ggx = (int)u32(); s.ggy = (int)u32(); s.ggz = (int)u32();
	size_t nb = ((size_t)s.ggx * s.ggy * s.ggz + 7) / 8;
	if (!need(nb)) { err = "truncated grid"; return false; }
	s.gbits.assign(b.begin() + at, b.begin() + at + nb);
	s.lo = vlo; s.hi = vhi;
	s.mesh = true;
	buildBVH(s);
	return true;
}

// the same room scaled about its origin, for SIZE
inline Space scaledCopy(const Space& b, double k) {
	Space s = b;
	for (Tri& T : s.tris) { T.a = T.a * k; T.e1 = T.e1 * k; T.e2 = T.e2 * k; }
	for (Tri& T : s.drawTris) { T.a = T.a * k; T.e1 = T.e1 * k; T.e2 = T.e2 * k; }
	for (BNode& n : s.bvh) for (int a = 0; a < 3; a++) { n.lo[a] *= (float)k; n.hi[a] *= (float)k; }
	for (Plane& p : s.planes) p.d *= k;
	for (Cyl& c : s.cyls) { c.x *= k; c.z *= k; c.r *= k; c.y0 *= k; c.y1 *= k; }
	s.glo = s.glo * k; s.gcell *= k;
	s.lo = s.lo * k; s.hi = s.hi * k; s.src = s.src * k; s.lis = s.lis * k;
	s.volume *= k * k * k;
	for (int m = 0; m < M_COUNT; m++) s.area[m] *= k * k;
	if (!s.cyls.empty() && s.cell > 0) s.buildGrid(s.cell * k);
	return s;
}

// ── built rooms: geometry from published dimensions ───────────────────────
// A room built rather than scanned: clean solids, every face facing the air,
// drawn as its own edges. `air` on each face is any direction into the air;
// the winding is fixed to agree with it.
struct Builder {
	Space& s;
	double solid = 0;                                     // volume the solids take
	explicit Builder(Space& sp) : s(sp) {}
	void tri(const V3& a, V3 b, V3 c, int mat, const V3& air) {
		V3 n = (b - a).cross(c - a);
		double l = n.len();
		if (l < 1e-12) return;
		if (n.dot(air) < 0) { std::swap(b, c); n = n * -1.0; }
		Tri t; t.a = a; t.e1 = b - a; t.e2 = c - a; t.n = n * (1.0 / l); t.mat = mat;
		s.tris.push_back(t);
		s.area[mat] += 0.5 * l;
	}
	void quad(const V3& a, const V3& b, const V3& c, const V3& d, int mat, const V3& air) { tri(a, b, c, mat, air); tri(a, c, d, mat, air); }
	// The big surfaces (floor, lid, walls) are traced as planes and cylinders
	// and only DRAWN as triangles: a floor as a fan of long slivers gave every
	// ray a dozen huge bounding boxes to test, and a trace went from 0.2 s to 2.4.
	void drawTri(const V3& a, V3 b, V3 c, int mat, const V3& air) {
		V3 n = (b - a).cross(c - a);
		double l = n.len();
		if (l < 1e-12) return;
		if (n.dot(air) < 0) { std::swap(b, c); n = n * -1.0; }
		Tri t; t.a = a; t.e1 = b - a; t.e2 = c - a; t.n = n * (1.0 / l); t.mat = mat;
		s.drawTris.push_back(t);
	}
	void drawQuad(const V3& a, const V3& b, const V3& c, const V3& d, int mat, const V3& air) { drawTri(a, b, c, mat, air); drawTri(a, c, d, mat, air); }
	static double polyArea(const std::vector<V3>& p) {
		double A = 0; for (size_t k = 0; k < p.size(); k++) { const V3 &a = p[k], &b = p[(k + 1) % p.size()]; A += a.x * b.z - b.x * a.z; }
		return 0.5 * std::fabs(A);
	}
	// a solid between two horizontal outlines with the same number of points:
	// its sides, and its top or bottom if they are not against the floor or lid
	void loft(const std::vector<V3>& lo, const std::vector<V3>& hi, int mat, bool capBottom = false, bool capTop = false) {
		size_t n = lo.size();
		V3 c; for (const V3& p : lo) c = c + p; c = c * (1.0 / n);
		for (size_t k = 0; k < n; k++) {
			size_t k2 = (k + 1) % n;
			V3 mid = (lo[k] + lo[k2] + hi[k] + hi[k2]) * 0.25;
			quad(lo[k], lo[k2], hi[k2], hi[k], mat, V3(mid.x - c.x, 0, mid.z - c.z));
		}
		auto cap = [&](const std::vector<V3>& p, const V3& air) {
			V3 m; for (const V3& q : p) m = m + q; m = m * (1.0 / p.size());
			for (size_t k = 0; k < p.size(); k++) tri(m, p[k], p[(k + 1) % p.size()], mat, air);
		};
		if (capBottom) cap(lo, V3(0, -1, 0));
		if (capTop) cap(hi, V3(0, 1, 0));
		double A1 = polyArea(lo), A2 = polyArea(hi), h = std::fabs(hi[0].y - lo[0].y);
		solid += h / 3 * (A1 + A2 + std::sqrt(A1 * A2));
	}
	// outlines: a square (with `n` points round it, so it can loft to an
	// n-sided shaft), a regular polygon, a rectangle
	static std::vector<V3> square(double cx, double cz, double half, double y, int n = 4) {
		std::vector<V3> p;
		for (int k = 0; k < n; k++) {
			double a = 2 * M_PI * (k + 0.5) / n, ca = std::cos(a), sa = std::sin(a), m = std::max(std::fabs(ca), std::fabs(sa));
			p.push_back(V3(cx + half * ca / m, y, cz + half * sa / m));
		}
		return p;
	}
	static std::vector<V3> ngon(double cx, double cz, double r, double y, int n) {
		std::vector<V3> p;
		for (int k = 0; k < n; k++) { double a = 2 * M_PI * (k + 0.5) / n; p.push_back(V3(cx + r * std::cos(a), y, cz + r * std::sin(a))); }
		return p;
	}
	static std::vector<V3> rect(double x0, double z0, double x1, double z1, double y) {
		return {V3(x0, y, z0), V3(x1, y, z0), V3(x1, y, z1), V3(x0, y, z1)};
	}
	// where something can stand, on a grid of `cell`
	template <typename F> void airGrid(double cell, F isAir) {
		s.gcell = cell; s.glo = s.lo;
		s.ggx = (int)std::ceil((s.hi.x - s.lo.x) / cell); s.ggy = (int)std::ceil((s.hi.y - s.lo.y) / cell); s.ggz = (int)std::ceil((s.hi.z - s.lo.z) / cell);
		s.gbits.assign(((size_t)s.ggx * s.ggy * s.ggz + 7) / 8, 0);
		for (int z = 0; z < s.ggz; z++) for (int y = 0; y < s.ggy; y++) for (int x = 0; x < s.ggx; x++)
			if (isAir(V3(s.glo.x + (x + 0.5) * cell, s.glo.y + (y + 0.5) * cell, s.glo.z + (z + 0.5) * cell))) {
				size_t i = (size_t)x + (size_t)s.ggx * ((size_t)y + (size_t)s.ggy * z);
				s.gbits[i >> 3] |= 1 << (i & 7);
			}
	}
	void finish() { s.mesh = true; s.clean = true; buildBVH(s); }
};

// The Dan Harpole Cistern, Fort Worden: 186 ft (56.7 m) across and 14 ft
// (4.27 m) deep, so round. Inside, from Centrum's photographs: square columns
// with flared capitals and pyramid footings on a square grid, and a grid of
// beams under the slab, haunched into each capital. Spacing and member sizes
// are ESTIMATES from the photographs until plans turn up. The beam grid is
// acoustically a coffered ceiling: it breaks up the floor-to-lid bounce that a
// flat lid sends straight back.
struct FortWordenDims {
	double R = 28.35, H = 4.27, spacing = 4.9;
	double col = 0.40, baseW = 1.2, baseH = 0.40, capW = 1.0, capH = 0.55, beamW = 0.35, beamD = 0.40;
};
inline Space makeCistern() {
	FortWordenDims d;
	Space s; s.name = "Dan Harpole Cistern";
	s.rough = 0.5;
	Builder b(s);
	// floor, lid and the round wall, traced exactly
	s.planes.push_back({V3(0, 1, 0), 0.0, M_SEALED_CONCRETE});
	s.planes.push_back({V3(0, -1, 0), -d.H, M_SEALED_CONCRETE});
	s.cyls.push_back({0, 0, d.R, 0, d.H, M_SEALED_CONCRETE, true});
	s.area[M_SEALED_CONCRETE] += 2 * M_PI * d.R * d.R + 2 * M_PI * d.R * d.H;
	const int K = 128;
	V3 o(0, 0, 0), top(0, d.H, 0);
	for (int k = 0; k < K; k++) {
		double a0 = 2 * M_PI * k / K, a1 = 2 * M_PI * (k + 1) / K;
		V3 p0(d.R * std::cos(a0), 0, d.R * std::sin(a0)), p1(d.R * std::cos(a1), 0, d.R * std::sin(a1));
		V3 q0 = p0 + V3(0, d.H, 0), q1 = p1 + V3(0, d.H, 0);
		b.drawTri(o, p0, p1, M_SEALED_CONCRETE, V3(0, 1, 0));
		b.drawTri(top, q0, q1, M_SEALED_CONCRETE, V3(0, -1, 0));
		b.drawQuad(p0, p1, q1, q0, M_SEALED_CONCRETE, (p0 + p1) * -1.0);
	}
	// the columns: offset half a bay, so the centre is open floor
	int kk = (int)std::floor(d.R / d.spacing);
	auto exists = [&](int i, int j) {
		double x = (i + 0.5) * d.spacing, z = (j + 0.5) * d.spacing;
		return std::sqrt(x * x + z * z) <= d.R - d.spacing * 0.6;
	};
	double yCap = d.H - d.beamD - d.capH, yBeam = d.H - d.beamD;
	for (int i = -kk - 1; i <= kk; i++) for (int j = -kk - 1; j <= kk; j++) {
		if (!exists(i, j)) continue;
		double x = (i + 0.5) * d.spacing, z = (j + 0.5) * d.spacing;
		b.loft(Builder::square(x, z, d.baseW / 2, 0), Builder::square(x, z, d.col / 2, d.baseH), M_SEALED_CONCRETE);
		b.loft(Builder::square(x, z, d.col / 2, d.baseH), Builder::square(x, z, d.col / 2, yCap), M_SEALED_CONCRETE);
		b.loft(Builder::square(x, z, d.col / 2, yCap), Builder::square(x, z, d.capW / 2, yBeam), M_SEALED_CONCRETE);
		b.loft(Builder::square(x, z, d.capW / 2, yBeam), Builder::square(x, z, d.capW / 2, d.H), M_SEALED_CONCRETE);
		// the beams to the next column along each axis
		if (exists(i + 1, j)) {
			double x0 = x + d.capW / 2, x1 = x + d.spacing - d.capW / 2;
			b.loft(Builder::rect(x0, z - d.beamW / 2, x1, z + d.beamW / 2, yBeam), Builder::rect(x0, z - d.beamW / 2, x1, z + d.beamW / 2, d.H), M_SEALED_CONCRETE, true);
		}
		if (exists(i, j + 1)) {
			double z0 = z + d.capW / 2, z1 = z + d.spacing - d.capW / 2;
			b.loft(Builder::rect(x - d.beamW / 2, z0, x + d.beamW / 2, z1, yBeam), Builder::rect(x - d.beamW / 2, z0, x + d.beamW / 2, z1, d.H), M_SEALED_CONCRETE, true);
		}
	}
	s.volume = M_PI * d.R * d.R * d.H - b.solid;
	s.lo = V3(-d.R, 0, -d.R); s.hi = V3(d.R, d.H, d.R);
	s.src = V3(-6.0, 1.5, 3.0); s.lis = V3(8.0, 1.6, -2.0);
	b.airGrid(0.25, [&](const V3& p) {
		if (std::hypot(p.x, p.z) > d.R - 0.15 || p.y < 0.1 || p.y > yBeam - 0.1) return false;
		double fx = std::floor(p.x / d.spacing), fz = std::floor(p.z / d.spacing);
		double cxx = (fx + 0.5) * d.spacing, czz = (fz + 0.5) * d.spacing;
		if (!exists((int)fx, (int)fz)) return true;
		double half = p.y < d.baseH ? d.baseW / 2 : p.y > yCap ? d.capW / 2 : d.col / 2;
		return std::fabs(p.x - cxx) > half + 0.1 || std::fabs(p.z - czz) > half + 0.1;
	});
	b.finish();
	return s;
}

// The Buffalo Bayou Park Cistern, Houston (1926): about 87,500 sq ft, 221
// columns 25 ft (7.62 m) tall, round with belled capitals and square bases,
// under a flat slab; a few inches of water on the floor; a reverberation time
// measured at more than 15 s and quoted at about 17. 221 is 13 x 17, and at
// 20 ft bays that grid covers 88,400 sq ft: the grid is that INFERENCE. The
// sloped skirt along the walls (2.4 m up over 2.4 m in) and the member sizes
// are ESTIMATES from photographs. The concrete is chosen to land the measured
// decay.
inline Space makeHouston() {
	const double sp = 6.096, H = 7.62, hx = 6.5 * sp, hz = 8.5 * sp, skirt = 2.4;
	const double shaftR = 0.30, baseHalf = 0.45, baseH = 0.30, capH = 1.0, plateHalf = 0.9, plateH = 0.25;
	Space s; s.name = "Buffalo Bayou Cistern";
	s.rough = 0.4;
	Builder b(s);
	const int W = M_BOARD_CONCRETE, F = M_WATER;
	// the floor inside the skirt, the skirt sloping up to the walls, the
	// walls, the slab
	double ix = hx - skirt, iz = hz - skirt;
	// floor, slab and walls traced as planes (the skirt is in front of the
	// walls' lower part, so a ray meets it first), drawn as faces
	s.planes.push_back({V3(0, 1, 0), 0.0, F});
	s.planes.push_back({V3(0, -1, 0), -H, W});
	s.planes.push_back({V3(1, 0, 0), -hx, W}); s.planes.push_back({V3(-1, 0, 0), -hx, W});
	s.planes.push_back({V3(0, 0, 1), -hz, W}); s.planes.push_back({V3(0, 0, -1), -hz, W});
	s.area[F] += 4 * ix * iz;
	s.area[W] += 4 * hx * hz + 2 * (2 * hx + 2 * hz) * (H - skirt);
	b.drawQuad(V3(-ix, 0, -iz), V3(ix, 0, -iz), V3(ix, 0, iz), V3(-ix, 0, iz), F, V3(0, 1, 0));
	V3 in[4] = {V3(-ix, 0, -iz), V3(ix, 0, -iz), V3(ix, 0, iz), V3(-ix, 0, iz)};
	V3 out[4] = {V3(-hx, skirt, -hz), V3(hx, skirt, -hz), V3(hx, skirt, hz), V3(-hx, skirt, hz)};
	for (int k = 0; k < 4; k++) {
		int k2 = (k + 1) % 4;
		V3 mid = (in[k] + in[k2]) * 0.5;
		b.quad(in[k], in[k2], out[k2], out[k], W, V3(-mid.x, std::fabs(mid.x) + std::fabs(mid.z), -mid.z));   // the skirt, traced
		V3 t0 = out[k] + V3(0, H - skirt, 0), t1 = out[k2] + V3(0, H - skirt, 0);
		b.drawQuad(out[k], out[k2], t1, t0, W, V3(-mid.x, 0, -mid.z));
	}
	b.drawQuad(V3(-hx, H, -hz), V3(hx, H, -hz), V3(hx, H, hz), V3(-hx, H, hz), W, V3(0, -1, 0));
	for (int i = 0; i < 13; i++) for (int j = 0; j < 17; j++) {
		double x = (i - 6) * sp, z = (j - 8) * sp;
		b.loft(Builder::square(x, z, baseHalf, 0), Builder::square(x, z, baseHalf, baseH), W, false, true);
		b.loft(Builder::ngon(x, z, shaftR, baseH, 8), Builder::ngon(x, z, shaftR, H - capH, 8), W);
		b.loft(Builder::ngon(x, z, shaftR, H - capH, 8), Builder::square(x, z, plateHalf, H - plateH, 8), W);
		b.loft(Builder::square(x, z, plateHalf, H - plateH), Builder::square(x, z, plateHalf, H), W, true);
	}
	// the room is a box less the skirt's wedge
	double wedge = 0.5 * skirt * skirt * 2 * (2 * hx + 2 * hz);
	s.volume = 2 * hx * 2 * hz * H - wedge - b.solid;
	s.lo = V3(-hx, 0, -hz); s.hi = V3(hx, H, hz);
	s.src = V3(0.5 * sp, 1.5, -1.5 * sp); s.lis = V3(0.5 * sp, 1.6, 1.5 * sp);
	b.airGrid(0.3, [&](const V3& p) {
		if (p.y < 0.1 || p.y > H - plateH - 0.1) return false;
		double dx = hx - std::fabs(p.x), dz = hz - std::fabs(p.z), dw = std::min(dx, dz);
		if (dw < 0.15 || p.y < skirt - dw + 0.15) return false;           // inside the skirt's slope
		double cxx = std::round(p.x / sp) * sp, czz = std::round(p.z / sp) * sp;
		if (std::fabs(cxx) > 6 * sp + 1 || std::fabs(czz) > 8 * sp + 1) return true;
		double half = p.y < baseH ? baseHalf : p.y > H - capH ? plateHalf : shaftR;
		return std::fabs(p.x - cxx) > half + 0.1 || std::fabs(p.z - czz) > half + 0.1;
	});
	b.finish();
	return s;
}

// The Basilica Cistern, Istanbul (6th century): 138 x 65 m, 336 marble columns
// 9 m high in 12 rows of 28 about 5 m apart (Ionic and Corinthian capitals,
// most of them spolia), a firebrick wall 4 m thick coated with waterproofing
// mortar, the original brick floor kept with a little water on it, and brick
// CROSS VAULTS springing from the columns. The vault's rise (half the bay) and
// the column members are ESTIMATES. Each bay's vault is two barrels crossing
// (the soffit is the higher of the two), so arches stand over every column line
// and the groins run corner to corner; built as a height field whose
// triangles split along the groin, so the crease is exact.
inline Space makeBasilica() {
	const double L = 138, Wd = 65, sx = L / 28, sz = Wd / 12, impost = 9.0, rise = 0.5 * std::min(sx, sz);
	const double hx = L / 2, hz = Wd / 2;
	const double baseHalf = 0.55, baseH = 0.5, shaftR = 0.38, capH = 0.8, capHalf = 0.6;
	Space s; s.name = "Basilica Cistern";
	s.rough = 0.4;
	s.foldDeg = 65;                                       // a vault's facets and a six-sided shaft fold gently (60 deg); arches, groins, bases sharply: the shafts draw as their silhouettes
	Builder b(s);
	const int F = M_WATER, Wm = M_PLASTER, V = M_BRICK, C = M_MARBLE;
	// columns sit on lines x = (i - 13.5) sx, z = (j - 5.5) sz; bays between them
	auto vault = [&](double x, double z) {
		double u = x / sx + 13.5, v = z / sz + 5.5;                  // column lines at integers
		double du = std::fabs(u - std::floor(u) - 0.5) * 2, dv = std::fabs(v - std::floor(v) - 0.5) * 2;   // 0 at bay centre, 1 at a column line
		double h1 = std::sqrt(std::max(0.0, 1 - du * du)), h2 = std::sqrt(std::max(0.0, 1 - dv * dv));
		return impost + rise * std::max(h1, h2);
	};
	// the vault: per bay N x N cells, each split along its quadrant's groin
	const int N = 10;
	double u0 = -hx / sx + 13.5, u1 = hx / sx + 13.5, v0 = -hz / sz + 5.5, v1 = hz / sz + 5.5;
	int cu = (int)std::ceil((u1 - u0) * N), cv = (int)std::ceil((v1 - v0) * N);
	auto P = [&](int i, int j) {
		double u = std::min(u1, u0 + (double)i / N), v = std::min(v1, v0 + (double)j / N);
		double x = (u - 13.5) * sx, z = (v - 5.5) * sz;
		return V3(x, vault(x, z), z);
	};
	for (int i = 0; i < cu; i++) for (int j = 0; j < cv; j++) {
		V3 a = P(i, j), bb = P(i + 1, j), c = P(i + 1, j + 1), d = P(i, j + 1);
		// which diagonal is the groin: in a bay quadrant where u and v rise
		// together, the one from the low corner to the high
		double um = u0 + (i + 0.5) / N, vm = v0 + (j + 0.5) / N;
		double fu = um - std::floor(um) - 0.5, fv = vm - std::floor(vm) - 0.5;
		if (fu * fv > 0) { b.tri(a, bb, c, V, V3(0, -1, 0)); b.tri(a, c, d, V, V3(0, -1, 0)); }
		else { b.tri(a, bb, d, V, V3(0, -1, 0)); b.tri(bb, c, d, V, V3(0, -1, 0)); }
	}
	// floor and walls traced as planes, drawn as faces (walls to the impost; the
	// vault's own edge draws where it meets them)
	s.planes.push_back({V3(0, 1, 0), 0.0, F});
	s.planes.push_back({V3(1, 0, 0), -hx, Wm}); s.planes.push_back({V3(-1, 0, 0), -hx, Wm});
	s.planes.push_back({V3(0, 0, 1), -hz, Wm}); s.planes.push_back({V3(0, 0, -1), -hz, Wm});
	s.area[F] += L * Wd;
	s.area[Wm] += 2 * (L + Wd) * (impost + 0.6 * rise);
	b.drawQuad(V3(-hx, 0, -hz), V3(hx, 0, -hz), V3(hx, 0, hz), V3(-hx, 0, hz), F, V3(0, 1, 0));
	V3 c4[4] = {V3(-hx, 0, -hz), V3(hx, 0, -hz), V3(hx, 0, hz), V3(-hx, 0, hz)};
	for (int k = 0; k < 4; k++) {
		const V3 &p0 = c4[k], &p1 = c4[(k + 1) % 4];
		V3 m = (p0 + p1) * -0.5;
		b.drawQuad(p0, p1, p1 + V3(0, impost, 0), p0 + V3(0, impost, 0), Wm, V3(m.x, 0, m.z));
	}
	for (int i = 0; i < 28; i++) for (int j = 0; j < 12; j++) {
		double x = (i - 13.5) * sx, z = (j - 5.5) * sz;
		b.loft(Builder::square(x, z, baseHalf, 0), Builder::square(x, z, baseHalf, baseH), C, false, true);
		b.loft(Builder::ngon(x, z, shaftR, baseH, 6), Builder::ngon(x, z, shaftR, impost - capH, 6), C);
		b.loft(Builder::ngon(x, z, shaftR, impost - capH, 6), Builder::square(x, z, capHalf, impost, 6), C);
	}
	// the air: the box to the impost, the vault's mean rise above it, less the columns
	s.volume = L * Wd * (impost + 0.7 * rise) - b.solid;
	s.lo = V3(-hx, 0, -hz); s.hi = V3(hx, impost + rise, hz);
	s.src = V3(-1.0 * sx, 1.6, 0); s.lis = V3(2.0 * sx, 1.6, 0);
	b.airGrid(0.35, [&](const V3& p) {
		if (p.y < 0.2 || p.y > vault(p.x, p.z) - 0.2) return false;
		if (std::fabs(p.x) > hx - 0.2 || std::fabs(p.z) > hz - 0.2) return false;
		double cxx = (std::round(p.x / sx + 13.5) - 13.5) * sx, czz = (std::round(p.z / sz + 5.5) - 5.5) * sz;
		double half = p.y < baseH ? baseHalf : p.y > impost - capH ? capHalf : shaftR;
		return std::fabs(p.x - cxx) > half + 0.1 || std::fabs(p.z - czz) > half + 0.1;
	});
	b.finish();
	return s;
}

// ── the program the audio thread plays: built on the worker ────────────────
static const int RB = 3;                                   // render bands: low, mid, high
static const int NSRC = 2;                                 // sources: one per input channel
struct Tap { int delay; int in; float g[2][RB]; };        // which input, per ear, per render band
struct Program {
	std::vector<Tap> taps;
	int maxDelay = 1;
	double t60[NB] = {};                                   // the room's decay per band
	double t60x8k = 1;                                     // and at 8 kHz, for the tail's filter
	double tMix = 0.1;                                     // seconds: where the tail takes over
	double eEnd = 0;                                       // the echogram's mid-band level before the join, per second per ear
	double eEndRays = 0, eEndDiffuse = 0;                  // what the rays found there, and what a diffuse field would give
	double tailGain = 1.0;                                 // calibrated so the join is level
	int tailDelay = 0;                                     // samples: so the tail starts at tMix
	double wetNorm = 1.0;                                  // so a steady input's wet level matches the dry
	double decay = 1.0;                                    // the scale already applied to t60
	uint32_t traceId = 0;                                  // which trace its early field came from
	double sr = 48000;
	// stats for the harness and the screen
	int rays = 0, detections = 0, nsrc = 1; double mfp = 0, volume = 0, surface = 0;
};

struct BuildParams {
	double tempC = 20, rh = 50, decayScale = 1.0;
	double scatter = -1;                                   // >= 0 overrides the space's roughness (the panel's control)
	int rays = 100000; uint64_t seed = 1;                  // 0.3 s on the worker for the cistern, per source
	bool trueDistance = false;                             // else far sources are pulled in (musicalDistance)
};

// Distance, made musical. Out to DIST_NEAR metres a source is where it is;
// beyond, the extra distance counts logarithmically (DIST_K a factor of e), so
// a source 150 m off arrives as from 65 m and one 300 m off as from 79 m:
// still later, softer and duller the further it goes, but still in the room
// with you. True distance in a 300 m tunnel is a faint dull arrival half a
// second late, buried in the reverb: right, and not much to play with. Applied
// by pulling a source's whole arrival pattern in by the same excess (every
// path, its delay, its 1/r^2 and its air loss), so the room's response keeps
// its shape and no source is moved through a wall to do it.
static const double DIST_NEAR = 25.0, DIST_K = 20.0;
inline double musicalDistance(double d) {
	return d <= DIST_NEAR ? d : DIST_NEAR + DIST_K * std::log(1.0 + (d - DIST_NEAR) / DIST_K);
}

// render bands from octave bands (energy means)
inline void toRender(const double e[NB], double out[RB]) {
	out[0] = 0.5 * (e[0] + e[1]); out[1] = 0.5 * (e[2] + e[3]); out[2] = 0.5 * (e[4] + e[5]);
}

// The decay the tail must have. The cistern is analytic, so Eyring is exact
// for it. A scanned room is not: an opening is part of its absorption, its
// shape decides how often a ray meets a wall, and Eyring knows neither. So its
// decay is MEASURED from rays: how much energy a reflection keeps on average
// (energy-weighted, an opening or an escape keeping none) and how far a ray
// travels between reflections, per band, with the air on top.
inline void rayDecay(const Space& s, const BuildParams& bp, const V3& src, double t60[NB], double& t60x8k, double& mfp) {
	Rng rng(bp.seed * 31 + 5);
	double before[NB] = {}, after[NB] = {}, L = 0; long nseg = 0;
	// 2000 rays of up to 200 reflections: the per-reflection average converges
	// long before a stone room's energy runs out (which takes ~300 bounces), and
	// 4000 x 1000 took four seconds in the larger scans
	const int N = 2000;
	for (int r = 0; r < N; r++) {
		V3 o = src, d = randDir(rng);
		double en[NB]; for (int b = 0; b < NB; b++) en[b] = 1.0;
		for (int bounce = 0; bounce < 200; bounce++) {
			double t; V3 n; int mat;
			if (!intersect(s, o, d, t, n, mat)) { for (int b = 0; b < NB; b++) before[b] += en[b]; break; }
			L += t; nseg++;
			o = o + d * t;
			const Material& M = MATERIALS[mat];
			for (int b = 0; b < NB; b++) { before[b] += en[b]; en[b] *= (1.0 - M.a[b]); after[b] += en[b]; }
			if (en[0] < 1e-4) break;
			// a surface that is a mirror (water) stays one whatever SCATTER says
			double scat = M.scatter <= 0 ? 0 : bp.scatter >= 0 ? bp.scatter : s.rough >= 0 ? s.rough : M.scatter;
			if (rng.u() < scat) d = lambert(rng, n);
			else d = d - n * (2 * d.dot(n));
		}
	}
	mfp = nseg ? L / nseg : 1.0;
	auto T = [&](double keep, double f) {
		double perHit = -std::log(std::max(1e-9, keep));
		return 13.8155 / (C_SOUND / mfp * perHit + C_SOUND * airM(f, bp.tempC, bp.rh));
	};
	for (int b = 0; b < NB; b++) t60[b] = T(after[b] / std::max(1e-30, before[b]), BAND_HZ[b]);
	t60x8k = T(after[NB - 1] / std::max(1e-30, before[NB - 1]), 8000);
}
inline void roomDecay(const Space& s, const BuildParams& bp, const V3& src, Program& P) {
	if (s.mesh) { rayDecay(s, bp, src, P.t60, P.t60x8k, P.mfp); return; }
	decayTimes(s, bp.tempC, bp.rh, P.t60);
	P.t60x8k = decay8k(s, bp.tempC, bp.rh);
	P.mfp = s.meanFreePath();
}

// ── the spaces ─────────────────────────────────────────────────────────────
// The cistern is built from its dimensions; the rest are photogrammetry scans
// converted by tools/cistern-convert.cpp (the commands are in
// docs/cistern-design.md). Each names the materials of its floor, walls and
// ceiling, how rough it is at room scale, and its credit: the scans are CC BY
// 4.0 and the credit travels with them.
struct SpaceInfo {
	const char* slug; const char* name; const char* file; const char* credit;
	int floor, wall, ceil; double rough;
	Space (*make)();                                      // a room built from its dimensions
};
static const SpaceInfo SPACES[] = {
	{"cistern",    "Dan Harpole Cistern",           nullptr, "", M_SEALED_CONCRETE, M_SEALED_CONCRETE, M_SEALED_CONCRETE, 0.5, makeCistern},
	{"houston",    "Buffalo Bayou Cistern",         nullptr, "", M_WATER, M_BOARD_CONCRETE, M_BOARD_CONCRETE, 0.4, makeHouston},
	{"basilica",   "Basilica Cistern, Istanbul",    nullptr, "", M_WATER, M_PLASTER, M_BRICK, 0.4, makeBasilica},
	{"pantheon",   "The Pantheon",                  "pantheon.bin",   "\"The Pantheon Interior\" by artfletch, skfb.ly/pJOwI, CC BY 4.0", M_PEWS, M_MARBLE, M_COFFERED, 0.5, nullptr},
	{"hintze",     "Hintze Hall",                   "hintze.bin",     "\"Hintze Hall\" by artfletch, skfb.ly/oF9N6, CC BY 4.0", M_FURNISHED, M_STONE, M_PLASTER, 0.4, nullptr},
	{"leake",      "Leake Street Tunnel",           "leake.bin",      "\"Leake Street Graffiti Tunnel\" by artfletch, skfb.ly/6TFJV, CC BY 4.0", M_ROUGH_CONCRETE, M_BRICK, M_BRICK, 0.4, nullptr},
	{"oldchurch",  "Old Church",                    "oldchurch.bin",  "\"Old church modeling - Interior Scene\" by Aurélien Martel, skfb.ly/STIp, CC BY 4.0", M_PEWS, M_STONE, M_WOOD, 0.4, nullptr},
	{"undercroft", "Lincoln's Inn Undercroft",      "undercroft.bin", "\"Lincoln's Inn Chapel Undercroft\" by artfletch, skfb.ly/oItqQ, CC BY 4.0", M_STONE, M_ROUGH_STONE, M_ROUGH_STONE, 0.5, nullptr},
	{"bartless",   "St Bartholomew-the-Less",       "bartless.bin",   "\"St Bartholomew-the-Less Interior\" by artfletch, skfb.ly/o6rPA, CC BY 4.0", M_PEWS, M_PLASTER, M_LATH, 0.4, nullptr},
	{"stourton",   "St Peter, Stourton",            "stourton.bin",   "\"Church Of St Peter Stourton\" by artfletch, skfb.ly/oGNzU, CC BY 4.0", M_PEWS, M_PLASTER, M_WOOD, 0.4, nullptr},
	{"mosque",     "Mosque of the Tree, Jordan",    "mosque.bin",     "\"Mosque of Tree of Al Buqayawiyya, Jordan\" by SJoudeh, skfb.ly/oZYxr, CC BY 4.0", M_CARPET, M_ROUGH_STONE, M_ROUGH_STONE, 0.5, nullptr},
	{"salt",       "Salt Tower, Lower Room",        "salt.bin",       "\"Salt Tower Lower Room\" by artfletch, skfb.ly/oNoqN, CC BY 4.0", M_STONE, M_ROUGH_STONE, M_ROUGH_STONE, 0.5, nullptr},
};
static const int NSPACES = (int)(sizeof(SPACES) / sizeof(SPACES[0]));

// a space, loaded: the cistern built, a scan read from `dir`
inline bool makeSpace(int id, const std::string& dir, Space& s, std::string& err) {
	const SpaceInfo& I = SPACES[std::max(0, std::min(NSPACES - 1, id))];
	if (I.make) { s = I.make(); s.name = I.name; return true; }
	int cm[4] = {I.floor, I.wall, I.ceil, M_OPEN};
	if (!loadSpace(dir + "/" + I.file, cm, s, err)) return false;
	s.name = I.name;
	s.rough = I.rough;
	return true;
}

// Trace the early field: everything that depends on where things are, and
// nothing that depends on the decay, so a DECAY change reuses it. Receiver: a
// sphere at the listener; stereo from the arrival direction through two
// cardioids aimed +-55 degrees off the listener's facing (towards the sources'
// midpoint), so the image follows the geometry. With two sources (one per input
// channel) each carries half the energy, as a pair of speakers sharing a signal.
inline std::unique_ptr<Program> trace(const Space& s, const BuildParams& bp, double sr,
                                      const V3* srcs, int nsrc, const V3& lis, const Program* decayFrom = nullptr) {
	std::unique_ptr<Program> P(new Program());
	static std::atomic<uint32_t> traces{0};
	P->traceId = ++traces;
	nsrc = std::max(1, std::min(NSRC, nsrc));
	P->sr = sr; P->volume = s.volume; P->surface = s.surface(); P->nsrc = nsrc;
	// The decay depends on the room, its materials and the air, not on where
	// anyone stands: a caller dragging a source passes the last program back
	// and the measurement (half the cost of a trace in a scanned room) is kept.
	if (decayFrom) {
		for (int b = 0; b < NB; b++) P->t60[b] = decayFrom->t60[b];
		P->t60x8k = decayFrom->t60x8k; P->mfp = decayFrom->mfp;
	} else roomDecay(s, bp, s.src, *P);   // from the room's own reference point, not the dragged source:
	                                      // a source dragged to Leake Street's open end lost its rays out of
	                                      // the tunnel on the first trip and measured the room at 0.1 s
	// the mixing time, but never before the direct sound and a little after it:
	// in a 300 m tunnel the source can be further away than the mixing time
	// reaches, and the early window then held nothing at all, not even the
	// direct path
	// how far each source's paths are pulled in (none with true distance)
	double excess[NSRC] = {0, 0}, dmax = 0;
	for (int k = 0; k < nsrc; k++) {
		double d = (srcs[k] - lis).len();
		double de = bp.trueDistance ? d : musicalDistance(d);
		excess[k] = d - de;
		dmax = std::max(dmax, de);
	}
	P->tMix = std::min(0.9, std::max(std::max(0.04, std::min(0.3, 0.002 * std::sqrt(s.volume))), dmax / C_SOUND + 0.06));
	double m[NB]; for (int b = 0; b < NB; b++) m[b] = airM(BAND_HZ[b], bp.tempC, bp.rh);

	const double binSec = 0.00025;
	int nbins = (int)(P->tMix / binSec) + 1;
	std::vector<double> E[NSRC][2][RB];
	std::vector<int> sign[NSRC];
	for (int k = 0; k < nsrc; k++) {
		for (int e = 0; e < 2; e++) for (int r = 0; r < RB; r++) E[k][e][r].assign(nbins, 0.0);
		sign[k].assign(nbins, 0);
	}

	V3 mid; for (int k = 0; k < nsrc; k++) mid = mid + srcs[k] * (1.0 / nsrc);
	V3 face = (mid - lis); face.y = 0; face = face.len() > 1e-6 ? face.norm() : V3(0, 0, 1);
	V3 side(face.z, 0, -face.x);                            // listener's right (y up)
	auto earGains = [&](const V3& from, double g[2]) {      // from: unit vector, listener -> arrival
		double c = from.dot(face), sd = from.dot(side);
		const double ca = std::cos(55 * M_PI / 180), sa = std::sin(55 * M_PI / 180);
		double cosL = c * ca - sd * sa, cosR = c * ca + sd * sa;   // angle to each mic's axis
		g[0] = 0.5 + 0.5 * cosL; g[1] = 0.5 + 0.5 * cosR;
	};
	const double srcW = 1.0 / nsrc;

	const double rr = std::max(0.5, 0.02 * std::cbrt(s.volume));     // receiver radius
	const double maxLen = P->tMix * C_SOUND;
	// each source traced on its own thread: they share nothing but the room
	std::atomic<int> detections{0};
	auto traceSource = [&](int k) {
		const V3& src = srcs[k];
		// the direct sound, exact
		{
			V3 v = src - lis; double dist = std::max(0.3, v.len() - excess[k]);
			double g[2]; earGains(v * (1.0 / std::max(1e-6, v.len())), g);
			int bin = (int)(dist / C_SOUND / binSec);
			if (bin < nbins) {
				double eb[NB], er[RB];
				for (int b = 0; b < NB; b++) eb[b] = std::exp(-m[b] * dist) / (dist * dist);
				toRender(eb, er);
				for (int e = 0; e < 2; e++) for (int r = 0; r < RB; r++) E[k][e][r][bin] += er[r] * g[e] * g[e] * srcW;
				sign[k][bin] += 4;
			}
		}
		// the rays
		Rng rng(bp.seed + 7919 * k);
		for (int ray = 0; ray < bp.rays; ray++) {
			V3 o = src, d = randDir(rng);
			double en[NB]; for (int b = 0; b < NB; b++) en[b] = 1.0;
			double travelled = 0;
			for (int bounce = 0; bounce < 400 && travelled < maxLen + excess[k]; bounce++) {
				double t; V3 n; int mat;
				if (!intersect(s, o, d, t, n, mat)) break;
				// the receiver: closest approach on this segment, after the first bounce
				// (the direct path is added exactly above)
				if (bounce > 0) {
					V3 w = lis - o; double tc = w.dot(d);
					if (tc > 0 && tc < t) {
						double miss = (w - d * tc).len();
						if (miss < rr) {
							double Lt = travelled + tc, L = Lt - excess[k];   // the path as heard: pulled in
							int bin = (int)(L / C_SOUND / binSec);
							if (bin < nbins) {
								double eb[NB], er[RB];
								// the ray density gives 1/Lt^2; the pulled-in path is heard at 1/L^2
								double pull = (Lt * Lt) / (L * L);
								for (int b = 0; b < NB; b++) eb[b] = en[b] * std::exp(-m[b] * L) * pull;
								toRender(eb, er);
								double g[2]; earGains(d * -1.0, g);
								// N rays through a sphere of radius rr at distance L carry rr²/(4L²) of
								// the energy, so this weighting makes the direct path read 1/L² exactly
								double w8 = 4.0 / (rr * rr) / bp.rays * srcW;
								for (int e = 0; e < 2; e++) for (int r = 0; r < RB; r++) E[k][e][r][bin] += er[r] * g[e] * g[e] * w8;
								sign[k][bin] += (rng.u() < 0.5) ? 1 : -1;
								detections++;
							}
						}
					}
				}
				travelled += t;
				o = o + d * t;
				const Material& M = MATERIALS[mat];
				for (int b = 0; b < NB; b++) en[b] *= (1.0 - M.a[b]);
				if (en[3] < 1e-7) break;
				double scat = M.scatter <= 0 ? 0 : bp.scatter >= 0 ? bp.scatter : s.rough >= 0 ? s.rough : M.scatter;
				if (rng.u() < scat) d = lambert(rng, n);          // diffuse
				else d = d - n * (2 * d.dot(n));
			}
		}
	};
	if (nsrc > 1) {
		std::thread other(traceSource, 1);
		traceSource(0);
		other.join();
	} else traceSource(0);
	P->detections = detections.load();
	P->rays = bp.rays * nsrc;

	// taps: one per non-empty bin per source, amplitude sqrt(energy), sign from the arrivals
	double peak = 0;
	for (int k = 0; k < nsrc; k++) for (int i = 0; i < nbins; i++) for (int e = 0; e < 2; e++) for (int r = 0; r < RB; r++) peak = std::max(peak, E[k][e][r][i]);
	for (int k = 0; k < nsrc; k++)
		for (int i = 0; i < nbins; i++) {
			double tot = 0; for (int e = 0; e < 2; e++) for (int r = 0; r < RB; r++) tot += E[k][e][r][i];
			if (tot < peak * 1e-6) continue;
			Tap tp; tp.delay = (int)std::lround((i + 0.5) * binSec * sr); tp.in = k;
			float sg = sign[k][i] >= 0 ? 1.f : -1.f;
			for (int e = 0; e < 2; e++) for (int r = 0; r < RB; r++) tp.g[e][r] = sg * (float)std::sqrt(E[k][e][r][i]);
			P->taps.push_back(tp);
			P->maxDelay = std::max(P->maxDelay, tp.delay + 1);
		}
	// The echogram's level just before the join, for the calibration. With few
	// rays caught there (a big room, a short window) that level is a handful of
	// arrivals and means little, so it falls back on the diffuse field: an
	// impulse of the energy that reads 1/r^2 direct leaves 4 pi c / V per
	// second, decaying at the room's rate, a third of it through each cardioid.
	double eEnd = 0; int n0 = (int)(nbins * 0.8), caught = 0;
	for (int k = 0; k < nsrc; k++) for (int i = n0; i < nbins; i++) {
		for (int e = 0; e < 2; e++) eEnd += E[k][e][1][i];
		if (E[k][0][1][i] > 0) caught++;
	}
	P->eEndRays = eEnd / ((double)(nbins - n0) * binSec * 2);   // mid band, per second, per ear
	double tm = 0.5 * (P->t60[2] + P->t60[3]);
	P->eEndDiffuse = 4 * M_PI * C_SOUND / s.volume / 3.0 * std::exp(-13.8155 * 0.9 * P->tMix / tm);
	P->eEnd = caught >= 30 ? P->eEndRays : P->eEndDiffuse;
	return P;
}

inline void calibrate(Program& P, double modAmt, uint64_t seed);
// A traced early field made playable at a decay: the tail calibrated against
// it, the early taps faded where the tail comes in, and the wet level set.
inline std::unique_ptr<Program> assemble(const Program& traced, double decay, double modAmt = 1.0, uint64_t seed = 7) {
	std::unique_ptr<Program> P(new Program(traced));
	P->decay = decay;
	for (int b = 0; b < NB; b++) P->t60[b] *= decay;
	P->t60x8k *= decay;
	calibrate(*P, modAmt, seed);
	return P;
}

// one source, the space's own positions: the harness's and the render tool's case
inline std::unique_ptr<Program> build(const Space& s, const BuildParams& bp, double sr) {
	auto T = trace(s, bp, sr, &s.src, 1, s.lis);
	return assemble(*T, bp.decayScale);
}

// ── the late tail: a 16-line FDN ───────────────────────────────────────────
static const int NL = 16;
// The decay each line must apply is a CURVE across frequency, and the curve is
// steep: in the cistern a line loses 0.1 dB a pass at 125 Hz and 3 dB at 8 kHz.
// A three-band split cannot draw that (it measured +40% at 500 Hz and +96% at
// 4 kHz), so each line carries a flat gain and six high shelves between the
// octaves, solved so its loss at every octave centre is exactly the room's
// (Jot's attenuation filter). Losses this small add in dB, so the solve is a
// linear system, refined against the real filters' response.
static const int EQ_N = 7;
static const double EQ_HZ[EQ_N] = {125, 250, 500, 1000, 2000, 4000, 8000};
static const double SHELF_HZ[EQ_N - 1] = {177, 354, 707, 1414, 2828, 5657};

struct BQ {
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
	inline double run(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
	void highShelf(double f, double gDb, double sr) {               // RBJ, slope 1
		double A = std::pow(10.0, gDb / 40), w = 2 * M_PI * f / sr, c = std::cos(w);
		double al = std::sin(w) / 2 * std::sqrt(2.0), sA = 2 * std::sqrt(A) * al;
		double a0 = (A + 1) - (A - 1) * c + sA;
		b0 = A * ((A + 1) + (A - 1) * c + sA) / a0;
		b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
		b2 = A * ((A + 1) + (A - 1) * c - sA) / a0;
		a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
		a2 = ((A + 1) - (A - 1) * c - sA) / a0;
	}
	double dbAt(double f, double sr) const {
		std::complex<double> z = std::polar(1.0, -2 * M_PI * f / sr), z2 = z * z;
		return 20 * std::log10(std::abs((b0 + b1 * z + b2 * z2) / (1.0 + a1 * z + a2 * z2)));
	}
};
// a small dense solve, partial pivoting
inline void solve(double A[EQ_N][EQ_N], double b[EQ_N], double x[EQ_N]) {
	double M[EQ_N][EQ_N + 1];
	for (int i = 0; i < EQ_N; i++) { for (int j = 0; j < EQ_N; j++) M[i][j] = A[i][j]; M[i][EQ_N] = b[i]; }
	for (int c = 0; c < EQ_N; c++) {
		int p = c; for (int r = c + 1; r < EQ_N; r++) if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
		for (int j = 0; j <= EQ_N; j++) std::swap(M[c][j], M[p][j]);
		for (int r = 0; r < EQ_N; r++) if (r != c) { double f = M[r][c] / M[c][c]; for (int j = c; j <= EQ_N; j++) M[r][j] -= f * M[c][j]; }
	}
	for (int i = 0; i < EQ_N; i++) x[i] = M[i][EQ_N] / M[i][i];
}

// The tail is ONE network for the life of the module: a retrace (a source
// moved, SIZE turned) retargets it rather than replacing it, so a 45 s tail is
// never cut. New decay filters apply at once (a change of a fraction of a dB a
// pass); new line lengths are GLIDED to, LEN_GLIDE samples per sample, which
// is a 9-cent Doppler on the tail while the room changes size.
// the diffusers' lengths, at namespace scope: a static constexpr ARRAY in a
// class needs an out-of-class definition under C++11 once it is indexed, and
// MinGW's linker (unlike the macOS and Linux ones) refused the plugin without it
static const double TAIL_AP_SEC[6] = {0.0047, 0.0071, 0.0113, 0.0167, 0.0239, 0.0311};

struct Tail {
	struct Line { std::vector<float> buf; int w = 0, mask = 0; double len = 0, lenTarget = 0, spread = 1;
	              double depthFrac = 0, modDepth = 0, modPhase = 0, modInc = 0;
	              double g0 = 1; BQ sh[EQ_N - 1]; };
	Line line[NL];
	float out[2] = {0, 0};
	struct AP { std::vector<float> b; int w = 0, mask = 0, len = 1; float g = 0.6f; };
	// six allpasses ahead of the feed: with four, the onset's worst crest ran 4.5
	// to 7.1 depending on the seed; six keep every seed tried under 5.1
	static const int AP_N = 6;
	AP diff[AP_N];
	// How the input reaches the lines. Fed the same signal at once, every line
	// sent back a bare copy of a click one trip later: sixteen clicks between 40
	// and 80 ms after the tail started, crest factor 16, heard as an echo. Each
	// line now receives its own sparse random burst of the input (velvet noise,
	// VEL_N signed pulses over VEL_SEC), so its first return is already noise.
	static const int VEL_N = 24;
	static constexpr double VEL_SEC = 0.05;
	static constexpr double MAX_BASE = 0.08, MAX_MOD = 2.0, LEN_GLIDE = 0.005;
	struct VTap { int d; int line; float g; };
	std::vector<VTap> vel;
	std::vector<float> vbuf; int vw = 0, vmask = 0;
	bool freeze = false;                                     // lossless loop, for FREEZE
	double sr = 48000;

	// (the class constants are copied into locals before std::min sees them: the
	// plugin builds as C++11, where binding one to a reference needs a definition)
	static double baseFor(double mfp) { const double mx = MAX_BASE; return std::max(0.03, std::min(mx, mfp / C_SOUND * 2.0)); }

	void setup(const Program& P, double modAmt, uint64_t seed) {
		sr = P.sr;
		Rng r(seed);
		// every line sized for the largest room and the deepest modulation, so a
		// retarget never allocates
		int need = (int)((MAX_BASE * 2.0 + 2 * MAX_MOD * 0.001) * sr) + 16, size = 1;
		while (size < need) size <<= 1;
		double base = baseFor(P.mfp);
		for (int i = 0; i < NL; i++) {
			Line& L = line[i];
			L.spread = std::pow(2.0, (i + 0.5 * r.u()) / NL);   // 1x to 2x, spread geometrically
			L.lenTarget = L.len = base * L.spread * sr;
			L.depthFrac = 0.0004 + 0.0006 * r.u();
			L.modInc = 2 * M_PI * (0.05 + 0.25 * r.u()) / sr;
			L.modPhase = 2 * M_PI * r.u();
			L.buf.assign(size, 0.f); L.mask = size - 1; L.w = 0;
		}
		setMod(modAmt);
		// one pulse in each of VEL_N equal segments, per line, random place and
		// sign; the energy each line receives is what a plain 0.25 feed gave it
		vel.clear();
		int vlen = (int)(VEL_SEC * sr);
		float g = 1.f / std::sqrt((float)(VEL_N * NL));
		for (int i = 0; i < NL; i++)
			for (int k = 0; k < VEL_N; k++)
				vel.push_back({(int)((k + r.u()) * vlen / VEL_N), i, r.u() < 0.5 ? -g : g});
		std::sort(vel.begin(), vel.end(), [](const VTap& a, const VTap& b) { return a.d < b.d; });
		int vs = 1; while (vs < vlen + 2) vs <<= 1;
		vbuf.assign(vs, 0.f); vmask = vs - 1; vw = 0;
		setDecay(P);
		for (int k = 0; k < AP_N; k++) {
			int need = (int)(TAIL_AP_SEC[k] * sr) + 2, size = 1;
			while (size < need) size <<= 1;
			diff[k].b.assign(size, 0.f); diff[k].mask = size - 1; diff[k].w = 0; diff[k].g = 0.62f;
		}
		setDiffusion(P);
	}
	void setMod(double amt) {
		const double mx = MAX_MOD;
		amt = std::max(0.0, std::min(mx, amt));
		for (int i = 0; i < NL; i++) line[i].modDepth = amt * line[i].depthFrac * sr;
	}
	// a new room: new lengths to glide to, and the decay for them
	void retarget(const Program& P) {
		double base = baseFor(P.mfp);
		for (int i = 0; i < NL; i++) line[i].lenTarget = base * line[i].spread * sr;
		setDecay(P);
		setDiffusion(P);
	}
	// The diffusers ring on their own (the longest, 31 ms at g = 0.62, for
	// 0.45 s), and the tail can decay no faster than its own input does: the
	// carpeted mosque, 0.55 s at 2 kHz, rendered 38% long. So they shorten in a
	// room whose fastest band (to 4 kHz) is shorter than five times their ring;
	// the cistern and every larger room keep them as they were.
	void setDiffusion(const Program& P) {
		double fastest = 1e9;
		for (int b = 0; b < NB; b++) fastest = std::min(fastest, P.t60[b]);
		double ring = 3.0 * TAIL_AP_SEC[AP_N - 1] / -std::log10(0.62);
		double k = std::max(0.15, std::min(1.0, 0.2 * fastest / ring));
		for (int i = 0; i < AP_N; i++) diff[i].len = std::max(1, std::min(diff[i].mask, (int)(TAIL_AP_SEC[i] * k * sr)));
	}
	void setDecay(const Program& P) {
		double T[EQ_N] = {P.t60[0], P.t60[1], P.t60[2], P.t60[3], P.t60[4], P.t60[5], P.t60x8k};
		// the basis: a flat dB, and each shelf at 1 dB, read at the octave centres
		double A[EQ_N][EQ_N];
		BQ unit[EQ_N - 1];
		for (int k = 0; k < EQ_N - 1; k++) unit[k].highShelf(SHELF_HZ[k], 1.0, sr);
		for (int j = 0; j < EQ_N; j++) { A[j][0] = 1; for (int k = 0; k < EQ_N - 1; k++) A[j][k + 1] = unit[k].dbAt(EQ_HZ[j], sr); }
		for (int i = 0; i < NL; i++) {
			Line& L = line[i];
			double n = L.lenTarget + L.modDepth;                      // the mean trip, modulation included
			double tgt[EQ_N], c[EQ_N] = {}, res[EQ_N], dc[EQ_N];
			for (int j = 0; j < EQ_N; j++) tgt[j] = -60.0 * n / (T[j] * sr);
			for (int j = 0; j < EQ_N; j++) res[j] = tgt[j];
			BQ sh[EQ_N - 1];
			for (int it = 0; it < 4; it++) {                          // refine against the real response
				solve(A, res, dc);
				for (int k = 0; k < EQ_N; k++) c[k] += dc[k];
				for (int k = 0; k < EQ_N - 1; k++) sh[k].highShelf(SHELF_HZ[k], c[k + 1], sr);
				for (int j = 0; j < EQ_N; j++) {
					double got = c[0]; for (int k = 0; k < EQ_N - 1; k++) got += sh[k].dbAt(EQ_HZ[j], sr);
					res[j] = tgt[j] - got;
				}
			}
			// coefficients only: the filters' state carries over, so a running tail does not click
			for (int k = 0; k < EQ_N - 1; k++) {
				L.sh[k].b0 = sh[k].b0; L.sh[k].b1 = sh[k].b1; L.sh[k].b2 = sh[k].b2;
				L.sh[k].a1 = sh[k].a1; L.sh[k].a2 = sh[k].a2;
			}
			L.g0 = std::pow(10.0, c[0] / 20);
		}
	}
	static inline float cubic(const std::vector<float>& b, int mask, double pos) {
		int i = (int)std::floor(pos); float f = (float)(pos - i);
		float y0 = b[(i - 1) & mask], y1 = b[i & mask], y2 = b[(i + 1) & mask], y3 = b[(i + 2) & mask];
		float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
		return ((c3 * f + c2) * f + c1) * f + y1;
	}
	void process(float in) {
		// diffusion ahead of the network
		for (int k = 0; k < AP_N; k++) {
			AP& a = diff[k]; float d = a.b[(a.w - a.len) & a.mask];
			float v = in + a.g * d; float y = d - a.g * v;
			a.b[a.w] = v; a.w = (a.w + 1) & a.mask; in = y;
		}
		vbuf[vw] = in;
		float feed[NL] = {};
		for (const VTap& t : vel) feed[t.line] += t.g * vbuf[(vw - t.d) & vmask];
		vw = (vw + 1) & vmask;
		float o[NL];
		for (int i = 0; i < NL; i++) {
			Line& L = line[i];
			double dl = L.lenTarget - L.len;
			const double gl = LEN_GLIDE;
			if (dl != 0) L.len += std::max(-gl, std::min(gl, dl));
			L.modPhase += L.modInc; if (L.modPhase > 2 * M_PI) L.modPhase -= 2 * M_PI;
			double rd = L.w - L.len - L.modDepth * (1.0 + std::sin(L.modPhase));
			double x = cubic(L.buf, L.mask, rd);
			if (freeze) { o[i] = (float)x; continue; }
			double y = L.g0 * x;
			for (int k = 0; k < EQ_N - 1; k++) y = L.sh[k].run(y);
			o[i] = (float)y;
		}
		// Hadamard 16, orthonormal (fast transform)
		float h[NL]; for (int i = 0; i < NL; i++) h[i] = o[i];
		for (int len = 1; len < NL; len <<= 1)
			for (int i = 0; i < NL; i += len << 1)
				for (int j = i; j < i + len; j++) { float a = h[j], b = h[j + len]; h[j] = a + b; h[j + len] = a - b; }
		for (int i = 0; i < NL; i++) {
			Line& L = line[i];
			L.buf[L.w] = h[i] * 0.25f + feed[i];
			L.w = (L.w + 1) & L.mask;
		}
		float l = 0, r = 0;
		for (int i = 0; i < NL; i++) { l += (i & 1) ? -o[i] : o[i]; r += (i & 2) ? -o[i] : o[i]; }
		out[0] = l * 0.25f; out[1] = r * 0.25f;
	}
};

// The handover. An FDN does not start at full level: its energy arrives over
// its line lengths and its diffusers, and a tail switched on at the mixing time
// left a 7 dB hole after it. So the network is run offline on an impulse, its
// build-up measured (decay removed), and then (1) it is started early enough to
// be at 90% by the mixing time, (2) the early taps where it is building are
// faded by the complement, so the two energies sum to a constant, and (3) its
// gain is set so its level at the join is the echogram's. Last, the wet level:
// the whole response's energy (early + tail) set to one, so a steady input
// comes out of the wet path at the level it went in, whatever the decay.
inline void calibrate(Program& P, double modAmt, uint64_t seed) {
	Tail t; t.setup(P, modAmt, seed);
	double tm = 0.5 * (P.t60[2] + P.t60[3]);
	const double blk = 0.0025; int bs = (int)(blk * P.sr), nb = (int)(0.8 / blk);
	std::vector<double> env(nb, 0.0);
	for (int i = 0; i < nb * bs; i++) {
		t.process(i == 0 ? 1.f : 0.f);
		env[i / bs] += 0.5 * (t.out[0] * t.out[0] + t.out[1] * t.out[1]);
	}
	for (int k = 0; k < nb; k++) env[k] *= std::pow(10.0, 6.0 * (k + 0.5) * blk / tm) / blk;   // per second, decay undone
	std::vector<double> sm(nb, 0.0);
	for (int k = 0; k < nb; k++) { int c = 0; for (int j = std::max(0, k - 2); j <= std::min(nb - 1, k + 1); j++) { sm[k] += env[j]; c++; } sm[k] /= c; }
	double full = 0; int c = 0;
	for (int k = (int)(0.4 / blk); k < nb; k++) { full += sm[k]; c++; }
	full /= c;
	std::vector<double> b(nb);
	double run = 0;
	for (int k = 0; k < nb; k++) { run = std::max(run, std::min(1.0, sm[k] / full)); b[k] = run; }
	int k90 = 0; while (k90 < nb - 1 && b[k90] < 0.9) k90++;
	double tB = (k90 + 0.5) * blk;
	P.tailDelay = std::max(1, (int)((P.tMix - tB) * P.sr));
	double tAtJoin = P.tMix - P.tailDelay / P.sr;                 // the tail's own age at the join
	P.tailGain = std::sqrt(P.eEnd / std::max(full * std::pow(10.0, -6.0 * tAtJoin / tm), 1e-30));
	std::vector<Tap> kept;
	double eEarly = 0;
	for (Tap tp : P.taps) {
		double age = (tp.delay - P.tailDelay) / P.sr;
		if (age > 0) {
			int k = (int)(age / blk);
			double g = std::sqrt(std::max(0.0, 1.0 - (k < nb ? b[k] : 1.0)));
			if (g <= 0) continue;
			for (int e = 0; e < 2; e++) for (int r = 0; r < RB; r++) tp.g[e][r] *= (float)g;
		}
		eEarly += 0.5 * (tp.g[0][1] * tp.g[0][1] + tp.g[1][1] * tp.g[1][1]);
		kept.push_back(tp);
	}
	P.taps.swap(kept);
	// the tail's energy from its start: full * G^2 per second, decaying at tm
	double eTail = full * P.tailGain * P.tailGain * tm / 13.8155;
	P.wetNorm = 1.0 / std::sqrt(std::max(eEarly * P.nsrc + eTail, 1e-30));
}

// ── the whole reverb, as the audio thread runs it ──────────────────────────
// Two programs at most: the one in force and the one it is crossfading out of
// (the early taps and the tail's feed, over XF_SEC). The tail itself is never
// doubled; it is retargeted. The output is the WET signal at its calibrated
// level (wetNorm applied). Nothing here allocates after setProgram().
struct Reverb {
	static constexpr double XF_SEC = 0.04, HIST_SEC = 1.2, FREEZE_SEC = 0.01;   // HIST_SEC > the longest early window (0.9 s)
	const Program* cur = nullptr;
	const Program* prev = nullptr;
	float fade = 1.f, fadeInc = 0.f;
	Tail tail;
	// band-split input history, per input, the three bands of a sample side by
	// side (and a pad), so a tap is one cache line rather than three
	std::vector<float> hist[NSRC];
	std::vector<float> tbuf;                                // the tail's feed, delayed
	int w = 0, hmask = 0;
	float lp1[NSRC] = {}, lp2[NSRC] = {};
	float a1 = 0, a2 = 0;
	float inGain = 1.f, inStep = 0.f;
	bool freeze = false;
	double sr = 0;

	void setProgram(const Program* p, double modAmt, uint64_t seed) {
		cur = p; prev = nullptr; fade = 1.f;
		sr = p->sr;
		int size = 1; while (size < (int)(HIST_SEC * sr)) size <<= 1;
		hmask = size - 1;
		for (int k = 0; k < NSRC; k++) hist[k].assign((size_t)size * 4, 0.f);
		tbuf.assign(size, 0.f); w = 0;
		for (int k = 0; k < NSRC; k++) lp1[k] = lp2[k] = 0.f;
		a1 = 1.f - (float)std::exp(-2 * M_PI * 300.0 / sr);
		a2 = 1.f - (float)std::exp(-2 * M_PI * 3000.0 / sr);
		fadeInc = (float)(1.0 / (XF_SEC * sr));
		inStep = (float)(1.0 / (FREEZE_SEC * sr));
		tail.setup(*p, modAmt, seed);
	}
	bool fading() const { return prev != nullptr; }
	// take a new program (same sample rate); the one being crossfaded from
	// must have finished (`fading()` false)
	void adopt(const Program* p) {
		prev = cur; cur = p; fade = 0.f;
		tail.retarget(*p);
	}
	// once a crossfade ends, the program it left, for the caller to free
	const Program* finished() {
		if (prev && fade >= 1.f) { const Program* q = prev; prev = nullptr; return q; }
		return nullptr;
	}
	float wetNorm() const { return prev ? (float)(prev->wetNorm + (cur->wetNorm - prev->wetNorm) * fade) : (float)cur->wetNorm; }
	void setMod(double amt) { tail.setMod(amt); }

	inline void early(const Program* P, float g, float& l, float& r) {
		const float* h[NSRC] = {hist[0].data(), hist[1].data()};
		float sl = 0.f, sr2 = 0.f;
		for (const Tap& t : P->taps) {
			const float* b = h[t.in] + (size_t)((w - t.delay) & hmask) * 4;
			sl  += t.g[0][0] * b[0] + t.g[0][1] * b[1] + t.g[0][2] * b[2];
			sr2 += t.g[1][0] * b[0] + t.g[1][1] * b[1] + t.g[1][2] * b[2];
		}
		l += g * sl; r += g * sr2;
	}
	void process(float inL, float inR, float& outL, float& outR) {
		// FREEZE: the loop goes lossless and the input is shut out of it
		tail.freeze = freeze;
		if (freeze) inGain = std::max(0.f, inGain - inStep); else inGain = std::min(1.f, inGain + inStep);
		// The program's gains go on what ENTERS, not on what leaves: a retrace
		// changes the calibration, and applied at the output it turned up the
		// tail already ringing (a 3.5 dB step when the room shrank). On the way
		// in, sound already in the room keeps the level it entered at.
		float g = inGain * wetNorm();
		float in[NSRC] = {inL * g, inR * g};
		for (int k = 0; k < NSRC; k++) {
			// the input split into the render bands, complementary
			lp1[k] += (in[k] - lp1[k]) * a1; float rest = in[k] - lp1[k];
			lp2[k] += (rest - lp2[k]) * a2; float hi = rest - lp2[k], mid = rest - hi;
			float* hw = hist[k].data() + (size_t)w * 4;
			hw[0] = lp1[k]; hw[1] = mid; hw[2] = hi;
		}
		tbuf[w] = 0.5f * (in[0] + in[1]);
		float l = 0, r = 0;
		float td;
		if (prev) {
			// two traces' taps are unrelated, so they cross at equal power; a
			// decay change keeps the same taps, which cross linearly
			float gc = fade, gp = 1.f - fade;
			if (prev->traceId != cur->traceId) { gc = std::sin(fade * 1.5707963f); gp = std::cos(fade * 1.5707963f); }
			early(cur, gc, l, r); early(prev, gp, l, r);
			td = fade * tbuf[(w - cur->tailDelay) & hmask] + (1.f - fade) * tbuf[(w - prev->tailDelay) & hmask];
		} else {
			early(cur, 1.f, l, r);
			td = tbuf[(w - cur->tailDelay) & hmask];
		}
		w = (w + 1) & hmask;
		// the tail, fed late so it takes over at the mixing time
		float tg = prev ? (float)(prev->tailGain + (cur->tailGain - prev->tailGain) * fade) : (float)cur->tailGain;
		tail.process(td * tg);
		outL = l + tail.out[0];
		outR = r + tail.out[1];
		if (prev && fade < 1.f) fade = std::min(1.f, fade + fadeInc);
	}
};

}  // namespace cistern

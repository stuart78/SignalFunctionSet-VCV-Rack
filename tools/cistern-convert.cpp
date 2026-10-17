// Turns a photogrammetry scan (glTF) into a space Cistern can trace.
//
//   clang++ -std=c++17 -O2 -I ../Rack-SDK/dep/include tools/cistern-convert.cpp -o /tmp/cistern-convert \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack        (for jansson)
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" \
//     /tmp/cistern-convert <dir with scene.gltf> <out.bin> [--h 0.3] [--enclose 0.7] [--close 0.4]
//     [--seed x y z] [--scale k] [--cut x z r y] [--preview prefix]
//
// --cut removes scan triangles inside the vertical cylinder (x, z, r) above
// height y (metres, after --scale): photogrammetry fills an opening with
// whatever the camera saw through it, and the Pantheon's oculus came back
// capped.
//
// A scan is a skin of millions of tiny triangles, with holes where the scanner
// could not see, clutter, sometimes the street outside, and openings that are
// really there (a doorway, an oculus, an open arcade). What the engine needs is
// the AIR: the room's volume, and a closed surface around it that knows which
// parts of itself are walls and which are openings. So:
//
//   1. the scan is rasterised into voxels of side h (the WALLS)
//   2. the walls are thickened by `close` metres, which seals scan holes
//      smaller than twice that
//   3. every free voxel is scored for how ENCLOSED it is: of 26 directions, how
//      many reach a wall before leaving the scan. A real opening (the
//      Pantheon's 8 m oculus) cannot be sealed by thickening, but a point under
//      it is still enclosed in 25 of 26 directions, where a point outside the
//      building is enclosed in a third of them
//   4. the air is the enclosed free space connected to the deepest point, grown
//      back out to the scanned surface
//   5. its boundary is extracted (surface nets) and smoothed (Taubin), each
//      face marked floor / wall / ceiling by its normal, or OPENING where the
//      air meets unenclosed space rather than a wall. An opening absorbs
//      everything, as the sky through an oculus does
//
// Output (little endian): "CSP1", u32 nverts, u32 ntris, f32 lo[3], hi[3],
// u16 verts[3n] (quantised over lo..hi), u32 tris[3t], u8 class[t] (0 floor,
// 1 wall, 2 ceiling, 3 opening), f32 volume, f32 src[3], f32 lis[3], f32 glo[3],
// f32 gcell, u32 gx, gy, gz, u8 inside-bits. Faces point INTO the air.
#include <jansson.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct V { double x = 0, y = 0, z = 0; };
static V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V operator*(V a, double k) { return {a.x * k, a.y * k, a.z * k}; }
static double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static double len(V a) { return std::sqrt(dot(a, a)); }

// ── glTF ────────────────────────────────────────────────────────────────────
typedef std::array<double, 16> M4;   // column-major, as glTF stores it
static M4 ident() { M4 m{}; m[0] = m[5] = m[10] = m[15] = 1; return m; }
static M4 mul(const M4& a, const M4& b) {
	M4 r{};
	for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
		double s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
		r[c * 4 + rr] = s;
	}
	return r;
}
static V xform(const M4& m, V p) {
	return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
	        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}
static double num(json_t* j) { return json_number_value(j); }
static M4 nodeMatrix(json_t* n) {
	M4 m = ident();
	if (json_t* mm = json_object_get(n, "matrix")) { for (int i = 0; i < 16; i++) m[i] = num(json_array_get(mm, i)); return m; }
	M4 T = ident(), R = ident(), S = ident();
	if (json_t* t = json_object_get(n, "translation")) { T[12] = num(json_array_get(t, 0)); T[13] = num(json_array_get(t, 1)); T[14] = num(json_array_get(t, 2)); }
	if (json_t* q = json_object_get(n, "rotation")) {
		double x = num(json_array_get(q, 0)), y = num(json_array_get(q, 1)), z = num(json_array_get(q, 2)), w = num(json_array_get(q, 3));
		R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y + z * w); R[2] = 2 * (x * z - y * w);
		R[4] = 2 * (x * y - z * w); R[5] = 1 - 2 * (x * x + z * z); R[6] = 2 * (y * z + x * w);
		R[8] = 2 * (x * z + y * w); R[9] = 2 * (y * z - x * w); R[10] = 1 - 2 * (x * x + y * y);
	}
	if (json_t* s = json_object_get(n, "scale")) { S[0] = num(json_array_get(s, 0)); S[5] = num(json_array_get(s, 1)); S[10] = num(json_array_get(s, 2)); }
	return mul(T, mul(R, S));
}
struct Mesh { std::vector<V> p; std::vector<uint32_t> t; };
static bool loadGltf(const std::string& dir, Mesh& out) {
	json_error_t err;
	json_t* g = json_load_file((dir + "/scene.gltf").c_str(), 0, &err);
	if (!g) { fprintf(stderr, "gltf: %s\n", err.text); return false; }
	json_t* bufs = json_object_get(g, "buffers");
	std::string uri = json_string_value(json_object_get(json_array_get(bufs, 0), "uri"));
	FILE* f = fopen((dir + "/" + uri).c_str(), "rb");
	if (!f) { fprintf(stderr, "no %s\n", uri.c_str()); return false; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> bin(sz); if (fread(bin.data(), 1, sz, f) != (size_t)sz) return false; fclose(f);
	json_t* views = json_object_get(g, "bufferViews");
	json_t* accs = json_object_get(g, "accessors");
	json_t* meshes = json_object_get(g, "meshes");
	json_t* nodes = json_object_get(g, "nodes");
	auto accessor = [&](int i, size_t& count, size_t& stride) -> const uint8_t* {
		json_t* a = json_array_get(accs, i);
		json_t* bv = json_array_get(views, json_integer_value(json_object_get(a, "bufferView")));
		size_t off = json_integer_value(json_object_get(bv, "byteOffset")) + json_integer_value(json_object_get(a, "byteOffset"));
		count = json_integer_value(json_object_get(a, "count"));
		stride = json_integer_value(json_object_get(bv, "byteStride"));
		return bin.data() + off;
	};
	std::function<void(int, M4)> walk = [&](int ni, M4 parent) {
		json_t* n = json_array_get(nodes, ni);
		M4 m = mul(parent, nodeMatrix(n));
		if (json_t* mi = json_object_get(n, "mesh")) {
			json_t* prims = json_object_get(json_array_get(meshes, json_integer_value(mi)), "primitives");
			for (size_t k = 0; k < json_array_size(prims); k++) {
				json_t* pr = json_array_get(prims, k);
				size_t cnt, st;
				const uint8_t* pp = accessor(json_integer_value(json_object_get(json_object_get(pr, "attributes"), "POSITION")), cnt, st);
				if (!st) st = 12;
				uint32_t base = (uint32_t)out.p.size();
				for (size_t v = 0; v < cnt; v++) {
					float xyz[3]; memcpy(xyz, pp + v * st, 12);
					out.p.push_back(xform(m, {xyz[0], xyz[1], xyz[2]}));
				}
				size_t ic, is;
				const uint8_t* ip = accessor(json_integer_value(json_object_get(pr, "indices")), ic, is);
				for (size_t v = 0; v < ic; v++) { uint32_t x; memcpy(&x, ip + v * 4, 4); out.t.push_back(base + x); }
			}
		}
		if (json_t* ch = json_object_get(n, "children"))
			for (size_t k = 0; k < json_array_size(ch); k++) walk((int)json_integer_value(json_array_get(ch, k)), m);
	};
	json_t* sc = json_array_get(json_object_get(g, "scenes"), json_integer_value(json_object_get(g, "scene")));
	json_t* roots = json_object_get(sc, "nodes");
	for (size_t k = 0; k < json_array_size(roots); k++) walk((int)json_integer_value(json_array_get(roots, k)), ident());
	json_decref(g);
	return true;
}

// ── voxels ──────────────────────────────────────────────────────────────────
struct Grid {
	int nx = 0, ny = 0, nz = 0; V o; double h = 1;
	size_t n() const { return (size_t)nx * ny * nz; }
	size_t id(int x, int y, int z) const { return (size_t)x + (size_t)nx * ((size_t)y + (size_t)ny * z); }
	bool in(int x, int y, int z) const { return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz; }
	V centre(int x, int y, int z) const { return {o.x + (x + 0.5) * h, o.y + (y + 0.5) * h, o.z + (z + 0.5) * h}; }
};
// Chebyshev dilation by r, separably: a box max along each axis in turn
static void dilate(const Grid& G, std::vector<uint8_t>& v, int r) {
	if (r <= 0) return;
	std::vector<uint8_t> t(v.size());
	auto pass = [&](int axis) {
		int L = axis == 0 ? G.nx : axis == 1 ? G.ny : G.nz;
		std::vector<int> run(L);
		for (int a = 0; a < (axis == 0 ? G.ny : G.nx); a++)
			for (int b = 0; b < (axis == 2 ? G.ny : G.nz); b++) {
				auto at = [&](int i) -> size_t {
					if (axis == 0) return G.id(i, a, b);
					if (axis == 1) return G.id(a, i, b);
					return G.id(a, b, i);
				};
				// prefix count of set cells, then any-in-window
				int c = 0; std::vector<int> pre(L + 1, 0);
				for (int i = 0; i < L; i++) { c += v[at(i)] ? 1 : 0; pre[i + 1] = c; }
				for (int i = 0; i < L; i++) {
					int lo = std::max(0, i - r), hi = std::min(L, i + r + 1);
					t[at(i)] = pre[hi] - pre[lo] > 0;
				}
			}
		v.swap(t);
	};
	pass(0); pass(1); pass(2);
}

int main(int argc, char** argv) {
	if (argc < 3) { fprintf(stderr, "usage: %s <scan dir> <out.bin> [options]\n", argv[0]); return 2; }
	std::string dir = argv[1], outPath = argv[2], preview;
	double h = -1, enclose = 0.7, closeM = 0.4, scale = 1; bool seedSet = false; V seedW;
	std::vector<std::array<double, 4>> cuts;
	for (int i = 3; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--h") h = atof(argv[++i]);
		else if (a == "--enclose") enclose = atof(argv[++i]);
		else if (a == "--close") closeM = atof(argv[++i]);
		else if (a == "--scale") scale = atof(argv[++i]);
		else if (a == "--preview") preview = argv[++i];
		else if (a == "--cut") { std::array<double, 4> c; for (int k2 = 0; k2 < 4; k2++) c[k2] = atof(argv[++i]); cuts.push_back(c); }
		else if (a == "--seed") { seedW.x = atof(argv[++i]); seedW.y = atof(argv[++i]); seedW.z = atof(argv[++i]); seedSet = true; }
	}
	Mesh M;
	if (!loadGltf(dir, M)) return 1;
	V lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
	for (V& p : M.p) {
		p = p * scale;
		lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
		hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
	}
	if (!cuts.empty()) {
		std::vector<uint32_t> keep; size_t gone = 0;
		for (size_t t = 0; t + 2 < M.t.size(); t += 3) {
			V c = (M.p[M.t[t]] + M.p[M.t[t + 1]] + M.p[M.t[t + 2]]) * (1.0 / 3);
			bool cut = false;
			for (auto& k2 : cuts) if (c.y > k2[3] && std::hypot(c.x - k2[0], c.z - k2[1]) < k2[2]) cut = true;
			if (cut) { gone++; continue; }
			keep.insert(keep.end(), &M.t[t], &M.t[t] + 3);
		}
		M.t.swap(keep);
		printf("  cut %zu triangles\n", gone);
	}
	V ext = hi - lo;
	if (h <= 0) h = std::max(0.12, std::min(0.8, std::cbrt(ext.x * ext.y * ext.z) / 60));
	int k = std::max(1, (int)std::lround(closeM / h));
	printf("%s: %zu triangles, %.1f x %.1f x %.1f m, voxel %.2f m, closing %d\n", dir.c_str(), M.t.size() / 3, ext.x, ext.y, ext.z, h, k);

	Grid G; G.h = h;
	int pad = k + 2;
	G.nx = (int)std::ceil(ext.x / h) + 2 * pad; G.ny = (int)std::ceil(ext.y / h) + 2 * pad; G.nz = (int)std::ceil(ext.z / h) + 2 * pad;
	G.o = lo - V{pad * h, pad * h, pad * h};
	printf("  grid %d x %d x %d, bounds (%.2f %.2f %.2f) .. (%.2f %.2f %.2f)\n", G.nx, G.ny, G.nz, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
	std::vector<uint8_t> S(G.n(), 0);
	auto mark = [&](V p) {
		int x = (int)((p.x - G.o.x) / h), y = (int)((p.y - G.o.y) / h), z = (int)((p.z - G.o.z) / h);
		if (G.in(x, y, z)) S[G.id(x, y, z)] = 1;
	};
	for (size_t t = 0; t + 2 < M.t.size(); t += 3) {
		V a = M.p[M.t[t]], b = M.p[M.t[t + 1]], c = M.p[M.t[t + 2]];
		double e = std::max(len(b - a), std::max(len(c - a), len(c - b)));
		int n = std::max(1, (int)std::ceil(e / (0.5 * h)));
		for (int i = 0; i <= n; i++) for (int j = 0; i + j <= n; j++) {
			double u = (double)i / n, w = (double)j / n;
			mark(a + (b - a) * u + (c - a) * w);
		}
	}
	std::vector<uint8_t> D = S;
	dilate(G, D, k);

	// enclosure, on a coarse grid of about half a metre
	int f = std::max(1, (int)std::lround(0.5 / h));
	Grid C; C.h = h * f; C.o = G.o; C.nx = (G.nx + f - 1) / f; C.ny = (G.ny + f - 1) / f; C.nz = (G.nz + f - 1) / f;
	std::vector<uint8_t> CS(C.n(), 0);
	for (int z = 0; z < G.nz; z++) for (int y = 0; y < G.ny; y++) for (int x = 0; x < G.nx; x++)
		if (D[G.id(x, y, z)]) CS[C.id(x / f, y / f, z / f)] = 1;
	std::vector<V> dirs;
	for (int a = -1; a <= 1; a++) for (int b = -1; b <= 1; b++) for (int c = -1; c <= 1; c++)
		if (a || b || c) { V d{(double)a, (double)b, (double)c}; dirs.push_back(d * (1.0 / len(d))); }
	// Marched through the FULL-resolution walls, sampled once per block: a
	// coarse grid where a cell is solid if any of it is swallowed the free space
	// under a vault that springs low (the Salt Tower kept 36 of its ~250 m3).
	std::vector<float> enc(C.n(), 0.f);
	for (int bz = 0; bz < C.nz; bz++) for (int by = 0; by < C.ny; by++) for (int bx = 0; bx < C.nx; bx++) {
		// the free voxel nearest the block's centre
		int sx = -1, sy = 0, sz = 0; double bd = 1e9;
		for (int dz = 0; dz < f; dz++) for (int dy = 0; dy < f; dy++) for (int dx = 0; dx < f; dx++) {
			int X = bx * f + dx, Y = by * f + dy, Z = bz * f + dz;
			if (!G.in(X, Y, Z) || D[G.id(X, Y, Z)]) continue;
			double d = std::fabs(dx - (f - 1) * 0.5) + std::fabs(dy - (f - 1) * 0.5) + std::fabs(dz - (f - 1) * 0.5);
			if (d < bd) { bd = d; sx = X; sy = Y; sz = Z; }
		}
		if (sx < 0) continue;
		int hits = 0;
		for (const V& d : dirs) {
			double px = sx + 0.5, py = sy + 0.5, pz = sz + 0.5;
			for (;;) {
				px += d.x * 0.5; py += d.y * 0.5; pz += d.z * 0.5;
				if (px < 0 || py < 0 || pz < 0) break;
				int ix = (int)px, iy = (int)py, iz = (int)pz;
				if (!G.in(ix, iy, iz)) break;
				if (D[G.id(ix, iy, iz)]) { hits++; break; }
			}
		}
		enc[C.id(bx, by, bz)] = hits / 26.f;
	}
	auto cand = [&](size_t i, int x, int y, int z) { return !D[i] && enc[C.id(x / f, y / f, z / f)] >= enclose; };
	if (getenv("CC_COL")) {
		double wx, wz; sscanf(getenv("CC_COL"), "%lf,%lf", &wx, &wz);
		int x = (int)((wx - G.o.x) / h), z = (int)((wz - G.o.z) / h);
		printf("  column %d %d, S then D, bottom to top:\n  ", x, z);
		for (int y = 0; y < G.ny; y++) printf("%c", S[G.id(x, y, z)] ? '#' : D[G.id(x, y, z)] ? '+' : '.');
		printf("\n");
		size_t ns = 0, nd = 0; for (size_t i = 0; i < G.n(); i++) { ns += S[i]; nd += D[i]; }
		printf("  S %.1f%% of voxels, D %.1f%%\n", 100.0 * ns / G.n(), 100.0 * nd / G.n());
	}
	if (getenv("CC_PROBE")) {
		double wx, wy, wz; sscanf(getenv("CC_PROBE"), "%lf,%lf,%lf", &wx, &wy, &wz);
		int x = (int)((wx - C.o.x) / C.h), y = (int)((wy - C.o.y) / C.h), z = (int)((wz - C.o.z) / C.h);
		printf("  probe cell %d %d %d solid %d enc %.2f\n", x, y, z, CS[C.id(x, y, z)], enc[C.id(x, y, z)]);
		for (const V& d : dirs) {
			double px = x + 0.5, py = y + 0.5, pz = z + 0.5; int steps = 0; bool hit = false;
			for (;;) { px += d.x * 0.5; py += d.y * 0.5; pz += d.z * 0.5; steps++;
				int ix = (int)px, iy = (int)py, iz = (int)pz;
				if (!C.in(ix, iy, iz)) break;
				if (CS[C.id(ix, iy, iz)]) { hit = true; break; } }
			printf("    dir %+.2f %+.2f %+.2f: %s after %.1f m\n", d.x, d.y, d.z, hit ? "wall" : "ESCAPES", steps * 0.5 * C.h);
		}
	}
	if (getenv("CC_DEBUG")) {
		// per height: how enclosed the free cells are, and which directions escape at the centre column
		for (int y = 0; y < C.ny; y++) {
			int n = 0; double m = 0; int hi = 0;
			for (int z = 0; z < C.nz; z++) for (int x = 0; x < C.nx; x++) { size_t i = C.id(x, y, z); if (CS[i]) continue; n++; m += enc[i]; hi += enc[i] >= enclose; }
			int solid = 0; for (int z = 0; z < C.nz; z++) for (int x = 0; x < C.nx; x++) solid += CS[C.id(x, y, z)];
			printf("  y %5.1f m: free %5d mean enclosure %.2f, %5d >= %.2f, solid cells %d\n", C.o.y + (y + 0.5) * C.h, n, n ? m / n : 0, hi, enclose, solid);
		}
	}

	// the seed: the free, enclosed voxel farthest from any wall
	std::vector<uint16_t> dist(G.n(), 0xffff);
	std::deque<size_t> q;
	for (int z = 0; z < G.nz; z++) for (int y = 0; y < G.ny; y++) for (int x = 0; x < G.nx; x++) {
		size_t i = G.id(x, y, z);
		if (!cand(i, x, y, z)) { dist[i] = 0; q.push_back(i); }
	}
	const int NX[6] = {1, -1, 0, 0, 0, 0}, NY[6] = {0, 0, 1, -1, 0, 0}, NZ[6] = {0, 0, 0, 0, 1, -1};
	while (!q.empty()) {
		size_t i = q.front(); q.pop_front();
		int x = i % G.nx, y = (i / G.nx) % G.ny, z = i / ((size_t)G.nx * G.ny);
		for (int d = 0; d < 6; d++) {
			int a = x + NX[d], b = y + NY[d], c = z + NZ[d];
			if (!G.in(a, b, c)) continue;
			size_t j = G.id(a, b, c);
			if (dist[j] > dist[i] + 1) { dist[j] = dist[i] + 1; q.push_back(j); }
		}
	}
	size_t seed = 0; int best = -1;
	if (seedSet) {
		seed = G.id((int)((seedW.x - G.o.x) / h), (int)((seedW.y - G.o.y) / h), (int)((seedW.z - G.o.z) / h));
	} else {
		for (size_t i = 0; i < G.n(); i++) if (dist[i] != 0xffff && dist[i] > best) { best = dist[i]; seed = i; }
	}
	// flood the air from it, then grow it back to the scanned surface
	std::vector<uint8_t> A(G.n(), 0);
	q.clear(); q.push_back(seed); A[seed] = 1;
	while (!q.empty()) {
		size_t i = q.front(); q.pop_front();
		int x = i % G.nx, y = (i / G.nx) % G.ny, z = i / ((size_t)G.nx * G.ny);
		for (int d = 0; d < 6; d++) {
			int a = x + NX[d], b = y + NY[d], c = z + NZ[d];
			if (!G.in(a, b, c)) continue;
			size_t j = G.id(a, b, c);
			if (!A[j] && cand(j, a, b, c)) { A[j] = 1; q.push_back(j); }
		}
	}
	for (int it = 0; it < k; it++) {
		std::vector<uint8_t> grow = A;
		for (int z = 1; z < G.nz - 1; z++) for (int y = 1; y < G.ny - 1; y++) for (int x = 1; x < G.nx - 1; x++) {
			size_t i = G.id(x, y, z);
			if (A[i] || S[i] || !D[i]) continue;
			bool nb = false;
			for (int d = 0; d < 6 && !nb; d++) nb = A[G.id(x + NX[d], y + NY[d], z + NZ[d])];
			if (nb) grow[i] = 1;
		}
		A.swap(grow);
	}
	size_t nair = 0; for (uint8_t a : A) nair += a;
	double volume = nair * h * h * h;
	printf("  air %.0f m3 (seed %s)\n", volume, seedSet ? "given" : "deepest point");

	// ── the boundary: surface nets on the air ───────────────────────────────
	// a vertex per cell (the cube between eight voxel centres) that straddles
	// the boundary, at the mean of its crossing edges' midpoints
	std::unordered_map<size_t, uint32_t> vid; vid.reserve(1 << 20);
	std::vector<V> vp;
	auto airAt = [&](int x, int y, int z) -> bool { return G.in(x, y, z) && A[G.id(x, y, z)]; };
	auto cellVert = [&](int x, int y, int z) -> uint32_t {
		size_t key = G.id(x, y, z);
		auto it = vid.find(key);
		if (it != vid.end()) return it->second;
		V s; int n = 0;
		static const int E[12][2][3] = {{{0,0,0},{1,0,0}},{{0,1,0},{1,1,0}},{{0,0,1},{1,0,1}},{{0,1,1},{1,1,1}},
		                                {{0,0,0},{0,1,0}},{{1,0,0},{1,1,0}},{{0,0,1},{0,1,1}},{{1,0,1},{1,1,1}},
		                                {{0,0,0},{0,0,1}},{{1,0,0},{1,0,1}},{{0,1,0},{0,1,1}},{{1,1,0},{1,1,1}}};
		for (auto& e : E) {
			bool a = airAt(x + e[0][0], y + e[0][1], z + e[0][2]), b = airAt(x + e[1][0], y + e[1][1], z + e[1][2]);
			if (a != b) {
				s = s + V{x + 0.5 * (e[0][0] + e[1][0]), y + 0.5 * (e[0][1] + e[1][1]), z + 0.5 * (e[0][2] + e[1][2])};
				n++;
			}
		}
		s = s * (1.0 / std::max(1, n));
		uint32_t id = (uint32_t)vp.size();
		vp.push_back({G.o.x + (s.x + 0.5) * h, G.o.y + (s.y + 0.5) * h, G.o.z + (s.z + 0.5) * h});
		vid[key] = id;
		return id;
	};
	struct Quad { uint32_t v[4]; V want; bool open; };
	std::vector<Quad> quads;
	for (int z = 1; z < G.nz - 1; z++) for (int y = 1; y < G.ny - 1; y++) for (int x = 1; x < G.nx - 1; x++) {
		bool a = A[G.id(x, y, z)];
		for (int ax = 0; ax < 3; ax++) {
			int bx = x + (ax == 0), by = y + (ax == 1), bz = z + (ax == 2);
			bool b = A[G.id(bx, by, bz)];
			if (a == b) continue;
			// the four cells around this edge (cells indexed by their low voxel)
			uint32_t c[4];
			if (ax == 0) { c[0] = cellVert(x, y - 1, z - 1); c[1] = cellVert(x, y, z - 1); c[2] = cellVert(x, y, z); c[3] = cellVert(x, y - 1, z); }
			else if (ax == 1) { c[0] = cellVert(x - 1, y, z - 1); c[1] = cellVert(x - 1, y, z); c[2] = cellVert(x, y, z); c[3] = cellVert(x, y, z - 1); }
			else { c[0] = cellVert(x - 1, y - 1, z); c[1] = cellVert(x, y - 1, z); c[2] = cellVert(x, y, z); c[3] = cellVert(x - 1, y, z); }
			V axis{(double)(ax == 0), (double)(ax == 1), (double)(ax == 2)};
			size_t solid = a ? G.id(bx, by, bz) : G.id(x, y, z);
			quads.push_back({{c[0], c[1], c[2], c[3]}, a ? axis * -1.0 : axis, !D[solid]});
		}
	}
	// Taubin smoothing: takes the staircase off without shrinking the room
	{
		std::vector<std::vector<uint32_t>> nb(vp.size());
		for (const Quad& qd : quads) for (int i = 0; i < 4; i++) {
			uint32_t a = qd.v[i], b = qd.v[(i + 1) & 3];
			nb[a].push_back(b); nb[b].push_back(a);
		}
		for (auto& l : nb) { std::sort(l.begin(), l.end()); l.erase(std::unique(l.begin(), l.end()), l.end()); }
		std::vector<V> tmp(vp.size());
		for (int it = 0; it < 12; it++) {
			double lam = (it & 1) ? -0.53 : 0.5;
			for (size_t i = 0; i < vp.size(); i++) {
				if (nb[i].empty()) { tmp[i] = vp[i]; continue; }
				V m; for (uint32_t j : nb[i]) m = m + vp[j];
				m = m * (1.0 / nb[i].size());
				tmp[i] = vp[i] + (m - vp[i]) * lam;
			}
			vp.swap(tmp);
		}
	}
	std::vector<uint32_t> tris; std::vector<uint8_t> cls;
	double area[4] = {0, 0, 0, 0};
	for (const Quad& qd : quads) {
		// split along the shorter diagonal
		uint32_t a = qd.v[0], b = qd.v[1], c = qd.v[2], d = qd.v[3];
		uint32_t T[2][3];
		if (len(vp[a] - vp[c]) <= len(vp[b] - vp[d])) { uint32_t t0[3] = {a, b, c}, t1[3] = {a, c, d}; memcpy(T[0], t0, 12); memcpy(T[1], t1, 12); }
		else { uint32_t t0[3] = {a, b, d}, t1[3] = {b, c, d}; memcpy(T[0], t0, 12); memcpy(T[1], t1, 12); }
		for (auto& t : T) {
			V n = cross(vp[t[1]] - vp[t[0]], vp[t[2]] - vp[t[0]]);
			double ar = 0.5 * len(n);
			if (ar < 1e-9) continue;
			if (dot(n, qd.want) < 0) { std::swap(t[1], t[2]); n = n * -1.0; }   // face into the air
			n = n * (1.0 / len(n));
			uint8_t k8 = qd.open ? 3 : n.y > 0.7 ? 0 : n.y < -0.5 ? 2 : 1;
			tris.insert(tris.end(), t, t + 3); cls.push_back(k8); area[k8] += ar;
		}
	}
	printf("  surface: %zu triangles; floor %.0f m2, walls %.0f m2, ceiling %.0f m2, OPEN %.0f m2\n",
	       cls.size(), area[0], area[1], area[2], area[3]);

	// ── where to stand, by default: along the room's long axis, ears at 1.6 m
	// above whatever floor is under them, sources a fifth of the way in ──────
	V cen; double sxx = 0, sxz = 0, szz = 0; size_t cnt = 0;
	for (int z = 0; z < G.nz; z++) for (int x = 0; x < G.nx; x++) for (int y = 0; y < G.ny; y++)
		if (A[G.id(x, y, z)]) { V c = G.centre(x, y, z); cen = cen + c; cnt++; break; }   // one per column
	cen = cen * (1.0 / cnt);
	for (int z = 0; z < G.nz; z++) for (int x = 0; x < G.nx; x++) for (int y = 0; y < G.ny; y++)
		if (A[G.id(x, y, z)]) { V c = G.centre(x, y, z) - cen; sxx += c.x * c.x; sxz += c.x * c.z; szz += c.z * c.z; break; }
	double ang = 0.5 * std::atan2(2 * sxz, sxx - szz);
	V axis{std::cos(ang), 0, std::sin(ang)};
	double tmin = 1e30, tmax = -1e30;
	for (int z = 0; z < G.nz; z++) for (int x = 0; x < G.nx; x++) for (int y = 0; y < G.ny; y++)
		if (A[G.id(x, y, z)]) { double t = dot(G.centre(x, y, z) - cen, axis); tmin = std::min(tmin, t); tmax = std::max(tmax, t); break; }
	auto stand = [&](double t, double ear) -> V {
		// the nearest column along the axis with room to stand, ear above its floor
		for (int r = 0; r < 200; r++) for (int s = -1; s <= 1; s += 2) {
			double tt = t + s * r * h * 0.5;
			V p = cen + axis * tt;
			int x = (int)((p.x - G.o.x) / h), z = (int)((p.z - G.o.z) / h);
			if (!G.in(x, 1, z)) continue;
			for (int y = 0; y < G.ny; y++) if (A[G.id(x, y, z)]) {
				int ey = y + (int)std::lround(ear / h);
				if (G.in(x, ey, z) && A[G.id(x, ey, z)] && dist[G.id(x, ey, z)] >= std::max(1, (int)(0.6 / h))) return G.centre(x, ey, z);
				break;
			}
		}
		return cen;
	};
	// listener a little past the middle, source behind it along the axis, no
	// more than 14 m away: in a 300 m tunnel, a source 120 m off is a distant
	// rumble rather than a starting point
	double L = tmax - tmin, gap = std::min(0.3 * L, 14.0);
	V lis = stand(tmin + 0.55 * L, 1.6), src = stand(tmin + 0.55 * L - gap, 1.5);
	printf("  default source (%.1f, %.1f, %.1f), listener (%.1f, %.1f, %.1f), %.1f m apart\n",
	       src.x, src.y, src.z, lis.x, lis.y, lis.z, len(src - lis));

	// ── the inside test: the air on a grid of about half a metre ────────────
	Grid I; I.h = h * f; I.o = G.o; I.nx = C.nx; I.ny = C.ny; I.nz = C.nz;
	std::vector<uint8_t> bits((I.n() + 7) / 8, 0);
	for (int z = 0; z < I.nz; z++) for (int y = 0; y < I.ny; y++) for (int x = 0; x < I.nx; x++) {
		int n = 0, a = 0;
		for (int dz = 0; dz < f; dz++) for (int dy = 0; dy < f; dy++) for (int dx = 0; dx < f; dx++) {
			int X = x * f + dx, Y = y * f + dy, Z = z * f + dz;
			if (!G.in(X, Y, Z)) continue;
			n++; a += A[G.id(X, Y, Z)];
		}
		if (n && a * 2 >= n) { size_t i = I.id(x, y, z); bits[i >> 3] |= 1 << (i & 7); }
	}

	// ── write ───────────────────────────────────────────────────────────────
	V vlo{1e30, 1e30, 1e30}, vhi{-1e30, -1e30, -1e30};
	for (const V& p : vp) { vlo = {std::min(vlo.x, p.x), std::min(vlo.y, p.y), std::min(vlo.z, p.z)}; vhi = {std::max(vhi.x, p.x), std::max(vhi.y, p.y), std::max(vhi.z, p.z)}; }
	FILE* o = fopen(outPath.c_str(), "wb");
	if (!o) { fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
	auto f32 = [&](double v) { float x = (float)v; fwrite(&x, 4, 1, o); };
	auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, o); };
	fwrite("CSP1", 1, 4, o);
	u32((uint32_t)vp.size()); u32((uint32_t)cls.size());
	f32(vlo.x); f32(vlo.y); f32(vlo.z); f32(vhi.x); f32(vhi.y); f32(vhi.z);
	for (const V& p : vp) {
		uint16_t q3[3] = {(uint16_t)std::lround((p.x - vlo.x) / (vhi.x - vlo.x) * 65535), (uint16_t)std::lround((p.y - vlo.y) / (vhi.y - vlo.y) * 65535),
		                  (uint16_t)std::lround((p.z - vlo.z) / (vhi.z - vlo.z) * 65535)};
		fwrite(q3, 2, 3, o);
	}
	fwrite(tris.data(), 4, tris.size(), o);
	fwrite(cls.data(), 1, cls.size(), o);
	f32(volume);
	f32(src.x); f32(src.y); f32(src.z); f32(lis.x); f32(lis.y); f32(lis.z);
	f32(I.o.x); f32(I.o.y); f32(I.o.z); f32(I.h); u32(I.nx); u32(I.ny); u32(I.nz);
	fwrite(bits.data(), 1, bits.size(), o);
	long bytes = ftell(o);
	fclose(o);
	printf("  wrote %s (%.0f KB)\n", outPath.c_str(), bytes / 1024.0);

	// previews: plan (air columns, seen from above) and section (seen from the side), as PGM
	if (!preview.empty()) {
		auto pgm = [&](const std::string& p, int w, int hh, const std::vector<uint8_t>& px) {
			FILE* g = fopen(p.c_str(), "wb"); fprintf(g, "P5 %d %d 255\n", w, hh); fwrite(px.data(), 1, px.size(), g); fclose(g);
		};
		std::vector<uint8_t> plan((size_t)G.nx * G.nz), sect((size_t)G.nx * G.ny);
		for (int z = 0; z < G.nz; z++) for (int x = 0; x < G.nx; x++) {
			int a = 0, s = 0;
			for (int y = 0; y < G.ny; y++) { a += A[G.id(x, y, z)]; s += S[G.id(x, y, z)]; }
			plan[(size_t)z * G.nx + x] = a ? (uint8_t)std::min(255, 60 + a * 195 / G.ny * 2) : s ? 30 : 0;
		}
		for (int y = 0; y < G.ny; y++) for (int x = 0; x < G.nx; x++) {
			int a = 0, s = 0;
			for (int z = 0; z < G.nz; z++) { a += A[G.id(x, y, z)]; s += S[G.id(x, y, z)]; }
			sect[(size_t)(G.ny - 1 - y) * G.nx + x] = a ? (uint8_t)std::min(255, 60 + a * 195 / G.nz * 2) : s ? 30 : 0;
		}
		// the scan itself: how much surface each column holds, from above and from the side and the front
		std::vector<uint8_t> sp((size_t)G.nx * G.nz), ss((size_t)G.nx * G.ny), sf((size_t)G.nz * G.ny);
		for (int z = 0; z < G.nz; z++) for (int x = 0; x < G.nx; x++) { int c = 0; for (int y = 0; y < G.ny; y++) c += S[G.id(x, y, z)]; sp[(size_t)z * G.nx + x] = (uint8_t)std::min(255, c * 40); }
		for (int y = 0; y < G.ny; y++) for (int x = 0; x < G.nx; x++) { int c = 0; for (int z = 0; z < G.nz; z++) c += S[G.id(x, y, z)]; ss[(size_t)(G.ny - 1 - y) * G.nx + x] = (uint8_t)std::min(255, c * 40); }
		for (int y = 0; y < G.ny; y++) for (int z = 0; z < G.nz; z++) { int c = 0; for (int x = 0; x < G.nx; x++) c += S[G.id(x, y, z)]; sf[(size_t)(G.ny - 1 - y) * G.nz + z] = (uint8_t)std::min(255, c * 40); }
		pgm(preview + "-scan-plan.pgm", G.nx, G.nz, sp);
		pgm(preview + "-scan-side.pgm", G.nx, G.ny, ss);
		pgm(preview + "-scan-front.pgm", G.nz, G.ny, sf);
		pgm(preview + "-plan.pgm", G.nx, G.nz, plan);
		pgm(preview + "-section.pgm", G.nx, G.ny, sect);
	}
	return 0;
}

// Drives the REAL Carillon -- src/carillon.cpp compiled as-is against libRack -- and
// checks what docs/carillon-design.md says must be true: V/OCT strikes the bell
// of that note and no other, REACH and velocity decide how far a strike goes,
// nothing runs away, a crossing takes one pulse per neighbour spacing of rod
// (and time in proportion to its length free-running), a felted bell conducts
// without sounding, a bell a fifth away answers louder than one a semitone
// away, the mics move the image, every bell can be hovered in every layout,
// the projection round-trips, and the cost.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/carillon-harness.cpp -o /tmp/carillon-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/carillon-harness
//   ... /tmp/carillon-harness render DIR    also writes listening WAVs into DIR
#include "../src/carillon.cpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Run {
	Carillon m; rack::engine::Module::ProcessArgs a; int64_t frame = 0;
	Run() { a.sampleRate = SR; a.sampleTime = 1.f / SR; for (int i = 0; i < 64; i++) tick(); }
	void tick() { a.frame = frame++; m.process(a); }
	void run(float sec) { int n = (int)(sec * SR); for (int i = 0; i < n; i++) tick(); }
	void runPeaks(float sec, float* pk) {
		for (int i = 0; i < PL_N; i++) pk[i] = 0.f;
		int n = (int)(sec * SR);
		for (int s = 0; s < n; s++) { tick(); for (int i = 0; i < PL_N; i++) pk[i] = std::max(pk[i], m.bell[i].energy); }
	}
	void quietGlow() { for (int i = 0; i < PL_N; i++) m.bell[i].glow = 0.f; }
	// how many bells a strike reached (a bell is excited only by an arrival above the floor)
	int reached(float sec) {
		bool hit[PL_N] = {}; int n = (int)(sec * SR);
		for (int s = 0; s < n; s++) { tick(); for (int i = 0; i < PL_N; i++) if (m.bell[i].glow > 0.99f) hit[i] = true; }
		int c = 0; for (int i = 0; i < PL_N; i++) c += hit[i]; return c;
	}
};

static void writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R) {
	FILE* f = fopen(path.c_str(), "wb"); if (!f) return;
	uint32_t n = (uint32_t)L.size(), bytes = n * 4, sr = (uint32_t)SR, br = sr * 4; uint16_t ch = 2, bits = 16, ba = 4, fmt = 1; uint32_t fl = 16, riff = 36 + bytes;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f);
	fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
	float pk = 1e-6f; for (uint32_t i = 0; i < n; i++) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
	float g = 0.7f / pk;
	for (uint32_t i = 0; i < n; i++) { int16_t a = (int16_t)(clamp(L[i] * g, -1.f, 1.f) * 32767), b = (int16_t)(clamp(R[i] * g, -1.f, 1.f) * 32767); fwrite(&a, 2, 1, f); fwrite(&b, 2, 1, f); }
	fclose(f);
}

// Play notes (semitones from C4, onset seconds, velocity 0-1) through the V/OCT
// and GATE inputs, record the stereo out.
struct NoteEv { float t; int semis; float vel; };
static void renderNotes(const std::string& path, const std::vector<NoteEv>& notes, float len, float shape = 0.f, int rods = Carillon::ROD_CLEAR, float reach = 0.5f, float damp = 0.36f) {
	Run r; r.m.unmask = true; r.m.presetRods(rods);
	r.m.params[Carillon::SHAPE_PARAM].setValue(shape);
	r.m.params[Carillon::REACH_PARAM].setValue(reach);
	r.m.params[Carillon::DAMP_PARAM].setValue(damp);
	// a poly cable, one channel per note in turn, so a chord is a chord
	sfs::previewConnect(r.m.inputs[Carillon::VOCT_INPUT], 8);
	sfs::previewConnect(r.m.inputs[Carillon::GATE_INPUT], 8);
	sfs::previewConnect(r.m.inputs[Carillon::VEL_INPUT], 8);
	r.run(0.01f);
	std::vector<float> L, R; int n = (int)(len * SR);
	for (int s = 0; s < n; s++) {
		float t = s / SR;
		for (int c = 0; c < 8; c++) r.m.inputs[Carillon::GATE_INPUT].setVoltage(0.f, c);
		for (size_t k = 0; k < notes.size(); k++) { const NoteEv& e = notes[k]; if (t >= e.t && t < e.t + 0.02f) {
			int c = (int)(k % 8);
			r.m.inputs[Carillon::VOCT_INPUT].setVoltage(e.semis / 12.f, c);
			r.m.inputs[Carillon::VEL_INPUT].setVoltage(e.vel * 10.f, c);
			r.m.inputs[Carillon::GATE_INPUT].setVoltage(10.f, c);
		} }
		r.tick();
		L.push_back(r.m.outputs[Carillon::L_OUTPUT].getVoltage()); R.push_back(r.m.outputs[Carillon::R_OUTPUT].getVoltage());
	}
	writeWav(path, L, R);
}

int main(int argc, char** argv) {
	rack::random::init();

	printf("== V/OCT strikes the bell of that note: every semitone of the two octaves, and octaves outside them fold ==\n");
	for (int rootSel : {0, 2}) {
		Run r; r.m.params[Carillon::ROOT_PARAM].setValue((float)rootSel);
		r.m.params[Carillon::SCALE_PARAM].setValue(1.f);   // a key, so felted bells are included in the test
		sfs::previewConnect(r.m.inputs[Carillon::VOCT_INPUT], 1); sfs::previewConnect(r.m.inputs[Carillon::GATE_INPUT], 1);
		r.m.presetRods(Carillon::ROD_CLEAR); r.run(0.01f);
		int wrong = 0, dup = 0; bool seen[PL_N] = {}; std::string bad;
		for (int semis = -24; semis <= 24; semis++) {
			r.quietGlow();
			r.m.inputs[Carillon::VOCT_INPUT].setVoltage((rootSel + semis) / 12.f);
			r.m.inputs[Carillon::GATE_INPUT].setVoltage(10.f); r.tick(); r.tick();
			r.m.inputs[Carillon::GATE_INPUT].setVoltage(0.f); r.tick();
			int got = -1, n = 0; for (int i = 0; i < PL_N; i++) if (r.m.bell[i].glow > 0.99f) { got = i; n++; }
			int want = semis; while (want > 12) want -= 12; while (want < -12) want += 12;
			if (n != 1 || got != want + 12) { wrong++; bad += string::f(" %+d->%d", semis, got - 12); }
			if (semis >= -12 && semis <= 12) { if (got >= 0 && seen[got]) dup++; if (got >= 0) seen[got] = true; }
		}
		check(wrong == 0 && dup == 0, string::f("root %s: 49 notes from -24 to +24, %d on the wrong bell, %d bells struck twice in the two octaves%s",
		      PL_NOTE[rootSel], wrong, dup, bad.c_str()));
	}

	printf("== REACH decides how far: bells reached along the chain from bell 0, full velocity ==\n");
	{
		int prev = 0; bool mono = true; std::string row; int c0 = 0, c1 = 0;
		for (float k : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
			Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_PATH); r.m.params[Carillon::REACH_PARAM].setValue(k); r.m.params[Carillon::SPEED_PARAM].setValue(1.f); r.run(0.01f);
			r.quietGlow(); r.m.strike(0, 4.f, -1);
			int c = r.reached(1.5f); row += string::f(" %d", c);
			if (c < prev) mono = false; prev = c;
			if (k == 0.f) c0 = c; if (k == 1.f) c1 = c;
		}
		check(mono && c0 == 1 && c1 == PL_N, "REACH 0 / .25 / .5 / .75 / 1:" + row + " bells (want 1 at the bottom, all 25 at the top)");
	}
	printf("== ... and velocity: bells reached at the default REACH ==\n");
	{
		int prev = 0; bool mono = true; std::string row;
		for (float v : {1.f, 2.5f, 5.f, 7.5f, 10.f}) {
			Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_PATH); r.m.params[Carillon::SPEED_PARAM].setValue(1.f); r.run(0.01f);
			float vel = v * 0.1f; r.quietGlow(); r.m.strike(0, vel * vel * vel * 4.f, -1);
			int c = r.reached(1.5f); row += string::f(" %d", c);
			if (c < prev) mono = false; prev = c;
		}
		check(mono && prev > 2, "1 / 2.5 / 5 / 7.5 / 10 V:" + row + " bells");
	}

	printf("== nothing runs away: every bell on a neighbour mesh struck at once, REACH at maximum ==\n");
	for (float damp : {0.36f, 0.f}) {
		Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_MESH); r.m.params[Carillon::DAMP_PARAM].setValue(damp);
		r.m.params[Carillon::REACH_PARAM].setValue(1.f); r.run(0.01f);
		for (int i = 0; i < PL_N; i++) r.m.strike(i, 4.f, -1);
		double tot[20]; bool grew = false;
		for (int w = 0; w < 20; w++) {
			r.run(1.f); tot[w] = 0; for (int i = 0; i < PL_N; i++) tot[w] += r.m.bell[i].energy;
			if (w > 1 && tot[w] > tot[w - 1] * 1.05) grew = true;
		}
		// at the bottom of DAMP the bells ring 4x as long as cast (a C3 hum for
		// minutes), so there the test is only that nothing grows
		bool decays = damp > 0.f ? tot[19] < tot[1] * 0.05 : tot[19] < tot[1];
		check(!grew && decays, string::f("DAMP %.2f: total energy %.2e at 2 s, %.2e at 20 s%s", damp, tot[1], tot[19], grew ? " (GREW)" : ""));
	}

	printf("== the clock is the crossing: the grid's chain at 4 Hz rings 250 ms per bell whatever SPEED says ==\n");
	{
		Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_PATH);
		r.m.params[Carillon::SPEED_PARAM].setValue(1.f);
		r.m.params[Carillon::REACH_PARAM].setValue(1.f);
		sfs::previewConnect(r.m.inputs[Carillon::CLOCK_INPUT], 1);
		r.run(0.01f);
		int period = (int)(0.25f * SR); double first[PL_N]; for (int i = 0; i < PL_N; i++) first[i] = -1;
		r.quietGlow();
		for (int s = 0; s < period * 10; s++) {
			r.m.inputs[Carillon::CLOCK_INPUT].setVoltage((s % period) < 100 ? 10.f : 0.f);
			if (s == 1000) { r.m.strike(0, 4.f, -1); first[0] = s / (double)SR; }
			r.tick();
			for (int i = 0; i < PL_N; i++) if (first[i] < 0 && r.m.bell[i].glow > 0.999f) first[i] = s / (double)SR;
		}
		bool ok = true; std::string gaps;
		for (int k = 2; k < 8; k++) { double g = first[k] - first[k - 1]; gaps += string::f("%.3f ", g); if (first[k] < 0 || std::fabs(g - 0.25) > 0.002) ok = false; }
		double wait = first[1] - first[0];
		check(ok && wait > 0.2 && wait < 0.25, string::f("struck 21 ms after a pulse: first crossing waited %.3f s, then gaps ", wait) + gaps);
	}

	printf("== a strike on the same sample as a pulse crosses on the NEXT pulse ==\n");
	{
		Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_PATH); r.m.params[Carillon::REACH_PARAM].setValue(1.f);
		sfs::previewConnect(r.m.inputs[Carillon::CLOCK_INPUT], 1); sfs::previewConnect(r.m.inputs[Carillon::GATE_INPUT], 1);
		sfs::previewConnect(r.m.inputs[Carillon::VOCT_INPUT], 1); r.m.inputs[Carillon::VOCT_INPUT].setVoltage(-1.f);   // C3 = bell 0
		r.run(0.01f); r.quietGlow();
		int period = (int)(0.25f * SR); double t0 = -1, t1 = -1;
		for (int s = 0; s < period * 3; s++) {
			r.m.inputs[Carillon::CLOCK_INPUT].setVoltage((s % period) < 100 ? 10.f : 0.f);
			r.m.inputs[Carillon::GATE_INPUT].setVoltage((s >= period && s < period + 200) ? 10.f : 0.f);
			if (s == period) t0 = s / (double)SR;
			r.tick();
			if (t1 < 0 && r.m.bell[1].glow > 0.999f) t1 = s / (double)SR;
		}
		check(t1 > 0 && std::fabs((t1 - t0) - 0.25) < 0.002, string::f("first crossing landed %.3f s after the strike (want one full period, 0.250)", t1 - t0));
	}

	printf("== a rod twice the neighbour spacing takes two pulses, and twice as long free-running ==\n");
	{
		// grid, by pitch: bell 0 at the front left, bell 2 two places along the same row
		Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_CLEAR); r.m.setRod(0, 1, true); r.m.setRod(0, 2, true);
		r.m.params[Carillon::REACH_PARAM].setValue(1.f);
		sfs::previewConnect(r.m.inputs[Carillon::CLOCK_INPUT], 1); r.run(0.01f); r.quietGlow();
		int period = (int)(0.25f * SR); double t1 = -1, t2 = -1, t0 = 1000 / (double)SR;
		for (int s = 0; s < period * 4; s++) {
			r.m.inputs[Carillon::CLOCK_INPUT].setVoltage((s % period) < 100 ? 10.f : 0.f);
			if (s == 1000) r.m.strike(0, 4.f, -1);
			r.tick();
			if (t1 < 0 && r.m.bell[1].glow > 0.999f) t1 = s / (double)SR;
			if (t2 < 0 && r.m.bell[2].glow > 0.999f) t2 = s / (double)SR;
		}
		check(std::fabs((t2 - t1) - 0.25) < 0.002, string::f("clocked: neighbour after %.3f s, two along after %.3f s (rod %.2f spacings, %d pulses)",
		      t1 - t0, t2 - t0, r.m.rodLen(0, 2), r.m.rodPulses(0, 2)));
		Run f; f.m.unmask = true; f.m.presetRods(Carillon::ROD_CLEAR); f.m.setRod(0, 1, true); f.m.setRod(0, 24, true);
		f.m.params[Carillon::REACH_PARAM].setValue(1.f); f.m.params[Carillon::SPEED_PARAM].setValue(0.5f); f.run(0.01f); f.quietGlow();
		f.m.strike(0, 4.f, -1);
		double a = -1, b = -1;
		for (int s = 0; s < (int)(SR * 3.f); s++) { f.tick(); if (a < 0 && f.m.bell[1].glow > 0.999f) a = s / (double)SR; if (b < 0 && f.m.bell[24].glow > 0.999f) b = s / (double)SR; }
		float want = f.m.rodLen(0, 24) / f.m.rodLen(0, 1);
		check(a > 0 && b > 0 && std::fabs(b / a - want) < 0.05f, string::f("free-running: neighbour %.3f s, far bell %.3f s, ratio %.2f (lengths %.2f)", a, b, b / a, want));
	}

	printf("== a felted bell conducts without sounding ==\n");
	{
		// C major, the chain by pitch: bell 0 is C (open), bell 1 C# (felted), bell 2 D (open)
		Run r; r.m.unmask = false; r.m.params[Carillon::SCALE_PARAM].setValue(1.f); r.m.presetRods(Carillon::ROD_PATH);
		r.m.params[Carillon::REACH_PARAM].setValue(0.75f); r.run(0.01f);
		r.m.strike(0, 4.f, -1);
		float pk[PL_N]; r.runPeaks(3.f, pk);
		check(!r.m.bell[0].masked && r.m.bell[1].masked && !r.m.bell[2].masked, string::f("C open %d, C# felted %d, D open %d", !r.m.bell[0].masked, r.m.bell[1].masked, !r.m.bell[2].masked));
		check(pk[2] > 1e-4f && pk[1] < pk[0] * 0.05f, string::f("struck C %.2e, felted C# %.2e, D beyond it %.2e", pk[0], pk[1], pk[2]));
	}

	printf("== a bell a fifth away answers louder than one a semitone away ==\n");
	{
		float fifth = 0, semi = 0;
		{ Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_CLEAR); r.m.setRod(12, 19, true); r.run(0.01f); r.m.strike(12, 4.f, -1); float pk[PL_N]; r.runPeaks(3.f, pk); fifth = pk[19]; }
		{ Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_CLEAR); r.m.setRod(12, 13, true); r.run(0.01f); r.m.strike(12, 4.f, -1); float pk[PL_N]; r.runPeaks(3.f, pk); semi = pk[13]; }
		check(fifth > semi * 1.3f, string::f("peak energy: a fifth away %.2e, a semitone away %.2e", fifth, semi));
	}

	printf("== the mics move the image ==\n");
	{
		Run probe; int left = 0; for (int i = 0; i < PL_N; i++) if (probe.m.bell[i].u < probe.m.bell[left].u - 1e-3f || (std::fabs(probe.m.bell[i].u - probe.m.bell[left].u) < 1e-3f && probe.m.bell[i].v < probe.m.bell[left].v)) left = i;
		auto balance = [](Run& r) { double l = 0, rr = 0; for (int s = 0; s < (int)(0.5f * SR); s++) { r.tick(); float L = r.m.outputs[Carillon::L_OUTPUT].getVoltage(), R = r.m.outputs[Carillon::R_OUTPUT].getVoltage(); l += L * L; rr += R * R; } return (rr - l) / (rr + l + 1e-9); };
		Run a; a.m.presetRods(Carillon::ROD_CLEAR); a.m.unmask = true; a.run(0.01f); a.m.strike(left, 4.f, -1);
		double b1 = balance(a);
		Run b; b.m.presetRods(Carillon::ROD_CLEAR); b.m.unmask = true; b.m.micU[0] = 1.5f; b.m.micU[1] = -1.5f; b.run(0.01f); b.m.strike(left, 4.f, -1);
		double b2 = balance(b);
		check(b1 < -0.2 && b2 > 0.2, string::f("front-left bell %d: balance %+.2f; the pair swapped: %+.2f (negative is left)", left, b1, b2));
	}

	printf("== in tune: SIZE moves whole octaves, and the partials that set the heard pitch are true ==\n");
	{
		float worst = 0.f; std::string row;
		for (float sz : {0.f, 0.2f, 0.4f, 0.5f, 0.6f, 0.8f, 1.f}) {
			Run r; r.m.params[Carillon::SIZE_PARAM].setValue(sz); r.run(0.01f);
			float semisFromC4 = 12.f * std::log2(r.m.bell[12].hz[2] / dsp::FREQ_C4);
			float off = 100.f * std::fabs(semisFromC4 - std::round(semisFromC4 / 12.f) * 12.f);
			worst = std::max(worst, off); row += string::f(" %+.0f", semisFromC4);
		}
		check(worst < 0.1f, string::f("bell C at SIZE 0/.2/.4/.5/.6/.8/1 sits at%s semitones from C4, worst %.3f cents off an octave", row.c_str(), worst));
		// the strike note is implied by the nominal, twelfth and upper octave: level-weighted, those must agree with the prime
		auto pairC = [](int a, int b, float harm) { float wa = std::pow(10.f, PL_DB[a] / 20.f), wb = std::pow(10.f, PL_DB[b] / 20.f);
			return 1200.f * std::log2((PL_RATIO[a] * wa + PL_RATIO[b] * wb) / (wa + wb) / harm); };
		float cp = pairC(2, 3, 1.f), cn = pairC(7, 8, 2.f);
		float up = 0.f; for (int m : {11, 12, 13, 15}) up = std::max(up, std::fabs(1200.f * std::log2(PL_RATIO[m] / std::round(PL_RATIO[m]))));
		check(std::fabs(cp) < 1.5f && std::fabs(cn) < 1.5f && up < 10.f, string::f("prime doublet %+.1f c, nominal doublet %+.1f c (sum %+.1f), twelfth/upper octave/5th/6th harmonics within %.1f c", cp, cn, cp + cn, up));
	}

	printf("== DAMP sets the ring: time for a middle-C bell to fall 60 dB ==\n");
	{
		std::string row; float t0 = 0.f, tDef = 0.f;
		for (float d : {0.f, 0.27f, 0.36f, 1.f}) {
			Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_CLEAR); r.m.params[Carillon::DAMP_PARAM].setValue(d); r.run(0.01f);
			r.m.strike(12, 4.f, -1); r.run(0.3f);
			float e0 = r.m.bell[12].energy; float t = 0.3f;
			while (r.m.bell[12].energy > e0 * 1e-6f && t < 400.f) { r.run(0.25f); t += 0.25f; }
			row += string::f(" %.1f s", t); if (d == 0.f) t0 = t; if (d == 0.36f) tDef = t;
		}
		check(t0 > tDef * 3.5f, "DAMP 0 / 0.27 (as cast) / 0.36 (default) / 1:" + row);
	}

	printf("== headroom: where the output clamped, it now bends, and position decides level less than velocity ==\n");
	{
		auto peakOf = [](std::vector<int> bells, float vel, float rate, float secs, int& over6) {
			Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_CLEAR); r.run(0.01f);
			float pk = 0.f; over6 = 0; int n = (int)(secs * SR), every = (int)(SR / rate);
			for (int i = 0; i < n; i++) {
				if (i % every == 0) for (int b : bells) r.m.strike(b, vel * vel * vel * 4.f, -1);
				r.tick();
				float v = std::max(std::fabs(r.m.outputs[Carillon::L_OUTPUT].getVoltage()), std::fabs(r.m.outputs[Carillon::R_OUTPUT].getVoltage()));
				pk = std::max(pk, v); if (v > 6.f) over6++;
			}
			return pk; };
		int o1, o2, o3;
		float tri = peakOf({0, 4, 7}, 1.f, 2.f, 6.f, o1);
		float rep = peakOf({0}, 0.8f, 4.f, 8.f, o2);
		float mid = peakOf({12}, 1.f, 0.25f, 2.f, o3);
		check(tri < 8.5f, string::f("front-row triad at 2 Hz, full velocity: peak %.1f V, %.0f ms in the soft knee above 6 V (it clamped 39 samples at 10 V)", tri, o1 * 1000.f / SR));
		check(rep < 6.f, string::f("front-left C3 struck at 4 Hz, velocity 0.8: peak %.1f V", rep));
		float lo = 1e9f, hi = 0.f;
		for (int b = 0; b < PL_N; b++) { int o; float p = peakOf({b}, 1.f, 0.25f, 1.f, o); lo = std::min(lo, p); hi = std::max(hi, p); }
		check(20.f * std::log10(hi / lo) < 9.f, string::f("one bell at full velocity: middle C %.1f V; across all 25 on the grid %.1f to %.1f V (%.1f dB)", mid, lo, hi, 20.f * std::log10(hi / lo)));
	}

	printf("== SHAPE and BRIGHT move the spectrum: centroid of one struck bell over its first half second ==\n");
	{
		auto centroid = [](float shape, float bright) {
			Run r; r.m.presetRods(Carillon::ROD_CLEAR); r.m.unmask = true;
			r.m.params[Carillon::SHAPE_PARAM].setValue(shape); r.m.params[Carillon::BRIGHT_PARAM].setValue(bright); r.run(0.01f);
			r.m.strike(12, 4.f, -1);
			const int N = 4096; rack::dsp::RealFFT fft(N); std::vector<float> in(N), sp(2 * N); double num = 0, den = 0;
			for (int blk = 0; blk < 6; blk++) {
				for (int i = 0; i < N; i++) { r.tick(); in[i] = r.m.bell[12].out * (0.5f - 0.5f * std::cos(2.f * (float)M_PI * i / (N - 1))); }
				fft.rfft(in.data(), sp.data());
				for (int k = 1; k < N / 2; k++) { double p = sp[2 * k] * sp[2 * k] + sp[2 * k + 1] * sp[2 * k + 1]; num += p * k * SR / N; den += p; }
			}
			return (float)(num / std::max(den, 1e-12));
		};
		float bell = centroid(0.f, 0.5f), bar = centroid(1.f, 0.5f), dull = centroid(0.f, 0.f), bright = centroid(0.f, 1.f);
		check(bar > bell * 1.3f, string::f("bell %.0f Hz, bar %.0f Hz", bell, bar));
		check(bright > dull * 1.3f, string::f("BRIGHT 0: %.0f Hz, BRIGHT 1: %.0f Hz", dull, bright));
	}

	printf("== every bell can be hovered, in every layout and tuning order, and the projection round-trips ==\n");
	for (int lay = 0; lay < PL_NLAYOUT; lay++)
		for (int ord = 0; ord < (lay == PL_KEYS ? 1 : PL_NORDER); ord++) {
			Carillon m; m.layout = lay; m.order = ord; m.relayout();
			CarillonDisplay d; d.module = &m; d.box.size = mm2px(Vec(126.08f, 56.f));
			CarillonView V(d.box.size.x, d.box.size.y);
			int missed = 0, offscreen = 0; float worst = 0.f; std::string which;
			for (int i = 0; i < PL_N; i++) {
				float x, y, u, v; V.project(m.bell[i].u, m.bell[i].v, x, y); V.unproject(x, y, u, v);
				worst = std::max(worst, std::max(std::fabs(u - m.bell[i].u), std::fabs(v - m.bell[i].v)));
				float r = carillonRadius(V, m, i);
				if (x - r < 0 || x + r > d.box.size.x || y - r < 0 || y + r > d.box.size.y) offscreen++;
				if (d.bellAt(Vec(x, y)) != i) { missed++; which += string::f(" %d", i); }
			}
			check(missed == 0 && offscreen == 0 && worst < 0.01f, string::f("%-12.12s %-10.10s: %d not hoverable at their own centre%s, %d off the display, round-trip error %.4f",
			      PL_LAYOUT_NAME[lay], lay == PL_KEYS ? "" : PL_ORDER_NAME[ord], missed, which.c_str(), offscreen, worst));
		}
	for (int root = 0; root < 12; root++) {       // the keyboard follows the root
		Carillon m; m.layout = PL_KEYS; m.root = root; m.relayout();
		CarillonDisplay d; d.module = &m; d.box.size = mm2px(Vec(126.08f, 56.f));
		CarillonView V(d.box.size.x, d.box.size.y);
		int missed = 0;
		for (int i = 0; i < PL_N; i++) { float x, y; V.project(m.bell[i].u, m.bell[i].v, x, y); if (d.bellAt(Vec(x, y)) != i) missed++; }
		if (missed) check(false, string::f("keyboard at root %s: %d bells not hoverable", PL_NOTE[root], missed));
	}

	printf("== cost: 25 bells ringing on a neighbour mesh, percent of one core ==\n");
	{
		Run r; r.m.unmask = true; r.m.presetRods(Carillon::ROD_MESH); r.run(0.01f);
		for (int i = 0; i < PL_N; i++) r.m.strike(i, 4.f, -1);
		double t0 = now(); r.run(2.f); double t = now() - t0;
		Run q; double t1 = now(); q.run(2.f); double tq = now() - t1;
		printf("  ringing %.2f%%, silent %.2f%%\n", 100.0 * t / 2.0, 100.0 * tq / 2.0);
	}

	if (argc > 2 && !std::strcmp(argv[1], "render")) {
		std::string dir = argv[2];
		printf("== listening renders into %s ==\n", dir.c_str());
		renderNotes(dir + "/single-C4.wav", {{0.f, 0, 0.8f}}, 8.f);
		renderNotes(dir + "/single-C3.wav", {{0.f, -12, 0.8f}}, 10.f);
		renderNotes(dir + "/single-C5.wav", {{0.f, 12, 0.8f}}, 6.f);
		std::vector<NoteEv> mel; static const int M[8] = {0, 2, 4, 5, 7, 5, 4, 0};
		for (int k = 0; k < 8; k++) mel.push_back({k * 0.45f, M[k], 0.75f});
		renderNotes(dir + "/melody.wav", mel, 8.f);
		renderNotes(dir + "/chord.wav", {{0.f, 0, 0.7f}, {0.f, 4, 0.7f}, {0.f, 7, 0.7f}}, 8.f);
		renderNotes(dir + "/ripple-chain.wav", {{0.f, -12, 0.9f}}, 10.f, 0.f, Carillon::ROD_PATH, 0.6f);
		renderNotes(dir + "/bar-C5.wav", {{0.f, 12, 0.8f}}, 4.f, 1.f);
		renderNotes(dir + "/sustain-DAMP0.wav", {{0.f, 7, 0.8f}, {3.f, 12, 0.6f}, {6.f, 4, 0.6f}}, 30.f, 0.f, Carillon::ROD_CLEAR, 0.5f, 0.f);
	}

	printf("\n%d failure(s)\n", fails);
	return fails ? 1 : 0;
}

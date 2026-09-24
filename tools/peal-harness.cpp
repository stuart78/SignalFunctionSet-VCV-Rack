// Drives the REAL Peal -- src/peal.cpp compiled as-is against libRack -- and
// checks what docs/peal-design.md says must be true: reach follows velocity,
// nothing runs away, a clocked crossing is one pulse, a felted bell conducts
// without sounding, a bell a fifth away answers louder than one a semitone
// away, the mics move the image, the projection round-trips, and the cost.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/peal-harness.cpp -o /tmp/peal-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/peal-harness
#include "../src/peal.cpp"
#include <chrono>
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Run {
	Peal m; rack::engine::Module::ProcessArgs a; int64_t frame = 0;
	Run() { a.sampleRate = SR; a.sampleTime = 1.f / SR; for (int i = 0; i < 64; i++) tick(); }
	void tick() { a.frame = frame++; m.process(a); }
	void run(float sec) { int n = (int)(sec * SR); for (int i = 0; i < n; i++) tick(); }
	// peak energy each bell reached over a run
	void runPeaks(float sec, float* pk) {
		for (int i = 0; i < PL_N; i++) pk[i] = 0.f;
		int n = (int)(sec * SR);
		for (int s = 0; s < n; s++) { tick(); for (int i = 0; i < PL_N; i++) pk[i] = std::max(pk[i], m.bell[i].energy); }
	}
};

int main() {
	rack::random::init();
	const float RING = 2e-4f;   // a bell above this rang audibly

	printf("== reach follows velocity: bells that ring along the 25-bell chain, REACH at its default ==\n");
	{
		int counts[4] = {}; const float vel[4] = {1.f, 2.5f, 5.f, 10.f};
		for (int k = 0; k < 4; k++) {
			Run r; r.m.presetRods(4); r.m.unmask = true; r.run(0.01f);
			float v = vel[k] * 0.1f;
			r.m.strike(0, v * v * v * 4.f, -1);
			float pk[PL_N]; r.runPeaks(6.f, pk);
			for (int i = 0; i < PL_N; i++) if (pk[i] > RING) counts[k]++;
		}
		check(counts[0] < counts[1] && counts[1] < counts[2] && counts[2] < counts[3] && counts[3] >= 10,
		      string::f("bells rung at 1 / 2.5 / 5 / 10 V: %d %d %d %d", counts[0], counts[1], counts[2], counts[3]));
	}

	printf("== nothing runs away: a full mesh struck everywhere at full velocity decays and never grows ==\n");
	{
		for (float damp : {0.25f, 0.f}) {
			Run r; r.m.presetRods(0); r.m.unmask = true; r.m.params[Peal::DAMP_PARAM].setValue(damp); r.run(0.01f);
			for (int i = 0; i < PL_N; i++) r.m.strike(i, 4.f, -1);
			double tot[12]; bool grew = false; double peakT = 0; double peakE = 0;
			for (int w = 0; w < 12; w++) {
				r.run(1.f); tot[w] = 0; for (int i = 0; i < PL_N; i++) tot[w] += r.m.bell[i].energy;
				if (tot[w] > peakE) { peakE = tot[w]; peakT = w + 1; }
				if (w > 1 && tot[w] > tot[w - 1] * 1.05) grew = true;
			}
			check(!grew && tot[11] < tot[1] * 0.01, string::f("DAMP %.2f: total energy %.2e at 2 s, %.2e at 12 s, peak in second %.0f%s", damp, tot[1], tot[11], peakT, grew ? " (GREW)" : ""));
		}
	}

	printf("== the clock is the crossing: with CLOCK at 4 Hz the chain rings 250 ms per bell whatever SPEED says ==\n");
	{
		Run r; r.m.presetRods(4); r.m.unmask = true;
		r.m.params[Peal::SPEED_PARAM].setValue(1.f);   // 10 ms free-running, which must NOT apply
		sfs::previewConnect(r.m.inputs[Peal::CLOCK_INPUT], 1);
		r.m.params[Peal::REACH_PARAM].setValue(1.f);
		r.run(0.01f);
		int period = (int)(0.25f * SR); double first[PL_N]; for (int i = 0; i < PL_N; i++) first[i] = -1;
		for (int s = 0; s < period * 8; s++) {
			r.m.inputs[Peal::CLOCK_INPUT].setVoltage((s % period) < 100 ? 10.f : 0.f);
			if (s == 1000) { r.m.strike(0, 4.f, -1); first[0] = s / (double)SR; }   // between pulses, so the first crossing waits for one
			r.tick();
			for (int i = 0; i < PL_N; i++) if (first[i] < 0 && r.m.bell[i].glow > 0.999f) first[i] = s / (double)SR;
		}
		// the chain visits bells 0 1 2 3 4 then 9 8 7 6 5 ...
		static const int ORDER[8] = {0, 1, 2, 3, 4, 9, 8, 7};
		bool ok = true; std::string gaps;
		for (int k = 2; k < 8; k++) { double g = first[ORDER[k]] - first[ORDER[k - 1]]; gaps += string::f("%.3f ", g); if (first[ORDER[k]] < 0 || std::fabs(g - 0.25) > 0.002) ok = false; }
		double wait = first[1] - first[0];
		check(ok && wait > 0.2 && wait < 0.25, string::f("struck 21 ms after a pulse: first crossing waited %.3f s, then gaps ", wait) + gaps);
	}

	printf("== a strike on the same sample as a pulse crosses on the NEXT pulse, not that one ==\n");
	{
		Run r; r.m.presetRods(4); r.m.unmask = true; r.m.params[Peal::REACH_PARAM].setValue(1.f);
		sfs::previewConnect(r.m.inputs[Peal::CLOCK_INPUT], 1); sfs::previewConnect(r.m.inputs[Peal::GATE_INPUT], 1);
		sfs::previewConnect(r.m.inputs[Peal::VOCT_INPUT], 1); r.m.inputs[Peal::VOCT_INPUT].setVoltage(r.m.bell[0].semis / 12.f);
		r.run(0.01f);
		int period = (int)(0.25f * SR); double t0 = -1, t1 = -1;
		for (int s = 0; s < period * 3; s++) {
			r.m.inputs[Peal::CLOCK_INPUT].setVoltage((s % period) < 100 ? 10.f : 0.f);
			// the gate rises on the same sample as the pulse, as a sequencer clocked from the same source does
			r.m.inputs[Peal::GATE_INPUT].setVoltage((s >= period && s < period + 200) ? 10.f : 0.f);
			if (s == period) t0 = s / (double)SR;
			r.tick();
			if (t1 < 0 && r.m.bell[1].glow > 0.999f) t1 = s / (double)SR;
		}
		check(t1 > 0 && std::fabs((t1 - t0) - 0.25) < 0.002, string::f("first crossing landed %.3f s after the strike (want one full period, 0.250)", t1 - t0));
	}

	printf("== a felted bell conducts without sounding ==\n");
	{
		// C major, chromatic layout: the top row runs G# A A# B C, so bell 1 is A (open), bell 2 A# (felted), bell 3 B (open)
		Run r; r.m.layout = 1; r.m.retune(); r.m.presetRods(4); r.m.unmask = false; r.run(0.01f);
		r.m.strike(1, 4.f, -1);
		float pk[PL_N]; r.runPeaks(3.f, pk);
		check(!r.m.bell[1].masked && r.m.bell[2].masked && !r.m.bell[3].masked, string::f("A open %d, A# felted %d, B open %d", !r.m.bell[1].masked, r.m.bell[2].masked, !r.m.bell[3].masked));
		check(pk[3] > RING && pk[2] < pk[1] * 0.05f, string::f("struck A %.2e, felted A# %.2e, B beyond it %.2e", pk[1], pk[2], pk[3]));
	}

	printf("== complementary resonance: a bell a fifth away answers louder than one a semitone away ==\n");
	{
		float fifth = 0, semi = 0;
		{ Run r; r.m.layout = 0; r.m.retune(); r.m.presetRods(1); r.m.unmask = true; r.run(0.01f);   // Tonnetz rows: right is a fifth
		  r.m.strike(12, 4.f, -1); float pk[PL_N]; r.runPeaks(3.f, pk); fifth = pk[13]; }
		{ Run r; r.m.layout = 1; r.m.retune(); r.m.presetRods(1); r.m.unmask = true; r.run(0.01f);   // chromatic rows: right is a semitone
		  r.m.strike(12, 4.f, -1); float pk[PL_N]; r.runPeaks(3.f, pk); semi = pk[13]; }
		check(fifth > semi * 1.3f, string::f("neighbour's peak energy: a fifth away %.2e, a semitone away %.2e", fifth, semi));
	}

	printf("== the mics move the image ==\n");
	{
		auto balance = [](Run& r) { double l = 0, rr = 0; for (int s = 0; s < (int)(0.5f * SR); s++) { r.tick(); float L = r.m.outputs[Peal::L_OUTPUT].getVoltage(), R = r.m.outputs[Peal::R_OUTPUT].getVoltage(); l += L * L; rr += R * R; } return (rr - l) / (rr + l + 1e-9); };
		Run a; a.m.presetRods(5); a.m.unmask = true; a.run(0.01f); a.m.strike(10, 4.f, -1);   // bell 10: left edge, middle row
		double b1 = balance(a);
		Run b; b.m.presetRods(5); b.m.unmask = true; b.m.micU[0] = 1.5f; b.m.micU[1] = -1.5f; b.run(0.01f); b.m.strike(10, 4.f, -1);   // the pair swapped
		double b2 = balance(b);
		check(b1 < -0.2 && b2 > 0.2, string::f("left-edge bell, mics at their default: balance %+.2f; the pair swapped over: %+.2f (negative is left)", b1, b2));
	}

	printf("== a rod may join any two bells, and a longer rod takes longer free-running ==\n");
	{
		Run r; r.m.presetRods(99); r.m.unmask = true; r.m.setRod(0, 24, true); r.m.setRod(0, 1, true);   // corner to corner, and a neighbour
		r.m.params[Peal::SPEED_PARAM].setValue(0.5f); r.run(0.01f);
		float per = pealSpeedSec(0.5f);
		r.m.strike(0, 4.f, -1);
		double t1 = -1, t24 = -1;
		for (int s = 0; s < (int)(SR * 3.f); s++) { r.tick(); if (t1 < 0 && r.m.bell[1].glow > 0.999f) t1 = s / (double)SR; if (t24 < 0 && r.m.bell[24].glow > 0.999f) t24 = s / (double)SR; }
		check(t1 > 0 && t24 > 0 && std::fabs(t24 / t1 - pealDist(0, 24)) < 0.05, string::f("neighbour at %.3f s, far corner at %.3f s: ratio %.2f (rod length %.2f)", t1, t24, t24 / t1, pealDist(0, 24)));
		(void)per;
	}

	printf("== SHAPE and BRIGHT move the spectrum: centroid of one struck bell over its first half second ==\n");
	{
		auto centroid = [](float shape, float bright) {
			Run r; r.m.presetRods(99); r.m.unmask = true;
			r.m.params[Peal::SHAPE_PARAM].setValue(shape); r.m.params[Peal::BRIGHT_PARAM].setValue(bright); r.run(0.01f);
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

	printf("== the projection round-trips: every bell's drawn position maps back to itself ==\n");
	{
		PealView V(mm2px(126.08f), mm2px(56.f));
		int bad = 0; float worst = 0;
		for (int i = 0; i < PL_N; i++) {
			float u0 = (float)(i % PL_SIDE) - 2.f, v0 = (float)(i / PL_SIDE), x, y, u, v;
			V.project(u0, v0, x, y); V.unproject(x, y, u, v);
			float e = std::max(std::fabs(u - u0), std::fabs(v - v0)); worst = std::max(worst, e); if (e > 0.01f) bad++;
		}
		check(bad == 0, string::f("%d of 25 off, worst error %.4f grid units", bad, worst));
	}

	printf("== cost: 25 bells ringing on a full mesh, percent of one core ==\n");
	{
		Run r; r.m.presetRods(0); r.m.unmask = true; r.run(0.01f);
		for (int i = 0; i < PL_N; i++) r.m.strike(i, 4.f, -1);
		double t0 = now(); r.run(2.f); double t = now() - t0;
		printf("  %.2f%%\n", 100.0 * t / 2.0);
	}

	printf("\n%d failure(s)\n", fails);
	return fails ? 1 : 0;
}

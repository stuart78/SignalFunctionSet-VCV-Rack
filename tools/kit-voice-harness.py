#!/usr/bin/env python3
"""Renders every Kit preset through the REAL engine and measures what a drum
designer would listen for: decay, where the energy sits in the spectrum, and
what the strongest partials are. Built on kit-stereo-harness.py's extraction
(verbatim blocks from src/kit.cpp, src/membrane.hpp included) so it cannot
test a stale copy; only the main() is its own.

    python3 tools/kit-voice-harness.py [preset-name ...]
"""
import os, sys, subprocess, importlib.util
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ksh", os.path.join(HERE, "kit-stereo-harness.py"))
ksh = importlib.util.module_from_spec(spec)
_argv = sys.argv; sys.argv = [sys.argv[0]]
spec.loader.exec_module(ksh)          # writes h.cpp as a side effect; harmless
sys.argv = _argv
H = ksh.H
cut = H.index('// ── measuring')
shim = H[:cut]

MAIN = r'''
#include <vector>
#include <cstdlib>
static const float SR = 48000.f;
struct Band { float lo, hi; double e; };
int main(int argc, char** argv) {
	const int N = 4096;
	for (int p = 0; p < KIT_NPRESET; p++) {
		if (argc > 1) { bool want = false; for (int a = 1; a < argc; a++) if (!std::strcmp(argv[a], KIT_PRESETS[p].name)) want = true; if (!want) continue; }
		Kit k; k.loadPreset(p); k.stereo = getenv("KV_STEREO") != NULL;
		if (getenv("KV_SNARE")) k.params[Kit::SNARE_PARAM].v = atof(getenv("KV_SNARE"));
		if (getenv("KV_TUNE"))  k.params[Kit::SNARETHR_PARAM].v = atof(getenv("KV_TUNE"));
		if (getenv("KV_MUFFLE")) k.params[Kit::MUFFLE_PARAM].v = atof(getenv("KV_MUFFLE"));
		if (getenv("KV_EXCITE")) k.params[Kit::EXCITE_PARAM].v = atof(getenv("KV_EXCITE"));
		k.inst[0].drum.sr = SR; k.inst[0].drum.clear(); k.control();
		sfs::Drum& d = k.inst[0].drum;
		// EXPERIMENTS, from the environment: things no knob reaches today.
		if (getenv("KV_TILT"))    { d.tilt = atof(getenv("KV_TILT")); d.updateStrike(); }
		if (getenv("KV_STRIKER")) { d.strikeR = atof(getenv("KV_STRIKER")); d.updateStrike(); }
		k.hit(0.7f);
		if (getenv("KV_GRAV"))    d.mallet.grav = atof(getenv("KV_GRAV"));
		if (getenv("KV_MSTIFF"))  d.mallet.stiff *= atof(getenv("KV_MSTIFF"));   // a harder CONTACT, not a harder mallet
		bool wantEnv = getenv("KV_ENV") != NULL;
		printf("== %-12s f0 %6.0f Hz   stiff %.2f  decay %.2fs  tone %.2f  muffle %.2f  strikeR %.2f\n",
		       KIT_PRESETS[p].name, d.f0, d.stiff, d.decay, d.tone, d.muffle, d.strikeR);
		// the eight strongest modes by excitation gain
		std::vector<int> idx; for (int q = 0; q < sfs::Drum::NM; q++) idx.push_back(q);
		std::sort(idx.begin(), idx.end(), [&](int a, int b){ return std::fabs(d.gain[a]) > std::fabs(d.gain[b]); });
		printf("   modes (m,n) Hz gain t60:");
		for (int i = 0; i < 8; i++) { int q = idx[i];
			printf("  (%d,%d)%5.0f %.2f %.2fs", sfs::MEMBRANE_MODES[q].m, sfs::MEMBRANE_MODES[q].n,
			       d.f0 * d.ratio[q], std::fabs(d.gain[q]), d.t60lo[q]); }
		printf("\n");
		// render 2 s mono
		int L = (int)(2.f * SR); std::vector<float> x(L);
		float peak = 0.f;
		int contact = 0; bool seen = false;
		for (int n = 0; n < L; n++) { float h[2], w[2]; d.process(h, w, k.stereo); x[n] = h[0] + w[0]; peak = std::max(peak, std::fabs(x[n]));
			if (d.mallet.active) { contact++; seen = true; } else if (seen && n > 100 && contact > 0 && n < (int)(0.2f*SR)) {} }
		printf("   mallet in contact %.2f ms%s\n", 1000.f * contact / SR, k.stereo ? "   (stereo tap: one tilt)" : "");
		// envelope: 5 ms peak windows; time from the peak to -20 / -40 / -60 dB
		int W = (int)(SR * 0.005f); std::vector<float> env;
		for (int n = 0; n + W <= L; n += W) { float m = 0; for (int i = 0; i < W; i++) m = std::max(m, std::fabs(x[n+i])); env.push_back(m); }
		int ip = 0; for (size_t i = 0; i < env.size(); i++) if (env[i] > env[ip]) ip = (int)i;
		auto tdb = [&](float db){ float thr = env[ip] * std::pow(10.f, db / 20.f); for (size_t i = ip; i < env.size(); i++) if (env[i] < thr) return (float)(i - ip) * 0.005f; return 9.f; };
		printf("   peak %.2f   t-20 %.3fs  t-40 %.3fs  t-60 %.3fs\n", peak, tdb(-20), tdb(-40), tdb(-60));
		if (wantEnv) {   // the first 80 ms in 2 ms steps, as dB re the peak
			int W2 = (int)(SR * 0.002f); printf("   env:");
			for (int n = 0; n + W2 <= (int)(SR * 0.08f); n += W2) { float m = 0; for (int i = 0; i < W2; i++) m = std::max(m, std::fabs(x[n+i]));
				printf(" %3.0f", 20.f * std::log10(std::max(m, 1e-5f) / std::max(peak, 1e-5f))); }
			printf("\n");
		}
		// spectra of three windows
		const float wins[3][2] = {{0.f, 0.03f}, {0.03f, 0.12f}, {0.12f, 0.40f}};
		for (int w = 0; w < 3; w++) {
			int a = ip * W + (int)(wins[w][0] * SR), b = ip * W + (int)(wins[w][1] * SR); if (b > L) b = L;
			int M = b - a; if (M < 256) continue;
			int nb = 1; while (nb * 2 <= M && nb < 16384) nb *= 2;
			std::vector<double> re(nb/2+1, 0), im(nb/2+1, 0);
			for (int kk = 0; kk <= nb/2; kk++) { double sr_ = 0, si = 0;
				for (int n = 0; n < nb; n++) { double hn = 0.5 - 0.5 * std::cos(2*M_PI*n/(nb-1)); double v = x[a+n] * hn;
					double ph = 2*M_PI*kk*n/nb; sr_ += v*std::cos(ph); si -= v*std::sin(ph); }
				re[kk] = sr_; im[kk] = si; }
			std::vector<double> P(nb/2+1); double tot = 0, cen = 0;
			Band bands[5] = {{0,500,0},{500,2000,0},{2000,6000,0},{6000,12000,0},{12000,24000,0}};
			for (int kk = 1; kk <= nb/2; kk++) { P[kk] = re[kk]*re[kk]+im[kk]*im[kk]; double f = (double)kk*SR/nb; tot += P[kk]; cen += f*P[kk];
				for (auto& B : bands) if (f >= B.lo && f < B.hi) B.e += P[kk]; }
			printf("   %3.0f-%3.0fms  centroid %5.0f Hz   energy <500 %2.0f%%  0.5-2k %2.0f%%  2-6k %2.0f%%  6-12k %2.0f%%  >12k %2.0f%%   peaks:",
			       wins[w][0]*1000, wins[w][1]*1000, tot > 0 ? cen/tot : 0.,
			       100*bands[0].e/tot, 100*bands[1].e/tot, 100*bands[2].e/tot, 100*bands[3].e/tot, 100*bands[4].e/tot);
			// top 4 local maxima
			std::vector<int> pk; for (int kk = 2; kk < nb/2-1; kk++) if (P[kk] > P[kk-1] && P[kk] >= P[kk+1]) pk.push_back(kk);
			std::sort(pk.begin(), pk.end(), [&](int a2, int b2){ return P[a2] > P[b2]; });
			double pmax = pk.empty() ? 1 : P[pk[0]];
			for (int i = 0; i < 4 && i < (int)pk.size(); i++) printf(" %5.0fHz %+.0fdB", (double)pk[i]*SR/nb, 10*std::log10(P[pk[i]]/pmax));
			printf("\n");
		}
	}
	return 0;
}
'''
OUT = os.path.join(HERE, '..')
open(os.path.join(OUT, 'hv.cpp'), 'w').write(shim + MAIN)
r = subprocess.run(['c++', '-std=c++11', '-O2', '-I', os.path.join(OUT, 'src'), '-o', '/tmp/kitvoice', os.path.join(OUT, 'hv.cpp')], capture_output=True, text=True)
if r.returncode: sys.exit(r.stderr[:3000])
os.execv('/tmp/kitvoice', ['/tmp/kitvoice'] + sys.argv[1:])

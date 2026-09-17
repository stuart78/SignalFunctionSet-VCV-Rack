// Drives the REAL Flock -- src/flock.cpp compiled as-is against Rack's own
// libRack -- and measures what the design doc says must be true: cohesion,
// the propagation delay after a pitch step, startle and recovery, call
// density against WEIGHT, and cost per bird. Nothing is extracted or
// re-implemented; if flock.cpp changes, this measures the change.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/flock-harness.cpp -o /tmp/flock-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/flock-harness
#include "../src/flock.cpp"
#include <chrono>
#include <cstdio>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Run {
	Flock m; rack::engine::Module::ProcessArgs a; int64_t frame = 0; int calls = 0;
	Run(float nbirdsK) {
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		sfs::previewConnect(m.inputs[Flock::GATE_INPUT], 1);
		sfs::previewConnect(m.inputs[Flock::VOCT_INPUT], 1);
		m.inputs[Flock::GATE_INPUT].setVoltage(10.f);
		m.params[Flock::WEIGHT_PARAM].setValue(nbirdsK);
	}
	void run(float sec) {
		int n = (int)(sec * SR);
		for (int i = 0; i < n; i++) { a.frame = frame++; m.process(a); calls += m.callsThisTick; m.callsThisTick = 0; }
	}
	float pct(float q) {   // q-quantile of bird height relative to the roost
		std::vector<float> v; for (int i = 0; i < m.nActive; i++) v.push_back(m.bird[i].heard + m.bird[i].y * m.latSt - m.roostY);
		std::sort(v.begin(), v.end()); return v[(size_t)(q * (v.size() - 1))];
	}
};

int main() {
	rack::random::init();
	printf("== cohesion: rms distance to the centroid in cube units, and the flock's pitch spread in semitones, after 4 s ==\n");
	for (float k : {0.1f, 0.214f, 0.5f, 1.f}) {
		Run r(k); r.run(4.f);
		float ps = 0.f; for (int i = 0; i < r.m.nActive; i++) { float d = r.m.bird[i].heard + r.m.bird[i].y * r.m.latSt - r.m.cyAbs; ps += d * d; }
		// elongation: the ratio of the largest to the smallest axis variance,
		// 1 for a ball; a real murmuration is nowhere near 1
		float vx = 0, vy = 0, vz = 0; for (int i = 0; i < r.m.nActive; i++) { float dx = r.m.bird[i].x - r.m.cx, dy = r.m.bird[i].y - r.m.cy, dz = r.m.bird[i].z - r.m.cz; vx += dx*dx; vy += dy*dy; vz += dz*dz; }
		float el = std::sqrt(std::max(vx, std::max(vy, vz)) / std::max(1e-6f, std::min(vx, std::min(vy, vz))));
		printf("  %4d birds  lat %.1f st  spread %.2f  elongation %.2f  pitch rms %.2f st  centre %+.2f st  calls/s %.0f\n",
		       r.m.nActive, r.m.latSt, r.m.spread, el, std::sqrt(ps / r.m.nActive), r.m.cyAbs - r.m.roostY, r.calls / 4.f);
	}
	printf("== propagation: +12 st step in V/OCT at t=0, arrival of the centroid and of the 10th/90th bird ==\n");
	for (float lag : {0.f, 0.5f, 1.f}) {
		Run r(0.214f); r.m.params[Flock::LAG_PARAM].setValue(lag); r.run(3.f);
		r.m.inputs[Flock::VOCT_INPUT].setVoltage(1.f);
		float tc = -1, t10 = -1, t90 = -1;
		for (int step = 0; step < 300; step++) {
			r.run(0.02f); float t = (step + 1) * 0.02f;
			if (tc  < 0 && std::fabs(r.m.cyAbs - r.m.roostY) < 1.5f) tc = t;
			if (t10 < 0 && r.pct(0.9f) > -1.5f) t10 = t;   // the keenest tenth has arrived
			if (t90 < 0 && r.pct(0.1f) > -1.5f) t90 = t;   // all but the slowest tenth
		}
		printf("  LAG %.1f: centroid within 1.5 st at %.2f s, first 10%% of birds at %.2f s, all but the slowest 10%% at %.2f s\n", lag, tc, t10, t90);
	}
	printf("== startle: spread before, peak during, and 1.5 s after; HAWK gate length ==\n");
	{
		Run r(0.214f); r.run(3.f); float s0 = r.m.spread; float peak = s0; float gate = 0.f;
		r.m.startle();
		for (int step = 0; step < 150; step++) { r.run(0.01f); peak = std::max(peak, r.m.spread); if (r.m.hawkInside) gate += 0.01f; }
		printf("  spread %.2f -> peak %.2f -> %.2f after 1.5 s; hawk inside for %.2f s\n", s0, peak, r.m.spread, gate);
	}
	printf("== audio: peak and rms of L/R over 3 s, and whether anything is not finite ==\n");
	{
		Run r(0.214f); r.run(1.f);
		double sl = 0, sr2 = 0; float pk = 0.f; long bad = 0; int n = (int)(3.f * SR);
		for (int i = 0; i < n; i++) {
			r.a.frame = r.frame++; r.m.process(r.a);
			float l = r.m.outputs[Flock::L_OUTPUT].getVoltage(), rr = r.m.outputs[Flock::R_OUTPUT].getVoltage();
			if (!std::isfinite(l) || !std::isfinite(rr)) { bad++; continue; }
			sl += l * l; sr2 += rr * rr; pk = std::max(pk, std::max(std::fabs(l), std::fabs(rr)));
		}
		printf("  peak %.2f V  rms L %.2f V  R %.2f V  non-finite %ld  centre %+.2f V  density %.1f V  hawk %.0f V\n",
		       pk, std::sqrt(sl / n), std::sqrt(sr2 / n), bad,
		       r.m.outputs[Flock::CENTRE_OUTPUT].getVoltage(), r.m.outputs[Flock::DENSITY_OUTPUT].getVoltage(),
		       r.m.outputs[Flock::HAWK_OUTPUT].getVoltage());
		// rotation moves the image: L/R energy balance at 0 and 180 degrees
		for (float rot : {0.f, 0.5f}) {
			Run q(0.214f); q.m.params[Flock::ROTATE_PARAM].setValue(rot);
			q.m.params[Flock::LATITUDE_PARAM].setValue(0.8f);   // a wide flock, so the image has width
			q.run(2.f); double el = 0, er = 0, elr = 0;
			for (int i = 0; i < n; i++) { q.a.frame = q.frame++; q.m.process(q.a);
				float l = q.m.outputs[Flock::L_OUTPUT].getVoltage(), rr = q.m.outputs[Flock::R_OUTPUT].getVoltage(); el += l * l; er += rr * rr; elr += l * rr; }
			printf("  ROTATE %3.0f deg: L %.2f  R %.2f (rms V)  L/R correlation %.2f (1 = mono)\n", rot * 360.f, std::sqrt(el / n), std::sqrt(er / n), elr / std::sqrt(el * er));
		}
	}
	printf("== stereo on startle: L/R correlation at rest, then over the half second after the hawk ==\n");
	{
		Run q(0.214f); q.run(2.f); int n = (int)(0.5f * SR);
		auto corr = [&]() { double el = 0, er = 0, elr = 0;
			for (int i = 0; i < n; i++) { q.a.frame = q.frame++; q.m.process(q.a);
				float l = q.m.outputs[Flock::L_OUTPUT].getVoltage(), rr = q.m.outputs[Flock::R_OUTPUT].getVoltage(); el += l * l; er += rr * rr; elr += l * rr; }
			return elr / std::sqrt(el * er + 1e-9); };
		double c0 = corr(); q.m.startle(); q.run(0.4f); double c1 = corr();
		printf("  rest %.2f -> startle %.2f\n", c0, c1);
	}
	printf("== cost: ms of CPU per second of audio ==\n");
	for (float k : {0.1f, 0.214f, 0.5f, 1.f}) {
		Run r(k); r.run(0.5f); double t0 = now(); r.run(2.f); double ms = (now() - t0) * 1000.0 / 2.0;
		printf("  %3d birds: %.1f ms/s (%.1f%% of one core)\n", r.m.nActive, ms, ms / 10.0);
	}
	return 0;
}

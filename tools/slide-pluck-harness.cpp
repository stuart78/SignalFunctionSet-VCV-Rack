// Slide's strings against the real module: a pick must sound from silence, on
// every string, whatever the gate length. A string asleep at -80 dB wakes on a
// pick, and what the pick put in has not reached the pickup when its burst
// ends, so a string that went back to sleep at that point stayed silent. A
// patch with something already ringing cannot show this (the bridge keeps the
// others awake, so the benchmark's busy stimulus measures +0.4 dB): every check
// here starts from silence.
//
//   clang++ -std=c++11 -O2 -w -DARCH_MAC -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/slide-pluck-harness.cpp -o /tmp/slide-pluck-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack -Wl,-undefined,dynamic_lookup
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/slide-pluck-harness
#include "../src/slide.cpp"
#include <cstdio>
#include <string>

rack::plugin::Plugin* pluginInstance = nullptr;
rack::plugin::Model* modelSlideX = nullptr;
static int fails = 0;
static void check(bool ok, const std::string& what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }

struct Rig {
	Slide* m = new Slide();
	Module::ProcessArgs a;
	int64_t fr = 0;
	Rig() {
		a.sampleRate = 48000.f; a.sampleTime = 1.f / 48000.f;
		for (auto& o : m->outputs) o.channels = 1;
		m->onReset();
		run(4800);
	}
	~Rig() { delete m; }
	void run(int n) { for (int i = 0; i < n; i++) { a.frame = fr++; m->process(a); } }
	// a gate of `gateLen` samples on channel c, then the rest of `total`; returns the peak
	float note(int c, int gateLen, int total) {
		float pk = 0.f;
		for (int i = 0; i < total; i++) {
			m->inputs[Slide::GATE_INPUT].setVoltage(i < gateLen ? 10.f : 0.f, c);
			a.frame = fr++; m->process(a);
			pk = std::max(pk, std::fabs(m->outputs[Slide::MIX_L_OUTPUT].getVoltage()));
		}
		return pk;
	}
	int awake() const { int n = 0; for (int i = 0; i < SLIDE_NCH; i++) n += !m->str[i].asleep; return n; }
};

int main() {
	contextSet(new Context);
	APP->engine = new engine::Engine;
	APP->engine->setSampleRate(48000.f);
	char buf[96];

	printf("== a mono stroke from silence, one fresh module per note\n");
	for (int gate : {240, 24000}) for (float v : {-2.f, -1.f, 0.f, 1.f, 2.f}) {
		Rig r;
		r.m->inputs[Slide::VOCT_INPUT].channels = 1;
		r.m->inputs[Slide::GATE_INPUT].channels = 1;
		r.m->inputs[Slide::VOCT_INPUT].setVoltage(v);
		float pk = r.note(0, gate, 48000);
		snprintf(buf, sizeof buf, "V/OCT %+.0f, %3.0f ms gate: %.2f V", v, gate / 48.f, pk);
		check(pk > 1.f, buf);
	}

	printf("== each string on its own poly channel, a 5 ms trigger, one fresh module per string\n");
	for (int c = 0; c < SLIDE_NCH; c++) {
		Rig r;
		r.m->inputs[Slide::GATE_INPUT].channels = SLIDE_NCH;
		r.run(1);   // a Rack trigger first read high does not fire, and these only run on a poly cable
		float pk = r.note(c, 240, 48000);
		snprintf(buf, sizeof buf, "string %d: %.2f V", c + 1, pk);
		check(pk > 0.5f, buf);
	}

	printf("== the pedal down at the pick (SWELL full)\n");
	{
		Rig r;
		r.m->params[Slide::SWELL_PARAM].setValue(1.f);
		r.m->inputs[Slide::GATE_INPUT].channels = 1;
		float pk = r.note(0, 240, 96000);
		snprintf(buf, sizeof buf, "swells in to %.2f V", pk);
		check(pk > 0.5f, buf);
	}

	printf("== lone notes, each after the last has rung out\n");
	{
		Rig r;
		r.m->inputs[Slide::GATE_INPUT].channels = 1;
		for (int k = 0; k < 8; k++) {
			float pk = r.note(0, 240, 48000);
			int waited = 0;
			while (r.awake() > 0 && waited < 60) { r.note(0, 0, 48000); waited++; }
			snprintf(buf, sizeof buf, "stroke %d: %.2f V, then all asleep after %d s more", k + 1, pk, waited);
			check(pk > 0.5f && r.awake() == 0, buf);
		}
	}

	printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
	return fails ? 1 : 0;
}

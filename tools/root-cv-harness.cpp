// The ROOT CV menu, against each real module. In every mode a cable must sound
// exactly like turning the ROOT knob to the same note, the default must stay
// 1V/oct, and the setting must survive a save and name itself on the jack.
// Each module is its own build, since their sources cannot share a file:
//
//   tools/root-cv-harness.sh
#ifdef MOD_SPOOL
#define DR_WAV_IMPLEMENTATION   // the implementation lives in phase.cpp
#endif
#include MOD_SRC
#include <cstdio>
#include <string>
#include <vector>
rack::plugin::Plugin* pluginInstance = nullptr;
#ifdef MOD_SLIDE
rack::plugin::Model* modelSlideX = nullptr;   // defined in the expander's source
#endif

static const int FRAMES = 96000;
static const int PERIOD = 6000;   // 8 Hz at 48 kHz
static int fails = 0;
static void check(bool ok, const std::string& what) {
	if (!ok) { printf("  FAIL  %s\n", what.c_str()); fails++; }
}

static void setup(MOD_CLASS* m) {
#ifdef MOD_SPOOL
	m->params[Spool::PMODE_PARAM].setValue(1.f);
	Spool::Tape& t = m->tape[0];
	t.len = 48000; t.pos = 0.0; t.detHz = 220.f; t.detOk = true;
	for (int i = 0; i < t.len; i++) t.buf[i] = 5.f * std::sin(2.f * (float)M_PI * 220.f * i / 48000.f);
#endif
	(void)m;
}

static std::vector<float> render(float knob, bool cable, float volts, int mode) {
	random::local().seed(1u, 2u);
	MOD_CLASS* m = new MOD_CLASS;
	engine::Module::SampleRateChangeEvent e; e.sampleRate = 48000.f; e.sampleTime = 1.f / 48000.f;
	static_cast<engine::Module*>(m)->onSampleRateChange(e);
	for (auto& o : m->outputs) o.channels = 1;
#ifdef MOD_RESET
	m->onReset();
#endif
	setup(m);
	m->rootCvMode = mode;
	m->params[MOD_CLASS::ROOT_PARAM].setValue(knob);
	m->inputs[MOD_CLASS::MOD_ROOT].channels = cable ? 1 : 0;
	m->inputs[MOD_CLASS::MOD_ROOT].setVoltage(cable ? volts : 0.f);
	static const std::vector<int> trigs = MOD_TRIGS;
	for (int id : trigs) m->inputs[id].channels = 1;
#ifdef MOD_KEY
	m->inputs[Key::IN_INPUT].channels = 1;
#endif
	engine::Module::ProcessArgs a; a.sampleRate = e.sampleRate; a.sampleTime = e.sampleTime;
	std::vector<float> out;
	out.reserve(FRAMES);
	for (int i = 0; i < FRAMES; i++) {
		bool high = (i % PERIOD) >= PERIOD / 2;   // starts low: a trigger first read high does not fire
		for (int id : trigs) m->inputs[id].setVoltage(high ? 10.f : 0.f);
#ifdef MOD_KEY
		m->inputs[Key::IN_INPUT].setVoltage((float)i / FRAMES);
#endif
		a.frame = i;
		m->process(a);
		float sum = 0.f;
		for (auto& o : m->outputs) sum += o.getVoltage();
		out.push_back(sum);
	}
	delete m;
	return out;
}

static float diff(const std::vector<float>& a, const std::vector<float>& b) {
	float d = 0.f;
	for (size_t i = 0; i < a.size(); i++) d = std::max(d, std::fabs(a[i] - b[i]));
	return d;
}

int main() {
	// Loom and Wheel read the sample rate through APP->engine
	contextSet(new Context);
	APP->engine = new engine::Engine;
	const std::vector<float> ref7 = render(7.f, false, 0.f, 0);
	const std::vector<float> ref5 = render(5.f, false, 0.f, 0);
	const std::vector<float> ref11 = render(11.f, false, 0.f, 0);
	// Without this the rest proves nothing: the render has to depend on the root.
	check(diff(ref7, ref5) > 1e-3f, "the render does not depend on the ROOT knob");

	struct { float volts; int mode; const std::vector<float>* ref; const char* what; } cases[] = {
		{7.f / 12.f,  sfs::ROOT_CV_VOCT, &ref7,  "1V/oct: 0.583 V = 7 semitones"},
		{7.f,         sfs::ROOT_CV_SEMI, &ref7,  "1 V per semitone: 7 V = 7 semitones"},
		{7.2f,        sfs::ROOT_CV_SEMI, &ref7,  "1 V per semitone: 7.2 V rounds to 7"},
		{7.f / 1.1f,  sfs::ROOT_CV_SPAN, &ref7,  "0-10 V: 6.36 V = 7 semitones"},
		{6.5f,        sfs::ROOT_CV_SPAN, &ref7,  "0-10 V: 6.5 V rounds to 7"},
		{10.f,        sfs::ROOT_CV_SPAN, &ref11, "0-10 V: 10 V = 11 semitones"},
	};
	for (auto& c : cases) {
		float d = diff(render(0.f, true, c.volts, c.mode), *c.ref);
		char line[160]; snprintf(line, sizeof line, "%s (off by %g V)", c.what, d);
		check(d == 0.f, line);
	}

	MOD_CLASS* m = new MOD_CLASS;
	const int id = MOD_CLASS::MOD_ROOT;
	check(m->rootCvMode == sfs::ROOT_CV_VOCT, "a new module is not 1V/oct");
	const std::string voctName = m->inputInfos[id]->name;
	check(voctName.find("1V/oct") != std::string::npos, "the jack does not say 1V/oct: " + voctName);
#ifdef MOD_SPOOL
	// Spool's dataToJson needs an engine's patch storage, so its JSON hooks are
	// read through the shared helpers directly.
	sfs::rootCvSet(m, id, m->rootCvMode, sfs::ROOT_CV_SPAN);
	check(m->inputInfos[id]->name == sfs::rootCvName(sfs::ROOT_CV_SPAN), "the jack does not name the setting");
	sfs::rootCvSet(m, id, m->rootCvMode, sfs::ROOT_CV_VOCT);
	check(m->inputInfos[id]->name == voctName, "the jack did not return to its 1V/oct name");
#else
	json_t* j = m->dataToJson();
	check(!json_object_get(j, "rootCv"), "the default was saved");
	json_decref(j);
	for (int mode : {(int)sfs::ROOT_CV_SEMI, (int)sfs::ROOT_CV_SPAN}) {
		m->rootCvMode = mode;
		j = m->dataToJson();
		MOD_CLASS* n = new MOD_CLASS;
		n->dataFromJson(j);
		check(n->rootCvMode == mode, "the setting did not survive a save");
		check(n->inputInfos[id]->name == sfs::rootCvName(mode), "the jack does not name the setting: " + n->inputInfos[id]->name);
		json_object_del(j, "rootCv");   // a patch from before the option
		n->dataFromJson(j);
		check(n->rootCvMode == sfs::ROOT_CV_VOCT, "a patch without the key is not 1V/oct");
		check(n->inputInfos[id]->name == voctName, "the jack did not return to its 1V/oct name: " + n->inputInfos[id]->name);
		json_decref(j);
		delete n;
	}
#endif
	delete m;
	printf("%-10s %s\n", MOD_NAME, fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}

// Drives the REAL Key module -- src/key.cpp compiled as-is and linked against
// Rack's own libRack -- from a module's JSON as saved in a .vcv patch, then
// runs process() with a CV into IN 1 and a trigger into TRIG 1, both poly.
// The strongest form of "verify with real code": nothing is extracted or
// retyped, and the module's own dataFromJson does the loading.
//
//   tar --use-compress-program=unzstd -xf patch.vcv -C /tmp/p
//   python3 -c "import json;p=json.load(open('/tmp/p/patch.json'));json.dump([m for m in p['modules'] if m['model']=='Key'][0],open('/tmp/key.json','w'))"
//   RACK="/Applications/VCV Rack 2 Pro.app/Contents/Resources"
//   c++ -std=c++11 -O1 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include -I src \
//       -o /tmp/keyrun tools/key-patch-harness.cpp "$RACK/libRack.dylib"
//   DYLD_LIBRARY_PATH="$RACK" /tmp/keyrun /tmp/key.json 16
//
// Two things a harness like this has to know about Rack: Port::setChannels()
// is a no-op on a port the engine has not connected (set .channels directly),
// and Module::paramsFromJson() goes through APP->engine, which is null here
// (set the params by hand from the JSON). Written for a tester's report that
// Key's sub-scales had no effect; with their exact patch data the sub-scale
// outputs differed from MAIN at 83-96% of trigger samples.
#include "plugin.hpp"
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
rack::plugin::Plugin* pluginInstance = nullptr;
#include "key.cpp"
int main(int argc, char** argv) {
	std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf();
	json_error_t err; json_t* mj = json_loads(ss.str().c_str(), 0, &err);
	if (!mj) { printf("json: %s\n", err.text); return 1; }
	Key k;
	// Module::paramsFromJson goes through the engine (APP is null here), so the
	// params are set by hand exactly as it would: id -> value.
	{ json_t* pa = json_object_get(mj, "params"); size_t i; json_t* pj;
	  json_array_foreach(pa, i, pj) {
		int id = (int)json_integer_value(json_object_get(pj, "id"));
		if (id >= 0 && id < (int)k.params.size()) k.params[id].setValue((float)json_number_value(json_object_get(pj, "value")));
	  } }
	k.dataFromJson(json_object_get(mj, "data"));
	printf("after fromJson: scalaLoaded=%d scala.n=%d path=%s\n", (int)k.scalaLoaded, k.scala.n, k.scalaPath.c_str());
	printf("masks: %llu %llu %llu\n", (unsigned long long)k.subMask[0].w[0], (unsigned long long)k.subMask[1].w[0], (unsigned long long)k.subMask[2].w[0]);
	int NCH = argc > 2 ? atoi(argv[2]) : 1;
	k.inputs[Key::IN_INPUT + 0].channels = NCH;
	k.inputs[Key::TRIG_CH_INPUT + 0].channels = NCH;    // a poly trigger, one channel per voice
	rack::Module::ProcessArgs a; a.sampleRate = 48000.f; a.sampleTime = 1.f / 48000.f; a.frame = 0;
	int diff[3] = {0,0,0}, N = 0;
	for (int n = 0; n < 48000 * 20; n++) {
		float t = n / 48000.f;
		for (int ch = 0; ch < NCH; ch++) {
			float cv = 1.0f * std::sin(t * (0.7f + 0.31f * ch)) + 0.6f * std::sin(t * 1.9f + ch) + 0.3f * std::sin(t * 4.3f + 2.f * ch);
			k.inputs[Key::IN_INPUT + 0].setVoltage(cv, ch);
		}
		for (int ch = 0; ch < NCH; ch++) {                 // each voice on its own clock phase
			bool trig = ((n + ch * 300) % 4800) < 240;
			k.inputs[Key::TRIG_CH_INPUT + 0].setVoltage(trig ? 10.f : 0.f, ch);
		}
		k.process(a); a.frame++;
		if (n % 4800 == 1000) {
			for (int ch = 0; ch < NCH; ch++) {
				float o0 = k.outputs[Key::OUT_OUTPUT].getVoltage(ch);
				for (int c = 1; c < 4; c++) if (std::fabs(k.outputs[Key::OUT_OUTPUT + c].getVoltage(ch) - o0) > 1e-4f) diff[c-1]++;
			}
			N += NCH;
		}
	}
	printf("footer now: M %.3fV  1 %.3fV  2 %.3fV  3 %.3fV\n", k.shownVolts[0], k.shownVolts[1], k.shownVolts[2], k.shownVolts[3]);
	printf("parent %d, subs %d/%d/%d;  of %d voice-samples SUB1 differs %d, SUB2 %d, SUB3 %d\n",
	       k.parent.n, k.sub[0].n, k.sub[1].n, k.sub[2].n, N, diff[0], diff[1], diff[2]);
	return 0;
}

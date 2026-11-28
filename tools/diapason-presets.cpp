// Writes Diapason's factory presets to presets/Diapason/, from the REAL module:
// each preset is a module set up in code and saved as Rack saves one (the
// parameters, then the module's own data), so a preset cannot fall out of step
// with the parameter list.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/diapason-presets.cpp -o /tmp/diapason-presets \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/diapason-presets
//
// Run from the repository. The plugin's version is read from plugin.json.
#include "../src/diapason.cpp"
#include <cstdio>
#include <string>
#include <sys/stat.h>
rack::plugin::Plugin* pluginInstance = nullptr;

struct Preset {
	const char* name;
	std::vector<int> stops;     // stops, then 100 + coupler, 200 the tremulant
	float swell, wind;
	int pipes;                  // 0 steady, 1 natural, 2 old organ
	bool sides;                 // the two sides of the case to LEFT and RIGHT
	float level;                // dB
	int room;                   // 0 dry; 1 Old Church, 2 St Bartholomew-the-Less, 3 St Peter Stourton,
	                            // 4 Hintze Hall, 5 the Pantheon, 6 the Basilica Cistern
};

static const std::vector<Preset> PRESETS = {
	{"Evensong strings", {SW_GAMBA8, SW_CELESTE8, SW_GEDACKT8, 200, PD_SUBBASS16, 100 + SW_PD}, 0.6f, 0.4f, 1, false, 4.f, 2},
	{"Flute choir", {GT_ROHR8, GT_BOURDON16, SW_GEDACKT8, PD_SUBBASS16, 100 + SW_GT, 100 + SW_PD}, 0.8f, 0.4f, 1, false, 3.f, 3},
	{"Principal chorus", {GT_PRINCIPAL8, GT_OCTAVE4, GT_FIFTEENTH2, GT_MIXTURE, GT_BOURDON16, PD_SUBBASS16, PD_OCTAVE8, 100 + GT_PD},
	 1.f, 0.4f, 1, false, 0.f, 1},
	{"Trumpet tune", {GT_TRUMPET8, GT_PRINCIPAL8, SW_GEDACKT8, SW_PRINCIPAL4, PD_SUBBASS16, PD_OCTAVE8}, 0.7f, 0.4f, 1, false, 0.f, 1},
	{"Oboe solo", {SW_OBOE8, SW_GEDACKT8, 200, GT_ROHR8, PD_SUBBASS16}, 0.7f, 0.4f, 1, false, 4.f, 2},
	{"Reed chorus", {SW_TROMPETTE8, SW_OBOE8, SW_PRINCIPAL4, SW_GAMBA8, GT_PRINCIPAL8, GT_TRUMPET8,
	                 PD_POSAUNE16, PD_OCTAVE8, PD_SUBBASS16, 100 + SW_GT, 100 + GT_PD}, 1.f, 0.5f, 1, false, -2.f, 1},
	{"Full organ", {GT_BOURDON16, GT_PRINCIPAL8, GT_ROHR8, GT_OCTAVE4, GT_FIFTEENTH2, GT_MIXTURE, GT_TRUMPET8,
	                SW_GEDACKT8, SW_GAMBA8, SW_PRINCIPAL4, SW_OBOE8, SW_TROMPETTE8,
	                PD_BOURDON32, PD_SUBBASS16, PD_OCTAVE8, PD_POSAUNE16, 100 + SW_GT, 100 + SW_PD, 100 + GT_PD},
	 1.f, 0.5f, 1, false, -6.f, 5},
	{"Quiet 32-foot", {PD_BOURDON32, PD_SUBBASS16, SW_GEDACKT8, SW_GAMBA8, SW_CELESTE8, 100 + SW_PD}, 0.2f, 0.3f, 1, false, 8.f, 6},
	{"Village organ", {GT_PRINCIPAL8, GT_OCTAVE4, GT_ROHR8, PD_SUBBASS16, 100 + GT_PD}, 1.f, 0.9f, 2, false, 2.f, 3},
	{"Plenum for Cistern", {GT_PRINCIPAL8, GT_OCTAVE4, GT_FIFTEENTH2, GT_MIXTURE, GT_BOURDON16,
	                SW_GEDACKT8, SW_PRINCIPAL4, PD_SUBBASS16, PD_OCTAVE8, 100 + SW_GT, 100 + GT_PD}, 1.f, 0.4f, 1, true, 0.f, 0},
};

int main() {
	static rack::plugin::Plugin plug;
	plug.slug = "SignalFunctionSet";
	plug.path = ".";
	pluginInstance = &plug;
	rack::contextSet(new rack::Context);
	APP->engine = new rack::engine::Engine;
	json_error_t err;
	json_t* pj = json_load_file("plugin.json", 0, &err);
	if (!pj) { fprintf(stderr, "run from the repository: no plugin.json\n"); return 1; }
	std::string version = json_string_value(json_object_get(pj, "version"));
	json_decref(pj);
	mkdir("presets", 0755);
	mkdir("presets/Diapason", 0755);
	for (const Preset& P : PRESETS) {
		Diapason m;
		for (int s = 0; s < NSTOP; s++) m.params[Diapason::STOP_PARAM + s].setValue(0.f);
		for (int c = 0; c < NCOUP; c++) m.params[Diapason::COUPLER_PARAM + c].setValue(0.f);
		m.params[Diapason::TREM_PARAM].setValue(0.f);
		for (int v : P.stops) {
			if (v >= 200) m.params[Diapason::TREM_PARAM].setValue(1.f);
			else if (v >= 100) m.params[Diapason::COUPLER_PARAM + v - 100].setValue(1.f);
			else m.params[Diapason::STOP_PARAM + v].setValue(1.f);
		}
		m.params[Diapason::SWELL_PARAM].setValue(P.swell);
		m.params[Diapason::WIND_PARAM].setValue(P.wind);
		m.params[Diapason::LEVEL_PARAM].setValue(P.level);
		m.params[Diapason::ROOM_PARAM].setValue((float)P.room);
		m.pipes = P.pipes;
		m.sidesOut = P.sides;
		m.lastPiston = 0; m.pistonInit = true;
		json_t* root = json_object();
		json_object_set_new(root, "plugin", json_string("SignalFunctionSet"));
		json_object_set_new(root, "model", json_string("Diapason"));
		json_object_set_new(root, "version", json_string(version.c_str()));
		json_object_set_new(root, "params", m.paramsToJson());
		json_object_set_new(root, "data", m.dataToJson());
		std::string path = std::string("presets/Diapason/") + P.name + ".vcvm";
		if (json_dump_file(root, path.c_str(), JSON_INDENT(2) | JSON_REAL_PRECISION(9)) != 0) { fprintf(stderr, "could not write %s\n", path.c_str()); return 1; }
		json_decref(root);
		printf("%s\n", path.c_str());
	}
	return 0;
}

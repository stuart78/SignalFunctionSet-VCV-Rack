#pragma once
// What a volt on a ROOT input means. 1V/oct is the plugin's convention and the
// default, because Key's and Note's ROOT outs are 1V/oct and any pitch cable can
// set the key. It also fits all twelve roots into one volt, which is hard to dial
// by hand, so the context menu offers two other readings. Only the input changes:
// a module's own ROOT out stays 1V/oct.
#include "plugin.hpp"
#include <cmath>
#include <string>

namespace sfs {

enum RootCvMode { ROOT_CV_VOCT, ROOT_CV_SEMI, ROOT_CV_SPAN, ROOT_CV_MODES };

static const char* const ROOT_CV_QUANTIZED_NAME = "Root CV (1V/oct, semitone-quantized)";
static const char* const ROOT_CV_FREE_NAME = "Root CV (1V/oct)";

// Semitones to add to the ROOT knob. 1V/oct is left unrounded for the caller, as
// it always was; the other two are whole semitones. 10 V x 1.1 lands on 11, so
// 0 V is the knob's own note and 10 V is the eleventh semitone above it.
inline float rootCvSemis(float volts, int mode) {
	if (mode == ROOT_CV_SEMI) return std::round(volts);
	if (mode == ROOT_CV_SPAN) return std::round(volts * 1.1f);
	return volts * 12.f;
}

inline std::string rootCvName(int mode, const char* voctName = ROOT_CV_QUANTIZED_NAME) {
	if (mode == ROOT_CV_SEMI) return "Root CV (1 V per semitone)";
	if (mode == ROOT_CV_SPAN) return "Root CV (0-10 V across the 12 notes, 0.91 V per note)";
	return voctName;
}

inline void rootCvSet(Module* m, int inputId, int& mode, int value, const char* voctName = ROOT_CV_QUANTIZED_NAME) {
	mode = clamp(value, 0, ROOT_CV_MODES - 1);
	m->inputInfos[inputId]->name = rootCvName(mode, voctName);
}

// Saved only when it is not the default, so a patch that never touched it is unchanged.
inline void rootCvToJson(json_t* j, int mode) {
	if (mode != ROOT_CV_VOCT) json_object_set_new(j, "rootCv", json_integer(mode));
}

inline void rootCvFromJson(json_t* j, Module* m, int inputId, int& mode, const char* voctName = ROOT_CV_QUANTIZED_NAME) {
	json_t* v = json_object_get(j, "rootCv");
	rootCvSet(m, inputId, mode, v ? (int)json_integer_value(v) : (int)ROOT_CV_VOCT, voctName);
}

inline void rootCvMenu(Menu* menu, Module* m, int inputId, int& mode, const char* voctName = ROOT_CV_QUANTIZED_NAME) {
	int* p = &mode;
	menu->addChild(createIndexSubmenuItem("ROOT CV",
		{"1V/oct", "1 V per semitone", "0-10 V across the 12 notes"},
		[=]() { return (size_t)*p; },
		[=](size_t v) { rootCvSet(m, inputId, *p, (int)v, voctName); }));
}

} // namespace sfs

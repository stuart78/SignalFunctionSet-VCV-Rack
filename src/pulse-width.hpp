#pragma once
#include "plugin.hpp"

// Shared "encoder-safe" gate/trigger pulse width.
//
// Direct ES-9 outputs are full-sample-rate DAC channels, so the default 1 ms
// trigger is tens of samples wide and always lands cleanly. The Expert Sleepers
// Encoders (8CV / 8GT / ES-5) time-multiplex 8 channels down one connection, so
// each channel is effectively resampled to ~2-16 kHz depending on how many are
// active. A 1 ms pulse can be only ~2 frames wide there -- jittered, halved, or
// dropped outright. Widening the pulse to 3-5 ms makes clocks/gates survive that
// path (and gives a companion V/oct channel time to settle before the gate is
// sampled).
//
// The index is stored (not the raw float) so patches stay stable and the menu
// can render a radio selection. Default index 0 == 1 ms, i.e. unchanged from
// legacy behaviour for existing patches that never touch the setting.
namespace sfs {

static const int   NUM_PULSE_WIDTHS = 4;
static const float PULSE_WIDTHS[NUM_PULSE_WIDTHS]       = { 0.001f, 0.002f, 0.005f, 0.010f };
static const char* PULSE_WIDTH_LABELS[NUM_PULSE_WIDTHS] = {
	"1 ms (default)", "2 ms", "5 ms (encoder-safe)", "10 ms" };

// Clamp a stored index and return the pulse width in seconds.
inline float pulseWidthSec(int idx) {
	if (idx < 0 || idx >= NUM_PULSE_WIDTHS) idx = 0;
	return PULSE_WIDTHS[idx];
}

// Append a "Gate/trigger width" submenu that reads/writes *idxPtr. The current
// choice shows as the submenu's right-hand text.
inline void addPulseWidthMenu(Menu* menu, int* idxPtr,
                              const char* label = "Gate/trigger width") {
	int cur = (*idxPtr >= 0 && *idxPtr < NUM_PULSE_WIDTHS) ? *idxPtr : 0;
	menu->addChild(createSubmenuItem(label, PULSE_WIDTH_LABELS[cur],
		[idxPtr](Menu* sub) {
			for (int i = 0; i < NUM_PULSE_WIDTHS; i++) {
				sub->addChild(createCheckMenuItem(PULSE_WIDTH_LABELS[i], "",
					[idxPtr, i]() { return *idxPtr == i; },
					[idxPtr, i]() { *idxPtr = i; }));
			}
		}));
}

} // namespace sfs

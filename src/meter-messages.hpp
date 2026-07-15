#pragma once
// Meter → Meter X expander bus. Meter (mother) writes this into its
// rightExpander.producerMessage each sample and flips; the expander reads it
// from leftExpander.module->rightExpander.consumerMessage.
struct MeterExpanderMessage {
	bool running = false;     // clock is running (drives the RUN gate out)
	bool ppqn24 = false;      // a 24-PPQN clock pulse fired this sample
	bool bar[8] = {};         // 1, 2, 4, 8, 16, 32, 64, 128-bar pulse fired this sample
	float barPos = 0.f;       // continuous position in bars since reset (for cycle pie charts)
};

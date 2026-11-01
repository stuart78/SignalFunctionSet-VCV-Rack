#pragma once
// Concrete's two expanders, over the expander bus, once per sample.
//
//   Concrete In  (LEFT of Concrete):  the four lanes' recording inputs and REC states.
//   Concrete Out (RIGHT of Concrete): the four lanes' stereo outputs.
//
// Both expanders are dumb: the take buffers and the clips live in Concrete, which
// is the only thing that reads or writes them.
static const int CONCRETE_LANES = 4;

// Concrete In -> Concrete
struct ConcreteInMessage {
	float l[CONCRETE_LANES] = {}, r[CONCRETE_LANES] = {};   // this sample, volts
	bool rec[CONCRETE_LANES] = {};                      // REC button latched or REC gate high
	bool on[CONCRETE_LANES] = {};                       // a cable in L or R
};

// Concrete -> Concrete In: what is actually being recorded, for the REC lights
struct ConcreteBackMessage {
	bool recording[CONCRETE_LANES] = {};
	bool running = false;
};

// Concrete -> Concrete Out
struct ConcreteOutMessage {
	float l[CONCRETE_LANES] = {}, r[CONCRETE_LANES] = {};   // volts, after VOL, FILTER and PAN
};

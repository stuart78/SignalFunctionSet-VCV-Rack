#pragma once
// Flock In -> Flock, over the expander bus, once per sample.
//
// The expander is deliberately dumb: two audio samples and three controls.
// The buffer lives in Flock, because Flock's birds are what read it, and
// reading another module's memory across the bus is a race the message
// protocol exists to avoid.
struct FlockInMessage {
	float l = 0.f, r = 0.f;      // this sample, volts
	float env = 0.25f;           // grain envelope: attack as a fraction of the call
	float reach = 1.f;           // seconds of the buffer the birds may read
	bool  freeze = false;        // hold the buffer where it is
	bool  on = false;            // a cable is patched into L or R
};

#pragma once
// PolyKit In -> Kit expander bus. PolyKit In sits to the LEFT of a Kit and
// exposes every one of Kit's per-instrument inputs as its own jack, in a
// matrix: thirteen columns (Kit's thirteen inputs) by eight rows (the
// instruments). It is the alternative to a poly cable, for a rack where the
// sources are eight separate modules rather than one poly one.
//
// PolyKit In writes this into its rightExpander.producerMessage each sample
// and flips; Kit reads it from leftExpander.module->rightExpander
// .consumerMessage. A patched jack OVERRIDES that channel of the poly cable
// on Kit's own panel; an unpatched one leaves the poly cable in charge, so
// the two can be mixed.
static const int PK_NCH = 8;
enum PolyKitCol {
	PK_TRIG, PK_VOCT, PK_VEL, PK_X, PK_Y,
	PK_SIZE, PK_TENS, PK_MAT, PK_AIR, PK_DECAY, PK_TONE, PK_EXC, PK_MUFFLE,
	PK_NCOL
};
struct PolyKitMessage {
	float v[PK_NCOL][PK_NCH] = {};
	bool  on[PK_NCOL][PK_NCH] = {};
};

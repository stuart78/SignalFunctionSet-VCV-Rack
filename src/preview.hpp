#pragma once
// A BROWSER THUMBNAIL DRAWN BY THE LIVE DISPLAY CODE.
//
// The Library's screenshot and the module browser build every widget with
// `module == NULL`, and the pattern in docs/conventions/browser-preview-pattern.md
// was to draw a stand-in: a synthetic spectrum, four uniform ribbons, a row of
// tubes at hand-picked angles. A stand-in is a second drawing of the same screen,
// and it drifts -- Sigma's was still bars an hour after the live view stopped
// being, and Chime's had every tube dimmed because the flag that meant "preview"
// also meant "dark".
//
// The alternative is to draw a REAL instance: build the module outside the
// engine, run it for a second or two with something to react to (a note held,
// a kit struck, a hand on the paper), then point the display at it and let the
// live path draw what it would draw on a running patch. Nothing here touches
// the engine; the instance is private to the UI thread, built once, and kept
// for the life of the plugin. It is never freed on purpose: it would otherwise
// be destroyed at unload, after the things it might reference are gone.
#include <rack.hpp>

namespace sfs {

static const float PREVIEW_SR = 48000.f;

inline rack::engine::Module::ProcessArgs previewArgs(int64_t frame = 0) {
	rack::engine::Module::ProcessArgs a;
	a.sampleRate = PREVIEW_SR;
	a.sampleTime = 1.f / PREVIEW_SR;
	a.frame = frame;
	return a;
}

// Run `seconds` of audio through the module with its inputs as they are.
// `frame` carries across calls so a module counting frames sees time pass.
inline void previewRun(rack::engine::Module& m, float seconds, int64_t& frame) {
	rack::engine::Module::ProcessArgs a = previewArgs();
	int n = (int)(seconds * PREVIEW_SR);
	for (int i = 0; i < n; i++) { a.frame = frame++; m.process(a); }
}

// Patch a cable into an input of a module that is not in the engine.
// Port::setChannels() is a no-op on an unconnected port, so the field is set.
inline void previewConnect(rack::engine::Port& p, int channels) {
	p.channels = (uint8_t)channels;
}

} // namespace sfs

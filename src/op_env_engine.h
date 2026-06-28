#pragma once
#include <cstdint>

// Rack-free wrapper that turns a DX7 voice's carrier envelope into a standalone
// gate-driven CV envelope (the OP ENV module). Kept in its own TU so the msfa
// headers don't collide with Rack's globals (same reason as bell_voice).
class OpEnvEngine {
public:
	static const int BLOCK = 64;

	OpEnvEngine();
	~OpEnvEngine();

	void setSampleRate(double sr);

	// Bank management — same DX7 .syx handling as the Operator module.
	bool loadBank(const uint8_t* data, int len);
	void getBank(uint8_t out[4096]) const;
	void setBankRaw(const uint8_t data[4096]);
	void setVoice(int idx);                 // 0..31
	int  voice() const;
	void getVoiceName(char out[11]) const;

	// Per-attribute offsets (added to the voice's carrier EG, clamped 0..99).
	void setOffsets(const int rateOff[4], const int levelOff[4]);

	// DX7 global LFO (tremolo). Offsets on the voice's rate/delay/depth and the
	// carrier's AM sensitivity; waveOverride < 0 = use the voice's wave.
	void setLfo(int rateOff, int delayOff, int depthOff, int amSensOff, int waveOverride);

	// V/oct for rate scaling (key-tracked envelope rates). connected=false → none.
	void setNote(int midinote, bool connected);

	// When true (default), gate-off releases toward 0V instead of the voice's
	// DX7 L4 level (which can leave the output stuck high).
	void setReleaseToZero(bool v);

	void gate(bool down);                    // rising edge re-triggers the attack
	void renderBlock();                      // advance the envelope one BLOCK
	float level() const;                     // amplitude 0..1 (EG x tremolo)

	// Static EG shape of the current (voice + offsets) envelope, for the display.
	void renderEnvShape(float* out, int n) const;
	// Amplitude (0..1, peak-normalized) of each resolved level L1..L4, for the
	// on-screen guide lines.
	void getLevelAmps(float out[4]) const;
	// Fraction (0..1) along the rendered shape where release (key-off) begins.
	float releaseFraction() const;

	struct Impl;   // defined in the .cpp; public so file-local helpers can use it

private:
	Impl* p_;
	OpEnvEngine(const OpEnvEngine&) = delete;
	OpEnvEngine& operator=(const OpEnvEngine&) = delete;
};

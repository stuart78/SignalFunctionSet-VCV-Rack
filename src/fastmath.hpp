#pragma once
// =============================================================================
// Cheap maths for the audio thread (desktop). The MetaModule port has the same
// helpers in its src/mm-fastmath.hpp; they are brought over as-is so the two
// builds share one set, and here the macros are ALWAYS the fast versions.
//
// Why it matters on desktop too: tools/perf (2026-09-25) found libm calls --
// sin, exp, exp2, pow, tan, tanh, much of it double-precision -- taking 50-85%
// of the time in eight modules, called per voice per sample for values that
// either only move with a knob (use Memo, exact) or are an oscillator, a
// decay, a pan law or a saturator (use these, and check the render with
// tools/perf/compare.py).
//
//  - sfs::Memo recomputes a function of a slowly-changing input only when the
//    input changes. Exact: a knob that is not moving costs one compare.
//  - SFS_SIN2PI / SFS_COS2PI (|error| < 4e-6, -108 dB), SFS_EXP2 (2e-6
//    relative, 0.003 cents: fine for pitch), SFS_EXP, SFS_TANH (2e-4),
//    SFS_POWABS / SFS_LOG2 (1e-4 relative, for shapes, not pitch).
// =============================================================================
#include <cmath>
#include <cstdint>

namespace sfs {

struct Memo {
	float in = NAN, out = 0.f;
	template <class F>
	float operator()(float x, F f) {
		if (x != in) { in = x; out = f(x); }
		return out;
	}
};
// The same, keyed on two inputs (typically a knob and the sample time).
struct Memo2 {
	float a = NAN, b = NAN, out = 0.f;
	template <class F>
	float operator()(float x, float y, F f) {
		if (x != a || y != b) { a = x; b = y; out = f(x, y); }
		return out;
	}
};

// sin(2πp) for any p. Range-reduced to a quarter wave, then odd Taylor terms
// to x^11: |error| < 4e-6 (-108 dB), good enough for an audible partial.
inline float fastSin2Pi(float p) {
	p -= std::floor(p);                          // [0, 1)
	float x = p < 0.5f ? p : p - 0.5f;           // half wave
	float sign = p < 0.5f ? 1.f : -1.f;
	if (x > 0.25f) x = 0.5f - x;                 // quarter wave, [0, 0.25]
	float t = x * 6.28318530718f, t2 = t * t;
	float y = t * (1.f + t2 * (-1.f/6 + t2 * (1.f/120 + t2 * (-1.f/5040
	          + t2 * (1.f/362880 + t2 * (-1.f/39916800))))));
	return sign * y;
}
inline float fastCos2Pi(float p) { return fastSin2Pi(p + 0.25f); }

// log2/exp2 for SHAPES (window curves, envelope bends), not for pitch:
// |error| ~ 1e-4 relative. Bit-level log2 with an atanh series on the mantissa, and a
// quintic exp2 on the fraction.
inline float fastLog2(float x) {
	union { float f; uint32_t i; } u{x};
	float e = (float)(int)((u.i >> 23) & 255) - 127.f;
	u.i = (u.i & 0x007FFFFF) | 0x3F800000;        // mantissa in [1, 2)
	float m = u.f;
	// log2(m) = 2/ln2 * atanh(z), z = (m-1)/(m+1) in [0, 1/3]
	float z = (m - 1.f) / (m + 1.f), z2 = z * z;
	return e + 2.88539008f * z * (1.f + z2 * (1.f/3 + z2 * (1.f/5 + z2 * (1.f/7 + z2 * (1.f/9)))));
}
inline float fastExp2(float x) {
	if (x < -126.f) return 0.f;
	if (x > 127.f) x = 127.f;
	float fl = std::floor(x), f = x - fl;
	// Taylor to f^7: |relative error| < 2e-6, i.e. 0.003 cents -- close enough
	// to use for pitch, which is what SFS_EXP2 is for.
	float p = 1.f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f
	        + f * (0.00961813f + f * (0.00133336f + f * (0.00015404f + f * 0.00001525f))))));
	union { uint32_t i; float f; } u{(uint32_t)((int)fl + 127) << 23};
	return u.f * p;
}
// e^x for envelopes and one-pole coefficients (same accuracy as fastExp2).
inline float fastExp(float x) { return fastExp2(x * 1.44269504f); }
// |x|^p for shape curves (x any sign, p > 0).
inline float fastPowAbs(float x, float p) {
	float a = std::fabs(x);
	return a <= 0.f ? 0.f : fastExp2(p * fastLog2(a));
}

// tanh, rational (Lambert continued fraction), clamped: |error| < 2e-4.
inline float fastTanh(float x) {
	if (x > 4.97f) return 1.f;
	if (x < -4.97f) return -1.f;
	float x2 = x * x;
	return x * (135135.f + x2 * (17325.f + x2 * (378.f + x2)))
	         / (135135.f + x2 * (62370.f + x2 * (3150.f + x2 * 28.f)));
}

// Noise for exciters and texture. random::normal() is Box-Muller -- a log, a
// sqrt, a sin and a cos per sample. Three xorshift uniforms summed have the
// same variance and are Gaussian enough for anything that gets filtered.
struct FastNoise {
	uint32_t s = 0x9E3779B9u;
	inline uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
	inline float uniform() { return (float)(next() >> 8) * (1.f / 16777216.f); }   // [0, 1)
	inline float normal() {
		return (uniform() + uniform() + uniform() - 1.5f) * 2.f;   // mean 0, var 1
	}
};

} // namespace sfs


#define SFS_SIN2PI(p) sfs::fastSin2Pi(p)
#define SFS_COS2PI(p) sfs::fastCos2Pi(p)
#define SFS_TANH(x)   sfs::fastTanh(x)
#define SFS_POWABS(x, p) sfs::fastPowAbs(x, p)
#define SFS_EXP(x)    sfs::fastExp(x)
#define SFS_EXP2(x)   sfs::fastExp2(x)
#define SFS_LOG2(x)   sfs::fastLog2(x)

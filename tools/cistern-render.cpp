// Run a WAV through Cistern's engine, offline (no Rack).
//
//   clang++ -O2 -std=c++17 -Isrc tools/cistern-render.cpp -o /tmp/cistern-render
//   /tmp/cistern-render in.wav out.wav [--space cistern] [--mix 0.5] [--scatter x]
//                       [--tail 30] [--decay 1] [--wet] [--impulse]
// (run from the repository: scans load from res/spaces). --impulse ignores the
// input and renders the room's impulse response. Two sources, one per input
// channel, placed as the module places them by default.
//
// The engine is built at the file's own sample rate. --mix is the wet share
// after the wet signal has been matched to the dry's RMS (a 45 s tail summed
// raw is far louder than its source); --wet writes the reverb alone. The result
// is peak-normalised to -1 dBFS only if it would clip. 32-bit float out.
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "cistern-engine.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
using namespace cistern;

int main(int argc, char** argv) {
	if (argc < 3) { std::fprintf(stderr, "usage: %s in.wav out.wav [--mix 0.5] [--scatter x] [--tail 30] [--decay 1] [--wet]\n", argv[0]); return 2; }
	double mix = 0.5, tail = 30, decay = 1, scatter = 0.5; bool wetOnly = false, impulse = false;   // scatter as the panel defaults it
	std::string slug = "cistern";
	for (int i = 3; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::atof(argv[++i]) : 0.0; };
		if (a == "--mix") mix = next(); else if (a == "--tail") tail = next(); else if (a == "--decay") decay = next();
		else if (a == "--scatter") scatter = next(); else if (a == "--wet") wetOnly = true;
		else if (a == "--impulse") impulse = true;
		else if (a == "--space" && i + 1 < argc) slug = argv[++i];
		else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
	}
	unsigned ch = 0, sr = 0; drwav_uint64 frames = 0;
	float* in = drwav_open_file_and_read_pcm_frames_f32(argv[1], &ch, &sr, &frames, nullptr);
	if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
	std::printf("%s: %u ch, %u Hz, %.2f s\n", argv[1], ch, sr, frames / (double)sr);

	int id = -1;
	for (int k = 0; k < NSPACES; k++) if (slug == SPACES[k].slug) id = k;
	if (id < 0) { std::fprintf(stderr, "no space %s\n", slug.c_str()); return 2; }
	Space s; std::string err;
	if (!makeSpace(id, "res/spaces", s, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
	BuildParams bp; bp.scatter = scatter;
	V3 face = s.src - s.lis; face.y = 0; face = face.norm();
	V3 side(face.z, 0, -face.x), src[2] = {s.src, s.src};
	for (double half : {1.5, 0.8, 0.3}) {
		V3 a = s.src - side * half, b = s.src + side * half;
		if (s.inside(a) && s.inside(b)) { src[0] = a; src[1] = b; break; }
	}
	auto T = trace(s, bp, sr, src, 2, s.lis);
	auto P = assemble(*T, decay);
	if (impulse) { for (size_t i = 0; i < frames * ch; i++) in[i] = 0; in[0] = 1; if (ch > 1) in[1] = 1; wetOnly = true; }
	std::printf("%s, scatter %.2f, T60 125 Hz %.1f s, 1 kHz %.1f s, 4 kHz %.1f s\n", s.name,
		scatter >= 0 ? scatter : s.rough, P->t60[0], P->t60[3], P->t60[5]);
	Reverb rv; rv.setProgram(P.get(), 1.0, 7);

	size_t n = (size_t)frames + (size_t)(tail * sr);
	std::vector<float> dl(n, 0.f), dr(n, 0.f), wl(n), wr(n);
	for (size_t i = 0; i < frames; i++) { dl[i] = in[i * ch]; dr[i] = in[i * ch + (ch > 1 ? 1 : 0)]; }
	drwav_free(in, nullptr);
	auto t0 = std::chrono::steady_clock::now();
	for (size_t i = 0; i < n; i++) rv.process(dl[i], dr[i], wl[i], wr[i]);
	double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

	double ed = 0, ew = 0;
	for (size_t i = 0; i < n; i++) { ed += dl[i] * dl[i] + dr[i] * dr[i]; ew += wl[i] * wl[i] + wr[i] * wr[i]; }
	double wg = ew > 0 ? std::sqrt(ed / ew) : 0;            // both measured over the whole render
	std::vector<float> out(n * 2);
	float pk = 0;
	for (size_t i = 0; i < n; i++) {
		double l = wetOnly ? wg * wl[i] : (1 - mix) * dl[i] + mix * wg * wl[i];
		double r = wetOnly ? wg * wr[i] : (1 - mix) * dr[i] + mix * wg * wr[i];
		out[2 * i] = (float)l; out[2 * i + 1] = (float)r;
		pk = std::max(pk, (float)std::max(std::fabs(l), std::fabs(r)));
	}
	if (pk > 0.891f) for (float& v : out) v *= 0.891f / pk;

	drwav_data_format fmt = {}; fmt.container = drwav_container_riff; fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
	fmt.channels = 2; fmt.sampleRate = sr; fmt.bitsPerSample = 32;
	drwav w;
	if (!drwav_init_file_write(&w, argv[2], &fmt, nullptr)) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
	drwav_write_pcm_frames(&w, n, out.data());
	drwav_uninit(&w);
	std::printf("wrote %s (%.1f s, rendered in %.1f s)\n", argv[2], n / (double)sr, sec);
	return 0;
}

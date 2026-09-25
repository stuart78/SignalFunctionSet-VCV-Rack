// Kit CPU benchmark: the real module compiled against libRack, timed idle, with
// one drum ringing, and with all eight struck every 250 ms (a busy kit; the
// default kit has two hats). Best of three runs, since Apple Silicon moves a
// busy process between core types.
//
//   L="/Applications/VCV Rack 2 Pro.app/Contents/Resources"
//   clang++ -std=c++11 -O3 -funsafe-math-optimizations -DARCH_MAC -I ../Rack-SDK/include \
//     -I ../Rack-SDK/dep/include tools/kit-bench.cpp -o /tmp/kb -L"$L" -lRack
//   DYLD_LIBRARY_PATH="$L" /tmp/kb [seconds]
#include "../src/kit.cpp"
Plugin* pluginInstance = nullptr;
Model* modelPolyKitIn = nullptr;
#include <chrono>
#include <cstdio>
#include <cstdlib>
static double run(Kit& k, int pattern, double secs) {
	Module::ProcessArgs a; a.sampleRate = 48000.f; a.sampleTime = 1.f / 48000.f;
	int N = (int)(secs * 48000);
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < N; i++) {
		// pattern 0 idle, 1 one drum every 500 ms, 2 all eight every 250 ms (a busy kit)
		bool g = pattern == 1 ? (i % 24000) < 480 : pattern == 2 ? (i % 12000) < 480 : false;
		for (int c = 0; c < 8; c++) k.inputs[Kit::GATE_INPUT].setVoltage((g && (pattern == 2 || c == 0)) ? 10.f : 0.f, c);
		k.process(a);
	}
	return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / N;
}
int main(int argc, char** argv) {
	double secs = argc > 1 ? atof(argv[1]) : 3.0;
	int only = argc > 2 ? atoi(argv[2]) : -1;
	const char* nm[4] = {"idle", "one drum", "all eight", "all eight, stereo off"};
	for (int p = 0; p < 4; p++) {
		if (only >= 0 && p != only) continue;
		Kit k; k.inputs[Kit::GATE_INPUT].channels = 8; k.outputs[Kit::OUT_OUTPUT].channels = 16;
		if (p == 3) k.stereo = false;
		run(k, p == 3 ? 2 : p, 0.5);                   // warm
		double best = 1e9;
		for (int r = 0; r < 3; r++) best = std::min(best, run(k, p == 3 ? 2 : p, secs));
		printf("%-24s %6.3f us/sample  = %5.1f%% of one core at 48 kHz\n", nm[p], best, best * 4.8);
	}
}

// Drives the REAL Diapason -- src/diapason.cpp compiled as-is against libRack --
// and checks what docs/diapason-design.md says the module does: each division's
// poly V/OCT + GATE plays that division, a pulse shorter than a block still
// speaks, a V/OCT that moves under a held gate changes key, the split sends low
// GREAT notes to the Pedal, the pistons draw their registration when the
// resolved piston changes (knob, CV, NEXT) and SET stores one, a saved patch
// keeps its hand registration on load, and the cost of a full organ.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/diapason-harness.cpp -o /tmp/diapason-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/diapason-harness
#include "../src/diapason.cpp"
#include <chrono>
#include <cstdio>
#include <thread>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) {
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) fails++;
}

struct Rig {
	Diapason m;
	rack::engine::Module::ProcessArgs a;
	int64_t frame = 0;
	double sumSq = 0; int nSq = 0;
	Rig() {
		// toJson() names the plugin and the model, so give it both
		static rack::plugin::Plugin plug;
		plug.slug = "SignalFunctionSet";
		plug.path = ".";                 // the room's scans load from res/spaces
		pluginInstance = &plug;
		modelDiapason->plugin = &plug;
		m.model = modelDiapason;
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		for (int i = 0; i < Diapason::INPUTS_LEN; i++) sfs::previewConnect(m.inputs[i], 1);
		for (int o = 0; o < Diapason::OUTPUTS_LEN; o++) sfs::previewConnect(m.outputs[o], 1);
		for (int i = 0; i < Diapason::INPUTS_LEN; i++) m.inputs[i].setVoltage(0.f);
		m.params[Diapason::ROOM_PARAM].setValue(0.f);   // dry unless a check asks for a room
		run(64);
	}
	void run(int n) {
		for (int i = 0; i < n; i++) {
			a.frame = frame++;
			m.process(a);
			float l = m.outputs[Diapason::LEFT_OUTPUT].getVoltage(), r = m.outputs[Diapason::RIGHT_OUTPUT].getVoltage();
			sumSq += l * l + r * r; nSq++;
		}
	}
	float level(int n) { sumSq = 0; nSq = 0; run(n); return (float)std::sqrt(sumSq / std::max(1, nSq)); }
	void only(std::initializer_list<int> stops) {
		for (int s = 0; s < NSTOP; s++) m.params[Diapason::STOP_PARAM + s].setValue(0.f);
		for (int c = 0; c < NCOUP; c++) m.params[Diapason::COUPLER_PARAM + c].setValue(0.f);
		m.params[Diapason::TREM_PARAM].setValue(0.f);
		for (int s : stops) m.params[Diapason::STOP_PARAM + s].setValue(1.f);
	}
	void notes(int div, std::initializer_list<float> v) {
		int vo = Diapason::GREAT_VOCT_INPUT + 2 * div, ga = vo + 1;
		sfs::previewConnect(m.inputs[vo], (int)v.size());
		sfs::previewConnect(m.inputs[ga], (int)v.size());
		int c = 0;
		for (float x : v) { m.inputs[vo].setVoltage(x, c); m.inputs[ga].setVoltage(10.f, c); c++; }
	}
	void release(int div) {
		int ga = Diapason::GREAT_GATE_INPUT + 2 * div;
		for (int c = 0; c < 16; c++) m.inputs[ga].setVoltage(0.f, c);
	}
	bool reg(int s) { return m.params[Diapason::STOP_PARAM + s].getValue() > 0.5f; }
	// ROOM on, and wait (in real time: the worker traces on its own thread) for it
	bool room(int r) {
		m.params[Diapason::ROOM_PARAM].setValue((float)r);
		for (int i = 0; i < 400; i++) {
			run(64);
			if (m.rv && m.rvRoom == r) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}
};

// The Full organ preset's stops on a chord progression, through the module
// (and its room), written as a stereo float WAV.
static void renderProgression(const std::string& path, int room) {
	Rig r;
	if (room) r.room(room);
	r.only({GT_BOURDON16, GT_PRINCIPAL8, GT_ROHR8, GT_OCTAVE4, GT_FIFTEENTH2, GT_MIXTURE, GT_TRUMPET8,
	        SW_GEDACKT8, SW_GAMBA8, SW_PRINCIPAL4, SW_OBOE8, SW_TROMPETTE8,
	        PD_BOURDON32, PD_SUBBASS16, PD_OCTAVE8, PD_POSAUNE16});
	for (int c = 0; c < NCOUP; c++) r.m.params[Diapason::COUPLER_PARAM + c].setValue(1.f);
	r.m.params[Diapason::WIND_PARAM].setValue(0.5f);
	r.m.params[Diapason::LEVEL_PARAM].setValue(-6.f);
	std::vector<float> L, R;
	auto go = [&](float sec) {
		for (int i = 0; i < (int)(sec * SR); i++) {
			r.run(1);
			L.push_back(r.m.outputs[0].getVoltage() / 10.f); R.push_back(r.m.outputs[1].getVoltage() / 10.f);
		}
	};
	const int prog[4][4] = {{48, 55, 60, 64}, {45, 52, 57, 60}, {41, 53, 57, 60}, {43, 50, 55, 59}};
	const int bass[4] = {48, 45, 41, 43};
	for (int bar = 0; bar < 4; bar++) {
		std::vector<float> v;
		r.notes(GREAT, {(prog[bar][0] - 48) / 12.f, (prog[bar][1] - 48) / 12.f, (prog[bar][2] - 48) / 12.f, (prog[bar][3] - 48) / 12.f});
		r.notes(PEDAL, {(bass[bar] - 72) / 12.f});
		go(1.9f);
		r.release(GREAT); r.release(PEDAL);
		go(0.1f);
	}
	go(4.f);
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) return;
	int n = (int)L.size(), bytes = n * 8;
	auto w32 = [&](int v) { fwrite(&v, 4, 1, f); };
	auto w16 = [&](short v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(3); w16(2);
	w32((int)SR); w32((int)SR * 8); w16(8); w16(32); fwrite("data", 1, 4, f); w32(bytes);
	for (int i = 0; i < n; i++) { fwrite(&L[i], 4, 1, f); fwrite(&R[i], 4, 1, f); }
	fclose(f);
	printf("wrote %s\n", path.c_str());
}

int main(int argc, char** argv) {
	// saving a module reads its parameters through the engine
	rack::contextSet(new rack::Context);
	APP->engine = new rack::engine::Engine;
	APP->engine->setSampleRate(SR);
	if (argc > 2 && std::string(argv[1]) == "render") {
		std::string dir = argv[2];
		renderProgression(dir + "/module-full-organ-dry.wav", 0);
		renderProgression(dir + "/module-full-organ-old-church.wav", 1);
		renderProgression(dir + "/module-full-organ-pantheon.wav", 5);
		return 0;
	}
	printf("== divisions ==\n");
	{
		Rig r; r.only({GT_PRINCIPAL8});
		float q = r.level(4800);
		r.notes(GREAT, {0.f, 4.f / 12.f, 7.f / 12.f});
		r.run(24000);
		check(q < 1e-4f && r.m.eng.nActive == 3 && r.level(4800) > 0.1f, "three GREAT channels sound three Principal pipes");
		r.notes(SWELL, {0.f});
		r.run(4800);
		check(r.m.eng.nActive == 3, "the Swell keyboard plays nothing while only a Great stop is drawn");
		r.m.params[Diapason::COUPLER_PARAM + SW_GT].setValue(1.f);
		r.m.params[Diapason::STOP_PARAM + SW_GEDACKT8].setValue(1.f);
		r.run(4800);
		check(r.m.eng.nActive == 3 + 3, "drawing Gedackt with Swell to Great adds a pipe for each held Great key (the Swell's C4 is one of them)");
		r.release(GREAT); r.release(SWELL);
		r.run(48000);
		check(r.m.eng.nActive == 0, "released, every pipe dies away and is freed");
	}
	{
		Rig r; r.only({PD_SUBBASS16});
		r.notes(PEDAL, {-2.f});
		r.run(24000);
		check(r.m.eng.nActive == 1 && r.m.eng.pipes[r.m.eng.active[0]].key == 36, "PEDAL at -2 V plays the bottom C of the pedal");
		r.notes(PEDAL, {3.f});
		r.run(4800);
		int k = r.m.eng.nActive ? r.m.eng.pipes[r.m.eng.active[0]].key : -1;
		for (int i = 0; i < r.m.eng.nActive; i++) if (r.m.eng.pipes[r.m.eng.active[i]].on) k = r.m.eng.pipes[r.m.eng.active[i]].key;
		check(k == 60, "PEDAL at +3 V (C7) is folded into the pedal compass, to C4");
	}
	printf("== keys ==\n");
	{
		Rig q; q.only({GT_ROHR8});
		int seen = 0;
		q.notes(GREAT, {0.f}); q.run(3); q.release(GREAT);
		for (int i = 0; i < 2000; i++) { q.run(1); seen = std::max(seen, q.m.eng.nActive); }
		check(seen == 1, "a 3-sample gate still makes the pipe speak");
		Rig s; s.only({GT_ROHR8});
		s.notes(GREAT, {0.f}); s.run(9600);
		s.m.inputs[Diapason::GREAT_VOCT_INPUT].setVoltage(2.f / 12.f, 0);
		s.run(9600);
		int held = 0;
		for (int i = 0; i < s.m.eng.nActive; i++) if (s.m.eng.pipes[s.m.eng.active[i]].on) { held++; check(s.m.eng.pipes[s.m.eng.active[i]].key == 62, "the held pipe is D4"); }
		check(held == 1, "a V/OCT that moves under a held gate releases C4 and presses D4");
	}
	{
		Rig r; r.only({GT_PRINCIPAL8, PD_SUBBASS16});
		r.m.split = 55;
		r.notes(GREAT, {-1.f, 0.f});
		r.run(9600);
		bool ped = false, gt = false;
		for (int i = 0; i < r.m.eng.nActive; i++) {
			const Pipe& p = r.m.eng.pipes[r.m.eng.active[i]];
			if (p.div == PEDAL && p.key == 48) ped = true;
			if (p.div == GREAT && p.key == 60) gt = true;
		}
		check(ped && gt && r.m.eng.nActive == 2, "split below G3: C3 on GREAT plays the Pedal's Subbass, C4 the Great's Principal");
	}
	printf("== pistons ==\n");
	{
		Rig r;
		check(r.reg(GT_ROHR8) && r.reg(SW_GEDACKT8) && r.reg(PD_SUBBASS16) && !r.reg(GT_PRINCIPAL8),
		      "a new module has piston 1 drawn");
		r.m.params[Diapason::PISTON_PARAM].setValue(3.f);
		r.run(32);
		check(r.reg(GT_MIXTURE) && r.reg(GT_FIFTEENTH2) && !r.reg(GT_ROHR8), "the PISTON knob on 4 draws the principal chorus");
		r.m.params[Diapason::STOP_PARAM + GT_TRUMPET8].setValue(1.f);
		r.m.params[Diapason::SET_PARAM].setValue(1.f); r.run(32);
		r.m.params[Diapason::SET_PARAM].setValue(0.f); r.run(32);
		r.m.params[Diapason::PISTON_PARAM].setValue(0.f); r.run(32);
		check(!r.reg(GT_TRUMPET8), "back on piston 1, the Trumpet goes in");
		r.m.inputs[Diapason::PISTON_INPUT].setVoltage(3.f); r.run(32);
		check(r.reg(GT_TRUMPET8) && r.reg(GT_MIXTURE), "3 V on PISTON CV recalls piston 4, with the Trumpet SET added to it");
		r.m.inputs[Diapason::PISTON_INPUT].setVoltage(0.f); r.run(32);
		for (int i = 0; i < 2; i++) {
			r.m.inputs[Diapason::NEXT_INPUT].setVoltage(10.f); r.run(32);
			r.m.inputs[Diapason::NEXT_INPUT].setVoltage(0.f); r.run(32);
		}
		check(r.m.lastPiston == 2 && r.reg(GT_PRINCIPAL8) && r.reg(GT_OCTAVE4) && !r.reg(GT_MIXTURE), "two NEXT triggers step to piston 3");
		// a hand registration survives a save and a load
		r.m.params[Diapason::STOP_PARAM + SW_OBOE8].setValue(1.f);
		r.run(32);
		// as Rack loads a patch: the parameters, then the module's own data
		json_t* jp = r.m.paramsToJson();
		json_t* jd = r.m.dataToJson();
		Rig q;
		q.m.paramsFromJson(jp);
		q.m.dataFromJson(jd);
		json_decref(jp); json_decref(jd);
		q.m.inputs[Diapason::NEXT_INPUT].setVoltage(0.f);
		q.run(64);
		check(q.reg(SW_OBOE8) && q.reg(GT_TRUMPET8) == false && q.m.lastPiston == 2, "a loaded patch keeps the stops drawn by hand and does not redraw its piston");
		q.m.params[Diapason::PISTON_PARAM].setValue(1.f); q.run(32);   // with two NEXTs counted: piston 4
		check(q.reg(GT_TRUMPET8), "and keeps the pistons SET into it");
	}
	printf("== presets ==\n");
	{
		json_error_t err;
		json_t* j = json_load_file("presets/Diapason/Trumpet tune.vcvm", 0, &err);
		check(j != nullptr, "presets/Diapason/Trumpet tune.vcvm reads");
		if (j) {
			Rig r;
			r.m.inputs[Diapason::PISTON_INPUT].setVoltage(3.f);
			r.run(64);
			r.m.paramsFromJson(json_object_get(j, "params"));
			r.m.dataFromJson(json_object_get(j, "data"));
			json_decref(j);
			r.run(64);
			check(r.reg(GT_TRUMPET8) && r.reg(SW_PRINCIPAL4) && !r.reg(GT_MIXTURE),
			      "loaded onto a module with 3 V on PISTON CV, it keeps its own stops");
		}
	}
	printf("== the room ==\n");
	{
		// the shipped decay table against the engine's own measurement
		bool same = true;
		for (int r = 0; r < DP_NROOM; r++) {
			std::shared_ptr<const cistern::Space> sp = dpSpace(dpRoomSpace(r));
			cistern::Program P, D;
			cistern::BuildParams bp;
			cistern::roomDecay(*sp, bp, sp->src, P);
			dpRoomDecay(r, D);
			for (int b = 0; b < cistern::NB; b++) same = same && std::fabs(P.t60[b] - D.t60[b]) <= 1e-9 * P.t60[b];
			same = same && std::fabs(P.t60x8k - D.t60x8k) <= 1e-9 * P.t60x8k && std::fabs(P.mfp - D.mfp) <= 1e-9 * P.mfp;
		}
		check(same, "DP_ROOM_DECAY matches the engine's measurement of every room (else rerun tools/diapason-rooms.cpp)");
		// how soon a room comes in, and that the full trace follows it
		Rig r; r.only({GT_PRINCIPAL8});
		auto t0 = std::chrono::steady_clock::now();
		r.m.params[Diapason::ROOM_PARAM].setValue(1.f);
		double tIn = -1, tFull = -1;
		size_t quickTaps = 0;
		for (int i = 0; i < 800 && tFull < 0; i++) {
			r.run(64);
			double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			if (r.m.rv && tIn < 0) { tIn = t; quickTaps = r.m.rv->cur->packed[0].size() + r.m.rv->cur->packed[1].size(); }
			if (r.m.rv && tIn >= 0 && r.m.rv->cur->packed[0].size() + r.m.rv->cur->packed[1].size() > quickTaps) tFull = t;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		check(tIn >= 0 && tIn < 0.5, string::f("the Old Church comes in %.0f ms after ROOM is turned on", tIn * 1000));
		check(tFull > tIn, string::f("and its full trace is cross-faded in %.1f s later", tFull - tIn));
	}
	{
		float tail[2], held[2];
		for (int k = 0; k < 2; k++) {
			Rig r; r.only({GT_PRINCIPAL8, GT_OCTAVE4});
			if (k) check(r.room(1), "the Old Church is traced and handed over within four seconds");
			r.notes(GREAT, {0.f, 4.f / 12.f, 7.f / 12.f});
			r.run(48000);
			held[k] = r.level(9600);
			r.release(GREAT);
			r.run(24000);
			tail[k] = r.level(4800);    // half a second after the keys come up
		}
		float dry = 20.f * std::log10(tail[0] / held[0] + 1e-9f), wet = 20.f * std::log10(tail[1] / held[1] + 1e-9f);
		check(wet > -40.f && wet > dry + 20.f, string::f("half a second after the keys: %.0f dB in the Old Church, %.0f dB dry", wet, dry));
		check(std::fabs(20.f * std::log10(held[1] / held[0])) < 6.f, string::f("the room keeps the level: %.1f dB against dry", 20.f * std::log10(held[1] / held[0])));
		Rig s; s.only({GT_PRINCIPAL8});
		s.room(1);
		s.notes(GREAT, {0.f}); s.run(24000);
		bool ok = s.room(5);
		float pk = 0;
		for (int i = 0; i < 48000; i++) { s.run(1); pk = std::max(pk, std::fabs(s.m.outputs[0].getVoltage())); }
		check(ok && pk < 10.f && std::isfinite(pk), "ROOM moved to the Pantheon while a note sounds: the new room takes over, nothing blows up");
		s.m.params[Diapason::ROOM_PARAM].setValue(0.f); s.run(4800);
		check(!s.m.roomOn(), "ROOM at 0 plays the organ dry");
	}
	printf("== level and cost ==\n");
	{
		Rig r;
		r.room(1);
		for (int s = 0; s < NSTOP; s++) r.m.params[Diapason::STOP_PARAM + s].setValue(1.f);
		for (int c = 0; c < NCOUP; c++) r.m.params[Diapason::COUPLER_PARAM + c].setValue(1.f);
		r.m.params[Diapason::PISTON_PARAM].setValue(0.f);
		r.notes(GREAT, {0.f, 4.f / 12.f, 7.f / 12.f, 1.f});
		r.notes(SWELL, {-5.f / 12.f, -1.f / 12.f, 2.f / 12.f, 7.f / 12.f});
		r.notes(PEDAL, {-2.f});
		r.run(24000);
		auto t0 = std::chrono::steady_clock::now();
		float lv = r.level(48000 * 4);
		double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		float pk = 0;
		for (int i = 0; i < 4800; i++) { r.run(1); pk = std::max(pk, std::fabs(r.m.outputs[0].getVoltage())); }
		printf("    full organ in the Old Church: %d pipes, %.2f V rms, peak %.2f V, %.1f%% of one core\n", r.m.eng.nActive, lv / 1.4142f, pk, 100.0 * sec / 4.0);
		check(pk < 10.f, "a full organ stays inside 10 V");
		check(sec / 4.0 < 0.10, "and costs under 10% of a core");
		// what each room adds, playing a chord continuously
		for (int room = 0; room <= DP_NROOM; room++) {
			Rig c; c.only({GT_PRINCIPAL8, GT_OCTAVE4});
			if (room) c.room(room);
			c.notes(GREAT, {0.f, 4.f / 12.f, 7.f / 12.f});
			c.run(24000);
			auto t1 = std::chrono::steady_clock::now();
			c.run(96000);
			double s2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
			printf("    %-28s %.1f%% of one core (a chord on two stops)\n", room ? dpRoomName(room - 1).c_str() : "dry", 100.0 * s2 / 2.0);
		}
		Rig q; q.only({GT_PRINCIPAL8});
		q.notes(GREAT, {0.f}); q.run(24000);
		printf("    one Principal 8' note: %.2f V rms\n", q.level(24000) / 1.4142f);
	}
	printf(fails ? "FAILED (%d failures)\n" : "all passed (0 failures)\n", fails);
	return fails ? 1 : 0;
}

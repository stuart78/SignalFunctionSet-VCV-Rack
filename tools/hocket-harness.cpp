// Drives the REAL Hocket -- src/hocket.cpp compiled as-is against libRack -- and
// checks what docs/hocket-design.md says must be true: every pattern deals the
// channels in the order its name says, COUNT and the ON buttons choose which
// channels are in play, GROUP and STAY hold a channel, each channel holds its
// pitch after its note, the gate only ever reaches the selected channel, TRIG
// takes over the advance, and the audio switch fades.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/hocket-harness.cpp -o /tmp/hocket-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/hocket-harness
#include "../src/hocket.cpp"
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) {
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) fails++;
}

struct Rig {
	Hocket m;
	rack::engine::Module::ProcessArgs a;
	int64_t frame = 0;
	Rig() {
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		sfs::previewConnect(m.inputs[Hocket::CV_INPUT], 1);
		sfs::previewConnect(m.inputs[Hocket::GATE_INPUT], 1);
		for (int c = 0; c < HK_N; c++) {
			sfs::previewConnect(m.outputs[Hocket::GATE_OUTPUT + c], 1);
		}
		// a Rack trigger read high on its first sample does not fire, so every
		// input starts low, as it does in a patch
		run(16);
	}
	void run(int n) { for (int i = 0; i < n; i++) { a.frame = frame++; m.process(a); } }
	// which channels' gates are high
	unsigned gates() {
		unsigned g = 0;
		for (int c = 0; c < HK_N; c++) if (m.outputs[Hocket::GATE_OUTPUT + c].getVoltage() > 5.f) g |= 1u << c;
		return g;
	}
	// one note: gate high for 10 ms at pitch v, then low for 10 ms. Returns the
	// channel that took it (-1 if none, -2 if more than one gate was high).
	int note(float v = 0.f) {
		m.inputs[Hocket::CV_INPUT].setVoltage(v);
		m.inputs[Hocket::GATE_INPUT].setVoltage(10.f);
		run(240);
		unsigned g = gates();
		m.inputs[Hocket::GATE_INPUT].setVoltage(0.f);
		run(480);
		if (!g) return -1;
		if (g & (g - 1)) return -2;
		int c = 0; while (!(g & (1u << c))) c++;
		return c;
	}
	std::string deal(int n, float v = 0.f) {
		std::string s;
		for (int i = 0; i < n; i++) { int c = note(v); s += c >= 0 ? (char)('1' + c) : c == -1 ? '.' : '*'; }
		return s;
	}
	void pattern(int p) { m.params[Hocket::PATTERN_PARAM].setValue((float)p); }
};

int main() {
	rack::random::init();

	printf("== patterns over eight channels ==\n");
	{
		struct T { int p; const char* want; };
		static const T Ts[] = {
			{HK_UP,      "1234567812345678"},
			{HK_DOWN,    "8765432187654321"},
			{HK_PEND,    "1234567876543212"},
			{HK_STRIDE3, "1472583614725836"},
			{HK_STRIDE5, "1638527416385274"},
			{HK_CONV,    "1827364518273645"},
		};
		for (const T& t : Ts) {
			Rig r; r.pattern(t.p);
			std::string got = r.deal(16);
			check(got == t.want, string::f("%-18s %s", HK_PAT_NAME[t.p].c_str(), got.c_str()));
		}
	}

	printf("== the random patterns ==\n");
	{
		Rig r; r.pattern(HK_NOREP);
		std::string s = r.deal(2000);
		bool rep = false; int seen[8] = {};
		for (size_t i = 0; i < s.size(); i++) { if (i && s[i] == s[i - 1]) rep = true; if (s[i] >= '1' && s[i] <= '8') seen[s[i] - '1']++; }
		bool all = true; for (int c = 0; c < 8; c++) all &= seen[c] > 150;
		check(!rep, "Random, no repeat: never the same channel twice running over 2000 notes");
		check(all, "Random, no repeat: every channel used");
	}
	{
		Rig r; r.pattern(HK_SHUF);
		std::string s = r.deal(800);
		bool ok = true, rep = false;
		for (int b = 0; b < 100; b++) {
			unsigned seen = 0;
			for (int i = 0; i < 8; i++) seen |= 1u << (s[b * 8 + i] - '1');
			ok &= seen == 0xFF;
		}
		for (size_t i = 1; i < s.size(); i++) if (s[i] == s[i - 1]) rep = true;
		check(ok, "Shuffle: every block of eight notes is each channel once (" + s.substr(0, 24) + "...)");
		check(!rep, "Shuffle: a new cycle never starts on the channel that ended the last");
	}
	{
		Rig r; r.pattern(HK_DRUNK);
		std::string s = r.deal(1000);
		bool ok = true;
		for (size_t i = 1; i < s.size(); i++) ok &= std::abs(s[i] - s[i - 1]) == 1;
		check(ok, "Drunk: every step is to a neighbour (" + s.substr(0, 24) + "...)");
	}
	{
		Rig r; r.pattern(HK_RAND);
		std::string s = r.deal(2000);
		int rep = 0; for (size_t i = 1; i < s.size(); i++) rep += s[i] == s[i - 1];
		check(rep > 150 && rep < 350, string::f("Random: repeats about one note in eight (%d of 1999)", rep));
	}

	printf("== Register: pitch to channel ==\n");
	{
		Rig r; r.pattern(HK_REG);
		// two octaves up and down, so the range is known after the first pass
		std::string s;
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < 16; i++) { int c = r.note(i * 2.f / 15.f); if (pass) s += (char)('1' + c); }
		check(s == "1122334455667788", "a rising line over two octaves climbs the channels two notes at a time: " + s);
		bool mono = true; for (size_t i = 1; i < s.size(); i++) mono &= s[i] >= s[i - 1];
		check(mono && s.back() == '8', "and never goes back down, ending on 8");
	}

	printf("== which channels are in play ==\n");
	{
		Rig r; r.pattern(HK_UP);
		r.m.params[Hocket::ON_PARAM + 1].setValue(0.f);
		r.m.params[Hocket::COUNT_PARAM].setValue(3.f);
		std::string s = r.deal(9);
		check(s == "134134134", "COUNT 3 with channel 2 off: the first three that are on, 1 3 4: " + s);
		Rig q; q.pattern(HK_STRIDE3);
		q.m.params[Hocket::COUNT_PARAM].setValue(6.f);
		std::string t = q.deal(12);
		check(t == "142536142536", "Stride 3 over six moves on when its orbit closes, reaching all six: " + t);
		Rig z; z.pattern(HK_UP);
		for (int c = 0; c < 8; c++) z.m.params[Hocket::ON_PARAM + c].setValue(0.f);
		check(z.deal(4) == "....", "every channel off: no gate anywhere");
	}

	printf("== GROUP and STAY ==\n");
	{
		Rig r; r.pattern(HK_UP);
		r.m.params[Hocket::GROUP_PARAM].setValue(3.f);
		std::string s = r.deal(9);
		check(s == "111222333", "GROUP 3: three notes each: " + s);
		Rig q; q.pattern(HK_UP);
		q.m.params[Hocket::STAY_PARAM].setValue(1.f);
		std::string t = q.deal(6);
		check(t == "111111", "STAY 100%: never leaves: " + t);
		Rig h; h.pattern(HK_UP);
		h.m.params[Hocket::STAY_PARAM].setValue(0.5f);
		std::string u = h.deal(2000);
		int rep = 0; for (size_t i = 1; i < u.size(); i++) rep += u[i] == u[i - 1];
		check(rep > 900 && rep < 1100, string::f("STAY 50%%: about half the notes stay (%d of 1999)", rep));
	}

	printf("== the pitch lane ==\n");
	{
		Rig r; r.pattern(HK_UP);
		r.note(1.f); r.note(2.f); r.note(3.f);
		bool held = std::fabs(r.m.outputs[Hocket::CV_OUTPUT + 0].getVoltage() - 1.f) < 1e-6f
		         && std::fabs(r.m.outputs[Hocket::CV_OUTPUT + 1].getVoltage() - 2.f) < 1e-6f
		         && std::fabs(r.m.outputs[Hocket::CV_OUTPUT + 2].getVoltage() - 3.f) < 1e-6f;
		check(held, "each channel holds its own note's pitch after the line has moved on");
		// a glide under the held gate follows; after the gate it holds
		r.m.inputs[Hocket::CV_INPUT].setVoltage(0.5f);
		r.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f);
		r.run(100);
		r.m.inputs[Hocket::CV_INPUT].setVoltage(0.7f);
		r.run(100);
		float during = r.m.outputs[Hocket::CV_OUTPUT + 3].getVoltage();
		r.m.inputs[Hocket::GATE_INPUT].setVoltage(0.f);
		r.run(10);
		r.m.inputs[Hocket::CV_INPUT].setVoltage(-2.f);
		r.run(100);
		float after = r.m.outputs[Hocket::CV_OUTPUT + 3].getVoltage();
		check(std::fabs(during - 0.7f) < 1e-6f && std::fabs(after - 0.7f) < 1e-6f,
		      string::f("CV follows while the gate is held (%.2f) and holds after it (%.2f)", during, after));
		Rig q; q.pattern(HK_UP); q.m.cvTrack = false;
		q.m.inputs[Hocket::CV_INPUT].setVoltage(0.5f);
		q.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f);
		q.run(100);
		q.m.inputs[Hocket::CV_INPUT].setVoltage(0.7f);
		q.run(100);
		check(std::fabs(q.m.outputs[Hocket::CV_OUTPUT].getVoltage() - 0.5f) < 1e-6f, "with following off, the note keeps its starting pitch");
	}

	printf("== legato ==\n");
	{
		// a held gate with three pitches under it
		Rig r; r.pattern(HK_UP); r.m.legatoNotes = true;
		r.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f);
		std::string s;
		for (int i = 0; i < 3; i++) {
			r.m.inputs[Hocket::CV_INPUT].setVoltage(i * 0.25f);
			r.run(240);
			unsigned g = r.gates(); int c = 0; while (c < 8 && !(g & (1u << c))) c++;
			s += (char)('1' + c);
		}
		check(s == "123", "with the menu option, a new pitch under a held gate is a new note: " + s);
		Rig q; q.pattern(HK_UP);
		q.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f);
		q.m.inputs[Hocket::CV_INPUT].setVoltage(0.f); q.run(240);
		q.m.inputs[Hocket::CV_INPUT].setVoltage(0.5f); q.run(240);
		check(q.gates() == 1u, "without it, the held note stays where it is");
		// GROUP 2 under a held gate: the second note on the same channel gets a gap
		Rig g; g.pattern(HK_UP); g.m.legatoNotes = true;
		g.m.params[Hocket::GROUP_PARAM].setValue(2.f);
		g.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f);
		g.m.inputs[Hocket::CV_INPUT].setVoltage(0.f); g.run(240);
		g.m.inputs[Hocket::CV_INPUT].setVoltage(0.5f);
		int low = 0;
		for (int i = 0; i < 240; i++) { g.run(1); if (!(g.gates() & 1u)) low++; }
		check(low >= 40 && low <= 52, string::f("a second note on a held channel drops its gate for 1 ms (%d samples)", low));
	}

	printf("== TRIG ==\n");
	{
		Rig r; r.pattern(HK_UP);
		sfs::previewConnect(r.m.inputs[Hocket::TRIG_INPUT], 1);
		// one long held gate, four triggers under it
		r.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f);
		std::string s;
		for (int i = 0; i < 4; i++) {
			r.m.inputs[Hocket::TRIG_INPUT].setVoltage(10.f); r.run(48);
			r.m.inputs[Hocket::TRIG_INPUT].setVoltage(0.f); r.run(480);
			unsigned g = r.gates(); int c = 0; while (c < 8 && !(g & (1u << c))) c++;
			s += (char)('1' + c);
		}
		check(s == "1234", "TRIG moves the held gate along a channel per trigger: " + s);
		// GATE unpatched: normalled from TRIG
		Rig q; q.pattern(HK_UP);
		q.m.inputs[Hocket::GATE_INPUT].channels = 0;
		sfs::previewConnect(q.m.inputs[Hocket::TRIG_INPUT], 1);
		std::string t;
		for (int i = 0; i < 4; i++) {
			q.m.inputs[Hocket::TRIG_INPUT].setVoltage(10.f); q.run(48);
			unsigned g = q.gates(); int c = 0; while (c < 8 && !(g & (1u << c))) c++;
			t += c < 8 ? (char)('1' + c) : '.';
			q.m.inputs[Hocket::TRIG_INPUT].setVoltage(0.f); q.run(480);
		}
		check(t == "1234", "with GATE unpatched, each channel's gate is the trigger: " + t);
	}

	printf("== audio ==\n");
	{
		Rig r; r.pattern(HK_UP);
		r.m.params[Hocket::FADE_PARAM].setValue(std::log(20.f) / std::log(2000.f));   // 10 ms
		r.m.inputs[Hocket::AUDIO_L_INPUT].setVoltage(5.f);
		r.note(); r.run(1000);
		float a1 = r.m.outputs[Hocket::AUDIO_L_OUTPUT + 0].getVoltage();
		float a1r = r.m.outputs[Hocket::AUDIO_R_OUTPUT + 0].getVoltage();
		r.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f);
		r.run(240);    // 5 ms into a 10 ms fade
		float mid0 = r.m.outputs[Hocket::AUDIO_L_OUTPUT + 0].getVoltage();
		float mid1 = r.m.outputs[Hocket::AUDIO_L_OUTPUT + 1].getVoltage();
		float mid1r = r.m.outputs[Hocket::AUDIO_R_OUTPUT + 1].getVoltage();
		r.run(480);
		float end0 = r.m.outputs[Hocket::AUDIO_L_OUTPUT + 0].getVoltage();
		float end1 = r.m.outputs[Hocket::AUDIO_L_OUTPUT + 1].getVoltage();
		check(std::fabs(a1 - 5.f) < 1e-5f, "the selected channel passes the audio at unity");
		check(a1r == a1, "with IN R unpatched, R is a copy of L");
		check(std::fabs(mid0 - 2.5f) < 0.05f && std::fabs(mid1 - 2.5f) < 0.05f && mid1r == mid1,
		      string::f("halfway through a 10 ms fade both are at half (%.2f, %.2f), R with L", mid0, mid1));
		check(end0 == 0.f && std::fabs(end1 - 5.f) < 1e-5f, "after it, the old channel is silent and the new one at unity");
	}
	{
		Rig r; r.pattern(HK_UP);
		sfs::previewConnect(r.m.inputs[Hocket::AUDIO_R_INPUT], 1);
		r.m.inputs[Hocket::AUDIO_L_INPUT].setVoltage(3.f);
		r.m.inputs[Hocket::AUDIO_R_INPUT].setVoltage(-4.f);
		r.note(); r.note(); r.run(1000);
		bool ok = r.m.outputs[Hocket::AUDIO_L_OUTPUT + 1].getVoltage() == 3.f
		       && r.m.outputs[Hocket::AUDIO_R_OUTPUT + 1].getVoltage() == -4.f
		       && r.m.outputs[Hocket::AUDIO_L_OUTPUT + 0].getVoltage() == 0.f
		       && r.m.outputs[Hocket::AUDIO_R_OUTPUT + 0].getVoltage() == 0.f;
		check(ok, "with IN R patched, each side keeps its own signal, on the selected channel only");
	}

	printf("== RESET and switching off ==\n");
	{
		Rig r; r.pattern(HK_UP);
		r.deal(3);
		r.m.inputs[Hocket::RESET_INPUT].setVoltage(10.f); r.run(10); r.m.inputs[Hocket::RESET_INPUT].setVoltage(0.f);
		std::string s = r.deal(3);
		check(s == "123", "RESET: the next note starts from the top: " + s);
		// a reset during a held note leaves it sounding
		Rig q; q.pattern(HK_UP);
		q.deal(2);
		q.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f); q.run(240);
		q.m.inputs[Hocket::RESET_INPUT].setVoltage(10.f); q.run(240);
		check(q.gates() == (1u << 2), "and does not cut the note that is sounding");
		// switching off the selected channel lets go at once
		Rig z; z.pattern(HK_UP);
		z.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f); z.run(240);
		z.m.params[Hocket::ON_PARAM + 0].setValue(0.f); z.run(2);
		check(z.gates() == 0u, "switching off the selected channel drops its gate at once");
	}

	printf("\n== the gate passes whole ==\n");
	{
		// gates of very different lengths: the selected channel is high for
		// exactly as many samples as the input, and no other channel is
		static const int lens[] = {1, 48, 4800, 96000};
		for (int pat : {HK_UP, HK_RAND}) {
			Rig r; r.pattern(pat);
			bool ok = true; std::string why;
			for (int rep = 0; rep < 3; rep++) for (int len : lens) {
				r.m.inputs[Hocket::GATE_INPUT].setVoltage(10.f);
				int high = 0, stray = 0, ch = -1;
				for (int i = 0; i < len; i++) {
					r.run(1);
					unsigned g = r.gates();
					if (g && !(g & (g - 1))) { int c = 0; while (!(g & (1u << c))) c++; if (ch < 0) ch = c; if (c == ch) high++; else stray++; }
					else if (g) stray++;
				}
				r.m.inputs[Hocket::GATE_INPUT].setVoltage(0.f);
				r.run(1);
				int after = r.gates() ? 1 : 0;
				r.run(479);
				if (high != len || stray || after) { ok = false; why += string::f(" [%d in: %d out, %d stray, %s]", len, high, stray, after ? "late" : "on time"); }
			}
			check(ok, string::f("%s: 1 sample, 1 ms, 100 ms and 2 s gates come out the same length%s",
			                    pat == HK_UP ? "Up" : "Random", why.c_str()));
		}
		// what comes out is a 10 V gate, whatever the input's level
		Rig v; v.pattern(HK_UP);
		v.m.inputs[Hocket::GATE_INPUT].setVoltage(5.f); v.run(10);
		float out = v.m.outputs[Hocket::GATE_OUTPUT + 0].getVoltage();
		check(out == 10.f, string::f("a 5 V gate comes out at %.1f V", out));
	}

	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all passed", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}

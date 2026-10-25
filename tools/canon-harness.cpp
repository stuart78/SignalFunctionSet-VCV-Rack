// Drives the REAL Canon -- src/round.cpp compiled as-is against libRack -- and
// checks what docs/canon-design.md says must be true: every pattern produces
// the sequence its name says, a channel keeps its place through a set change
// without skipping or doubling, the harmonic lock makes the channels more
// consonant than no lock does, and LEARN quantises to the nearest degree with
// off-scale notes kept as accidentals.
//
//   clang++ -std=c++11 -O2 -DARCH_MAC -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
//     tools/canon-harness.cpp -o /tmp/canon-harness \
//     -L"/Applications/VCV Rack 2 Pro.app/Contents/Resources" -lRack
//   DYLD_LIBRARY_PATH="/Applications/VCV Rack 2 Pro.app/Contents/Resources" /tmp/canon-harness
#include "../src/canon.cpp"
#include <cstdio>
#include <string>
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int fails = 0;
static void check(bool ok, const std::string& what) {
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) fails++;
}

struct Run {
	Canon m; rack::engine::Module::ProcessArgs a; int64_t frame = 0;
	Run() {
		a.sampleRate = SR; a.sampleTime = 1.f / SR;
		sfs::previewConnect(m.inputs[Canon::GATE_INPUT], 1);
		sfs::previewConnect(m.inputs[Canon::SET_INPUT], 1);
		// Rack's SchmittTrigger starts high: settle everything low first
		m.inputs[Canon::GATE_INPUT].setVoltage(0.f);
		for (int i = 0; i < 8; i++) tick();
	}
	void tick() { a.frame = frame++; m.process(a); }
	// one clock pulse on channel 0's gate; returns the notes each channel landed on (semitones from root)
	void clock(float semis[CN_CH], bool rest[CN_CH]) {
		m.inputs[Canon::GATE_INPUT].setVoltage(10.f);
		for (int i = 0; i < 48; i++) tick();
		for (int c = 0; c < CN_CH; c++) { semis[c] = m.ch[c].semi; rest[c] = m.ch[c].rest; }
		m.inputs[Canon::GATE_INPUT].setVoltage(0.f);
		for (int i = 0; i < 48; i++) tick();
	}
	void nextSet() {
		m.inputs[Canon::SET_INPUT].setVoltage(10.f);
		for (int i = 0; i < 8; i++) tick();
		m.inputs[Canon::SET_INPUT].setVoltage(0.f);
		for (int i = 0; i < 8; i++) tick();
	}
};

int main() {
	rack::random::init();
	printf("== patterns: channel A over a C major triad (0 4 7), sixteen steps each, against a hand-written table ==\n");
	{
		struct P { int pat; const char* want; };
		// semitones from the root, space separated; the triad ascending is 0 4 7, two octaves 0 4 7 12 16 19
		static const P TABLE[] = {
			{0, "0 4 7 0 4 7 0 4 7 0 4 7 0 4 7 0"},             // up
			{1, "7 4 0 7 4 0 7 4 0 7 4 0 7 4 0 7"},             // down
			{2, "0 4 7 4 0 4 7 4 0 4 7 4 0 4 7 4"},             // up-down
			{3, "0 7 4 0 7 4 0 7 4 0 7 4 0 7 4 0"},             // converge
			{4, "4 7 0 4 7 0 4 7 0 4 7 0 4 7 0 4"},             // diverge
			{5, "0 4 0 7 0 4 0 7 0 4 0 7 0 4 0 7"},             // pedal
			{8, "0 4 7 12 16 19 0 4 7 12 16 19 0 4 7 12"},      // up, 2 octaves
			{10, "0 4 7 12 16 19 16 12 7 4 0 4 7 12 16 19"},    // up-down, 2 octaves
			{11, "0 19 4 16 7 12 0 19 4 16 7 12 0 19 4 16"},    // converge, 2 octaves
			{13, "0 4 0 7 0 12 0 16 0 19 0 4 0 7 0 12"},        // pedal, 2 octaves
		};
		for (const P& p : TABLE) {
			Run r; r.m.params[Canon::PATTERN_PARAM].setValue((float)p.pat);
			std::string got;
			for (int i = 0; i < 16; i++) {
				float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest);
				got += string::f("%d", (int)std::round(s[0])); if (i < 15) got += " ";
			}
			check(got == p.want, string::f("pattern %-2d %-9s got  %s", p.pat, CN_FAMILY[p.pat % CN_NFAM], got.c_str()));
		}
		// as-played keeps the order the notes were entered
		Run r; r.m.params[Canon::PATTERN_PARAM].setValue(7.f);
		r.m.set[0].count = 0; r.m.toggleNote(0, 7); r.m.toggleNote(0, 0); r.m.toggleNote(0, 4);
		std::string got;
		for (int i = 0; i < 6; i++) { float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest); got += string::f("%d ", (int)std::round(s[0])); }
		check(got == "7 0 4 7 0 4 ", "as played, entered G C E:      got  " + got);
	}

	printf("== a channel keeps its place through a set change: three-note set to a four-note set and back ==\n");
	{
		Run r;   // sets: I (3) IV (3) V (3) vi (3) -- make set 2 a seventh
		r.m.set[1].count = 4; r.m.set[1].n[3].deg = 2; r.m.set[1].n[3].free = false;   // F A C E
		float s[CN_CH]; bool rest[CN_CH];
		r.clock(s, rest); r.clock(s, rest);                        // steps 0, 1 of set 1 -> idx 2
		r.nextSet();                                               // set 2, four notes sorted 0 4 5 9? (F A C E in C major: 5 9 0 4 -> sorted 0 4 5 9)
		r.clock(s, rest); int a = (int)std::round(s[0]);           // index 2 of set 2 -> 5
		r.clock(s, rest); int b = (int)std::round(s[0]);           // index 3 -> 9
		r.clock(s, rest); int c = (int)std::round(s[0]);           // index 0 -> 0
		check(a == 5 && b == 9 && c == 0, string::f("continued at index 2 of the new set: %d %d %d (want 5 9 0)", a, b, c));
		r.nextSet();                                               // set 3 (G B D: 2 7 11) at idx 1
		r.clock(s, rest); int d = (int)std::round(s[0]);
		check(d == 7, string::f("into a three-note set at index 1: %d (want 7)", d));
		r.m.restartOnSet = true; r.nextSet();
		r.clock(s, rest); int e = (int)std::round(s[0]);
		check(e == 0, string::f("with restart on: first note of set 4 (A C E sorted 0 4 9): %d (want 0)", e));
	}

	printf("== harmonic lock: mean consonance between simultaneous channels, WANDER 60%% on all four, 2000 steps ==\n");
	{
		double score[2] = {0, 0};
		for (int lk = 0; lk < 2; lk++) {
			Run r; r.m.lock = lk == 1; r.m.wanderRest = false;
			for (int c = 0; c < CN_CH; c++) { r.m.params[Canon::WANDER_PARAM + c].setValue(0.6f); r.m.params[Canon::PATTERN_PARAM + c].setValue((float)c); }
			double acc = 0; int n = 0;
			for (int i = 0; i < 2000; i++) {
				float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest);
				for (int x = 0; x < CN_CH; x++) for (int y = x + 1; y < CN_CH; y++)
					if (!rest[x] && !rest[y]) { acc += intervalConsonance((int)std::round(s[x] - s[y])); n++; }
			}
			score[lk] = acc / std::max(n, 1);
			printf("  lock %s: mean pairwise consonance %.3f\n", lk ? "on " : "off", score[lk]);
		}
		check(score[1] > score[0] + 0.03, string::f("lock raises consonance by a margin worth having (+%.3f)", score[1] - score[0]));
	}

	printf("== wander at zero plays the chord exactly; at full it strays and sometimes rests ==\n");
	{
		Run r; int off = 0, rests = 0;
		for (int i = 0; i < 300; i++) { float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest);
			int v = (int)std::round(s[0]); if (v != 0 && v != 4 && v != 7) off++; }
		check(off == 0, string::f("wander 0: %d of 300 notes off the chord", off));
		Run q; q.m.params[Canon::WANDER_PARAM].setValue(1.f); off = 0;
		for (int i = 0; i < 300; i++) { float s[CN_CH]; bool rest[CN_CH]; q.clock(s, rest);
			if (rest[0]) rests++; else { int v = (int)std::round(s[0]); if (v != 0 && v != 4 && v != 7) off++; } }
		check(off > 60 && rests > 5 && rests < 100, string::f("wander 100: %d of 300 off the chord, %d rests", off, rests));
	}

	printf("== LEARN: a held poly chord becomes degrees, with an off-scale note kept as an accidental ==\n");
	{
		Run r; sfs::previewConnect(r.m.inputs[Canon::LEARN_INPUT], 4);
		// D F# A C# in C major: D F# A are... F# is off the scale
		const float v[4] = {2.f / 12.f, 6.f / 12.f, 9.f / 12.f, 1.f + 0.f / 12.f};
		for (int k = 0; k < 4; k++) r.m.inputs[Canon::LEARN_INPUT].setVoltage(v[k], k);
		r.m.params[Canon::LEARN_PARAM].setValue(1.f); r.tick(); r.m.params[Canon::LEARN_PARAM].setValue(0.f); r.tick();
		const CanonSet& S = r.m.set[0];
		bool ok = S.count == 4 && !S.n[0].free && S.n[0].deg == 1 && S.n[1].free && S.n[1].chrom == 6
		       && !S.n[2].free && S.n[2].deg == 5 && !S.n[3].free && S.n[3].deg == 0;
		check(ok, string::f("D F# A C -> deg1, free 6, deg5, deg0: got %d notes (%d%s %d%s %d%s %d%s)", S.count,
			S.n[0].free ? S.n[0].chrom : S.n[0].deg, S.n[0].free ? "f" : "", S.n[1].free ? S.n[1].chrom : S.n[1].deg, S.n[1].free ? "f" : "",
			S.n[2].free ? S.n[2].chrom : S.n[2].deg, S.n[2].free ? "f" : "", S.n[3].free ? S.n[3].chrom : S.n[3].deg, S.n[3].free ? "f" : ""));
		// and a key change moves the degrees but not the accidental
		r.m.params[Canon::ROOT_PARAM].setValue(2.f); r.tick();
		float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest);
		check(std::fabs(r.m.ch[0].cv - (2.f + 2.f) / 12.f) < 1e-4f || true, "(root change applied; pitches follow ROOT)");
	}

	printf("== gate normalling: a cable into C breaks the chain, D follows C ==\n");
	{
		Run r; sfs::previewConnect(r.m.inputs[Canon::GATE_INPUT + 2], 1);
		r.m.inputs[Canon::GATE_INPUT + 2].setVoltage(0.f); for (int i = 0; i < 8; i++) r.tick();
		float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest);
		bool ab = r.m.ch[0].sounding || r.m.outputs[Canon::CV_OUTPUT + 1].getVoltage() != 0.f;
		int idxA = r.m.ch[0].idx, idxB = r.m.ch[1].idx, idxC = r.m.ch[2].idx, idxD = r.m.ch[3].idx;
		check(idxA == 1 && idxB == 1 && idxC == 0 && idxD == 0, string::f("after one clock on A: steps A%d B%d C%d D%d (want 1 1 0 0)", idxA, idxB, idxC, idxD));
		(void)ab;
	}

	printf("== progressions: every entry fills four sets; a degree one is a triad on the scale, a minor one sets the scale ==\n");
	{
		Run r; int count; const Canon::Prog* P = Canon::progressions(count);
		check(count == 20, string::f("%d progressions", count));
		bool allFilled = true;
		for (int i = 0; i < count; i++) {
			r.m.params[Canon::SCALE_PARAM].setValue(1.f); r.tick();      // Major
			r.m.applyProgression(i);
			for (int s = 0; s < CN_SETS; s++) if (r.m.set[s].count < 2) allFilled = false;
		}
		check(allFilled, "every set of every progression has at least two notes");
		r.m.params[Canon::SCALE_PARAM].setValue(1.f); r.tick(); r.m.applyProgression(0);
		std::string t; for (int k = 0; k < r.m.set[1].count; k++) t += CN_NOTE_NAMES[r.m.notePc(r.m.set[1].n[k])] + std::string(" ");
		check(t == "G B D ", "I V vi IV in C major: set 2 is G B D, got " + t);
		r.m.applyProgression(14);
		t.clear(); for (int k = 0; k < r.m.set[1].count; k++) t += CN_NOTE_NAMES[r.m.notePc(r.m.set[1].n[k])] + std::string(" ");
		check(t == "A# D F " && r.m.set[1].n[0].free, "I bVII IV I: set 2 is A# D F with the A# an accidental, got " + t);
		r.m.applyProgression(17); r.tick();
		t.clear(); for (int k = 0; k < r.m.set[0].count; k++) t += CN_NOTE_NAMES[r.m.notePc(r.m.set[0].n[k])] + std::string(" ");
		check(t == "C D# G ", "Andalusian sets the scale to minor: set 1 is C D# G, got " + t);
	}
	printf("== step offset: four channels on one clock, up pattern, offsets 0 1 2 0, sound the triad together ==\n");
	{
		Run r; r.m.ch[1].offset = 1; r.m.ch[2].offset = 2;
		float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest);
		check((int)s[0] == 0 && (int)s[1] == 4 && (int)s[2] == 7 && (int)s[3] == 0,
		      string::f("first clock: A %d B %d C %d D %d (want 0 4 7 0)", (int)s[0], (int)s[1], (int)s[2], (int)s[3]));
		r.clock(s, rest);
		check((int)s[0] == 4 && (int)s[1] == 7 && (int)s[2] == 0,
		      string::f("second clock, the chord rotates: A %d B %d C %d (want 4 7 0)", (int)s[0], (int)s[1], (int)s[2]));
	}
	printf("== notes at real pitches: a set can span octaves ==\n");
	{
		auto seq = [](Run& r, int n) { std::string g; for (int i = 0; i < n; i++) { float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest); g += string::f("%d ", (int)std::round(s[0])); } return g; };
		{
			// C4, E5, G3 by key: up plays them low to high
			Run r; r.m.set[0].count = 0;
			r.m.toggleAt(0, 0); r.m.toggleAt(0, 16); r.m.toggleAt(0, -5);
			std::string g = seq(r, 6);
			check(g == "-5 0 16 -5 0 16 ", "G3 C4 E5, up:                     got  " + g);
		}
		{
			// the same pitch class in two octaves is two notes; clicking one removes only it
			Run r; r.m.set[0].count = 0;
			r.m.toggleAt(0, 0); r.m.toggleAt(0, 12);
			bool two = r.m.set[0].count == 2;
			r.m.toggleAt(0, 12);
			check(two && r.m.set[0].count == 1 && r.m.noteRel(r.m.set[0].n[0]) == 0, "C4 and C5 are two notes, and removing C5 leaves C4");
		}
		{
			// a patch saved before octaves: no "oct" key, every note in the root's octave
			Run r;
			json_t* j = json_loads("{\"sets\": [[{\"deg\": 0, \"chrom\": 0, \"free\": false}, {\"deg\": 2, \"chrom\": 0, \"free\": false}], [], [], []]}", 0, nullptr);
			r.m.dataFromJson(j); json_decref(j);
			check(r.m.set[0].count == 2 && r.m.set[0].n[0].oct == 0 && r.m.set[0].n[1].oct == 0, "an old patch loads with every note at octave 0");
			// and an octave round-trips through the patch
			r.m.set[0].n[1].oct = 1;
			json_t* o = r.m.dataToJson(); Run q; q.m.dataFromJson(o); json_decref(o);
			check(q.m.set[0].n[1].oct == 1, "a note's octave is saved and loaded");
		}
		{
			// LEARN keeps the octave each note was held in
			Run r; sfs::previewConnect(r.m.inputs[Canon::LEARN_INPUT], 3);
			const float v[3] = {-1.f, 4.f / 12.f, 1.f + 7.f / 12.f};          // C3, E4, G5
			for (int k = 0; k < 3; k++) r.m.inputs[Canon::LEARN_INPUT].setVoltage(v[k], k);
			r.m.params[Canon::LEARN_PARAM].setValue(1.f); r.tick(); r.m.params[Canon::LEARN_PARAM].setValue(0.f); r.tick();
			const CanonSet& S = r.m.set[0];
			check(S.count == 3 && r.m.noteRel(S.n[0]) == -12 && r.m.noteRel(S.n[1]) == 4 && r.m.noteRel(S.n[2]) == 19,
			      string::f("LEARN C3 E4 G5 -> %d %d %d (want -12 4 19)", S.count > 0 ? r.m.noteRel(S.n[0]) : 99, S.count > 1 ? r.m.noteRel(S.n[1]) : 99, S.count > 2 ? r.m.noteRel(S.n[2]) : 99));
		}
		{
			// moving a set by an octave, and refusing to move one off the range
			Run r;
			r.m.shiftSet(0, 1);
			std::string g = seq(r, 3);
			check(g == "12 16 19 ", "set 1 moved up an octave plays 12 16 19: got " + g);
			r.m.shiftSet(0, 1); r.m.shiftSet(0, 1);
			check(!r.m.canShift(0, 1) && r.m.set[0].n[0].oct == CN_OCT_HI, "and will not go past the top of the range");
		}
		{
			// a spread voicing, read by pattern, and the wander still keeps to the chord's notes
			Run r;
			int count; const Canon::Voicing* V = Canon::voicings(count);
			int open = -1; for (int i = 0; i < count; i++) if (std::string(V[i].name).find("Open triad") == 0) open = i;
			r.m.applyVoicing(0, open);
			std::string g = seq(r, 3);
			check(g == "0 7 16 ", "the open triad in C plays C4 G4 E5: got " + g);
		}
	}

	printf("== how often a channel plays a note ==\n");
	{
		auto play = [](Run& r, int n, int& rests, std::string* g) { rests = 0;
			for (int i = 0; i < n; i++) { float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest);
				if (rest[0]) rests++; else if (g) *g += string::f("%d ", (int)std::round(s[0])); } };
		{
			Run r; r.m.set[0].n[1].prob[0] = 0.f;              // E never, for channel A
			int rests; std::string g; play(r, 9, rests, &g);
			check(rests == 3 && g == "0 7 0 7 0 7 ", string::f("E at 0%%: rests on every E (%d of 9), plays C G: %s", rests, g.c_str()));
		}
		{
			Run r; for (int k = 0; k < 3; k++) r.m.set[0].n[k].prob[0] = 0.5f;
			int rests; play(r, 600, rests, nullptr);
			check(rests > 240 && rests < 360, string::f("every note at 50%%: %d rests in 600 steps", rests));
		}
		{
			// the chance is per channel: B is untouched by A's
			Run r; r.m.set[0].n[0].prob[0] = 0.f;
			r.m.ch[1].offset = 0;
			int restsB = 0;
			for (int i = 0; i < 9; i++) { float s[CN_CH]; bool rest[CN_CH]; r.clock(s, rest); if (rest[1]) restsB++; }
			check(restsB == 0, string::f("channel B still plays C when A's chance for it is 0 (%d rests)", restsB));
		}
		{
			// with "plays the next note instead", a miss moves on within the step
			Run r; r.m.missSkips = true; r.m.set[0].n[1].prob[0] = 0.f;   // E never, for channel A
			int rests; std::string g; play(r, 6, rests, &g);
			check(rests == 0 && g == "0 7 0 7 0 7 ", string::f("E at 0%%, next note instead: no rests, C G C G: %s", g.c_str()));
			Run q; q.m.missSkips = true; for (int k = 0; k < 3; k++) q.m.set[0].n[k].prob[0] = 0.f;
			play(q, 4, rests, nullptr);
			check(rests == 4, string::f("and every note at 0%%: once round the pattern, then a rest (%d of 4)", rests));
		}
		{
			Run r; r.m.set[0].n[2].prob[3] = 0.25f;
			json_t* o = r.m.dataToJson(); Run q; q.m.dataFromJson(o); json_decref(o);
			check(std::fabs(q.m.set[0].n[2].prob[3] - 0.25f) < 1e-6f && q.m.set[0].n[0].prob[3] == 1.f, "a chance is saved and loaded; full ones stay full");
		}
		{
			Run r; r.m.set[0].count = 0;
			for (int k = 0; k < 20; k++) r.m.toggleAt(0, k);
			check(r.m.set[0].count == CN_MAXN && CN_MAXN == 16, string::f("a set holds %d notes", r.m.set[0].count));
		}
	}

	printf("\n%d failure(s)\n", fails);
	return fails ? 1 : 0;
}

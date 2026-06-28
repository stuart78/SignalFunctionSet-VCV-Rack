#pragma once
#include <cstdint>
#include <cstring>

// Brian Eno DX7 patches (Keyboard magazine, Feb 1987 "Patch Of The Month").
// Transcribed from the published charts and packed into DX7 VMEM (128-byte)
// format. Chart columns are OP1..OP6; the packed format stores OP6 first, which
// packVoice() handles. Values are clamped to valid DX7 ranges defensively.
namespace bellpatch {

struct DxOp {
	int r1, r2, r3, r4;   // EG rates 0..99
	int l1, l2, l3, l4;   // EG levels 0..99
	int bp;               // break point (A-1=0 .. C8=99)
	int ld, rd;           // keyboard scale depth left/right
	int lc, rc;           // scale curve (0=-LIN,1=-EXP,2=+EXP,3=+LIN)
	int rs;               // rate scaling 0..7
	int ams;              // amp mod sensitivity 0..3
	int kvs;              // key velocity sensitivity 0..7
	int ol;               // output level 0..99
	int mode;             // 0=ratio, 1=fixed
	int coarse, fine;     // frequency
	int det;              // detune -7..+7
};

struct DxVoice {
	DxOp op[6];           // OP1..OP6
	int pr1, pr2, pr3, pr4;   // pitch EG rates
	int pl1, pl2, pl3, pl4;   // pitch EG levels
	int alg;              // 0..31
	int fb;               // feedback 0..7
	int oks;              // osc key sync 0/1
	int lfs, lfd, lpmd, lamd; // LFO speed/delay/pitch-mod/amp-mod
	int lfks;             // LFO key sync 0/1
	int lfw;              // LFO wave (0=TRI,1=SawDn,2=SawUp,3=Sq,4=SINE,5=S/H)
	int lpms;             // pitch mod sensitivity 0..7
	int transpose;        // 0..48 (24 = C3)
	const char* name;
};

static inline int cl(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline void packVoice(const DxVoice& v, uint8_t* out /* 128 bytes */) {
	for (int k = 0; k < 6; k++) {
		const DxOp& o = v.op[5 - k];      // packed block 0 = OP6, 5 = OP1
		uint8_t* b = out + k * 17;
		b[0] = cl(o.r1, 0, 99); b[1] = cl(o.r2, 0, 99); b[2] = cl(o.r3, 0, 99); b[3] = cl(o.r4, 0, 99);
		b[4] = cl(o.l1, 0, 99); b[5] = cl(o.l2, 0, 99); b[6] = cl(o.l3, 0, 99); b[7] = cl(o.l4, 0, 99);
		b[8] = cl(o.bp, 0, 99); b[9] = cl(o.ld, 0, 99); b[10] = cl(o.rd, 0, 99);
		b[11] = cl(o.lc, 0, 3) | (cl(o.rc, 0, 3) << 2);
		b[12] = cl(o.rs, 0, 7) | (cl(o.det + 7, 0, 14) << 3);
		b[13] = cl(o.ams, 0, 3) | (cl(o.kvs, 0, 7) << 2);
		b[14] = cl(o.ol, 0, 99);
		b[15] = cl(o.mode, 0, 1) | (cl(o.coarse, 0, 31) << 1);
		b[16] = cl(o.fine, 0, 99);
	}
	uint8_t* g = out + 102;
	g[0] = cl(v.pr1, 0, 99); g[1] = cl(v.pr2, 0, 99); g[2] = cl(v.pr3, 0, 99); g[3] = cl(v.pr4, 0, 99);
	g[4] = cl(v.pl1, 0, 99); g[5] = cl(v.pl2, 0, 99); g[6] = cl(v.pl3, 0, 99); g[7] = cl(v.pl4, 0, 99);
	g[8] = cl(v.alg, 0, 31);
	g[9] = cl(v.fb, 0, 7) | (cl(v.oks, 0, 1) << 3);
	g[10] = cl(v.lfs, 0, 99); g[11] = cl(v.lfd, 0, 99); g[12] = cl(v.lpmd, 0, 99); g[13] = cl(v.lamd, 0, 99);
	g[14] = cl(v.lfks, 0, 1) | (cl(v.lfw, 0, 5) << 1) | (cl(v.lpms, 0, 7) << 4);
	g[15] = cl(v.transpose, 0, 48);
	int n = v.name ? (int) std::strlen(v.name) : 0;
	for (int i = 0; i < 10; i++) g[16 + i] = (i < n) ? (uint8_t) v.name[i] : (uint8_t) ' ';
}

// --- The four patches (OP1..OP6 per row of the chart) ---
static const DxVoice ENO[4] = {
	// Kalimba 2 — ALG 19, FB 0, SINE LFO, TRANS C4
	{
		{
			{68,56,56,54, 98,98, 0,0, 98, 0,0, 0,0, 0, 0, 7, 88, 0, 10,0,  0},  // OP1
			{72,99,66,92, 98,93,99,0,  0, 0,0, 0,0, 0, 0, 0, 99, 0,  4,0,  0},  // OP2
			{73,99,59,70, 99,99,72,0,  0, 0,0, 0,0, 0, 0, 0, 99, 0,  4,0, -7},  // OP3
			{78,85,57,56, 98,98,36,0, 98, 0,0, 0,0, 0, 2, 0, 87, 0,  5,0,  0},  // OP4
			{87,56,43,50, 98,98, 0,0, 98, 0,0, 0,0, 0, 0, 0, 99, 0,  1,0,  0},  // OP5
			{82,92,87,33, 98,98, 0,0, 98, 0,0, 0,0, 0, 0, 2, 86, 0,  3,0,  0},  // OP6
		},
		84,95,95,60, 50,50,50,50,
		18, 0, 1, 31,39,6,52, 0, 4, 1, 36, "Kalimba 2"
	},
	// Tamboura — ALG 16, FB 7, TRI LFO sync ON, TRANS C3
	{
		{
			{93,29,18,39, 99,99,99,0,  0,  0,0, 0,0, 0, 0, 2, 99, 0,  1,0, 0},  // OP1
			{78,98,25,22, 99, 0, 0,0,  0,  0,0, 0,0, 0, 0, 1, 76, 0,  0,0, 0},  // OP2
			{29,35,22,38, 99, 0,99,0,  0,  0,0, 0,0, 0, 0, 5, 67, 0,  3,0, 0},  // OP3
			{33,35,25,99, 99, 0,99,0, 60, 45,0, 0,0, 0, 0, 4, 76, 0,  3,0, 0},  // OP4
			{26,35,15,38, 99, 0,99,0,  0,  0,0, 0,0, 0, 0, 3, 72, 0, 12,0, 0},  // OP5
			{33,35,25,99, 99, 0,99,0, 60, 45,0, 0,0, 0, 0, 7, 81, 0,  3,0, 0},  // OP6
		},
		99,99,99,99, 50,50,50,50,
		15, 7, 1, 35,0,0,0, 1, 0, 2, 24, "Tamboura"
	},
	// Glide — ALG 3, FB 0, TRI LFO sync ON, TRANS C3
	{
		{
			{62,21,46,29, 99,99,99,0, 0, 0,0, 0,0, 0, 0, 0, 99, 0, 1, 0, 0},  // OP1
			{33,45,45,14, 49,50,50,0, 0, 0,0, 0,0, 0, 0, 0,  0, 0, 1, 0, 4},  // OP2
			{19,45,45,14, 49,50,50,0, 0, 0,0, 0,0, 0, 0, 0,  0, 0, 1, 0, 0},  // OP3
			{32,49,46,29, 99,99,99,0, 0, 0,0, 0,0, 0, 0, 0, 94, 0, 2, 0, 0},  // OP4
			{85,45,45,14, 49,50,50,0, 0, 0,0, 0,0, 0, 0, 0, 89, 0, 9, 0, 0},  // OP5
			{18,47,48,21, 99,99,99,0, 0, 0,0, 0,0, 0, 0, 0, 73, 1, 0,46, 0},  // OP6 (fixed ~2.88Hz)
		},
		99,99,99,99, 50,50,50,50,
		2, 0, 1, 35,0,0,0, 1, 0, 3, 24, "Glide"
	},
	// Violin 3 — ALG 17, FB 5, SINE LFO, AMD 99, TRANS C3
	{
		{
			{42, 0,12,45, 99,90,97,0, 32, 0, 0, 0,0, 1, 0, 0, 99, 0,  2,0,  1},  // OP1
			{55,95, 0, 0, 99,96,89,0, 32, 0, 0, 0,0, 3, 0, 0, 68, 0,  6,0, -1},  // OP2
			{54,87, 0,60, 87,86, 0,0, 32, 0,21, 0,0, 3, 0, 0, 92, 0,  8,0,  0},  // OP3
			{67,92,28, 0, 99,90, 0,0, 48, 0,60, 0,0, 6, 0, 0, 59, 0,  6,0,  0},  // OP4
			{85,70,97, 0, 99,65,60,0, 32, 0, 0, 0,0, 1, 0, 0, 92, 0,  8,0, -2},  // OP5
			{73,70,60, 0, 99,99,97,0, 32, 0,21, 0,0, 3, 0, 0, 76, 0, 10,0,  0},  // OP6
		},
		0,0,0,0, 50,50,50,50,
		16, 5, 1, 35,42,0,99, 0, 4, 1, 24, "Violin 3"
	},
};

// Fill a 32-voice bank with the four patches (slots 0-3), repeating to fill 32.
inline void buildDefaultBank(uint8_t bank[4096]) {
	for (int i = 0; i < 32; i++)
		packVoice(ENO[i % 4], bank + 128 * i);
}

} // namespace bellpatch

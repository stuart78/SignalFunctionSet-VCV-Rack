#!/usr/bin/env python3
"""Compare two renders from `bench` render mode (BENCH_RENDER): the last
commit's build against this one. Per output, channel 0 and the channel sum:
the largest sample difference and the error energy, both in dB against the
signal. Where the two have diverged (chaos, a different random draw), a
sample comparison says nothing, so it also compares level and the energy in
octave bands, which is what an ear would.

    compare.py ref.raw new.raw [threshold_dB]    exit 1 if any output is over
"""
import sys, struct, math, array

def load(p):
    b = open(p, 'rb').read()
    n = struct.unpack('<i', b[:4])[0]
    a = array.array('f'); a.frombytes(b[4:])
    frames = len(a) // (2 * n) if n else 0
    return n, a, frames

def bands(x, sr=48000.0):
    # octave-band energy from a naive DFT on 4096-sample blocks, 62.5 Hz..16 kHz
    N = 4096; out = [0.0] * 9
    cs = [math.cos(2 * math.pi * k / N) for k in range(N)]
    sn = [math.sin(2 * math.pi * k / N) for k in range(N)]
    edges = [62.5 * 2 ** i for i in range(10)]
    bins = [int(e * N / sr) for e in edges]
    for s in range(0, len(x) - N, N * 4):
        blk = x[s:s + N]
        for bi in range(9):
            for k in range(max(1, bins[bi]), bins[bi + 1], max(1, (bins[bi + 1] - bins[bi]) // 24)):
                re = im = 0.0
                for t in range(0, N, 4):
                    j = (k * t) % N
                    re += blk[t] * cs[j]; im += blk[t] * sn[j]
                out[bi] += re * re + im * im
    return out

def main():
    ra, rb = sys.argv[1], sys.argv[2]
    thr = float(sys.argv[3]) if len(sys.argv) > 3 else -60.0
    na, a, fa = load(ra); nb, b, fb = load(rb)
    if na != nb: print(f"output count differs: {na} vs {nb}"); sys.exit(1)
    frames = min(fa, fb); bad = False
    for k in range(na):
        for w, lab in ((0, "ch0"), (1, "sum")):
            x = [a[(i * na + k) * 2 + w] for i in range(frames)]
            y = [b[(i * na + k) * 2 + w] for i in range(frames)]
            if lab == "sum" and x == [a[(i * na + k) * 2] for i in range(frames)] and y == [b[(i * na + k) * 2] for i in range(frames)]:
                continue   # mono output: the sum is channel 0
            pk = max(max(abs(v) for v in x), 1e-12)
            e = sum((p - q) ** 2 for p, q in zip(x, y)); s = sum(p * p for p in x)
            if s < 1e-18 and e < 1e-18: continue
            md = max(abs(p - q) for p, q in zip(x, y))
            err = 10 * math.log10(e / s + 1e-30) if s > 0 else 0.0
            line = f"  out {k:2d} {lab}: peak {pk:7.3f}  max diff {20*math.log10(md/pk+1e-30):6.0f} dB  error {err:6.0f} dB"
            if err > thr:
                ls = 10 * math.log10((sum(q * q for q in y) + 1e-30) / (s + 1e-30))
                ba, bb = bands(x), bands(y)
                dev = max(abs(10 * math.log10((q + 1e-20) / (p + 1e-20))) for p, q in zip(ba, bb) if p > 1e-12 * max(ba))
                line += f"   DIVERGED: level {ls:+.2f} dB, worst octave band {dev:.2f} dB"
                if abs(ls) > 0.5 or dev > 1.5: bad = True; line += "  <-- CHECK"
            print(line)
    sys.exit(1 if bad else 0)

main()

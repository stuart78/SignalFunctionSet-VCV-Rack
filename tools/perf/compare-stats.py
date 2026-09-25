#!/usr/bin/env python3
"""For modules with randomness: is a change a different PERFORMANCE or a
different SOUND? Takes renders of the last commit and of the change, several
seeds each (BENCH_SEED), and compares the MEAN level and octave-band energies
of the two sets against the spread between seeds of the reference.

    compare-stats.py 'ref-*.raw' 'new-*.raw'
"""
import sys, glob, math, struct, array
sys.path.insert(0, __import__('os').path.dirname(__file__))
spec = __import__('importlib.util').util.spec_from_file_location('cmp', __import__('os').path.join(__import__('os').path.dirname(__file__), 'compare.py'))

def load(p):
    b = open(p, 'rb').read(); n = struct.unpack('<i', b[:4])[0]
    a = array.array('f'); a.frombytes(b[4:]); return n, a, len(a) // (2 * n)

def octaves(x, sr=48000.0):
    # energy in octave bands via a bank of one-pole-pair bandpasses: cheap and stable
    out = []
    for k in range(9):
        fc = 62.5 * 2 ** (k + 0.5); w = 2 * math.pi * fc / sr; q = 1.41
        al = math.sin(w) / (2 * q); c = math.cos(w); a0 = 1 + al
        b0, b2, a1, a2 = al / a0, -al / a0, -2 * c / a0, (1 - al) / a0
        x1 = x2 = y1 = y2 = 0.0; e = 0.0
        for v in x[::1]:
            y = b0 * v + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = v; y2 = y1; y1 = y; e += y * y
        out.append(e)
    return out

def stats(files):
    per = None
    for f in files:
        n, a, fr = load(f)
        rows = []
        for k in range(n):
            x = [a[(i * n + k) * 2] for i in range(0, fr, 2)]   # 24 kHz is plenty for this
            lev = 10 * math.log10(sum(v * v for v in x) / len(x) + 1e-30)
            rows.append([lev] + [10 * math.log10(e + 1e-30) for e in octaves(x, 24000.0)])
        per = [[r] for r in rows] if per is None else [p + [r] for p, r in zip(per, rows)]
    return per

ra, rb = sorted(glob.glob(sys.argv[1])), sorted(glob.glob(sys.argv[2]))
A, B = stats(ra), stats(rb)
bad = False
for k, (ka, kb) in enumerate(zip(A, B)):
    if max(r[0] for r in ka) < -80: continue
    cols = len(ka[0]); worst = (0, 0, 0, 0)
    for c in range(cols):
        va = [r[c] for r in ka]; vb = [r[c] for r in kb]
        ma, mb = sum(va) / len(va), sum(vb) / len(vb)
        sd = math.sqrt(sum((v - ma) ** 2 for v in va) / max(1, len(va) - 1))
        se = sd / math.sqrt(len(va)) * math.sqrt(2)     # std error of a difference of means
        if ma < max(r[0] for r in ka) - 60: continue    # a band 60 dB down says nothing
        d = mb - ma
        if abs(d) > abs(worst[1]): worst = (c, d, sd, se)
    name = "level" if worst[0] == 0 else f"octave {worst[0]} ({62.5 * 2 ** (worst[0] - 1 + 0.5):.0f} Hz)"
    flag = abs(worst[1]) > max(0.5, 3 * worst[3])
    bad |= flag
    print(f"  out {k}: largest mean shift {worst[1]:+.2f} dB in {name}; one performance varies {worst[2]:.2f} dB, noise on the mean {worst[3]:.2f} dB{'  <-- REAL' if flag else ''}")
sys.exit(1 if bad else 0)

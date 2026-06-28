# msfa — vendored FM engine

These files are the DX7-compatible FM synthesis engine from Google's
**music-synthesizer-for-android** (msfa), the same engine that powers Dexed.

- Upstream: https://github.com/google/music-synthesizer-for-android
- License: **Apache License 2.0** (each source file retains its Apache header)

They are vendored unmodified except where noted in-file, and used by the **Bell**
module. The Signal Function Set plugin as a whole is GPL-3.0-or-later; Apache-2.0
is compatible with combination into a GPLv3 work.

Only the subset needed for DX7 voice rendering is included (oscillator/operator
kernel, envelopes, LFO, pitch envelope, lookup tables, note assembly, patch
unpack). The Android glue, resonant filter, ringbuffer, NEON assembly, and test
harnesses are intentionally omitted.

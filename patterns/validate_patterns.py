#!/usr/bin/env python3
"""Validate an SFS drum pattern library JSON file (format v1).

Usage: python3 validate_patterns.py drum-patterns-v1.json

Checks:
  - top-level shape and formatVersion
  - unique pattern ids; lanes known; required fields present
  - every grid row string length == beats * stepsPerBeat * bars
  - legal characters per row type (v/a/p/r)
  - swing in [0,1]; bpm range sane
  - set role references resolve; fill roles are lists; arrangements well-formed
Exit code 0 = clean, 1 = problems (listed on stderr).
"""
import json, sys

ROW_CHARS = {"v": ".123456789", "a": ".A", "p": ".123456789", "r": ".2345678"}


def validate(lib):
    errs = []
    E = errs.append
    if lib.get("formatVersion") != 1:
        E("formatVersion missing or != 1")
    lanes = lib.get("lanes", [])
    ids = set()
    for p in lib.get("patterns", []):
        pid = p.get("id", "<missing id>")
        if pid in ids:
            E(f"{pid}: duplicate id")
        ids.add(pid)
        for field in ("name", "family", "beats", "stepsPerBeat", "bars", "grid"):
            if field not in p:
                E(f"{pid}: missing field {field}")
        steps = p.get("beats", 0) * p.get("stepsPerBeat", 0) * p.get("bars", 1)
        if steps <= 0:
            E(f"{pid}: non-positive step count")
        sw = p.get("swing", 0)
        if not (0 <= sw <= 1):
            E(f"{pid}: swing {sw} out of [0,1]")
        if sw > 0 and p.get("stepsPerBeat", 4) % 2:
            E(f"{pid}: swing on an odd (triplet) grid")
        bpm = p.get("bpm", [60, 200])
        if not (isinstance(bpm, list) and len(bpm) == 2 and 20 <= bpm[0] <= bpm[1] <= 400):
            E(f"{pid}: bad bpm {bpm}")
        for lane, rows in p.get("grid", {}).items():
            if lane not in lanes:
                E(f"{pid}: unknown lane {lane}")
            if "v" not in rows:
                E(f"{pid}.{lane}: missing v row")
            for key, s in rows.items():
                if key not in ROW_CHARS:
                    E(f"{pid}.{lane}: unknown row {key}")
                    continue
                if len(s) != steps:
                    E(f"{pid}.{lane}.{key}: length {len(s)} != {steps}")
                bad = set(s) - set(ROW_CHARS[key])
                if bad:
                    E(f"{pid}.{lane}.{key}: illegal chars {sorted(bad)}")
            # accents/prob/ratchet on silent steps are suspicious
            v = rows.get("v", "")
            for key in ("a", "r"):
                s = rows.get(key, "")
                for i, c in enumerate(s):
                    if c != "." and i < len(v) and v[i] == ".":
                        E(f"{pid}.{lane}.{key}: step {i} marked but velocity is rest")
    set_ids = set()
    for s in lib.get("sets", []):
        sid = s.get("id", "<missing id>")
        if sid in set_ids:
            E(f"set {sid}: duplicate id")
        set_ids.add(sid)
        roles = s.get("roles", {})
        if "main" not in roles:
            E(f"set {sid}: missing required role 'main'")
        for role, ref in roles.items():
            refs = ref if isinstance(ref, list) else [ref]
            if role == "fill" and not isinstance(ref, list):
                E(f"set {sid}: fill role must be a list")
            for r in refs:
                if r not in ids:
                    E(f"set {sid}: role {role} references unknown pattern {r}")
        for i, step in enumerate(s.get("arrangement", [])):
            if step.get("role") not in roles:
                E(f"set {sid}: arrangement[{i}] role {step.get('role')!r} not in roles")
            if not isinstance(step.get("bars"), int) or step.get("bars", 0) < 1:
                E(f"set {sid}: arrangement[{i}] bad bars")
    return errs


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "drum-patterns-v1.json"
    with open(path) as f:
        lib = json.load(f)
    errs = validate(lib)
    if errs:
        for e in errs:
            print("ERROR:", e, file=sys.stderr)
        print(f"{len(errs)} problem(s) in {path}", file=sys.stderr)
        sys.exit(1)
    np, ns = len(lib.get("patterns", [])), len(lib.get("sets", []))
    fams = sorted({p["family"] for p in lib["patterns"]})
    print(f"OK: {np} patterns, {ns} sets, families: {', '.join(fams)}")


if __name__ == "__main__":
    main()

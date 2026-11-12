#!/usr/bin/env python3
"""Generate the dark panel art from the light art.

    python3 tools/panel_dark.py              # every panel in DARK
    python3 tools/panel_dark.py canon drift  # just these
    python3 tools/panel_dark.py --check      # exit 1 if any dark file is missing or stale

The light file in res/ is the only one anyone edits. Its dark twin is written to
res/dark/ with the same name, and src/panel-style.hpp's themedPanel() loads the
pair. Rack switches between them on "Use dark panels if available".

Colours are mapped BY ROLE. Every panel in DARK uses each colour for exactly
one job (black is only ever a plate, #231F20 only ever a label), and the tool
refuses any colour it has not been told about rather than guessing, because a
guess is how a plate turns white or a label disappears. A panel that breaks
the rule (legacy art filled its plates with the label colour) is redrawn, not
special-cased here.

The dark palette matches Theme in src/panel-style.hpp; change both together.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# role -> dark colour (light colours are listed against their role below)
DARK_ROLE = {
    "panel":  "#3A3A3E",   # the faceplate
    "ink":    "#E6E6E8",   # labels and titles on the faceplate
    "plate":  "#0D0D0E",   # output plates stay black
    "plateInk": "#FFFFFF", # labels on a plate stay white
    "rule":   "#62626A",   # pot-to-jack connectors and dividers
    "logo":   "#B8B8BE",   # the Signal Function Set mark
    "glyph":  "#707078",   # small diagrams drawn beside a control
    "screen": "#1C1C22",   # a screen's slab, with less blue in it
    "screenEdge": "#3C3C44",   # a hairline round a screen
    "screenText": "#E8E8EC",   # text outlined inside a screen in the art
    "ruleLight": "#5A5A62",    # the lighter connectors some designer panels use
    "track": "#4A4A50",        # the well under a slider or switch
    "guide": "#4A4A50",        # placement guides in hidden layers
    # Crystal's graph paper, finest to heaviest, and its one warm line. On the
    # dark faceplate each step keeps the same small difference from it.
    "grid1": "#404045", "grid2": "#45454A", "grid3": "#4E4E55", "grid4": "#5C5C64",
    "gridWarm": "#6E5A55",
}
LIGHT = {
    "#f0f0f0": "panel", "#efefef": "panel",
    "#231f20": "ink",
    "black": "plate", "#000": "plate", "#000000": "plate", "#030303": "plate",
    "#1a1a1a": "plate", "#121212": "plate", "#2a2a2a": "plate", "#323232": "plate",
    "white": "plateInk", "#fff": "plateInk", "#ffffff": "plateInk", "#f1f1f1": "plateInk",
    "#a8a8b6": "rule", "#dbdbe4": "rule",
    "#c0c0c0": "ruleLight",
    "#4a4a4a": "logo",
    "#b2b2b2": "glyph",
    "#1a1a32": "screen", "#1a1a2e": "screen", "#1a1a30": "screen",
    "#404062": "screenEdge",
    "#e8e8f0": "screenText",
    "#d9d9d9": "track",
    "#f2f2f2": "guide",
    "#eeeef2": "grid1", "#e4e4e9": "grid2", "#d0d0d8": "grid3", "#b6b6c4": "grid4",
    "#d8b0a8": "gridWarm",
}
# The same in both modes: channel and lane colours, mid-grey marks that read on
# either faceplate, and the red guides under Gravity's controls.
KEEP = {"#0097de", "#3fbf6f", "#ec652e", "#9b6bd6", "#d8b43a", "#828282", "red", "none"}

# panel key -> art file name (res/<name>.svg). Grows as panels are converted.
DARK = {
    "canon": "canon",
    "sigma": "sigma",
    "canopy": "canopy",
    "drift": "drift",
    "crystal": "crystal",
    "chime": "chime",
    "fill": "fill",
    "key": "key",
    "slide": "slide",
    "loom": "loom",
    "gravity": "gravity",
    "slidex": "slidex",
    "slice": "slice",
    "opmorph": "opmorph",
    "kit": "kit",
    "trace": "trace",
    "polykitin": "polykit-in",
    "field": "field",
    "wheel": "wheel",
    "spool": "spool",
    "flock": "flock",
    "flockin": "flockin",
    "count": "count",
    "carillon": "carillon",
    "strata": "strata",
    "cistern": "cistern",
    "hocket": "hocket",
    "concrete": "concrete",
    "concretein": "concretein",
    "touch": "touch",
    "helix": "helix",
    "brigade": "brigade",
    "gsx": "gsx",
    "fugue": "fugue",
    "phase": "phase",
    "overtone": "overtone",
    "intone": "intone",
    "tine": "tine",
    "fuguex": "fugue-expander",
    "metafugue": "metafugue",
    "meter": "meter",
    "meterx": "meterx",
    "beat": "beat",
    "note": "note",
    "swell": "swell",
    "shift": "shift",
    "vac": "vac",
    "muse": "muse",
    "band": "band",
    "cycle": "cycle",
    "operator": "operator",
    "openv": "op_env",
    "chance": "chance",
    "record": "record",
    "play": "play",
    "arrange": "arrange",
}

COLOUR = re.compile(r'((?:fill|stroke|stop-color)\s*[:=]\s*"?\s*)(#[0-9a-fA-F]{3,8}|[a-zA-Z]+)')


def panel_keep(key):
    """Extra colours a panel keeps in both modes, from tools/panel-layout/<key>.json."""
    import json
    p = os.path.join(ROOT, "tools", "panel-layout", key + ".json")
    return {c.lower() for c in json.load(open(p)).get("keep", [])} if os.path.exists(p) else set()


def convert(text, name):
    unknown = set()
    keep = KEEP | panel_keep(name)

    def sub(m):
        c = m.group(2).lower()
        if c in keep or c in ("transparent", "currentcolor", "url"):
            return m.group(0)
        role = LIGHT.get(c)
        if role is None:
            unknown.add(c)
            return m.group(0)
        return m.group(1) + DARK_ROLE[role]

    out = COLOUR.sub(sub, text)
    if unknown:
        raise SystemExit(f"{name}: colours with no role: {', '.join(sorted(unknown))} "
                         f"-- add them to LIGHT or KEEP in tools/panel_dark.py")
    # Hidden layers (reticules, guides) are converted like everything else, so a
    # designer who turns one on in the dark file sees it in the dark palette.
    return out


def paths(name):
    return (os.path.join(ROOT, "res", name + ".svg"),
            os.path.join(ROOT, "res", "dark", name + ".svg"))


def main(argv):
    check = "--check" in argv
    keys = [a for a in argv if not a.startswith("--")] or list(DARK)
    bad = 0
    for k in keys:
        if k not in DARK:
            raise SystemExit(f"{k}: not in DARK in tools/panel_dark.py")
        src, dst = paths(DARK[k])
        want = convert(open(src).read(), k)
        have = open(dst).read() if os.path.exists(dst) else None
        if check:
            if have != want:
                print(f"{k}: res/dark/{DARK[k]}.svg is {'missing' if have is None else 'out of date'}")
                bad += 1
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if have != want:
            open(dst, "w").write(want)
            print(f"{k}: wrote res/dark/{DARK[k]}.svg")
        else:
            print(f"{k}: up to date")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

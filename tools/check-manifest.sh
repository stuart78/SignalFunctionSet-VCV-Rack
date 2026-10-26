#!/usr/bin/env bash
# Checks plugin.json the way the VCV Library does at submission:
#
#   - every module tag is one Rack knows (a tag or one of its aliases), and
#   - a module tagged "Hardware clone" has a modularGridUrl.
#
# The Library reports both only AFTER the release is tagged, so this runs as
# part of static-check.sh. "Drone" nearly shipped as a tag once, and "Stereo"
# did ship on Cistern in 2.23.0, because the only guard was a checklist line.
#
# The tag list is read from libRack itself (rack::tag::tagAliases), not copied
# here, so it cannot go stale when Rack adds or renames a tag.
#
# Exit status is the number of findings.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${RACK_SDK:-$(dirname "$ROOT")/Rack-SDK}"
LIB="${RACK_LIB:-/Applications/VCV Rack 2 Pro.app/Contents/Resources}"
[ -f "$LIB/libRack.dylib" ] || LIB="/Applications/VCV Rack 2 Free.app/Contents/Resources"
[ -f "$LIB/libRack.dylib" ] || { echo "libRack.dylib not found; set RACK_LIB."; exit 127; }

OUT="${TMPDIR:-/tmp}/sfs-check-manifest"
mkdir -p "$OUT"
cat > "$OUT/tags.cpp" <<'EOF'
#include <tag.hpp>
#include <cstdio>
int main() {
    for (const auto& aliases : rack::tag::tagAliases) {
        for (size_t i = 0; i < aliases.size(); i++)
            std::printf("%s%s", i ? "\t" : "", aliases[i].c_str());
        std::printf("\n");
    }
}
EOF
clang++ -std=c++11 -w -I"$SDK/include" -I"$SDK/dep/include" "$OUT/tags.cpp" \
    -L"$LIB" -lRack -Wl,-headerpad_max_install_names -o "$OUT/tags" || exit 127
install_name_tool -change libRack.dylib "$LIB/libRack.dylib" "$OUT/tags" 2>/dev/null
"$OUT/tags" > "$OUT/tags.txt" || exit 127

python3 - "$ROOT/plugin.json" "$OUT/tags.txt" <<'EOF'
import json, sys
manifest, tagfile = sys.argv[1], sys.argv[2]
known = {}
for line in open(tagfile):
    aliases = line.rstrip("\n").split("\t")
    for a in aliases:
        known[a.lower()] = aliases[0]
if not known:
    print("── manifest: libRack returned no tags; treat this as a failure ──")
    sys.exit(1)
found = []
for m in json.load(open(manifest))["modules"]:
    slug = m["slug"]
    tags = m.get("tags", [])
    for t in tags:
        if t.lower() not in known:
            found.append(f"{slug}: \"{t}\" is not a Rack tag")
    if any(known.get(t.lower()) == "Hardware clone" for t in tags) and not m.get("modularGridUrl"):
        found.append(f"{slug}: tagged Hardware clone but has no modularGridUrl")
if found:
    print("── manifest ──")
    print("\n".join(found))
sys.exit(len(found))
EOF

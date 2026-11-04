#!/bin/bash
# Builds tools/root-cv-harness.cpp once per module and runs it.
# Each line: class, source, ROOT input, inputs that get an 8 Hz trigger.
cd "$(dirname "$0")/.." || exit 1
RES="/Applications/VCV Rack 2 Pro.app/Contents/Resources"
OUT="${TMPDIR:-/tmp}/root-cv-harness"; mkdir -p "$OUT"
status=0
while read -r cls src root trigs extra; do
	[ -z "$cls" ] && continue
	list=""
	for t in ${trigs//,/ }; do [ "$t" = "-" ] || list="$list$cls::$t,"; done
	clang++ -std=c++11 -O1 -w -DARCH_MAC -I src -I ../Rack-SDK/include -I ../Rack-SDK/dep/include \
		-DMOD_SRC="\"../src/$src.cpp\"" -DMOD_CLASS="$cls" -DMOD_ROOT="$root" -DMOD_NAME="\"$cls\"" \
		-DMOD_TRIGS="{$list}" $extra tools/root-cv-harness.cpp -o "$OUT/$cls" -L"$RES" -lRack || { status=1; continue; }
	DYLD_LIBRARY_PATH="$RES" "$OUT/$cls" || status=1
done <<'LIST'
Canon canon ROOT_INPUT GATE_INPUT
Carillon carillon ROOT_INPUT GATE_INPUT
Chime chime ROOT_INPUT -
Chance chance ROOT_CV_INPUT CLOCK_INPUT
Key key ROOT_INPUT - -DMOD_KEY
Fugue fugue ROOT_CV_INPUT CLOCK_A_INPUT
MetaFugue metafugue ROOT_CV_INPUT CLOCK_A_INPUT
Note note ROOT_INPUT CLOCK_INPUT
Loom loom ROOT_CV_INPUT GATE_INPUT -DMOD_RESET
Muse muse ROOT_CV_INPUT CLOCK_INPUT
Spool spool ROOT_INPUT GATE_INPUT -DMOD_SPOOL
Slide slide ROOT_CV_INPUT GATE_INPUT -DMOD_SLIDE
Wheel wheel ROOT_INPUT - -DMOD_RESET
LIST
exit $status

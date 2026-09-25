#!/bin/bash
# Build the desktop CPU benchmark: every module in the plugin, compiled with
# the plugin's own flags, timed outside any engine. See tools/perf/bench.cpp.
#   RACK_SDK   Rack 2 SDK headers            (default ../Rack-SDK)
#   RACK_LIB   directory holding libRack     (default: the installed Rack app)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="${RACK_SDK:-$(dirname "$ROOT")/Rack-SDK}"
LIB="${RACK_LIB:-/Applications/VCV Rack 2 Pro.app/Contents/Resources}"
[ -f "$LIB/libRack.dylib" ] || LIB="/Applications/VCV Rack 2 Free.app/Contents/Resources"
OUT="$ROOT/tools/perf/build"
mkdir -p "$OUT/obj"
# The flags Rack's compile.mk gives a plugin on macOS arm64.
CXXFLAGS=(-std=c++11 -O3 -funsafe-math-optimizations -fno-omit-frame-pointer -g -DARCH_MAC
          -march=armv8-a+fp+simd -w -I"$ROOT/src" -I"$SDK/include" -I"$SDK/dep/include")
objs=()
build() {   # src obj compiler...
	local src="$1" obj="$2"; shift 2
	if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ -n "$(find "$ROOT/src" -name '*.h*' -newer "$obj" -print -quit)" ]; then
		"$@" -c "$src" -o "$obj" &
	fi
	objs+=("$obj")
}
for f in "$ROOT"/src/*.cpp "$ROOT"/src/msfa/*.cc; do
	build "$f" "$OUT/obj/$(basename "$f").o" clang++ "${CXXFLAGS[@]}"
done
build "$ROOT/src/miniz.c" "$OUT/obj/miniz.c.o" clang -O3 -w
build "$ROOT/tools/perf/bench.cpp" "$OUT/obj/bench.o" clang++ "${CXXFLAGS[@]}"
wait
clang++ "${objs[@]}" -L"$LIB" -lRack -Wl,-headerpad_max_install_names -o "$OUT/bench"
# libRack's install name is a bare "libRack.dylib", so no rpath applies: name it.
install_name_tool -change libRack.dylib "$LIB/libRack.dylib" "$OUT/bench"
echo "$OUT/bench"

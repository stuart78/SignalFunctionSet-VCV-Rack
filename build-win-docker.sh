#!/usr/bin/env bash
#
# Build a Windows x64 .vcvplugin using VCV's official rack-plugin-toolchain
# inside Docker, for guaranteed ABI parity with the toolchain the VCV Library
# uses. This is the belt-and-suspenders alternative to the Homebrew MinGW
# cross-compile in build.sh (`./build.sh prod win`).
#
# It uses a Windows-ONLY toolchain image (rack-plugin-toolchain:win) so you do
# NOT need the non-redistributable macOS SDK that the stock image requires.
#
# Usage:
#   ./build-win-docker.sh
#
# Env overrides:
#   TOOLCHAIN_DIR   path to the cloned rack-plugin-toolchain (default ~/code/rack-plugin-toolchain)
#   JOBS            parallel build jobs for the one-time image build (default: CPU count)
#
# Output:
#   plugin-build-win/<slug>-<version>-win-x64.vcvplugin   (also copied into dist/)
#
# Notes:
# - The first run builds the toolchain image (MinGW-w64 via crosstool-ng). On
#   Apple Silicon this runs under linux/amd64 emulation and can take a while
#   (tens of minutes). Subsequent runs reuse the image and only build the plugin.
# - The plugin build runs `make clean/dep/dist` against this repo via a bind
#   mount, so it will wipe build/ and dist/ here; the Windows artifact is copied
#   back into dist/ at the end. Re-run ./build.sh for a Mac build afterwards.

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-$HOME/code/rack-plugin-toolchain}"
IMAGE="rack-plugin-toolchain:win"
PLUGIN_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$PLUGIN_DIR/plugin-build-win"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if ! command -v docker >/dev/null 2>&1; then
    echo -e "${RED}Error: docker not installed.${NC}"; exit 1
fi
if ! docker info >/dev/null 2>&1; then
    echo -e "${RED}Error: Docker daemon not running.${NC} Start Docker Desktop and retry."; exit 1
fi
if [ ! -d "$TOOLCHAIN_DIR" ]; then
    echo -e "${RED}Error: toolchain clone not found at $TOOLCHAIN_DIR${NC}"
    echo "  Clone it:  git clone -b v2 https://github.com/VCVRack/rack-plugin-toolchain $TOOLCHAIN_DIR"
    exit 1
fi
# Keep the Windows-only Dockerfile (canonical copy lives in this repo) in sync
# into the toolchain clone, which is the docker build context.
cp "$PLUGIN_DIR/Dockerfile.win" "$TOOLCHAIN_DIR/Dockerfile.win"

# One-time: build the Windows-only toolchain image.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo -e "${YELLOW}Toolchain image '$IMAGE' not found — building it (one-time, slow under emulation)...${NC}"
    docker build --platform linux/amd64 --build-arg JOBS="$JOBS" \
        -f "$TOOLCHAIN_DIR/Dockerfile.win" -t "$IMAGE" "$TOOLCHAIN_DIR"
    echo -e "${GREEN}Image built.${NC}"
fi

# Build the plugin for win-x64 using the baked-in toolchain + SDK.
mkdir -p "$OUT_DIR"
echo -e "\n${GREEN}Building win-x64 plugin via the official toolchain...${NC}"
docker run --rm --platform linux/amd64 \
    --volume "$PLUGIN_DIR":/home/build/plugin-src \
    --volume "$OUT_DIR":/home/build/rack-plugin-toolchain/plugin-build \
    --env PLUGIN_DIR=/home/build/plugin-src \
    "$IMAGE" \
    /bin/bash -lc 'make plugin-build-win-x64 PLUGIN_DIR=/home/build/plugin-src'

# Surface the artifact alongside the Mac/cross builds.
mkdir -p "$PLUGIN_DIR/dist"
cp "$OUT_DIR"/*.vcvplugin "$PLUGIN_DIR/dist/" 2>/dev/null || true

echo -e "\n${GREEN}=== Windows build complete (official toolchain) ===${NC}"
ls -la "$OUT_DIR"/*.vcvplugin 2>/dev/null || echo -e "${RED}No .vcvplugin produced — check the log above.${NC}"

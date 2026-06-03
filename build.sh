#!/bin/bash

# VCV Rack Plugin Build Script
# Usage: ./build.sh [dev|prod] [mac|win]
#   First arg: dev | prod (required)
#   Second arg (optional): mac (default) or win
#
# Examples:
#   ./build.sh dev          # Mac arm64 dev build, installed to local Rack
#   ./build.sh prod         # Mac arm64 prod build for distribution
#   ./build.sh dev  win     # Windows x64 dev build (cross-compiled, packaged only)
#   ./build.sh prod win     # Windows x64 prod build (cross-compiled, packaged only)
#
# Windows builds require MinGW + the Windows Rack SDK. By default the script
# looks for the SDK at $WIN_RACK_DIR (default /Users/sfs/code/Rack-SDK-win-x64).
# It also requires GNU coreutils (`gcp`) for the SDK's `cp --parents` step.

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ---------- args ----------
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo -e "${RED}Error: Bad arguments${NC}"
    echo "Usage: $0 [dev|prod] [mac|win]"
    exit 1
fi

BUILD_TARGET="$1"
PLATFORM="${2:-mac}"

case "$BUILD_TARGET" in dev|prod) ;; *)
    echo -e "${RED}Error: Invalid build target '$BUILD_TARGET'${NC}"
    echo "Valid targets: dev, prod"
    exit 1
;; esac

case "$PLATFORM" in mac|win) ;; *)
    echo -e "${RED}Error: Invalid platform '$PLATFORM'${NC}"
    echo "Valid platforms: mac, win"
    exit 1
;; esac

# ---------- platform-specific setup ----------
WIN_RACK_DIR="${WIN_RACK_DIR:-/Users/sfs/code/Rack-SDK-win-x64}"

if [ "$PLATFORM" = "win" ]; then
    # Sanity-check the cross-compile toolchain before disturbing files.
    for tool in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-strip gcp zstd; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo -e "${RED}Error: '$tool' not found in PATH.${NC}"
            echo "  Required for Windows cross-compile."
            echo "  Install: brew install mingw-w64 coreutils zstd"
            exit 1
        fi
    done
    if [ ! -d "$WIN_RACK_DIR" ]; then
        echo -e "${RED}Error: Windows Rack SDK not found at $WIN_RACK_DIR${NC}"
        echo "  Set WIN_RACK_DIR or download from https://vcvrack.com/downloads"
        exit 1
    fi
    if [ ! -f "$WIN_RACK_DIR/libRack.dll.a" ]; then
        echo -e "${RED}Error: $WIN_RACK_DIR/libRack.dll.a missing — wrong SDK?${NC}"
        exit 1
    fi
fi

# ---------- file backup / restore ----------
MAKEFILE="Makefile"
PLUGIN_JSON="plugin.json"
BACKUP_DIR=".build-backup"

echo -e "${GREEN}=== Building $PLATFORM / $BUILD_TARGET ===${NC}\n"

mkdir -p "$BACKUP_DIR"
cp "$MAKEFILE" "$BACKUP_DIR/Makefile.bak"
cp "$PLUGIN_JSON" "$BACKUP_DIR/plugin.json.bak"

cleanup() {
    echo -e "\n${YELLOW}Restoring original files...${NC}"
    mv "$BACKUP_DIR/Makefile.bak" "$MAKEFILE"
    mv "$BACKUP_DIR/plugin.json.bak" "$PLUGIN_JSON"
    rmdir "$BACKUP_DIR" 2>/dev/null || true
    echo -e "${GREEN}Files restored${NC}"
}
trap cleanup EXIT

# ---------- Makefile + plugin.json mutation ----------
echo "Configuring for $BUILD_TARGET build..."

if [ "$BUILD_TARGET" = "dev" ]; then
    sed -i.tmp '
        /^\[PROD-START\]/,/^\[PROD-END\]/ {
            /^\[PROD-START\]/n
            /^\[PROD-END\]/!s/^/# /
        }
        /^# \[DEV-START\]/,/^# \[DEV-END\]/ {
            s/^# \(FLAGS\)/\1/
            s/^# \(CFLAGS\)/\1/
            s/^# \(CXXFLAGS\)/\1/
            s/^# \(RACK_USER_DIR\)/\1/
            s/^# \(install:\)/\1/
            s/^# \([\t ]*mkdir\)/\1/
            s/^# \([\t ]*cp\)/\1/
        }
    ' "$MAKEFILE"
    rm -f "${MAKEFILE}.tmp"

    sed -i.tmp '1,/^  "slug":/ s/"slug": *"\([^"]*\)"/"slug": "\1-dev"/' "$PLUGIN_JSON"
    sed -i.tmp '1,/^  "name":/ s/"name": *"\([^"]*\)"/"name": "\1 (Dev)"/' "$PLUGIN_JSON"
    sed -i.tmp 's/"brand": *"\([^"]*\)"/"brand": "\1-dev"/' "$PLUGIN_JSON"
    sed -i.tmp 's/"version": *"\([^"]*\)"/"version": "\1-dev"/' "$PLUGIN_JSON"
    sed -i.tmp 's/-dev-dev/-dev/g' "$PLUGIN_JSON"
    sed -i.tmp 's/ (Dev) (Dev)/ (Dev)/g' "$PLUGIN_JSON"
    # Dev builds always expose every module: un-hide anything flagged
    # "hidden": true in the source plugin.json (prod keeps the flags).
    sed -i.tmp 's/"hidden": *true/"hidden": false/g' "$PLUGIN_JSON"
    rm -f "${PLUGIN_JSON}.tmp"
else
    sed -i.tmp 's/"slug": *"\([^"]*\)-dev"/"slug": "\1"/g' "$PLUGIN_JSON"
    sed -i.tmp 's/"name": *"\([^"]*\) (Dev)"/"name": "\1"/g' "$PLUGIN_JSON"
    sed -i.tmp 's/"brand": *"\([^"]*\)-dev"/"brand": "\1"/g' "$PLUGIN_JSON"
    sed -i.tmp 's/"version": *"\([^"]*\)-dev"/"version": "\1"/g' "$PLUGIN_JSON"
    rm -f "${PLUGIN_JSON}.tmp"
fi

SLUG=$(grep '"slug"' "$PLUGIN_JSON" | head -1 | sed 's/.*"slug": *"\([^"]*\)".*/\1/')
NAME=$(grep '"name"' "$PLUGIN_JSON" | head -1 | sed 's/.*"name": *"\([^"]*\)".*/\1/')
VERSION=$(grep '"version"' "$PLUGIN_JSON" | sed 's/.*"version": *"\([^"]*\)".*/\1/')
echo -e "${GREEN}Building plugin:${NC}"
echo -e "  Slug:    ${YELLOW}$SLUG${NC}"
echo -e "  Name:    ${YELLOW}$NAME${NC}"
echo -e "  Version: ${YELLOW}$VERSION${NC}\n"

# ---------- build ----------
echo "Cleaning previous build..."
make clean

if [ "$PLATFORM" = "mac" ]; then
    # ----- Mac build: use the default Mac toolchain + SDK; SDK's `make
    #       dist` packages the .vcvplugin via rsync (BSD cp safe).
    echo -e "\n${GREEN}Building (mac arm64)...${NC}"
    make dist

    if [ "$BUILD_TARGET" = "dev" ]; then
        echo -e "\n${GREEN}Installing to VCV Rack plugins folder...${NC}"
        make install
        echo -e "${GREEN}Dev build installed!${NC}"
    fi

    OUT_FILE="dist/$SLUG-$VERSION-mac-arm64.vcvplugin"
    INSTALL_DIR="$HOME/Library/Application Support/Rack2/plugins-mac-arm64/"
else
    # ----- Windows cross-compile -----
    # MinGW (Homebrew) defaults to posix threads + dynamic libgcc, so the
    # plugin would otherwise depend on libwinpthread-1.dll and libgcc_s_seh-1.dll
    # at runtime — neither shipped with Rack on Windows. The EXTRA_LDFLAGS
    # below force them static so the .vcvplugin is self-contained.
    EXTRA_LD='-static-libgcc -Wl,--whole-archive,-l:libwinpthread.a,--no-whole-archive'

    echo -e "\n${GREEN}Cross-compiling (win x64) against $WIN_RACK_DIR...${NC}"
    CC=x86_64-w64-mingw32-gcc \
    CXX=x86_64-w64-mingw32-g++ \
    STRIP=x86_64-w64-mingw32-strip \
    RACK_DIR="$WIN_RACK_DIR" \
    EXTRA_LDFLAGS="$EXTRA_LD" \
        make

    # Sanity-check: the plugin should NOT depend on MinGW runtime DLLs.
    if x86_64-w64-mingw32-objdump -p plugin.dll 2>/dev/null | \
            grep -qE 'libgcc_s|libwinpthread|libstdc\+\+'; then
        echo -e "${RED}WARNING: plugin.dll has MinGW runtime DLL dependencies.${NC}"
        echo "  It will likely crash on machines without those DLLs installed."
        x86_64-w64-mingw32-objdump -p plugin.dll | grep "DLL Name:"
    fi

    # Manual packaging: SDK's `make dist` uses GNU `cp --parents` which
    # BSD cp on macOS doesn't support. Use `gcp` (coreutils) instead.
    DIST_DIR="dist-win"
    rm -rf "$DIST_DIR"
    mkdir -p "$DIST_DIR/$SLUG"
    cp plugin.dll "$DIST_DIR/$SLUG/"
    x86_64-w64-mingw32-strip -s "$DIST_DIR/$SLUG/plugin.dll"
    gcp -r --parents res LICENSE "$PLUGIN_JSON" "$DIST_DIR/$SLUG/"

    OUT_NAME="$SLUG-$VERSION-win-x64.vcvplugin"
    (cd "$DIST_DIR" && tar --no-xattrs -c "$SLUG" | zstd -19 -o "$OUT_NAME") >/dev/null

    # Place the .vcvplugin alongside any Mac builds in dist/ for convenience.
    mkdir -p dist
    cp "$DIST_DIR/$OUT_NAME" "dist/"

    OUT_FILE="dist/$OUT_NAME"
    INSTALL_DIR=""
fi

# ---------- summary ----------
echo -e "\n${GREEN}=== Build complete ===${NC}"
echo -e "Platform: ${YELLOW}$PLATFORM${NC}"
echo -e "Target:   ${YELLOW}$BUILD_TARGET${NC}"
echo -e "Slug:     ${YELLOW}$SLUG${NC}"
echo -e "Name:     ${YELLOW}$NAME${NC}"
echo -e "Version:  ${YELLOW}$VERSION${NC}"
echo -e "Output:   ${YELLOW}$OUT_FILE${NC}"
if [ -n "$INSTALL_DIR" ] && [ "$BUILD_TARGET" = "dev" ]; then
    echo -e "Installed to: ${YELLOW}$INSTALL_DIR${NC}"
fi

# Files restored by trap.

# Build Instructions

Instructions for building the Signal Function Set VCV Rack plugin.

## Prerequisites

- VCV Rack SDK installed at `../Rack-SDK` relative to this directory
- C++ compiler with C++11 support (Xcode Command Line Tools on macOS)
- `zstd` compression tool (installed via Homebrew at `/opt/homebrew/bin/zstd`)

## PATH Setup

Homebrew binaries (including `zstd`) are not on the default shell PATH in this environment. Always prepend Homebrew to PATH before running the build script:

```bash
export PATH="/opt/homebrew/bin:$PATH"
```

## Build Commands

### Development Build (recommended during development)

```bash
export PATH="/opt/homebrew/bin:$PATH" && ./build.sh dev
```

This will:
- Compile all source files in `src/`
- Create a `.vcvplugin` package with `-dev` suffix
- Auto-install to `~/Library/Application Support/Rack2/plugins-mac-arm64/`
- The dev build coexists with production builds (different slug)

After building, restart VCV Rack to load the updated plugin.

### Production Build

```bash
export PATH="/opt/homebrew/bin:$PATH" && ./build.sh prod
```

This will:
- Compile with production settings
- Create `dist/SignalFunctionSet-{version}-mac-arm64.vcvplugin`
- Does NOT auto-install (manual install or GitHub release)

### Windows Build (native, MSYS2 / MinGW64)

Build on Windows itself — useful for reproducing VCV Library Windows-build
failures locally (the library uses the same MinGW64 toolchain) before
resubmitting.

**One-time setup:**

1. Install [MSYS2](https://www.msys2.org/) (e.g. `winget install MSYS2.MSYS2`).
2. From the **MSYS2 MinGW64** shell, install the toolchain:
   ```bash
   pacman -S --needed make tar zip unzip zstd mingw-w64-x86_64-gcc mingw-w64-x86_64-jq
   ```
3. Download the **Windows** Rack SDK (`Rack-SDK-latest-win-x64.zip`) from
   <https://vcvrack.com/downloads> and unzip it to `../Rack-SDK` (next to the
   repo), or point `WIN_RACK_DIR` at wherever you put it.

**Build (from the MSYS2 MinGW64 shell, in the repo root):**

```bash
./build.sh dev  win   # Build + auto-install to %LOCALAPPDATA%\Rack2\plugins-win-x64
./build.sh prod win   # Production .vcvplugin in dist/ (the resubmission artifact)
```

`build.sh` auto-detects the native-Windows environment via `uname`. The plugin
is statically linked (libstdc++ / libgcc / winpthread) so the resulting `.dll`
is self-contained; the script runs `objdump` afterward to confirm it pulls in no
MinGW runtime DLLs. Restart VCV Rack after a dev build to load the plugin.

> The same `./build.sh ... win` invocation **cross-compiles** when run on macOS
> instead (requires `brew install mingw-w64 coreutils zstd`). That cross-build is
> validated to be ABI-clean against the SDK, but for *guaranteed* parity with the
> exact toolchain the VCV Library uses, prefer the Docker method below.

### Windows Build (Docker — official toolchain, guaranteed parity)

`./build-win-docker.sh` builds the Windows `.vcvplugin` inside VCV's official
[`rack-plugin-toolchain`](https://github.com/VCVRack/rack-plugin-toolchain), the
same toolchain the Library compiles with. Use this when you want byte-for-byte
toolchain parity rather than the Homebrew MinGW cross-compile.

To avoid the non-redistributable macOS SDK that the stock all-platforms image
requires, we use a **Windows-only** image (`Dockerfile.win`, kept in this repo's
tooling and copied into the toolchain clone).

**One-time setup:**

1. Install Docker Desktop and start it.
2. Clone the toolchain and add the Windows-only Dockerfile:
   ```bash
   git clone -b v2 https://github.com/VCVRack/rack-plugin-toolchain ~/code/rack-plugin-toolchain
   cp Dockerfile.win ~/code/rack-plugin-toolchain/      # from this repo
   ```

**Build:**

```bash
./build-win-docker.sh
```

The first run builds the toolchain image (MinGW-w64; on Apple Silicon it runs
under `linux/amd64` emulation and can take tens of minutes). Later runs reuse the
image and only compile the plugin. Output: `plugin-build-win/*.vcvplugin`, also
copied into `dist/`.

> The plugin build bind-mounts this repo and runs `make clean/dep/dist`, so it
> wipes `build/` and `dist/` here; the Windows artifact is copied back into
> `dist/` at the end. Run `./build.sh` again afterward for a Mac build.
> Override the toolchain location with `TOOLCHAIN_DIR=...`.

### Manual Make (without build script)

```bash
RACK_DIR=../Rack-SDK make        # Build plugin.dylib
RACK_DIR=../Rack-SDK make clean  # Clean build artifacts
RACK_DIR=../Rack-SDK make dist   # Create distribution package
```

Note: The build script (`build.sh`) handles dev/prod configuration, slug suffixing, and auto-install. Prefer `./build.sh dev` over raw `make` during development.

## Build Script Behavior

`build.sh` temporarily modifies `Makefile` and `plugin.json` during the build:
- **Dev mode**: Adds `-dev` suffix to slug/name/version, enables debug flags, runs `make install`
- **Prod mode**: Uses clean slug/name/version, production optimization

Both modes restore the original files via a trap on exit, so the working directory is always clean after a build (even if it fails).

## Source Structure

The Makefile auto-discovers source files:
```
SOURCES += $(wildcard src/*.cpp)
```

Adding a new `.cpp` file to `src/` automatically includes it in the build. No Makefile changes needed for new modules.

## Adding a New Module

1. Create `src/modulename.cpp` with the module struct, widget, and model registration
2. Add `extern Model* modelModuleName;` to `src/plugin.hpp`
3. Add `p->addModel(modelModuleName);` to `src/plugin.cpp`
4. Add module entry to `plugin.json` modules array
5. Create `res/modulename.svg` for the panel
6. Run `./build.sh dev` to compile and install

## Common Issues

- **`zstd: command not found`**: Ensure PATH includes `/opt/homebrew/bin` before running the build script
- **Module not appearing in Rack**: Restart VCV Rack after building. Check that the slug in `plugin.json` matches the string passed to `createModel<>()` in the `.cpp` file
- **SVG not rendering**: VCV Rack's nanosvg does not support CSS `<style>` blocks. Use inline presentation attributes (`fill="..."`, `stroke="..."`) not CSS classes. When exporting from Illustrator, select "Presentation Attributes" under CSS Properties in SVG Options.

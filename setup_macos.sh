#!/bin/bash
#
# Splatoon 1 PC Port - one-click macOS setup (Apple Silicon, CMake flow).
#
# Builds the recompiler, recompiles YOUR OWN copy of the game into a native
# arm64 program and creates a ready-to-play Splatoon.app. The renderer stays
# Vulkan: the app carries the Khronos Vulkan loader and MoltenVK (Vulkan on
# Metal). Nothing from Nintendo is downloaded or shipped by this project: the
# game files always come from the dump you pass.
#
#   ./setup_macos.sh --dump "/path/to/dump"
#
# Re-running is safe: finished steps are skipped, your save and shader cache
# are kept. --clean rebuilds everything. Windows keeps Setup.bat, Linux
# setup.sh. Written for the bash 3.2 that macOS ships.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RECOMPILER="$ROOT/recompiler"
WORK="$ROOT/build"
DOWNLOADS="$WORK/downloads"
DEPS="$WORK/deps"

MANIFEST_DIR="$ROOT/Manifest"
BOUNDARY_PROFILE="$ROOT/profiles/agmp-v16.entries"
HOOK_PROFILE="$ROOT/profiles/agmp-v16.hooks"
MINIMUM_FUNCTION_COUNT=96300
DEPLOYMENT_TARGET=13.3

# Open-source build dependencies (official upstream releases, pinned).
SDL_URL='https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.zip'
GLSLANG_URL='https://github.com/KhronosGroup/glslang/archive/refs/tags/16.4.0.tar.gz'
VOLK_URL='https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz'
HEADERS_URL='https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz'
LOADER_URL='https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz'
MOLTENVK_URL='https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar'
SDL_SHA256=12b2dc2eb8f2836100a7916b5d394a0c82f1f7e32693f95f98305403af242f08
VOLK_SHA256=1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3
HEADERS_SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e
LOADER_SHA256=941eff558fc74f248745fb7df367640cf89a8457f216dae498a74ae14b6ba6f7
MOLTENVK_SHA256=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e

FONT_BASE_URL='https://raw.githubusercontent.com/cemu-project/Cemu/cc6fea125a5d4f559c662d13c5f4af0b6dd0876f/bin/resources/sharedFonts'
FONT_NAMES=(CafeCn.ttf CafeKr.ttf CafeStd.ttf CafeTw.ttf)
FONT_SHA256=(
    6afd751a9bb76bc788efb50a138c6f3765624ee84d8d8203be04d871ade40a6a
    c3c326f6ce1899e391dc4996b10a0c30558c5b8f36dfbaa418976862b3e2dffb
    59e2494727b8e7a7bbf48c8749f2dbc07d438c9fabeaa9847a08ec690f9888a1
    c8f9e16345dbc3b6e165fe12435486a748e730ebe6002b5d713efe73d171d51c
)
MII_NAMES=(FFLResHigh.dat FFLResHighLG.dat FFLResMiddle.dat FFLResMiddleLG.dat)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
STEP_NUMBER=0
TOTAL_START=$(date +%s)
STAGE_REPORT=""

usage() {
    cat <<'EOF'
Usage: ./setup_macos.sh [options]

  --dump PATH       Wii U dump folder (contains code/, content/, meta/)
  --output PATH     folder for the playable game (default: ./Game); the app
                    is PATH/Splatoon.app, the game data sits beside it
  --mlc PATH        optional local system-resource source; never prompted
  --clean           delete previous build outputs first
  --non-interactive never prompt; fail if input would be required
  --jobs N          parallel compile jobs (default: CPU/memory based)
  -h, --help        show this help
EOF
}

info() { printf '      %s\n' "$*"; }
good() { printf '      %s\n' "$*"; }
warn() { printf '      %s\n' "$*" >&2; }
fail() {
    printf '\nSETUP FAILED: %s\n' "$*" >&2
    printf 'If you cannot fix this, open an issue on GitHub and paste this terminal'"'"'s text.\n' >&2
    exit 1
}
step() {
    STEP_NUMBER=$((STEP_NUMBER + 1))
    printf '\n[%d/9] %s\n' "$STEP_NUMBER" "$*"
    STAGE_START=$(date +%s)
}
stage_done() { STAGE_REPORT="$STAGE_REPORT$(printf '   %-52s : %6s s' "$1" "$(( $(date +%s) - STAGE_START ))")"$'\n'; }

sha256() { shasum -a 256 -- "$1" | awk '{print $1}'; }
realpath_py() { python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"; }
ensure_folder() { mkdir -p -- "$1"; }

select_manifest() { # <rpx>
    python3 "$RECOMPILER/tools/plt_resolver.py" select --rpx "$1" --manifest-dir "$MANIFEST_DIR"
}

download() { # <url> <destination> [sha256]
    local url="$1" dest="$2" expected="${3:-}"
    if [ ! -f "$dest" ]; then
        info "Downloading $(basename "$dest") ..."
        curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 -o "$dest.part" "$url" \
            || fail "Could not download $url (internet/DNS/firewall?). Run setup_macos.sh again to retry."
        mv -f "$dest.part" "$dest"
    fi
    if [ -n "$expected" ] && [ "$(sha256 "$dest")" != "$expected" ]; then
        rm -f -- "$dest"
        fail "$(basename "$dest") failed its integrity check. Run setup_macos.sh again to retry."
    fi
}

extract_tarball() { # <archive> <destination folder>
    if [ ! -f "$2/CMakeLists.txt" ]; then
        rm -rf -- "$2"; ensure_folder "$2"
        tar -xzf "$1" -C "$2" --strip-components=1
    fi
}

find_system_resources() { # <relative/dir> <name>...
    local rel="$1"
    shift
    local roots=() start parent base suffix dir ok name i
    roots+=("$OUTPUT/mlc01")
    [ -n "$MLC" ] && roots+=("$MLC")
    [ -n "${RBREW_SYSTEM_MLC_DIR:-}" ] && roots+=("$RBREW_SYSTEM_MLC_DIR")
    [ -n "${RBREW_MLC_DIR:-}" ] && roots+=("$RBREW_MLC_DIR")
    for start in "$GAME_DUMP" "$ROOT"; do
        parent="$start"
        for i in 1 2 3 4 5; do
            roots+=("$parent")
            [ "$parent" = "/" ] && break
            parent="$(dirname -- "$parent")"
        done
    done
    # Cemu's default macOS locations, plus a configured custom MLC.
    local cemu="$HOME/Library/Application Support/Cemu"
    roots+=("$cemu" "$HOME/.local/share/Cemu")
    local settings="$cemu/settings.xml" custom=""
    if [ -f "$settings" ]; then
        custom="$(sed -n 's:.*<mlc_path>\(.*\)</mlc_path>.*:\1:p' "$settings" | head -n 1)"
        if [ -n "$custom" ]; then
            case "$custom" in /*) ;; *) custom="$cemu/$custom" ;; esac
            roots+=("$custom")
        fi
    fi
    for base in "${roots[@]}"; do
        for suffix in "" "mlc01" "mlc" "wiiu-system/mlc"; do
            dir="$base${suffix:+/$suffix}/$rel"
            ok=1
            for name in "$@"; do
                if [ ! -s "$dir/$name" ]; then ok=0; break; fi
            done
            if [ "$ok" = 1 ]; then printf '%s\n' "$dir"; return 0; fi
        done
    done
    return 1
}

copy_resource_files() { # <from> <to> <name>...
    local from="$1" to="$2" name
    shift 2
    ensure_folder "$to"
    for name in "$@"; do
        if [ "$(realpath_py "$from/$name")" != "$(realpath_py "$to/$name")" ]; then
            cp -f -- "$from/$name" "$to/$name"
        fi
    done
}

initialize_wiiu_folders() { # <game folder>
    local folder
    for folder in \
        save/common \
        mlc01/sys \
        mlc01/usr/title/00050000 \
        mlc01/usr/title/0005000c \
        mlc01/usr/title/0005000e \
        mlc01/usr/save/00050000 \
        mlc01/usr/boss \
        mlc01/usr/save/00050010/1004a000/user/common/db \
        mlc01/usr/save/00050010/1004a100/user/common/db \
        mlc01/usr/save/00050010/1004a200/user/common/db \
        mlc01/sys/title/0005001b/10042400/content \
        mlc01/sys/title/0005001b/10056000/content \
        mlc01/sys/title/0005001b/1005c000/content; do
        ensure_folder "$1/$folder"
    done
}

prepare_wiiu_resources() {
    initialize_wiiu_folders "$OUTPUT"
    local font_rel='sys/title/0005001b/10042400/content'
    local font_dest="$OUTPUT/mlc01/$font_rel"
    local font_src=""
    font_src="$(find_system_resources "$font_rel" "${FONT_NAMES[@]}")" || true
    if [ -n "$font_src" ]; then
        copy_resource_files "$font_src" "$font_dest" "${FONT_NAMES[@]}"
        good "Shared fonts ready: $font_src"
    else
        info 'Installing the same Noto replacement fonts supplied with Cemu ...'
        local font_cache="$DOWNLOADS/shared-fonts" i
        ensure_folder "$font_cache"
        for i in "${!FONT_NAMES[@]}"; do
            download "$FONT_BASE_URL/${FONT_NAMES[$i]}" "$font_cache/${FONT_NAMES[$i]}" "${FONT_SHA256[$i]}"
        done
        copy_resource_files "$font_cache" "$font_dest" "${FONT_NAMES[@]}"
        ensure_folder "$OUTPUT/licenses"
        cp -f -- "$ROOT/LICENSES/cemu-shared-fonts-OFL-1.1.txt" "$OUTPUT/licenses/"
        good 'Noto replacement fonts installed.'
    fi
    local mii_rel='sys/title/0005001b/10056000/content'
    local mii_src=""
    mii_src="$(find_system_resources "$mii_rel" "${MII_NAMES[@]}")" || true
    if [ -n "$mii_src" ]; then
        copy_resource_files "$mii_src" "$OUTPUT/mlc01/$mii_rel" "${MII_NAMES[@]}"
        good "Local Mii graphics found automatically: $mii_src"
    else
        warn 'No local Mii graphics resources found; some Mii graphics may be unavailable.'
    fi
    good 'Wii U folders initialized automatically; no saves or accounts imported.'
}

generated_module_complete() { # <generated dir>
    local dir="$1" name registered
    for name in Gambit.h Gambit_data.cpp Gambit_register.cpp; do
        [ -f "$dir/$name" ] || return 1
    done
    registered="$(grep -c '^[[:space:]]*rbrew_register_func(' "$dir/Gambit_register.cpp" || true)"
    [ "$registered" -ge "$MINIMUM_FUNCTION_COUNT" ]
}

# Copy a game-data folder. A new copy is an APFS clone (no extra disk space,
# no copy time); an existing copy is refreshed with newer files only.
copy_tree() { # <from> <to>
    if [ ! -e "$2" ]; then
        cp -cR -- "$1" "$2" 2>/dev/null || cp -R -- "$1" "$2"
    else
        rsync -a --update -- "$1/" "$2/"
    fi
}

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
GAME_DUMP=""
OUTPUT=""
MLC=""
CLEAN=0
NONINTERACTIVE=0
JOBS="${JOBS:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --dump|-GameDump)  GAME_DUMP="${2:-}"; shift 2 ;;
        --output|-Output)  OUTPUT="${2:-}"; shift 2 ;;
        --mlc|-Mlc)        MLC="${2:-}"; shift 2 ;;
        --clean|-Clean)    CLEAN=1; shift ;;
        --non-interactive) NONINTERACTIVE=1; shift ;;
        --jobs|-Jobs)      JOBS="${2:-}"; shift 2 ;;
        -h|--help)         usage; exit 0 ;;
        *)                 usage >&2; fail "Unknown option: $1" ;;
    esac
done

printf '============================================================\n'
printf '   Splatoon 1 PC Port - macOS setup (Apple Silicon)\n'
printf '   Uses only YOUR OWN game files.\n'
printf '============================================================\n'

# ---------------------------------------------------------------------------
step 'Checking the tools needed to build'
[ "$(uname -s)" = Darwin ] || fail 'setup_macos.sh is for macOS. Use setup.sh on Linux and Setup.bat on Windows.'
[ "$(uname -m)" = arm64 ] || fail 'This port targets Apple Silicon (arm64). Run it natively, not under Rosetta.'
xcode-select -p >/dev/null 2>&1 || fail 'The Xcode Command Line Tools are missing. Run "xcode-select --install" and then setup_macos.sh again.'
for tool in clang++ python3 curl tar shasum rsync codesign; do
    command -v "$tool" >/dev/null 2>&1 || fail "'$tool' is missing."
done
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)' \
    || fail "Python $(python3 --version | awk '{print $2}') is too old; 3.12 or newer is required (python.org installer)."

# The SDK must link with the installed linker: a Command Line Tools update
# can ship an SDK whose library stubs the linker does not read yet.
probe="$WORK/sdk-probe"
ensure_folder "$probe"
printf 'int main(){return 0;}\n' > "$probe/probe.cpp"
if [ -z "${SDKROOT:-}" ] || ! clang++ -isysroot "$SDKROOT" "$probe/probe.cpp" -o "$probe/probe" >/dev/null 2>&1; then
    SDKROOT=""
    for candidate in "$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)" \
                     $(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.sdk 2>/dev/null | sort -t X -k 2 -V -r); do
        [ -d "$candidate" ] || continue
        if clang++ -isysroot "$candidate" "$probe/probe.cpp" -o "$probe/probe" >/dev/null 2>&1; then
            SDKROOT="$candidate"; break
        fi
    done
fi
[ -n "$SDKROOT" ] || fail 'No macOS SDK links with the installed Command Line Tools. Reinstall them ("xcode-select --install").'
export SDKROOT
good "SDK: $SDKROOT"

# CMake >= 3.20 and Ninja: the system ones when present, else a private
# Python environment inside build/ (no Homebrew needed).
CMAKE="$(command -v cmake || true)"
NINJA="$(command -v ninja || true)"
if [ -n "$CMAKE" ]; then
    v="$("$CMAKE" --version | head -n 1 | awk '{print $3}')"
    [ "$(printf '%s\n' '3.20' "$v" | sort -V | head -n 1)" = '3.20' ] || CMAKE=""
fi
if [ -z "$CMAKE" ] || [ -z "$NINJA" ]; then
    VENV="$WORK/tools/venv"
    if [ ! -x "$VENV/bin/cmake" ] || [ ! -x "$VENV/bin/ninja" ]; then
        info 'Installing CMake and Ninja into build/tools (pip) ...'
        python3 -m venv "$VENV" || fail 'Could not create a Python environment for CMake.'
        "$VENV/bin/pip" install -q cmake ninja || fail 'Could not install CMake/Ninja with pip.'
    fi
    [ -n "$CMAKE" ] || CMAKE="$VENV/bin/cmake"
    [ -n "$NINJA" ] || NINJA="$VENV/bin/ninja"
fi
export PATH="$(dirname "$NINJA"):$(dirname "$CMAKE"):$PATH"
GENERATOR=Ninja
good "CMake $("$CMAKE" --version | head -n 1 | awk '{print $3}') · Ninja $("$NINJA" --version) · $(clang++ --version | head -n 1)"

if [ -z "$JOBS" ]; then
    JOBS="$(sysctl -n hw.ncpu)"
    mem_jobs=$(( $(sysctl -n hw.memsize) / (3 * 1024 * 1024 * 1024) ))
    [ "$mem_jobs" -lt 1 ] && mem_jobs=1
    [ "$JOBS" -gt "$mem_jobs" ] && JOBS="$mem_jobs"
fi
info "Compile jobs: $JOBS (override with --jobs N)"
APPLE_ARGS=(-DCMAKE_OSX_ARCHITECTURES=arm64 "-DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET"
            "-DCMAKE_OSX_SYSROOT=$SDKROOT" -DCMAKE_POLICY_VERSION_MINIMUM=3.5)

if [ "$CLEAN" = 1 ] && [ -d "$WORK" ]; then
    info 'Removing previous build outputs (--clean) ...'
    find "$WORK" -mindepth 1 -maxdepth 1 ! -name downloads ! -name tools -exec rm -rf {} +
fi
ensure_folder "$DOWNLOADS"
stage_done '1. Toolchain & environment check'

# ---------------------------------------------------------------------------
step 'Finding your game files'
if [ -z "$GAME_DUMP" ]; then
    for candidate in "$HOME"/Downloads/* "$ROOT/dump" "$ROOT/game-dump"; do
        if [ -f "$candidate/code/Gambit.rpx" ] && select_manifest "$candidate/code/Gambit.rpx" >/dev/null 2>&1; then
            GAME_DUMP="$candidate"
            good "Automatically detected valid game dump: $GAME_DUMP"
            break
        fi
    done
fi
if [ -z "$GAME_DUMP" ]; then
    if [ "$NONINTERACTIVE" = 1 ] || [ ! -t 0 ]; then
        fail 'No game dump given. Pass --dump "/path/to/Splatoon dump".'
    fi
    info 'Drag the folder of your Splatoon dump here (it contains code, content and meta) and press Return.'
    printf '      Dump folder: '
    IFS= read -r GAME_DUMP || fail 'No game folder was selected.'
    GAME_DUMP="${GAME_DUMP%\'}"; GAME_DUMP="${GAME_DUMP#\'}"; GAME_DUMP="${GAME_DUMP%\"}"; GAME_DUMP="${GAME_DUMP#\"}"
    GAME_DUMP="${GAME_DUMP%/}"; GAME_DUMP="${GAME_DUMP//\\ / }"
fi
[ -n "$GAME_DUMP" ] && [ -d "$GAME_DUMP" ] || fail "The game dump folder does not exist: $GAME_DUMP"
GAME_DUMP="$(realpath_py "$GAME_DUMP")"
for folder in code content meta; do
    [ -d "$GAME_DUMP/$folder" ] || fail "Your dump is missing the $folder folder. Select the folder that contains code, content and meta."
done
rpx="$GAME_DUMP/code/Gambit.rpx"
[ -f "$rpx" ] || fail 'Your dump is missing code/Gambit.rpx.'
[ -f "$GAME_DUMP/meta/meta.xml" ] || fail 'Your dump is missing meta/meta.xml.'
info 'Checking the game version (this takes a few seconds) ...'
rpx_hash="$(sha256 "$rpx")"
SELECTED_MANIFEST="$(select_manifest "$rpx")" \
    || fail "Unsupported game version. None of the manifests in $MANIFEST_DIR describes your code/Gambit.rpx (SHA-256 $rpx_hash)."
good "Matched manifest: $SELECTED_MANIFEST"
[ -n "$OUTPUT" ] || OUTPUT="$ROOT/Game"
ensure_folder "$OUTPUT"
OUTPUT="$(realpath_py "$OUTPUT")"
info "The game will be installed to: $OUTPUT/Splatoon.app"
prepare_wiiu_resources
stage_done '2. Game validation & resources'

# ---------------------------------------------------------------------------
step 'Building the window and shader-compiler libraries (SDL2, glslang)'
SDL_ZIP="$DOWNLOADS/SDL2-2.32.10.zip"
GLSLANG_ARCHIVE="$DOWNLOADS/glslang-16.4.0.tar.gz"
download "$SDL_URL" "$SDL_ZIP" "$SDL_SHA256"
download "$GLSLANG_URL" "$GLSLANG_ARCHIVE"   # SHA-512 checked by the builder
SDL_SOURCE="$WORK/sdl-source-archive/SDL2-2.32.10"
if [ ! -d "$SDL_SOURCE" ]; then
    python3 -c 'import zipfile, sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$SDL_ZIP" "$WORK/sdl-source-archive"
fi
cmake_args=()
for a in "${APPLE_ARGS[@]}"; do cmake_args+=("--cmake-arg=$a"); done
python3 "$RECOMPILER/tools/build_vulkan_dependencies.py" \
    --sdl-source "$SDL_SOURCE" --glslang-archive "$GLSLANG_ARCHIVE" \
    --build-root "$WORK/deps-build" --prefix "$DEPS" --generator "$GENERATOR" \
    --cmake "$CMAKE" "${cmake_args[@]}" \
    || fail 'Building SDL2/glslang'
good 'Libraries ready.'
stage_done '3. SDL2 & glslang (Vulkan dependency profile)'

# ---------------------------------------------------------------------------
step 'Building the Vulkan headers, loader and volk; fetching MoltenVK'
download "$HEADERS_URL" "$DOWNLOADS/vulkan-headers-1.4.363.tar.gz" "$HEADERS_SHA256"
download "$LOADER_URL" "$DOWNLOADS/vulkan-loader-1.4.363.tar.gz" "$LOADER_SHA256"
download "$VOLK_URL" "$DOWNLOADS/volk-1.4.363.tar.gz" "$VOLK_SHA256"
download "$MOLTENVK_URL" "$DOWNLOADS/MoltenVK-macos-1.4.2.tar" "$MOLTENVK_SHA256"
HEADERS_PREFIX="$DEPS/vulkan-headers"
if [ ! -f "$HEADERS_PREFIX/share/cmake/VulkanHeaders/VulkanHeadersConfig.cmake" ]; then
    extract_tarball "$DOWNLOADS/vulkan-headers-1.4.363.tar.gz" "$WORK/vulkan-headers-source"
    "$CMAKE" -S "$WORK/vulkan-headers-source" -B "$WORK/vulkan-headers-build" -G "$GENERATOR" "${APPLE_ARGS[@]}" \
        -DCMAKE_INSTALL_PREFIX="$HEADERS_PREFIX" >/dev/null || fail 'Configuring Vulkan-Headers'
    "$CMAKE" --build "$WORK/vulkan-headers-build" --target install >/dev/null || fail 'Installing Vulkan-Headers'
fi
VOLK_PREFIX="$DEPS/volk"
if [ ! -d "$VOLK_PREFIX/lib/cmake/volk" ]; then
    extract_tarball "$DOWNLOADS/volk-1.4.363.tar.gz" "$WORK/volk-source"
    "$CMAKE" -S "$WORK/volk-source" -B "$WORK/volk-build" -G "$GENERATOR" "${APPLE_ARGS[@]}" \
        -DCMAKE_BUILD_TYPE=Release -DVOLK_INSTALL=ON -DVULKAN_HEADERS_INSTALL_DIR="$HEADERS_PREFIX" \
        -DCMAKE_INSTALL_PREFIX="$VOLK_PREFIX" >/dev/null || fail 'Configuring volk'
    "$CMAKE" --build "$WORK/volk-build" --target install --parallel "$JOBS" >/dev/null || fail 'Building volk'
fi
LOADER_PREFIX="$DEPS/vulkan-loader"
if [ ! -f "$LOADER_PREFIX/lib/libvulkan.1.dylib" ]; then
    extract_tarball "$DOWNLOADS/vulkan-loader-1.4.363.tar.gz" "$WORK/vulkan-loader-source"
    "$CMAKE" -S "$WORK/vulkan-loader-source" -B "$WORK/vulkan-loader-build" -G "$GENERATOR" "${APPLE_ARGS[@]}" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HEADERS_PREFIX" -DBUILD_TESTS=OFF \
        -DCMAKE_INSTALL_PREFIX="$LOADER_PREFIX" >/dev/null || fail 'Configuring the Vulkan loader'
    "$CMAKE" --build "$WORK/vulkan-loader-build" --target install --parallel "$JOBS" >/dev/null || fail 'Building the Vulkan loader'
fi
MOLTENVK="$WORK/MoltenVK"
if [ ! -f "$MOLTENVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" ]; then
    rm -rf -- "$MOLTENVK"; ensure_folder "$WORK/moltenvk-extract"
    tar -xf "$DOWNLOADS/MoltenVK-macos-1.4.2.tar" -C "$WORK/moltenvk-extract"
    mv "$WORK/moltenvk-extract/MoltenVK" "$MOLTENVK"; rm -rf -- "$WORK/moltenvk-extract"
fi
[ -f "$MOLTENVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" ] || fail 'MoltenVK archive has an unexpected layout.'
good 'Vulkan loader 1.4.363, volk and MoltenVK 1.4.2 ready.'
stage_done '4. Vulkan loader, volk, MoltenVK'

# ---------------------------------------------------------------------------
step 'Building the recompiler (RebrewU)'
REBREWU_BUILD="$WORK/rebrewu"
"$CMAKE" -S "$RECOMPILER" -B "$REBREWU_BUILD" -G "$GENERATOR" "${APPLE_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Release -DREBREWU_BUILD_TESTS=OFF || fail 'Configuring the recompiler'
"$CMAKE" --build "$REBREWU_BUILD" --target rebrewu --parallel "$JOBS" || fail 'Building the recompiler'
REBREWU="$REBREWU_BUILD/bin/rebrewu"
[ -x "$REBREWU" ] || fail 'rebrewu was not produced.'
stage_done '5. RebrewU recompiler build'

# ---------------------------------------------------------------------------
step 'Recompiling your Gambit.rpx into C++ (a few minutes)'
GENERATED="$WORK/generated/Gambit"
STAMP="$WORK/generated/source.sha256"
[ -f "$BOUNDARY_PROFILE" ] && [ -f "$HOOK_PROFILE" ] || fail 'The supported-game profiles are missing. Extract the complete repository and retry.'
current="$rpx_hash:$(sha256 "$REBREWU"):$(sha256 "$BOUNDARY_PROFILE"):$(sha256 "$HOOK_PROFILE"):$(sha256 "$SELECTED_MANIFEST")"
previous=""
[ -f "$STAMP" ] && previous="$(cat -- "$STAMP")"
if ! generated_module_complete "$GENERATED" || [ "$current" != "$previous" ]; then
    ensure_folder "$GENERATED"
    "$REBREWU" recompile --rpx "$rpx" --output "$GENERATED" --manifest "$SELECTED_MANIFEST" \
        --analyzer-entries "$BOUNDARY_PROFILE" --hooks "$HOOK_PROFILE" || fail 'Recompiling Gambit.rpx'
    generated_module_complete "$GENERATED" \
        || fail 'The recompiler did not produce the complete supported-game function table.'
    printf '%s\n' "$current" > "$STAMP"
fi
good 'Game code recompiled (stays on your Mac, inside build/generated).'
stage_done '6. RPX decompilation to C++'

# ---------------------------------------------------------------------------
step 'Building the game (this is the long part)'
GAME_BUILD="$WORK/game"
SDL2_CONFIG="$(find "$DEPS/sdl2-vulkan" -name SDL2Config.cmake -print -quit)"
[ -n "$SDL2_CONFIG" ] || fail 'SDL2Config.cmake not found in the dependency profile.'
GLSLANG_DIR="$DEPS/glslang-vulkan/lib/cmake/glslang"
"$CMAKE" -S "$RECOMPILER/port" -B "$GAME_BUILD" -G "$GENERATOR" "${APPLE_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Release -DRBREW_GRAPHICS_BACKEND=Vulkan -DRBREW_RPX="$rpx" \
    -DGAMBIT_GENERATED_DIR="$GENERATED" -DSDL2_DIR="$(dirname -- "$SDL2_CONFIG")" -Dglslang_DIR="$GLSLANG_DIR" \
    -DCMAKE_PREFIX_PATH="$VOLK_PREFIX;$HEADERS_PREFIX" \
    -DRBREW_BUILD_GAME=ON -DRBREW_ENABLE_COVERAGE=OFF -DRBREW_ENABLE_PPC_FALLBACK=OFF \
    -DRBREW_DIAGNOSTIC_RUNTIME=OFF -DRBREW_TEXTURE_DIAGNOSTICS=OFF || fail 'Configuring the game'
"$CMAKE" --build "$GAME_BUILD" --target Gambit --parallel "$JOBS" || fail 'Building the game'
EXE="$GAME_BUILD/Splatoon"
[ -x "$EXE" ] || fail 'The Splatoon executable was not produced.'
lipo -archs "$EXE" | grep -qx arm64 || fail 'The game was not built as a native arm64 program.'
stage_done '7. Game compilation & linking'

# ---------------------------------------------------------------------------
step 'Creating Splatoon.app'
APP="$OUTPUT/Splatoon.app"
STAGING="$OUTPUT/.Splatoon.app.new"
rm -rf -- "$STAGING"
ensure_folder "$STAGING/Contents/MacOS"
ensure_folder "$STAGING/Contents/Frameworks"
ensure_folder "$STAGING/Contents/Resources/vulkan/icd.d"
cp -f -- "$EXE" "$STAGING/Contents/MacOS/Splatoon"
strip -x "$STAGING/Contents/MacOS/Splatoon"
cp -f -- "$LOADER_PREFIX/lib/libvulkan.1.dylib" "$STAGING/Contents/Frameworks/libvulkan.1.dylib"
cp -f -- "$MOLTENVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$STAGING/Contents/Frameworks/"
# The loader finds drivers in Contents/Resources/vulkan/icd.d of the running
# bundle; library_path is relative to the manifest.
cat > "$STAGING/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../Frameworks/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
EOF
ICON_KEY=""
if sips -s format icns "$RECOMPILER/port/resources/splatoon.ico" --out "$STAGING/Contents/Resources/Splatoon.icns" >/dev/null 2>&1; then
    ICON_KEY='    <key>CFBundleIconFile</key><string>Splatoon</string>'
fi
cat > "$STAGING/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>Splatoon</string>
    <key>CFBundleIdentifier</key><string>io.github.splatpc.splatoon</string>
    <key>CFBundleName</key><string>Splatoon</string>
    <key>CFBundleDisplayName</key><string>Splatoon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
$ICON_KEY
    <key>LSMinimumSystemVersion</key><string>$DEPLOYMENT_TARGET</string>
    <key>LSArchitecturePriority</key><array><string>arm64</string></array>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>GCSupportsControllerUserInteraction</key><true/>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
EOF
# Ad-hoc signature (arm64 code must be signed to run); local build only.
codesign --force --sign - "$STAGING/Contents/Frameworks/libvulkan.1.dylib" >/dev/null
codesign --force --sign - "$STAGING/Contents/Frameworks/libMoltenVK.dylib" >/dev/null
codesign --force --sign - "$STAGING" >/dev/null
codesign --verify --strict "$STAGING" || fail 'The app bundle signature does not verify.'
rm -rf -- "$APP"
mv -- "$STAGING" "$APP"
initialize_wiiu_folders "$OUTPUT"
info 'Copying game content (APFS clone: no extra disk space on the same volume) ...'
copy_tree "$GAME_DUMP/content" "$OUTPUT/content"
copy_tree "$GAME_DUMP/meta" "$OUTPUT/meta"
good 'Game folder ready. No existing game or Mii saves were imported.'
stage_done '8. App bundle & game folder'

# ---------------------------------------------------------------------------
step 'Done'
printf '\n============================================================\n'
printf '   Setup complete!\n'
printf '   Play: open "%s"\n' "$APP"
printf '============================================================\n'
printf '\n------------------------------------------------------------\n'
printf '   BUILD PERFORMANCE REPORT (Total: %ss)\n' "$(( $(date +%s) - TOTAL_START ))"
printf '%s\n' '------------------------------------------------------------'
printf '%s' "$STAGE_REPORT"
printf '%s\n' '------------------------------------------------------------'

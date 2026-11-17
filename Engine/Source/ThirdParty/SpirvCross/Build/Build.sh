#!/bin/sh
# Offline build of the SPIRV-Cross C API shared library (spirv-cross-c-shared) (macOS arm64). The Windows version is Build.ps1 in the same folder.
# Status: unverified (no macOS host has a record of running this script yet, Docs/Design/ShaderSystem.ko.md section 8 progress).
#
# Usage: sh Engine/Source/ThirdParty/SpirvCross/Build/Build.sh [work directory] [archive output directory]
# Requires: Xcode command line tools (clang, install_name_tool, codesign, dsymutil), CMake, Ninja, git (to apply patches).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../../../../../Build/ThirdPartyTools/tool-build.sh"
library=$(dirname "$here")
work=${1:-${TMPDIR:-/tmp}/rocks-tool-build/SpirvCross}
output=${2:-$work/archives}
jobs=$(sysctl -n hw.ncpu)

rocks_initialize_source_tree "$library" "$work"
source="$work/source"
rocks_assert_committed_files "$library" "$source"

build="$work/build"
cmake -G Ninja -S "$source" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -g" \
    -DSPIRV_CROSS_SHARED=ON -DSPIRV_CROSS_STATIC=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SKIP_INSTALL=ON
cmake --build "$build" --target spirv-cross-c-shared -j "$jobs"

stage="$work/stage"
rm -rf "$stage"
mkdir -p "$stage/lib"
built=$(find "$build" -maxdepth 1 -name 'libspirv-cross-c-shared.*.dylib' -type f | head -n 1)
cp "$built" "$stage/lib/libspirv-cross-c-shared.dylib"
dylib="$stage/lib/libspirv-cross-c-shared.dylib"
dsymutil "$dylib" -o "$dylib.dSYM"
strip -x "$dylib"
rocks_fix_dylib "$dylib"
cp "$source/LICENSE" "$stage/"

set -- "lib/libspirv-cross-c-shared.dylib=runtime" "LICENSE=notice"
for symbol in $(cd "$stage" && find lib/libspirv-cross-c-shared.dylib.dSYM -type f); do
    set -- "$@" "$symbol=symbols"
done

rocks_tool_archive "$library" "$stage" "$output" "$@"

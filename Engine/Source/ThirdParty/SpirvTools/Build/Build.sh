#!/bin/sh
# Offline build of the SPIRV-Tools C API shared library (macOS arm64). The Windows version is Build.ps1 in the same folder.
# Builds the libspirv.h C API (spvValidate and the like, plus the optimizer C API) as a single libSPIRV-Tools-c.dylib (CMake wrapper SpirvToolsC.cmake; see
# Build.ps1 for why). The upstream CMake generates grammar tables with Python 3 (PATH).
# Status: unverified (no macOS host has a record of running this script yet, Docs/Design/ShaderSystem section 8 progress).
#
# Usage: sh Engine/Source/ThirdParty/SpirvTools/Build/Build.sh [work directory] [archive output directory]
# Requires: Xcode command line tools, CMake, Ninja, Python 3, git (to apply patches).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../../../../../Build/ThirdPartyTools/tool-build.sh"
library=$(dirname "$here")
work=${1:-${TMPDIR:-/tmp}/rocks-tool-build/SpirvTools}
output=${2:-$work/archives}
jobs=$(sysctl -n hw.ncpu)

rocks_initialize_source_tree "$library" "$work"
source="$work/source"
rocks_assert_committed_files "$library" "$source"

wrapper="$work/wrapper"
mkdir -p "$wrapper"
cp "$here/SpirvToolsC.cmake" "$wrapper/CMakeLists.txt"
build="$work/build"
cmake -G Ninja -S "$wrapper" -B "$build" -DCMAKE_BUILD_TYPE=Release "-DROCKS_SPIRV_TOOLS_SOURCE=$source" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -g"
cmake --build "$build" --target SPIRV-Tools-c -j "$jobs"

stage="$work/stage"
rm -rf "$stage"
mkdir -p "$stage/lib"
dylib="$stage/lib/libSPIRV-Tools-c.dylib"
cp -L "$build/libSPIRV-Tools-c.dylib" "$dylib"
dsymutil "$dylib" -o "$dylib.dSYM"
strip -x "$dylib"
rocks_fix_dylib "$dylib"
cp "$source/LICENSE" "$stage/"

set -- "lib/libSPIRV-Tools-c.dylib=runtime" "LICENSE=notice"
for symbol in $(cd "$stage" && find lib/libSPIRV-Tools-c.dylib.dSYM -type f); do
    set -- "$@" "$symbol=symbols"
done

rocks_tool_archive "$library" "$stage" "$output" "$@"

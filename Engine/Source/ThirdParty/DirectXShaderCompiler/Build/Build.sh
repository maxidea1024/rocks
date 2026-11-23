#!/bin/sh
# Offline build of DirectXShaderCompiler (DXC) (macOS arm64). The Windows version is Build.ps1 in the same folder. Like UE 5.8
# ThirdParty/ShaderConductor/Build_ShaderConductor_Mac.command, builds with CMake and Ninja and fixes up install names and dSYMs.
# The upstream build uses exceptions and RTTI and generates tables with Python 3 (PATH).
# Status: unverified (no macOS host has a record of running this script yet, Docs/Design/ShaderSystem section 8 progress).
#
# Usage: sh Engine/Source/ThirdParty/DirectXShaderCompiler/Build/Build.sh [work directory] [archive output directory]
# Requires: Xcode command line tools, CMake, Ninja, Python 3, git (to apply patches).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../../../../../Build/ThirdPartyTools/tool-build.sh"
library=$(dirname "$here")
work=${1:-${TMPDIR:-/tmp}/rocks-tool-build/Dxc}
output=${2:-$work/archives}
jobs=$(sysctl -n hw.ncpu)

rocks_initialize_source_tree "$library" "$work"
source="$work/source"
rocks_assert_committed_files "$library" "$source"

build="$work/build"
cmake -G Ninja -S "$source" -B "$build" -C "$source/cmake/caches/PredefinedParams.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -g" \
    "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG -g" -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF -DLLVM_APPEND_VC_REV=OFF -DHLSL_DISABLE_SOURCE_GENERATION=ON
cmake --build "$build" --target dxcompiler dxc -j "$jobs"

stage="$work/stage"
rm -rf "$stage"
mkdir -p "$stage/lib" "$stage/bin"
dylib="$stage/lib/libdxcompiler.dylib"
cp -L "$build/lib/libdxcompiler.dylib" "$dylib"
dsymutil "$dylib" -o "$dylib.dSYM"
strip -x "$dylib"
rocks_fix_dylib "$dylib"
tool="$stage/bin/dxc"
cp -L "$build/bin/dxc" "$tool"
dsymutil "$tool" -o "$tool.dSYM"
strip -x "$tool"
# dxc uses libdxcompiler.dylib in ../lib.
for dependency in $(otool -L "$tool" | tail -n +2 | awk '{print $1}'); do
    case "$dependency" in
        @rpath/libdxcompiler*.dylib) install_name_tool -change "$dependency" "@loader_path/../lib/libdxcompiler.dylib" "$tool" ;;
    esac
done
codesign --force --sign - "$tool"
cp "$source/LICENSE.TXT" "$source/ThirdPartyNotices.txt" "$stage/"

set -- "lib/libdxcompiler.dylib=runtime" "bin/dxc=tool" "LICENSE.TXT=notice" "ThirdPartyNotices.txt=notice"
for symbol in $(cd "$stage" && find lib/libdxcompiler.dylib.dSYM bin/dxc.dSYM -type f); do
    set -- "$@" "$symbol=symbols"
done

rocks_tool_archive "$library" "$stage" "$output" "$@"

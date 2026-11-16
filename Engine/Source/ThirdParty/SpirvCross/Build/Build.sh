#!/bin/sh
# SPIRV-Cross C API 공유 라이브러리(spirv-cross-c-shared) 오프라인 빌드다(macOS arm64). Windows 판은 같은 폴더의 Build.ps1이다.
# 상태: 미검증(이 스크립트를 실행한 macOS 호스트 기록이 아직 없다, Docs/Design/ShaderSystem.ko.md 8절 진척).
#
# 사용: sh Engine/Source/ThirdParty/SpirvCross/Build/Build.sh [작업 디렉터리] [archive 출력 디렉터리]
# 필요: Xcode 명령행 도구(clang·install_name_tool·codesign·dsymutil), CMake, Ninja, git(patch 적용).
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

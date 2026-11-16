#!/bin/sh
# SPIRV-Tools C API 공유 라이브러리 오프라인 빌드다(macOS arm64). Windows 판은 같은 폴더의 Build.ps1이다.
# libspirv.h C API(spvValidate 등과 최적화기 C API)를 libSPIRV-Tools-c.dylib 하나로 만든다(CMake 래퍼 SpirvToolsC.cmake, 이유는
# Build.ps1). 업스트림 CMake가 Python 3(PATH)으로 문법 표를 생성한다.
# 상태: 미검증(이 스크립트를 실행한 macOS 호스트 기록이 아직 없다, Docs/Design/ShaderSystem.ko.md 8절 진척).
#
# 사용: sh Engine/Source/ThirdParty/SpirvTools/Build/Build.sh [작업 디렉터리] [archive 출력 디렉터리]
# 필요: Xcode 명령행 도구, CMake, Ninja, Python 3, git(patch 적용).
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

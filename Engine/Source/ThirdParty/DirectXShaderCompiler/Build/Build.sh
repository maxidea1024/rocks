#!/bin/sh
# DirectXShaderCompiler(DXC) 오프라인 빌드다(macOS arm64). Windows 판은 같은 폴더의 Build.ps1이다. UE 5.8
# ThirdParty/ShaderConductor/Build_ShaderConductor_Mac.command처럼 CMake·Ninja로 빌드하고 install name·dSYM을 정리한다.
# 업스트림 빌드는 예외·RTTI를 쓰며 Python 3(PATH)으로 표를 생성한다.
# 상태: 미검증(이 스크립트를 실행한 macOS 호스트 기록이 아직 없다, Docs/Design/ShaderSystem.ko.md 8절 진척).
#
# 사용: sh Engine/Source/ThirdParty/DirectXShaderCompiler/Build/Build.sh [작업 디렉터리] [archive 출력 디렉터리]
# 필요: Xcode 명령행 도구, CMake, Ninja, Python 3, git(patch 적용).
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
# dxc는 ../lib의 libdxcompiler.dylib를 쓴다.
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

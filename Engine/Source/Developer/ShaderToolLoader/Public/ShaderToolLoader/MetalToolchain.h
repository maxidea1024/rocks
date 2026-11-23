#pragma once

#include <Base/Types.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Text/StringView.h>
#include <ShaderToolLoader/ShaderToolLoader.h>

// Apple Metal 도구(system 도구 MetalToolchain) 찾기와 지문이다(Docs/Design/ShaderSystem 11.1절
// SH1e E7). 저장소 도구와 달리 공유 라이브러리로 열지 않고 실행 파일(metal·metallib)을 P1
// RunProcess로 실행한다.
//
// 찾는 곳(UE 5.8 MetalShaderFormat.cpp 대응):
// - macOS: /usr/bin/xcrun --sdk macosx --find metal·metallib.
// - Windows: Metal Developer Tools for Windows 기본 설치 위치
//   %ProgramFiles%/Metal Developer Tools/metal/<MetalWindowsToolchainSubversion>/bin/metal.exe·
//   metallib.exe(UE 기대 버전 5.3).
// - 그 밖(Android): 없다.
// 두 실행 파일이 있고 `metal -v`에서 버전 줄을 읽으면 found다. 지문은 버전 줄 + Target 줄(설치
// 경로가 든 줄은 뺀다)과 두 실행 파일의 SHA-256이다(10절 키 4행).
//
// 사용 예(워커 도구 조회):
//   MetalToolchain metal;
//   LocateMetalToolchain(metal);
//   if (metal.found) { ... metal.version, metal.sha256[MetalCompilerIndex] ... }
namespace rocks::shader {

// Windows Metal Developer Tools의 설치 하위 폴더(UE 5.8 WindowsToolchainSubversion)다.
inline constexpr char MetalWindowsToolchainSubversion[] = "32023";
// metal -v 버전 칸(버전 줄 + 줄바꿈 + Target 줄, NUL 포함)의 크기다.
inline constexpr usize MaxMetalVersionBytes = 512;
// 실행 파일 순서다.
inline constexpr usize MetalCompilerIndex = 0;
inline constexpr usize MetalLibrarianIndex = 1;
inline constexpr usize MetalToolExecutables = 2;
// 도구 실행 시간 상한이다(실패 판정에만 쓴다).
inline constexpr uint32 MetalToolTimeoutMilliseconds = 120'000;

struct MetalToolchain {
    bool found = false;
    // metal·metallib 절대 경로(UTF-8, NUL 종료)다.
    char executables[MetalToolExecutables][MaxShaderToolPathBytes] = {};
    // 버전 줄과 Target 줄("\n"으로 이음)이다.
    char version[MaxMetalVersionBytes] = {};
    Sha256Digest sha256[MetalToolExecutables] = {};
};

// 이 호스트의 Metal 도구를 찾아 out에 둔다(out은 먼저 비운다). 찾지 못하거나 버전을 읽지 못하면
// found가 false다(오류가 아니다 — 그 형식 job이 UnsupportedCapability가 된다).
ROCKS_SHADERTOOLLOADER_API void LocateMetalToolchain(MetalToolchain& out) noexcept;

// `metal -v` 출력에서 버전 줄("Apple "로 시작하고 " version "이 든 줄)과 "Target:" 줄을 뽑아 out에
// 둔다(CR은 뺀다). 버전 줄이 없거나 out이 작으면 false다. 다른 줄(InstalledDir 등)은 넣지 않는다.
ROCKS_SHADERTOOLLOADER_API bool ParseMetalVersion(StringView output,
                                                  char (&out)[MaxMetalVersionBytes]) noexcept;

} // namespace rocks::shader

#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <ShaderCompiler/ShaderSourceTree.h>

// 셰이더 전처리기다(Docs/Design/ShaderSystem 11.1절 SC3, 결정 6). C 전처리기의 부분집합을
// Rocks가 직접 구현한다. 메인 프로세스가 job마다 원문을 전처리해 워커에 넘기므로 워커는 원본 파일
// 경로를 받지 않는다(10절 경로 누출 금지).
//
// 처리 순서:
// 1. defines(SC10 정의 조립 결과 — ShaderPreprocessDefines.h)를 이름순으로 원문 앞에 놓인
//    #define처럼 적용한다.
// 2. /Engine/Public/Platform.rsh를 강제로 include한다(작성자는 쓰지 않는다).
// 3. 엔트리 원문(.rsf)을 처리한다.
//
// 원문 정규화: 파일 앞 UTF-8 BOM을 떼고 CRLF·CR을 LF로 바꾼 뒤 처리한다. UTF-8이 아니거나 NUL이
// 있으면 InvalidEncoding이다. include 폐포의 SHA-256은 원래 파일 byte로 계산한다.
//
// 지원: 줄 이음('\' + 줄바꿈), 주석 제거(문자열 안 제외), #include "…"('/'로 시작하면 가상 절대
// 경로, 아니면 포함한 파일 디렉터리 기준 상대 경로 — "."·".."를 접은 결과가 가상 경로 규칙에
// 맞아야 한다), #pragma once(정규화된 가상 경로 기준), #define·#undef(객체형·함수형, #·##,
// __VA_ARGS__), #if·#ifdef·#ifndef·#elif·#else·#endif(defined, 64-bit 정수 산술·비교·논리·삼항,
// true·false), #error, 빈 지시문 '#', __LINE__·__FILE__(가상 경로). 그 밖의 #pragma는 그대로
// 출력에 남긴다(컴파일러가 해석한다).
//
// 거부(위치와 함께 오류): #include <…>, #include_next, 절대 OS 경로 include, 매크로로 만든 include,
// trigraph, __DATE__·__TIME__·__TIMESTAMP__, 모르는 지시문(#line 입력·#warning 포함), 다른 내용의
// 매크로 재정의, 조건(#if·#ifdef·#ifndef·#elif·defined)의 컴파일러 내장 이름(__로 시작,
// __LINE__·__FILE__ 제외 — 매크로 전개 결과도 본다). 조건 밖 원문의 내장 이름은 그대로 남아 실제
// 컴파일러가 펼친다.
//
// 출력: 원문 줄 구조를 유지한다. 토큰은 원문 줄 위치에 놓이고(같은 파일에서 MaxShaderBlankLineRun
// 줄 이하로 건너뛰면 빈 줄, 더 건너뛰거나 파일이 바뀌면 #line N "<가상 경로>"), 원문 토큰 앞의
// 공백·탭은 그대로, 주석은 공백 하나가 된다. 줄 끝은 '\n'이다. 같은 입력이면 같은 byte다(작업
// 디렉터리·mount한 실제 디렉터리와 무관하다).
//
// 사용 예:
//   ShaderDefineSet defines(allocator);
//   if (auto built = BuildShaderPreprocessDefines(format, ShaderStagePixel, format.hlsl2021, {},
//                                                 defines); !built) { ... }
//   PreprocessedShader shader(allocator);
//   if (auto done = PreprocessShader(tree, "/Engine/Private/Sample.rsf", defines.Defines(),
//   shader);
//       !done) {
//       // done.path:done.line: ShaderSourceErrorName(done.code) done.message
//   }
//   // shader.text를 워커에, shader.includes를 셰이더 맵 키에 쓴다.
//
// 스레드: 재진입 가능하며 전역 상태를 쓰지 않는다. 같은 tree를 여러 스레드가 동시에 써도 된다.
namespace rocks::shader {

// 한도(SC3). 넘으면 위치와 함께 오류다.
// include 깊이: 엔트리가 0, Platform.rsh와 엔트리가 직접 include한 파일이 1이다.
inline constexpr uint32 MaxShaderIncludeDepth = 64;
// 매크로 전개 깊이: 펼치는 중인 매크로의 중첩 수(숨김 집합 크기)와 인자 미리 전개의 중첩 수.
inline constexpr uint32 MaxShaderMacroExpansionDepth = 256;
// 출력 byte 수(64 MiB). 한 줄의 전개 중간 결과에도 같은 한도를 쓴다.
inline constexpr usize MaxShaderPreprocessOutputBytes = usize{64} * 1024 * 1024;
// 같은 파일 안에서 이 줄 수 이하로 건너뛰면 #line 대신 빈 줄을 낸다(clang -E와 같은 값).
inline constexpr uint32 MaxShaderBlankLineRun = 8;
// 강제 include하는 플랫폼 머리다(SC10).
inline constexpr char ShaderPlatformHeaderPath[] = "/Engine/Public/Platform.rsh";

// 한도 묶음이다. 기본값이 제품 값이며, 시험은 경계를 싸게 재려고 줄인 값을 준다.
struct ShaderPreprocessLimits {
    uint32 includeDepth = MaxShaderIncludeDepth;
    uint32 macroExpansionDepth = MaxShaderMacroExpansionDepth;
    usize fileBytes = MaxShaderSourceFileBytes;
    usize outputBytes = MaxShaderPreprocessOutputBytes;
};

// 전처리 정의 하나(#define name value와 같다)다. 빌린 view이며 호출 동안 살아 있어야 한다. value가
// 비면 빈 정의다.
struct ShaderDefine {
    StringView name;
    StringView value;
};

// 생성 include 하나다. 두 모양이 있다.
// - 연결(RenderBase 10.6절 2항 — 정점 입력 축 job의 VertexInputGeneratedIncludePath → 그 정점 입력
//   타입 원문): virtualPath를 include하면 sourcePath 파일을 읽는다. include 폐포·#line·__FILE__에는
//   sourcePath가 남는다.
// - 메모리 원문(ShaderSystem 16.2절 SSD52 — 생성 파라미터 구조체 HLSL): contents가 비지 않으면
//   virtualPath를 include할 때 트리 대신 contents를 읽는다(sourcePath는 비어야 한다). include
//   폐포에는 virtualPath와 contents의 SHA-256이 남는다.
// 빌린 view이며 호출 동안 살아 있어야 한다.
struct ShaderGeneratedInclude {
    StringView virtualPath;
    StringView sourcePath;
    StringView contents;
};

// include 폐포의 파일 하나다. sha256은 원래 파일 byte(BOM·CRLF 정규화 전)의 SHA-256이다.
struct ShaderIncludeFile {
    explicit ShaderIncludeFile(Allocator& allocator) noexcept : virtualPath(allocator) {
    }

    String virtualPath;
    Sha256Digest sha256;
};

// 전처리 결과다. includes는 이번 전처리가 읽은 모든 파일(엔트리·Platform.rsh 포함, 비활성 구역의
// include는 읽지 않으므로 없다)을 가상 경로 byte 순으로 한 번씩 담는다(셰이더 맵 키 — 3절).
struct ROCKS_SHADERCOMPILER_API PreprocessedShader {
    explicit PreprocessedShader(Allocator& allocator) noexcept
        : text(allocator), includes(allocator) {
    }

    void Reset() noexcept {
        text.Clear();
        includes.Clear();
    }

    String text;
    Array<ShaderIncludeFile> includes;
};

// entryPath(.rsf 가상 경로)를 tree에서 읽어 전처리한다. 처리 규칙은 파일 주석을 따른다. defines의
// 이름이 식별자가 아니거나(InvalidDefine) 겹치면(DuplicateDefine) 오류다. generated는 생성 include
// 연결이다(연결 대상 경로가 가상 경로 규칙에 맞지 않으면 그 include 줄의 오류다. 연결이 없는
// <루트>/Generated/ include는 소스 트리의 GeneratedSource다). 실패하면 out은 비어 있고 결과에
// 위치(가상 경로·줄)와 세부가 있다. out은 먼저 비운다.
ROCKS_SHADERCOMPILER_API ShaderSourceStatus PreprocessShader(
    const ShaderSourceTree& tree, StringView entryPath, Span<const ShaderDefine> defines,
    PreprocessedShader& out, const ShaderPreprocessLimits& limits = {},
    Span<const ShaderGeneratedInclude> generated = {}) noexcept;

} // namespace rocks::shader

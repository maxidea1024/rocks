#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

// 셰이더 컴파일 진단의 위치 해석이다(Docs/Design/ShaderSystem 16.3절 SSD72, UE
// FShaderCompilerError::ExtractSourceLocation·GetErrorStringWithSourceLocation 대응).
//
// 도구 진단 원문(job 결과의 diagnostics)은 줄마다 가상
// 경로(/Engine/…·/Plugin/<이름>/…·/Project/…)로 위치를 단다. 전처리 결과의 #line이 가상 경로만
// 담으므로(10절 경로 누출 금지) 도구도 그 경로를 쓴다. 이 함수들이 그 줄을 (경로, 줄, 열, 심각도,
// 문구)로 나누고, 형식·퍼뮤테이션마다 반복되는 같은 오류를 하나로 합치고, 편집기에서 눌러 이동하는
// MSVC 꼴 한 줄로 만든다.
//
// 받는 꼴(위치 앞의 머리말 "DXC compile failed: "·"FXC: " 등은 건너뛴다):
// - clang·DXC: <경로>:<줄>:<열>: <심각도>: <문구>, <경로>:<줄>: <심각도>: <문구>
// - MSVC·FXC: <경로>(<줄>,<열>): <심각도> <코드>: <문구>, <경로>(<줄>,<열>-<열>): …, <경로>(<줄>):
// …
// - Rocks 전처리기: <경로>:<줄>: <ShaderSourceError 이름>: <문구>(심각도 Error, 이름은 문구에
// 남는다) 심각도 낱말은 "fatal error"·"error"(Error), "warning"(Warning), "note"(Note)이고 없으면
// Error다. 위치가 없는 줄은 앞 진단의 이어지는 줄(도구가 붙이는 원문 줄과 '^' 표지 — UE
// HighlightedLine)이 되고, 앞 진단이 없으면 위치 없는 진단 하나가 된다. 빈 줄은 버린다.
//
// 사용 예:
//   Array<ShaderDiagnostic> diagnostics(allocator);
//   (void)ParseShaderDiagnostics(result.diagnostics.View(), diagnostics);
//   String line(allocator);
//   for (const ShaderDiagnostic& diagnostic : diagnostics) {
//       (void)FormatShaderDiagnostic(diagnostic, diskPath, line);   // "C:/…/A.rsf(3,7): error: …"
//   }
//
// 스레드: 전역 상태가 없다.
namespace rocks::shader {

enum class ShaderDiagnosticSeverity : uint8 {
    Error,
    Warning,
    Note
};

// 소문자 낱말("error"·"warning"·"note")이다. 범위 밖이면 "error".
ROCKS_SHADERCOMPILER_API const char*
ShaderDiagnosticSeverityName(ShaderDiagnosticSeverity severity) noexcept;

// 진단 하나에 붙이는 이어지는 줄의 최대 수다(넘는 줄은 버린다).
inline constexpr uint32 MaxShaderDiagnosticContextLines = 4;

struct ShaderDiagnostic {
    explicit ShaderDiagnostic(Allocator& allocator) noexcept
        : virtualPath(allocator), message(allocator), context(allocator) {
    }

    // 가상 경로다. 비면 위치 없는 진단이다.
    String virtualPath;
    // 1부터의 줄·열이다(0 = 모름).
    uint32 line = 0;
    uint32 column = 0;
    ShaderDiagnosticSeverity severity = ShaderDiagnosticSeverity::Error;
    // 위치·심각도 낱말을 뗀 문구다(FXC 오류 코드 "X3004: …"는 남는다).
    String message;
    // 이어지는 줄을 '\n'으로 이은 것이다(없으면 빈 값).
    String context;
    // 같은 진단이 나온 횟수다(MergeShaderDiagnostics가 더한다).
    uint32 occurrences = 1;
};

// text를 해석해 out 끝에 더한다(위 규칙). 할당 실패만 OutOfMemory다.
ROCKS_SHADERCOMPILER_API Result<void> ParseShaderDiagnostics(StringView text,
                                                             Array<ShaderDiagnostic>& out) noexcept;

// from의 진단을 into에 합친다: (경로, 줄, 열, 심각도, 문구)가 같은 항목은 occurrences를 더하고 새
// 항목은 처음 나온 순서로 끝에 옮긴다. 할당 실패만 OutOfMemory다(그때 into는 일부만 합쳐졌을 수
// 있다).
ROCKS_SHADERCOMPILER_API Result<void>
MergeShaderDiagnostics(Array<ShaderDiagnostic>& into, Array<ShaderDiagnostic>& from) noexcept;

// 한 줄로 만든다(out은 먼저 비운다): "<표시 경로>(<줄>,<열>): <심각도>: <문구>". 열이 0이면
// "(<줄>)", 줄이 0이면 "<표시 경로>: ", 경로가 없으면 "<심각도>: <문구>"다. displayPath가 비면 가상
// 경로를 쓴다(디스크 경로가 없는 메모리 원문). 할당 실패만 OutOfMemory다.
ROCKS_SHADERCOMPILER_API Result<void> FormatShaderDiagnostic(const ShaderDiagnostic& diagnostic,
                                                             StringView displayPath,
                                                             String& out) noexcept;

} // namespace rocks::shader

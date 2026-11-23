#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>

// Sparks 공식 formatter다(Docs/Design/SparksLanguage.ko.md 3.1절 "단일 공식 formatter", 규칙은
// Docs/Design/SparksTools.ko.md 3절·8절). 설정이 없다. token 기반이다: 토큰 글자는 바꾸지 않고 토큰
// 사이의 줄바꿈·빈 줄·들여쓰기·공백만 정한다(주석은 자리를 지키고 문자열 리터럴은 원문 그대로).
//
// 사용 예:
//   StringBuilder<4096> formatted(allocator);
//   auto done = FormatSource(allocator, sources, id, diagnostics, formatted);
//   if (done && done.Value()) { /* formatted가 서식한 원문 */ }
//   else if (done) { /* 어휘·구문 진단이 있어 서식하지 않았다 — diagnostics */ }

namespace rocks::sparks {

// id 파일의 공식 서식을 out 끝에 붙인다. 값 false는 어휘·구문 진단(ParseFile이 내는 것)이 있어
// 서식하지 않았음이다(진단은 diagnostics에 붙고 out은 그대로). 결과는 멱등이다(다시 서식해도
// 같다). 줄 끝은 LF이고 내용이 있으면 LF 하나로 끝난다.
// 실패: 파일이 IsUsable이 아니면 InvalidArgument, 할당 실패는 그 오류(out에 일부가 남을 수 있다).
ROCKS_SPARKSCOMPILER_API Result<bool> FormatSource(Allocator& allocator, const SourceSet& sources,
                                                   SourceId id, DiagnosticList& diagnostics,
                                                   StringBuilderBase& out) noexcept;

} // namespace rocks::sparks

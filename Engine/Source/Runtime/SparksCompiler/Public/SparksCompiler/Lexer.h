#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <SparksCompiler/Token.h>

// Sparks 어휘 분석이다(Docs/Design/SparksLanguage.ko.md 8.1절 "어휘 분석", 3.1·3.6절).
//
// 사용 예:
//   Array<Token> tokens(allocator);
//   if (sources.Get(id).IsUsable()) {
//       auto lexed = Tokenize(sources, id, diagnostics, tokens);
//   }
//
// - 원문의 BOM 뒤부터 끝까지 토큰으로 나누고 마지막에 EndOfFile 하나를 붙인다.
// - 오류(S0104–S0108, S0110–S0114)가 나도 끝까지 간다(구문 분석이 이어지게). 잘못된 문자와 닫히지
//   않은 문자열·주석 자리에는 Invalid 토큰을 남긴다(Token.h). 같은 줄에서 같은 코드는 한 번만 낸다.
// - 일반 주석은 토큰을 만들지 않는다. `///` 문서 주석은 DocComment 토큰이다.
// - S0109(정수 범위)는 쓰는 자리의 타입을 아는 검사 단계(LC3)가 낸다(3.3절 — 리터럴은 타입이 없는
//   상수다).

namespace rocks::sparks {

// id 파일을 토큰으로 나눠 tokens 끝에 붙인다. 파일은 IsUsable()이어야 한다(아니면 InvalidArgument).
// 실패: 할당 실패는 그 오류이며 tokens·diagnostics에 일부가 남을 수 있다(호출자가 버린다 — 할당
// 실패한 컴파일은 결과를 쓰지 않는다).
ROCKS_SPARKSCOMPILER_API Result<void> Tokenize(const SourceSet& sources, SourceId id,
                                               DiagnosticList& diagnostics,
                                               Array<Token>& tokens) noexcept;

} // namespace rocks::sparks

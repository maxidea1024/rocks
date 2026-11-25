#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// Sparks 리터럴 원문의 값을 푼다(Docs/Design/SparksLanguage 3.6절 어휘, SD54).
//
// 어휘 분석은 토큰의 원문 위치만 남기고, 값은 쓰는 쪽(어휘 분석의 범위 검사, 구문 분석)이 이 함수로
// 원문에서 다시 읽는다. 입력은 어휘 분석이 오류 없이 만든 리터럴 토큰의 원문이어야 한다(형식은
// 다시 검사하지 않는다 — 형식 오류는 어휘 분석의 S0110이다).
//
// 사용 예:
//   auto value = TryDecodeIntegerLiteral("0xFF_FF");      // 65535
//   auto real = TryDecodeFloatLiteral(allocator, "1_000.5e-3");

namespace rocks::sparks {

// 정수 리터럴(10진·`0x` 16진·`0b` 2진, 구분자 `_` 포함)의 값이다. 범위 검사(S0109)는 쓰는 자리의
// 타입을 아는 검사 단계(LC3)가 하므로 여기서는 uint64까지 받는다.
// 실패: uint64를 넘으면 Overflow.
ROCKS_SPARKSCOMPILER_API Result<uint64> TryDecodeIntegerLiteral(StringView text) noexcept;

// 실수 리터럴(`1.5`·`.5`·`1e3`, 구분자 포함)의 값이다. 가장 가까운 double로 반올림하며 너무 작은
// 값은 0 또는 비정규 수다. allocator는 구분자를 뺀 사본에 쓴다(짧은 리터럴은 할당하지 않는다).
// 실패: 유한한 double을 넘으면 Overflow(S0113), 할당 실패는 그 오류.
ROCKS_SPARKSCOMPILER_API Result<double> TryDecodeFloatLiteral(Allocator& allocator,
                                                              StringView text) noexcept;

} // namespace rocks::sparks

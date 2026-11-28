#pragma once

#include <Core/Result.h>
#include <Core/Text/String.h>
#include <UiFramework/Numeric/NumericKind.h>

namespace rocks::ui {

// 소수 자리 기본값(UE DefaultMinFractionalDigits 1·DefaultMaxFractionalDigits 6)과 상한이다.
inline constexpr int32 DefaultMinNumericFractionalDigits = 1;
inline constexpr int32 DefaultMaxNumericFractionalDigits = 6;
inline constexpr int32 MaxNumericFractionalDigits = 15;

// 숫자를 글자로 바꾸는 규칙이다(Docs/Design/UiToolkit 24.2절 — UE TDefaultNumericTypeInterface).
struct NumericFormatOptions {
    NumericKind kind = NumericKind::Double;
    // 소수 자리 최소·최대다(정수 형식은 0). 최대가 최소보다 작으면 최소를 쓴다(UE Max(Max, Min)).
    int32 minFractionalDigits = DefaultMinNumericFractionalDigits;
    int32 maxFractionalDigits = DefaultMaxNumericFractionalDigits;
    // 자른 값이 정수와 거의 같지만 같지 않으면 "..."를 붙인다(UE IndicateNearlyInteger, 기본 끔).
    bool indicateNearlyInteger = false;
};

// 값을 글자로 쓴다: 묶음 기호 없음, 소수점 '.', 최대 자리에서 반올림한 뒤 끝 0을 최소 자리까지
// 지운다. 정수 형식은 RoundToNumericKind 뒤 소수 없이 쓴다. "-0"은 "0"으로 쓴다(UE는 "-0.0").
// 자릿수가 범위(0..MaxNumericFractionalDigits) 밖이거나 값이 유한하지 않으면 InvalidArgument, 쓰기
// 실패는 OutOfMemory다. out은 지우고 쓴다.
//
// 사용 예:
//   String text(allocator);
//   (void)FormatNumber(2.5, {}, text);                               // "2.5"
//   (void)FormatNumber(3, {.minFractionalDigits = 2}, text);         // "3.00"
ROCKS_UIFRAMEWORK_API Result<void> FormatNumber(double value, const NumericFormatOptions& options,
                                                String& out) noexcept;

// 글자를 값으로 읽는다(UE FromString): 전체가 숫자면 그 값, 아니면 수식(EvaluateNumericExpression —
// existing은 상대 입력 "+=5"의 지금 값). 결과는 RoundToNumericKind로 형식에 맞춘다. 읽지 못하면
// 수식 오류를 돌려준다(값을 바꾸지 않는다).
//
// 사용 예:
//   auto value = ParseNumber("10+5*2", 0, NumericKind::Int32);   // 20
ROCKS_UIFRAMEWORK_API Result<double> ParseNumber(StringView text, double existing,
                                                 NumericKind kind) noexcept;

// 숫자 입력 칸에 칠 수 있는 글자인지다(UE IsCharacterValid — 0-9 ( ) - + = \ / . , * ^ %).
// 공백·영문자는 받지 않는다(UE와 같다 — 그래서 칸에서는 sqrt를 칠 수 없다). 단위 입력은 단위 글자를
// 더한다(24.4절).
ROCKS_UIFRAMEWORK_API bool IsNumericCharacter(char32_t c) noexcept;

} // namespace rocks::ui

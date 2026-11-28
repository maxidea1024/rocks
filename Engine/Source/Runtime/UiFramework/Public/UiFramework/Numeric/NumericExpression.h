#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

namespace rocks::ui {

// 수식 토큰 수와 괄호 깊이 상한이다(붙여 넣은 긴 글자가 스택을 넘지 않게 — 24.3절).
inline constexpr usize MaxNumericExpressionTokens = 256;
inline constexpr usize MaxNumericExpressionDepth = 64;

// 숫자 입력 칸의 수식을 계산한다(Docs/Design/UiToolkit 24.3절 — UE FBasicMathExpressionEvaluator).
// 문법은 UE 그대로다: 숫자, ( ), 앞 단항 + - sqrt(가장 먼저 묶인다 — "-2^2"는 4), 이항 ^(오른쪽
// 결합), * / %(왼쪽 결합), + -(왼쪽 결합). 공백은 무시한다. 처음이 += -= *= /=이면 existing을 앞에
// 넣는다("*=2"는 existing × 2). UE는 sqrt·^·%를 float로 계산하지만 여기서는 double로 계산한다.
//
// 오류: 문법 오류·0으로 나눔·0으로 나머지·빈 글자·NaN 결과(음수의 sqrt)는 InvalidArgument,
// 토큰·깊이 상한을 넘으면 CapacityExceeded, 결과가 무한이면 Overflow다.
//
// 사용 예:
//   auto value = EvaluateNumericExpression("10+5*2", 0);   // 20
//   auto scaled = EvaluateNumericExpression("*=2", 7);     // 14
ROCKS_UIFRAMEWORK_API Result<double> EvaluateNumericExpression(StringView text,
                                                               double existing) noexcept;

} // namespace rocks::ui

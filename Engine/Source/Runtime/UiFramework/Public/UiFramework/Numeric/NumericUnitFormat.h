#pragma once

#include <UiFramework/Numeric/NumericFormat.h>
#include <UiFramework/Numeric/NumericUnit.h>

namespace rocks::ui {

// 칸의 단위다(Docs/Design/UiToolkit 24.4절 — UE TNumericUnitTypeInterface).
struct NumericUnitOptions {
    // 값이 저장된 단위다. Unspecified면 단위 없이 24.2절 그대로다.
    NumericUnit underlying = NumericUnit::Unspecified;
    // 고정 표시 단위다(UE FixedDisplayUnits). 저장 단위와 호환되지 않으면 무시한다.
    Optional<NumericUnit> display{};
    // 고정 표시 단위가 없을 때 크기에 맞는 단위로 표시한다(UE QuantizeUnitsToBestFit).
    bool bestFit = false;
};

// 값을 "값 + 공백 + 기호"로 쓴다("100 cm"). 표시 단위는 고정 → 가장 알맞은 → 저장 단위 순이다.
// 단위가 Unspecified면 FormatNumber와 같다. 정수 형식이라도 다른 단위로 바꿔 보이면 소수를
// 남긴다(500 m를 km로 보이면 "0.5 km" — UE는 정수로 잘라 "0 km"이다). 오류는 FormatNumber와 같다.
//
// 사용 예:
//   (void)FormatNumberWithUnit(150, {}, {.underlying = NumericUnit::Centimeters,
//                                         .display = NumericUnit::Meters}, text);   // "1.5 m"
ROCKS_UIFRAMEWORK_API Result<void> FormatNumberWithUnit(double value,
                                                        const NumericFormatOptions& format,
                                                        const NumericUnitOptions& units,
                                                        String& out) noexcept;

// 글자를 저장 단위 값으로 읽는다: 단위가 Unspecified면 ParseNumber, 아니면 단위 수식을 표시
// 단위(고정 또는 저장 단위)를 기본으로 계산해 저장 단위로 바꾼다. 결과는
// RoundToNumericKind다("0.5km"를 m 정수 칸에 → 500). 단위가 호환되지 않으면 InvalidArgument다.
//
// 사용 예:
//   auto value = ParseNumberWithUnit("1m", 0, NumericKind::Double,
//                                    {.underlying = NumericUnit::Centimeters});   // 100
ROCKS_UIFRAMEWORK_API Result<double> ParseNumberWithUnit(StringView text, double existing,
                                                         NumericKind kind,
                                                         const NumericUnitOptions& units) noexcept;

// 칸에 칠 수 있는 글자인지다: 단위가 없으면 IsNumericCharacter, 있으면 탭 말고 모두(UE
// IsCharacterValid).
ROCKS_UIFRAMEWORK_API bool IsNumericUnitCharacter(char32_t c,
                                                  const NumericUnitOptions& units) noexcept;

} // namespace rocks::ui

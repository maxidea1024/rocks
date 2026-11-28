#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 숫자 입력이 다루는 값의 형식이다(Docs/Design/UiToolkit 24.2절 — UE SSpinBox 인스턴스 목록:
// double·float·int8–int64·uint8–uint64). 값은 늘 double로 들고, 형식이 반올림과 범위를 정한다.
// 64비트 정수는 2^53을 넘으면 double 정밀도를 넘는다(UE도 같은 double 경로다).
enum class NumericKind : uint8 {
    Double,
    Float,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64
};

// 정수 형식인지다.
ROCKS_UIFRAMEWORK_API bool IsIntegralNumericKind(NumericKind kind) noexcept;
// 형식이 담는 가장 작은·큰 유한 값이다(double로). 64비트 정수의 최대는 그 형식으로 옮겨도 넘치지
// 않는 가장 큰 double이다(INT64_MAX·UINT64_MAX 자체는 double로 정확히 나타나지 않는다).
ROCKS_UIFRAMEWORK_API double NumericKindLowest(NumericKind kind) noexcept;
ROCKS_UIFRAMEWORK_API double NumericKindMax(NumericKind kind) noexcept;
// 값을 형식에 맞춘다: 정수 형식은 Floor(v + 0.5)(UE RoundIfIntegerValue), 모두 형식 범위로 자르고
// float는 float로 반올림한다. 유한하지 않은 값은 그대로 돌려준다(호출자가 거른다).
//
// 사용 예:
//   const double count = RoundToNumericKind(2.5, NumericKind::Int32);   // 3
ROCKS_UIFRAMEWORK_API double RoundToNumericKind(double value, NumericKind kind) noexcept;

} // namespace rocks::ui

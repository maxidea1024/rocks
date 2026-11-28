#pragma once

#include <Core/Optional.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

namespace rocks::ui {

// 숫자 입력 칸의 단위다(Docs/Design/UiToolkit 24.4절 — UE EUnit, 순서도 같다). Unspecified는 단위
// 없음이다.
enum class NumericUnit : uint8 {
    Micrometers,
    Millimeters,
    Centimeters,
    Meters,
    Kilometers,
    Inches,
    Feet,
    Yards,
    Miles,
    Lightyears,
    Degrees,
    Radians,
    CentimetersPerSecond,
    MetersPerSecond,
    KilometersPerHour,
    MilesPerHour,
    DegreesPerSecond,
    RadiansPerSecond,
    CentimetersPerSecondSquared,
    MetersPerSecondSquared,
    Celsius,
    Farenheit,
    Kelvin,
    Micrograms,
    Milligrams,
    Grams,
    Kilograms,
    MetricTons,
    Ounces,
    Pounds,
    Stones,
    GramsPerCubicCentimeter,
    GramsPerCubicMeter,
    KilogramsPerCubicCentimeter,
    KilogramsPerCubicMeter,
    Newtons,
    PoundsForce,
    KilogramsForce,
    KilogramCentimetersPerSecondSquared,
    NewtonMeters,
    KilogramCentimetersSquaredPerSecondSquared,
    NewtonSeconds,
    KilogramCentimeters,
    KilogramMeters,
    Decibels,
    Hertz,
    Kilohertz,
    Megahertz,
    Gigahertz,
    RevolutionsPerMinute,
    Bytes,
    Kilobytes,
    Megabytes,
    Gigabytes,
    Terabytes,
    Lumens,
    Candela,
    Lux,
    CandelaPerMeter2,
    Nit,
    ExposureValue,
    Nanoseconds,
    Microseconds,
    Milliseconds,
    Seconds,
    Minutes,
    Hours,
    Days,
    Months,
    Years,
    PixelsPerInch,
    Percentage,
    Multiplier,
    Pascals,
    KiloPascals,
    MegaPascals,
    GigaPascals,
    Unspecified
};

// 단위 종류다(UE EUnitType). 같은 종류끼리만 바꾼다.
enum class NumericUnitType : uint8 {
    Distance,
    Angle,
    Speed,
    AngularSpeed,
    Acceleration,
    Temperature,
    Mass,
    Density,
    Force,
    Torque,
    Impulse,
    PositionalImpulse,
    Loudness,
    Frequency,
    DataSize,
    LuminousFlux,
    LuminousIntensity,
    Illuminance,
    Luminance,
    Time,
    PixelDensity,
    Multipliers,
    ExposureValue,
    Stress,
    None
};

// 단위가 붙은 값이다.
struct NumericUnitValue {
    double value = 0;
    NumericUnit unit = NumericUnit::Unspecified;
};

// 단위의 종류다(Unspecified는 None).
ROCKS_UIFRAMEWORK_API NumericUnitType GetNumericUnitType(NumericUnit unit) noexcept;
// 표시 글자(UTF-8 — UE DisplayStrings: "cm", "°", "cm/s²" 등)다. Unspecified는 빈 글자다.
ROCKS_UIFRAMEWORK_API StringView NumericUnitDisplayString(NumericUnit unit) noexcept;
// 이름·기호 글자를 대소문자 무시로 단위로 읽는다(UE UnitFromString — ParseCandidates). 없으면 비어
// 있다.
ROCKS_UIFRAMEWORK_API Optional<NumericUnit> NumericUnitFromString(StringView text) noexcept;
// 같은 종류인지다(둘 다 Unspecified여도 참 — UE AreUnitsCompatible).
ROCKS_UIFRAMEWORK_API bool AreNumericUnitsCompatible(NumericUnit from, NumericUnit to) noexcept;

// 값을 from에서 to로 바꾼다(UE FUnitConversion::Convert). 호환되지 않거나 한쪽이 Unspecified면
// 그대로다. 온도는 켈빈을 거치는 오프셋 변환이고, 소리·충격량·광도·조도·휘도·픽셀 밀도·노출은
// 바꾸지 않는다.
//
// 사용 예:
//   const double centimeters = ConvertNumericUnit(1.5, NumericUnit::Meters,
//   NumericUnit::Centimeters); // 150
ROCKS_UIFRAMEWORK_API double ConvertNumericUnit(double value, NumericUnit from,
                                                NumericUnit to) noexcept;

// 크기에 맞는 단위로 옮긴다(UE QuantizeUnitsToBestFit): 절댓값이 1보다 크면 위 단위로, 1 이하(0
// 제외)면 아래 단위로. 묶음(미터·영국식 길이·질량, 밀도, 주파수, 데이터 크기, 시간, 응력)이 없는
// 단위는 그대로다.
//
// 사용 예:
//   const auto fit = QuantizeNumericUnitToBestFit(2500, NumericUnit::Meters);   // {2.5,
//   Kilometers}
ROCKS_UIFRAMEWORK_API NumericUnitValue QuantizeNumericUnitToBestFit(double value,
                                                                    NumericUnit unit) noexcept;

// 단위 수식을 계산한다(UE FUnitExpressionParser — 24.4절): 숫자(+ 단위), ( ), 단항 + -, * /, +
// -(왼쪽 결합 — UE는 오른쪽 결합 결함), 처음 += -= *= /=(existing을 앞에 둔다). 단위 없는 결과는
// defaultUnit이다. 더하기·빼기에서 한쪽만 단위가 없으면 그쪽은 defaultUnit이다. 오류: 문법·호환되지
// 않는 단위 더하기·단위 곱하기·단위로 나누기·0으로 나누기는 InvalidArgument, 상한 초과는
// CapacityExceeded, 무한은 Overflow다.
//
// 사용 예:
//   auto length = EvaluateNumericUnitExpression("1m + 20cm", NumericUnit::Centimeters, {});
//   // {1.2, Meters}
ROCKS_UIFRAMEWORK_API Result<NumericUnitValue>
EvaluateNumericUnitExpression(StringView text, NumericUnit defaultUnit,
                              const NumericUnitValue& existing) noexcept;

} // namespace rocks::ui

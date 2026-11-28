#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Numeric/NumericUnitFormat.h>

// 숫자 입력 단위 시험이다(Docs/Design/UiToolkit 24.4절 — UE EUnit·FUnitConversion·
// FUnitExpressionParser·TNumericUnitTypeInterface): 표·별칭, 종류 안 변환(길이·각·온도
// 오프셋·바꾸지 않는 종류), 가장 알맞은 단위, 단위 수식(더하기 변환·곱하기 규칙·왼쪽 결합·상대
// 입력), 칸 형식·해석.

namespace rocks::ui {

namespace {

bool UnitNear(double a, double b, double tolerance = 1e-9) noexcept {
    return math::Abs(a - b) <= tolerance * math::Max(1.0, math::Abs(b));
}

String FormatWith(Allocator& allocator, double value, const NumericFormatOptions& format,
                  const NumericUnitOptions& units) noexcept {
    String text(allocator);
    if (!FormatNumberWithUnit(value, format, units, text)) {
        (void)text.TryAssign("<error>");
    }

    return text;
}

} // namespace

TEST(UiFramework_NumericUnitTablesMatchUe) {
    CHECK(GetNumericUnitType(NumericUnit::Centimeters) == NumericUnitType::Distance);
    CHECK(GetNumericUnitType(NumericUnit::Kelvin) == NumericUnitType::Temperature);
    CHECK(GetNumericUnitType(NumericUnit::GigaPascals) == NumericUnitType::Stress);
    CHECK(GetNumericUnitType(NumericUnit::Unspecified) == NumericUnitType::None);
    CHECK(NumericUnitDisplayString(NumericUnit::Centimeters) == "cm");
    CHECK(NumericUnitDisplayString(NumericUnit::Degrees) == "\xC2\xB0");
    CHECK(NumericUnitDisplayString(NumericUnit::Kilobytes) == "KiB");
    CHECK(NumericUnitDisplayString(NumericUnit::Hours) == "hr");
    CHECK(NumericUnitDisplayString(NumericUnit::Unspecified).IsEmpty());
    // 별칭은 대소문자를 가리지 않는다(UE UnitFromString — Stricmp).
    const auto meters = NumericUnitFromString("METERS");
    CHECK(meters && meters.Value() == NumericUnit::Meters);
    const auto kmh = NumericUnitFromString("kmph");
    CHECK(kmh && kmh.Value() == NumericUnit::KilometersPerHour);
    const auto micro = NumericUnitFromString("\xC2\xB5s");
    CHECK(micro && micro.Value() == NumericUnit::Microseconds);
    CHECK(!NumericUnitFromString("parsec"));
    CHECK(!NumericUnitFromString(""));
    CHECK(AreNumericUnitsCompatible(NumericUnit::Inches, NumericUnit::Kilometers));
    CHECK(!AreNumericUnitsCompatible(NumericUnit::Inches, NumericUnit::Grams));
}

TEST(UiFramework_NumericUnitConvertsWithinATypeOnly) {
    CHECK(UnitNear(ConvertNumericUnit(1.5, NumericUnit::Meters, NumericUnit::Centimeters), 150));
    CHECK(UnitNear(ConvertNumericUnit(1, NumericUnit::Feet, NumericUnit::Centimeters), 30.48));
    CHECK(UnitNear(ConvertNumericUnit(1, NumericUnit::Miles, NumericUnit::Kilometers), 1.609344));
    CHECK(UnitNear(ConvertNumericUnit(180, NumericUnit::Degrees, NumericUnit::Radians),
                   3.14159265358979323846));
    CHECK(UnitNear(
        ConvertNumericUnit(1, NumericUnit::MetersPerSecond, NumericUnit::KilometersPerHour), 3.6));
    CHECK(UnitNear(ConvertNumericUnit(1, NumericUnit::Pounds, NumericUnit::Grams), 453.592));
    CHECK(UnitNear(ConvertNumericUnit(2, NumericUnit::Hours, NumericUnit::Minutes), 120));
    CHECK(UnitNear(ConvertNumericUnit(1, NumericUnit::Megabytes, NumericUnit::Bytes), 1048576));
    CHECK(UnitNear(ConvertNumericUnit(50, NumericUnit::Percentage, NumericUnit::Multiplier), 0.5));
    // 온도는 켈빈을 거치는 오프셋 변환이다.
    CHECK(UnitNear(ConvertNumericUnit(100, NumericUnit::Celsius, NumericUnit::Farenheit), 212));
    CHECK(UnitNear(ConvertNumericUnit(0, NumericUnit::Celsius, NumericUnit::Kelvin), 273.15));
    CHECK(UnitNear(ConvertNumericUnit(32, NumericUnit::Farenheit, NumericUnit::Celsius), 0, 1e-9));
    // 종류가 다르거나 Unspecified거나 바꾸지 않는 종류면 그대로다.
    CHECK(ConvertNumericUnit(5, NumericUnit::Meters, NumericUnit::Grams) == 5);
    CHECK(ConvertNumericUnit(5, NumericUnit::Unspecified, NumericUnit::Meters) == 5);
    CHECK(ConvertNumericUnit(5, NumericUnit::Candela, NumericUnit::Candela) == 5);
    CHECK(ConvertNumericUnit(5, NumericUnit::CandelaPerMeter2, NumericUnit::Nit) == 5);
}

TEST(UiFramework_NumericUnitQuantizesToTheBestFit) {
    const auto km = QuantizeNumericUnitToBestFit(2500, NumericUnit::Meters);
    CHECK(km.unit == NumericUnit::Kilometers && UnitNear(km.value, 2.5));
    const auto mm = QuantizeNumericUnitToBestFit(0.5, NumericUnit::Centimeters);
    CHECK(mm.unit == NumericUnit::Millimeters && UnitNear(mm.value, 5));
    const auto minutes = QuantizeNumericUnitToBestFit(90, NumericUnit::Seconds);
    CHECK(minutes.unit == NumericUnit::Minutes && UnitNear(minutes.value, 1.5));
    const auto kib = QuantizeNumericUnitToBestFit(2048, NumericUnit::Bytes);
    CHECK(kib.unit == NumericUnit::Kilobytes && UnitNear(kib.value, 2));
    // 묶음 위 끝·0·묶음 없는 단위·rpm은 그대로다.
    CHECK(QuantizeNumericUnitToBestFit(5000, NumericUnit::Kilometers).unit ==
          NumericUnit::Kilometers);
    CHECK(QuantizeNumericUnitToBestFit(0, NumericUnit::Meters).unit == NumericUnit::Meters);
    CHECK(QuantizeNumericUnitToBestFit(5000, NumericUnit::Degrees).unit == NumericUnit::Degrees);
    CHECK(QuantizeNumericUnitToBestFit(5000, NumericUnit::RevolutionsPerMinute).unit ==
          NumericUnit::RevolutionsPerMinute);
}

TEST(UiFramework_NumericUnitExpressionsConvertAndFollowUeRules) {
    const NumericUnitValue none{};
    auto sum = EvaluateNumericUnitExpression("1m + 20cm", NumericUnit::Centimeters, none);
    REQUIRE(sum);
    CHECK(sum.Value().unit == NumericUnit::Meters && UnitNear(sum.Value().value, 1.2));
    // 단위 없는 쪽은 기본 단위다.
    auto mixed = EvaluateNumericUnitExpression("10 + 1m", NumericUnit::Centimeters, none);
    REQUIRE(mixed);
    CHECK(mixed.Value().unit == NumericUnit::Centimeters && UnitNear(mixed.Value().value, 110));
    // 가장 긴 별칭과 대소문자 무시("5min" 분, "2 KM").
    auto minutes = EvaluateNumericUnitExpression("5min", NumericUnit::Seconds, none);
    CHECK(minutes && minutes.Value().unit == NumericUnit::Minutes);
    auto upper = EvaluateNumericUnitExpression("2 KM", NumericUnit::Meters, none);
    CHECK(upper && upper.Value().unit == NumericUnit::Kilometers);
    // 곱하기는 한쪽만 단위, 나누기는 오른쪽 단위 없음.
    auto scaled = EvaluateNumericUnitExpression("2 * 3m", NumericUnit::Meters, none);
    CHECK(scaled && UnitNear(scaled.Value().value, 6) &&
          scaled.Value().unit == NumericUnit::Meters);
    CHECK(!EvaluateNumericUnitExpression("2m * 3m", NumericUnit::Meters, none));
    CHECK(!EvaluateNumericUnitExpression("6m / 2m", NumericUnit::Meters, none));
    CHECK(!EvaluateNumericUnitExpression("6m / 0", NumericUnit::Meters, none));
    CHECK(!EvaluateNumericUnitExpression("1m + 1kg", NumericUnit::Meters, none));
    // 왼쪽 결합이다(UE는 오른쪽 결합 결함으로 9).
    auto chain = EvaluateNumericUnitExpression("10 - 4 - 3", NumericUnit::Meters, none);
    CHECK(chain && UnitNear(chain.Value().value, 3));
    // 결과에 단위가 없으면 기본 단위다.
    auto plain = EvaluateNumericUnitExpression("(2+3)*2", NumericUnit::Grams, none);
    CHECK(plain && plain.Value().unit == NumericUnit::Grams && UnitNear(plain.Value().value, 10));
    // 상대 입력은 지금 값(그 단위)을 앞에 둔다.
    auto relative = EvaluateNumericUnitExpression("+=1m", NumericUnit::Centimeters,
                                                  {50, NumericUnit::Centimeters});
    CHECK(relative && UnitNear(relative.Value().value, 150));
    CHECK(!EvaluateNumericUnitExpression("", NumericUnit::Meters, none));
    CHECK(!EvaluateNumericUnitExpression("2 ^ 2", NumericUnit::Meters, none));
}

TEST(UiFramework_NumericUnitFieldFormatsAndParsesInItsUnits) {
    SystemAllocator allocator;
    const NumericUnitOptions centimeters{.underlying = NumericUnit::Centimeters};
    CHECK(FormatWith(allocator, 100, {}, centimeters).View() == "100.0 cm");
    CHECK(FormatWith(allocator, 150, {},
                     {.underlying = NumericUnit::Centimeters, .display = NumericUnit::Meters})
              .View() == "1.5 m");
    CHECK(
        FormatWith(allocator, 250000, {}, {.underlying = NumericUnit::Centimeters, .bestFit = true})
            .View() == "2.5 km");
    // 호환되지 않는 표시 단위는 무시한다.
    CHECK(FormatWith(allocator, 3, {},
                     {.underlying = NumericUnit::Centimeters, .display = NumericUnit::Grams})
              .View() == "3.0 cm");
    // 정수 형식도 다른 단위로 보이면 소수를 남긴다.
    CHECK(FormatWith(allocator, 500, {.kind = NumericKind::Int32},
                     {.underlying = NumericUnit::Meters, .display = NumericUnit::Kilometers})
              .View() == "0.5 km");
    CHECK(FormatWith(allocator, 7, {}, {}).View() == "7.0");
    // 해석은 저장 단위로 바꾼다("1m" → 100 cm, 정수 형식 반올림).
    auto meter = ParseNumberWithUnit("1m", 0, NumericKind::Double, centimeters);
    CHECK(meter && UnitNear(meter.Value(), 100));
    auto half =
        ParseNumberWithUnit("0.5km", 0, NumericKind::Int32, {.underlying = NumericUnit::Meters});
    CHECK(half && half.Value() == 500);
    // 단위 없는 글자는 표시 단위다.
    auto shown = ParseNumberWithUnit(
        "2", 0, NumericKind::Double,
        {.underlying = NumericUnit::Centimeters, .display = NumericUnit::Meters});
    CHECK(shown && UnitNear(shown.Value(), 200));
    CHECK(!ParseNumberWithUnit("1kg", 0, NumericKind::Double, centimeters));
    // 단위 없는 칸은 24.2절 해석(수식 포함)이다.
    auto bare = ParseNumberWithUnit("2^3", 0, NumericKind::Double, {});
    CHECK(bare && bare.Value() == 8);
    CHECK(IsNumericUnitCharacter(U'k', centimeters) && !IsNumericUnitCharacter(U'\t', centimeters));
    CHECK(!IsNumericUnitCharacter(U'k', {}));
}

} // namespace rocks::ui

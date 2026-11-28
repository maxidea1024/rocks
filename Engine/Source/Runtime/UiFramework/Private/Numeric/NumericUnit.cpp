#include <Core/Math/MathUtility.h>
#include <Core/Math/UnitConversion.h>
#include <Core/Text/NumberText.h>
#include <UiFramework/Numeric/NumericExpression.h>
#include <UiFramework/Numeric/NumericUnit.h>

namespace rocks::ui {

namespace {

using U = NumericUnit;
using T = NumericUnitType;

constexpr usize UnitCount = static_cast<usize>(U::Unspecified);

// 입력 별칭이다(UE UnitConversion.cpp ParseCandidates 30–128행 그대로 — µ·°·²·³는 UTF-8 escape).
struct ParseCandidate {
    StringView text;
    U unit;
};

constexpr ParseCandidate ParseCandidates[] = {
    {"Micrometers", U::Micrometers},
    {"um", U::Micrometers},
    {"\xC2\xB5m", U::Micrometers},
    {"Millimeters", U::Millimeters},
    {"mm", U::Millimeters},
    {"Centimeters", U::Centimeters},
    {"cm", U::Centimeters},
    {"Meters", U::Meters},
    {"m", U::Meters},
    {"Kilometers", U::Kilometers},
    {"km", U::Kilometers},
    {"Inches", U::Inches},
    {"in", U::Inches},
    {"Feet", U::Feet},
    {"ft", U::Feet},
    {"Yards", U::Yards},
    {"yd", U::Yards},
    {"Miles", U::Miles},
    {"mi", U::Miles},
    {"Lightyears", U::Lightyears},
    {"ly", U::Lightyears},
    {"Degrees", U::Degrees},
    {"deg", U::Degrees},
    {"\xC2\xB0", U::Degrees},
    {"Radians", U::Radians},
    {"rad", U::Radians},
    {"CentimetersPerSecond", U::CentimetersPerSecond},
    {"cm/s", U::CentimetersPerSecond},
    {"MetersPerSecond", U::MetersPerSecond},
    {"m/s", U::MetersPerSecond},
    {"KilometersPerHour", U::KilometersPerHour},
    {"km/h", U::KilometersPerHour},
    {"kmph", U::KilometersPerHour},
    {"MilesPerHour", U::MilesPerHour},
    {"mi/h", U::MilesPerHour},
    {"mph", U::MilesPerHour},
    {"DegreesPerSecond", U::DegreesPerSecond},
    {"deg/s", U::DegreesPerSecond},
    {"RadiansPerSecond", U::RadiansPerSecond},
    {"rad/s", U::RadiansPerSecond},
    {"CentimetersPerSecondSquared", U::CentimetersPerSecondSquared},
    {"cm/s2", U::CentimetersPerSecondSquared},
    {"cm/s\xC2\xB2", U::CentimetersPerSecondSquared},
    {"cm/s^2", U::CentimetersPerSecondSquared},
    {"MetersPerSecondSquared", U::MetersPerSecondSquared},
    {"m/s2", U::MetersPerSecondSquared},
    {"m/s\xC2\xB2", U::MetersPerSecondSquared},
    {"m/s^2", U::MetersPerSecondSquared},
    {"Celsius", U::Celsius},
    {"C", U::Celsius},
    {"degC", U::Celsius},
    {"\xC2\xB0"
     "C",
     U::Celsius},
    {"Farenheit", U::Farenheit},
    {"F", U::Farenheit},
    {"degF", U::Farenheit},
    {"\xC2\xB0"
     "F",
     U::Farenheit},
    {"Kelvin", U::Kelvin},
    {"K", U::Kelvin},
    {"Micrograms", U::Micrograms},
    {"ug", U::Micrograms},
    {"\xC2\xB5g", U::Micrograms},
    {"Milligrams", U::Milligrams},
    {"mg", U::Milligrams},
    {"Grams", U::Grams},
    {"g", U::Grams},
    {"Kilograms", U::Kilograms},
    {"kg", U::Kilograms},
    {"MetricTons", U::MetricTons},
    {"t", U::MetricTons},
    {"Ounces", U::Ounces},
    {"oz", U::Ounces},
    {"Pounds", U::Pounds},
    {"lb", U::Pounds},
    {"Stones", U::Stones},
    {"st", U::Stones},
    {"GramsPerCubicCentimeter", U::GramsPerCubicCentimeter},
    {"g/cm3", U::GramsPerCubicCentimeter},
    {"g/cm\xC2\xB3", U::GramsPerCubicCentimeter},
    {"GramsPerCubicMeter", U::GramsPerCubicMeter},
    {"g/m3", U::GramsPerCubicMeter},
    {"g/m\xC2\xB3", U::GramsPerCubicMeter},
    {"KilogramsPerCubicCentimeter", U::KilogramsPerCubicCentimeter},
    {"kg/cm3", U::KilogramsPerCubicCentimeter},
    {"kg/cm\xC2\xB3", U::KilogramsPerCubicCentimeter},
    {"KilogramsPerCubicMeter", U::KilogramsPerCubicMeter},
    {"kg/m3", U::KilogramsPerCubicMeter},
    {"kg/m\xC2\xB3", U::KilogramsPerCubicMeter},
    {"Newtons", U::Newtons},
    {"N", U::Newtons},
    {"PoundsForce", U::PoundsForce},
    {"lbf", U::PoundsForce},
    {"KilogramsForce", U::KilogramsForce},
    {"kgf", U::KilogramsForce},
    {"KilogramsCentimetersPerSecondSquared", U::KilogramCentimetersPerSecondSquared},
    {"kgcm/s2", U::KilogramCentimetersPerSecondSquared},
    {"kgcm/s\xC2\xB2", U::KilogramCentimetersPerSecondSquared},
    {"NewtonMeters", U::NewtonMeters},
    {"Nm", U::NewtonMeters},
    {"KilogramsCentimetersSquaredPerSecondSquared", U::KilogramCentimetersSquaredPerSecondSquared},
    {"kgcm2/s2", U::KilogramCentimetersSquaredPerSecondSquared},
    {"kgcm\xC2\xB2/s\xC2\xB2", U::KilogramCentimetersSquaredPerSecondSquared},
    {"NewtonSeconds", U::NewtonSeconds},
    {"Ns", U::NewtonSeconds},
    {"KilogramCentimeters", U::KilogramCentimeters},
    {"kgcm", U::KilogramCentimeters},
    {"KilogramMeters", U::KilogramMeters},
    {"kgm", U::KilogramMeters},
    {"Decibels", U::Decibels},
    {"dB", U::Decibels},
    {"Hertz", U::Hertz},
    {"Hz", U::Hertz},
    {"Kilohertz", U::Kilohertz},
    {"KHz", U::Kilohertz},
    {"Megahertz", U::Megahertz},
    {"MHz", U::Megahertz},
    {"Gigahertz", U::Gigahertz},
    {"GHz", U::Gigahertz},
    {"RevolutionsPerMinute", U::RevolutionsPerMinute},
    {"rpm", U::RevolutionsPerMinute},
    {"Bytes", U::Bytes},
    {"B", U::Bytes},
    {"Kilobytes", U::Kilobytes},
    {"KB", U::Kilobytes},
    {"KiB", U::Kilobytes},
    {"Megabytes", U::Megabytes},
    {"MB", U::Megabytes},
    {"MiB", U::Megabytes},
    {"Gigabytes", U::Gigabytes},
    {"GB", U::Gigabytes},
    {"GiB", U::Gigabytes},
    {"Terabytes", U::Terabytes},
    {"TB", U::Terabytes},
    {"TiB", U::Terabytes},
    {"Lumens", U::Lumens},
    {"lm", U::Lumens},
    {"Candela", U::Candela},
    {"cd", U::Candela},
    {"Lux", U::Lux},
    {"lx", U::Lux},
    {"CandelaPerMeterSquared", U::CandelaPerMeter2},
    {"cd/m2", U::CandelaPerMeter2},
    {"CandelaPerMeter2", U::CandelaPerMeter2},
    {"Nits", U::Nit},
    {"nt", U::Nit},
    {"EV", U::ExposureValue},
    {"Nanoseconds", U::Nanoseconds},
    {"ns", U::Nanoseconds},
    {"Microseconds", U::Microseconds},
    {"us", U::Microseconds},
    {"\xC2\xB5s", U::Microseconds},
    {"Milliseconds", U::Milliseconds},
    {"ms", U::Milliseconds},
    {"Seconds", U::Seconds},
    {"s", U::Seconds},
    {"Minutes", U::Minutes},
    {"min", U::Minutes},
    {"Hours", U::Hours},
    {"hrs", U::Hours},
    {"Days", U::Days},
    {"dy", U::Days},
    {"Months", U::Months},
    {"mth", U::Months},
    {"Years", U::Years},
    {"yr", U::Years},
    {"ppi", U::PixelsPerInch},
    {"dpi", U::PixelsPerInch},
    {"Percent", U::Percentage},
    {"%", U::Percentage},
    {"Times", U::Multiplier},
    {"x", U::Multiplier},
    {"Multiplier", U::Multiplier},
    {"Pascals", U::Pascals},
    {"Pa", U::Pascals},
    {"KiloPascals", U::KiloPascals},
    {"kPa", U::KiloPascals},
    {"MegaPascals", U::MegaPascals},
    {"MPa", U::MegaPascals},
    {"GigaPascals", U::GigaPascals},
    {"GPa", U::GigaPascals},
};

// 표시 글자다(UE DisplayStrings 131–174행 — 단위 순서와 같다).
constexpr StringView DisplayStrings[UnitCount] = {"\xC2\xB5m",
                                                  "mm",
                                                  "cm",
                                                  "m",
                                                  "km",
                                                  "in",
                                                  "ft",
                                                  "yd",
                                                  "mi",
                                                  "ly",
                                                  "\xC2\xB0",
                                                  "rad",
                                                  "cm/s",
                                                  "m/s",
                                                  "km/h",
                                                  "mi/h",
                                                  "deg/s",
                                                  "rad/s",
                                                  "cm/s\xC2\xB2",
                                                  "m/s\xC2\xB2",
                                                  "\xC2\xB0"
                                                  "C",
                                                  "\xC2\xB0"
                                                  "F",
                                                  "K",
                                                  "\xC2\xB5g",
                                                  "mg",
                                                  "g",
                                                  "kg",
                                                  "t",
                                                  "oz",
                                                  "lb",
                                                  "st",
                                                  "g/cm\xC2\xB3",
                                                  "g/m\xC2\xB3",
                                                  "kg/cm\xC2\xB3",
                                                  "kg/m\xC2\xB3",
                                                  "N",
                                                  "lbf",
                                                  "kgf",
                                                  "kgcm/s\xC2\xB2",
                                                  "Nm",
                                                  "kgcm\xC2\xB2/s\xC2\xB2",
                                                  "Ns",
                                                  "kgcm",
                                                  "kgm",
                                                  "dB",
                                                  "Hz",
                                                  "KHz",
                                                  "MHz",
                                                  "GHz",
                                                  "rpm",
                                                  "B",
                                                  "KiB",
                                                  "MiB",
                                                  "GiB",
                                                  "TiB",
                                                  "lm",
                                                  "cd",
                                                  "lux",
                                                  "cd/m2",
                                                  "nt",
                                                  "EV",
                                                  "ns",
                                                  "\xC2\xB5s",
                                                  "ms",
                                                  "s",
                                                  "min",
                                                  "hr",
                                                  "dy",
                                                  "mth",
                                                  "yr",
                                                  "ppi",
                                                  "%",
                                                  "x",
                                                  "Pa",
                                                  "kPa",
                                                  "MPa",
                                                  "GPa"};

// 단위 종류다(UE UnitTypes 277–299행).
constexpr T UnitTypes[UnitCount] = {T::Distance,
                                    T::Distance,
                                    T::Distance,
                                    T::Distance,
                                    T::Distance,
                                    T::Distance,
                                    T::Distance,
                                    T::Distance,
                                    T::Distance,
                                    T::Distance,
                                    T::Angle,
                                    T::Angle,
                                    T::Speed,
                                    T::Speed,
                                    T::Speed,
                                    T::Speed,
                                    T::AngularSpeed,
                                    T::AngularSpeed,
                                    T::Acceleration,
                                    T::Acceleration,
                                    T::Temperature,
                                    T::Temperature,
                                    T::Temperature,
                                    T::Mass,
                                    T::Mass,
                                    T::Mass,
                                    T::Mass,
                                    T::Mass,
                                    T::Mass,
                                    T::Mass,
                                    T::Mass,
                                    T::Density,
                                    T::Density,
                                    T::Density,
                                    T::Density,
                                    T::Force,
                                    T::Force,
                                    T::Force,
                                    T::Force,
                                    T::Torque,
                                    T::Torque,
                                    T::Impulse,
                                    T::PositionalImpulse,
                                    T::PositionalImpulse,
                                    T::Loudness,
                                    T::Frequency,
                                    T::Frequency,
                                    T::Frequency,
                                    T::Frequency,
                                    T::Frequency,
                                    T::DataSize,
                                    T::DataSize,
                                    T::DataSize,
                                    T::DataSize,
                                    T::DataSize,
                                    T::LuminousFlux,
                                    T::LuminousIntensity,
                                    T::Illuminance,
                                    T::Luminance,
                                    T::Luminance,
                                    T::ExposureValue,
                                    T::Time,
                                    T::Time,
                                    T::Time,
                                    T::Time,
                                    T::Time,
                                    T::Time,
                                    T::Time,
                                    T::Time,
                                    T::Time,
                                    T::PixelDensity,
                                    T::Multipliers,
                                    T::Multipliers,
                                    T::Stress,
                                    T::Stress,
                                    T::Stress,
                                    T::Stress};

static_assert(sizeof(DisplayStrings) / sizeof(DisplayStrings[0]) == UnitCount,
              "Display strings must cover every numeric unit");
static_assert(sizeof(UnitTypes) / sizeof(UnitTypes[0]) == UnitCount,
              "Unit types must cover every numeric unit");

constexpr double MetersPerInch = 0.0254;
constexpr double InchesPerFoot = 12;
constexpr double FeetPerYard = 3;
constexpr double YardsPerMile = 1760;
constexpr double MetersPerLightyear = 9.4605284e15;
constexpr double GramsPerOunce = 28.3495;
constexpr double OuncesPerPound = 16;
constexpr double PoundsPerStone = 14;
constexpr double NewtonsPerPoundForce = 4.44822162;
constexpr double NewtonsPerKilogramForce = 9.80665;
constexpr double DaysPerYear = 365.242;
constexpr double MonthsPerYear = 12;
constexpr double HoursPerDay = 24;
constexpr double SecondsPerHour = 3600;
constexpr double BytesPerKilobyte = 1024;
constexpr double KelvinAtZeroCelsius = 273.15;
constexpr double FarenheitOffset = 459.67;
constexpr double DegreesPerRadian = 180.0 / 3.14159265358979323846;
constexpr double PerCent = 0.01;

// 종류 기준 단위로 곱할 계수다(UE *UnificationFactor 713–917행 — 길이 m, 각 °, 속도 km/h, 각속도
// °/s, 가속도 m/s², 질량 g, 밀도 g/cm³, 힘 N, 토크 N·m, 위치 충격량 kg·m, 주파수 kHz, 데이터 MB,
// 시간 h, 배수 1, 응력 Pa).
double UnificationFactor(U unit) noexcept {
    const double centimeter = 1.0 / math::CentimetersPerMeter<double>;
    switch (unit) {
    case U::Micrometers:
        return 1e-6;
    case U::Millimeters:
        return 1e-3;
    case U::Centimeters:
        return centimeter;
    case U::Kilometers:
        return 1e3;
    case U::Inches:
        return MetersPerInch;
    case U::Feet:
        return MetersPerInch * InchesPerFoot;
    case U::Yards:
        return MetersPerInch * InchesPerFoot * FeetPerYard;
    case U::Miles:
        return MetersPerInch * InchesPerFoot * FeetPerYard * YardsPerMile;
    case U::Lightyears:
        return MetersPerLightyear;
    case U::Radians:
        return DegreesPerRadian;
    case U::CentimetersPerSecond:
        return centimeter * SecondsPerHour / 1e3;
    case U::MetersPerSecond:
        return SecondsPerHour / 1e3;
    case U::MilesPerHour:
        return MetersPerInch * InchesPerFoot * FeetPerYard * YardsPerMile / 1e3;
    case U::RadiansPerSecond:
        return DegreesPerRadian;
    case U::CentimetersPerSecondSquared:
        return centimeter;
    case U::Micrograms:
        return 1e-6;
    case U::Milligrams:
        return 1e-3;
    case U::Kilograms:
        return 1e3;
    case U::MetricTons:
        return 1e6;
    case U::Ounces:
        return GramsPerOunce;
    case U::Pounds:
        return GramsPerOunce * OuncesPerPound;
    case U::Stones:
        return GramsPerOunce * OuncesPerPound * PoundsPerStone;
    case U::GramsPerCubicMeter:
        return 1e-6;
    case U::KilogramsPerCubicCentimeter:
        return 1e3;
    case U::KilogramsPerCubicMeter:
        return 1.0 / math::KilogramsPerCubicMeterPerGramPerCubicCentimeter<double>;
    case U::PoundsForce:
        return NewtonsPerPoundForce;
    case U::KilogramsForce:
        return NewtonsPerKilogramForce;
    case U::KilogramCentimetersPerSecondSquared:
        return centimeter;
    case U::KilogramCentimetersSquaredPerSecondSquared:
        return 1.0 / math::SquareCentimetersPerSquareMeter<double>;
    case U::KilogramCentimeters:
        return centimeter;
    case U::Hertz:
        return 1e-3;
    case U::Megahertz:
        return 1e3;
    case U::Gigahertz:
        return 1e6;
    case U::RevolutionsPerMinute:
        return 1e-3 / 60;
    case U::Bytes:
        return 1.0 / (BytesPerKilobyte * BytesPerKilobyte);
    case U::Kilobytes:
        return 1.0 / BytesPerKilobyte;
    case U::Gigabytes:
        return BytesPerKilobyte;
    case U::Terabytes:
        return BytesPerKilobyte * BytesPerKilobyte;
    case U::Nanoseconds:
        return 1e-9 / SecondsPerHour;
    case U::Microseconds:
        return 1e-6 / SecondsPerHour;
    case U::Milliseconds:
        return 1e-3 / SecondsPerHour;
    case U::Seconds:
        return 1.0 / SecondsPerHour;
    case U::Minutes:
        return 60.0 / SecondsPerHour;
    case U::Days:
        return HoursPerDay;
    case U::Months:
        return DaysPerYear * HoursPerDay / MonthsPerYear;
    case U::Years:
        return DaysPerYear * HoursPerDay;
    case U::Percentage:
        return PerCent;
    case U::KiloPascals:
        return 1e3;
    case U::MegaPascals:
        return 1e6;
    case U::GigaPascals:
        return 1e9;
    default:
        return 1;
    }
}

// 계수로 바꾸는 종류인지다(UE Convert — 나머지는 그대로 돌려준다).
bool ScalesByFactor(T type) noexcept {
    switch (type) {
    case T::Distance:
    case T::Angle:
    case T::Speed:
    case T::AngularSpeed:
    case T::Acceleration:
    case T::Mass:
    case T::Density:
    case T::Force:
    case T::Torque:
    case T::PositionalImpulse:
    case T::Frequency:
    case T::DataSize:
    case T::Time:
    case T::Multipliers:
    case T::Stress:
        return true;
    default:
        return false;
    }
}

char FoldCase(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII 대소문자 무시로 text가 prefix로 시작하는지다.
bool StartsWithIgnoreCase(StringView text, StringView prefix) noexcept {
    if (text.ByteCount() < prefix.ByteCount()) {
        return false;
    }

    for (usize i = 0; i < prefix.ByteCount(); ++i) {
        if (FoldCase(text.Data()[i]) != FoldCase(prefix.Data()[i])) {
            return false;
        }
    }

    return true;
}

// 가장 알맞은 단위 묶음 하나다(UE GetQuantizationBounds 940–1012행). factor는 다음 단위로 가는
// 배수다.
struct QuantizationStep {
    U unit;
    double factor;
};

constexpr QuantizationStep MetricDistance[] = {{U::Micrometers, 1000},
                                               {U::Millimeters, 10},
                                               {U::Centimeters, 100},
                                               {U::Meters, 1000},
                                               {U::Kilometers, 0}};
constexpr QuantizationStep ImperialDistance[] = {
    {U::Inches, 12}, {U::Feet, 3}, {U::Yards, 1760}, {U::Miles, 0}};
constexpr QuantizationStep MetricMass[] = {{U::Micrograms, 1000},
                                           {U::Milligrams, 1000},
                                           {U::Grams, 1000},
                                           {U::Kilograms, 1000},
                                           {U::MetricTons, 0}};
constexpr QuantizationStep ImperialMass[] = {{U::Ounces, 16}, {U::Pounds, 14}, {U::Stones, 0}};
constexpr QuantizationStep MetricDensity[] = {{U::GramsPerCubicCentimeter, 0.000001},
                                              {U::GramsPerCubicMeter, 1000},
                                              {U::KilogramsPerCubicCentimeter, 0.000001},
                                              {U::KilogramsPerCubicMeter, 0}};
constexpr QuantizationStep FrequencySteps[] = {
    {U::Hertz, 1000}, {U::Kilohertz, 1000}, {U::Megahertz, 1000}, {U::Gigahertz, 0}};
constexpr QuantizationStep DataSizeSteps[] = {{U::Bytes, 1024},
                                              {U::Kilobytes, 1024},
                                              {U::Megabytes, 1024},
                                              {U::Gigabytes, 1024},
                                              {U::Terabytes, 0}};
constexpr QuantizationStep TimeSteps[] = {
    {U::Microseconds, 1000}, {U::Milliseconds, 1000},
    {U::Seconds, 60},        {U::Minutes, 60},
    {U::Hours, 24},          {U::Days, DaysPerYear / MonthsPerYear},
    {U::Months, 12},         {U::Years, 0}};
constexpr QuantizationStep StressSteps[] = {
    {U::Pascals, 1000}, {U::KiloPascals, 1000}, {U::MegaPascals, 1000}, {U::GigaPascals, 0}};

struct QuantizationBounds {
    const QuantizationStep* steps = nullptr;
    usize count = 0;
};

template <usize N>
constexpr QuantizationBounds Bounds(const QuantizationStep (&steps)[N]) noexcept {
    return {steps, N};
}

// 단위가 든 묶음이다. rpm은 옮기지 않는다(UE는 주파수 묶음에 넣고 자리를 잘못 읽는다).
QuantizationBounds FindBounds(U unit) noexcept {
    const QuantizationBounds all[] = {
        Bounds(MetricDistance), Bounds(ImperialDistance), Bounds(MetricMass),
        Bounds(ImperialMass),   Bounds(MetricDensity),    Bounds(FrequencySteps),
        Bounds(DataSizeSteps),  Bounds(TimeSteps),        Bounds(StressSteps)};
    for (const QuantizationBounds& bounds : all) {
        for (usize i = 0; i < bounds.count; ++i) {
            if (bounds.steps[i].unit == unit) {
                return bounds;
            }
        }
    }

    return {};
}

} // namespace

NumericUnitType GetNumericUnitType(NumericUnit unit) noexcept {
    const usize index = static_cast<usize>(unit);
    return index < UnitCount ? UnitTypes[index] : T::None;
}

StringView NumericUnitDisplayString(NumericUnit unit) noexcept {
    const usize index = static_cast<usize>(unit);
    return index < UnitCount ? DisplayStrings[index] : StringView{};
}

Optional<NumericUnit> NumericUnitFromString(StringView text) noexcept {
    if (text.IsEmpty()) {
        return {};
    }

    for (const ParseCandidate& candidate : ParseCandidates) {
        if (candidate.text.ByteCount() == text.ByteCount() &&
            StartsWithIgnoreCase(text, candidate.text)) {
            return candidate.unit;
        }
    }

    return {};
}

bool AreNumericUnitsCompatible(NumericUnit from, NumericUnit to) noexcept {
    return GetNumericUnitType(from) == GetNumericUnitType(to);
}

double ConvertNumericUnit(double value, NumericUnit from, NumericUnit to) noexcept {
    if (from == to || from == U::Unspecified || to == U::Unspecified ||
        !AreNumericUnitsCompatible(from, to)) {
        return value;
    }

    const T type = GetNumericUnitType(from);
    if (type == T::Temperature) {
        // 켈빈을 거친다(UE Convert 110–131행).
        double kelvin = value;
        if (from == U::Celsius) {
            kelvin = value + KelvinAtZeroCelsius;
        } else if (from == U::Farenheit) {
            kelvin = (value + FarenheitOffset) * 5.0 / 9.0;
        }

        if (to == U::Celsius) {
            return kelvin - KelvinAtZeroCelsius;
        }

        if (to == U::Farenheit) {
            return kelvin * 9.0 / 5.0 - FarenheitOffset;
        }

        return kelvin;
    }

    if (!ScalesByFactor(type)) {
        return value;
    }

    return value * UnificationFactor(from) / UnificationFactor(to);
}

NumericUnitValue QuantizeNumericUnitToBestFit(double value, NumericUnit unit) noexcept {
    const QuantizationBounds bounds = FindBounds(unit);
    if (bounds.count == 0 || !math::IsFinite(value)) {
        return {value, unit};
    }

    usize current = 0;
    while (bounds.steps[current].unit != unit) {
        ++current;
    }

    NumericUnitValue result{value, unit};
    if (math::Abs(value) > 1) {
        // 큰 값은 위 단위로(UE 170–186행).
        for (usize i = current; i + 1 < bounds.count; ++i) {
            const double factor = bounds.steps[i].factor;
            if (factor == 0) {
                break;
            }

            const double next = result.value / factor;
            if (math::Abs(next) < 1) {
                break;
            }

            result = {next, bounds.steps[i + 1].unit};
        }
    } else if (value != 0) {
        // 작은 값은 아래 단위로(UE 187–199행).
        for (usize i = current; i > 0; --i) {
            result = {result.value * bounds.steps[i - 1].factor, bounds.steps[i - 1].unit};
            if (math::Abs(result.value) > 1) {
                break;
            }
        }
    }

    return result;
}

namespace {

enum class UnitTokenKind : uint8 {
    Number,
    Plus,
    Minus,
    Star,
    Slash,
    Open,
    Close,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign
};

struct UnitToken {
    UnitTokenKind kind = UnitTokenKind::Number;
    NumericUnitValue value{};
};

bool IsSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// 숫자 뒤 단위 글자를 대소문자 무시로 가장 길게 맞춘다(UE ConsumeNumberWithUnits). 맞은 길이다.
usize MatchUnit(StringView rest, NumericUnit& unit) noexcept {
    usize longest = 0;
    for (const ParseCandidate& candidate : ParseCandidates) {
        if (candidate.text.ByteCount() > longest && StartsWithIgnoreCase(rest, candidate.text)) {
            longest = candidate.text.ByteCount();
            unit = candidate.unit;
        }
    }

    return longest;
}

Result<usize> LexUnits(StringView text, UnitToken* tokens) noexcept {
    usize count = 0;
    const char* data = text.Data();
    const usize size = text.ByteCount();
    usize i = 0;
    while (i < size) {
        const char c = data[i];
        if (IsSpace(c)) {
            ++i;
            continue;
        }

        if (count == MaxNumericExpressionTokens) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }

        UnitToken& token = tokens[count++];
        if (IsDigit(c) || c == '.') {
            usize end = i;
            bool point = false;
            while (end < size && (IsDigit(data[end]) || (data[end] == '.' && !point))) {
                point = point || data[end] == '.';
                ++end;
            }

            auto parsed = TryParseDouble({data + i, end - i});
            if (!parsed || !math::IsFinite(parsed.Value())) {
                return Result<usize>::Failure(ErrorCode::InvalidArgument);
            }

            // 숫자와 단위 사이 공백을 건너뛴다("1.0 cm").
            usize unitStart = end;
            while (unitStart < size && IsSpace(data[unitStart])) {
                ++unitStart;
            }

            NumericUnit unit = U::Unspecified;
            const usize matched = MatchUnit({data + unitStart, size - unitStart}, unit);
            token = {UnitTokenKind::Number, {parsed.Value(), unit}};
            i = matched != 0 ? unitStart + matched : end;
            continue;
        }

        const bool assign = i + 1 < size && data[i + 1] == '=';
        switch (c) {
        case '+':
            token.kind = assign ? UnitTokenKind::PlusAssign : UnitTokenKind::Plus;
            break;
        case '-':
            token.kind = assign ? UnitTokenKind::MinusAssign : UnitTokenKind::Minus;
            break;
        case '*':
            token.kind = assign ? UnitTokenKind::StarAssign : UnitTokenKind::Star;
            break;
        case '/':
            token.kind = assign ? UnitTokenKind::SlashAssign : UnitTokenKind::Slash;
            break;
        case '(':
            token.kind = UnitTokenKind::Open;
            break;
        case ')':
            token.kind = UnitTokenKind::Close;
            break;
        default:
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        const bool twoChars = assign && (c == '+' || c == '-' || c == '*' || c == '/');
        i += twoChars ? 2 : 1;
    }

    return Result<usize>::Success(count);
}

// 단위 값을 계산하는 재귀 하강 계산기다(우선순위: 단항 > * / > + -, 모두 왼쪽 결합).
class UnitParser {
public:
    UnitParser(const UnitToken* tokens, usize count, NumericUnit defaultUnit) noexcept
        : tokens_(tokens), count_(count), default_(defaultUnit) {
    }

    Result<NumericUnitValue> Run() noexcept {
        auto value = Sum();
        if (value && position_ != count_) {
            return Fail();
        }

        return value;
    }

private:
    static Result<NumericUnitValue> Fail(ErrorCode error = ErrorCode::InvalidArgument) noexcept {
        return Result<NumericUnitValue>::Failure(error);
    }

    bool At(UnitTokenKind kind) const noexcept {
        return position_ < count_ && tokens_[position_].kind == kind;
    }

    // 더하기·빼기(UE MapBinary<FPlus>·<FMinus>): 한쪽만 단위가 없으면 그쪽을 기본 단위로 본다.
    Result<NumericUnitValue> Combine(NumericUnitValue a, NumericUnitValue b,
                                     bool add) const noexcept {
        NumericUnit left = a.unit;
        NumericUnit right = b.unit;
        if (left == U::Unspecified && right != U::Unspecified) {
            left = default_;
        } else if (left != U::Unspecified && right == U::Unspecified) {
            right = default_;
        }

        if (!AreNumericUnitsCompatible(left, right)) {
            return Fail();
        }

        if (left != U::Unspecified) {
            const double other = ConvertNumericUnit(b.value, right, left);
            return Result<NumericUnitValue>::Success(
                {add ? a.value + other : a.value - other, left});
        }

        const double first = ConvertNumericUnit(a.value, left, right);
        return Result<NumericUnitValue>::Success({add ? first + b.value : first - b.value, right});
    }

    Result<NumericUnitValue> Sum() noexcept {
        auto left = Product();
        while (left && (At(UnitTokenKind::Plus) || At(UnitTokenKind::Minus))) {
            const bool add = At(UnitTokenKind::Plus);
            ++position_;
            auto right = Product();
            if (!right) {
                return right;
            }

            left = Combine(left.Value(), right.Value(), add);
        }

        return left;
    }

    Result<NumericUnitValue> Product() noexcept {
        auto left = Unary();
        while (left && (At(UnitTokenKind::Star) || At(UnitTokenKind::Slash))) {
            const bool multiply = At(UnitTokenKind::Star);
            ++position_;
            auto right = Unary();
            if (!right) {
                return right;
            }

            const NumericUnitValue a = left.Value();
            const NumericUnitValue b = right.Value();
            if (multiply) {
                // 단위끼리는 곱하지 않는다(UE "Cannot multiply by numbers with units").
                if (a.unit != U::Unspecified && b.unit != U::Unspecified) {
                    return Fail();
                }

                left = Result<NumericUnitValue>::Success(
                    {a.value * b.value, a.unit == U::Unspecified ? b.unit : a.unit});
            } else {
                if (b.unit != U::Unspecified || b.value == 0) {
                    return Fail();
                }

                left = Result<NumericUnitValue>::Success({a.value / b.value, a.unit});
            }
        }

        return left;
    }

    Result<NumericUnitValue> Unary() noexcept {
        if (At(UnitTokenKind::Plus) || At(UnitTokenKind::Minus)) {
            const bool negate = At(UnitTokenKind::Minus);
            ++position_;
            auto operand = Unary();
            if (!operand) {
                return operand;
            }

            NumericUnitValue v = operand.Value();
            v.value = negate ? -v.value : v.value;
            return Result<NumericUnitValue>::Success(v);
        }

        if (At(UnitTokenKind::Number)) {
            return Result<NumericUnitValue>::Success(tokens_[position_++].value);
        }

        if (!At(UnitTokenKind::Open)) {
            return Fail();
        }

        if (depth_ == MaxNumericExpressionDepth) {
            return Fail(ErrorCode::CapacityExceeded);
        }

        ++position_;
        ++depth_;
        auto inner = Sum();
        --depth_;
        if (!inner) {
            return inner;
        }

        if (!At(UnitTokenKind::Close)) {
            return Fail();
        }

        ++position_;
        return inner;
    }

    const UnitToken* tokens_;
    usize count_;
    NumericUnit default_;
    usize position_ = 0;
    usize depth_ = 0;
};

bool IsAssign(UnitTokenKind kind) noexcept {
    return kind == UnitTokenKind::PlusAssign || kind == UnitTokenKind::MinusAssign ||
           kind == UnitTokenKind::StarAssign || kind == UnitTokenKind::SlashAssign;
}

} // namespace

Result<NumericUnitValue> EvaluateNumericUnitExpression(StringView text, NumericUnit defaultUnit,
                                                       const NumericUnitValue& existing) noexcept {
    UnitToken tokens[MaxNumericExpressionTokens + 1];
    auto lexed = LexUnits(text, tokens + 1);
    if (!lexed) {
        return Result<NumericUnitValue>::Failure(lexed.Error());
    }

    usize count = lexed.Value();
    if (count == 0) {
        return Result<NumericUnitValue>::Failure(ErrorCode::InvalidArgument);
    }

    // 처음의 += -= *= /=는 "지금 값 op"로 바꾼다(UE Evaluate 505–535행).
    UnitToken* first = tokens + 1;
    if (IsAssign(first->kind)) {
        if (!math::IsFinite(existing.value)) {
            return Result<NumericUnitValue>::Failure(ErrorCode::InvalidArgument);
        }

        const UnitTokenKind lead = first->kind;
        first->kind = lead == UnitTokenKind::PlusAssign    ? UnitTokenKind::Plus
                      : lead == UnitTokenKind::MinusAssign ? UnitTokenKind::Minus
                      : lead == UnitTokenKind::StarAssign  ? UnitTokenKind::Star
                                                           : UnitTokenKind::Slash;
        tokens[0] = {UnitTokenKind::Number, existing};
        first = tokens;
        ++count;
    }

    for (usize i = 0; i < count; ++i) {
        if (IsAssign(first[i].kind)) {
            return Result<NumericUnitValue>::Failure(ErrorCode::InvalidArgument);
        }
    }

    UnitParser parser(first, count, defaultUnit);
    auto value = parser.Run();
    if (!value) {
        return value;
    }

    NumericUnitValue result = value.Value();
    if (math::IsNaN(result.value)) {
        return Result<NumericUnitValue>::Failure(ErrorCode::InvalidArgument);
    }

    if (!math::IsFinite(result.value)) {
        return Result<NumericUnitValue>::Failure(ErrorCode::Overflow);
    }

    // 단위 없는 결과는 기본 단위다(UE TryParseExpression).
    if (result.unit == U::Unspecified) {
        result.unit = defaultUnit;
    }

    return Result<NumericUnitValue>::Success(result);
}

} // namespace rocks::ui

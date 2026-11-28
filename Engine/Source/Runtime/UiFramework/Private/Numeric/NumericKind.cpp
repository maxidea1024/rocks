#include <Core/Math/MathUtility.h>
#include <Base/NumericLimits.h>
#include <UiFramework/Numeric/NumericKind.h>

namespace rocks::ui {

namespace {

// 64비트 정수로 옮겨도 넘치지 않는 가장 큰 double이다(2^63·2^64 바로 아래 — double(INT64_MAX)는
// 2^63로 올림돼 변환이 넘친다).
constexpr double Int64MaxDouble = 9223372036854774784.0;
constexpr double UInt64MaxDouble = 18446744073709549568.0;

} // namespace

bool IsIntegralNumericKind(NumericKind kind) noexcept {
    return kind != NumericKind::Double && kind != NumericKind::Float;
}

double NumericKindLowest(NumericKind kind) noexcept {
    switch (kind) {
    case NumericKind::Double:
        return NumericLimits<double>::Lowest();
    case NumericKind::Float:
        return static_cast<double>(NumericLimits<float>::Lowest());
    case NumericKind::Int8:
        return static_cast<double>(NumericLimits<int8>::Lowest());
    case NumericKind::Int16:
        return static_cast<double>(NumericLimits<int16>::Lowest());
    case NumericKind::Int32:
        return static_cast<double>(NumericLimits<int32>::Lowest());
    case NumericKind::Int64:
        return static_cast<double>(NumericLimits<int64>::Lowest());
    case NumericKind::UInt8:
    case NumericKind::UInt16:
    case NumericKind::UInt32:
    case NumericKind::UInt64:
        return 0.0;
    }

    return NumericLimits<double>::Lowest();
}

double NumericKindMax(NumericKind kind) noexcept {
    switch (kind) {
    case NumericKind::Double:
        return NumericLimits<double>::Max();
    case NumericKind::Float:
        return static_cast<double>(NumericLimits<float>::Max());
    case NumericKind::Int8:
        return static_cast<double>(NumericLimits<int8>::Max());
    case NumericKind::Int16:
        return static_cast<double>(NumericLimits<int16>::Max());
    case NumericKind::Int32:
        return static_cast<double>(NumericLimits<int32>::Max());
    case NumericKind::Int64:
        return Int64MaxDouble;
    case NumericKind::UInt8:
        return static_cast<double>(NumericLimits<uint8>::Max());
    case NumericKind::UInt16:
        return static_cast<double>(NumericLimits<uint16>::Max());
    case NumericKind::UInt32:
        return static_cast<double>(NumericLimits<uint32>::Max());
    case NumericKind::UInt64:
        return UInt64MaxDouble;
    }

    return NumericLimits<double>::Max();
}

double RoundToNumericKind(double value, NumericKind kind) noexcept {
    if (!math::IsFinite(value)) {
        return value;
    }

    double result = IsIntegralNumericKind(kind) ? math::Floor(value + 0.5) : value;
    result = math::Clamp(result, NumericKindLowest(kind), NumericKindMax(kind));
    if (kind == NumericKind::Float) {
        result = static_cast<double>(static_cast<float>(result));
    }

    return result;
}

} // namespace rocks::ui

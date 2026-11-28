#include <UiFramework/Numeric/NumericUnitFormat.h>

namespace rocks::ui {

namespace {

// 고정 표시 단위가 쓸 수 있는 것인지다.
bool HasDisplayUnit(const NumericUnitOptions& units) noexcept {
    return units.display && units.display.Value() != NumericUnit::Unspecified &&
           AreNumericUnitsCompatible(units.underlying, units.display.Value());
}

} // namespace

Result<void> FormatNumberWithUnit(double value, const NumericFormatOptions& format,
                                  const NumericUnitOptions& units, String& out) noexcept {
    if (units.underlying == NumericUnit::Unspecified) {
        return FormatNumber(value, format, out);
    }

    NumericUnitValue shown{value, units.underlying};
    if (HasDisplayUnit(units)) {
        shown = {ConvertNumericUnit(value, units.underlying, units.display.Value()),
                 units.display.Value()};
    } else if (units.bestFit) {
        shown = QuantizeNumericUnitToBestFit(value, units.underlying);
    }

    // 다른 단위로 바꿔 보이면 정수 형식도 소수를 남긴다(500 m → "0.5 km").
    NumericFormatOptions options = format;
    if (shown.unit != units.underlying && IsIntegralNumericKind(format.kind)) {
        options.kind = NumericKind::Double;
        options.minFractionalDigits = 0;
    }

    auto written = FormatNumber(shown.value, options, out);
    if (!written) {
        return written;
    }

    auto spaced = out.TryAppend(" ");
    if (!spaced) {
        return spaced;
    }

    return out.TryAppend(NumericUnitDisplayString(shown.unit));
}

Result<double> ParseNumberWithUnit(StringView text, double existing, NumericKind kind,
                                   const NumericUnitOptions& units) noexcept {
    if (units.underlying == NumericUnit::Unspecified) {
        return ParseNumber(text, existing, kind);
    }

    const NumericUnit defaultUnit =
        HasDisplayUnit(units) ? units.display.Value() : units.underlying;
    auto parsed = EvaluateNumericUnitExpression(text, defaultUnit, {existing, units.underlying});
    if (!parsed) {
        return Result<double>::Failure(parsed.Error());
    }

    if (!AreNumericUnitsCompatible(parsed.Value().unit, units.underlying)) {
        return Result<double>::Failure(ErrorCode::InvalidArgument);
    }

    const double value =
        ConvertNumericUnit(parsed.Value().value, parsed.Value().unit, units.underlying);
    return Result<double>::Success(RoundToNumericKind(value, kind));
}

bool IsNumericUnitCharacter(char32_t c, const NumericUnitOptions& units) noexcept {
    if (units.underlying == NumericUnit::Unspecified) {
        return IsNumericCharacter(c);
    }

    return c != U'\t';
}

} // namespace rocks::ui

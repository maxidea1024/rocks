#include <Core/Math/MathUtility.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <UiFramework/Numeric/NumericExpression.h>
#include <UiFramework/Numeric/NumericFormat.h>

namespace rocks::ui {

namespace {

// "거의 정수"의 허용 오차다(UE FMath::IsNearlyEqual 기본 UE_SMALL_NUMBER).
constexpr double NearlyIntegerTolerance = 1.0e-8;
// 이 값 이상은 int64에 담기지 않는다(2^63).
constexpr double Int64Limit = 9223372036854775808.0;

// "-0", "-0.00"처럼 0뿐인 음수 표시인지다.
bool IsNegativeZeroText(StringView text) noexcept {
    if (text.ByteCount() < 2 || text.Data()[0] != '-') {
        return false;
    }

    for (usize i = 1; i < text.ByteCount(); ++i) {
        const char c = text.Data()[i];
        if (c != '0' && c != '.') {
            return false;
        }
    }

    return true;
}

Result<void> FormatIntegral(double value, NumericKind kind, StringBuilder<>& text) noexcept {
    const double rounded = RoundToNumericKind(value, kind);
    // uint64 상단(2^63 이상)은 부호 없는 수로 쓴다.
    if (rounded >= Int64Limit) {
        return text.TryAppendFormat("{}", static_cast<uint64>(rounded));
    }

    return text.TryAppendFormat("{}", static_cast<int64>(rounded));
}

} // namespace

Result<void> FormatNumber(double value, const NumericFormatOptions& options, String& out) noexcept {
    const int32 minimum = options.minFractionalDigits;
    const int32 maximum =
        options.maxFractionalDigits > minimum ? options.maxFractionalDigits : minimum;
    if (!math::IsFinite(value) || minimum < 0 || maximum > MaxNumericFractionalDigits) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    StringBuilder<> text(out.GetAllocator());
    if (IsIntegralNumericKind(options.kind)) {
        auto written = FormatIntegral(value, options.kind, text);
        if (!written) {
            return written;
        }

        return out.TryAssign(text.View());
    }

    const double shown = RoundToNumericKind(value, options.kind);
    // "{:.Nf}"는 최대 자리에서 반올림한다(N은 두 자리까지).
    char spec[] = "{:.00f}";
    spec[3] = static_cast<char>('0' + maximum / 10);
    spec[4] = static_cast<char>('0' + maximum % 10);
    auto written = text.TryAppendFormat(spec, shown);
    if (!written) {
        return written;
    }

    // 끝 0을 최소 자리까지 지우고, 소수가 남지 않으면 소수점도 지운다.
    const StringView full = text.View();
    usize length = full.ByteCount();
    if (maximum > 0) {
        const usize point = length - static_cast<usize>(maximum) - 1;
        while (length > point + 1 + static_cast<usize>(minimum) && full.Data()[length - 1] == '0') {
            --length;
        }

        if (length == point + 1) {
            --length;
        }
    }

    StringView trimmed(full.Data(), length);
    if (IsNegativeZeroText(trimmed)) {
        trimmed = StringView(full.Data() + 1, length - 1);
    }

    auto assigned = out.TryAssign(trimmed);
    if (!assigned) {
        return assigned;
    }

    // 자른 값이 정수와 거의 같지만 같지 않으면 "..."(UE FastDecimalFormat 517–597행 — 1e-16이
    // "0.0"으로만 보여 0으로 오해하지 않게).
    if (options.indicateNearlyInteger) {
        const double integral = math::Round(shown);
        if (math::Abs(integral - shown) <= NearlyIntegerTolerance && integral != shown) {
            return out.TryAppend("...");
        }
    }

    return Result<void>::Success();
}

Result<double> ParseNumber(StringView text, double existing, NumericKind kind) noexcept {
    auto plain = TryParseDouble(text);
    if (plain && math::IsFinite(plain.Value())) {
        return Result<double>::Success(RoundToNumericKind(plain.Value(), kind));
    }

    auto evaluated = EvaluateNumericExpression(text, existing);
    if (!evaluated) {
        return evaluated;
    }

    return Result<double>::Success(RoundToNumericKind(evaluated.Value(), kind));
}

bool IsNumericCharacter(char32_t c) noexcept {
    if (c >= U'0' && c <= U'9') {
        return true;
    }

    switch (c) {
    case U'(':
    case U')':
    case U'-':
    case U'+':
    case U'=':
    case U'\\':
    case U'/':
    case U'.':
    case U',':
    case U'*':
    case U'^':
    case U'%':
        return true;
    default:
        return false;
    }
}

} // namespace rocks::ui

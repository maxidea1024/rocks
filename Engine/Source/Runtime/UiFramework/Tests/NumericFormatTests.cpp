#include <Base/NumericLimits.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Numeric/NumericExpression.h>
#include <UiFramework/Numeric/NumericFormat.h>

// 숫자 형식·해석·수식 시험이다(Docs/Design/UiToolkit 24.2–24.3절 — UE TDefaultNumericTypeInterface·
// FBasicMathExpressionEvaluator): 자릿수·정수 반올림·거의 정수·음수 0, 숫자·수식(우선순위·결합·상대
// 입력·0 나눔·상한)·입력 글자.

namespace rocks::ui {

namespace {

// 값을 글자로 써서 돌려준다(실패면 "<error>").
String Formatted(Allocator& allocator, double value, const NumericFormatOptions& options) noexcept {
    String text(allocator);
    if (!FormatNumber(value, options, text)) {
        (void)text.TryAssign("<error>");
    }

    return text;
}

bool Evaluates(StringView text, double existing, double expected) noexcept {
    auto value = EvaluateNumericExpression(text, existing);
    return value && value.Value() == expected;
}

} // namespace

TEST(UiFramework_NumericFormatTrimsToTheFractionalDigitRange) {
    SystemAllocator allocator;
    // 기본 자릿수 1–6(UE Default*FractionalDigits).
    CHECK(Formatted(allocator, 2.5, {}).View() == "2.5");
    CHECK(Formatted(allocator, 3, {}).View() == "3.0");
    CHECK(Formatted(allocator, 1.0 / 3.0, {}).View() == "0.333333");
    CHECK(Formatted(allocator, 0.1234567, {}).View() == "0.123457");
    CHECK(Formatted(allocator, 3, {.minFractionalDigits = 2}).View() == "3.00");
    CHECK(Formatted(allocator, 3.25, {.minFractionalDigits = 0, .maxFractionalDigits = 0}).View() ==
          "3");
    CHECK(Formatted(allocator, 12.5, {.minFractionalDigits = 0}).View() == "12.5");
    // 최대가 최소보다 작으면 최소를 쓴다.
    CHECK(Formatted(allocator, 1, {.minFractionalDigits = 3, .maxFractionalDigits = 1}).View() ==
          "1.000");
    // 음수 0은 "0"으로 쓴다(UE는 "-0.0").
    CHECK(Formatted(allocator, -0.0000001, {}).View() == "0.0");
    CHECK(Formatted(allocator, -1.5, {}).View() == "-1.5");
    // 범위 밖 자릿수·유한하지 않은 값은 오류다.
    String text(allocator);
    CHECK(!FormatNumber(1, {.maxFractionalDigits = MaxNumericFractionalDigits + 1}, text));
    CHECK(!FormatNumber(1, {.minFractionalDigits = -1}, text));
    CHECK(!FormatNumber(NumericLimits<double>::Max() * 2, {}, text));
}

TEST(UiFramework_NumericFormatRoundsIntegralKindsAndIndicatesNearlyIntegers) {
    SystemAllocator allocator;
    CHECK(Formatted(allocator, 2.5, {.kind = NumericKind::Int32}).View() == "3");
    CHECK(Formatted(allocator, -2.5, {.kind = NumericKind::Int32}).View() == "-2");
    CHECK(Formatted(allocator, 300, {.kind = NumericKind::UInt8}).View() == "255");
    CHECK(Formatted(allocator, -5, {.kind = NumericKind::UInt32}).View() == "0");
    CHECK(Formatted(allocator, 18446744073709551615.0, {.kind = NumericKind::UInt64}).View() ==
          "18446744073709549568");
    // 거의 정수면 "..."(UE IndicateNearlyInteger).
    CHECK(Formatted(allocator, 1e-16, {.indicateNearlyInteger = true}).View() == "0.0...");
    CHECK(Formatted(allocator, 2, {.indicateNearlyInteger = true}).View() == "2.0");
    CHECK(Formatted(allocator, 1e-16, {}).View() == "0.0");
    CHECK(RoundToNumericKind(2.5, NumericKind::Int32) == 3);
    CHECK(RoundToNumericKind(1e10, NumericKind::Int16) == 32767);
    CHECK(RoundToNumericKind(0.1, NumericKind::Float) == static_cast<double>(0.1f));
}

TEST(UiFramework_NumericExpressionFollowsUePrecedenceAndAssociativity) {
    CHECK(Evaluates("10+5*2", 0, 20));
    CHECK(Evaluates("(10+5)*2", 0, 30));
    CHECK(Evaluates("10 - 4 - 3", 0, 3));
    CHECK(Evaluates("16 / 4 / 2", 0, 2));
    CHECK(Evaluates("7 % 4", 0, 3));
    // ^는 오른쪽 결합이고, 단항은 ^보다 먼저 묶인다(UE 문법).
    CHECK(Evaluates("2^3^2", 0, 512));
    CHECK(Evaluates("-2^2", 0, 4));
    CHECK(Evaluates("2^-1", 0, 0.5));
    CHECK(Evaluates("sqrt 16 + 1", 0, 5));
    CHECK(Evaluates("sqrt(9)", 0, 3));
    CHECK(Evaluates("--3", 0, 3));
    CHECK(Evaluates(".5 + 5.", 0, 5.5));
    // 상대 입력은 지금 값을 앞에 둔다.
    CHECK(Evaluates("+=5", 10, 15));
    CHECK(Evaluates("-= 2", 10, 8));
    CHECK(Evaluates("*=2", 7, 14));
    CHECK(Evaluates("/=4", 10, 2.5));
    CHECK(Evaluates("*=2+1", 7, 15));
}

TEST(UiFramework_NumericExpressionRejectsInvalidInput) {
    const auto error = [](StringView text) noexcept {
        auto value = EvaluateNumericExpression(text, 1);
        return value ? ErrorCode::None : value.Error();
    };

    CHECK(error("") == ErrorCode::InvalidArgument);
    CHECK(error("   ") == ErrorCode::InvalidArgument);
    CHECK(error("1/0") == ErrorCode::InvalidArgument);
    CHECK(error("5%0") == ErrorCode::InvalidArgument);
    CHECK(error("(1+2") == ErrorCode::InvalidArgument);
    CHECK(error("1+") == ErrorCode::InvalidArgument);
    CHECK(error("1 2") == ErrorCode::InvalidArgument);
    CHECK(error("1+=2") == ErrorCode::InvalidArgument);
    CHECK(error("1.2.3") == ErrorCode::InvalidArgument);
    CHECK(error("abc") == ErrorCode::InvalidArgument);
    CHECK(error("sqrt(-1)") == ErrorCode::InvalidArgument);
    CHECK(error("10^400") == ErrorCode::Overflow);
    // 토큰·괄호 깊이 상한.
    SystemAllocator allocator;
    String many(allocator);
    for (usize i = 0; i <= MaxNumericExpressionTokens / 2; ++i) {
        REQUIRE(many.TryAppend("1+"));
    }

    REQUIRE(many.TryAppend("1"));
    CHECK(error(many.View()) == ErrorCode::CapacityExceeded);
    String deep(allocator);
    for (usize i = 0; i <= MaxNumericExpressionDepth; ++i) {
        REQUIRE(deep.TryAppend("("));
    }

    REQUIRE(deep.TryAppend("1"));
    for (usize i = 0; i <= MaxNumericExpressionDepth; ++i) {
        REQUIRE(deep.TryAppend(")"));
    }

    CHECK(error(deep.View()) == ErrorCode::CapacityExceeded);
}

TEST(UiFramework_ParseNumberReadsNumbersThenExpressionsAndFiltersCharacters) {
    auto plain = ParseNumber("12.75", 0, NumericKind::Double);
    CHECK(plain && plain.Value() == 12.75);
    auto expression = ParseNumber("10+5*2", 0, NumericKind::Int32);
    CHECK(expression && expression.Value() == 20);
    auto rounded = ParseNumber("2.5", 0, NumericKind::Int32);
    CHECK(rounded && rounded.Value() == 3);
    auto relative = ParseNumber("*=3", 4, NumericKind::Double);
    CHECK(relative && relative.Value() == 12);
    CHECK(!ParseNumber("nan", 0, NumericKind::Double));
    CHECK(!ParseNumber("inf", 0, NumericKind::Double));
    CHECK(!ParseNumber("1/0", 0, NumericKind::Double));
    // 칠 수 있는 글자(UE IsCharacterValid).
    for (char32_t c : U"0123456789()-+=\\/.,*^%") {
        if (c != 0) {
            CHECK(IsNumericCharacter(c));
        }
    }

    CHECK(!IsNumericCharacter(U' '));
    CHECK(!IsNumericCharacter(U'a'));
    CHECK(!IsNumericCharacter(U'e'));
}

} // namespace rocks::ui

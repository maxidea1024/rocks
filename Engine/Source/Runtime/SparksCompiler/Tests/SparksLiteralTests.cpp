#include <Core/Memory/SystemAllocator.h>
#include <SparksCompiler/Literal.h>
#include <TestSupport/Test.h>

// Sparks 리터럴 값 해석 시험이다(Docs/Design/SparksLanguage.ko.md 3.6절, SD54): 10진·16진·2진
// 정수와 구분자, uint64 경계, 실수의 구분자·`.5`·범위.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_literal_tests_file {

TEST(SparksLiteral_DecodesIntegers) {
    struct Case {
        StringView text;
        uint64 value;
    };
    const Case cases[] = {
        {"0", 0},
        {"007", 7},
        {"1_000_000", 1000000},
        {"1__0", 10},
        {"0xFF", 255},
        {"0X_ff_FF", 65535},
        {"0b1010_0101", 165},
        {"0B_1", 1},
        {"9223372036854775808", 9223372036854775808ull},
        {"18446744073709551615", NumericLimits<uint64>::Max()},
        {"0xFFFF_FFFF_FFFF_FFFF", NumericLimits<uint64>::Max()},
    };
    for (const Case& item : cases) {
        auto value = TryDecodeIntegerLiteral(item.text);
        REQUIRE(value);
        CHECK(value.Value() == item.value);
    }

    const StringView overflow[] = {
        "18446744073709551616", "0x1_0000_0000_0000_0000",
        "0b1"
        "0000000000000000000000000000000000000000000000000000000000000000"};
    for (const StringView text : overflow) {
        auto value = TryDecodeIntegerLiteral(text);
        CHECK(!value && value.Error() == ErrorCode::Overflow);
    }
}

TEST(SparksLiteral_DecodesFloats) {
    SystemAllocator allocator;
    {
        struct Case {
            StringView text;
            double value;
        };
        const Case cases[] = {
            {"1.5", 1.5},       {".5", 0.5},      {"1e3", 1000.0}, {"1_000.25", 1000.25},
            {"1.5E-3", 0.0015}, {"2e+1_0", 2e10}, {"1e-400", 0.0}, // 너무 작은 값은 0
        };
        for (const Case& item : cases) {
            auto value = TryDecodeFloatLiteral(allocator, item.text);
            REQUIRE(value);
            CHECK(value.Value() == item.value);
        }

        auto tooLarge = TryDecodeFloatLiteral(allocator, "1e400");
        CHECK(!tooLarge && tooLarge.Error() == ErrorCode::Overflow);

        // inline 크기(64 byte)를 넘는 긴 리터럴도 정확히 푼다.
        auto longText = TryDecodeFloatLiteral(
            allocator, "1_0000000000_0000000000_0000000000_0000000000_0000000000_0000000000.5");
        REQUIRE(longText);
        CHECK(longText.Value() == 1e60);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace sparks_literal_tests_file

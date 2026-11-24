#include <TestSupport/Test.h>
#include <TextLayout/TextShaping.h>

using namespace rocks;
namespace text_shaping_rules_tests_file {

// Auto shaping 판정 시험이다(Docs/Design/Font 6.4절, UE SlateTextShaper.cpp 154–273행 구간 표).

TEST(TextLayout_RequiresFullShapingMatchesUnrealRanges) {
    CHECK(!text::RequiresFullShaping("abc"));
    // 한글 음절은 표에 없다(KerningOnly).
    CHECK(!text::RequiresFullShaping("\xED\x95\x9C"));
    // 결합 문자(U+0301), ZWJ(U+200D), 태국어(U+0E2A), 데바나가리(U+0915), BMP 밖(U+1F600).
    CHECK(text::RequiresFullShaping("e\xCC\x81"));
    CHECK(text::RequiresFullShaping("a\xE2\x80\x8D"
                                    "b"));
    CHECK(text::RequiresFullShaping("\xE0\xB8\xAA"));
    CHECK(text::RequiresFullShaping("\xE0\xA4\x95"));
    CHECK(text::RequiresFullShaping("\xF0\x9F\x98\x80"));
    // 구간 경계: U+036F는 안, U+0370은 밖이다.
    CHECK(text::RequiresFullShaping("\xCD\xAF"));
    CHECK(!text::RequiresFullShaping("\xCD\xB0"));
    // 잘못된 UTF-8은 FullShaping 쪽으로 본다.
    CHECK(text::RequiresFullShaping(StringView("\xFF", 1)));
}

} // namespace text_shaping_rules_tests_file

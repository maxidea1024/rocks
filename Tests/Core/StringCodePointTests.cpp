#include "Tests/Support/Test.h"
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>

namespace rocks::test {
namespace {

bool Is(StringView view, const char* expected) {
    return view == StringView(expected);
}

// "가"(3 byte) "a"(1) "é"(2) "😀"(U+1F600, 4) — byte 길이가 서로 다른 code point를 섞는다.
constexpr const char* Mixed = "\xea\xb0\x80"
                              "a"
                              "\xc3\xa9"
                              "\xf0\x9f\x98\x80";

} // namespace

TEST(StringCodePoints_CountAndSlice) {
    const StringView text(Mixed);
    CHECK(text.ByteCount() == 10);
    CHECK(text.TryCountCodePoints().Value() == 4);
    CHECK(Is(text.TryLeftCodePoints(1).Value(), "\xea\xb0\x80"));
    CHECK(Is(text.TryLeftCodePoints(2).Value(), "\xea\xb0\x80"
                                                "a"));
    CHECK(Is(text.TryRightCodePoints(1).Value(), "\xf0\x9f\x98\x80"));
    CHECK(Is(text.TryRightCodePoints(2).Value(), "\xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(Is(text.TryMidCodePoints(1, 2).Value(), "a\xc3\xa9"));
    CHECK(Is(text.TryMidCodePoints(2).Value(), "\xc3\xa9\xf0\x9f\x98\x80"));
    // 범위는 길이로 제한한다(byte API와 같은 규약).
    CHECK(text.TryLeftCodePoints(100).Value() == text);
    CHECK(text.TryRightCodePoints(100).Value() == text);
    CHECK(text.TryLeftCodePoints(0).Value().IsEmpty());
    CHECK(text.TryMidCodePoints(4).Value().IsEmpty());
    CHECK(text.TryMidCodePoints(99, 3).Value().IsEmpty());
    CHECK(text.TryMidCodePoints(3, MaxSize).Value() == StringView("\xf0\x9f\x98\x80"));
    // 빈 문자열
    CHECK(StringView().TryCountCodePoints().Value() == 0);
    CHECK(StringView().TryLeftCodePoints(3).Value().IsEmpty());
}

TEST(StringCodePoints_ByteOffsets) {
    const StringView text(Mixed);
    const usize expected[] = {0, 3, 4, 6, 10};
    for (usize index = 0; index < 5; ++index) {
        auto offset = text.TryByteOffsetOfCodePoint(index);
        REQUIRE(offset);
        CHECK(offset.Value() == expected[index]);
    }
    CHECK(text.TryByteOffsetOfCodePoint(5).Error() == ErrorCode::InvalidArgument);
}

TEST(StringCodePoints_InvalidUtf8IsRejected) {
    const char* invalid[] = {"\xc3",             // 잘린 2-byte
                             "\xe0\x80\x80",     // overlong
                             "\xed\xa0\x80",     // surrogate
                             "\xf4\x90\x80\x80", // U+10FFFF 초과
                             "a\x80"};           // 단독 continuation
    for (const char* text : invalid) {
        const StringView view(text);
        CHECK(view.TryCountCodePoints().Error() == ErrorCode::InvalidArgument);
        CHECK(!view.TryLeftCodePoints(1));
        CHECK(!view.TryRightCodePoints(1));
        CHECK(!view.TryMidCodePoints(0, 1));
        CHECK(!view.TryByteOffsetOfCodePoint(0));
        CHECK(!view.TryCodePoints());
    }
}

TEST(StringCodePoints_IterationDecodesScalars) {
    auto points = StringView(Mixed).TryCodePoints();
    REQUIRE(points);
    const char32_t expected[] = {U'\xac00', U'a', U'\xe9', U'\x1f600'};
    usize index = 0;
    bool same = true;
    for (char32_t point : points.Value()) {
        same = same && index < 4 && point == expected[index];
        ++index;
    }
    CHECK(same && index == 4);
    // 결합 문자는 별도 code point다(grapheme 단위가 아님): "e" + U+0301
    auto combining = StringView("e\xcc\x81").TryCountCodePoints();
    CHECK(combining.Value() == 2);
    // embedded NUL도 유효한 code point(U+0000)다.
    const char withNul[] = {'a', '\0', 'b'};
    CHECK(StringView(withNul, 3).TryCountCodePoints().Value() == 3);
}

TEST(StringCodePoints_StringForwarding) {
    SystemAllocator allocator;
    String text(allocator);
    REQUIRE(text.TryAssign("\xed\x95\x9c\xea\xb8\x80 text")); // "한글 text"
    CHECK(text.TryCountCodePoints().Value() == 7);
    CHECK(Is(text.TryLeftCodePoints(2).Value(), "\xed\x95\x9c\xea\xb8\x80"));
    CHECK(Is(text.TryRightCodePoints(4).Value(), "text"));
    CHECK(Is(text.TryMidCodePoints(1, 1).Value(), "\xea\xb8\x80"));
}

} // namespace rocks::test

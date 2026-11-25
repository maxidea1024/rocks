#include <GraphicsTypes/IndexFormat.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace index_format_tests_file {

using namespace rocks::graphics;

// 고정 번호표(RenderBase 결정 R20 — UInt16 = 1, UInt32 = 2)와 byte 크기다.
TEST(IndexFormat_FixedNumbersAndSizes) {
    CHECK(IndexFormatNumber(IndexFormat::UInt16) == 1u);
    CHECK(IndexFormatNumber(IndexFormat::UInt32) == 2u);
    CHECK(IndexFormatCount == 2u);
    CHECK(IndexFormatSize(IndexFormat::UInt16) == 2u);
    CHECK(IndexFormatSize(IndexFormat::UInt32) == 4u);
    CHECK(IsValidIndexFormat(IndexFormat::UInt16));
    CHECK(IsValidIndexFormat(IndexFormat::UInt32));
    CHECK(!IsValidIndexFormat(static_cast<IndexFormat>(0)));
    CHECK(!IsValidIndexFormat(static_cast<IndexFormat>(3)));
    CHECK(IndexFormatSize(static_cast<IndexFormat>(0)) == 0u);
    CHECK(IndexFormatSize(static_cast<IndexFormat>(3)) == 0u);
}

// 번호 → 형식은 번호표 안만 받는다(0·3·최댓값은 InvalidArgument).
TEST(IndexFormat_FromNumberRejectsOutsideTable) {
    auto u16 = IndexFormatFromNumber(1);
    REQUIRE(u16);
    CHECK(u16.Value() == IndexFormat::UInt16);
    auto u32 = IndexFormatFromNumber(2);
    REQUIRE(u32);
    CHECK(u32.Value() == IndexFormat::UInt32);
    const uint32 rejected[] = {0u, 3u, 255u, 256u, 0xFFFFFFFFu};
    for (const uint32 number : rejected) {
        auto parsed = IndexFormatFromNumber(number);
        REQUIRE(!parsed);
        CHECK(parsed.Error() == ErrorCode::InvalidArgument);
    }
}

} // namespace index_format_tests_file

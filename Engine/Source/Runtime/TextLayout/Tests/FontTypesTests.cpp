#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <TextLayout/FontFaceData.h>
#include <TextLayout/FontTypes.h>

using namespace rocks;
namespace font_types_tests_file {

// TextLayout 글꼴 형식 시험이다(Docs/Design/Font 6.1·6.8절, FT2a).

TEST(TextLayout_ComputeFontPixelSizeMatchesUnrealFormula) {
    // 96 DPI 포인트: 24pt = 32px, 12pt = 16px, 9pt = 12px(UE LegacyFontSize 9).
    CHECK(text::ComputeFontPixelSize(24.0f, 1.0f) == 32u);
    CHECK(text::ComputeFontPixelSize(12.0f, 1.0f) == 16u);
    CHECK(text::ComputeFontPixelSize(9.0f, 1.0f) == 12u);
    // 10pt = 13.33px, 배율 1.5 → 20px(26.6 반올림 뒤 픽셀 반올림).
    CHECK(text::ComputeFontPixelSize(10.0f, 1.5f) == 20u);
    // 10pt × 1.0 = 13.33px → 13px, 10pt × 2.0 = 26.67px → 27px.
    CHECK(text::ComputeFontPixelSize(10.0f, 1.0f) == 13u);
    CHECK(text::ComputeFontPixelSize(10.0f, 2.0f) == 27u);
    // 0·음수 크기와 배율은 0이다.
    CHECK(text::ComputeFontPixelSize(0.0f, 1.0f) == 0u);
    CHECK(text::ComputeFontPixelSize(-5.0f, 1.0f) == 0u);
    CHECK(text::ComputeFontPixelSize(24.0f, -1.0f) == 0u);
}

TEST(TextLayout_FontFaceDataCopiesBytes) {
    SystemAllocator allocator;
    uint8 bytes[] = {1, 2, 3, 4};
    auto data = text::FontFaceData::TryCreate(allocator, Span<const uint8>(bytes, sizeof(bytes)));
    CHECK(static_cast<bool>(data));
    if (data) {
        bytes[0] = 9;
        CHECK(data.Value()->Bytes().Size() == 4u);
        CHECK(data.Value()->Bytes()[0] == 1u && data.Value()->Bytes()[3] == 4u);
    }

    auto empty = text::FontFaceData::TryCreate(allocator, Span<const uint8>());
    CHECK(!empty && empty.Error() == ErrorCode::InvalidArgument);
}

} // namespace font_types_tests_file

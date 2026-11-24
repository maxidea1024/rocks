#pragma once

#include <TextLayout/CompositeFont.h>
#include <TextLayout/TextBackend.h>
#include <TextLayout/TextShaping.h>

// 조합 글꼴로 문자열을 shaping한다(Docs/Design/Font 6.4·6.8절, UE
// FSlateTextShaper::ShapeBidirectionalText).
//
// 사용 예:
//   Array<text::ShapedGlyph> glyphs(allocator);
//   text::ShapeText(backend, *font, "Hello 한글", {.size = 24.0f}, glyphs);
//   const float width = text::MeasureWidth(glyphs.View());
namespace rocks::text {

struct ShapeOptions {
    // 96 DPI 포인트 크기와 UI 배율이다.
    float size = 24.0f;
    float scale = 1.0f;
    StringView typeface = {};
    FontStyleRequest style = {};
    FontFallback fallback = FontFallback::All;
    TextShapingMethod method = TextShapingMethod::Auto;
    TextBaseDirection direction = TextBaseDirection::Auto;
    // 1/1000 em 자간이다(UE LetterSpacing).
    int32 letterSpacing = 0;
    // 고정폭이면 advance = monospacedWidth × size × scale(UE와 같다).
    bool monospaced = false;
    float monospacedWidth = 1.0f;
};

// utf8을 shaping해 out에 시각 순서 글리프를 쓴다(out은 먼저 비운다). 잘못된 UTF-8·크기·배율 0
// 이하는 InvalidArgument, backend 오류는 그 오류다. font의 culture를 경계 규칙과 HarfBuzz 언어로
// 쓴다.
ROCKS_TEXTLAYOUT_API Result<void> ShapeText(TextBackend& backend, ResolvedCompositeFont& font,
                                            StringView utf8, const ShapeOptions& options,
                                            Array<ShapedGlyph>& out) noexcept;

} // namespace rocks::text

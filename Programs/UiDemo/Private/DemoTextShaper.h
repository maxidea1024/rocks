#pragma once

#include "DemoText.h"

#include <UiFramework/Text/TextShaper.h>

// 데모 편집 텍스트 셰이퍼다(Docs/Design/UiToolkit 22.4·22.9절). ui::TextShaper를 DemoText(엔진
// 글꼴 조합)로 구현한다. Text는 카탈로그 없이 원문으로 풀고(갤러리는 번역이 없다), fontSize를
// 96 DPI 포인트 크기(UE FSlateFontInfo Size — 갤러리 FontStyle과 같은 단위)의 Regular로 셰이핑한다.
// 단어 경계는 ICU(text::TextBackend FindBoundaries(Word) — UE FTextBreakIterator)다. DemoText는 이
// 셰이퍼를 쥔 편집기보다 오래 살아야 한다(갤러리 창이 소유한다).
//
// 사용 예:
//   auto shaper = SharedPtr<DemoTextShaper>::TryMake(allocator, allocator, context.text);
//   auto box = SharedPtr<EditableTextBox>::TryMake(allocator, allocator, shaper.Value(), platform,
//                                                  false);
namespace rocks::ui {

class DemoTextShaper final : public TextShaper {
public:
    DemoTextShaper(Allocator& allocator, DemoText& text) noexcept;

    Result<void> Shape(TextLabel& label, const Text& text, float fontSize) noexcept override;
    Result<void> FindWordBoundaries(StringView utf8, Array<usize>& out) noexcept override;

private:
    Allocator& allocator_;
    DemoText& text_;
};

} // namespace rocks::ui

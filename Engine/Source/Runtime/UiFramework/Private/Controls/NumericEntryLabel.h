#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 숫자 입력 칸의 글자 레이블이다(UE SNumericEntryBox::BuildLabel — Docs/Design/UiToolkit 24.6절).
// 둥근 바탕(RB(바탕, 3) — UE 꾸밈 png 대신) 위에 글자 자식을 여백 (1, 0, 6, 0)으로 두고, 글자는
// 전경색을 물려받는다. 누름은 받지 않는다(HitTestInvisible).
class NumericEntryLabel final : public Element {
    ROCKS_UI_ELEMENT(NumericEntryLabel, Element)

public:
    NumericEntryLabel(Allocator& allocator, ColorRef background) noexcept;

    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    ColorRef background_;
};

} // namespace rocks::ui

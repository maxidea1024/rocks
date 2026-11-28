#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 숫자 입력 칸의 좁은 색 레이블이다(UE SNumericEntryBox::BuildNarrowColorLabel —
// Docs/Design/UiToolkit 24.6절). 폭 2 세로 막대를 칸 높이로 그린다(RB(색, 1)). 누름은 받지 않는다.
class NumericColorBar final : public Element {
    ROCKS_UI_ELEMENT(NumericColorBar, Element)

public:
    NumericColorBar(Allocator& allocator, LinearColor color) noexcept;

    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    LinearColor GetBarColor() const noexcept;

private:
    LinearColor color_;
};

} // namespace rocks::ui

#pragma once

#include <UiCore/Element.h>
#include <UiCore/Styles/MenuStyle.h>

namespace rocks::ui {

// 메뉴 구분선이다(UE "Menu.Separator" — White25 1 px, 여백 (12, 6)). 초점을 받지 않는다.
class ROCKS_UIFRAMEWORK_API MenuSeparator final : public Element {
    ROCKS_UI_ELEMENT(MenuSeparator, Element)

public:
    MenuSeparator(Allocator& allocator, const MenuStyle& style) noexcept;

    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    MenuStyle style_;
};

} // namespace rocks::ui

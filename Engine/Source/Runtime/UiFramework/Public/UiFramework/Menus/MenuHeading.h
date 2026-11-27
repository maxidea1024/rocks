#pragma once

#include <UiCore/Styles/MenuStyle.h>
#include <UiFramework/Text/TextLabel.h>

namespace rocks::ui {

// 메뉴 묶음 제목이다(UE "Menu.Heading" — Bold 7·자간 250·White25, 여백 (12, 6)). 글자는 호출자가
// style.heading.font로 shaping한 레이블이다. 초점을 받지 않는다.
class ROCKS_UIFRAMEWORK_API MenuHeading final : public Element {
    ROCKS_UI_ELEMENT(MenuHeading, Element)

public:
    MenuHeading(Allocator& allocator, const MenuStyle& style) noexcept;
    Result<void> SetLabel(const SharedPtr<TextLabel>& label) noexcept;

    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    MenuStyle style_;
    SharedPtr<TextLabel> label_;
};

} // namespace rocks::ui

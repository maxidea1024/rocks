#pragma once

#include <UiCore/Element.h>
#include <UiCore/Styles/CoreStyles.h>

namespace rocks::ui {

// 제목 막대 왼쪽의 앱 아이콘이다(UE SAppIconWidget — "AppIcon.Small"). 창 영역 질의에
// SystemMenu를 돌려줘 누르면 OS 시스템 메뉴가 열린다(Docs/Design/UiToolkit 9.1절 UW7b). 아이콘은
// 전경색으로 tint한 벡터 이미지다.
class ROCKS_UIFRAMEWORK_API WindowAppIcon final : public Element {
    ROCKS_UI_ELEMENT(WindowAppIcon, Element)

public:
    explicit WindowAppIcon(Allocator& allocator,
                           const WindowStyle& style = DefaultWindowStyle()) noexcept;

    input::WindowRegion GetWindowRegion() const noexcept override;
    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    WindowStyle style_;
};

} // namespace rocks::ui

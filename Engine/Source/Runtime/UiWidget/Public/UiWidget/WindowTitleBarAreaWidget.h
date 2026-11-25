#pragma once
#include <UiWidget/ContentWidget.h>
namespace rocks::ui {
// 게임 창의 사용자 정의 제목 표시줄이다. AddChild로 로고나 버튼이 있는 내용 한 개를 넣는다.
// 창 호스트는 Window::SetRegionHitTest에 Application::QueryWindowRegion을 연결해야 한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API WindowTitleBarAreaWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit WindowTitleBarAreaWidget(Allocator& allocator) noexcept;
    void SetDoubleClickTogglesFullscreen(bool enabled) noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "doubleClickTogglesFullscreen") bool fullscreen_ = false;
};
} // namespace rocks::ui

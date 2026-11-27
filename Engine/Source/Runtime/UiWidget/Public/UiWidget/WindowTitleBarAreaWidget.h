#pragma once
#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 게임 창의 사용자 정의 제목 표시줄이다. AddChild로 로고나 버튼이 있는 내용 한 개를 넣는다.
// 창 호스트는 그리기마다 Application::BuildWindowRegionMap으로 만든 영역 지도를
// Window::PublishRegionMap으로 게시해야 한다(Docs/Design/UiToolkit 19.2절).
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

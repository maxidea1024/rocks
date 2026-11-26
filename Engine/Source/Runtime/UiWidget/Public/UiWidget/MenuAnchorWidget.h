#pragma once

#include <UiWidget/ContentWidget.h>
#include <UiFramework/Popups/MenuAnchor.h>

namespace rocks::ui {

// 하나의 자식은 메뉴 버튼의 표면이다. 메뉴 생성기는 열 때마다 호출되며, 메뉴는 화면의
// 전면 Overlay에 배치된다. 생성 중인 위젯을 직접 캡처할 때는 소유자의 수명을 보장한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API MenuAnchorWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    ~MenuAnchorWidget() noexcept;
    explicit MenuAnchorWidget(Allocator& allocator) noexcept;
    void SetMenuFactory(MenuAnchor::MenuFactory&& factory) noexcept;
    Result<void> Open() noexcept;
    Result<void> OpenFocused(Application& app, uint32 user = 0) noexcept;
    Result<void> ToggleOpen() noexcept;
    void SetPlacement(MenuPlacement placement) noexcept;
    void SetFitInWindow(bool fit) noexcept;
    Optional<Vec2> MenuPosition() const noexcept;
    void SetOnOpenChanged(Delegate<void(bool)>&& callback) noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "placement") uint8 placement_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "fitInWindow") bool fitInWindow_ = true;
    ROCKS_NATIVE() Delegate<void(bool)> openChanged_;
    ROCKS_NATIVE() MenuAnchor::MenuFactory factory_;
};

} // namespace rocks::ui

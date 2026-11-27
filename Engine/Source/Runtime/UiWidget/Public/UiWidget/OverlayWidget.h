#pragma once

#include <UiWidget/PanelWidget.h>

namespace rocks::ui {

// 자식을 같은 영역에 겹친다. HUD 배경처럼 먼저 그릴 자식부터 추가한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API OverlayWidget final : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit OverlayWidget(Allocator& allocator) noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
};

} // namespace rocks::ui

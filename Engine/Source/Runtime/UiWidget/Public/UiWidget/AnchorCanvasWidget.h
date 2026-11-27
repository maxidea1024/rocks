#pragma once

#include <UiWidget/PanelWidget.h>
#include <UiFramework/Layout/AnchorCanvas.h>

namespace rocks::ui {

// 앵커와 오프셋으로 HUD를 배치한다. 늘어나는 축의 끝 오프셋은 반대쪽 여백이다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API AnchorCanvasWidget final : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit AnchorCanvasWidget(Allocator& allocator) noexcept;

    // 같은 WidgetTree에 만든 자식을 지정한 위치에 붙인다. 성공하면 해당 자식의 슬롯 핸들을
    // 반환한다.
    Result<ObjectHandle> AddAt(ObjectRegistry& objects, ObjectHandle child,
                               const CanvasSlot& layout) noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                             const SharedPtr<Element>& child) noexcept override;
};

} // namespace rocks::ui

#pragma once
#include <UiWidget/PanelWidget.h>
#include <UiCore/StackLayout.h>

namespace rocks::ui {

// 가로·세로·가변 방향 패널이 같은 슬롯 설정을 애셋과 화면 트리에 적용한다.
ROCKS_CLASS(Abstract)
class ROCKS_UIWIDGET_API StackPanelWidget : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit StackPanelWidget(Allocator& allocator) noexcept;
    // Fill은 남은 공간을 weight 비율로 배분하고, Auto는 자식이 요청한 크기를 배정한다.
    Result<ObjectHandle> AddAt(ObjectRegistry& objects, ObjectHandle child,
                               const StackSlot& layout) noexcept;

protected:
    Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle id, Element& panel,
                             const SharedPtr<Element>& child) noexcept override;
};
} // namespace rocks::ui

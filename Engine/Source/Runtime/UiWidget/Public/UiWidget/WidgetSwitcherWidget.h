#pragma once

#include <UiWidget/PanelWidget.h>

namespace rocks::ui {

// 페이지를 모두 보관하고 현재 페이지 하나만 측정하고 그린다. 인덱스는 자식을 추가한 순서로 센다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API WidgetSwitcherWidget final : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit WidgetSwitcherWidget(Allocator& allocator) noexcept;
    Result<void> SetActiveIndex(uint32 index) noexcept;
    uint32 ActiveIndex() const noexcept;
    // 외부 탭·게임패드 탐색 UI도 동일한 페이지 전환을 관찰할 수 있다.
    MulticastDelegate<void(uint32)>& OnActivePageChanged() noexcept;
    Result<void> RemoveChild(ObjectRegistry& objects, ObjectHandle child) noexcept override;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_NATIVE() MulticastDelegate<void(uint32)> changed_;
    ROCKS_PROPERTY(ReadOnly, Name = "activeIndex") uint32 activeIndex_ = 0;
};

} // namespace rocks::ui

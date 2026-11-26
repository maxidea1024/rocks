#pragma once

#include <UiWidget/PanelWidget.h>
#include <UiFramework/Layout/WrapStack.h>

namespace rocks::ui {

// 가용 너비를 다 쓰면 다음 줄로 넘어간다. SetSpacing으로 열과 줄 사이 간격을 조정한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API WrapBoxWidget final : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit WrapBoxWidget(Allocator& allocator) noexcept;
    // 자식별로 새 줄 시작, 좁은 화면에서 한 줄 차지, 여백과 정렬을 지정한다.
    // AddChild는 기본 슬롯으로 붙이며 두 함수 모두 성공 시 슬롯 핸들을 반환한다.
    Result<ObjectHandle> AddAt(ObjectRegistry& objects, ObjectHandle child,
                               const WrapSlot& layout) noexcept;
    Result<void> SetSpacing(Vec2 value) noexcept;
    Vec2 Spacing() const noexcept;

protected:
    Result<void> UpdateAlignedSlot(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                                   Element& child, const SlotPlacement& layout) noexcept override;
    TypeId SlotType() const noexcept override;
    Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                             const SharedPtr<Element>& child) noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "spacing") Vec2 spacing_ {};
};

} // namespace rocks::ui

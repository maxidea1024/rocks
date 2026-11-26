#include <UiWidget/StackPanelWidget.h>
#include <UiWidget/StackWidgetSlot.generated.h>
#include <UiFramework/Layout/StackPanel.h>

namespace rocks::ui {

StackPanelWidget::StackPanelWidget(Allocator& allocator) noexcept : PanelWidget(allocator) {
}

Result<ObjectHandle> StackPanelWidget::AddAt(ObjectRegistry& objects, ObjectHandle child,
                                             const StackSlot& layout) noexcept {
    return AddChildWithSlot(
        objects, child,
        ConfigureSlot::CreateLambda([&layout](ObjectRegistry& registry, ObjectHandle id) noexcept {
            auto slot = registry.ResolveForWrite<StackWidgetSlot>(registry.OwnerAccess(), id);
            return slot ? slot.Value().Get().Value()->Configure(layout)
                        : Result<void>::Failure(slot.Error());
        }));
}

Result<void> StackPanelWidget::AttachChild(ObjectRegistry& objects, ObjectHandle id, Element& panel,
                                           const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<StackWidgetSlot>(objects.OwnerAccess(), id);
    if (!slot) {
        // 전용 슬롯 도입 전 저장된 트리는 기존 Auto 배치를 복원한다.
        return slot.Error() == ErrorCode::TypeMismatch
                   ? PanelWidget::AttachChild(objects, id, panel, child)
                   : Result<void>::Failure(slot.Error());
    }

    return static_cast<StackPanel&>(panel).TryAddSlot(child, slot.Value().Get().Value()->Layout());
}
} // namespace rocks::ui

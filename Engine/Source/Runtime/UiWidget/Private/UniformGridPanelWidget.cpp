#include "MakeWidgetElement.h"
#include <UiFramework/Layout/UniformGrid.h>
#include <UiWidget/UniformGridPanelWidget.h>
#include <UiWidget/UniformGridWidgetSlot.generated.h>

namespace rocks::ui {

UniformGridPanelWidget::UniformGridPanelWidget(Allocator& allocator) noexcept
    : PanelWidget(allocator) {
}

Result<void> UniformGridPanelWidget::SetCellPadding(Vec4 value) noexcept {
    if (!SlotPlacement{value}.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto applied = static_cast<UniformGrid&>(*live).SetCellPadding(value);
        if (!applied) {
            return applied;
        }
    }

    cellPadding_ = value;
    return Result<void>::Success();
}

Result<void> UniformGridPanelWidget::SetMinimumCellSize(Vec2 value) noexcept {
    if (!value.IsFinite() || value.x < 0 || value.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto applied = static_cast<UniformGrid&>(*live).SetMinimumCellSize(value);
        if (!applied) {
            return applied;
        }
    }

    minimumCellSize_ = value;
    return Result<void>::Success();
}

Vec4 UniformGridPanelWidget::CellPadding() const noexcept {
    return cellPadding_;
}

Vec2 UniformGridPanelWidget::MinimumCellSize() const noexcept {
    return minimumCellSize_;
}

TypeId UniformGridPanelWidget::SlotType() const noexcept {
    return GeneratedType<UniformGridWidgetSlot>::Id();
}

Result<SharedPtr<Element>> UniformGridPanelWidget::CreatePanel(Allocator& allocator) noexcept {
    auto made = MakeWidgetElement<UniformGrid>(allocator);
    if (!made) {
        return made;
    }

    auto& grid = static_cast<UniformGrid&>(*made.Value());
    auto padding = grid.SetCellPadding(cellPadding_);
    if (!padding) {
        return Result<SharedPtr<Element>>::Failure(padding.Error());
    }

    auto size = grid.SetMinimumCellSize(minimumCellSize_);
    if (!size) {
        return Result<SharedPtr<Element>>::Failure(size.Error());
    }

    return made;
}

Result<ObjectHandle> UniformGridPanelWidget::AddAt(ObjectRegistry& objects, ObjectHandle child,
                                                   const IntVec2& layout, StackAlignment horizontal,
                                                   StackAlignment vertical) noexcept {
    return AddChildWithSlot(
        objects, child,
        ConfigureSlot::CreateLambda([&layout, horizontal, vertical](ObjectRegistry& registry,
                                                                    ObjectHandle handle) noexcept {
            auto slot =
                registry.ResolveForWrite<UniformGridWidgetSlot>(registry.OwnerAccess(), handle);
            return slot ? slot.Value().Get().Value()->Configure(layout, horizontal, vertical)
                        : Result<void>::Failure(slot.Error());
        }));
}

Result<void> UniformGridPanelWidget::AttachChild(ObjectRegistry& objects, ObjectHandle handle,
                                                 Element& panel,
                                                 const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<UniformGridWidgetSlot>(objects.OwnerAccess(), handle);
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    const auto* data = slot.Value().Get().Value();
    const auto layout = data->Layout();
    return static_cast<UniformGrid&>(panel).TryAddSlot(
        child, static_cast<uint32>(layout.y), static_cast<uint32>(layout.x),
        data->HorizontalAlignment(), data->VerticalAlignment());
}

} // namespace rocks::ui

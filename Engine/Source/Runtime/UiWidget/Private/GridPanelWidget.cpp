#include "MakeWidgetElement.h"
#include <UiWidget/GridPanelWidget.h>
#include <UiWidget/GridWidgetSlot.generated.h>

namespace rocks::ui {

GridPanelWidget::GridPanelWidget(Allocator& allocator) noexcept
    : PanelWidget(allocator), columnFill_(allocator), rowFill_(allocator) {
}

float GridPanelWidget::ColumnFill(uint32 column) const noexcept {
    return column < columnFill_.Size() ? columnFill_[column] : 0;
}

float GridPanelWidget::RowFill(uint32 row) const noexcept {
    return row < rowFill_.Size() ? rowFill_[row] : 0;
}

Result<void> GridPanelWidget::SetFill(Array<float>& weights, uint32 index, float value,
                                      bool column) noexcept {
    if (!math::IsFinite(value) || value < 0 || index == NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize previous = weights.Size();
    if ((index < previous ? weights[index] : 0) == value) {
        return Result<void>::Success();
    }

    if (index >= previous) {
        auto grown = weights.TryResize(static_cast<usize>(index) + 1);
        if (!grown) {
            return grown;
        }
    }

    if (auto live = CachedElement()) {
        auto& grid = static_cast<Grid&>(*live);
        auto applied = column ? grid.SetColumnFill(index, value) : grid.SetRowFill(index, value);
        if (!applied) {
            (void)weights.TryResize(previous);
            return applied;
        }
    }

    weights[index] = value;
    return Result<void>::Success();
}

Result<void> GridPanelWidget::SetColumnFill(uint32 column, float weight) noexcept {
    return SetFill(columnFill_, column, weight, true);
}

Result<void> GridPanelWidget::SetRowFill(uint32 row, float weight) noexcept {
    return SetFill(rowFill_, row, weight, false);
}

void GridPanelWidget::ClearFill() noexcept {
    columnFill_.Clear();
    rowFill_.Clear();
    if (auto live = CachedElement()) {
        static_cast<Grid&>(*live).ClearFill();
    }
}

TypeId GridPanelWidget::SlotType() const noexcept {
    return GeneratedType<GridWidgetSlot>::Id();
}

Result<SharedPtr<Element>> GridPanelWidget::CreatePanel(Allocator& allocator) noexcept {
    auto made = MakeWidgetElement<Grid>(allocator);
    if (!made) {
        return made;
    }

    auto& grid = static_cast<Grid&>(*made.Value());
    for (usize i = 0; i < columnFill_.Size(); ++i) {
        auto applied = grid.SetColumnFill(static_cast<uint32>(i), columnFill_[i]);
        if (!applied) {
            return Result<SharedPtr<Element>>::Failure(applied.Error());
        }
    }

    for (usize i = 0; i < rowFill_.Size(); ++i) {
        auto applied = grid.SetRowFill(static_cast<uint32>(i), rowFill_[i]);
        if (!applied) {
            return Result<SharedPtr<Element>>::Failure(applied.Error());
        }
    }

    return made;
}

Result<ObjectHandle> GridPanelWidget::AddAt(ObjectRegistry& objects, ObjectHandle child,
                                            const GridSlot& layout) noexcept {
    return AddChildWithSlot(objects, child,
                            ConfigureSlot::CreateLambda(
                                [&layout](ObjectRegistry& registry, ObjectHandle handle) noexcept {
                                    auto slot = registry.ResolveForWrite<GridWidgetSlot>(
                                        registry.OwnerAccess(), handle);
                                    return slot ? slot.Value().Get().Value()->Configure(layout)
                                                : Result<void>::Failure(slot.Error());
                                }));
}

Result<void> GridPanelWidget::AttachChild(ObjectRegistry& objects, ObjectHandle handle,
                                          Element& panel,
                                          const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<GridWidgetSlot>(objects.OwnerAccess(), handle);
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    const auto layout = slot.Value().Get().Value()->Layout();
    return static_cast<Grid&>(panel).TryAddSlot(child, layout);
}

} // namespace rocks::ui

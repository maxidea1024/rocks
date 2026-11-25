#include <UiWidget/GridWidgetSlot.h>

#include "UpdateLiveWidgetSlot.h"
#include <UiWidget/GridPanelWidget.generated.h>
#include <UiFramework/Grid.h>

namespace rocks::ui {

GridSlot GridWidgetSlot::Layout() const noexcept {
    return {column_,
            row_,
            columnSpan_,
            rowSpan_,
            layer_,
            padding_,
            static_cast<StackAlignment>(horizontal_),
            static_cast<StackAlignment>(vertical_),
            nudge_};
}

Result<void> GridWidgetSlot::UpdateLayout(ObjectRegistry& objects,
                                          const GridSlot& layout) noexcept {
    auto applied = UpdateLiveWidgetSlot<GridPanelWidget, Grid>(
        objects, *this,
        [&layout](Grid& panel, Element& child) noexcept { return panel.SetSlot(child, layout); });
    if (!applied) {
        return applied;
    }
    return Configure(layout);
}

Result<void> GridWidgetSlot::Configure(const GridSlot& layout) noexcept {
    const SlotPlacement placement{layout.padding, layout.horizontal, layout.vertical};
    if (!layout.rowSpan || !layout.columnSpan || !placement.IsValid() || !layout.nudge.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    column_ = layout.column;
    row_ = layout.row;
    columnSpan_ = layout.columnSpan;
    rowSpan_ = layout.rowSpan;
    layer_ = layout.layer;
    padding_ = layout.padding;
    horizontal_ = static_cast<uint8>(layout.horizontal);
    vertical_ = static_cast<uint8>(layout.vertical);
    nudge_ = layout.nudge;
    return Result<void>::Success();
}

Result<void> GridWidgetSlot::SetRow(ObjectRegistry& objects, uint32 value) noexcept {
    auto layout = Layout();
    layout.row = value;
    return UpdateLayout(objects, layout);
}

Result<void> GridWidgetSlot::SetColumn(ObjectRegistry& objects, uint32 value) noexcept {
    auto layout = Layout();
    layout.column = value;
    return UpdateLayout(objects, layout);
}

Result<void> GridWidgetSlot::SetRowSpan(ObjectRegistry& objects, uint32 value) noexcept {
    auto layout = Layout();
    layout.rowSpan = value;
    return UpdateLayout(objects, layout);
}

Result<void> GridWidgetSlot::SetColumnSpan(ObjectRegistry& objects, uint32 value) noexcept {
    auto layout = Layout();
    layout.columnSpan = value;
    return UpdateLayout(objects, layout);
}

Result<void> GridWidgetSlot::SetLayer(ObjectRegistry& objects, int32 value) noexcept {
    auto layout = Layout();
    layout.layer = value;
    return UpdateLayout(objects, layout);
}

Result<void> GridWidgetSlot::SetNudge(ObjectRegistry& objects, Vec2 value) noexcept {
    auto layout = Layout();
    layout.nudge = value;
    return UpdateLayout(objects, layout);
}

Result<void> GridWidgetSlot::SetPadding(ObjectRegistry& objects, Vec4 value) noexcept {
    auto layout = Layout();
    layout.padding = value;
    return UpdateLayout(objects, layout);
}

Result<void> GridWidgetSlot::SetHorizontalAlignment(ObjectRegistry& objects,
                                                    StackAlignment value) noexcept {
    auto layout = Layout();
    layout.horizontal = value;
    return UpdateLayout(objects, layout);
}

Result<void> GridWidgetSlot::SetVerticalAlignment(ObjectRegistry& objects,
                                                  StackAlignment value) noexcept {
    auto layout = Layout();
    layout.vertical = value;
    return UpdateLayout(objects, layout);
}

} // namespace rocks::ui

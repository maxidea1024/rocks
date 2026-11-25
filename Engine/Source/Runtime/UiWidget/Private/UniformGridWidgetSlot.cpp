#include <UiWidget/UniformGridWidgetSlot.h>
#include <UiFramework/SlotPresenter.h>

#include "UpdateLiveWidgetSlot.h"
#include <UiWidget/UniformGridPanelWidget.generated.h>
#include <UiFramework/UniformGrid.h>

namespace rocks::ui {

IntVec2 UniformGridWidgetSlot::Layout() const noexcept {
    return {x_, y_};
}

StackAlignment UniformGridWidgetSlot::HorizontalAlignment() const noexcept {
    return static_cast<StackAlignment>(horizontal_);
}

StackAlignment UniformGridWidgetSlot::VerticalAlignment() const noexcept {
    return static_cast<StackAlignment>(vertical_);
}

Result<void> UniformGridWidgetSlot::UpdateLayout(ObjectRegistry& objects, const IntVec2& layout,
                                                 StackAlignment horizontal,
                                                 StackAlignment vertical) noexcept {
    if (layout.x < 0 || layout.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto applied = UpdateLiveWidgetSlot<UniformGridPanelWidget, UniformGrid>(
        objects, *this,
        [&layout, horizontal, vertical](UniformGrid& panel, Element& child) noexcept {
            return panel.SetSlot(child, static_cast<uint32>(layout.y),
                                 static_cast<uint32>(layout.x), horizontal, vertical);
        });
    if (!applied) {
        return applied;
    }

    return Configure(layout, horizontal, vertical);
}

Result<void> UniformGridWidgetSlot::Configure(const IntVec2& layout, StackAlignment horizontal,
                                              StackAlignment vertical) noexcept {
    const SlotPlacement placement{{}, horizontal, vertical};
    if (layout.x < 0 || layout.y < 0 || !placement.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    x_ = layout.x;
    y_ = layout.y;
    horizontal_ = static_cast<uint8>(horizontal);
    vertical_ = static_cast<uint8>(vertical);
    return Result<void>::Success();
}

Result<void> UniformGridWidgetSlot::SetColumn(ObjectRegistry& objects, int32 value) noexcept {
    return UpdateLayout(objects, {value, y_}, HorizontalAlignment(), VerticalAlignment());
}

Result<void> UniformGridWidgetSlot::SetRow(ObjectRegistry& objects, int32 value) noexcept {
    return UpdateLayout(objects, {x_, value}, HorizontalAlignment(), VerticalAlignment());
}

Result<void> UniformGridWidgetSlot::SetHorizontalAlignment(ObjectRegistry& objects,
                                                           StackAlignment value) noexcept {
    return UpdateLayout(objects, Layout(), value, VerticalAlignment());
}

Result<void> UniformGridWidgetSlot::SetVerticalAlignment(ObjectRegistry& objects,
                                                         StackAlignment value) noexcept {
    return UpdateLayout(objects, Layout(), HorizontalAlignment(), value);
}

} // namespace rocks::ui

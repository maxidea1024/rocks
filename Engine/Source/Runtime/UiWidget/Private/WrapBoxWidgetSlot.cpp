#include <UiWidget/WrapBoxWidgetSlot.h>

#include "UpdateLiveWidgetSlot.h"
#include <UiWidget/WrapBoxWidget.generated.h>
#include <UiFramework/Layout/WrapStack.h>

namespace rocks::ui {

WrapSlot WrapBoxWidgetSlot::WrapLayout() const noexcept {
    return {forceNewLine_, fillEmptySpace_, fillLineWhenSizeLessThan_, Layout()};
}

Result<void> WrapBoxWidgetSlot::UpdateLayout(ObjectRegistry& objects,
                                             const WrapSlot& layout) noexcept {
    auto applied = UpdateLiveWidgetSlot<WrapBoxWidget, WrapStack>(
        objects, *this, [&layout](WrapStack& panel, Element& child) noexcept {
            return panel.SetSlot(child, layout);
        });
    if (!applied) {
        return applied;
    }

    return Configure(layout);
}

Result<void> WrapBoxWidgetSlot::Configure(const WrapSlot& layout) noexcept {
    if (!layout.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto placed = SetLayout(layout.placement);
    if (!placed) {
        return placed;
    }

    forceNewLine_ = layout.forceNewLine;
    fillEmptySpace_ = layout.fillEmptySpace;
    fillLineWhenSizeLessThan_ = layout.fillLineWhenSizeLessThan;
    return Result<void>::Success();
}

Result<void> WrapBoxWidgetSlot::SetNewLine(ObjectRegistry& objects, bool value) noexcept {
    auto layout = WrapLayout();
    layout.forceNewLine = value;
    return UpdateLayout(objects, layout);
}

Result<void> WrapBoxWidgetSlot::SetFillEmptySpace(ObjectRegistry& objects, bool value) noexcept {
    auto layout = WrapLayout();
    layout.fillEmptySpace = value;
    return UpdateLayout(objects, layout);
}

Result<void> WrapBoxWidgetSlot::SetFillSpanWhenLessThan(ObjectRegistry& objects,
                                                        float value) noexcept {
    auto layout = WrapLayout();
    layout.fillLineWhenSizeLessThan = value;
    return UpdateLayout(objects, layout);
}

} // namespace rocks::ui

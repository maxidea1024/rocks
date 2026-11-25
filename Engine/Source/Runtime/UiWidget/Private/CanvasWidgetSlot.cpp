#include "UpdateLiveWidgetSlot.h"
#include <UiWidget/CanvasPanelWidget.generated.h>
#include <UiWidget/CanvasWidgetSlot.h>

namespace rocks::ui {

CanvasSlot CanvasWidgetSlot::Layout() const noexcept {
    return {anchorMin_, anchorMax_, offset_, alignment_, zOrder_, autoSize_};
}

Vec2 CanvasWidgetSlot::Position() const noexcept {
    return {offset_.x, offset_.y};
}

Vec2 CanvasWidgetSlot::Size() const noexcept {
    return {offset_.z, offset_.w};
}

Vec4 CanvasWidgetSlot::Offsets() const noexcept {
    return offset_;
}

Vec2 CanvasWidgetSlot::Minimum() const noexcept {
    return anchorMin_;
}

Vec2 CanvasWidgetSlot::Maximum() const noexcept {
    return anchorMax_;
}

Vec2 CanvasWidgetSlot::Alignment() const noexcept {
    return alignment_;
}

float CanvasWidgetSlot::ZOrder() const noexcept {
    return zOrder_;
}

bool CanvasWidgetSlot::AutoSize() const noexcept {
    return autoSize_;
}

Result<void> CanvasWidgetSlot::UpdateLayout(ObjectRegistry& objects,
                                            const CanvasSlot& layout) noexcept {
    auto live = UpdateLiveWidgetSlot<CanvasPanelWidget, AnchorCanvas>(
        objects, *this, [&layout](AnchorCanvas& canvas, Element& child) noexcept {
            return canvas.SetSlot(child, layout);
        });
    if (!live) {
        return live;
    }

    return Configure(layout);
}

Result<void> CanvasWidgetSlot::SetPosition(ObjectRegistry& objects, Vec2 position) noexcept {
    auto layout = Layout();
    layout.offset.x = position.x;
    layout.offset.y = position.y;
    return UpdateLayout(objects, layout);
}

Result<void> CanvasWidgetSlot::SetSize(ObjectRegistry& objects, Vec2 size) noexcept {
    auto layout = Layout();
    layout.offset.z = size.x;
    layout.offset.w = size.y;
    return UpdateLayout(objects, layout);
}

Result<void> CanvasWidgetSlot::SetOffsets(ObjectRegistry& objects, Vec4 offsets) noexcept {
    auto layout = Layout();
    layout.offset = offsets;
    return UpdateLayout(objects, layout);
}

Result<void> CanvasWidgetSlot::SetAnchors(ObjectRegistry& objects, Vec2 minimum,
                                          Vec2 maximum) noexcept {
    auto layout = Layout();
    layout.anchorMin = minimum;
    layout.anchorMax = maximum;
    return UpdateLayout(objects, layout);
}

Result<void> CanvasWidgetSlot::SetMinimum(ObjectRegistry& objects, Vec2 minimum) noexcept {
    auto layout = Layout();
    layout.anchorMin = minimum;
    return UpdateLayout(objects, layout);
}

Result<void> CanvasWidgetSlot::SetMaximum(ObjectRegistry& objects, Vec2 maximum) noexcept {
    auto layout = Layout();
    layout.anchorMax = maximum;
    return UpdateLayout(objects, layout);
}

Result<void> CanvasWidgetSlot::SetAlignment(ObjectRegistry& objects, Vec2 alignment) noexcept {
    auto layout = Layout();
    layout.alignment = alignment;
    return UpdateLayout(objects, layout);
}

Result<void> CanvasWidgetSlot::SetAutoSize(ObjectRegistry& objects, bool enabled) noexcept {
    auto layout = Layout();
    layout.autoSize = enabled;
    return UpdateLayout(objects, layout);
}

Result<void> CanvasWidgetSlot::SetZOrder(ObjectRegistry& objects, float order) noexcept {
    auto layout = Layout();
    layout.zOrder = order;
    return UpdateLayout(objects, layout);
}

Result<void> CanvasWidgetSlot::Configure(const CanvasSlot& layout) noexcept {
    if (!layout.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    anchorMin_ = layout.anchorMin;
    anchorMax_ = layout.anchorMax;
    offset_ = layout.offset;
    alignment_ = layout.alignment;
    zOrder_ = layout.zOrder;
    autoSize_ = layout.autoSize;
    return Result<void>::Success();
}

} // namespace rocks::ui

#include "MenuPopupLayer.h"

namespace rocks::ui {

MenuPopupLayer::MenuPopupLayer(Allocator& allocator, MenuAnchor& anchor) noexcept
    : Element(allocator), anchor_(&anchor) {
    SetClipToBounds(true);
}

void MenuPopupLayer::Disconnect() noexcept {
    anchor_ = nullptr;
}

Result<void> MenuPopupLayer::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    auto* menu = ChildAt(0);
    if (!anchor_ || !menu) {
        return Result<void>::Success();
    }
    const auto& anchor = anchor_->GetGeometry();
    AffineTransform2D inverse;
    if (!geometry.render.TryInverse(inverse)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const auto local = inverse.Compose(anchor.render);
    auto bounds = Geometry::Root(anchor.size, local).WindowBounds();
    if (!bounds) {
        return Result<void>::Failure(bounds.Error());
    }
    const auto rect = bounds.Value();
    const Vec2 desired = menu->GetDesiredSize();
    const auto placement = anchor_->placement_;
    const bool fit = anchor_->fit_;
    const bool combo =
        placement == MenuPlacement::ComboBox || placement == MenuPlacement::ComboBoxRight;
    float width = combo ? math::Max(desired.x, rect.max.x - rect.min.x) : desired.x;
    float height = desired.y;
    if (fit) {
        width = math::Min(width, geometry.size.x);
        height = math::Min(height, geometry.size.y);
    }
    const bool side = placement == MenuPlacement::Left || placement == MenuPlacement::Right ||
                      placement == MenuPlacement::RightCenter;
    float x = rect.min.x;
    float y = rect.max.y;
    if (side) {
        const float left = math::Clamp(rect.min.x, 0.f, geometry.size.x);
        const float right = geometry.size.x - math::Clamp(rect.max.x, 0.f, geometry.size.x);
        bool useLeft = placement == MenuPlacement::Left;
        if (fit && width > (useLeft ? left : right) && (useLeft ? right > left : left > right)) {
            useLeft = !useLeft;
        }
        if (fit) {
            width = math::Min(width, useLeft ? left : right);
        }
        x = useLeft ? rect.min.x - width : rect.max.x;
        y = placement == MenuPlacement::RightCenter ? (rect.min.y + rect.max.y - height) * 0.5f
                                                    : rect.min.y;
    } else if (placement == MenuPlacement::Center || placement == MenuPlacement::MatchBottomLeft) {
        if (placement == MenuPlacement::Center) {
            x = (rect.min.x + rect.max.x - width) * 0.5f;
            y = (rect.min.y + rect.max.y - height) * 0.5f;
        } else {
            y = rect.max.y - height;
        }
    } else {
        const float above = math::Clamp(rect.min.y, 0.f, geometry.size.y);
        const float below = geometry.size.y - math::Clamp(rect.max.y, 0.f, geometry.size.y);
        bool useAbove = placement == MenuPlacement::Above ||
                        placement == MenuPlacement::AboveRight ||
                        placement == MenuPlacement::CenteredAbove;
        if (fit && height > (useAbove ? above : below) &&
            (useAbove ? below > above : above > below)) {
            useAbove = !useAbove;
        }
        if (fit) {
            height = math::Min(height, useAbove ? above : below);
        }
        y = useAbove ? rect.min.y - height : rect.max.y;
        if (placement == MenuPlacement::CenteredBelow ||
            placement == MenuPlacement::CenteredAbove) {
            x = (rect.min.x + rect.max.x - width) * 0.5f;
        } else if (placement == MenuPlacement::BelowRight ||
                   placement == MenuPlacement::AboveRight ||
                   (placement == MenuPlacement::ComboBoxRight && width > rect.max.x - rect.min.x)) {
            x = rect.max.x - width;
        }
    }
    if (fit) {
        x = math::Clamp(x, 0.f, geometry.size.x - width);
        y = math::Clamp(y, 0.f, geometry.size.y - height);
    }
    return menu->ArrangeIfNeeded(geometry.MakeChild({width, height}, {1, 0, 0, 1, x, y}, {}, {}));
}

PointerReply MenuPopupLayer::OnPreviewPointerDown(const PointerEvent& event) noexcept {
    auto* menu = ChildAt(0);
    if (anchor_ && (!menu || !menu->GetGeometry().Contains(event.position))) {
        anchor_->Close();
        return {.handled = true};
    }
    return {};
}

bool MenuPopupLayer::OnPreviewKeyDown(uint32, const input::InputEvent& event) noexcept {
    if (anchor_ && event.key == input::Key::Escape) {
        anchor_->Close();
        return true;
    }
    return false;
}

} // namespace rocks::ui

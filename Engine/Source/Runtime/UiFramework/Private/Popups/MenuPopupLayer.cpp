#include "MenuPopupLayer.h"

namespace rocks::ui {

MenuPopupLayer::MenuPopupLayer(Allocator& allocator, MenuAnchor& anchor) noexcept
    : Element(allocator), anchor_(&anchor) {
    SetClipToBounds(true);
}

void MenuPopupLayer::SetPopupArea(const Optional<Box2<float>>& area) noexcept {
    const bool same = area.HasValue() == popupArea_.HasValue() &&
                      (!area || (area->min == popupArea_->min && area->max == popupArea_->max));
    if (!same) {
        popupArea_ = area;
        InvalidateArrangement();
    }
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
    // 배치 경계는 호스트가 알린 팝업 허용 영역(UW7d)이고, 없으면 층 전체다.
    const Vec2 areaMin = popupArea_ ? popupArea_->min : Vec2{};
    const Vec2 areaMax = popupArea_ ? popupArea_->max : geometry.size;
    const Vec2 areaSize{math::Max(0.0f, areaMax.x - areaMin.x),
                        math::Max(0.0f, areaMax.y - areaMin.y)};
    const Vec2 desired = menu->GetDesiredSize();
    const auto placement = anchor_->placement_;
    const bool fit = anchor_->fit_;
    const bool combo =
        placement == MenuPlacement::ComboBox || placement == MenuPlacement::ComboBoxRight;
    float width = combo ? math::Max(desired.x, rect.max.x - rect.min.x) : desired.x;
    float height = desired.y;
    if (fit) {
        width = math::Min(width, areaSize.x);
        height = math::Min(height, areaSize.y);
    }

    const bool side = placement == MenuPlacement::Left || placement == MenuPlacement::Right ||
                      placement == MenuPlacement::RightCenter;
    float x = rect.min.x;
    float y = rect.max.y;
    if (side) {
        const float left = math::Clamp(rect.min.x, areaMin.x, areaMax.x) - areaMin.x;
        const float right = areaMax.x - math::Clamp(rect.max.x, areaMin.x, areaMax.x);
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
        const float above = math::Clamp(rect.min.y, areaMin.y, areaMax.y) - areaMin.y;
        const float below = areaMax.y - math::Clamp(rect.max.y, areaMin.y, areaMax.y);
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

    x += anchor_->menuOffset_.x;
    y += anchor_->menuOffset_.y;
    if (fit) {
        x = math::Clamp(x, areaMin.x, math::Max(areaMin.x, areaMax.x - width));
        y = math::Clamp(y, areaMin.y, math::Max(areaMin.y, areaMax.y - height));
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

bool MenuPopupLayer::OnPreviewKeyChord(uint32, const input::InputEvent& event,
                                       KeyChord chord) noexcept {
    // Escape 또는 패드 뒤로(East)면 닫는다(20.8절).
    if (anchor_ && event.type == input::InputEventType::KeyDown &&
        (event.key == input::Key::Escape || chord.gamepad == input::GamepadButton::East)) {
        anchor_->Close();
        return true;
    }

    return false;
}

} // namespace rocks::ui

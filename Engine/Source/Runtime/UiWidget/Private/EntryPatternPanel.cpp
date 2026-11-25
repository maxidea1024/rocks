#include "EntryPatternPanel.h"

namespace rocks::ui {

EntryPatternPanel::EntryPatternPanel(Allocator& allocator, bool radial) noexcept
    : Element(allocator), radial_(radial), pattern_(allocator) {
    SetClipToBounds(true);
}

void EntryPatternPanel::SetSpacing(Vec2 spacing) noexcept {
    if (spacing_ != spacing) {
        spacing_ = spacing;
        InvalidateLayout();
    }
}

Result<void> EntryPatternPanel::SetPattern(Span<const Vec2> steps) noexcept {
    for (const auto& step : steps) {if (!step.IsFinite()) {return Result<void>::Failure(ErrorCode::InvalidArgument);}}
    Array<Vec2> next(pattern_.GetAllocator());
    auto copied = next.TryAppend(steps);
    if (!copied) {return copied;}
    pattern_ = Move(next);
    InvalidateLayout();
    return Result<void>::Success();
}
void EntryPatternPanel::SetRadialSettings(const RadialBoxSettings& settings) noexcept {
    settings_ = settings;
    InvalidateLayout();
}
bool EntryPatternPanel::StartsPaintLayer(usize, usize) const noexcept {
    return true;
}

Vec2 EntryPatternPanel::ComputeDesiredSize(float) const noexcept {
    Vec2 desired{};
    Vec2 offset{};
    usize count = 0;
    for (const auto& child : ActiveChildren()) {
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }
        const Vec2 size =
            child->GetDesiredSize() + (radial_ ? Vec2{} : Vec2{math::Abs(offset.x), math::Abs(offset.y)});
        desired.x = math::Max(desired.x, size.x);
        desired.y = math::Max(desired.y, size.y);
        offset += pattern_.IsEmpty() ? spacing_ : pattern_[count % pattern_.Size()];
        ++count;
    }
    if (radial_ && count > 1) {
        const float diameter =
            math::Max(desired.x, desired.y) *
            (1.0f + 1.0f / math::Sin(math::Pi<float> / static_cast<float>(count)));
        return {diameter + 2 * settings_.margin, diameter + 2 * settings_.margin};
    }
    return desired;
}

Result<void> EntryPatternPanel::Arrange(const Geometry& geometry) noexcept {
    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    SetGeometry(geometry);
    usize count = 0;
    if (radial_) {
        for (const auto& child : ActiveChildren()) {
            if (child->GetVisibility() != Visibility::Collapsed) {
                ++count;
            }
        }
    }
    const float step = settings_.distributeEvenly && count ? settings_.sectorAngle / static_cast<float>(count) : settings_.angleBetweenItems;
    const float start = -math::UnwindDegrees(settings_.startingAngle) - (settings_.distributeEvenly && settings_.sectorAngle < 360 ? step * 0.5f : 0);
    Vec2 offset{};
    usize index = 0;
    for (const auto& child : ActiveChildren()) {
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }
        const Vec2 size = child->GetDesiredSize();
        Vec2 pos = offset;
        if (radial_) {
            const float angle =
                math::DegreesToRadians(start + (settings_.clockwise ? step : -step) * static_cast<float>(index));
            const float radius = math::Max(
                0.0f, math::Min(geometry.size.x - size.x, geometry.size.y - size.y) * 0.5f - settings_.margin);
            pos = (geometry.size - size) * 0.5f + Vec2(math::Cos(angle), math::Sin(angle)) * radius;
        } else {
            if (pos.x < 0 || (pos.x == 0 && pattern_.IsEmpty() && spacing_.x < 0)) {
                pos.x += geometry.size.x - size.x;
            }
            if (pos.y < 0 || (pos.y == 0 && pattern_.IsEmpty() && spacing_.y < 0)) {
                pos.y += geometry.size.y - size.y;
            }
        }
        if (!pos.IsFinite()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        auto result = child->ArrangeIfNeeded(
            GetGeometry().MakeChild(size, {1, 0, 0, 1, pos.x, pos.y}, {}, {}));
        if (!result) {
            return result;
        }
        offset += pattern_.IsEmpty() ? spacing_ : pattern_[index % pattern_.Size()];
        ++index;
    }
    return Result<void>::Success();
}

} // namespace rocks::ui

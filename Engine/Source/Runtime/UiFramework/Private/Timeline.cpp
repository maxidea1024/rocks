#include <UiFramework/Timeline.h>

namespace rocks::ui {

namespace {

Result<void> Rectangle(PaintList& list, const Geometry& geometry, float x, float y, float width,
                       float height, LinearColor color, int32 layer) noexcept {
    if (width <= 0 || height <= 0) {
        return Result<void>::Success();
    }

    return list.TryAddBox(
        {geometry.MakeChild({width, height}, {1, 0, 0, 1, x, y}, {}, {}), color, layer});
}

} // namespace

class Timeline::Ruler final : public Element {
public:
    Ruler(Allocator& allocator, float minimum, float maximum) noexcept
        : Element(allocator), marks_(allocator), minimum_(minimum), maximum_(maximum) {
        SetVisibility(Visibility::HitTestInvisible);
    }

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override {
        const auto& geometry = GetGeometry();
        const auto divisions = major_ * minor_;
        const float start = ThumbExtent(geometry.size) * 0.5f;
        const float width = geometry.size.x - 2 * start;
        const float baseline = geometry.size.y * 0.62f;
        auto result = Rectangle(list, geometry, start, baseline, width, 1, color_, layer);
        for (uint32 i = 0; result && i <= divisions; ++i) {
            const float x = start + width * (static_cast<float>(i) / static_cast<float>(divisions));
            const float height = (i % minor_ == 0 ? 0.53f : 0.19f) * geometry.size.y;
            result = Rectangle(list, geometry, x, baseline - height, 1, height, color_, layer);
        }

        for (const auto& mark : marks_) {
            if (!result || mark.time < minimum_ || mark.time > maximum_) {
                continue;
            }

            result = Rectangle(list, geometry,
                               TrackPosition(mark.time, minimum_, maximum_, geometry.size) - 2,
                               baseline + 3, 4, 6, mark.color, layer);
        }

        return result;
    }

    Array<TimelineMark> marks_;
    float minimum_;
    float maximum_;
    LinearColor color_{0.4f, 0.4f, 0.4f, 1};
    uint32 major_ = 4;
    uint32 minor_ = 8;
};

Timeline::Timeline(Allocator& allocator) noexcept : Slider(allocator), allocator_(allocator) {
    SetDesiredSize({160, 38});
    SetClipToBounds(true);
}

Result<void> Timeline::EnsureRuler() noexcept {
    if (ruler_) {
        return Result<void>::Success();
    }

    auto ruler = SharedPtr<Ruler>::TryMake(allocator_, allocator_, Minimum(), Maximum());
    if (!ruler) {
        return Result<void>::Failure(ruler.Error());
    }

    auto added = Element::TryAddChild(ruler.Value());
    if (added) {
        ruler_ = ruler.Value().Get();
    }

    return added;
}

Result<void> Timeline::SetRulerColor(LinearColor color) noexcept {
    auto ready = EnsureRuler();
    if (ready && ruler_->color_ != color) {
        ruler_->color_ = color;
        ruler_->InvalidatePaint();
    }

    return ready;
}

Result<void> Timeline::SetMarks(Span<const TimelineMark> marks) noexcept {
    for (const auto& mark : marks) {
        if (!math::IsFinite(mark.time)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    Array<TimelineMark> staged(allocator_);
    auto copied = staged.TryAppend(marks);
    if (!copied) {
        return copied;
    }

    auto ready = EnsureRuler();
    if (!ready) {
        return ready;
    }

    ruler_->marks_ = Move(staged);
    ruler_->InvalidatePaint();
    return Result<void>::Success();
}

Result<void> Timeline::SetDivisions(uint32 major, uint32 minor) noexcept {
    if (!major || !minor || major > 4096 / minor) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto ready = EnsureRuler();
    if (!ready) {
        return ready;
    }

    if (ruler_->major_ != major || ruler_->minor_ != minor) {
        ruler_->major_ = major;
        ruler_->minor_ = minor;
        ruler_->InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> Timeline::SetRange(float minimum, float maximum) noexcept {
    const bool changed = Minimum() != minimum || Maximum() != maximum;
    auto result = Slider::SetRange(minimum, maximum);
    if (result && changed && ruler_) {
        ruler_->minimum_ = minimum;
        ruler_->maximum_ = maximum;
        ruler_->InvalidatePaint();
    }

    return result;
}

Result<void> Timeline::Arrange(const Geometry& geometry) noexcept {
    auto ready = EnsureRuler();
    if (!ready) {
        return ready;
    }

    SetGeometry(geometry);
    return ruler_->ArrangeIfNeeded(TrackGeometry());
}

Result<void> Timeline::OnPaint(PaintList& list, int32 layer) const noexcept {
    return Frame::OnPaint(list, layer);
}

Result<void> Timeline::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    const auto geometry = TrackGeometry();
    const float x = TrackPosition(GetValue(), Minimum(), Maximum(), geometry.size);
    auto drawn = Rectangle(list, geometry, x - 1, 2, 2, geometry.size.y - 2, FillColor(), layer);
    if (!drawn) {
        return drawn;
    }

    return Rectangle(list, geometry, x - 4, 0, 8, 5, FillColor(), layer);
}

Result<void> Timeline::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

bool Timeline::RemoveChild(Element&) noexcept {
    return false;
}

} // namespace rocks::ui

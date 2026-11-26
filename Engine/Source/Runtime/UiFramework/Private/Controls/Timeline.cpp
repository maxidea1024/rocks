#include <UiFramework/Controls/Timeline.h>

namespace rocks::ui {

namespace {

// 시간 막대 기본 크기, 눈금 기본 나눔(큰 눈금·작은 눈금)과 나눔 곱 상한이다.
constexpr Vec2 TimelineDefaultSize{160, 38};
constexpr uint32 DefaultMajorDivisions = 4;
constexpr uint32 DefaultMinorDivisions = 8;
constexpr uint32 MaxTotalDivisions = 4096;
// 표지(mark) 막대: 눈금 아래로 3, 폭 4, 높이 6이고 가운데에 맞춘다.
constexpr float MarkOffsetY = 3;
constexpr float MarkWidth = 4;
constexpr float MarkHeight = 6;
// 재생 위치 손잡이: 폭 8, 높이 5다.
constexpr float PlayheadWidth = 8;
constexpr float PlayheadHeight = 5;

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
        const float start = TrackStart(handle_, indent_);
        const float width =
            TrackPosition(maximum_, minimum_, maximum_, geometry.size.x, handle_, indent_) - start;
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

            result = Rectangle(
                list, geometry,
                TrackPosition(mark.time, minimum_, maximum_, geometry.size.x, handle_, indent_) -
                    MarkWidth * 0.5f,
                baseline + MarkOffsetY, MarkWidth, MarkHeight, mark.color, layer);
        }

        return result;
    }

    Array<TimelineMark> marks_;
    float minimum_;
    float maximum_;
    // 눈금 색이다(테마 Hover2 — UE 시간 막대 눈금처럼 바탕보다 밝은 회색).
    LinearColor color_ = DefaultThemeColors().Get(ThemeSlot::Hover2);
    uint32 major_ = DefaultMajorDivisions;
    uint32 minor_ = DefaultMinorDivisions;
    // 값 위치 계산에 쓰는 손잡이 폭·들여쓰기다(Timeline이 배치 때 넘긴다).
    float handle_ = 0;
    bool indent_ = true;
};

Timeline::Timeline(Allocator& allocator) noexcept : Slider(allocator), allocator_(allocator) {
    SetDesiredSize(TimelineDefaultSize);
    SetClipToBounds(true);
    // 재생 헤드는 Primary다(채움 색 — 기본 슬라이더의 채움은 투명이다).
    SetFillColor(DefaultThemeColors().Get(ThemeSlot::Primary));
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
    if (!major || !minor || major > MaxTotalDivisions / minor) {
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
    if (ruler_->handle_ != HandleWidth() || ruler_->indent_ != IsHandleIndented()) {
        ruler_->handle_ = HandleWidth();
        ruler_->indent_ = IsHandleIndented();
        ruler_->InvalidatePaint();
    }

    return ruler_->ArrangeIfNeeded(TrackGeometry());
}

Result<void> Timeline::OnPaint(PaintList& list, int32 layer) const noexcept {
    // 바탕은 Recessed 둥근 상자다(UE 시간 막대 바탕처럼 패널보다 어둡다). Frame 색을 tint로 곱한다.
    constexpr float BackgroundRadius = 2.0f;
    const StyleBrush background = StyleBrush::RoundedBox(ThemeSlot::Recessed, BackgroundRadius);
    return list.TryAddBrush(GetGeometry(), background.Resolve(list.Theme(), list.ColorContext()),
                            GetColor(), layer);
}

Result<void> Timeline::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    const auto geometry = TrackGeometry();
    const float x = TrackPosition(GetValue(), Minimum(), Maximum(), geometry.size.x, HandleWidth(),
                                  IsHandleIndented());
    auto drawn = Rectangle(list, geometry, x - 1, 2, 2, geometry.size.y - 2, FillColor(), layer);
    if (!drawn) {
        return drawn;
    }

    return Rectangle(list, geometry, x - PlayheadWidth * 0.5f, 0, PlayheadWidth, PlayheadHeight,
                     FillColor(), layer);
}

Result<void> Timeline::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

bool Timeline::RemoveChild(Element&) noexcept {
    return false;
}

} // namespace rocks::ui

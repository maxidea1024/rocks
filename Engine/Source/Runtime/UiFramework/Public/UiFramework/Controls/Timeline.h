#pragma once

#include <UiFramework/Controls/Slider.h>

namespace rocks::ui {

struct TimelineMark {
    float time;
    LinearColor color;
};

// 눈금과 마커는 별도 자식으로 캐시한다. 재생 위치만 변하면 눈금은 다시 그리지 않는다.
class ROCKS_UIFRAMEWORK_API Timeline final : public Slider {
public:
    explicit Timeline(Allocator& allocator) noexcept;
    Result<void> SetRulerColor(LinearColor color) noexcept;
    Result<void> SetMarks(Span<const TimelineMark> marks) noexcept;
    Result<void> SetDivisions(uint32 major, uint32 minor) noexcept;
    Result<void> SetRange(float minimum, float maximum) noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    bool RemoveChild(Element& child) noexcept override;

private:
    class Ruler;
    Result<void> EnsureRuler() noexcept;
    Allocator& allocator_;
    Ruler* ruler_ = nullptr;
};

} // namespace rocks::ui

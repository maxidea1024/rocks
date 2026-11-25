#pragma once

#include <UiCore/Element.h>
#include <UiWidget/RadialBoxSettings.h>

namespace rocks::ui {

// 생성 순서에 따라 겹치거나 원 둘레에 엔트리를 배치한다. 마지막 엔트리가 앞에 그려진다.
class EntryPatternPanel final : public Element {
public:
    EntryPatternPanel(Allocator& allocator, bool radial) noexcept;
    void SetSpacing(Vec2 spacing) noexcept;
    Result<void> SetPattern(Span<const Vec2> steps) noexcept;
    void SetRadialSettings(const RadialBoxSettings& settings) noexcept;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

protected:
    bool StartsPaintLayer(usize previous, usize next) const noexcept override;

private:
    bool radial_;
    Vec2 spacing_{};
    Array<Vec2> pattern_;
    RadialBoxSettings settings_{};
};

} // namespace rocks::ui

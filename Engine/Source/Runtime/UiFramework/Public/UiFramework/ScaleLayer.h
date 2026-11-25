#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 자식의 측정 배율과 배치 변환에 같은 상대 배율을 적용한다.
class ROCKS_UIFRAMEWORK_API ScaleLayer : public Element {
public:
    explicit ScaleLayer(Allocator& allocator) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    Result<void> SetScale(float scale) noexcept;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

protected:
    float RelativeLayoutScale() const noexcept override;

private:
    float scale_ = 1;
};

} // namespace rocks::ui

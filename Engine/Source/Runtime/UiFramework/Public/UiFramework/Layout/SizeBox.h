#pragma once

#include <Base/NumericLimits.h>
#include <UiCore/Element.h>

namespace rocks::ui {

// 내용 하나의 원하는 크기를 최소·최대 범위로 제한한다. 자신은 그리지 않는다.
class ROCKS_UIFRAMEWORK_API SizeBox : public Element {
    ROCKS_UI_ELEMENT(SizeBox, Element)

public:
    explicit SizeBox(Allocator& allocator) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;

    Result<void> SetSizeRange(Vec2 minimum, Vec2 maximum) noexcept;

    // 해당 축의 원하는 크기를 고정한다. Clear 호출 시 자식 크기를 다시 사용한다.
    Result<void> SetWidthOverride(float width) noexcept;
    Result<void> SetHeightOverride(float height) noexcept;
    void ClearWidthOverride() noexcept;
    void ClearHeightOverride() noexcept;

    static bool IsValidAspectRange(float minimum, float maximum) noexcept;
    Result<void> SetAspectRatioRange(float minimum, float maximum) noexcept;

    Vec2 ComputeDesiredSize(float) const noexcept override;

    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    Vec2 minimum_{};
    Vec2 maximum_{NumericLimits<float>::Max(), NumericLimits<float>::Max()};
    Vec2 aspectRange_{};
    Vec2 overrideSize_{};
    bool widthOverride_ = false;
    bool heightOverride_ = false;
};

} // namespace rocks::ui

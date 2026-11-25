#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 원본 이미지의 논리 크기를 배치에 제공하고, 할당된 영역에 브러시를 그린다.
class ROCKS_UIFRAMEWORK_API Image final : public Element {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    explicit Image(Allocator& allocator) noexcept;

    Result<void> SetBrush(const Brush& brush) noexcept;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    Brush brush_;
};

} // namespace rocks::ui

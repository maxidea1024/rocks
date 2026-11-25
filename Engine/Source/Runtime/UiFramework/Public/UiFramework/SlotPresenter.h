#pragma once
#include <UiCore/StackLayout.h>
#include <UiCore/Element.h>

namespace rocks::ui {

// 여백은 왼쪽·위·오른쪽·아래 순서다. Fill은 남은 영역을 채우고 나머지는 자식 측정 크기를 사용한다.
struct ROCKS_UIFRAMEWORK_API SlotPlacement {
    Vec4 padding{};
    StackAlignment horizontal = StackAlignment::Fill;
    StackAlignment vertical = StackAlignment::Fill;
    bool IsValid() const noexcept;
    Vec2 PaddedSize(Vec2 desired) const noexcept;
    Result<void> ArrangeChild(Element& child, const Geometry& area) const noexcept;
};

class ROCKS_UIFRAMEWORK_API SlotPresenter final : public Element {
public:
    SlotPresenter(Allocator& allocator, const SlotPlacement& placement) noexcept;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    static Result<void> WrapChild(Allocator& allocator, Element& child,
                                  const SlotPlacement& placement) noexcept;
    void SetPlacement(const SlotPlacement& placement) noexcept;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    SlotPlacement placement_;
};
} // namespace rocks::ui

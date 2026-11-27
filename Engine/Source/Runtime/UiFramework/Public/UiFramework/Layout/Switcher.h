#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 각 페이지의 상태를 보존하고 선택한 페이지 하나만 트리 순회에 노출한다.
class ROCKS_UIFRAMEWORK_API Switcher : public Element {
    ROCKS_UI_ELEMENT(Switcher, Element)

public:
    explicit Switcher(Allocator& allocator) noexcept;

    Result<void> SetActiveIndex(usize index) noexcept;
    Element* ActiveChild() const noexcept;

    bool RemoveChild(Element& child) noexcept override;
    Span<const SharedPtr<Element>> ActiveChildren() const noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    usize activeIndex_ = 0;
};

} // namespace rocks::ui

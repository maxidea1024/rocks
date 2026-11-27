#pragma once

#include <UiCore/Geometry.h>
#include <UiCore/StackLayout.h>

namespace rocks::ui {

class Element;

// slot의 여백과 두 축 정렬이다(UE TPaddingWidgetSlotMixin·TAlignmentWidgetSlotMixin —
// Docs/Design/UiToolkit 20.10절). 여백은 왼쪽·위·오른쪽·아래 순서다. Fill은 남은 영역을 채우고
// 나머지는 자식 측정 크기를 쓴다. 기본값(여백 0, 두 축 Fill)은 아무것도 바꾸지 않는다.
struct ROCKS_UICORE_API SlotPlacement {
    Vec4 padding{};
    StackAlignment horizontal = StackAlignment::Fill;
    StackAlignment vertical = StackAlignment::Fill;

    bool IsValid() const noexcept;
    // 기본값(아무것도 바꾸지 않음)인지다.
    bool IsIdentity() const noexcept;
    Vec2 PaddedSize(Vec2 desired) const noexcept;
    // natural 크기 자식을 area 안에 놓은 geometry다(UE AlignmentArrangeResult).
    Geometry PlaceChild(Vec2 natural, const Geometry& area) const noexcept;
    // child를 area 안에 놓아 배치한다(child의 원하는 크기 — slot 배치는 보지 않는다).
    Result<void> ArrangeChild(Element& child, const Geometry& area) const noexcept;
    bool operator==(const SlotPlacement&) const noexcept = default;
};

} // namespace rocks::ui

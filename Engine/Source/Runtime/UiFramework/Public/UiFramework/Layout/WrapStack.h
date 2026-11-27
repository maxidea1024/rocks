#pragma once

#include <UiFramework/Layout/StackPanel.h>
#include <UiCore/SlotPlacement.h>

namespace rocks::ui {

// forceNewLine이 켜져 있으면 현재 항목부터 새 줄을 시작한다. 한도보다 임계값이 크면
// 항목 하나가 한 줄을 차지한다. 두 설정을 함께 지정하면 명시적인 새 줄 시작이 우선한다.
struct ROCKS_UIFRAMEWORK_API WrapSlot {
    bool forceNewLine = false;
    bool fillEmptySpace = false;
    float fillLineWhenSizeLessThan = 0;
    SlotPlacement placement{};

    bool IsValid() const noexcept;
};

// 주축의 한도에 맞춰 자식을 여러 줄로 배치한다. 한 줄의 교차축 크기는 가장 큰 자식을 따른다.
class ROCKS_UIFRAMEWORK_API WrapStack : public Element {
public:
    explicit WrapStack(Allocator& allocator,
                       StackDirection direction = StackDirection::Horizontal) noexcept;

    Result<void> TryAddSlot(const SharedPtr<Element>& child, WrapSlot slot) noexcept;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    Result<void> SetSlot(Element& child, const WrapSlot& slot) noexcept;
    bool RemoveChild(Element& child) noexcept override;

    Result<void> SetPreferredExtent(float extent) noexcept;
    Result<void> SetSpacing(Vec2 spacing) noexcept;
    void SetUseAllottedSize(bool enabled) noexcept;

    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    struct Line {
        usize end;
        usize last;
        double length;
        float breadth;
    };

    Line MeasureLine(usize begin, float limit) const noexcept;

    Array<WrapSlot> slots_;
    StackDirection direction_;
    Vec2 spacing_{};
    float preferredExtent_ = 100;
    float allottedExtent_ = 0;
    bool hasAllottedExtent_ = false;
    bool useAllottedSize_ = false;
};

} // namespace rocks::ui

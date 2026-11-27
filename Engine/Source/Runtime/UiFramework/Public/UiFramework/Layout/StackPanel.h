#pragma once

#include <UiCore/Declarative.h>
#include <UiCore/StackLayout.h>

namespace rocks::ui {

enum class StackDirection : uint8 {
    Horizontal,
    Vertical
};

enum class FlowDirection : uint8 {
    LeftToRight,
    RightToLeft
};

inline constexpr StackSlot Auto{};
constexpr StackSlot Fill(float weight) noexcept {
    return {.rule = StackSizeRule::Fill, .weight = weight};
}

// 자식을 한 축으로 배치한다. Auto와 FillContent의 기준 크기는 자식의 측정 결과다.
class ROCKS_UIFRAMEWORK_API StackPanel : public Element {
    ROCKS_UI_ELEMENT(StackPanel, Element)

public:
    struct SlotArgs {
        StackSlot size = Auto;
        float padding = 0;
        StackAlignment align = StackAlignment::Fill;
        ElementChild content{};
    };
    struct Args {
        Span<const SlotArgs> slots{};
        FlowDirection flow = FlowDirection::LeftToRight;
        bool allowShrink = true;
        ElementArgs common{};
    };

    // Auto 슬롯은 자식의 측정 크기를 쓰고 Fill 슬롯은 남은 공간을 가중치로 나눈다.
    // padding은 네 변의 추가 여백이다. slots가 가리키는 배열은 Build 호출까지 유효해야 한다.
    Result<void> Construct(const Args& args) noexcept;

    StackPanel(Allocator& allocator, StackDirection direction) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;

    bool RemoveChild(Element& child) noexcept override;

    // 자식과 슬롯을 함께 추가한다. 실패하면 두 배열 모두 기존 상태를 유지한다.
    Result<void> TryAddSlot(const SharedPtr<Element>& child, const StackSlot& slot) noexcept;

    // 표시 중인 자식의 순서와 입력 상태를 유지하면서 새 배치 값을 적용한다.
    Result<void> SetSlotLayout(const Element& child, const StackSlot& slot) noexcept;

    bool RemoveSlot(Element& child) noexcept;

    // 접히지 않은 자식 사이에만 주축 간격을 넣는다.
    Result<void> SetSpacing(float spacing) noexcept;
    void SetDirection(StackDirection direction) noexcept;
    void SetFlowDirection(FlowDirection flow) noexcept;

    // 축소를 끄면 가변 슬롯들의 기준 크기 합을 배분할 공간의 하한으로 삼는다.
    void SetAllowShrink(bool allowShrink) noexcept;

    Result<void> Arrange(const Geometry& geometry) noexcept override;

    Vec2 ComputeDesiredSize(float) const noexcept override;
    // 세로 쌓기는 자식마다 같은 폭으로 측정해 높이를 더한다. 가로 쌓기는 폭을 나누는 규칙이 Fill
    // 배분에 달려 있어 원하는 크기 그대로다.
    Vec2 MeasureForWidth(float width) noexcept override;

private:
    Allocator& allocator_;
    Array<StackSlot> slots_;
    StackDirection direction_;
    FlowDirection flow_ = FlowDirection::LeftToRight;
    float spacing_ = 0;
    bool allowShrink_ = true;
};

} // namespace rocks::ui

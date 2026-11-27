#pragma once

#include <Core/Math/Vector4.h>
#include <Core/Optional.h>
#include <UiCore/Declarative.h>
#include <UiCore/StackLayout.h>

namespace rocks::ui {

// 배경 사각형을 그리는 Element다. 자식 배치는 호출자가 지정한다.
class ROCKS_UIFRAMEWORK_API Frame : public Element {
    ROCKS_UI_ELEMENT(Frame, Element)

public:
    struct Args {
        ElementChild content{};
        Bindable<LinearColor> color{LinearColor{1, 1, 1, 1}};
        Brush brush{};
        Vec4 padding{};
        Bindable<Vec2> desiredSize{};
        // 여백 안 내용 정렬이다(UE SBorder HAlign·VAlign). 비우면 지금 정렬(기본 Fill)을 둔다.
        Optional<StackAlignment> horizontalAlignment{};
        Optional<StackAlignment> verticalAlignment{};
        ElementArgs common{};
    };

    // 한 자식의 배경과 안쪽 여백을 설정한다. color는 다시 그리기만, desiredSize는
    // 레이아웃 측정까지 무효화한다. 고정값과 Bind로 만든 getter를 같은 필드에 쓸 수 있다.
    Result<void> Construct(Args&& args) noexcept;

    explicit Frame(Allocator& allocator) noexcept;

    // Frame은 하나의 내용만 가진다.
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;

    void SetColor(LinearColor color) noexcept;
    void BindColor(Bindable<LinearColor> source) noexcept;

    Result<void> SetBrush(const Brush& brush) noexcept;
    const Brush& GetBrush() const noexcept;

    LinearColor GetColor() const noexcept;

    void SetDesiredSize(Vec2 size) noexcept;
    void BindDesiredSize(Bindable<Vec2> source) noexcept;

    // x/y/z/w는 각각 왼쪽·위·오른쪽·아래 여백이다.
    Result<void> SetPadding(Vec4 padding) noexcept;

    // 여백 안에서 내용을 놓는 정렬이다(UE SBorder HAlign·VAlign). 기본은 가로·세로 모두 Fill이다.
    // Fill이 아닌 축은 내용의 원하는 크기(여백 안 크기를 넘지 않게)로 놓는다. 예: 버튼이 글자
    // 줄보다 높을 때 글자를 세로 가운데에 두려면 SetContentAlignment(Fill, Center).
    void SetContentAlignment(StackAlignment horizontal, StackAlignment vertical) noexcept;

    Vec2 ComputeDesiredSize(float) const noexcept override;
    Vec2 MeasureForWidth(float width) noexcept override;

    Result<void> Arrange(const Geometry& geometry) noexcept override;

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    Brush brush_;
    ElementAttribute<LinearColor, InvalidateReason::Paint> color_;
    ElementAttribute<Vec2, InvalidateReason::Layout> desiredSize_;
    Vec4 padding_{};
    StackAlignment horizontal_ = StackAlignment::Fill;
    StackAlignment vertical_ = StackAlignment::Fill;
};

} // namespace rocks::ui

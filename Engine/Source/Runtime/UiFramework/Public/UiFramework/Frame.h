#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/Declarative.h>

namespace rocks::ui {

// 배경 사각형을 그리는 Element다. 자식 배치는 호출자가 지정한다.
class ROCKS_UIFRAMEWORK_API Frame : public Element {
public:
    using Super = Element;
    struct Args {
        ElementChild content{};
        Bindable<LinearColor> color{LinearColor{1, 1, 1, 1}};
        Brush brush{};
        Vec4 padding{};
        Bindable<Vec2> desiredSize{};
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

    Vec2 ComputeDesiredSize(float) const noexcept override;

    Result<void> Arrange(const Geometry& geometry) noexcept override;

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    Brush brush_;
    ElementAttribute<LinearColor, InvalidateReason::Paint> color_;
    ElementAttribute<Vec2, InvalidateReason::Layout> desiredSize_;
    Vec4 padding_{};
};

} // namespace rocks::ui

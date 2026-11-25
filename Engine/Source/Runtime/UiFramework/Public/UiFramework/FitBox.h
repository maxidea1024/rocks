#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

enum class FitMode : uint8 {
    None,
    Fill,
    Fit,
    FitWidth,
    FitHeight,
    Cover,
    User
};

enum class FitDirection : uint8 {
    Both,
    ShrinkOnly,
    GrowOnly
};

// 자식 하나를 할당 영역에 맞춘다. Fit과 Cover는 자식의 종횡비를 유지한다.
class ROCKS_UIFRAMEWORK_API FitBox : public Element {
public:
    explicit FitBox(Allocator& allocator) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    void SetFit(FitMode mode, FitDirection direction = FitDirection::Both) noexcept;

    Result<void> SetUserScale(float scale) noexcept;

    // 0은 시작, 0.5는 중앙, 1은 끝이다. 넘친 내용에도 같은 정렬을 적용한다.
    Result<void> SetAlignment(Vec2 alignment) noexcept;

    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    float userScale_ = 1;
    FitMode mode_ = FitMode::Fit;
    FitDirection direction_ = FitDirection::Both;
    Vec2 alignment_{0.5f, 0.5f};
};

} // namespace rocks::ui

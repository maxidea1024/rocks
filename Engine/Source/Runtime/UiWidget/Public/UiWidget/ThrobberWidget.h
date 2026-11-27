#pragma once

#include <UiWidget/Widget.h>

namespace rocks::ui {

// 가로로 놓인 점이 차례로 밝아져 완료 시점을 모르는 작업의 진행을 표시한다.
// SetAnimating(false)는 현재 위상에서 멈춘다. SetPieceCount가 허용하는 범위는 1~64다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ThrobberWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetPieceCount(uint32 count) noexcept;
    void SetAnimating(bool animate) noexcept;
    // 브러시의 논리 크기와 색조는 에셋에 보존하고 런타임 텍스처는 이 함수로 연결한다.
    Result<void> SetPieceBrush(const Brush& brush) noexcept;
    void SetAnimationAxes(bool horizontal, bool vertical, bool opacity) noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "pieceSize") Vec2 pieceSize_ { 8, 8 };
    ROCKS_PROPERTY(Name = "pieceTint") LinearColor pieceTint_ { 1, 1, 1, 1 };
    ROCKS_NATIVE() Brush brush_ {};
    ROCKS_PROPERTY(Name = "animateHorizontally") bool animateHorizontally_ = true;
    ROCKS_PROPERTY(Name = "animateVertically") bool animateVertically_ = true;
    ROCKS_PROPERTY(Name = "animateOpacity") bool animateOpacity_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "pieceCount") uint32 pieceCount_ = 3;
    ROCKS_PROPERTY(Name = "animating") bool animating_ = true;
};

} // namespace rocks::ui

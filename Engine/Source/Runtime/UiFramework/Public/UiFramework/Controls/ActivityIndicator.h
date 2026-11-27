#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 활동 표시의 조각 수 상한이다(위젯 층 Throbber도 같은 한도).
inline constexpr uint32 MaxActivityIndicatorPieces = 64;

// 선형 또는 원형으로 놓인 점의 밝기를 순서대로 바꾼다. 애니메이션 중인 이 요소만
// volatile로 표시한다. 일시 정지하면 현재 위상을 유지하며 캐시한 그리기 결과를 재사용한다.
// 시각은 UI 프레임 시각이다(Docs/Design/UiToolkit 20.3절 — 일시 정지·녹화 재생은 Application
// 시계로 한다).
class ROCKS_UIFRAMEWORK_API ActivityIndicator final : public Element {
    ROCKS_UI_ELEMENT(ActivityIndicator, Element)

public:
    ActivityIndicator(Allocator& allocator, bool circular) noexcept;
    Result<void> SetPieceBrush(const Brush& brush) noexcept;
    void SetAnimationAxes(bool horizontal, bool vertical, bool opacity) noexcept;
    Result<void> SetRadius(float radius) noexcept;
    Result<void> SetPeriod(float seconds) noexcept;
    Result<void> SetPieceCount(uint32 count) noexcept;
    void SetAnimating(bool animate) noexcept;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;

private:
    double Elapsed(double now) const noexcept;
    double Cycle() const noexcept;
    void UpdateVolatility() noexcept;
    double origin_ = 0;
    double elapsed_ = 0;
    // 조각은 8x8 원이다(UE "Throbber.CircleChunk" 이미지의 모양 — 이미지 경로 전까지 알약 brush).
    Brush brush_ = [] {
        Brush piece = Brush::PillBox({1, 1, 1, 1});
        piece.imageSize = {8, 8};
        return piece;
    }();
    float radius_ = 16;
    float period_;
    bool horizontal_ = true;
    bool vertical_ = true;
    bool opacity_ = true;
    uint32 count_;
    bool circular_;
    bool animating_ = true;
};

} // namespace rocks::ui

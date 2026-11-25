#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 선형 또는 원형으로 놓인 점의 밝기를 순서대로 바꾼다. 애니메이션 중인 이 요소만
// volatile로 표시한다. 일시 정지하면 현재 위상을 유지하며 캐시한 그리기 결과를 재사용한다.
class ROCKS_UIFRAMEWORK_API ActivityIndicator final : public Element {
public:
    // 기본 시계는 단조 시간이다. 게임 일시 정지나 녹화 재생에는 자체 나노초 시계를 넘길 수 있다.
    using Clock = uint64 (*)() noexcept;
    ActivityIndicator(Allocator& allocator, bool circular, Clock clock = nullptr) noexcept;
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
    double Elapsed(uint64 now) const noexcept;
    double Cycle() const noexcept;
    void UpdateVolatility() noexcept;
    uint64 origin_ = 0;
    double elapsed_ = 0;
    Clock clock_;
    Brush brush_{.imageSize = {8, 8}};
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

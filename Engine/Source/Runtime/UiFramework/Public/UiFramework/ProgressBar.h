#pragma once

#include <UiFramework/Frame.h>

namespace rocks::ui {

// 가운데에서 퍼지는 방식은 양 축 또는 지정한 한 축만 채운다.
enum class ProgressFillStyle : uint8 {
    Scale,
    Mask
};

enum class ProgressDirection : uint8 {
    LeftToRight,
    RightToLeft,
    TopToBottom,
    BottomToTop,
    FromCenter,
    FromCenterHorizontal,
    FromCenterVertical
};

// 진행률이 정해지면 채움 비율을 캐시한다. 작업량을 모를 때는 SetMarquee(true)로 이동하는 띠를
// 표시한다.
class ROCKS_UIFRAMEWORK_API ProgressBar : public Frame {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    using Super = Frame;
    struct Args {
        ElementChild content{};
        Bindable<LinearColor> color{LinearColor{1, 1, 1, 1}};
        Brush brush{};
        Vec4 padding{};
        Bindable<Vec2> desiredSize{};
        ElementArgs common{};
        Bindable<float> value{0.0f};
        ProgressDirection direction = ProgressDirection::LeftToRight;
        bool marquee = false;
        LinearColor fillColor{0.0f, 0.45f, 0.85f, 1.0f};
    };
    using Frame::Construct;
    Result<void> Construct(Args&& args) noexcept;
    explicit ProgressBar(Allocator& allocator) noexcept;

    Result<void> SetValue(float value) noexcept;
    void BindValue(Bindable<float> source) noexcept;
    Result<void> SetDirection(ProgressDirection direction) noexcept;
    // Scale은 채움 브러시를 줄이고, Mask는 완성 크기의 브러시에서 진행 구간만 보인다.
    Result<void> SetFillBrush(const Brush& brush) noexcept;
    Result<void> SetFillStyle(ProgressFillStyle style) noexcept;
    Result<void> SetBorderPadding(Vec2 padding) noexcept;
    void SetMarquee(bool enabled) noexcept;
    float GetValue() const noexcept;
    void SetFillColor(LinearColor color) noexcept;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    ElementAttribute<float, InvalidateReason::Paint> value_;
    ProgressDirection direction_ = ProgressDirection::LeftToRight;
    Brush fillBrush_{};
    ProgressFillStyle fillStyle_ = ProgressFillStyle::Scale;
    Vec2 borderPadding_{};
    bool marquee_ = false;
    uint64 marqueeStart_ = 0;
    LinearColor fillColor_{0.0f, 0.45f, 0.85f, 1.0f};
};

} // namespace rocks::ui

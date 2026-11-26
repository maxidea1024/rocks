#pragma once

#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Layout/Frame.h>

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
        // 바탕 brush에 곱한다(바탕 모양은 스타일 — UE FProgressBarStyle.BackgroundImage).
        Bindable<LinearColor> color{LinearColor{1, 1, 1, 1}};
        Vec4 padding{};
        Bindable<Vec2> desiredSize{};
        ElementArgs common{};
        Bindable<float> value{0.0f};
        ProgressDirection direction = ProgressDirection::LeftToRight;
        bool marquee = false;
        // 채움 brush에 곱한다(UE FillColorAndOpacity — 기본 흰색은 스타일 색 그대로).
        LinearColor fillColor{1.0f, 1.0f, 1.0f, 1.0f};
    };
    using Frame::Construct;
    Result<void> Construct(Args&& args) noexcept;
    explicit ProgressBar(Allocator& allocator) noexcept;

    Result<void> SetValue(float value) noexcept;
    void BindValue(Bindable<float> source) noexcept;
    Result<void> SetDirection(ProgressDirection direction) noexcept;
    // 채움 brush를 스타일 채움 대신 쓴다(진행 값이 정해진 채움만 — marquee는 늘 스타일 marquee다).
    // ClearFillBrush로 스타일 채움으로 돌아간다. SetStyle은 덮은 채움을 지우지 않는다.
    // Scale은 채움 브러시를 줄이고, Mask는 완성 크기의 브러시에서 진행 구간만 보인다.
    Result<void> SetFillBrush(const Brush& brush) noexcept;
    void ClearFillBrush() noexcept;
    Result<void> SetFillStyle(ProgressFillStyle style) noexcept;
    Result<void> SetBorderPadding(Vec2 padding) noexcept;
    void SetMarquee(bool enabled) noexcept;
    float GetValue() const noexcept;
    void SetFillColor(LinearColor color) noexcept;
    // 바탕·채움·marquee 모양이다(UE FProgressBarStyle — 기본 "ProgressBar"). Frame brush는 쓰지
    // 않고, Frame 색은 바탕 tint다.
    Result<void> SetStyle(const ProgressBarStyle& style) noexcept;
    const ProgressBarStyle& GetStyle() const noexcept;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    ElementAttribute<float, InvalidateReason::Paint> value_;
    ProgressDirection direction_ = ProgressDirection::LeftToRight;
    Brush fillBrush_{};
    ProgressFillStyle fillStyle_ = ProgressFillStyle::Scale;
    Vec2 borderPadding_{};
    bool marquee_ = false;
    uint64 marqueeStart_ = 0;
    LinearColor fillColor_{1.0f, 1.0f, 1.0f, 1.0f};
    ProgressBarStyle style_ = DefaultProgressBarStyle();
    // SetFillBrush로 채움 brush를 정했는지다.
    bool customFill_ = false;
};

} // namespace rocks::ui

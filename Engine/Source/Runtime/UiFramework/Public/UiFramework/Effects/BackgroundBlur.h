#pragma once

#include <UiCore/Declarative.h>

#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

// 자식 아래까지 그려진 화면을 흐리게 합성한다. 강도는 대상 텍스처의 픽셀 반경이다.
class ROCKS_UIFRAMEWORK_API BackgroundBlur final : public SizeBox {
public:
    struct Args {
        ElementChild content{};
        Bindable<float> strength{};
        Vec4 cornerRadius{};
        int32 blurRadius = -1;
        bool applyAlphaToBlur = true;
        bool lowQuality = false;
        Brush fallback{.mode = BrushMode::None};
        ElementArgs common{};
    };

    // Build·Make로 중첩할 때 자식과 그리기 속성을 한 번에 연결한다.
    Result<void> Construct(Args&& args) noexcept;

    explicit BackgroundBlur(Allocator& allocator) noexcept;

    Result<void> SetStrength(float pixels) noexcept;

    // 모서리는 왼쪽 위부터 시계 방향이며 단위는 위젯 로컬 좌표다.
    Result<void> SetCornerRadius(Vec4 corners) noexcept;
    // -1은 강도에 맞는 폭을 사용한다. 다른 값은 필터의 샘플 영역을 픽셀로 지정한다.
    Result<void> SetBlurRadius(int32 kernel) noexcept;
    void SetApplyAlphaToBlur(bool apply) noexcept;
    void SetLowQuality(bool enabled) noexcept;
    Result<void> SetLowQualityFallbackBrush(const Brush& brush) noexcept;

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    Vec4 cornerRadius_{};
    int32 blurRadius_ = -1;
    bool applyAlpha_ = true;
    bool lowQuality_ = false;
    Brush fallback_{.mode = BrushMode::None};
    ElementAttribute<float, InvalidateReason::Paint> strength_;
};

} // namespace rocks::ui

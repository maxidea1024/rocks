#pragma once

#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 패널 뒤의 현재 화면을 흐리게 한 다음 자식을 선명하게 그린다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API BackgroundBlurWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit BackgroundBlurWidget(Allocator& allocator) noexcept;

    // 0이면 필터 패스를 생략한다. 단위는 대상 화면의 픽셀이다.
    Result<void> SetStrength(float pixels) noexcept;
    Result<void> SetCornerRadius(Vec4 corners) noexcept;
    Result<void> SetBlurRadius(int32 kernel) noexcept;
    void SetApplyAlphaToBlur(bool apply) noexcept;
    void SetLowQuality(bool enabled) noexcept;
    Result<void> SetLowQualityFallbackBrush(const Brush& brush) noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "cornerRadius") Vec4 cornerRadius_ {};
    ROCKS_PROPERTY(Name = "applyAlphaToBlur") bool applyAlphaToBlur_ = true;
    ROCKS_PROPERTY(Name = "blurRadius") int32 blurRadius_ = -1;
    ROCKS_PROPERTY(Name = "lowQuality") bool lowQuality_ = false;
    ROCKS_PROPERTY(Name = "fallbackTint") LinearColor fallbackTint_ { 0, 0, 0, 0 };
    ROCKS_NATIVE() Brush fallback_ {};
    ROCKS_PROPERTY(Name = "strength") float strength_ = 0;
};

} // namespace rocks::ui

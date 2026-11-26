#pragma once

#include <UiWidget/Widget.h>
#include <UiFramework/Controls/ProgressBar.h>

namespace rocks::ui {

// 0~1 범위의 진행률을 표시한다. SetValue로 설정한 값은 화면을 재구성해도 유지된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ProgressBarWidget final : public Widget {
    ROCKS_GENERATED_BODY()

public:
    Result<void> SetValue(float value) noexcept;
    // 채움 방향과 불확정 상태를 저장해 네이티브 위젯을 다시 만들어도 복원한다.
    Result<void> SetDirection(ProgressDirection direction) noexcept;
    ProgressDirection Direction() const noexcept;
    Result<void> SetFillBrush(const Brush& brush) noexcept;
    Result<void> SetFillColor(LinearColor color) noexcept;
    Result<void> SetFillStyle(ProgressFillStyle style) noexcept;
    Result<void> SetBorderPadding(Vec2 padding) noexcept;
    void SetMarquee(bool enabled) noexcept;
    bool IsMarquee() const noexcept;
    float Value() const noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "value") float value_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "direction") uint8 direction_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "marquee") bool marquee_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "fillColor") LinearColor fillColor_ { 1, 1, 1, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "borderPadding") Vec2 borderPadding_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "fillStyle") uint8 fillStyle_ = 0;
    ROCKS_NATIVE() Brush fillBrush_ {};
    // SetFillBrush를 불렀는지다. 부르지 않았으면 Element가 스타일 채움(UE "ProgressBar")을 쓴다.
    ROCKS_NATIVE() bool customFill_ = false;
    ROCKS_NATIVE() SharedPtr<ProgressBar> bar_;
};

} // namespace rocks::ui

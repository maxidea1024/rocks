#pragma once

#include <Core/Delegate.h>
#include <UiCore/ThemeColors.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 오디오 게인이나 게임 HUD의 다이얼을 원호를 따라 조절한다. 기본 가동 범위는 왼쪽 아래에서
// 오른쪽 아래까지 시계 방향 240도다. SetValue는 모델 값을 적용하고 사용자 입력만 변경 콜백을
// 보낸다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API RadialSliderWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetLimits(float minimum, float maximum, float step) noexcept;
    Result<void> SetValue(float value) noexcept;
    float Value() const noexcept;
    // 아래 방향이 0도다. 시작과 끝은 0~360도 안에서 지정하며 offset은 원호 전체를 회전시킨다.
    Result<void> SetArc(float startDegrees, float endDegrees, float offsetDegrees = 0) noexcept;
    void SetSnapToStep(bool enabled) noexcept;
    void SetHandVisibility(bool showHandle, bool showHand) noexcept;
    Result<void> SetColors(LinearColor track, LinearColor progress, LinearColor handle) noexcept;
    void SetOnChanged(Delegate<void(float)>&& callback) noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "value") float value_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "minimum") float minimum_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "maximum") float maximum_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "step") float step_ = 0.01f;
    ROCKS_PROPERTY(ReadOnly, Name = "startAngle") float startAngle_ = 60;
    ROCKS_PROPERTY(ReadOnly, Name = "endAngle") float endAngle_ = 300;
    ROCKS_PROPERTY(ReadOnly, Name = "angularOffset") float angularOffset_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "snapToStep") bool snapToStep_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "showHandle") bool showHandle_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "showHand") bool showHand_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "trackColor")
    LinearColor trackColor_ = DefaultThemeColors().Get(ThemeSlot::Input);
    ROCKS_PROPERTY(ReadOnly, Name = "progressColor")
    LinearColor progressColor_ = DefaultThemeColors().Get(ThemeSlot::Primary);
    ROCKS_PROPERTY(ReadOnly, Name = "handleColor")
    LinearColor handleColor_ = DefaultThemeColors().Get(ThemeSlot::Foreground);
    ROCKS_NATIVE() Delegate<void(float)> changed_;
};

} // namespace rocks::ui

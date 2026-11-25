#pragma once

#include <Core/Delegate.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 0~1 범위의 값을 조절한다. SetOnChanged는 사용자 입력으로 바뀐 값을 전달한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API SliderWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetValue(float value) noexcept;
    Result<void> SetRange(float minimum, float maximum) noexcept;
    Result<void> SetStep(float step) noexcept;
    void SetVertical(bool vertical) noexcept;
    void SetSnapToStep(bool enabled) noexcept;
    float GetNormalizedValue() const noexcept;
    void SetLocked(bool locked) noexcept;
    void SetIndentHandle(bool indent) noexcept;
    Result<void> SetTints(LinearColor bar, LinearColor handle) noexcept;
    void SetRequiresControllerLock(bool required) noexcept;
    void SetOnControllerCaptureBegin(Delegate<void()>&& callback) noexcept;
    void SetOnControllerCaptureEnd(Delegate<void()>&& callback) noexcept;
    void SetOnCaptureBegin(Delegate<void()>&& callback) noexcept;
    void SetOnCaptureEnd(Delegate<void()>&& callback) noexcept;
    float Value() const noexcept;
    void SetOnChanged(Delegate<void(float)>&& callback) noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "value") float value_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "minimum") float minimum_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "maximum") float maximum_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "step") float step_ = 0.01f;
    ROCKS_PROPERTY(ReadOnly, Name = "vertical") bool vertical_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "snapToStep") bool snapToStep_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "locked") bool locked_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "indentHandle") bool indentHandle_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "barTint") LinearColor barTint_ { 1, 1, 1, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "handleTint") LinearColor handleTint_ { 0, 0.45f, 0.85f, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "requiresControllerLock") bool requiresControllerLock_ = true;
    ROCKS_NATIVE() Delegate<void()> controllerBegin_;
    ROCKS_NATIVE() Delegate<void()> controllerEnd_;
    ROCKS_NATIVE() Delegate<void()> captureBegin_;
    ROCKS_NATIVE() Delegate<void()> captureEnd_;
    ROCKS_NATIVE() Delegate<void(float)> changed_;
};

} // namespace rocks::ui

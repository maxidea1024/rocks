#pragma once

#include <Core/Delegate.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 0~1 범위의 값을 조절한다. OnChanged 이벤트는 사용자 입력으로 바뀐 값을 전달한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API SliderWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    // 이벤트 필드가 allocator를 받는다(EventReflection ED14).
    explicit SliderWidget(Allocator& allocator) noexcept;
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
    // 사건은 리플렉션 이벤트다(Docs/Design/UiToolkit 21.5a절): C++는 TryAddLambda·TryAdd(native
    // 구독), 에셋·Sparks는 TryBind(리플렉션 함수)로 받는다. 구독은 여럿일 수 있다.
    Event<>& OnControllerCaptureBegin() noexcept;
    Event<>& OnControllerCaptureEnd() noexcept;
    Event<>& OnCaptureBegin() noexcept;
    Event<>& OnCaptureEnd() noexcept;
    float Value() const noexcept;
    Event<float>& OnChanged() noexcept;

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
    ROCKS_PROPERTY(ReadOnly, Name = "handleTint") LinearColor handleTint_ { 1, 1, 1, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "requiresControllerLock") bool requiresControllerLock_ = true;
    ROCKS_PROPERTY(Transient, Name = "controllerCaptureBegin") Event<> controllerBegin_;
    ROCKS_PROPERTY(Transient, Name = "controllerCaptureEnd") Event<> controllerEnd_;
    ROCKS_PROPERTY(Transient, Name = "captureBegin") Event<> captureBegin_;
    ROCKS_PROPERTY(Transient, Name = "captureEnd") Event<> captureEnd_;
    ROCKS_PROPERTY(Transient, Name = "valueChanged", Params = "value") Event<float> changed_;
};

} // namespace rocks::ui

#pragma once

#include <UiWidget/WidgetSpinCommitReason.h>
#include <Core/Delegate.h>
#include <UiFramework/Controls/SpinBox.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 연속 드래그, 방향키 또는 직접 입력으로 숫자를 조절한다. 현재 값·범위·스텝은
// 위젯 트리에 저장된다. 화면을 구성하기 전에 WidgetTree에 텍스트 셰이퍼를 지정한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API SpinBoxWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    // 이벤트 필드가 allocator를 받는다(EventReflection ED14).
    explicit SpinBoxWidget(Allocator& allocator) noexcept;
    Result<void> SetLimits(float minimum, float maximum, float step) noexcept;
    Result<void> SetValue(float value) noexcept;
    // 소수부 표시만 바꾸며, 사용자 입력의 반올림은 별도 단계 설정을 따른다.
    Result<void> SetFractionalDigits(int32 minimum, int32 maximum) noexcept;
    void SetSliderEnabled(bool enabled) noexcept;
    void SetSnapToStep(bool enabled) noexcept;
    // 사건은 리플렉션 이벤트다(Docs/Design/UiToolkit 21.5a절): C++는 TryAddLambda·TryAdd(native
    // 구독), 에셋·Sparks는 TryBind(리플렉션 함수)로 받는다. 구독은 여럿일 수 있다.
    Event<float, WidgetSpinCommitReason>& OnCommitted() noexcept;
    Event<>& OnDragBegin() noexcept;
    Event<float>& OnDragEnd() noexcept;
    float Value() const noexcept;
    Event<float>& OnChanged() noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "value") float value_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "minimum") float minimum_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "maximum") float maximum_ = 100;
    ROCKS_PROPERTY(ReadOnly, Name = "step") float step_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "sliderEnabled") bool sliderEnabled_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "snapToStep") bool snapToStep_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "minFractionDigits") int32 minFractionDigits_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "maxFractionDigits") int32 maxFractionDigits_ = -1;
    ROCKS_PROPERTY(Transient, Name = "committed", Params = "value,reason")
    Event<float, WidgetSpinCommitReason> committed_;
    ROCKS_PROPERTY(Transient, Name = "dragBegin") Event<> beginDrag_;
    ROCKS_PROPERTY(Transient, Name = "dragEnd", Params = "value") Event<float> endDrag_;
    ROCKS_PROPERTY(Transient, Name = "changed", Params = "value") Event<float> changed_;
};

} // namespace rocks::ui

#pragma once

#include <Core/Delegate.h>
#include <Core/Optional.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Layout/Frame.h>

namespace rocks::ui {

// 마우스·터치 drag와 좌우 키로 값을 조절하는 수평 슬라이더다. 모양은 SliderStyle(UE "Slider")이고,
// Frame 색은 막대 tint(UE SliderBarColor), 손잡이 색은 손잡이 tint(UE SliderHandleColor)다.
class ROCKS_UIFRAMEWORK_API Slider : public Frame {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    explicit Slider(Allocator& allocator) noexcept;

    virtual Result<void> SetRange(float minimum, float maximum) noexcept;
    Result<void> SetValue(float value) noexcept;
    Result<void> SetStep(float step) noexcept;
    // 세로 방향에서는 위쪽이 최댓값이다. 포인터 스냅은 최솟값을 기준으로 가장 가까운 단계에 맞춘다.
    void SetVertical(bool vertical) noexcept;
    void SetSnapToStep(bool enabled) noexcept;
    void SetLocked(bool locked) noexcept;
    // 잠금 모드에서 South로 게임패드 조절을 시작하거나 마치고, 조절 중에는 화면 탐색보다 방향
    // 입력을 우선한다.
    void SetRequiresControllerLock(bool required) noexcept;
    void SetOnControllerCaptureBegin(Delegate<void()>&& callback) noexcept;
    void SetOnControllerCaptureEnd(Delegate<void()>&& callback) noexcept;

    void OnFocusLost(uint32 user) noexcept override;
    void SetIndentHandle(bool indent) noexcept;
    void SetHandleColor(LinearColor color) noexcept;
    // 원하는 크기는 UE SSlider::ComputeDesiredSize(143-160행)처럼 (16, max(막대 두께, 손잡이
    // 높이))다 (세로면 뒤집는다). Frame 원하는 크기를 정했으면 그것을 쓴다.
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    // 막대·손잡이 brush가 그릴 수 없는 값이거나 두께·크기가 음수·비유한이면 InvalidArgument다.
    Result<void> SetStyle(const SliderStyle& style) noexcept;
    const SliderStyle& GetStyle() const noexcept;
    void OnMouseEnter(uint32 user) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
    void SetOnCaptureBegin(Delegate<void()>&& callback) noexcept;
    void SetOnCaptureEnd(Delegate<void()>&& callback) noexcept;
    float GetNormalizedValue() const noexcept;
    float GetValue() const noexcept;
    // 손잡이 앞쪽을 채우는 색이다. UE 기본 슬라이더에는 채움이 없어 기본은 투명이다(원형 슬라이더의
    // 진행 표시·타임라인 재생 헤드가 쓴다).
    void SetFillColor(LinearColor color) noexcept;
    void SetOnChanged(Delegate<void(float)>&& callback) noexcept;

    bool SupportsKeyboardFocus() const noexcept override;
    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    void OnPointerCaptureLost(PointerSource source, uint32 userId,
                              uint32 pointerId) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

protected:
    // 포인터 캡처·단계 맞춤·이벤트 발송은 공유하고 트랙 모양에 따라 좌표 해석만 바꾼다.
    virtual void UpdateFromPointer(Vec2 position, bool firstPoint) noexcept;
    void SetInteractiveRatio(float ratio) noexcept;
    void SetInteractiveValue(float value) noexcept;
    Geometry TrackGeometry() const noexcept;
    float Minimum() const noexcept;
    float Maximum() const noexcept;
    LinearColor FillColor() const noexcept;
    // 값 범위의 시작 위치(값 최소일 때 손잡이 가운데)다. UE SSlider(OnPaint 81-92행)처럼 손잡이 폭
    // handle과 들여쓰기(indent면 handle)로 정한다: 들여쓰기 / 2 + handle / 2.
    static float TrackStart(float handle, bool indent) noexcept;
    // 값 위치(손잡이 가운데)다: 시작 + 비율 x (폭 - 들여쓰기 - handle). 포인터 해석(UE
    // PositionToValue 439-445행)도 같은 식을 거꾸로 쓴다.
    static float TrackPosition(float value, float minimum, float maximum, float width, float handle,
                               bool indent) noexcept;
    // 지금 스타일의 손잡이 폭과 들여쓰기 여부다.
    float HandleWidth() const noexcept;
    bool IsHandleIndented() const noexcept;
    // 끌기 상태를 바꾸고, 바뀌면 다시 그린다(올림·끌기 모양이 끌기에 달려 있다).
    void SetDragging(bool dragging) noexcept;

private:
    // 패드 버튼 처리다(조종 잠금 South·East, D-pad 값 증감). 처리했으면 true다.
    bool HandleGamepadButton(uint32 user, input::GamepadButton button) noexcept;
    Delegate<void(float)> onChanged_;
    LinearColor fillColor_{0, 0, 0, 0};
    SliderStyle style_ = DefaultSliderStyle();
    uint32 hoverCount_ = 0;
    float minimum_ = 0;
    float maximum_ = 1;
    float value_ = 0;
    float step_ = 0.01f;
    PointerSource dragSource_ = PointerSource::Mouse;
    uint32 dragUser_ = 0;
    uint32 dragPointer_ = 0;
    void EndControllerCapture() noexcept;
    Delegate<void()> controllerBegin_, controllerEnd_;
    bool requiresControllerLock_ = true;
    bool controllerCaptured_ = false;
    uint32 controllerUser_ = 0;
    bool locked_ = false;
    bool indentHandle_ = true;
    Optional<LinearColor> handleTint_;
    Delegate<void()> captureBegin_, captureEnd_;
    bool dragging_ = false;
    bool vertical_ = false;
    bool snapToStep_ = false;
};

} // namespace rocks::ui

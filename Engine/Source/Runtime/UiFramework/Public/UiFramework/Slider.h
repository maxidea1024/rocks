#pragma once

#include <Core/Delegate.h>
#include <Core/Optional.h>
#include <UiFramework/Frame.h>

namespace rocks::ui {

// 마우스·터치 drag와 좌우 키로 값을 조절하는 수평 슬라이더다.
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
    bool OnGamepadButtonDown(uint32 user, input::GamepadButton button) noexcept override;
    void OnFocusLost(uint32 user) noexcept override;
    void SetIndentHandle(bool indent) noexcept;
    void SetHandleColor(LinearColor color) noexcept;
    void SetOnCaptureBegin(Delegate<void()>&& callback) noexcept;
    void SetOnCaptureEnd(Delegate<void()>&& callback) noexcept;
    float GetNormalizedValue() const noexcept;
    float GetValue() const noexcept;
    void SetFillColor(LinearColor color) noexcept;
    void SetOnChanged(Delegate<void(float)>&& callback) noexcept;

    bool SupportsKeyboardFocus() const noexcept override;
    bool OnKeyEvent(uint32 userId, const input::InputEvent& event) noexcept override;
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
    static float ThumbExtent(Vec2 size) noexcept;
    static float TrackPosition(float value, float minimum, float maximum, Vec2 size) noexcept;

private:
    Delegate<void(float)> onChanged_;
    LinearColor fillColor_{0.0f, 0.45f, 0.85f, 1};
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

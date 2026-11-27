#include <UiFramework/Application/InputAdapter.h>

namespace rocks::ui {

InputAdapter::InputAdapter(Application& application) noexcept : application_(application) {
}

input::MouseCursor InputAdapter::MouseCursor() const noexcept {
    return hasPosition_ ? application_.QueryMouseCursor(position_, mouseUserId_)
                        : input::MouseCursor::Arrow;
}

Result<bool> InputAdapter::Route(const input::InputEvent& event) noexcept {
    return RouteWithin(event, {});
}

Result<bool> InputAdapter::RouteWithin(const input::InputEvent& event,
                                       const SharedPtr<Element>& scope) noexcept {
    const auto pointer = [&](PointerEvent value) noexcept {
        value.userId = event.userId;
        return application_.RoutePointerWithin(value, scope);
    };
    // 터치는 Input의 손가락 번호(touchIndex)를 pointer 번호로 쓴다. OS 취소(cancelled)는 Cancel이다
    // (Docs/Design/TouchInput.ko.md 3.1절).
    if (event.type == input::InputEventType::TouchBegan ||
        event.type == input::InputEventType::TouchMoved ||
        event.type == input::InputEventType::TouchEnded) {
        PointerEventType type = PointerEventType::Move;
        if (event.type == input::InputEventType::TouchBegan) {
            type = PointerEventType::Down;
        } else if (event.type == input::InputEventType::TouchEnded) {
            type = event.cancelled ? PointerEventType::Cancel : PointerEventType::Up;
        }

        return pointer(
            {type, Vec2(event.x, event.y), event.touchIndex, event.userId, PointerSource::Touch});
    }

    if (event.type == input::InputEventType::GamepadState) {
        if (event.gamepadIndex >= input::MaxGamepads) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }

        auto& previous = gamepadButtons_[event.gamepadIndex];
        const uint32 buttons = event.gamepad.connected ? event.gamepad.buttons : 0;
        const uint32 pressed = buttons & ~previous;
        previous = buttons;
        return Result<bool>::Success(
            pressed && application_.RouteGamepadButtons(event.userId, pressed, scope));
    }

    if (event.type == input::InputEventType::MouseLeave) {
        hasPosition_ = false;
        application_.ClearHover(event.userId);
        return Result<bool>::Success(false);
    }

    if (event.type == input::InputEventType::PointerCaptureLost) {
        application_.CancelPointers(PointerSource::Mouse);
        return Result<bool>::Success(false);
    }

    if (event.type == input::InputEventType::NonClientPressed) {
        application_.DismissPopups();
        return Result<bool>::Success(false);
    }

    if (event.type == input::InputEventType::FocusLost) {
        for (auto& buttons : gamepadButtons_) {
            buttons = 0;
        }

        hasPosition_ = false;
        application_.ResetInput();
        return Result<bool>::Success(false);
    }

    if (event.type == input::InputEventType::TextInputMethod) {
        // IME 기록 표지다(Docs/Design/UiToolkit 19.2절). 이 자리에서 창의 기록을 문맥에 적용해 앞뒤
        // 키·문자 이벤트와 순서를 지킨다(확정 "한" 뒤의 Left는 확정 뒤에 움직인다).
        return Result<bool>::Success(application_.ProcessTextInput(event.textInputRecord));
    }

    if (event.type == input::InputEventType::KeyDown ||
        event.type == input::InputEventType::KeyUp || event.type == input::InputEventType::Text) {
        // 방향키·Tab은 초점 경로의 기본 키 처리가 navigation으로
        // 바꾼다(Docs/Design/UiToolkit 20.7절 — 처리 안 된 키에 대체 navigation은 없다, UE와 같다).
        auto routed = application_.RouteKeyWithin(event.userId, event, scope);
        if (!routed || routed.Value() || event.type != input::InputEventType::KeyDown) {
            return routed;
        }

        // 닫힌 툴팁을 키보드로 다시 연다(UE에 없음 — 9.1절 UW4t).
        if (event.key == input::Key::F1) {
            return Result<bool>::Success(application_.IsControlDown(event.userId) &&
                                         application_.ShowFocusToolTip(event.userId));
        }

        return routed;
    }

    if (event.type == input::InputEventType::MouseMove) {
        mouseUserId_ = event.userId;
        position_ = Vec2(event.x, event.y);
        hasPosition_ = true;
        return pointer({PointerEventType::Move, position_});
    }

    if (event.type == input::InputEventType::MouseWheel && event.hasPointerPosition) {
        mouseUserId_ = event.userId;
        position_ = Vec2(event.x, event.y);
        hasPosition_ = true;
    }

    if (event.type == input::InputEventType::MouseWheel && hasPosition_) {
        PointerEvent wheel{PointerEventType::Wheel, position_};
        wheel.wheel = event.wheel;
        return pointer(wheel);
    }

    if (event.type == input::InputEventType::MouseDown ||
        event.type == input::InputEventType::MouseUp) {
        if (event.hasPointerPosition) {
            mouseUserId_ = event.userId;
            position_ = Vec2(event.x, event.y);
            hasPosition_ = true;
        }

        if (!hasPosition_) {
            return Result<bool>::Success(false);
        }

        const PointerEventType type = event.type == input::InputEventType::MouseDown
                                          ? PointerEventType::Down
                                          : PointerEventType::Up;
        PointerEvent press{type, position_, 0, 0, PointerSource::Mouse, event.mouseButton};
        press.clickCount = event.clickCount;
        return pointer(press);
    }

    return Result<bool>::Success(false);
}

Result<void> InputAdapter::RouteBatch(Span<const input::InputEvent> events, Span<uint8> consumed,
                                      uint64 droppedCount) noexcept {
    if (events.Size() != consumed.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (droppedCount != droppedCount_) {
        // 배치를 버려도 IME 기록은 처리한다 — 가장 큰 표지 하나가 앞 기록을 모두 처리한다(19.2절).
        for (usize i = events.Size(); i > 0; --i) {
            if (events[i - 1].type == input::InputEventType::TextInputMethod) {
                (void)application_.ProcessTextInput(events[i - 1].textInputRecord);
                break;
            }
        }

        droppedCount_ = droppedCount;
        for (auto& buttons : gamepadButtons_) {
            buttons = 0;
        }

        hasPosition_ = false;
        application_.ResetInput();
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    for (usize i = 0; i < events.Size(); ++i) {
        auto routed = Route(events[i]);
        if (!routed) {
            return Result<void>::Failure(routed.Error());
        }

        consumed[i] = routed.Value() ? 1 : 0;
    }

    return Result<void>::Success();
}

} // namespace rocks::ui

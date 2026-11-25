#include <UiFramework/InputAdapter.h>

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
    if (event.type == input::InputEventType::TouchBegin ||
        event.type == input::InputEventType::TouchMove ||
        event.type == input::InputEventType::TouchEnd ||
        event.type == input::InputEventType::TouchCancel) {
        PointerEventType type = PointerEventType::Cancel;
        if (event.type == input::InputEventType::TouchBegin) {
            type = PointerEventType::Down;
        } else if (event.type == input::InputEventType::TouchMove) {
            type = PointerEventType::Move;
        } else if (event.type == input::InputEventType::TouchEnd) {
            type = PointerEventType::Up;
        }
        return pointer(
            {type, Vec2(event.x, event.y), event.pointerId, event.userId, PointerSource::Touch});
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
        hasPosition_=false;
        application_.ClearHover(event.userId);
        return Result<bool>::Success(false);
    }
    if (event.type == input::InputEventType::PointerCaptureLost) {
        application_.CancelPointers(PointerSource::Mouse);
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

    if (event.type == input::InputEventType::KeyDown ||
        event.type == input::InputEventType::KeyUp || event.type == input::InputEventType::Text ||
        event.type == input::InputEventType::TextComposition ||
        event.type == input::InputEventType::TextCompositionCommit) {
        auto routed = application_.RouteKeyWithin(event.userId, event, scope);
        if (!routed || routed.Value() || event.type != input::InputEventType::KeyDown) {
            return routed;
        }
        switch (event.key) {
        case input::Key::Left:
            return application_.Navigate(event.userId, FocusDirection::Left, scope);
        case input::Key::Right:
            return application_.Navigate(event.userId, FocusDirection::Right, scope);
        case input::Key::Up:
            return application_.Navigate(event.userId, FocusDirection::Up, scope);
        case input::Key::Down:
            return application_.Navigate(event.userId, FocusDirection::Down, scope);
        case input::Key::Tab: {
            const bool forward = !application_.IsShiftDown(event.userId);
            return scope ? application_.MoveFocusWithin(event.userId, scope, forward, true)
                         : application_.MoveFocus(event.userId, forward);
        }
        default:
            return routed;
        }
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
        return pointer({type, position_, 0, 0, PointerSource::Mouse, event.mouseButton});
    }

    return Result<bool>::Success(false);
}

Result<void> InputAdapter::RouteBatch(Span<const input::InputEvent> events, Span<uint8> consumed,
                                      uint64 droppedCount) noexcept {
    if (events.Size() != consumed.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (droppedCount != droppedCount_) {
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

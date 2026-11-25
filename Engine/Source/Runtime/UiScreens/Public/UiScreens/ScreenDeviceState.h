#pragma once
#include <Core/Delegate.h>
namespace rocks::ui {

enum class ScreenInputDevice : uint8 {
    KeyboardMouse,
    Gamepad,
    Touch
};

// 한 플레이어의 마지막 입력 장치를 공유한다. 라우터를 닫은 뒤에도 마지막 상태를 안전하게 읽는다.
class ROCKS_UISCREENS_API ScreenDeviceState final {
public:
    ScreenDeviceState(Allocator& allocator, ScreenInputDevice initial) noexcept;
    ScreenInputDevice Device() const noexcept;
    MulticastDelegate<void(ScreenInputDevice)>& OnChanged() noexcept;

private:
    friend class ScreenInputRouter;
    void SetDevice(ScreenInputDevice value) noexcept;
    ScreenInputDevice device_;
    MulticastDelegate<void(ScreenInputDevice)> changed_;
};
} // namespace rocks::ui

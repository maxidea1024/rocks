#pragma once
#include <Core/Delegate.h>

namespace rocks::ui {

enum class ScreenInputDevice : uint8 {
    KeyboardMouse,
    Gamepad,
    Touch
};

// 입력 장치 종류 수와 모든 장치를 켠 마스크다(장치마다 bit 하나).
inline constexpr uint8 ScreenInputDeviceCount = static_cast<uint8>(ScreenInputDevice::Touch) + 1;
inline constexpr uint8 AllScreenInputDevices = (1u << ScreenInputDeviceCount) - 1;

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

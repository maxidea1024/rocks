#pragma once

#include <Core/Delegate.h>
#include <UiScreens/ScreenDeviceState.h>
#include <UiFramework/InputAdapter.h>
#include <UiScreens/ScreenContainer.h>

namespace rocks::ui {

struct ScreenActionMapping {
    Guid domain{};
    Guid action{};
    input::Key key = input::Key::None;
    input::GamepadButton button = input::GamepadButton::Count;
    int32 priority = 0;
};

// 플레이어에게 할당된 이벤트만 전달한다. true는 게임 뷰포트가 같은 입력을 다시 처리하지 않는다는
// 뜻이다.
class ROCKS_UISCREENS_API ScreenInputRouter {
public:
    ScreenInputRouter(Allocator& allocator, Application& application,
                      ScreenContainer& screens) noexcept;
    Result<void> AddMapping(ScreenActionMapping mapping) noexcept;
    Result<void> RemoveMapping(Guid domain, Guid action) noexcept;
    Span<const ScreenActionMapping> Mappings() const noexcept;
    Result<bool> Route(const input::InputEvent& event) noexcept;
    Result<SharedPtr<ScreenDeviceState>> ObserveDevice() noexcept;
    ScreenInputDevice Device() const noexcept;
    void SetOnDeviceChanged(Delegate<void(ScreenInputDevice)>&& callback) noexcept;

private:
    void Observe(ScreenInputDevice device) noexcept;
    Result<bool> Dispatch(input::Key key, uint32 pressed) noexcept;
    Result<bool> Gamepad(const input::InputEvent& event, bool allowUi) noexcept;

    Application& app_;
    ScreenContainer& screens_;
    InputAdapter adapter_;
    Array<ScreenActionMapping> mappings_;
    input::GamepadSnapshot pads_[input::MaxGamepads]{};
    WeakPtr<Element> acceptTargets_[input::MaxGamepads]{};
    SharedPtr<ScreenDeviceState> deviceState_;
    ScreenInputDevice device_ = ScreenInputDevice::KeyboardMouse;
    Delegate<void(ScreenInputDevice)> deviceChanged_;
    bool routing_ = false;
};

} // namespace rocks::ui

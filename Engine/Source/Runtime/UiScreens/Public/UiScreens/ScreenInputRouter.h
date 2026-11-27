#pragma once

#include <Core/Delegate.h>
#include <Core/Optional.h>
#include <UiScreens/ScreenDeviceState.h>
#include <UiScreens/ScreenInputHost.h>
#include <UiFramework/Application/InputAdapter.h>
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
    // 활성 화면의 입력 설정을 적용할 호스트다(Docs/Design/UiToolkit 21.3절). host는 라우터보다 오래
    // 살아야 한다. nullptr이면 적용하지 않는다. 바꾸면 다음 적용이 처음부터(이전 설정 없음) 한다.
    void SetInputHost(ScreenInputHost* host) noexcept;
    // 활성 화면의 설정이 마지막 적용과 다르면 호스트에 적용한다(UE ApplyUIInputConfig —
    // CommonUIActionRouterBase.cpp 1866행). Route가 맨 앞에서 부르고, 화면을 바꾼 프레임에는
    // 호스트가 입력 전에 한 번 부른다. 적용했으면 true다.
    bool ApplyInputConfig() noexcept;

private:
    void Observe(ScreenInputDevice device) noexcept;
    Result<bool> Dispatch(input::Key key, uint32 pressed) noexcept;
    Result<bool> Gamepad(const input::InputEvent& event, bool allowUi) noexcept;

    Application& app_;
    ScreenContainer& screens_;
    InputAdapter adapter_;
    Array<ScreenActionMapping> mappings_;
    input::GamepadSnapshot pads_[input::MaxGamepads]{};
    SharedPtr<ScreenDeviceState> deviceState_;
    ScreenInputDevice device_ = ScreenInputDevice::KeyboardMouse;
    Delegate<void(ScreenInputDevice)> deviceChanged_;
    ScreenInputHost* inputHost_ = nullptr;
    // 마지막으로 호스트에 적용한 설정이다(없으면 아직 적용 전).
    Optional<ScreenInputConfig> appliedConfig_;
    bool routing_ = false;
};

} // namespace rocks::ui

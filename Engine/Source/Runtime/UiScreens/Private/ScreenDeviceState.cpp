#include <UiScreens/ScreenDeviceState.h>
namespace rocks::ui {
ScreenDeviceState::ScreenDeviceState(Allocator& allocator, ScreenInputDevice initial) noexcept
    : device_(initial), changed_(allocator) {
}
ScreenInputDevice ScreenDeviceState::Device() const noexcept {
    return device_;
}
MulticastDelegate<void(ScreenInputDevice)>& ScreenDeviceState::OnChanged() noexcept {
    return changed_;
}
void ScreenDeviceState::SetDevice(ScreenInputDevice value) noexcept {
    device_ = value;
    changed_.Broadcast(value);
}
} // namespace rocks::ui

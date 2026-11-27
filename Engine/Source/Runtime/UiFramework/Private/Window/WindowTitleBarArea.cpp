#include <UiFramework/Window/WindowTitleBarArea.h>

namespace rocks::ui {

namespace {

// 두 번 누르기의 누름 횟수다.
constexpr uint8 TitleBarDoubleClick = 2;

} // namespace

WindowTitleBarArea::WindowTitleBarArea(Allocator& allocator) noexcept : Frame(allocator) {
    (void)SetBrush({.mode = BrushMode::None});
}

void WindowTitleBarArea::SetDoubleClickTogglesFullscreen(bool enabled) noexcept {
    toggles_ = enabled;
}

void WindowTitleBarArea::SetFullscreen(bool fullscreen) noexcept {
    fullscreen_ = fullscreen;
}

void WindowTitleBarArea::SetOnRequestToggleFullscreen(Delegate<void()>&& request) noexcept {
    requestToggle_ = Move(request);
}

input::WindowRegion WindowTitleBarArea::GetWindowRegion() const noexcept {
    // 전체 화면에서는 끌어 옮기지 않도록 Client로 답한다(UE GetWindowZoneOverride).
    return fullscreen_ ? input::WindowRegion::Client : input::WindowRegion::Caption;
}

PointerReply WindowTitleBarArea::OnPointerEvent(const PointerEvent& event) noexcept {
    // 창 모드의 두 번 누르기는 OS가 제목 줄로 처리한다(최대화). 전체 화면에서만 여기서 받는다.
    if (event.type == PointerEventType::Down && event.button == input::MouseButton::Left &&
        event.clickCount >= TitleBarDoubleClick && fullscreen_ && toggles_ &&
        requestToggle_.IsBound()) {
        requestToggle_.Execute();
        return {.handled = true};
    }

    return Frame::OnPointerEvent(event);
}

} // namespace rocks::ui

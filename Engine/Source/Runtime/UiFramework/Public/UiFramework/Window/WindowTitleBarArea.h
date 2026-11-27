#pragma once
#include <UiFramework/Layout/Frame.h>

namespace rocks::ui {

// 클라이언트 장식 창에서 이동할 수 있는 제목 영역이다. 실제 이동 루프와 창 스냅은 플랫폼이
// 처리한다. 창이 전체 화면이면 Client로 답해 끌어 옮기지 않고, 두 번 누르기를 직접 받아 전체 화면
// 전환 요청을 부른다(UE SWindowTitleBarArea — Docs/Design/UiToolkit 19.2절).
class ROCKS_UIFRAMEWORK_API WindowTitleBarArea final : public Frame {
    ROCKS_UI_ELEMENT(WindowTitleBarArea, Frame)

public:
    explicit WindowTitleBarArea(Allocator& allocator) noexcept;
    // 전체 화면에서 두 번 누르기가 전환 요청을 부를지다(UE bDoubleClickTogglesFullscreen).
    void SetDoubleClickTogglesFullscreen(bool enabled) noexcept;
    // 창이 지금 전체 화면인지다(호스트가 창 모드를 따라 알린다).
    void SetFullscreen(bool fullscreen) noexcept;
    // 전체 화면 전환 요청이다(UE RequestToggleFullscreen). 호스트가 창 모드를 바꾼다.
    void SetOnRequestToggleFullscreen(Delegate<void()>&& request) noexcept;
    input::WindowRegion GetWindowRegion() const noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;

private:
    Delegate<void()> requestToggle_;
    bool toggles_ = false;
    bool fullscreen_ = false;
};
} // namespace rocks::ui

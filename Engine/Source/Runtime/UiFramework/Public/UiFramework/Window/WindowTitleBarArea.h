#pragma once
#include <UiFramework/Layout/Frame.h>

namespace rocks::ui {

// 클라이언트 장식 창에서 이동할 수 있는 제목 영역이다. 실제 이동 루프와 창 스냅은 플랫폼이
// 처리한다.
class ROCKS_UIFRAMEWORK_API WindowTitleBarArea final : public Frame {
public:
    explicit WindowTitleBarArea(Allocator& allocator) noexcept;
    void SetDoubleClickTogglesFullscreen(bool enabled) noexcept;
    input::WindowRegion GetWindowRegion() const noexcept override;

private:
    bool fullscreen_ = false;
};
} // namespace rocks::ui

#pragma once

#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Controls/Button.h>

namespace rocks::ui {

// 창 버튼의 역할이다.
enum class WindowButtonRole : uint8 {
    Minimize,
    Maximize,
    Close
};

// 직접 그린 창의 최소화·최대화(복원)·닫기 버튼이다(UE SWindowTitleBar의 버튼 셋 —
// Docs/Design/UiToolkit 9.1절 UW7b). 창 영역 질의에 자기 역할의 영역(MinimizeButton 등)을 돌려줘
// 창 계층이 OS 판정(Win11 스냅 레이아웃 등)에 쓴다. 키보드 초점은 받지 않는다(UE IsFocusable
// false). 누름은 Button처럼 onClicked로 알린다 — 창 동작은 호출자가 Window에 건다.
//
// 사용 예:
//   auto close = SharedPtr<WindowControlButton>::TryMake(allocator, allocator,
//                                                         WindowButtonRole::Close);
//   close.Value()->SetOnClicked(Delegate<void()>::CreateLambda([&] { window.RequestClose(); }));
class ROCKS_UIFRAMEWORK_API WindowControlButton final : public Button {
    ROCKS_UI_ELEMENT(WindowControlButton, Button)

public:
    WindowControlButton(Allocator& allocator, WindowButtonRole role,
                        const WindowStyle& style = DefaultWindowStyle()) noexcept;

    WindowButtonRole Role() const noexcept;
    // 최대화 버튼의 아이콘을 복원 모양으로 바꾼다(창이 최대화되어 있는지).
    void SetMaximized(bool maximized) noexcept;
    bool IsMaximized() const noexcept;
    // 창이 전체 화면이면 최대화 버튼을 "전체 화면 나가기" 아이콘으로 바꾼다(UE SWindowTitleBar
    // 501–520행).
    void SetFullscreen(bool fullscreen) noexcept;
    bool IsFullscreen() const noexcept;

    input::WindowRegion GetWindowRegion() const noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    VectorImageKey CurrentIcon() const noexcept;

    WindowStyle style_;
    WindowButtonRole role_;
    bool maximized_ = false;
    bool fullscreen_ = false;
};

} // namespace rocks::ui

#pragma once

#include <Core/Delegate.h>
#include <UiFramework/TextLabel.h>
#include <UiFramework/WindowAppIcon.h>
#include <UiFramework/WindowControlButton.h>

namespace rocks::ui {

// 직접 그린 창의 제목 막대다(UE SWindowTitleBar — SWindowTitleBar.cpp 186–360행, Docs/Design/
// UiToolkit 9.1절 UW7b). 왼쪽에 앱 아이콘(누르면 시스템 메뉴)과 왼쪽 내용(주 메뉴 막대 자리),
// 가운데에 제목, 오른쪽 위에 최소화·최대화(복원)·닫기를 둔다. 막대의 빈 곳은 창 영역 Caption이라
// 끌면 창이 옮겨지고 더블클릭하면 최대화가 바뀐다(OS 처리). 제목은 막대 전체 폭의 가운데에 두고,
// 왼쪽 내용·버튼과 겹치면 그 사이로 줄인다.
//
// 사용 예:
//   auto bar = SharedPtr<WindowTitleBar>::TryMake(allocator, allocator);
//   bar.Value()->Construct({.title = shapedTitle,
//                           .onMinimize = ..., .onToggleMaximize = ..., .onClose = ...});
class ROCKS_UIFRAMEWORK_API WindowTitleBar final : public Element {
public:
    struct Args {
        // 호출자가 style.title.font로 shaping한 제목이다. 없으면 제목을 비운다.
        SharedPtr<TextLabel> title{};
        // 앱 아이콘 오른쪽에 둘 내용이다(주 메뉴 막대 — UW7c).
        SharedPtr<Element> leftContent{};
        bool showAppIcon = true;
        // 창이 최소화·최대화를 허용하는지다(UE HasMinimizeBox·HasMaximizeBox). 아니면 버튼을 끈다.
        bool canMinimize = true;
        bool canMaximize = true;
        // 창 버튼 툴팁 글자를 shaping한다("Minimize"·"Maximize"/"Restore Down"·"Close" — UE
        // SWindowTitleBar.cpp 149·165·181행). 비우면 툴팁이 없다.
        Delegate<Result<SharedPtr<TextLabel>>(StringView, const FontStyle&)> makeLabel{};
        Delegate<void()> onMinimize{};
        Delegate<void()> onToggleMaximize{};
        Delegate<void()> onClose{};
    };

    explicit WindowTitleBar(Allocator& allocator,
                            const WindowStyle& style = DefaultWindowStyle()) noexcept;
    Result<void> Construct(Args&& args) noexcept;

    // 창이 최대화되어 있으면 최대화 버튼을 복원 모양으로 바꾼다.
    void SetMaximized(bool maximized) noexcept;
    // 전체 화면이면 최대화 버튼을 "전체 화면 나가기"로 바꾼다(누르면 onToggleMaximize — 호출자가
    // 전체 화면을 끈다).
    void SetFullscreen(bool fullscreen) noexcept;
    // 비활성 창은 제목과 앱 아이콘을 흐리게 그린다(InactiveWindowTitleOpacity).
    void SetActive(bool active) noexcept;
    bool IsActive() const noexcept;
    // 제목을 바꾼다(창 제목이 바뀔 때 — UE SWindowTitleBar는 창 GetTitle에 묶인다). 비우면 제목이
    // 없다.
    Result<void> SetTitle(SharedPtr<TextLabel> title) noexcept;

    // 제목 높이 titleHeight일 때 막대의 높이다(떠 있는 창의 크기를 내용 크기에서 정할 때 — UE
    // SWindow::GetWindowSizeFromClientSize).
    static float DesiredHeight(const WindowStyle& style, float titleHeight) noexcept;
    const SharedPtr<WindowControlButton>& ControlButton(WindowButtonRole role) const noexcept;

    input::WindowRegion GetWindowRegion() const noexcept override;
    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    Result<void> AttachToolTip(WindowControlButton& button, WindowButtonRole role) noexcept;

    Allocator& allocator_;
    WindowStyle style_;
    SharedPtr<WindowAppIcon> icon_;
    SharedPtr<Element> left_;
    SharedPtr<TextLabel> title_;
    SharedPtr<WindowControlButton> buttons_[3];
    Delegate<Result<SharedPtr<TextLabel>>(StringView, const FontStyle&)> makeLabel_;
    bool active_ = true;
};

} // namespace rocks::ui

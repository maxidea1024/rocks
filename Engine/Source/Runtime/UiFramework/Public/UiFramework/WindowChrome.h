#pragma once

#include <UiFramework/WindowTitleBar.h>

namespace rocks::ui {

// 직접 그린 창의 테두리다(UE SWindow의 바탕·틀·외곽선과 제목 영역 — SWindow.cpp 575–640행,
// Docs/Design/UiToolkit 9.1절 UW7b). 위에 제목 막대, 그 아래에 내용을 두고, 창이 최대화되지 않았을
// 때만 틀(2 px)·외곽선(1 px)과 여백(3)을 둔다. 창의 루트로 쓰고 매 프레임 창 상태(최대화)를
// SetMaximized로 알린다.
class ROCKS_UIFRAMEWORK_API WindowChrome final : public Element {
public:
    struct Args {
        SharedPtr<WindowTitleBar> titleBar{};
        SharedPtr<Element> content{};
    };

    explicit WindowChrome(Allocator& allocator,
                          const WindowStyle& style = DefaultWindowStyle()) noexcept;
    Result<void> Construct(Args&& args) noexcept;

    void SetMaximized(bool maximized) noexcept;
    bool IsMaximized() const noexcept;
    // 창 활성 상태를 제목 막대에 알린다(비활성이면 제목을 흐리게).
    void SetActive(bool active) noexcept;
    const SharedPtr<WindowTitleBar>& TitleBar() const noexcept;
    const SharedPtr<Element>& Content() const noexcept;

    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    Vec4 Inset() const noexcept;

    WindowStyle style_;
    SharedPtr<WindowTitleBar> titleBar_;
    SharedPtr<Element> content_;
    bool maximized_ = false;
};

} // namespace rocks::ui

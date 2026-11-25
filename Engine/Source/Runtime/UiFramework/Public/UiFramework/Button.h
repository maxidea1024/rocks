#pragma once

#include <Core/Delegate.h>
#include <Core/Optional.h>
#include <UiFramework/Frame.h>

namespace rocks::ui {

// 포인터를 눌렀다 버튼 안에서 놓으면 클릭을 통지한다.
enum class ButtonClickMethod : uint8 {
    DownAndUp,
    Down,
    Up,
    Precise
};
enum class ButtonPressMethod : uint8 {
    DownAndUp,
    Down,
    Up
};

class ROCKS_UIFRAMEWORK_API Button : public Frame {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    using Super = Frame;
    struct Args {
        ElementChild content{};
        Bindable<LinearColor> color{LinearColor{1, 1, 1, 1}};
        LinearColor pressedColor{0.8f, 0.8f, 0.8f, 1};
        Vec4 padding{};
        ElementArgs common{};
        Delegate<void()> onClicked{};
        ButtonClickMethod clickMethod = ButtonClickMethod::DownAndUp;
        ButtonClickMethod touchMethod = ButtonClickMethod::DownAndUp;
        ButtonPressMethod pressMethod = ButtonPressMethod::DownAndUp;
        bool focusable = true;
        Optional<LinearColor> hoveredColor{};
        Delegate<void()> onPressed{}, onReleased{}, onHovered{}, onUnhovered{};
    };

    // content에 레이블 또는 아이콘 트리를 넣고 onClicked에 동작을 연결한다.
    // 마우스 클릭과 포커스 상태의 키보드 활성화는 같은 콜백을 실행한다.
    Result<void> Construct(Args&& args) noexcept;

    explicit Button(Allocator& allocator) noexcept;

    void SetOnClicked(Delegate<void()>&& callback) noexcept;

    // 눌림과 놓임은 클릭 성립과 별개다. 포커스나 캡처를 잃어도 눌린 상태는 해제한다.
    void SetOnPressed(Delegate<void()>&& callback) noexcept;
    void SetOnReleased(Delegate<void()>&& callback) noexcept;
    void SetOnHovered(Delegate<void()>&& callback) noexcept;
    void SetOnUnhovered(Delegate<void()>&& callback) noexcept;
    Result<void> SetInputMethods(ButtonClickMethod click, ButtonClickMethod touch,
                                 ButtonPressMethod press) noexcept;
    void SetFocusable(bool enabled) noexcept;
    // 기본 브러시는 Frame에서 관리한다. 상태별 브러시를 생략하면 기본 브러시를 쓴다.
    Result<void> SetStateBrushes(Optional<Brush> hovered, Optional<Brush> pressed) noexcept;
    void SetHoveredColor(LinearColor color) noexcept;
    bool IsHovered() const noexcept;
    void OnMouseEnter(uint32 user) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
    void SetPressedColor(LinearColor color) noexcept;

    bool IsPressed() const noexcept;

    bool SupportsKeyboardFocus() const noexcept override;

    bool OnKeyEvent(uint32 userId, const input::InputEvent& event) noexcept override;

    void OnKeyCancelled(uint32 userId, input::Key key) noexcept override;
    void OnFocusLost(uint32 userId) noexcept override;

    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;

    void OnPointerCaptureLost(PointerSource source, uint32 userId,
                              uint32 pointerId) noexcept override;

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

protected:
    LinearColor CurrentColor() const noexcept;

    // 입력 방식과 무관하게 활성화될 때 호출한다.
    virtual void OnActivated() noexcept;

private:
    void ReleasePointer() noexcept;
    void Activate() noexcept;

    Delegate<void()> onClicked_, onPressed_, onReleased_, onHovered_, onUnhovered_;
    ButtonClickMethod clickMethod_ = ButtonClickMethod::DownAndUp,
                      touchMethod_ = ButtonClickMethod::DownAndUp;
    ButtonPressMethod pressMethod_ = ButtonPressMethod::DownAndUp;
    ButtonClickMethod activeMethod_ = ButtonClickMethod::DownAndUp;
    ButtonPressMethod activePress_ = ButtonPressMethod::DownAndUp;
    Vec2 pressOrigin_{};
    uint32 hoverCount_ = 0;
    bool focusable_ = true;
    Optional<Brush> hoveredBrush_, pressedBrush_;
    bool hasHoveredColor_ = false;
    LinearColor hoveredColor_{1, 1, 1, 1};
    LinearColor pressedColor_{0.8f, 0.8f, 0.8f, 1.0f};
    PointerSource pointerSource_ = PointerSource::Mouse;
    uint32 userId_ = 0;
    uint32 pointerId_ = 0;
    uint32 keyUserId_ = 0;
    input::Key pressedKey_ = input::Key::None;
    bool pressed_ = false;
    bool keyPressed_ = false;
};

} // namespace rocks::ui

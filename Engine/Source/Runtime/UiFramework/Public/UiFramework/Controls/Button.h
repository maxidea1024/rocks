#pragma once

#include <Core/Delegate.h>
#include <Core/Optional.h>
#include <UiCore/Style.h>
#include <UiFramework/Layout/Frame.h>

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

// 상태별 모양은 ButtonStyle이 정한다(UE SButton·FButtonStyle, UiToolkit 9.1절 UW3c). 기본은 UE 5.8
// 에디터 핵심 스타일 "Button"이다.
//
// 사용 예:
//   Make<Button>({.content = Label("Apply"), .style = PrimaryButtonStyle(), .onClicked = apply});
class ROCKS_UIFRAMEWORK_API Button : public Frame {
    ROCKS_UI_ELEMENT(Button, Frame)

public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    // 바탕이 강조색(Primary 계열)이면 흰 외곽선을 쓴다.
    const StyleBrush* FocusOutlineBrush() const noexcept override;
    struct Args {
        ElementChild content{};
        // 값을 복사한다. 상태별 brush·여백·전경색을 담는다. 비우면 활성 앱 스타일의
        // "Button"이다(20.11절).
        Optional<ButtonStyle> style{};
        // brush 색에 곱한다(UE ButtonColorAndOpacity). 기본 흰색은 스타일 색 그대로다.
        Bindable<LinearColor> color{LinearColor{1, 1, 1, 1}};
        // 자식 전경색이다. 없으면 스타일의 상태별 전경색이다(UE ForegroundColor = UseStyle).
        Optional<ColorRef> foregroundColor{};
        // 스타일의 보통·눌림 여백에 더한다(UE ContentPadding·GetCombinedPadding).
        Vec4 contentPadding{};
        // 여백 안 내용 정렬이다(UE SButton HAlign·VAlign, 기본 Fill). 비우면 지금 정렬을 둔다.
        Optional<StackAlignment> horizontalAlignment{};
        Optional<StackAlignment> verticalAlignment{};
        ElementArgs common{};
        Delegate<void()> onClicked{};
        ButtonClickMethod clickMethod = ButtonClickMethod::DownAndUp;
        ButtonClickMethod touchMethod = ButtonClickMethod::DownAndUp;
        ButtonPressMethod pressMethod = ButtonPressMethod::DownAndUp;
        bool focusable = true;
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
    // 여백이 음수·비유한이거나 brush·색이 그릴 수 없는 값이면 InvalidArgument다.
    // 명시 스타일이다. 부르지 않으면 활성 앱 스타일의 "Button"(없으면
    // DefaultButtonStyle)다(20.11절).
    Result<void> SetStyle(const ButtonStyle& style) noexcept;
    static bool IsValidStyle(const ButtonStyle& style) noexcept;
    const ButtonStyle& GetStyle() const noexcept;
    Result<void> SetContentPadding(Vec4 padding) noexcept;
    Vec4 GetContentPadding() const noexcept;
    bool IsHovered() const noexcept;
    // 올림(hovered) 모양으로 그릴지다. 기본은 IsHovered()이고, 메뉴 항목처럼 하위 메뉴가 열린 동안
    // 강조를 유지해야 하는 버튼이 넓힌다.
    virtual bool IsHighlighted() const noexcept;
    void OnMouseEnter(uint32 user) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
    bool IsPressed() const noexcept;

    // SetForegroundColor로 정하지 않았으면(ColorSource::Style) 상태별 스타일 전경색이다.
    ColorRef GetForegroundColor() const noexcept override;

    bool SupportsKeyboardFocus() const noexcept override;

    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;

    void OnKeyCancelled(uint32 userId, input::Key key) noexcept override;
    void OnFocusLost(uint32 userId) noexcept override;

    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;

    void OnPointerCaptureLost(PointerSource source, uint32 userId,
                              uint32 pointerId) noexcept override;

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

protected:
    LinearColor CurrentColor() const noexcept;

    // 지금 상태(비활성·눌림·올림·보통)의 스타일 brush다. disabled는 이 버튼이나 조상이
    // 비활성인지다.
    const StyleBrush& CurrentStyleBrush(bool disabled) const noexcept;
    ColorRef PaintForegroundColor(bool disabled) const noexcept override;

    // 입력 방식과 무관하게 활성화될 때 호출한다.
    virtual void OnActivated() noexcept;

private:
    // 스타일을 바꾼다(명시 표시는 건드리지 않는다).
    Result<void> ApplyStyle(const ButtonStyle& style) noexcept;
    void ApplyStyles(const UiHostServices& host) noexcept override;
    bool styleExplicit_ = false;
    void ReleasePointer() noexcept;
    // 눌림 상태가 바뀌면 스타일 여백 + 내용 여백을 다시 정한다(UE UpdatePadding).
    void UpdatePadding() noexcept;
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
    ButtonStyle style_ = DefaultButtonStyle();
    Vec4 contentPadding_{};
    PointerSource pointerSource_ = PointerSource::Mouse;
    uint32 userId_ = 0;
    uint32 pointerId_ = 0;
    uint32 keyUserId_ = 0;
    input::Key pressedKey_ = input::Key::None;
    bool pressed_ = false;
    bool keyPressed_ = false;
};

} // namespace rocks::ui

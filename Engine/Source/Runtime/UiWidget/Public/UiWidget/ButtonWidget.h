#pragma once

#include <Core/Delegate.h>
#include <UiCore/Styles/GameWidgetStyles.h>
#include <UiFramework/Controls/Button.h>
#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 한 자식을 클릭 가능한 영역으로 감싼다. 마우스와 키보드 활성화를 SetOnClicked에 연결한다.
// 모양은 ButtonStyle(UE UButton::WidgetStyle — 기본은 게임 위젯 스타일 "Button", UE
// DefaultStyleCache·UMGCoreStyle, Docs/Design/UiToolkit 20.11절)이 정하고,
// backgroundColor는 brush 색에 곱한다(UE BackgroundColor). 스타일의 리플렉션 직렬화는
// UiToolkit 9.1절 UW9에서 한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ButtonWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit ButtonWidget(Allocator& allocator) noexcept;
    // 마우스·터치·키보드 입력별로 클릭을 확정하는 시점을 선택한다.
    Result<void> SetInputMethods(ButtonClickMethod click, ButtonClickMethod touch,
                                 ButtonPressMethod press) noexcept;
    void SetFocusable(bool enabled) noexcept;
    bool IsFocusable() const noexcept;
    bool IsPressed() const noexcept;
    bool IsHovered() const noexcept;
    // 잘못된 스타일은 InvalidArgument이고 값을 바꾸지 않는다(Button::SetStyle과 같은 검사).
    Result<void> SetStyle(const ButtonStyle& style) noexcept;
    const ButtonStyle& GetStyle() const noexcept;
    Result<void> SetBackgroundColor(LinearColor color) noexcept;
    LinearColor GetBackgroundColor() const noexcept;
    // 스타일 여백에 더하는 내용 여백이다(UE UButtonSlot::Padding → SButton ContentPadding).
    Result<void> SetPadding(Vec4 padding) noexcept;
    void SetOnPressed(Delegate<void()>&& callback) noexcept;
    void SetOnReleased(Delegate<void()>&& callback) noexcept;
    void SetOnHovered(Delegate<void()>&& callback) noexcept;
    void SetOnUnhovered(Delegate<void()>&& callback) noexcept;
    void SetOnClicked(Delegate<void()>&& callback) noexcept;

protected:
    void ApplyProperties(Element& element) noexcept override;
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "focusable") bool focusable_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "clickMethod") uint8 clickMethod_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "touchMethod") uint8 touchMethod_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "pressMethod") uint8 pressMethod_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "backgroundColor") LinearColor backgroundColor_ { 1, 1, 1, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "padding") Vec4 padding_ {};
    ROCKS_NATIVE() ButtonStyle style_ = GameButtonStyle();
    ROCKS_NATIVE() Delegate<void()> clicked_;
    ROCKS_NATIVE() Delegate<void()> pressed_;
    ROCKS_NATIVE() Delegate<void()> released_;
    ROCKS_NATIVE() Delegate<void()> hovered_;
    ROCKS_NATIVE() Delegate<void()> unhovered_;
};

} // namespace rocks::ui

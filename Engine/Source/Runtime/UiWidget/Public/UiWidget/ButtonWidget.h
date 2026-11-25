#pragma once

#include <Core/Delegate.h>
#include <UiFramework/Button.h>
#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 한 자식을 클릭 가능한 영역으로 감싼다. 마우스와 키보드 활성화를 SetOnClicked에 연결한다.
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
    Result<void> SetTints(LinearColor normal, LinearColor hovered, LinearColor pressed) noexcept;
    Result<void> SetStateBrushes(Optional<Brush> hovered, Optional<Brush> pressed) noexcept;
    Result<void> SetBrush(const Brush& brush) noexcept;
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
    ROCKS_PROPERTY(ReadOnly, Name = "normalTint") LinearColor normalTint_ { 1, 1, 1, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "hoveredTint") LinearColor hoveredTint_ { 1, 1, 1, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "pressedTint") LinearColor pressedTint_ { 0.8f, 0.8f, 0.8f, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "padding") Vec4 padding_ {};
    ROCKS_NATIVE() Optional<Brush> hoveredBrush_;
    ROCKS_NATIVE() Optional<Brush> pressedBrush_;
    ROCKS_NATIVE() Brush brush_ {};
    ROCKS_NATIVE() Delegate<void()> clicked_;
    ROCKS_NATIVE() Delegate<void()> pressed_;
    ROCKS_NATIVE() Delegate<void()> released_;
    ROCKS_NATIVE() Delegate<void()> hovered_;
    ROCKS_NATIVE() Delegate<void()> unhovered_;
};

} // namespace rocks::ui

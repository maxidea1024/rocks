#include <UiWidget/ButtonWidgetSlot.generated.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiFramework/Controls/Button.h>

namespace rocks::ui {

TypeId ButtonWidget::SlotType() const noexcept {
    return GeneratedType<ButtonWidgetSlot>::Id();
}

ButtonWidget::ButtonWidget(Allocator& allocator) noexcept
    : ContentWidget(allocator), clicked_(allocator), pressed_(allocator), released_(allocator),
      hovered_(allocator), unhovered_(allocator) {
}

void ButtonWidget::SetFocusable(bool enabled) noexcept {
    focusable_ = enabled;
    if (auto live = CachedElement()) {
        StaticElementCast<Button>(*live).SetFocusable(enabled);
    }
}

bool ButtonWidget::IsFocusable() const noexcept {
    return focusable_;
}

bool ButtonWidget::IsPressed() const noexcept {
    auto live = CachedElement();
    return live && StaticElementCast<Button>(*live).IsPressed();
}

bool ButtonWidget::IsHovered() const noexcept {
    auto live = CachedElement();
    return live && StaticElementCast<Button>(*live).IsHovered();
}

Result<void> ButtonWidget::SetInputMethods(ButtonClickMethod click, ButtonClickMethod touch,
                                           ButtonPressMethod press) noexcept {
    if (click > ButtonClickMethod::Precise || touch > ButtonClickMethod::Precise ||
        press > ButtonPressMethod::Up) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    clickMethod_ = static_cast<uint8>(click);
    touchMethod_ = static_cast<uint8>(touch);
    pressMethod_ = static_cast<uint8>(press);
    if (auto live = CachedElement()) {
        return StaticElementCast<Button>(*live).SetInputMethods(click, touch, press);
    }

    return Result<void>::Success();
}

Result<void> ButtonWidget::SetStyle(const ButtonStyle& style) noexcept {
    if (!Button::IsValidStyle(style)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    style_ = style;
    if (auto live = CachedElement()) {
        return StaticElementCast<Button>(*live).SetStyle(style);
    }

    return Result<void>::Success();
}

const ButtonStyle& ButtonWidget::GetStyle() const noexcept {
    return style_;
}

Result<void> ButtonWidget::SetBackgroundColor(LinearColor color) noexcept {
    if (!color.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    backgroundColor_ = color;
    if (auto live = CachedElement()) {
        StaticElementCast<Button>(*live).SetColor(color);
    }

    return Result<void>::Success();
}

LinearColor ButtonWidget::GetBackgroundColor() const noexcept {
    return backgroundColor_;
}

Result<void> ButtonWidget::SetPadding(Vec4 padding) noexcept {
    if (!SlotPlacement{.padding = padding}.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    padding_ = padding;
    if (auto live = CachedElement()) {
        return StaticElementCast<Button>(*live).SetContentPadding(padding);
    }

    return Result<void>::Success();
}

void ButtonWidget::ApplyProperties(Element& element) noexcept {
    auto& button = StaticElementCast<Button>(element);
    button.SetColor(backgroundColor_);
    (void)button.SetStyle(style_);
    (void)button.SetContentPadding(padding_);
    button.SetFocusable(focusable_);
    (void)button.SetInputMethods(static_cast<ButtonClickMethod>(clickMethod_),
                                 static_cast<ButtonClickMethod>(touchMethod_),
                                 static_cast<ButtonPressMethod>(pressMethod_));
}

Event<>& ButtonWidget::OnPressed() noexcept {
    return pressed_;
}

Event<>& ButtonWidget::OnReleased() noexcept {
    return released_;
}

Event<>& ButtonWidget::OnHovered() noexcept {
    return hovered_;
}

Event<>& ButtonWidget::OnUnhovered() noexcept {
    return unhovered_;
}

Event<>& ButtonWidget::OnClicked() noexcept {
    return clicked_;
}

Result<SharedPtr<Element>> ButtonWidget::CreatePanel(Allocator& allocator) noexcept {
    auto button = SharedPtr<Button>::TryMake(allocator, allocator);
    if (!button) {
        return Result<SharedPtr<Element>>::Failure(button.Error());
    }

    return Result<SharedPtr<Element>>::Success(Move(button.Value()));
}

Result<SharedPtr<Element>> ButtonWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto element = PanelWidget::BuildElement(objects);
    if (!element) {
        return element;
    }

    auto* button = StaticElementCast<Button>(element.Value().Get());
    auto bind = [&](Event<> ButtonWidget::* event) noexcept {
        return Delegate<void()>::CreateLambda(
            [&objects, owner = Handle(), button, event]() noexcept {
                auto widget = objects.ResolveForWrite<ButtonWidget>(objects.OwnerAccess(), owner);
                if (widget) {
                    auto* current = widget.Value().Get().Value();
                    if (current->CachedElement().Get() == button) {
                        (current->*event).Broadcast({objects, objects.OwnerAccess(), owner});
                    }
                }
            });
    };
    button->SetOnClicked(bind(&ButtonWidget::clicked_));
    button->SetOnPressed(bind(&ButtonWidget::pressed_));
    button->SetOnReleased(bind(&ButtonWidget::released_));
    button->SetOnHovered(bind(&ButtonWidget::hovered_));
    button->SetOnUnhovered(bind(&ButtonWidget::unhovered_));
    return element;
}

} // namespace rocks::ui

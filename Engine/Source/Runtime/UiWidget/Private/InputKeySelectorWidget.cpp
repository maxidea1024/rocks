#include <UiWidget/InputKeySelectorWidget.h>
#include "ControlCaption.h"
#include <UiFramework/InputKeySelector.h>
#include <UiWidget/InputKeySelectorWidget.generated.h>

namespace rocks::ui {

InputKeySelectorWidget::InputKeySelectorWidget(Allocator& allocator) noexcept
    : keyName_(allocator) {
}

Result<void> InputKeySelectorWidget::StoreChord(KeyChord chord) noexcept {
    auto stored = keyName_.TryAssign(chord.InputName());
    if (stored) {
        shift_ = chord.shift;
        control_ = chord.control;
        alt_ = chord.alt;
        super_ = chord.super;
    }

    return stored;
}

Result<void> InputKeySelectorWidget::SetSelectedKey(KeyChord chord) noexcept {
    if (!chord.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto live = CachedElement();
    if (live) {
        auto updated = static_cast<InputKeySelector&>(*live).SetSelectedKey(chord);
        if (!updated) {
            return updated;
        }
    }

    auto stored = StoreChord(chord);
    if (!stored && live) {
        (void)static_cast<InputKeySelector&>(*live).SetSelectedKey(SelectedKey());
    }

    return stored;
}

KeyChord InputKeySelectorWidget::SelectedKey() const noexcept {
    auto key = KeyChord::FromInputName(keyName_.View());
    KeyChord chord = key ? key.Value() : KeyChord{};
    chord.shift = shift_;
    chord.control = control_;
    chord.alt = alt_;
    chord.super = super_;
    return chord;
}

void InputKeySelectorWidget::SetAllowModifiers(bool enabled) noexcept {
    allowModifiers_ = enabled;
    if (auto live = CachedElement()) {
        static_cast<InputKeySelector&>(*live).SetAllowModifiers(enabled);
    }
}

void InputKeySelectorWidget::SetAllowGamepad(bool enabled) noexcept {
    allowGamepad_ = enabled;
    if (auto live = CachedElement()) {
        static_cast<InputKeySelector&>(*live).SetAllowGamepad(enabled);
    }
}

void InputKeySelectorWidget::SetOnSelected(Delegate<void(KeyChord)>&& callback) noexcept {
    selected_ = Move(callback);
}

Result<SharedPtr<Element>> InputKeySelectorWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    auto made = SharedPtr<InputKeySelector>::TryMake(allocator, allocator);
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }

    auto& selector = *made.Value();
    if (!keyName_.View().IsEmpty() && !KeyChord::FromInputName(keyName_.View())) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::InvalidArgument);
    }

    auto initialized = selector.SetSelectedKey(SelectedKey());
    if (!initialized) {
        return Result<SharedPtr<Element>>::Failure(initialized.Error());
    }

    selector.SetAllowModifiers(allowModifiers_);
    selector.SetAllowGamepad(allowGamepad_);
    auto caption = AttachControlCaption(objects, Handle(), selector);
    if (!caption) {
        return Result<SharedPtr<Element>>::Failure(caption.Error());
    }

    initialized = selector.SetCaption(Move(caption.Value()));
    if (!initialized) {
        return Result<SharedPtr<Element>>::Failure(initialized.Error());
    }

    selector.SetOnSelected(Delegate<void(KeyChord)>::CreateLambda(
        [&objects, owner = Handle(), native = &selector](KeyChord chord) noexcept {
            auto widget =
                objects.ResolveForWrite<InputKeySelectorWidget>(objects.OwnerAccess(), owner);
            if (widget) {
                auto* current = widget.Value().Get().Value();
                if (current->CachedElement().Get() == native) {
                    if (current->StoreChord(chord)) {
                        current->selected_.ExecuteIfBound(chord);
                    } else {
                        (void)native->SetSelectedKey(current->SelectedKey());
                    }
                }
            }
        }));
    return Result<SharedPtr<Element>>::Success(Move(made.Value()));
}

} // namespace rocks::ui

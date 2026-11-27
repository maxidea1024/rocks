#include "EditableTextElement.h"
#include <UiWidget/EditableTextWidget.generated.h>

namespace rocks::ui {

Result<SharedPtr<TextLabel>> EditableTextElement::AddCaption(const Text& value, float size,
                                                             bool error) noexcept {
    if (!value.IsValid()) {
        return Result<SharedPtr<TextLabel>>::Success({});
    }

    if (!shaper_) {
        return Result<SharedPtr<TextLabel>>::Failure(ErrorCode::NotReady);
    }

    auto& allocator = registry_.ObjectAllocator();
    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!label) {
        return label;
    }

    auto ready = shaper_->Shape(*label.Value(), value, size);
    if (!ready) {
        return Result<SharedPtr<TextLabel>>::Failure(ready.Error());
    }

    if (label.Value()->GetText().IsEmpty()) {
        return Result<SharedPtr<TextLabel>>::Success({});
    }

    if (error) {
        ready = label.Value()->SetLayoutOptions(0, text::TextAlignment::Left, true);
    } else {
        ready = label_->CopyParagraphTo(*label.Value());
    }

    if (!ready) {
        return Result<SharedPtr<TextLabel>>::Failure(ready.Error());
    }

    label.Value()->SetColor(error ? LinearColor{1, 0.3f, 0.26f, 1}
                                  : LinearColor{0.65f, 0.67f, 0.7f, 1});
    label.Value()->SetClipToBounds(true);
    label.Value()->SetVisibility(error || label_->GetText().IsEmpty() ? Visibility::HitTestInvisible
                                                                      : Visibility::Collapsed);
    ready = TryAddChild(label.Value());
    return ready ? Move(label) : Result<SharedPtr<TextLabel>>::Failure(ready.Error());
}

void EditableTextElement::PublishCaption(SharedPtr<TextLabel> next, bool error) noexcept {
    auto& previous = error ? error_ : hint_;
    if (previous) {
        (void)RemoveChild(*previous);
    }

    previous = Move(next);
    InvalidateLayout();
}

Result<void> EditableTextElement::SetCaption(const Text& value, float size, bool error) noexcept {
    auto caption = AddCaption(value, size, error);
    if (!caption) {
        return Result<void>::Failure(caption.Error());
    }

    PublishCaption(Move(caption.Value()), error);
    return Result<void>::Success();
}

Result<void> EditableTextElement::ResizeText(EditableTextWidget& model, float size) noexcept {
    // 세 텍스트 모두 새 크기로 준비되어야 교체한다. 준비 중인 자식은 실패하면 즉시 분리한다.
    auto hint = AddCaption(model.hintText_, size, false);
    if (!hint) {
        return Result<void>::Failure(hint.Error());
    }

    auto error = AddCaption(model.errorText_, size, true);
    // 조합 중이면 미리보기를 새 크기로 셰이핑한다(조합을 끊지 않는다).
    auto ready = !error       ? Result<void>::Failure(error.Error())
                 : composing_ ? ShapeText(preview_.View(), size, model.IsPassword())
                              : Shape(model.GetText(), size, model.IsPassword());
    if (!ready) {
        if (hint.Value()) {
            (void)RemoveChild(*hint.Value());
        }

        if (error && error.Value()) {
            (void)RemoveChild(*error.Value());
        }

        return ready;
    }

    PublishCaption(Move(hint.Value()), false);
    PublishCaption(Move(error.Value()), true);
    return Result<void>::Success();
}

void EditableTextElement::SelectAll() noexcept {
    anchor_ = 0;
    MoveCaret(label_->GetText().ByteCount(), true);
    InvalidatePaint();
}

StringView EditableTextElement::Selection(StringView value) const noexcept {
    const usize start = anchor_ < caret_ ? anchor_ : caret_;
    const usize end = anchor_ < caret_ ? caret_ : anchor_;
    return end <= value.ByteCount() ? value.SliceBytes(start, end - start) : StringView{};
}

void EditableTextElement::RememberFocusText(const EditableTextWidget& model) noexcept {
    canRevert_ = false;
    focusText_.Clear();
    if (focused_ && model.revertOnEscape_ && !model.IsPassword()) {
        canRevert_ = static_cast<bool>(focusText_.TryAssign(model.GetText()));
    }
}

bool EditableTextElement::RevertToFocusText(EditableTextWidget& model) noexcept {
    if (!canRevert_ || model.IsReadOnly() || model.IsPassword() || !model.revertOnEscape_ ||
        focusText_.View() == model.GetText()) {
        return false;
    }

    auto snapshot = focusText_.TryClone(focusText_.GetAllocator());
    if (!snapshot || !model.SetText(snapshot.Value().View())) {
        return false;
    }

    model.changed_.ExecuteIfBound(snapshot.Value().View());
    return true;
}

} // namespace rocks::ui

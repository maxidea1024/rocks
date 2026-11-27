#include <UiFramework/Controls/EditableText.h>

namespace rocks::ui {

Result<SharedPtr<TextLabel>> EditableText::AddCaption(const Text& value, float size,
                                                      bool error) noexcept {
    if (!value.IsValid()) {
        return Result<SharedPtr<TextLabel>>::Success({});
    }

    if (!shaper_) {
        return Result<SharedPtr<TextLabel>>::Failure(ErrorCode::NotReady);
    }

    auto& allocator = allocator_;
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

void EditableText::PublishCaption(SharedPtr<TextLabel> next, bool error) noexcept {
    auto& previous = error ? error_ : hint_;
    if (previous) {
        (void)RemoveChild(*previous);
    }

    previous = Move(next);
    InvalidateLayout();
}

Result<void> EditableText::SetCaption(const Text& value, bool error) noexcept {
    if (error && !boxed_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto copy = value.IsValid() ? value.DeepClone(allocator_) : Result<Text>::Success({});
    if (!copy) {
        return Result<void>::Failure(copy.Error());
    }

    // 빈 원문의 불변 글자는 지우기와 같다.
    TextPersistentView view;
    if (copy.Value().TryGetPersistentForm(view) && view.form == TextPersistentForm::Invariant &&
        view.source.IsEmpty()) {
        copy.Value() = {};
    }

    if (label_) {
        auto caption = AddCaption(copy.Value(), fontSize_, error);
        if (!caption) {
            return Result<void>::Failure(caption.Error());
        }

        PublishCaption(Move(caption.Value()), error);
    }

    (error ? errorText_ : hintText_) = Move(copy.Value());
    return Result<void>::Success();
}

Result<void> EditableText::SetHintText(const Text& hint) noexcept {
    return SetCaption(hint, false);
}

const Text& EditableText::HintText() const noexcept {
    return hintText_;
}

Result<void> EditableText::SetError(const Text& message) noexcept {
    return SetCaption(message, true);
}

bool EditableText::HasError() const noexcept {
    return errorText_.IsValid();
}

float EditableText::FontSize() const noexcept {
    return fontSize_;
}

Result<void> EditableText::SetFontSize(float size) noexcept {
    if (!math::IsFinite(size) || size <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (fontSize_ == size) {
        return Result<void>::Success();
    }

    if (!label_) {
        fontSize_ = size;
        return Result<void>::Success();
    }

    // 세 텍스트 모두 새 크기로 준비되어야 교체한다. 준비 중인 자식은 실패하면 즉시 분리한다.
    auto hint = AddCaption(hintText_, size, false);
    if (!hint) {
        return Result<void>::Failure(hint.Error());
    }

    auto error = AddCaption(errorText_, size, true);
    // 조합 중이면 미리보기를 새 크기로 셰이핑한다(조합을 끊지 않는다).
    auto ready = !error       ? Result<void>::Failure(error.Error())
                 : composing_ ? ShapeText(preview_.View(), size, password_)
                              : Shape(text_.View(), size, password_);
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

void EditableText::SelectAllText() noexcept {
    if (!label_) {
        return;
    }

    anchor_ = 0;
    MoveCaret(label_->GetText().ByteCount(), true);
    InvalidatePaint();
}

StringView EditableText::SelectedText() const noexcept {
    if (password_) {
        return {};
    }

    const StringView value = text_.View();
    const usize start = anchor_ < caret_ ? anchor_ : caret_;
    const usize end = anchor_ < caret_ ? caret_ : anchor_;
    return end <= value.ByteCount() ? value.SliceBytes(start, end - start) : StringView{};
}

void EditableText::RememberFocusText() noexcept {
    canRevert_ = false;
    focusText_.Clear();
    if (focused_ && revertOnEscape_ && !password_) {
        canRevert_ = static_cast<bool>(focusText_.TryAssign(text_.View()));
    }
}

bool EditableText::RevertToFocusText() noexcept {
    if (!canRevert_ || readOnly_ || password_ || !revertOnEscape_ ||
        focusText_.View() == text_.View()) {
        return false;
    }

    auto snapshot = focusText_.TryClone(focusText_.GetAllocator());
    if (!snapshot || !SetText(snapshot.Value().View())) {
        return false;
    }

    NotifyChanged(snapshot.Value().View());
    return true;
}

} // namespace rocks::ui

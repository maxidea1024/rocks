#include <UiFramework/Controls/EditableText.h>

namespace rocks::ui {

Result<SharedPtr<TextLabel>> EditableText::AddHint(const Text& value, float size) noexcept {
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

    ready = label_->CopyParagraphTo(*label.Value());
    if (!ready) {
        return Result<SharedPtr<TextLabel>>::Failure(ready.Error());
    }

    // 힌트는 물려받은 전경색 × 스타일 불투명도다(UE HintTextOpacity — SlateEditableTextLayout.cpp
    // 3683행).
    label.Value()->SetColor(ColorRef{ColorSource::Foreground});
    (void)label.Value()->SetRenderOpacity(style_.hintOpacity);
    label.Value()->SetClipToBounds(true);
    label.Value()->SetVisibility(label_->GetText().IsEmpty() ? Visibility::HitTestInvisible
                                                             : Visibility::Collapsed);
    ready = TryAddChild(label.Value());
    return ready ? Move(label) : Result<SharedPtr<TextLabel>>::Failure(ready.Error());
}

void EditableText::PublishHint(SharedPtr<TextLabel> next) noexcept {
    if (hint_) {
        (void)RemoveChild(*hint_);
    }

    hint_ = Move(next);
    InvalidateLayout();
}

Result<void> EditableText::SetHintText(const Text& hint) noexcept {
    auto copy = hint.IsValid() ? hint.DeepClone(allocator_) : Result<Text>::Success({});
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
        auto made = AddHint(copy.Value(), fontSize_);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        PublishHint(Move(made.Value()));
    }

    hintText_ = Move(copy.Value());
    return Result<void>::Success();
}

const Text& EditableText::HintText() const noexcept {
    return hintText_;
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

    // 두 글자 모두 새 크기로 준비되어야 교체한다. 준비 중인 힌트는 실패하면 즉시 분리한다.
    auto hint = AddHint(hintText_, size);
    if (!hint) {
        return Result<void>::Failure(hint.Error());
    }

    // 조합 중이면 미리보기를 새 크기로 셰이핑한다(조합을 끊지 않는다).
    auto ready = composing_ ? ShapeText(preview_.View(), size, password_)
                            : Shape(text_.View(), size, password_);
    if (!ready) {
        if (hint.Value()) {
            (void)RemoveChild(*hint.Value());
        }

        return ready;
    }

    PublishHint(Move(hint.Value()));
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

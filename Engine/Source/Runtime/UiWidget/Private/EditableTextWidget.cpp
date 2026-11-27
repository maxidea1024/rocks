#include <UiFramework/Controls/EditableText.h>
#include <UiWidget/EditableTextWidget.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

namespace {

EditableText* LiveEditor(const Widget& widget) noexcept {
    auto live = widget.CachedElement();
    return live ? &StaticElementCast<EditableText>(*live) : nullptr;
}

} // namespace

EditableTextWidget::EditableTextWidget(Allocator& allocator) noexcept : text_(allocator) {
}

TextLabel& EditableTextWidget::ResolveTextLabel(Element& element) const noexcept {
    return StaticElementCast<EditableText>(element).Label();
}

bool EditableTextWidget::IsMultiline() const noexcept {
    return false;
}

bool EditableTextWidget::HasBox() const noexcept {
    return false;
}

StringView EditableTextWidget::GetText() const noexcept {
    return text_.View();
}

bool EditableTextWidget::IsReadOnly() const noexcept {
    return readOnly_;
}

void EditableTextWidget::SetReadOnly(bool readOnly) noexcept {
    if (auto* editor = LiveEditor(*this)) {
        editor->SetReadOnly(readOnly);
    }

    if (readOnly_ != readOnly) {
        readOnly_ = readOnly;
        NotifyPropertyChanged("readOnly");
    }
}

Result<void> EditableTextWidget::SetText(StringView text) noexcept {
    if (!IsMultiline() && (text.Contains("\n") || text.Contains("\r"))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (text == text_.View()) {
        return Result<void>::Success();
    }

    String copy(text_.GetAllocator());
    auto assigned = copy.TryAssign(text);
    if (!assigned) {
        return assigned;
    }

    if (auto* editor = LiveEditor(*this)) {
        auto applied = editor->SetText(text);
        if (!applied) {
            return applied;
        }
    }

    text_.Swap(copy);
    NotifyPropertyChanged("text");
    return Result<void>::Success();
}

void EditableTextWidget::OnEditedText(StringView text) noexcept {
    // Element가 바꾼 글자를 속성에 반영한다. 할당 실패면 속성이 Element보다 뒤처지므로 통지하지
    // 않는다(다음 편집에서 다시 맞춘다).
    if (text != text_.View()) {
        if (!text_.TryAssign(text)) {
            return;
        }

        NotifyPropertyChanged("text");
    }

    changed_.ExecuteIfBound(text);
}

bool EditableTextWidget::IsPassword() const noexcept {
    return password_;
}

Result<void> EditableTextWidget::SetPassword(bool password) noexcept {
    if (password == password_) {
        return Result<void>::Success();
    }

    if (auto* editor = LiveEditor(*this)) {
        auto applied = editor->SetPassword(password);
        if (!applied) {
            return applied;
        }
    }

    password_ = password;
    NotifyPropertyChanged("password");
    return Result<void>::Success();
}

Result<void> EditableTextWidget::SetFontSize(float size) noexcept {
    if (!math::IsFinite(size) || size <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (fontSize_ != size) {
        if (auto* editor = LiveEditor(*this)) {
            auto applied = editor->SetFontSize(size);
            if (!applied) {
                return applied;
            }
        }

        fontSize_ = size;
        NotifyPropertyChanged("fontSize");
    }

    return Result<void>::Success();
}

Result<void> EditableTextWidget::SetCaption(const Text& value, bool error) noexcept {
    if (error && !HasBox()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto copy = value.IsValid() ? value.DeepClone(text_.GetAllocator()) : Result<Text>::Success({});
    if (!copy) {
        return Result<void>::Failure(copy.Error());
    }

    TextPersistentView view;
    if (copy.Value().TryGetPersistentForm(view) && view.form == TextPersistentForm::Invariant &&
        view.source.IsEmpty()) {
        copy.Value() = {};
    }

    if (auto* editor = LiveEditor(*this)) {
        auto ready = error ? editor->SetError(copy.Value()) : editor->SetHintText(copy.Value());
        if (!ready) {
            return ready;
        }
    }

    (error ? errorText_ : hintText_) = Move(copy.Value());
    NotifyPropertyChanged(error ? "errorText" : "hintText");
    return Result<void>::Success();
}

Result<void> EditableTextWidget::SetHintText(const Text& hint) noexcept {
    return SetCaption(hint, false);
}

const Text& EditableTextWidget::HintText() const noexcept {
    return hintText_;
}

Result<void> EditableTextWidget::SetError(const Text& value) noexcept {
    return SetCaption(value, true);
}

bool EditableTextWidget::HasError() const noexcept {
    return errorText_.IsValid();
}

void EditableTextWidget::SetSelectAllTextWhenFocused(bool value) noexcept {
    if (auto* editor = LiveEditor(*this)) {
        editor->SetSelectAllTextWhenFocused(value);
    }

    if (selectAllOnFocus_ != value) {
        selectAllOnFocus_ = value;
        NotifyPropertyChanged("selectAllOnFocus");
    }
}

void EditableTextWidget::SetSelectAllTextOnCommit(bool value) noexcept {
    if (auto* editor = LiveEditor(*this)) {
        editor->SetSelectAllTextOnCommit(value);
    }

    if (selectAllOnCommit_ != value) {
        selectAllOnCommit_ = value;
        NotifyPropertyChanged("selectAllOnCommit");
    }
}

void EditableTextWidget::SetRevertTextOnEscape(bool value) noexcept {
    if (auto* editor = LiveEditor(*this)) {
        editor->SetRevertTextOnEscape(value);
    }

    if (revertOnEscape_ != value) {
        revertOnEscape_ = value;
        NotifyPropertyChanged("revertOnEscape");
    }
}

void EditableTextWidget::SelectAllText() noexcept {
    if (auto* editor = LiveEditor(*this)) {
        editor->SelectAllText();
    }
}

StringView EditableTextWidget::SelectedText() const noexcept {
    const auto* editor = LiveEditor(*this);
    return editor ? editor->SelectedText() : StringView{};
}

void EditableTextWidget::SetOnChanged(Delegate<void(StringView)>&& callback) noexcept {
    changed_ = Move(callback);
}

void EditableTextWidget::SetOnCommitted(
    Delegate<void(StringView, TextCommitReason)>&& callback) noexcept {
    committed_ = Move(callback);
}

Result<SharedPtr<Element>> EditableTextWidget::BuildElement(ObjectRegistry& objects) noexcept {
    const auto access = objects.OwnerAccess();
    auto outer = objects.GetOuter(access, Handle());
    if (!outer) {
        return Result<SharedPtr<Element>>::Failure(outer.Error());
    }

    auto tree = objects.Resolve<WidgetTree>(access, outer.Value());
    if (!tree) {
        return Result<SharedPtr<Element>>::Failure(tree.Error());
    }

    if (errorText_.IsValid() && !HasBox()) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::InvalidArgument);
    }

    auto& allocator = objects.ObjectAllocator();
    auto made = SharedPtr<EditableText>::TryMake(
        allocator, allocator, tree.Value().Get().Value()->GetTextShaper(),
        tree.Value().Get().Value()->GetPlatform(), IsMultiline(), HasBox());
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }

    auto& editor = *made.Value();
    editor.SetReadOnly(readOnly_);
    editor.SetSelectAllTextWhenFocused(selectAllOnFocus_);
    editor.SetSelectAllTextOnCommit(selectAllOnCommit_);
    editor.SetRevertTextOnEscape(revertOnEscape_);
    auto ready = editor.SetPassword(password_);
    if (ready) {
        ready = editor.SetFontSize(fontSize_);
    }

    if (ready && hintText_.IsValid()) {
        ready = editor.SetHintText(hintText_);
    }

    if (ready && errorText_.IsValid()) {
        ready = editor.SetError(errorText_);
    }

    if (ready) {
        ready = editor.Initialize(text_.View(), fontSize_);
    }

    if (ready) {
        ready = ApplyTextLayout(editor.Label());
    }

    if (!ready) {
        return Result<SharedPtr<Element>>::Failure(ready.Error());
    }

    // 해제된 Element가 뒤늦게 보낸 사건은 재구성된 Widget에 전달하지 않는다(BindLiveWidgetEvent와
    // 같은 확인).
    const auto owner = Handle();
    const Element* native = made.Value().Get();
    editor.SetOnTextChanged(Delegate<void(StringView)>::CreateLambda(
        [&objects, owner, native](StringView text) noexcept {
            auto widget = objects.ResolveForWrite<EditableTextWidget>(objects.OwnerAccess(), owner);
            if (widget && widget.Value().Get().Value()->CachedElement().Get() == native) {
                widget.Value().Get().Value()->OnEditedText(text);
            }
        }));
    editor.SetOnTextCommitted(Delegate<void(StringView, TextCommitReason)>::CreateLambda(
        [&objects, owner, native](StringView text, TextCommitReason reason) noexcept {
            auto widget = objects.ResolveForWrite<EditableTextWidget>(objects.OwnerAccess(), owner);
            if (widget && widget.Value().Get().Value()->CachedElement().Get() == native) {
                widget.Value().Get().Value()->committed_.ExecuteIfBound(text, reason);
            }
        }));
    return Result<SharedPtr<Element>>::Success(Move(made.Value()));
}

} // namespace rocks::ui

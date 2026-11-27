#include <UiFramework/Controls/EditableTextBox.h>
#include <UiWidget/EditableTextWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

namespace {

// 상자 위젯은 EditableTextBox를, 아니면 EditableText를 만든다(Docs/Design/UiToolkit 22.8절).
EditableText& EditorOf(Element& element) noexcept {
    if (auto* box = ElementCast<EditableTextBox>(&element)) {
        return box->Editor();
    }

    return StaticElementCast<EditableText>(element);
}

EditableText* LiveEditor(const Widget& widget) noexcept {
    auto live = widget.CachedElement();
    return live ? &EditorOf(*live) : nullptr;
}

EditableTextBox* LiveBox(const Widget& widget) noexcept {
    auto live = widget.CachedElement();
    return live ? ElementCast<EditableTextBox>(live.Get()) : nullptr;
}

} // namespace

EditableTextWidget::EditableTextWidget(Allocator& allocator) noexcept
    : text_(allocator), changed_(allocator), committed_(allocator) {
}

TextLabel& EditableTextWidget::ResolveTextLabel(Element& element) const noexcept {
    return EditorOf(element).Label();
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

void EditableTextWidget::OnEditedText(ObjectRegistry& objects, StringView text) noexcept {
    // Element가 바꾼 글자를 속성에 반영한다. 할당 실패면 속성이 Element보다 뒤처지므로 통지하지
    // 않는다(다음 편집에서 다시 맞춘다).
    if (text != text_.View()) {
        if (!text_.TryAssign(text)) {
            return;
        }

        NotifyPropertyChanged("text");
    }

    // 구독자가 SetText를 불러도 받은 글자가 유효하게 사본을 넘긴다.
    String snapshot(objects.ObjectAllocator());
    if (snapshot.TryAssign(text)) {
        changed_.Broadcast({objects, objects.OwnerAccess(), Handle()}, snapshot);
    }
}

void EditableTextWidget::OnCommittedText(ObjectRegistry& objects, StringView text,
                                         TextCommitReason reason) noexcept {
    String snapshot(objects.ObjectAllocator());
    if (snapshot.TryAssign(text)) {
        committed_.Broadcast({objects, objects.OwnerAccess(), Handle()}, snapshot,
                             ToWidgetTextCommitReason(reason));
    }
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

    if (error) {
        if (auto* box = LiveBox(*this)) {
            auto ready = box->SetError(copy.Value());
            if (!ready) {
                return ready;
            }
        }
    } else if (auto* editor = LiveEditor(*this)) {
        auto ready = editor->SetHintText(copy.Value());
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

Event<const String&>& EditableTextWidget::OnChanged() noexcept {
    return changed_;
}

Event<const String&, WidgetTextCommitReason>& EditableTextWidget::OnCommitted() noexcept {
    return committed_;
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
    auto shaper = tree.Value().Get().Value()->GetTextShaper();
    const Platform platform = tree.Value().Get().Value()->GetPlatform();
    SharedPtr<Element> made;
    EditableText* built = nullptr;
    EditableTextBox* box = nullptr;
    if (HasBox()) {
        // 상자는 Initialize가 안쪽 편집기를 만든다 — 속성은 그 뒤에 넣는다(글자는 Initialize가
        // 넣는다).
        auto boxed = SharedPtr<EditableTextBox>::TryMake(allocator, allocator, Move(shaper),
                                                         platform, IsMultiline());
        if (!boxed) {
            return Result<SharedPtr<Element>>::Failure(boxed.Error());
        }

        auto initialized = boxed.Value()->Initialize(text_.View(), fontSize_);
        if (!initialized) {
            return Result<SharedPtr<Element>>::Failure(initialized.Error());
        }

        box = boxed.Value().Get();
        built = &box->Editor();
        made = Move(boxed.Value());
    } else {
        auto plain = SharedPtr<EditableText>::TryMake(allocator, allocator, Move(shaper), platform,
                                                      IsMultiline());
        if (!plain) {
            return Result<SharedPtr<Element>>::Failure(plain.Error());
        }

        built = plain.Value().Get();
        made = Move(plain.Value());
    }

    auto& editor = *built;
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
        ready = box->SetError(errorText_);
    }

    if (ready && !box) {
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
    const Element* native = made.Get();
    editor.SetOnTextChanged(Delegate<void(StringView)>::CreateLambda(
        [&objects, owner, native](StringView text) noexcept {
            auto widget = objects.ResolveForWrite<EditableTextWidget>(objects.OwnerAccess(), owner);
            if (widget && widget.Value().Get().Value()->CachedElement().Get() == native) {
                widget.Value().Get().Value()->OnEditedText(objects, text);
            }
        }));
    editor.SetOnTextCommitted(Delegate<void(StringView, TextCommitReason)>::CreateLambda(
        [&objects, owner, native](StringView text, TextCommitReason reason) noexcept {
            auto widget = objects.ResolveForWrite<EditableTextWidget>(objects.OwnerAccess(), owner);
            if (widget && widget.Value().Get().Value()->CachedElement().Get() == native) {
                widget.Value().Get().Value()->OnCommittedText(objects, text, reason);
            }
        }));
    return Result<SharedPtr<Element>>::Success(Move(made));
}

} // namespace rocks::ui

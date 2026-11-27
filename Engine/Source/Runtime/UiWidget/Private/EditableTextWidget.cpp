#include "EditableTextElement.h"
#include <UiWidget/EditableTextWidget.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

EditableTextWidget::EditableTextWidget(Allocator& allocator) noexcept : text_(allocator) {
}

TextLabel& EditableTextWidget::ResolveTextLabel(Element& element) const noexcept {
    return StaticElementCast<EditableTextElement>(element).Label();
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
    if (readOnly) {
        if (auto live = CachedElement()) {
            StaticElementCast<EditableTextElement>(*live).CancelComposition(*this);
        }
    }

    if (readOnly_ != readOnly) {
        readOnly_ = readOnly;
        if (auto live = CachedElement()) {
            // IME 허용이 바뀌었다(읽기 전용은 IME를 끈다 — 19.2절).
            StaticElementCast<EditableTextElement>(*live).RefreshTextInput();
        }

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

    auto result = ExchangeText(copy);
    if (result) {
        if (auto live = CachedElement()) {
            StaticElementCast<EditableTextElement>(*live).ClearHistory();
        }
    }

    return result;
}

Result<void> EditableTextWidget::ExchangeText(String& copy) noexcept {
    if (auto live = CachedElement()) {
        auto& editor = StaticElementCast<EditableTextElement>(*live);
        auto shaped = editor.Shape(copy.View(), fontSize_, password_);
        if (!shaped) {
            return shaped;
        }

        editor.MoveCaret(copy.ByteCount(), false);
    }

    const bool changed = text_.View() != copy.View();
    text_.Swap(copy);
    if (changed) {
        NotifyPropertyChanged("text");
    }

    return Result<void>::Success();
}

bool EditableTextWidget::IsPassword() const noexcept {
    return password_;
}

Result<void> EditableTextWidget::SetPassword(bool password) noexcept {
    if (password != password_) {
        if (auto live = CachedElement()) {
            auto& editor = StaticElementCast<EditableTextElement>(*live);
            auto shaped = editor.Shape(text_.View(), fontSize_, password);
            if (!shaped) {
                return shaped;
            }

            editor.MoveCaret(text_.ByteCount(), false);
            editor.ClearHistory();
        }

        password_ = password;
        if (auto live = CachedElement()) {
            // 비밀번호 입력란은 IME를 끈다(19.2절).
            StaticElementCast<EditableTextElement>(*live).RefreshTextInput();
        }

        NotifyPropertyChanged("password");
    }

    return Result<void>::Success();
}

Result<void> EditableTextWidget::SetFontSize(float size) noexcept {
    if (!math::IsFinite(size) || size <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (fontSize_ != size) {
        if (auto live = CachedElement()) {
            auto shaped = StaticElementCast<EditableTextElement>(*live).ResizeText(*this, size);
            if (!shaped) {
                return shaped;
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

    if (auto live = CachedElement()) {
        auto ready = StaticElementCast<EditableTextElement>(*live).SetCaption(copy.Value(),
                                                                              fontSize_, error);
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
    if (selectAllOnFocus_ != value) {
        selectAllOnFocus_ = value;
        NotifyPropertyChanged("selectAllOnFocus");
    }
}

void EditableTextWidget::SetSelectAllTextOnCommit(bool value) noexcept {
    if (selectAllOnCommit_ != value) {
        selectAllOnCommit_ = value;
        NotifyPropertyChanged("selectAllOnCommit");
    }
}

void EditableTextWidget::SetRevertTextOnEscape(bool value) noexcept {
    if (revertOnEscape_ != value) {
        revertOnEscape_ = value;
        if (auto live = CachedElement()) {
            StaticElementCast<EditableTextElement>(*live).RememberFocusText(*this);
        }

        NotifyPropertyChanged("revertOnEscape");
    }
}

void EditableTextWidget::SelectAllText() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<EditableTextElement>(*live).SelectAll();
    }
}

StringView EditableTextWidget::SelectedText() const noexcept {
    auto live = CachedElement();
    return live && !IsPassword()
               ? StaticElementCast<EditableTextElement>(*live).Selection(GetText())
               : StringView{};
}

void EditableTextWidget::SetOnChanged(Delegate<void(StringView)>&& callback) noexcept {
    changed_ = Move(callback);
}

void EditableTextWidget::SetOnCommitted(
    Delegate<void(StringView, TextCommitReason)>&& callback) noexcept {
    committed_ = Move(callback);
}

void EditableTextWidget::Commit(TextCommitReason reason) noexcept {
    if (!committed_.IsBound()) {
        return;
    }

    // 수신자가 SetText를 호출해도 전달한 문자열을 계속 읽을 수 있게 보존한다.
    auto snapshot = text_.TryClone(text_.GetAllocator());
    if (snapshot) {
        committed_.ExecuteIfBound(snapshot.Value().View(), reason);
    }
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

    auto& allocator = objects.ObjectAllocator();
    auto made = SharedPtr<EditableTextElement>::TryMake(
        allocator, allocator, objects, Handle(), tree.Value().Get().Value()->TextShaper(),
        tree.Value().Get().Value()->GetPlatform(), IsMultiline(), HasBox());
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }

    auto init = made.Value()->Initialize(text_.View(), fontSize_, password_);
    if (!init) {
        return Result<SharedPtr<Element>>::Failure(init.Error());
    }

    auto layout = ApplyTextLayout(made.Value()->Label());
    if (layout && hintText_.IsValid()) {
        layout = made.Value()->SetCaption(hintText_, fontSize_, false);
    }

    if (layout && errorText_.IsValid()) {
        layout = HasBox() ? made.Value()->SetCaption(errorText_, fontSize_, true)
                          : Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!layout) {
        return Result<SharedPtr<Element>>::Failure(layout.Error());
    }

    return Result<SharedPtr<Element>>::Success(Move(made.Value()));
}

} // namespace rocks::ui

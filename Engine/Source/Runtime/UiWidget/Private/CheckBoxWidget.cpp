#include "MakeWidgetElement.h"
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiFramework/CheckBox.h>

namespace rocks::ui {

CheckBoxWidget::CheckBoxWidget(Allocator& allocator) noexcept : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> CheckBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<CheckBox>(allocator);
}

bool CheckBoxWidget::IsChecked() const noexcept {
    return GetState() == CheckState::Checked;
}

void CheckBoxWidget::SetOnChanged(Delegate<void(bool)>&& callback) noexcept {
    changed_ = Move(callback);
}

void CheckBoxWidget::SetChecked(bool checked) noexcept {
    (void)SetState(checked ? CheckState::Checked : CheckState::Unchecked);
}

CheckState CheckBoxWidget::GetState() const noexcept {
    return indeterminate_ ? CheckState::Indeterminate
           : checked_     ? CheckState::Checked
                          : CheckState::Unchecked;
}

void CheckBoxWidget::SetOnStateChanged(Delegate<void(CheckState)>&& callback) noexcept {
    stateChanged_ = Move(callback);
}

Result<void> CheckBoxWidget::SetState(CheckState state) noexcept {
    if (state > CheckState::Indeterminate) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const bool changed = checked_ != (state == CheckState::Checked);
    checked_ = state == CheckState::Checked;
    indeterminate_ = state == CheckState::Indeterminate;
    if (auto element = CachedElement()) {
        ApplyProperties(*element);
    }
    if (changed) {
        NotifyPropertyChanged("checked");
    }
    return Result<void>::Success();
}

void CheckBoxWidget::ApplyProperties(Element& element) noexcept {
    (void)static_cast<CheckBox&>(element).SetState(GetState());
}

Result<SharedPtr<Element>> CheckBoxWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto box = PanelWidget::BuildElement(objects);
    if (!box) {
        return Result<SharedPtr<Element>>::Failure(box.Error());
    }
    static_cast<CheckBox&>(*box.Value())
        .SetOnChanged(Delegate<void(CheckState)>::CreateLambda(
            [&objects, owner = Handle(), native = box.Value().Get()](CheckState state) noexcept {
                auto widget = objects.ResolveForWrite<CheckBoxWidget>(objects.OwnerAccess(), owner);
                if (!widget) {
                    return;
                }
                auto* value = widget.Value().Get().Value();
                if (value->CachedElement().Get() != native) {
                    return;
                }
                const bool changed = value->checked_ != (state == CheckState::Checked);
                value->checked_ = state == CheckState::Checked;
                value->indeterminate_ = state == CheckState::Indeterminate;
                if (changed) { value->NotifyPropertyChanged("checked"); }
                value->changed_.ExecuteIfBound(state == CheckState::Checked);
                value->stateChanged_.ExecuteIfBound(state);
            }));
    return box;
}

} // namespace rocks::ui

#include <Base/NumericLimits.h>
#include <UiWidget/ComboBoxWidget.generated.h>

namespace rocks::ui {

ComboBoxWidget::ComboBoxWidget(Allocator& allocator) noexcept : options_(allocator) {
}

usize ComboBoxWidget::OptionCount() const noexcept {
    return options_.Size();
}

Result<ObjectHandle> ComboBoxWidget::OptionAt(usize index) const noexcept {
    if (index >= OptionCount()) {
        return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<ObjectHandle>::Success(options_.View()[index]);
}

Result<void> ComboBoxWidget::AddOption(ObjectRegistry& objects, ObjectHandle option) noexcept {
    if (!CanEditOptions()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (OptionCount() >= static_cast<usize>(NumericLimits<int32>::Max())) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    if (option.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    auto added = objects.AddReference(objects.OwnerAccess(), Handle(), options_, option);
    if (added) {
        OptionsChanged(SelectedIndex());
    }
    return added;
}

Result<void> ComboBoxWidget::RemoveOptionAt(ObjectRegistry& objects, usize index) noexcept {
    if (!CanEditOptions()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (index >= OptionCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const int32 next = SelectionAfterRemoving(index);
    auto removed = objects.RemoveReferenceAt(objects.OwnerAccess(), Handle(), options_, index);
    if (removed) {
        OptionsChanged(next);
    }
    return removed;
}

Result<void> ComboBoxWidget::ClearOptions(ObjectRegistry& objects) noexcept {
    if (!CanEditOptions()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (auto cleared = objects.ClearReferences(objects.OwnerAccess(), Handle(), options_);
        !cleared) {
        return cleared;
    }
    OptionsChanged(-1);
    return Result<void>::Success();
}

Optional<usize> ComboBoxWidget::FindOptionIndex(ObjectHandle option) const noexcept {
    for (usize index = 0; index < OptionCount(); ++index) {
        if (options_.View()[index] == option) {
            return index;
        }
    }
    return {};
}

Result<ObjectHandle> ComboBoxWidget::SelectedOption() const noexcept {
    return SelectedIndex() < 0 ? Result<ObjectHandle>::Failure(ErrorCode::NotFound)
                               : OptionAt(static_cast<usize>(SelectedIndex()));
}

Result<void> ComboBoxWidget::RemoveOption(ObjectRegistry& objects, ObjectHandle option) noexcept {
    auto index = FindOptionIndex(option);
    return index ? RemoveOptionAt(objects, *index) : Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ComboBoxWidget::SelectOption(ObjectHandle option) noexcept {
    auto index = FindOptionIndex(option);
    return index ? SetSelectedIndex(static_cast<int32>(*index))
                 : Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ComboBoxWidget::WriteLabel(ObjectRegistry& objects, usize index,
                                        String& out) const noexcept {
    return objects.GetName(objects.OwnerAccess(), options_.View()[index], out);
}

} // namespace rocks::ui

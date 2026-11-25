#include <Base/NumericLimits.h>
#include <UiWidget/ComboBoxKeyWidget.generated.h>
#include <Core/Text/NamePool.h>

namespace rocks::ui {

ComboBoxKeyWidget::ComboBoxKeyWidget(Allocator& allocator) noexcept : options_(allocator) {
}

usize ComboBoxKeyWidget::OptionCount() const noexcept {
    return options_.Size();
}

Result<Name> ComboBoxKeyWidget::OptionAt(usize index) const noexcept {
    if (index >= OptionCount()) {
        return Result<Name>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Name>::Success(options_[index]);
}

Result<void> ComboBoxKeyWidget::AddOption(ObjectRegistry& objects, Name option) noexcept {
    if (!CanEditOptions()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (OptionCount() >= static_cast<usize>(NumericLimits<int32>::Max())) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    if (!objects.Names()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    auto label = objects.Names()->Acquire(option);
    if (!label) {
        return Result<void>::Failure(label.Error());
    }
    for (Name existing : options_) {
        if (existing == option) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }
    auto added = options_.TryPushBack(option);
    if (added) {
        OptionsChanged(SelectedIndex());
    }
    return added;
}

Result<void> ComboBoxKeyWidget::RemoveOptionAt(usize index) noexcept {
    if (!CanEditOptions()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (index >= OptionCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const int32 next = SelectionAfterRemoving(index);
    auto removed = options_.TryRemoveAt(index);
    if (removed) {
        OptionsChanged(next);
    }
    return removed;
}

Result<void> ComboBoxKeyWidget::ClearOptions() noexcept {
    if (!CanEditOptions()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    options_.Clear();
    OptionsChanged(-1);
    return Result<void>::Success();
}

Optional<usize> ComboBoxKeyWidget::FindOptionIndex(Name option) const noexcept {
    for (usize index = 0; index < OptionCount(); ++index) {
        if (options_[index] == option) {
            return index;
        }
    }
    return {};
}

Result<Name> ComboBoxKeyWidget::SelectedOption() const noexcept {
    return SelectedIndex() < 0 ? Result<Name>::Failure(ErrorCode::NotFound)
                               : OptionAt(static_cast<usize>(SelectedIndex()));
}

Result<void> ComboBoxKeyWidget::RemoveOption(Name option) noexcept {
    auto index = FindOptionIndex(option);
    return index ? RemoveOptionAt(*index) : Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ComboBoxKeyWidget::SelectOption(Name option) noexcept {
    auto index = FindOptionIndex(option);
    return index ? SetSelectedIndex(static_cast<int32>(*index))
                 : Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ComboBoxKeyWidget::WriteLabel(ObjectRegistry& objects, usize index,
                                           String& out) const noexcept {
    if (!objects.Names()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    auto label = objects.Names()->Acquire(options_[index]);
    return label ? out.TryAssign(label.Value().View()) : Result<void>::Failure(label.Error());
}

} // namespace rocks::ui

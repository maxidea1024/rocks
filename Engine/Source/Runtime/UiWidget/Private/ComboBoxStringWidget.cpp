#include <Base/NumericLimits.h>
#include <UiWidget/ComboBoxStringWidget.generated.h>

namespace rocks::ui {

ComboBoxStringWidget::ComboBoxStringWidget(Allocator& allocator) noexcept
    : ComboBoxWidgetBase(allocator), options_(allocator) {
}

usize ComboBoxStringWidget::OptionCount() const noexcept {
    return options_.Size();
}

Result<StringView> ComboBoxStringWidget::OptionAt(usize index) const noexcept {
    if (index >= OptionCount()) {
        return Result<StringView>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<StringView>::Success(options_[index].View());
}

Result<void> ComboBoxStringWidget::AddOption(StringView option) noexcept {
    if (!CanEditOptions()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (OptionCount() >= static_cast<usize>(NumericLimits<int32>::Max())) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    String owned(options_.GetAllocator());
    if (auto copied = owned.TryAssign(option); !copied) {
        return copied;
    }

    auto added = options_.TryPushBack(Move(owned));
    if (added) {
        OptionsChanged(SelectedIndex());
    }

    return added;
}

Result<void> ComboBoxStringWidget::RemoveOptionAt(usize index) noexcept {
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

Result<void> ComboBoxStringWidget::ClearOptions() noexcept {
    if (!CanEditOptions()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    options_.Clear();
    OptionsChanged(-1);
    return Result<void>::Success();
}

Optional<usize> ComboBoxStringWidget::FindOptionIndex(StringView option) const noexcept {
    for (usize index = 0; index < OptionCount(); ++index) {
        if (options_[index].View() == option) {
            return index;
        }
    }

    return {};
}

Result<StringView> ComboBoxStringWidget::SelectedOption() const noexcept {
    return SelectedIndex() < 0 ? Result<StringView>::Failure(ErrorCode::NotFound)
                               : OptionAt(static_cast<usize>(SelectedIndex()));
}

Result<void> ComboBoxStringWidget::RemoveOption(StringView option) noexcept {
    auto index = FindOptionIndex(option);
    return index ? RemoveOptionAt(*index) : Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ComboBoxStringWidget::SelectOption(ObjectRegistry& objects,
                                                StringView option) noexcept {
    auto index = FindOptionIndex(option);
    return index ? SetSelectedIndex(objects, static_cast<int32>(*index))
                 : Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ComboBoxStringWidget::WriteLabel(ObjectRegistry&, usize index,
                                              String& out) const noexcept {
    return out.TryAssign(options_[index].View());
}

} // namespace rocks::ui

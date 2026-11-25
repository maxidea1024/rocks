#include <Core/ScopeExit.h>
#include <UiWidget/ComboBoxWidgetBase.generated.h>
#include <UiWidget/WidgetTextShaper.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

ComboBoxWidgetBase::~ComboBoxWidgetBase() noexcept {
    if (auto live = CachedElement()) {
        static_cast<ComboBox&>(*live).DisconnectModel();
    }
}

int32 ComboBoxWidgetBase::SelectedIndex() const noexcept {
    return selectedIndex_;
}

bool ComboBoxWidgetBase::CanEditOptions() const noexcept {
    return !generating_;
}

int32 ComboBoxWidgetBase::SelectionAfterRemoving(usize index) const noexcept {
    if (selectedIndex_ < 0 || static_cast<usize>(selectedIndex_) < index) {
        return selectedIndex_;
    }
    return index == static_cast<usize>(selectedIndex_) ? -1 : selectedIndex_ - 1;
}

void ComboBoxWidgetBase::OptionsChanged(int32 selection) noexcept {
    const bool cleared = selectedIndex_ >= 0 && selection < 0;
    selectedIndex_ = selection;
    if (auto live = CachedElement()) {
        static_cast<ComboBox&>(*live).UpdateOptions(OptionCount(), selection);
    }
    if (cleared) {
        selectionChanged_.ExecuteIfBound(-1);
    }
}

Result<void> ComboBoxWidgetBase::SetSelectedIndex(int32 index) noexcept {
    if (index < -1 || (index >= 0 && static_cast<usize>(index) >= OptionCount())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (generating_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (auto live = CachedElement()) {
        return static_cast<ComboBox&>(*live).Select(index);
    }
    if (selectedIndex_ != index) {
        selectedIndex_ = index;
        selectionChanged_.ExecuteIfBound(index);
    }
    return Result<void>::Success();
}

void ComboBoxWidgetBase::SetOnSelectionChanged(Delegate<void(int32)>&& callback) noexcept {
    selectionChanged_ = Move(callback);
}

void ComboBoxWidgetBase::SetOptionFactory(ComboBox::MakeOption&& factory) noexcept {
    factory_ = Move(factory);
}

Result<SharedPtr<Element>> ComboBoxWidgetBase::MakeOption(ObjectRegistry& objects, usize index,
                                                          bool display) noexcept {
    if (generating_ || index >= OptionCount()) {
        return Result<SharedPtr<Element>>::Failure(generating_ ? ErrorCode::Busy
                                                               : ErrorCode::InvalidArgument);
    }
    generating_ = true;
    ROCKS_ON_SCOPE_EXIT {
        generating_ = false;
    };
    if (factory_) {
        return factory_.Execute(index, display);
    }
    auto access = objects.OwnerAccess();
    auto outer = objects.GetOuter(access, Handle());
    if (!outer) {
        return Result<SharedPtr<Element>>::Failure(outer.Error());
    }
    auto tree = objects.Resolve<WidgetTree>(access, outer.Value());
    if (!tree || !tree.Value().Get().Value()->TextShaper()) {
        return Result<SharedPtr<Element>>::Failure(tree ? ErrorCode::NotReady : tree.Error());
    }
    auto& allocator = objects.ObjectAllocator();
    String string(allocator);
    auto name = WriteLabel(objects, index, string);
    if (!name) {
        return Result<SharedPtr<Element>>::Failure(name.Error());
    }
    auto text = Text::Invariant(allocator, string.View());
    if (!text) {
        return Result<SharedPtr<Element>>::Failure(text.Error());
    }
    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!label) {
        return Result<SharedPtr<Element>>::Failure(label.Error());
    }
    auto shaped = tree.Value().Get().Value()->TextShaper()->Shape(*label.Value(), text.Value(), 16);
    return shaped ? Result<SharedPtr<Element>>::Success(Move(label.Value()))
                  : Result<SharedPtr<Element>>::Failure(shaped.Error());
}

Result<SharedPtr<Element>> ComboBoxWidgetBase::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    auto combo = SharedPtr<ComboBox>::TryMake(allocator, allocator);
    if (!combo) {
        return Result<SharedPtr<Element>>::Failure(combo.Error());
    }
    auto height = combo.Value()->SetMaxListHeight(maxListHeight_);
    if (!height) {
        return Result<SharedPtr<Element>>::Failure(height.Error());
    }
    combo.Value()->SetOnOpening(
        Delegate<void()>::CreateLambda([&objects, owner = Handle()]() noexcept {
            auto widget = objects.ResolveForWrite<ComboBoxWidgetBase>(objects.OwnerAccess(), owner);
            if (widget) {
                widget.Value().Get().Value()->opening_.ExecuteIfBound();
            }
        }));
    combo.Value()->SetOptionFactory(ComboBox::MakeOption::CreateLambda(
        [&objects, owner = Handle()](usize i, bool display) noexcept {
            auto widget = objects.ResolveForWrite<ComboBoxWidgetBase>(objects.OwnerAccess(), owner);
            return widget ? widget.Value().Get().Value()->MakeOption(objects, i, display)
                          : Result<SharedPtr<Element>>::Failure(widget.Error());
        }));
    combo.Value()->SetOnSelectionChanged(Delegate<void(int32)>::CreateLambda(
        [&objects, owner = Handle(), native = combo.Value().Get()](int32 index) noexcept {
            auto widget = objects.ResolveForWrite<ComboBoxWidgetBase>(objects.OwnerAccess(), owner);
            if (widget) {
                auto* value = widget.Value().Get().Value();
                if (value->CachedElement().Get() == native) {
                    value->selectedIndex_ = index;
                    value->selectionChanged_.ExecuteIfBound(index);
                }
            }
        }));
    auto status = combo.Value()->Refresh(OptionCount(), selectedIndex_);
    return status ? Result<SharedPtr<Element>>::Success(Move(combo.Value()))
                  : Result<SharedPtr<Element>>::Failure(status.Error());
}

Result<void> ComboBoxWidgetBase::ClearSelection() noexcept {
    return SetSelectedIndex(-1);
}

Result<void> ComboBoxWidgetBase::RefreshOptions() noexcept {
    auto live = CachedElement();
    return live ? static_cast<ComboBox&>(*live).Refresh(OptionCount(), selectedIndex_)
                : Result<void>::Success();
}

Result<void> ComboBoxWidgetBase::Open() noexcept {
    auto live = CachedElement();
    return live ? static_cast<ComboBox&>(*live).Open() : Result<void>::Failure(ErrorCode::NotReady);
}

void ComboBoxWidgetBase::Close() noexcept {
    if (auto live = CachedElement()) {
        static_cast<ComboBox&>(*live).Close();
    }
}

void ComboBoxWidgetBase::SetOnOpening(Delegate<void()>&& callback) noexcept {
    opening_ = Move(callback);
}

Result<void> ComboBoxWidgetBase::SetMaxListHeight(float height) noexcept {
    if (!math::IsFinite(height) || height <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    maxListHeight_ = height;
    if (auto live = CachedElement()) {
        return static_cast<ComboBox&>(*live).SetMaxListHeight(height);
    }
    return Result<void>::Success();
}

bool ComboBoxWidgetBase::IsOpen() const noexcept {
    auto live = CachedElement();
    return live && static_cast<ComboBox&>(*live).IsOpen();
}

void ComboBoxWidgetBase::ReleaseElements() noexcept {
    if (auto live = CachedElement()) {
        static_cast<ComboBox&>(*live).DisconnectModel();
    }
    Widget::ReleaseElements();
}

} // namespace rocks::ui

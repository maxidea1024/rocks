#include <UiWidget/ListViewBaseWidget.generated.h>

namespace rocks::ui {

Result<void> ListViewBaseWidget::SetSource(Application& app,
                                           const SharedRef<TableRowSource>& source) noexcept {
    if (CachedElement()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    app_ = &app;
    source_ = source.AsSharedPtr();
    return Result<void>::Success();
}

Result<SharedPtr<Element>> ListViewBaseWidget::BuildElement(ObjectRegistry& objects) noexcept {
    if (!app_ || !source_) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::NotReady);
    }

    auto built = CreateView(objects.ObjectAllocator(), *app_, source_.ToSharedRef());
    if (built) {
        auto& view = StaticElementCast<ListView>(*built.Value());
        view.SetOnItemClicked(
            Delegate<void(uint64)>::CreateLambda([&objects, owner = Handle()](uint64 key) noexcept {
                auto widget =
                    objects.ResolveForWrite<ListViewBaseWidget>(objects.OwnerAccess(), owner);
                if (widget) {
                    widget.Value().Get().Value()->itemClicked_.ExecuteIfBound(key);
                }
            }));
        view.SetOnSelectionChanged(Delegate<void()>::CreateLambda([&objects,
                                                                   owner = Handle()]() noexcept {
            auto widget = objects.ResolveForWrite<ListViewBaseWidget>(objects.OwnerAccess(), owner);
            if (widget) {
                widget.Value().Get().Value()->selectionChanged_.ExecuteIfBound();
            }
        }));
        view.SetOnItemScrolledIntoView(
            Delegate<void(uint64)>::CreateLambda([&objects, owner = Handle()](uint64 key) noexcept {
                auto widget =
                    objects.ResolveForWrite<ListViewBaseWidget>(objects.OwnerAccess(), owner);
                if (widget) {
                    widget.Value().Get().Value()->itemScrolled_.ExecuteIfBound(key);
                }
            }));
    }

    return built;
}

void ListViewBaseWidget::ApplyProperties(Element& element) noexcept {
    auto& view = StaticElementCast<ListView>(element);
    view.SetSelectionMode(static_cast<ListSelectionMode>(selectionMode_));
    view.SetScrollIntoViewAlignment(static_cast<ScrollIntoViewAlignment>(scrollAlignment_));
    view.SetPointerScrolling(pointerScrolling_);
    view.SetSelectOnNavigation(selectOnNavigation_);
    (void)view.SetWheelMultiplier(wheelMultiplier_);
}

void ListViewBaseWidget::Refresh() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<ListView>(*live).Refresh();
    }
}

void ListViewBaseWidget::SetPointerScrolling(bool enabled) noexcept {
    pointerScrolling_ = enabled;
    if (auto live = CachedElement()) {
        StaticElementCast<ListView>(*live).SetPointerScrolling(enabled);
    }
}

void ListViewBaseWidget::SetSelectOnNavigation(bool enabled) noexcept {
    selectOnNavigation_ = enabled;
    if (auto live = CachedElement()) {
        StaticElementCast<ListView>(*live).SetSelectOnNavigation(enabled);
    }
}

Result<void> ListViewBaseWidget::SetWheelMultiplier(float multiplier) noexcept {
    if (!math::IsFinite(multiplier) || multiplier < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    wheelMultiplier_ = multiplier;
    if (auto live = CachedElement()) {
        return StaticElementCast<ListView>(*live).SetWheelMultiplier(multiplier);
    }

    return Result<void>::Success();
}

double ListViewBaseWidget::ScrollOffset() const noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ListView>(*live).ScrollOffset() : 0;
}

Result<void> ListViewBaseWidget::ScrollToStart() noexcept {
    return SetScrollOffset(0);
}

Result<void> ListViewBaseWidget::ScrollToEnd() noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ListView>(*live).ScrollToEnd()
                : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> ListViewBaseWidget::SetScrollOffset(double offset) noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ListView>(*live).SetScrollOffset(offset)
                : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> ListViewBaseWidget::ScrollIndexIntoView(uint32 index) noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ListView>(*live).ScrollIndexIntoView(index)
                : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> ListViewBaseWidget::NavigateToIndex(uint32 index, uint32 user) noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ListView>(*live).NavigateToIndex(index, user)
                : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> ListViewBaseWidget::SelectIndex(uint32 index) noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ListView>(*live).SelectIndex(index)
                : Result<void>::Failure(ErrorCode::NotReady);
}

void ListViewBaseWidget::SetOnItemClicked(Delegate<void(uint64)>&& callback) noexcept {
    itemClicked_ = Move(callback);
}

void ListViewBaseWidget::SetOnSelectionChanged(Delegate<void()>&& callback) noexcept {
    selectionChanged_ = Move(callback);
}

void ListViewBaseWidget::SetOnItemScrolledIntoView(Delegate<void(uint64)>&& callback) noexcept {
    itemScrolled_ = Move(callback);
}

void ListViewBaseWidget::SetSelectionMode(ListSelectionMode mode) noexcept {
    selectionMode_ = static_cast<uint8>(mode);
    if (auto live = CachedElement()) {
        StaticElementCast<ListView>(*live).SetSelectionMode(mode);
    }
}

void ListViewBaseWidget::SetScrollIntoViewAlignment(ScrollIntoViewAlignment align) noexcept {
    scrollAlignment_ = static_cast<uint8>(align);
    if (auto live = CachedElement()) {
        StaticElementCast<ListView>(*live).SetScrollIntoViewAlignment(align);
    }
}

bool ListViewBaseWidget::IsItemVisible(uint64 key) const noexcept {
    auto live = CachedElement();
    return live && StaticElementCast<ListView>(*live).IsItemVisible(key);
}

bool ListViewBaseWidget::IsRefreshPending() const noexcept {
    auto live = CachedElement();
    return live && StaticElementCast<ListView>(*live).IsRefreshPending();
}

Span<const uint64> ListViewBaseWidget::SelectedKeys() const noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ListView>(*live).SelectedKeys() : Span<const uint64>{};
}

Result<void> ListViewBaseWidget::SetItemSelection(uint64 key, bool selected) noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ListView>(*live).SetItemSelection(key, selected)
                : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> ListViewBaseWidget::ScrollItemIntoView(uint64 key) noexcept {
    auto live = CachedElement();
    if (!live) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto& view = StaticElementCast<ListView>(*live);
    auto index = view.FindIndex(key);
    return index ? view.ScrollIndexIntoView(index.Value()) : Result<void>::Failure(index.Error());
}

Result<void> ListViewBaseWidget::NavigateToItem(uint64 key, uint32 user) noexcept {
    auto live = CachedElement();
    if (!live) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto& view = StaticElementCast<ListView>(*live);
    auto index = view.FindIndex(key);
    return index ? view.NavigateToIndex(index.Value(), user) : Result<void>::Failure(index.Error());
}

void ListViewBaseWidget::CancelScrollIntoView() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<ListView>(*live).CancelScrollIntoView();
    }
}

void ListViewBaseWidget::ReleaseElements() noexcept {
    if (auto live = CachedElement()) {
        auto& view = StaticElementCast<ListView>(*live);
        view.SetOnItemClicked({});
        view.SetOnSelectionChanged({});
        view.SetOnItemScrolledIntoView({});
    }

    Widget::ReleaseElements();
}

void ListViewBaseWidget::ClearSelection() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<ListView>(*live).ClearSelection();
    }
}

Optional<uint64> ListViewBaseWidget::SelectedKey() const noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ListView>(*live).SelectedKey() : Optional<uint64>{};
}

} // namespace rocks::ui

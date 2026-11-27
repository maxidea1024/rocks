#include <Core/ScopeExit.h>
#include <UiScreens/ScreenTabListWidget.generated.h>
#include <Core/Text/NamePool.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetSwitcherWidget.generated.h>
#include <UiWidget/WidgetSlot.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/WidgetTextShaper.h>
#include <UiFramework/Layout/HorizontalStack.h>
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

namespace {

// 탭 상태 flag다(사용 가능·보임). 기본은 둘 다 켬이다.
constexpr uint8 TabEnabledFlag = 1u << 0;
constexpr uint8 TabVisibleFlag = 1u << 1;
constexpr uint8 DefaultTabFlags = TabEnabledFlag | TabVisibleFlag;
// 탭 버튼 안쪽 여백과 선택·보통 탭 색이다.
constexpr Vec4 TabButtonPadding{8, 4, 8, 4};
constexpr LinearColor ActiveTabColor{0.12f, 0.32f, 0.59f, 1};
constexpr LinearColor InactiveTabColor{0.16f, 0.16f, 0.16f, 1};

} // namespace

ScreenTabListWidget::ScreenTabListWidget(Allocator& allocator) noexcept
    : tabIds_(allocator), contents_(allocator), flags_(allocator) {
}

Span<const Name> ScreenTabListWidget::Tabs() const noexcept {
    return tabIds_.View();
}

Optional<usize> ScreenTabListWidget::FindTab(Name id) const noexcept {
    for (usize i = 0; i < tabIds_.Size(); ++i) {
        if (tabIds_[i] == id) {
            return i;
        }
    }

    return {};
}

uint8 ScreenTabListWidget::Flags(usize index) const noexcept {
    return index < flags_.Size() ? flags_[index] : DefaultTabFlags;
}

Optional<uint32> ScreenTabListWidget::PageIndex(ObjectRegistry& objects,
                                                ObjectHandle page) const noexcept {
    const auto access = objects.OwnerAccess();
    auto value = objects.Resolve<WidgetSwitcherWidget>(access, switcher_.Handle());
    if (!value) {
        return {};
    }

    auto* switcher = value.Value().Get().Value();
    for (usize i = 0; i < switcher->ChildCount(); ++i) {
        auto slot = objects.Resolve<WidgetSlot>(access, switcher->SlotAt(i));
        if (slot && slot.Value().Get().Value()->Content() == page) {
            return static_cast<uint32>(i);
        }
    }

    return {};
}

Name ScreenTabListWidget::SelectedTab(ObjectRegistry& objects) const noexcept {
    const auto access = objects.OwnerAccess();
    auto value = objects.Resolve<WidgetSwitcherWidget>(access, switcher_.Handle());
    if (!value) {
        return {};
    }

    auto* switcher = value.Value().Get().Value();
    if (switcher->ActiveIndex() >= switcher->ChildCount()) {
        return {};
    }

    auto slot = objects.Resolve<WidgetSlot>(access, switcher->SlotAt(switcher->ActiveIndex()));
    if (!slot) {
        return {};
    }

    const auto page = slot.Value().Get().Value()->Content();
    for (usize i = 0; i < contents_.Size() && i < tabIds_.Size(); ++i) {
        if (contents_.View()[i] == page) {
            return tabIds_[i];
        }
    }

    return {};
}

Result<void> ScreenTabListWidget::SetLinkedSwitcher(ObjectRegistry& objects,
                                                    ObjectHandle switcher) noexcept {
    if (switcher_.Handle() == switcher) {
        return Result<void>::Success();
    }

    if (!tabIds_.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    auto old = switcher_.Handle();
    auto oldHandle = activeChanged_;
    auto changed = objects.SetReference(objects.OwnerAccess(), Handle(), switcher_, switcher);
    if (!changed) {
        return changed;
    }

    activeChanged_.Reset();
    if (auto live = CachedElement()) {
        changed = Subscribe(objects, live);
        if (!changed) {
            (void)objects.SetReference(objects.OwnerAccess(), Handle(), switcher_, old);
            activeChanged_ = oldHandle;
            return changed;
        }
    }

    if (oldHandle.IsValid()) {
        auto oldSwitcher =
            objects.ResolveForWrite<WidgetSwitcherWidget>(objects.OwnerAccess(), old);
        if (oldSwitcher) {
            oldSwitcher.Value().Get().Value()->OnActivePageChanged().Remove(oldHandle);
        }
    }

    return changed;
}

Result<void> ScreenTabListWidget::RegisterTab(ObjectRegistry& objects, Name id,
                                              ObjectHandle page) noexcept {
    if (changing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    if (id.IsNone() || page.IsNull() || tabIds_.Size() != contents_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (FindTab(id)) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    for (auto existing : contents_.View()) {
        if (existing == page) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    const auto access = objects.OwnerAccess();
    auto value = objects.ResolveForWrite<WidgetSwitcherWidget>(access, switcher_.Handle());
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    auto* switcher = value.Value().Get().Value();
    const usize index = tabIds_.Size();
    auto reserved = tabIds_.TryEnsureCapacity(index + 1);
    if (!reserved) {
        return reserved;
    }

    auto added = objects.AddReference(access, Handle(), contents_, page);
    if (!added) {
        return added;
    }

    const bool attached = !PageIndex(objects, page);
    if (attached) {
        auto slot = switcher->AddChild(objects, page);
        if (!slot) {
            (void)objects.RemoveReferenceAt(access, Handle(), contents_, index);
            return Result<void>::Failure(slot.Error());
        }
    }

    (void)tabIds_.TryPushBack(id);
    if (auto view = CachedElement()) {
        auto built = Rebuild(objects, *view);
        if (!built) {
            (void)tabIds_.TryRemoveAt(index);
            (void)objects.RemoveReferenceAt(access, Handle(), contents_, index);
            if (attached) {
                (void)switcher->RemoveChild(objects, page);
            }

            return built;
        }
    }

    return Result<void>::Success();
}

Result<void> ScreenTabListWidget::RemoveTab(ObjectRegistry& objects, Name id) noexcept {
    if (changing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    auto index = FindTab(id);
    if (!index || *index >= contents_.Size()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto page = contents_.View()[*index];
    auto value =
        objects.ResolveForWrite<WidgetSwitcherWidget>(objects.OwnerAccess(), switcher_.Handle());
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    if (PageIndex(objects, page)) {
        auto removed = value.Value().Get().Value()->RemoveChild(objects, page);
        if (!removed) {
            return removed;
        }
    }

    auto removed = objects.RemoveReferenceAt(objects.OwnerAccess(), Handle(), contents_, *index);
    if (!removed) {
        return removed;
    }

    (void)tabIds_.TryRemoveAt(*index);
    if (*index < flags_.Size()) {
        (void)flags_.TryRemoveAt(*index);
    }

    if (auto view = CachedElement()) {
        auto* row = view->ChildAt(0);
        if (row && *index < row->ChildCount()) {
            (void)row->RemoveChild(*row->ChildAt(*index));
        }

        UpdateButtons(objects, *view);
    }

    return Result<void>::Success();
}

Result<void> ScreenTabListWidget::SelectTab(ObjectRegistry& objects, Name id) noexcept {
    auto index = FindTab(id);
    if (!index || *index >= contents_.Size()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (Flags(*index) != DefaultTabFlags) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto page = PageIndex(objects, contents_.View()[*index]);
    if (!page) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto value =
        objects.ResolveForWrite<WidgetSwitcherWidget>(objects.OwnerAccess(), switcher_.Handle());
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    auto* switcher = value.Value().Get().Value();
    bool changed = switcher->ActiveIndex() != *page;
    auto result = switcher->SetActiveIndex(*page);
    if (result) {
        if (auto view = CachedElement()) {
            UpdateButtons(objects, *view);
        }

        if (changed && !activeChanged_.IsValid()) {
            selected_.ExecuteIfBound(id);
        }
    }

    return result;
}

Result<void> ScreenTabListWidget::StepTab(ObjectRegistry& objects, bool forward) noexcept {
    const usize n = tabIds_.Size();
    if (!n) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto active = FindTab(SelectedTab(objects));
    usize index = active.ValueOr(forward ? n - 1 : 0);
    for (usize i = 0; i < n; ++i) {
        index = forward ? (index + 1) % n : (index ? index - 1 : n - 1);
        if (Flags(index) == DefaultTabFlags) {
            return SelectTab(objects, tabIds_[index]);
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ScreenTabListWidget::SetFlag(Name id, uint8 bit, bool enabled) noexcept {
    auto i = FindTab(id);
    if (!i) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto capacity = flags_.TryEnsureCapacity(tabIds_.Size());
    if (!capacity) {
        return capacity;
    }
    while (flags_.Size() < tabIds_.Size()) {
        (void)flags_.TryPushBack(DefaultTabFlags);
    }

    if (enabled) {
        flags_[*i] |= bit;
    } else {
        flags_[*i] &= static_cast<uint8>(~bit);
    }

    if (auto view = CachedElement(); view && registry_) {
        UpdateButtons(*registry_, *view);
    }

    return Result<void>::Success();
}

Result<void> ScreenTabListWidget::SetTabEnabled(Name id, bool enabled) noexcept {
    return SetFlag(id, 1, enabled);
}

Result<void> ScreenTabListWidget::SetTabVisible(Name id, bool visible) noexcept {
    return SetFlag(id, 2, visible);
}

void ScreenTabListWidget::SetOnTabSelected(Delegate<void(Name)>&& callback) noexcept {
    selected_ = Move(callback);
}

Result<void> ScreenTabListWidget::SetButtonFactory(ObjectRegistry& objects,
                                                   ButtonFactory&& factory) noexcept {
    if (changing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    auto previous = Move(factory_);
    factory_ = Move(factory);
    if (auto live = CachedElement()) {
        auto result = Rebuild(objects, *live);
        if (!result) {
            factory_ = Move(previous);
        }

        return result;
    }

    return Result<void>::Success();
}

Result<SharedPtr<Button>> ScreenTabListWidget::MakeButton(ObjectRegistry& objects,
                                                          Name id) noexcept {
    if (factory_) {
        return factory_.Execute(id);
    }

    auto& allocator = objects.ObjectAllocator();
    if (!objects.Names()) {
        return Result<SharedPtr<Button>>::Failure(ErrorCode::NotReady);
    }

    auto name = objects.Names()->Acquire(id);
    if (!name) {
        return Result<SharedPtr<Button>>::Failure(name.Error());
    }

    auto outer = objects.GetOuter(objects.OwnerAccess(), Handle());
    if (!outer) {
        return Result<SharedPtr<Button>>::Failure(outer.Error());
    }

    auto tree = objects.Resolve<WidgetTree>(objects.OwnerAccess(), outer.Value());
    if (!tree || !tree.Value().Get().Value()->TextShaper()) {
        return Result<SharedPtr<Button>>::Failure(ErrorCode::NotReady);
    }

    auto text = Text::Invariant(allocator, name.Value().View());
    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto button = SharedPtr<Button>::TryMake(allocator, allocator);
    if (!text || !label || !button) {
        return Result<SharedPtr<Button>>::Failure(!text    ? text.Error()
                                                  : !label ? label.Error()
                                                           : button.Error());
    }

    auto result = tree.Value().Get().Value()->TextShaper()->Shape(*label.Value(), text.Value(),
                                                                  DefaultWidgetFontSize);
    if (result) {
        result = button.Value()->SetPadding(TabButtonPadding);
    }

    if (result) {
        result = button.Value()->TryAddChild(label.Value());
    }

    return result ? Move(button) : Result<SharedPtr<Button>>::Failure(result.Error());
}

void ScreenTabListWidget::UpdateButtons(ObjectRegistry& objects, Element& host) noexcept {
    auto* row = host.ChildAt(0);
    auto active = SelectedTab(objects);
    for (usize i = 0; row && i < row->ChildCount() && i < tabIds_.Size(); ++i) {
        auto* button = StaticElementCast<Button>(row->ChildAt(i));
        auto flags = Flags(i);
        button->SetEnabled((flags & TabEnabledFlag) != 0);
        button->SetVisibility((flags & TabVisibleFlag) ? Visibility::Visible
                                                       : Visibility::Collapsed);
        button->SetColor(tabIds_[i] == active ? ActiveTabColor : InactiveTabColor);
    }
}

Result<void> ScreenTabListWidget::Rebuild(ObjectRegistry& objects, Element& host) noexcept {
    if (tabIds_.Size() != contents_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto& allocator = objects.ObjectAllocator();
    auto row = SharedPtr<HorizontalStack>::TryMake(allocator, allocator);
    if (!row) {
        return Result<void>::Failure(row.Error());
    }

    for (Name id : tabIds_) {
        auto button = MakeButton(objects, id);
        if (!button || !button.Value() || button.Value()->Parent()) {
            return Result<void>::Failure(button ? ErrorCode::InvalidArgument : button.Error());
        }

        // 캡처(레지스트리·handle·이름)가 inline 저장보다 커서 객체 할당기에 둔다.
        auto clicked = Delegate<void()>::TryCreateLambda(allocator, [&objects, owner = Handle(),
                                                                     id]() noexcept {
            auto value = objects.ResolveForWrite<ScreenTabListWidget>(objects.OwnerAccess(), owner);
            if (value) {
                (void)value.Value().Get().Value()->SelectTab(objects, id);
            }
        });
        if (!clicked) {
            return Result<void>::Failure(clicked.Error());
        }

        button.Value()->SetOnClicked(Move(clicked.Value()));
        auto attached = row.Value()->TryAddChild(button.Value());
        if (!attached) {
            return attached;
        }
    }

    auto old = host.ChildRefAt(0);
    if (old) {
        (void)host.RemoveChild(*old);
    }

    auto changed = host.TryAddChild(row.Value());
    if (!changed && old) {
        (void)host.TryAddChild(old);
    }

    if (changed) {
        UpdateButtons(objects, host);
    }

    return changed;
}

void ScreenTabListWidget::Unsubscribe(ObjectRegistry& objects) noexcept {
    if (activeChanged_.IsValid()) {
        auto value = objects.ResolveForWrite<WidgetSwitcherWidget>(objects.OwnerAccess(),
                                                                   switcher_.Handle());
        if (value) {
            value.Value().Get().Value()->OnActivePageChanged().Remove(activeChanged_);
        }
    }

    activeChanged_.Reset();
}

Result<void> ScreenTabListWidget::Subscribe(ObjectRegistry& objects,
                                            const SharedPtr<Element>& host) noexcept {
    if (switcher_.Handle().IsNull()) {
        return Result<void>::Success();
    }

    auto switcher =
        objects.ResolveForWrite<WidgetSwitcherWidget>(objects.OwnerAccess(), switcher_.Handle());
    if (!switcher) {
        return Result<void>::Failure(switcher.Error());
    }

    auto handle = switcher.Value().Get().Value()->OnActivePageChanged().TryAddWeakLambda(
        host, [&objects, owner = Handle()](uint32) noexcept {
            auto widget =
                objects.ResolveForWrite<ScreenTabListWidget>(objects.OwnerAccess(), owner);
            if (widget) {
                auto* tabs = widget.Value().Get().Value();
                if (auto live = tabs->CachedElement()) {
                    tabs->UpdateButtons(objects, *live);
                }

                tabs->selected_.ExecuteIfBound(tabs->SelectedTab(objects));
            }
        });
    if (!handle) {
        return Result<void>::Failure(handle.Error());
    }

    activeChanged_ = handle.Value();
    return Result<void>::Success();
}

Result<SharedPtr<Element>> ScreenTabListWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    auto host = SharedPtr<SizeBox>::TryMake(allocator, allocator);
    if (!host) {
        return Result<SharedPtr<Element>>::Failure(host.Error());
    }

    auto built = Rebuild(objects, *host.Value());
    if (!built) {
        return Result<SharedPtr<Element>>::Failure(built.Error());
    }

    if (registry_) {
        Unsubscribe(*registry_);
    }

    auto subscribed = Subscribe(objects, host.Value());
    if (!subscribed) {
        return Result<SharedPtr<Element>>::Failure(subscribed.Error());
    }

    registry_ = &objects;
    return Result<SharedPtr<Element>>::Success(Move(host.Value()));
}

void ScreenTabListWidget::ReleaseElements() noexcept {
    if (registry_) {
        Unsubscribe(*registry_);
    }

    registry_ = nullptr;
    Widget::ReleaseElements();
}
} // namespace rocks::ui

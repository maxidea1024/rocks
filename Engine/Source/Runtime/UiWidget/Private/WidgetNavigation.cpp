#include <UiWidget/Widget.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

namespace {

// 탐색 식별자는 위 32 bit가 위젯 slot, 아래 32 bit가 generation이다.
constexpr uint32 NavigationIdentitySlotShift = 32;

} // namespace

Result<ObjectHandle> Widget::PresentationTree(ObjectRegistry& objects) const noexcept {
    const auto access = objects.OwnerAccess();
    auto outer = objects.GetOuter(access, Handle());
    if (!outer) {
        return outer;
    }

    if (!outer.Value().IsNull()) {
        auto tree = objects.Resolve<WidgetTree>(access, outer.Value());
        if (tree) {
            return outer;
        }

        if (tree.Error() != ErrorCode::TypeMismatch) {
            return Result<ObjectHandle>::Failure(tree.Error());
        }
    }

    auto user = objects.Resolve<UserWidget>(access, Handle());
    if (user) {
        return Result<ObjectHandle>::Success(user.Value().Get().Value()->Tree());
    }

    return user.Error() == ErrorCode::TypeMismatch ? Result<ObjectHandle>::Success(ObjectHandle{})
                                                   : Result<ObjectHandle>::Failure(user.Error());
}

WidgetNavigationRule* Widget::NavigationEntry(FocusDirection direction) noexcept {
    switch (direction) {
    case FocusDirection::Left:
        return &navLeft_;
    case FocusDirection::Right:
        return &navRight_;
    case FocusDirection::Up:
        return &navUp_;
    case FocusDirection::Down:
        return &navDown_;
    case FocusDirection::Next:
        return &navNext_;
    case FocusDirection::Previous:
        return &navPrevious_;
    }

    return nullptr;
}

NavigationBoundary Widget::GetNavigationRule(FocusDirection direction) const noexcept {
    const auto* entry = const_cast<Widget*>(this)->NavigationEntry(direction);
    return entry ? NavigationBoundary{static_cast<NavigationRule>(entry->rule), entry->target}
                 : NavigationBoundary{};
}

Result<void> Widget::SetNavigationRule(FocusDirection direction, NavigationRule rule,
                                       Guid target) noexcept {
    auto* entry = NavigationEntry(direction);
    if (!entry || rule > NavigationRule::CustomBoundary ||
        (rule == NavigationRule::Explicit && target == Guid{})) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto set = live->SetNavigationRule(direction, {rule, target});
        if (!set) {
            return set;
        }
    }

    entry->rule = static_cast<uint8>(rule);
    entry->target = target;
    return Result<void>::Success();
}

Result<void> Widget::SetAllNavigationRules(NavigationRule rule) noexcept {
    if (rule == NavigationRule::Explicit || rule > NavigationRule::CustomBoundary) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint8 i = 0; i <= static_cast<uint8>(FocusDirection::Previous); ++i) {
        auto applied = SetNavigationRule(static_cast<FocusDirection>(i), rule);
        if (!applied) {
            return applied;
        }
    }

    return Result<void>::Success();
}

Result<void> Widget::SetNavigationHandler(ObjectRegistry& objects,
                                          NavigationHandler&& handler) noexcept {
    auto shared = SharedPtr<NavigationHandler>::TryMake(objects.ObjectAllocator(), Move(handler));
    if (!shared) {
        return Result<void>::Failure(shared.Error());
    }

    if (auto live = CachedElement()) {
        auto applied = live->SetNavigationHandler(NavigationHandler::CreateLambda(
            [cb = shared.Value()](uint32 user, FocusDirection direction) noexcept {
                return cb->IsBound() ? cb->Execute(user, direction) : Guid{};
            }));
        if (!applied) {
            return applied;
        }
    }

    navigationHandler_ = Move(shared.Value());
    return Result<void>::Success();
}

Result<void> Widget::ApplySemantics(ObjectRegistry& objects, Element& element) noexcept {
    const auto outer = PresentationTree(objects);
    if (!outer) {
        return Result<void>::Failure(outer.Error());
    }

    const auto tree = outer.Value();
    element.SetNavigationIdentity(
        {tree.registry, (uint64(tree.slot) << NavigationIdentitySlotShift) | tree.generation},
        stableId_);
    for (uint8 i = 0; i <= static_cast<uint8>(FocusDirection::Previous); ++i) {
        auto d = static_cast<FocusDirection>(i);
        auto set = element.SetNavigationRule(d, GetNavigationRule(d));
        if (!set) {
            return set;
        }
    }

    if (navigationHandler_) {
        auto set = element.SetNavigationHandler(NavigationHandler::CreateLambda(
            [cb = navigationHandler_](uint32 user, FocusDirection d) noexcept {
                return cb->IsBound() ? cb->Execute(user, d) : Guid{};
            }));
        if (!set) {
            return set;
        }
    }

    auto accessible = ApplyAccessibility(
        objects, element, accessibleText_, accessibleSummaryText_,
        static_cast<AccessibleBehavior>(accessibleBehavior_),
        static_cast<AccessibleBehavior>(accessibleSummaryBehavior_), childrenAccessible_);
    if (!accessible) {
        return accessible;
    }

    return ApplyToolTip(objects, element, toolTipText_, toolTipWidget_.Handle());
}

} // namespace rocks::ui

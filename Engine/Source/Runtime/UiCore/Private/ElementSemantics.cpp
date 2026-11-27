#include <UiCore/Element.h>

namespace rocks::ui {

class ElementSemantics {
public:
    explicit ElementSemantics(Allocator& allocator) noexcept
        : name(allocator), summary(allocator), tipText(allocator) {
    }

    NavigationBoundary rules[NavigationDirectionCount]{};
    SharedPtr<NavigationHandler> navigation;
    String name, summary, tipText;
    AccessibleBehavior behavior = AccessibleBehavior::Auto;
    AccessibleBehavior summaryBehavior = AccessibleBehavior::Auto;
    bool allowChildren = true;
    bool interactive = false;
    uint64 tipRevision = 0;
    SharedPtr<Element::ToolTipFactory> tip;
    SharedPtr<UiHostServices> host;
};

Result<void> Element::SetUiHostServices(const SharedPtr<UiHostServices>& services) noexcept {
    if (!semantics_ && !services) {
        return Result<void>::Success();
    }

    auto ready = EnsureSemantics();
    if (ready) {
        semantics_->host = services;
    }

    return ready;
}

const UiHostServices* Element::GetUiHostServices() const noexcept {
    return semantics_ ? semantics_->host.Get() : nullptr;
}

const UiHostServices* Element::FindUiHostServices() const noexcept {
    for (const Element* current = this; current; current = current->parent_) {
        if (current->semantics_ && current->semantics_->host) {
            return current->semantics_->host.Get();
        }
    }

    return nullptr;
}

bool Element::QueryPopupWorkArea(Vec2 window, Box2<float>& area) const noexcept {
    const UiHostServices* host = FindUiHostServices();
    return host && host->QueryPopupWorkArea(window, area);
}

SharedPtr<TextInputService> Element::FindTextInputService() const noexcept {
    const UiHostServices* host = FindUiHostServices();
    return host ? host->TextInput() : SharedPtr<TextInputService>{};
}

FrameTime Element::FindFrameTime() const noexcept {
    const UiHostServices* host = FindUiHostServices();
    return host ? host->Time() : FrameTime{};
}

Result<void> Element::EnsureSemantics() noexcept {
    if (!semantics_) {
        auto& alloc = children_.GetAllocator();
        auto made = SharedPtr<ElementSemantics>::TryMake(alloc, alloc);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        semantics_ = Move(made.Value());
    }

    return Result<void>::Success();
}

void Element::SetNavigationIdentity(Guid domain, Guid identity) noexcept {
    navigationDomain_ = domain;
    navigationIdentity_ = identity;
}

Guid Element::NavigationIdentity() const noexcept {
    return navigationIdentity_;
}

Guid Element::NavigationDomain() const noexcept {
    return navigationDomain_;
}

Result<void> Element::SetNavigationRule(FocusDirection direction,
                                        NavigationBoundary boundary) noexcept {
    if (direction > FocusDirection::Previous || boundary.rule > NavigationRule::CustomBoundary ||
        (boundary.rule == NavigationRule::Explicit && boundary.target == Guid{})) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!semantics_ && boundary.rule == NavigationRule::Escape) {
        return Result<void>::Success();
    }

    auto ready = EnsureSemantics();
    if (!ready) {
        return ready;
    }

    semantics_->rules[static_cast<usize>(direction)] = boundary;
    return Result<void>::Success();
}

NavigationBoundary Element::GetNavigationRule(FocusDirection direction) const noexcept {
    return semantics_ && direction <= FocusDirection::Previous
               ? semantics_->rules[static_cast<usize>(direction)]
               : NavigationBoundary{};
}

Result<void> Element::SetNavigationHandler(NavigationHandler&& handler) noexcept {
    auto ready = EnsureSemantics();
    if (!ready) {
        return ready;
    }

    if (!handler) {
        semantics_->navigation.Reset();
        return Result<void>::Success();
    }

    auto retained = SharedPtr<NavigationHandler>::TryMake(children_.GetAllocator(), Move(handler));
    if (!retained) {
        return Result<void>::Failure(retained.Error());
    }

    semantics_->navigation = Move(retained.Value());
    return Result<void>::Success();
}

Guid Element::ResolveCustomNavigation(uint32 user, FocusDirection direction) noexcept {
    auto handler = semantics_ ? semantics_->navigation : SharedPtr<NavigationHandler>{};
    return handler && handler->IsBound() ? handler->Execute(user, direction) : Guid{};
}

Result<void> Element::SetAccessibility(StringView name, StringView summary,
                                       AccessibleBehavior behavior,
                                       AccessibleBehavior summaryBehavior,
                                       bool allowChildren) noexcept {
    if (behavior > AccessibleBehavior::Excluded || summaryBehavior > AccessibleBehavior::Excluded) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!semantics_ && name.IsEmpty() && summary.IsEmpty() &&
        behavior == AccessibleBehavior::Auto && summaryBehavior == AccessibleBehavior::Auto &&
        allowChildren) {
        return Result<void>::Success();
    }

    auto ready = EnsureSemantics();
    if (!ready) {
        return ready;
    }

    String copiedName(children_.GetAllocator()), copiedSummary(children_.GetAllocator());
    auto n = copiedName.TryAssign(name);
    if (!n) {
        return n;
    }

    auto s = copiedSummary.TryAssign(summary);
    if (!s) {
        return s;
    }

    semantics_->name = Move(copiedName);
    semantics_->summary = Move(copiedSummary);
    semantics_->behavior = behavior;
    semantics_->summaryBehavior = summaryBehavior;
    semantics_->allowChildren = allowChildren;
    return Result<void>::Success();
}

bool Element::IsAccessible() const noexcept {
    return (!semantics_ || semantics_->behavior != AccessibleBehavior::Excluded) &&
           GetVisibility() != Visibility::Hidden && GetVisibility() != Visibility::Collapsed;
}

bool Element::AllowsAccessibleChildren() const noexcept {
    return !semantics_ || semantics_->allowChildren;
}

AccessibleRole Element::GetAccessibleRole() const noexcept {
    return AccessibleRole::Group;
}

StringView Element::DefaultAccessibleText() const noexcept {
    return {};
}

Result<void> Element::GetAccessibleText(String& output, bool summary) const noexcept {
    auto behavior = semantics_ ? (summary ? semantics_->summaryBehavior : semantics_->behavior)
                               : AccessibleBehavior::Auto;
    if (!IsAccessible() || behavior == AccessibleBehavior::Excluded) {
        output.Clear();
        return Result<void>::Success();
    }

    if (behavior == AccessibleBehavior::ToolTip) {
        return output.TryAssign(semantics_->tipText.View());
    }

    if (behavior == AccessibleBehavior::Custom) {
        return output.TryAssign(summary ? semantics_->summary.View() : semantics_->name.View());
    }

    if (behavior == AccessibleBehavior::Auto && !DefaultAccessibleText().IsEmpty()) {
        return output.TryAssign(DefaultAccessibleText());
    }

    String combined(output.GetAllocator());
    if (AllowsAccessibleChildren()) {
        for (const auto& child : ActiveChildren()) {
            String part(output.GetAllocator());
            auto got = child->GetAccessibleText(part, true);
            if (!got) {
                return got;
            }

            if (part.IsEmpty()) {
                continue;
            }

            if (!combined.IsEmpty()) {
                auto space = combined.TryAppend(" ");
                if (!space) {
                    return space;
                }
            }

            auto added = combined.TryAppend(part.View());
            if (!added) {
                return added;
            }
        }
    }

    output = Move(combined);
    return Result<void>::Success();
}

Result<void> Element::SetToolTipFactory(ToolTipFactory&& factory, StringView description,
                                        bool interactive) noexcept {
    if (!semantics_ && !factory && description.IsEmpty()) {
        return Result<void>::Success();
    }

    auto ready = EnsureSemantics();
    if (!ready) {
        return ready;
    }

    SharedPtr<ToolTipFactory> retained;
    if (factory) {
        auto made = SharedPtr<ToolTipFactory>::TryMake(children_.GetAllocator(), Move(factory));
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        retained = Move(made.Value());
    }

    auto set = semantics_->tipText.TryAssign(description);
    if (!set) {
        return set;
    }

    semantics_->interactive = interactive;
    semantics_->tip = Move(retained);
    ++semantics_->tipRevision;
    return Result<void>::Success();
}

bool Element::HasToolTip() const noexcept {
    return semantics_ && semantics_->tip && semantics_->tip->IsBound();
}

bool Element::IsToolTipInteractive() const noexcept {
    return semantics_ && semantics_->interactive;
}

uint64 Element::ToolTipRevision() const noexcept {
    return semantics_ ? semantics_->tipRevision : 0;
}

Result<SharedPtr<Element>> Element::MakeToolTip() noexcept {
    auto factory = semantics_ ? semantics_->tip : SharedPtr<ToolTipFactory>{};
    return factory && factory->IsBound() ? factory->Execute()
                                         : Result<SharedPtr<Element>>::Failure(ErrorCode::NotFound);
}
} // namespace rocks::ui

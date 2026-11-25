#include <UiFramework/Application.h>

namespace rocks::ui {
namespace {

struct DirectionalDistance {
    bool overlaps;
    double forward;
    double sideways;

    bool CloserThan(const DirectionalDistance& other) const noexcept {
        if (overlaps != other.overlaps) {
            return overlaps;
        }
        if (overlaps && forward != other.forward) {
            return forward < other.forward;
        }
        const double total = forward * forward + sideways * sideways;
        const double previous = other.forward * other.forward + other.sideways * other.sideways;
        return total < previous;
    }
};

} // namespace

Result<bool> Application::Navigate(uint32 userId, FocusDirection direction,
                                   const SharedPtr<Element>& scope) noexcept {
    if (direction > FocusDirection::Previous) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }
    return NavigateInternal(userId, direction, scope ? scope : root_, false);
}

Result<bool> Application::NavigateInternal(uint32 userId, FocusDirection direction,
                                           const SharedPtr<Element>& root, bool wrap) noexcept {
    if (!root || !IsFocusablePath(root.Get())) {
        return Result<bool>::Success(false);
    }
    Array<SharedPtr<Element>> targets(allocator_);
    auto collected = CollectFocusTargets(root, targets);
    if (!collected) {
        return Result<bool>::Failure(collected.Error());
    }
    if (targets.IsEmpty()) {
        return Result<bool>::Success(false);
    }
    auto focus = GetFocus(userId);
    const bool forward = direction != FocusDirection::Previous;
    auto move = [&](const SharedPtr<Element>& target) noexcept -> Result<bool> {
        if (!target || target == focus) {
            return Result<bool>::Success(false);
        }
        auto moved = SetFocus(userId, target);
        return moved ? Result<bool>::Success(true) : Result<bool>::Failure(moved.Error());
    };
    if (!focus || !focus->IsInputPathActive(*root, false)) {
        return move(targets[forward ? 0 : targets.Size() - 1]);
    }
    Element* boundary = root.Get();
    NavigationBoundary policy;
    for (auto* node = focus.Get(); node; node = node->Parent()) {
        policy = node->GetNavigationRule(direction);
        if (policy.rule != NavigationRule::Escape) {
            boundary = node;
            break;
        }
        if (node == root.Get()) {
            break;
        }
    }
    auto customOrExplicit = [&]() noexcept -> Result<bool> {
        auto domain = boundary->NavigationDomain();
        const Guid id = policy.rule == NavigationRule::Explicit
                            ? policy.target
                            : boundary->ResolveCustomNavigation(userId, direction);
        if (id == Guid{}) {
            return Result<bool>::Success(false);
        }
        for (const auto& target : targets) {
            if (target->NavigationIdentity() == id && target->NavigationDomain() == domain &&
                target->IsInputPathActive(*root, false)) {
                return move(target);
            }
        }
        return Result<bool>::Success(false);
    };
    if (policy.rule == NavigationRule::Custom) {
        return customOrExplicit();
    }
    const bool shouldWrap =
        policy.rule == NavigationRule::Wrap || (policy.rule == NavigationRule::Escape && wrap);
    if (direction == FocusDirection::Next || direction == FocusDirection::Previous) {
        SharedPtr<Element> first;
        bool seen = false;
        for (usize i = 0; i < targets.Size(); ++i) {
            const auto& target = targets[forward ? i : targets.Size() - 1 - i];
            if (!target->IsWithin(*boundary)) {
                continue;
            }
            if (!first) {
                first = target;
            }
            if (seen) {
                return move(target);
            }
            if (target == focus) {
                seen = true;
            }
        }
        if (policy.rule == NavigationRule::Explicit ||
            policy.rule == NavigationRule::CustomBoundary) {
            return customOrExplicit();
        }
        return shouldWrap ? move(first) : Result<bool>::Success(false);
    }
    auto origin = focus->GetGeometry().WindowBounds();
    if (!origin) {
        return Result<bool>::Failure(origin.Error());
    }
    const bool horizontal = direction == FocusDirection::Left || direction == FocusDirection::Right;
    const double sign =
        direction == FocusDirection::Left || direction == FocusDirection::Up ? -1.0 : 1.0;
    const auto axes = [horizontal](const Box2<float>& box, double& main, double& low,
                                   double& high) noexcept {
        main = horizontal ? (static_cast<double>(box.min.x) + box.max.x) * 0.5
                          : (static_cast<double>(box.min.y) + box.max.y) * 0.5;
        low = horizontal ? box.min.y : box.min.x;
        high = horizontal ? box.max.y : box.max.x;
    };
    double center, start, end;
    axes(origin.Value(), center, start, end);
    SharedPtr<Element> best;
    DirectionalDistance nearest{};
    SharedPtr<Element> opposite;
    DirectionalDistance farthest{};
    for (const auto& candidate : targets) {
        if (candidate.Get() == focus.Get() || !candidate->IsWithin(*boundary)) {
            continue;
        }
        const Geometry& geometry = candidate->GetGeometry();
        if (geometry.size.x <= 0 || geometry.size.y <= 0) {
            continue;
        }
        auto bounds = geometry.WindowBounds();
        if (!bounds) {
            continue;
        }
        double main, low, high;
        axes(bounds.Value(), main, low, high);
        DirectionalDistance distance{low < end && high > start, (main - center) * sign,
                                     (low + high - start - end) * 0.5};
        if (shouldWrap && distance.forward <= 0 &&
            (!opposite ||
             (distance.overlaps != farthest.overlaps
                  ? distance.overlaps
                  : (distance.forward < farthest.forward ||
                     (distance.forward == farthest.forward &&
                      math::Abs(distance.sideways) < math::Abs(farthest.sideways)))))) {
            opposite = candidate;
            farthest = distance;
        }
        if (distance.forward > 0 && (!best || distance.CloserThan(nearest))) {
            best = candidate;
            nearest = distance;
        }
    }
    if (best) {
        return move(best);
    }
    if (policy.rule == NavigationRule::Explicit || policy.rule == NavigationRule::CustomBoundary) {
        return customOrExplicit();
    }
    return shouldWrap ? move(opposite) : Result<bool>::Success(false);
}

} // namespace rocks::ui

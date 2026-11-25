#include <WorldIntegration/WorldWidgetInteraction.h>
#include <WorldIntegration/WorldWidgetComponent.generated.h>

namespace rocks::ui {

WorldWidgetInteraction::WorldWidgetInteraction(uint32 userId, uint32 pointerId) noexcept
    : userId_(userId), pointerId_(pointerId) {
}

Result<bool> WorldWidgetInteraction::Route(World& world, Span<const ObjectHandle> candidates,
                                           Vec3d origin, Vec3d direction, PointerEventType type,
                                           input::MouseButton button, float wheel) noexcept {
    if (!origin.IsFinite() || !direction.IsFinite() || !math::IsFinite(wheel)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    ObjectAccessScope scope(world.Access());
    ObjectHandle target = pressed_;
    auto& objects = world.Objects();
    if (!target.IsNull()) {
        auto captured = objects.ResolveForWrite<WorldWidgetComponent>(world.Access(), target);
        if (!captured || !captured.Value().Get().Value()->WindowApplication().HasPointerCapture(
                             PointerSource::Mouse, userId_, pointerId_)) {
            target = {};
            pressed_ = {};
        }
    }

    if (target.IsNull()) {
        double nearest = NumericLimits<double>::Max();
        for (const auto handle : candidates) {
            auto found = objects.Resolve<WorldWidgetComponent>(world.Access(), handle);
            if (!found) {
                continue;
            }

            const auto* component = found.Value().Get().Value();
            if (!component->IsEnabled() || component->IsScreenSpace() ||
                component->GetWidget().IsNull()) {
                continue;
            }

            auto point = component->RayToLocal(origin, direction);
            if (!point) {
                continue;
            }

            const auto p = point.Value(), size = component->DrawSize(), pivot = component->Pivot();
            if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) {
                continue;
            }

            const auto position = component->WorldMatrix().TransformPoint(
                {0, double(pivot.x) * size.x - p.x, double(pivot.y) * size.y - p.y});
            const auto delta = position - origin;
            const double distance = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
            if (distance < nearest) {
                nearest = distance;
                target = handle;
            }
        }
    }

    PointerEvent event{type, {}, pointerId_, userId_, PointerSource::Mouse, button, wheel};
    if (hovered_ != target && !hovered_.IsNull()) {
        auto old = objects.ResolveForWrite<WorldWidgetComponent>(world.Access(), hovered_);
        if (old) {
            auto& app = old.Value().Get().Value()->WindowApplication();
            auto leave = event;
            leave.position = {-1, -1};
            leave.type = PointerEventType::Move;
            (void)app.RoutePointer(leave);
        }
    }

    hovered_ = target;
    if (type == PointerEventType::Up || type == PointerEventType::Cancel) {
        pressed_ = {};
    }

    if (target.IsNull()) {
        return Result<bool>::Success(false);
    }

    auto found = objects.ResolveForWrite<WorldWidgetComponent>(world.Access(), target);
    if (!found) {
        return Result<bool>::Failure(found.Error());
    }

    auto routed = found.Value().Get().Value()->RoutePointerRay(origin, direction, event);
    if (routed && routed.Value() && type == PointerEventType::Down) {
        auto& app = found.Value().Get().Value()->WindowApplication();
        if (app.HasPointerCapture(PointerSource::Mouse, userId_, pointerId_)) {
            pressed_ = target;
        }

        if (app.GetFocus(userId_)) {
            focus_ = target;
        }
    }

    return routed;
}

Result<bool> WorldWidgetInteraction::RouteKey(World& world,
                                              const input::InputEvent& event) noexcept {
    if (focus_.IsNull()) {
        return Result<bool>::Success(false);
    }

    ObjectAccessScope scope(world.Access());
    auto found = world.Objects().ResolveForWrite<WorldWidgetComponent>(world.Access(), focus_);
    if (!found) {
        focus_ = {};
        return Result<bool>::Success(false);
    }

    return found.Value().Get().Value()->RouteVirtualKey(userId_, event);
}

void WorldWidgetInteraction::Reset(World& world) noexcept {
    (void)Route(world, {}, {}, {}, PointerEventType::Cancel);
    pressed_ = {};
    focus_ = {};
    hovered_ = {};
}
} // namespace rocks::ui

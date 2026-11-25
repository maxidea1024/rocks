#include <UiWidget/Widget.h>
#include <UiWidget/PanelWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiFramework/Application.h>

namespace rocks::ui {

bool Widget::IsVisible() const noexcept {
    const auto visibility = GetVisibility();
    return visibility != Visibility::Hidden && visibility != Visibility::Collapsed;
}

bool Widget::IsRendered() const noexcept {
    return IsVisible() && renderOpacity_ > 0;
}

bool Widget::ClipsToBounds() const noexcept {
    auto live = CachedElement();
    return live ? live->ClipsToBounds() : clipToBounds_ == 1;
}

float Widget::GetRenderOpacity() const noexcept {
    return renderOpacity_;
}

Result<void> Widget::SetRenderOpacity(float opacity) noexcept {
    if (!math::IsFinite(opacity) || opacity < 0 || opacity > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (renderOpacity_ == opacity) {
        return Result<void>::Success();
    }

    if (auto live = CachedElement()) {
        auto applied = live->SetRenderOpacity(opacity);
        if (!applied) {
            return applied;
        }
    }

    renderOpacity_ = opacity;
    NotifyPropertyChanged("renderOpacity");
    return Result<void>::Success();
}

const WidgetTransform& Widget::GetRenderTransform() const noexcept {
    return transform_;
}

Result<void> Widget::SetRenderTransform(const WidgetTransform& transform) noexcept {
    auto affine = transform.ToAffine();
    if (!affine) {
        return Result<void>::Failure(affine.Error());
    }

    if (auto live = CachedElement()) {
        auto result = live->SetRenderTransform(affine.Value(), renderPivot_);
        if (!result) {
            return result;
        }
    }

    transform_ = transform;
    return Result<void>::Success();
}

Result<void> Widget::SetRenderScale(Vec2 value) noexcept {
    auto transform = transform_;
    transform.scale = value;
    return SetRenderTransform(transform);
}

Result<void> Widget::SetRenderShear(Vec2 value) noexcept {
    auto transform = transform_;
    transform.shear = value;
    return SetRenderTransform(transform);
}

Result<void> Widget::SetRenderTranslation(Vec2 value) noexcept {
    auto transform = transform_;
    transform.translation = value;
    return SetRenderTransform(transform);
}

Result<void> Widget::SetRenderTransformAngle(float value) noexcept {
    auto transform = transform_;
    transform.angle = value;
    return SetRenderTransform(transform);
}

Vec2 Widget::GetRenderTransformPivot() const noexcept {
    return renderPivot_;
}

Result<void> Widget::SetRenderTransformPivot(Vec2 pivot) noexcept {
    if (!pivot.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto affine = transform_.ToAffine();
    if (!affine) {
        return Result<void>::Failure(affine.Error());
    }

    if (auto live = CachedElement()) {
        auto applied = live->SetRenderTransform(affine.Value(), pivot);
        if (!applied) {
            return applied;
        }
    }

    renderPivot_ = pivot;
    return Result<void>::Success();
}

Result<void> Widget::SetCursor(input::MouseCursor cursor) noexcept {
    if (cursor > input::MouseCursor::Wait) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    cursorOverride_ = static_cast<uint8>(cursor);
    if (auto live = CachedElement()) {
        live->SetCursorOverride(cursor);
    }

    return Result<void>::Success();
}

void Widget::ResetCursor() noexcept {
    cursorOverride_ = 255;
    if (auto live = CachedElement()) {
        live->ResetCursorOverride();
    }
}

bool Widget::IsHovered(const Application& app, uint32 user) const noexcept {
    auto live = CachedElement();
    return live && app.IsHovered(*live, user);
}

bool Widget::SupportsKeyboardFocus() const noexcept {
    auto live = CachedElement();
    return live && live->SupportsKeyboardFocus();
}

bool Widget::HasUserFocus(const Application& app, uint32 user) const noexcept {
    auto live = CachedElement();
    return live && app.GetFocus(user) == live;
}

bool Widget::HasFocusedDescendants(const Application& app, uint32 user) const noexcept {
    auto live = CachedElement();
    auto focus = app.GetFocus(user);
    return live && focus && focus != live && focus->IsWithin(*live);
}

bool Widget::HasAnyUserFocus(const Application& app) const noexcept {
    auto live = CachedElement();
    return live && app.HasAnyUserFocus(*live);
}

bool Widget::HasMouseCapture(const Application& app, uint32 user, uint32 pointer) const noexcept {
    auto live = CachedElement();
    return live && app.HasMouseCapture(*live, user, pointer);
}

Result<void> Widget::SetUserFocus(Application& app, uint32 user) noexcept {
    auto live = CachedElement();
    return live ? app.SetFocus(user, live) : Result<void>::Failure(ErrorCode::NotReady);
}

Vec2 Widget::GetDesiredSize() const noexcept {
    auto live = CachedElement();
    return live ? live->GetDesiredSize() : Vec2{};
}

Result<Geometry> Widget::GetCachedGeometry() const noexcept {
    auto live = CachedElement();
    return live ? Result<Geometry>::Success(live->GetGeometry())
                : Result<Geometry>::Failure(ErrorCode::NotReady);
}

Result<void> Widget::ForceLayoutPrepass(float scale) noexcept {
    auto live = CachedElement();
    return live ? live->Prepass(scale) : Result<void>::Failure(ErrorCode::NotReady);
}

void Widget::InvalidateLayout() noexcept {
    if (auto live = CachedElement()) {
        live->InvalidateLayout();
    }
}

Result<ObjectHandle> Widget::GetParent(ObjectRegistry& objects) const noexcept {
    return Slot().IsNull() ? Result<ObjectHandle>::Success(ObjectHandle{})
                           : objects.GetOuter(objects.OwnerAccess(), Slot());
}

Result<void> Widget::RemoveFromParent(ObjectRegistry& objects) noexcept {
    const auto access = objects.OwnerAccess();
    auto owner = GetParent(objects);
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    if (!owner.Value().IsNull()) {
        auto panel = objects.ResolveForWrite<PanelWidget>(access, owner.Value());
        return panel ? panel.Value().Get().Value()->RemoveChild(objects, Handle())
                     : Result<void>::Failure(panel.Error());
    }

    auto outer = objects.GetOuter(access, Handle());
    if (!outer) {
        return Result<void>::Failure(outer.Error());
    }

    if (!outer.Value().IsNull()) {
        auto tree = objects.ResolveForWrite<WidgetTree>(access, outer.Value());
        if (tree && tree.Value().Get().Value()->RootWidget() == Handle()) {
            return tree.Value().Get().Value()->SetRoot(objects, {});
        }
    }

    return Result<void>::Success();
}
} // namespace rocks::ui

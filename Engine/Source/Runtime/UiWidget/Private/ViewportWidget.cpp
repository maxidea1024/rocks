#include "MakeWidgetElement.h"
#include <UiWidget/ViewportWidget.h>
#include <UiFramework/Effects/Viewport.h>

namespace rocks::ui {

ViewportWidget::ViewportWidget(Allocator& allocator) noexcept : ContentWidget(allocator) {
}

Result<SharedPtr<ViewportWorld>>
ViewportWidget::AcquireViewportWorld(ObjectRegistry& objects) noexcept {
    using ResultType = Result<SharedPtr<ViewportWorld>>;
    if (auto existing = world_.Lock()) {
        return ResultType::Success(Move(existing));
    }

    auto& allocator = objects.ObjectAllocator();
    auto made = SharedPtr<ViewportWorld>::TryMake(allocator, allocator, objects.Types());
    if (!made) {
        return made;
    }

    auto ready = made.Value()->SetCamera(viewLocation_, viewRotation_, fieldOfView_);
    if (ready) {
        ready = made.Value()->Initialize();
    }

    if (!ready) {
        return ResultType::Failure(ready.Error());
    }

    made.Value()->SetBackground(background_);
    world_ = WeakPtr<ViewportWorld>(made.Value());
    return made;
}

Result<World*> ViewportWidget::GetViewportWorld(ObjectRegistry&) noexcept {
    auto world = world_.Lock();
    return world ? Result<World*>::Success(world->GetWorld())
                 : Result<World*>::Failure(ErrorCode::NotReady);
}

Result<SharedPtr<Element>> ViewportWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto world = AcquireViewportWorld(objects);
    if (!world) {
        return Result<SharedPtr<Element>>::Failure(world.Error());
    }

    auto panel = PanelWidget::BuildElement(objects);
    if (panel) {
        static_cast<Viewport&>(*panel.Value()).SetSceneSource(world.Value());
    }

    return panel;
}

Result<ObjectHandle> ViewportWidget::Spawn(ObjectRegistry&, TypeId actorType) noexcept {
    auto lease = world_.Lock();
    if (!lease) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotReady);
    }

    return lease->GetWorld()->SpawnActor(actorType);
}

Result<void> ViewportWidget::SetSceneRenderer(ObjectRegistry&, ViewportWorld::Renderer renderer,
                                              bool depth) noexcept {
    auto lease = world_.Lock();
    if (!lease) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    lease->SetRenderer(Move(renderer), depth);
    return Result<void>::Success();
}

Result<void> ViewportWidget::SetCamera(Vec3 location, Quat rotation, float fieldOfView) noexcept {
    if (!location.IsFinite() || !rotation.IsNormalized(1.0e-3f) || !math::IsFinite(fieldOfView) ||
        fieldOfView <= 0 || fieldOfView >= 180) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    viewLocation_ = location;
    viewRotation_ = rotation;
    fieldOfView_ = fieldOfView;
    auto world = world_.Lock();
    return world ? world->SetCamera(location, rotation, fieldOfView) : Result<void>::Success();
}

Vec3 ViewportWidget::ViewLocation() const noexcept {
    return viewLocation_;
}

Quat ViewportWidget::ViewRotation() const noexcept {
    return viewRotation_;
}

float ViewportWidget::FieldOfView() const noexcept {
    return fieldOfView_;
}

Result<SharedPtr<Element>> ViewportWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<Viewport>(allocator);
}

Result<void> ViewportWidget::SetBackground(LinearColor color) noexcept {
    if (!color.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    background_ = color;
    if (auto world = world_.Lock()) {
        world->SetBackground(color);
    }

    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

void ViewportWidget::SetSceneTexture(SlotHandle texture) noexcept {
    scene_ = texture;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
}

void ViewportWidget::ApplyProperties(Element& element) noexcept {
    auto& native = static_cast<Viewport&>(element);
    (void)native.SetBackground(background_);
    native.SetSceneTexture(scene_);
}

} // namespace rocks::ui

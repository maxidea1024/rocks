#include <WorldIntegration/WorldWidgetComponent.generated.h>
#include <WorldIntegration/WorldIntegration.module.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <World/World.h>

namespace rocks::ui {

Result<void> RegisterWorldIntegrationTypes(ModuleRegistration& registration) noexcept {
    return generated::WorldIntegration::RegisterModuleTypes(registration);
}

WorldWidgetComponent::WorldWidgetComponent(Allocator& allocator) noexcept
    : SceneComponent(allocator), window_(allocator) {
}

ObjectHandle WorldWidgetComponent::GetWidget() const noexcept {
    return widget_.Handle();
}

Vec2 WorldWidgetComponent::DrawSize() const noexcept {
    return drawSize_;
}

Vec2 WorldWidgetComponent::Pivot() const noexcept {
    return pivot_;
}

bool WorldWidgetComponent::IsScreenSpace() const noexcept {
    return screenSpace_;
}

Application& WorldWidgetComponent::WindowApplication() noexcept {
    return window_;
}

SharedPtr<Element> WorldWidgetComponent::WindowRoot() const noexcept {
    return root_;
}

Result<void> WorldWidgetComponent::SetWidget(ObjectHandle widget) noexcept {
    if (!HasWorld()) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (widget == widget_.Handle()) {
        return Result<void>::Success();
    }

    auto& objects = GetWorld().Objects();
    auto result = objects.SetReference(objects.OwnerAccess(), Handle(), widget_, widget);
    if (result) {
        ResetWindow();
    }

    return result;
}

Result<void> WorldWidgetComponent::SetDrawSize(Vec2 pixels) noexcept {
    if (!pixels.IsFinite() || pixels.x < 1 || pixels.y < 1 ||
        double(pixels.x) > NumericLimits<uint32>::Max() ||
        double(pixels.y) > NumericLimits<uint32>::Max() || pixels.x != math::Floor(pixels.x) ||
        pixels.y != math::Floor(pixels.y)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    drawSize_ = pixels;
    return Result<void>::Success();
}

Result<void> WorldWidgetComponent::SetPivot(Vec2 pivot) noexcept {
    if (!pivot.IsFinite() || pivot.x < 0 || pivot.x > 1 || pivot.y < 0 || pivot.y > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    pivot_ = pivot;
    return Result<void>::Success();
}

void WorldWidgetComponent::DetachLayer() noexcept {
    if (auto canvas = layer_.Lock(); canvas && root_ && root_->Parent() == canvas.Get()) {
        canvas->RemoveChild(*root_);
    }

    layer_ = {};
}

void WorldWidgetComponent::SetScreenSpace(bool enabled, uint32 playerId) noexcept {
    if (screenSpace_ != enabled || playerId_ != playerId) {
        DetachLayer();
        window_.SetRoot({});
        screenSpace_ = enabled;
        playerId_ = playerId;
        if (!screenSpace_ && root_) {
            window_.SetRoot(root_);
        }
    }
}

Result<void> WorldWidgetComponent::EnsureRoot() noexcept {
    if (!IsEnabled() || !HasWorld() || widget_.Handle().IsNull()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (root_) {
        return Result<void>::Success();
    }

    auto& objects = GetWorld().Objects();
    auto widget = objects.ResolveForWrite<UserWidget>(objects.OwnerAccess(), widget_.Handle());
    if (!widget) {
        return Result<void>::Failure(widget.Error());
    }

    auto built = widget.Value().Get().Value()->TakeElement(objects);
    if (!built) {
        return Result<void>::Failure(built.Error());
    }

    if (built.Value()->Parent()) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    root_ = Move(built.Value());
    if (!screenSpace_) {
        window_.SetRoot(root_);
    }

    return Result<void>::Success();
}

Result<void> WorldWidgetComponent::PrepareWindow() noexcept {
    if (screenSpace_) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    auto root = EnsureRoot();
    if (!root) {
        return root;
    }

    return window_.PrepareLayout(Geometry::Root(drawSize_, {}), 1);
}

void WorldWidgetComponent::ResetWindow() noexcept {
    DetachLayer();
    window_.SetRoot({});
    root_ = {};
}

void WorldWidgetComponent::OnDisable() noexcept {
    ResetWindow();
}

void WorldWidgetComponent::OnDestroy(EndPlayReason) noexcept {
    ResetWindow();
}

void WorldWidgetComponent::PlaneCorners(Vec3d& topLeft, Vec3d& topRight,
                                        Vec3d& bottomLeft) const noexcept {
    const Mat4d transform = WorldMatrix();
    const double y = double(pivot_.x) * drawSize_.x, z = double(pivot_.y) * drawSize_.y;
    topLeft = transform.TransformPoint({0, y, z});
    topRight = transform.TransformPoint({0, y - drawSize_.x, z});
    bottomLeft = transform.TransformPoint({0, y, z - drawSize_.y});
}

Result<Vec2> WorldWidgetComponent::RayToLocal(Vec3d origin, Vec3d direction) const noexcept {
    if (!origin.IsFinite() || !direction.IsFinite()) {
        return Result<Vec2>::Failure(ErrorCode::InvalidArgument);
    }

    auto inverse = WorldMatrix().TryInverse();
    if (!inverse) {
        return Result<Vec2>::Failure(inverse.Error());
    }

    const auto at = inverse.Value().TransformPoint(origin);
    const auto ray = inverse.Value().TransformVector(direction);
    if (ray.x == 0) {
        return Result<Vec2>::Failure(ErrorCode::NotFound);
    }

    const double distance = -at.x / ray.x;
    if (distance < 0 || !math::IsFinite(distance)) {
        return Result<Vec2>::Failure(ErrorCode::NotFound);
    }

    const auto hit = at + ray * distance;
    Vec2 local{static_cast<float>(double(pivot_.x) * drawSize_.x - hit.y),
               static_cast<float>(double(pivot_.y) * drawSize_.y - hit.z)};
    if (!local.IsFinite()) {
        return Result<Vec2>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<Vec2>::Success(local);
}

Result<bool> WorldWidgetComponent::RoutePointerRay(Vec3d origin, Vec3d direction,
                                                   PointerEvent event) noexcept {
    auto ready = PrepareWindow();
    if (!ready) {
        return Result<bool>::Failure(ready.Error());
    }

    auto cache = window_.UpdateArrangedPaintCache();
    if (!cache) {
        return Result<bool>::Failure(cache.Error());
    }

    auto local = RayToLocal(origin, direction);
    if (!local) {
        if (local.Error() != ErrorCode::NotFound) {
            return Result<bool>::Failure(local.Error());
        }

        event.type = PointerEventType::Cancel;
        event.position = {};
    } else {
        event.position = local.Value();
    }

    return window_.RoutePointer(event);
}

Result<bool> WorldWidgetComponent::RouteVirtualKey(uint32 userId,
                                                   const input::InputEvent& event) noexcept {
    auto ready = PrepareWindow();
    if (!ready) {
        return Result<bool>::Failure(ready.Error());
    }

    return window_.RouteKey(userId, event);
}

Result<bool> WorldWidgetComponent::ProjectToPlayer(uint32 playerId, const Mat4d& viewProjection,
                                                   Vec2 viewportSize,
                                                   const SharedPtr<AnchorCanvas>& layer) noexcept {
    if (!screenSpace_ || !layer || !viewportSize.IsFinite() || viewportSize.x <= 0 ||
        viewportSize.y <= 0) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (playerId != playerId_) {
        return Result<bool>::Success(false);
    }

    const auto position = WorldMatrix().TransformPoint({});
    const auto clip = viewProjection.TransformVector4({position.x, position.y, position.z, 1});
    if (!clip.IsFinite()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (!IsEnabled() || clip.w <= 0 || clip.z < 0 || clip.z > clip.w ||
        math::Abs(clip.x) > clip.w || math::Abs(clip.y) > clip.w) {
        DetachLayer();
        return Result<bool>::Success(false);
    }

    auto ready = EnsureRoot();
    if (!ready) {
        return Result<bool>::Failure(ready.Error());
    }

    const float x = static_cast<float>((clip.x / clip.w + 1) * 0.5) * viewportSize.x;
    const float y = static_cast<float>((1 - clip.y / clip.w) * 0.5) * viewportSize.y;
    CanvasSlot slot;
    slot.offset = {x, y, drawSize_.x, drawSize_.y};
    slot.alignment = pivot_;
    if (layer_.Lock().Get() != layer.Get()) {
        DetachLayer();
    }

    auto placed = root_->Parent() == layer.Get() ? layer->SetSlot(*root_, slot)
                                                 : layer->TryAddSlot(root_, slot);
    if (!placed) {
        return Result<bool>::Failure(placed.Error());
    }

    layer_ = layer;
    return Result<bool>::Success(true);
}

} // namespace rocks::ui

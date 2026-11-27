#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>
#include <World/WorldLog.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>

namespace rocks {

namespace {

bool HasZeroComponent(const Vec3d& scale) noexcept {
    return scale.x == 0.0 || scale.y == 0.0 || scale.z == 0.0;
}

// UE FTransform A * B(child 먼저, parent 나중)의 성분별 합성이다(shear를 버린다 — ERD31): 회전
// Qp·Qc, scale Sc·Sp, 위치 Qp(Sp·Tc) + Tp.
Transform ComposeTrs(const Transform& child, const Transform& parent) noexcept {
    Transform world;
    world.rotation = parent.rotation * child.rotation;
    world.scale = child.scale * parent.scale;
    world.translation =
        parent.rotation.RotateVector(parent.scale * child.translation) + parent.translation;
    return world;
}

// UE FTransform::GetRelativeTransform의 성분별 식이다: 회전 Qp⁻¹·Qc, scale Sc ÷ Sp, 위치
// Qp⁻¹(Tc − Tp) ÷ Sp. 부모 scale에 0이 있으면 InvalidArgument다.
Result<Transform> RelativeTrs(const Transform& world, const Transform& parent) noexcept {
    if (HasZeroComponent(parent.scale)) {
        return Result<Transform>::Failure(ErrorCode::InvalidArgument);
    }

    Transform local;
    local.rotation = parent.rotation.Conjugate() * world.rotation;
    local.scale = world.scale / parent.scale;
    local.translation =
        parent.rotation.UnrotateVector(world.translation - parent.translation) / parent.scale;
    return Result<Transform>::Success(local);
}

} // namespace

Result<void> SceneComponent::SetRelativeTransform(const Transform& transform) noexcept {
    if (!transform.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto movable = CheckMovable("move"); !movable) {
        return movable;
    }

    relative_ = transform;
    MarkDirty();
    NotifyTransformUpdated();
    return Result<void>::Success();
}

bool SceneComponent::IsPastGameInitialization() const noexcept {
    // 등록된 게임 World Component이고 소유 Actor가 초기화된 뒤다(Spawn·OnAwake 안은 아니다).
    if (!HasWorld() || !IsRegistered()) {
        return false;
    }

    World& world = GetWorld();
    if (world.Kind() != WorldKind::Game && world.Kind() != WorldKind::Tryout) {
        return false;
    }

    auto state = world.GetActorState(GetOwner());
    return state &&
           (state.Value() == ActorState::Initialized || state.Value() == ActorState::Playing);
}

Result<void> SceneComponent::CheckDynamicDataChange(const char* property,
                                                    bool blockStationary) const noexcept {
    // UE AreDynamicDataChangesAllowed(SceneComponent.h:1363): Static은 늘, Stationary는
    // blockStationary일 때 막는다.
    const bool blocked = mobility_ == ComponentMobility::Static ||
                         (blockStationary && mobility_ == ComponentMobility::Stationary);
    if (!blocked || !IsPastGameInitialization()) {
        return Result<void>::Success();
    }

    LogContextScope context("World", GetWorld().Name());
    Log::Warning(LogWorld,
                 "Scene component #{Slot} cannot change {Property} at runtime; mobility is "
                 "{Mobility} ({Code})",
                 Handle().slot, StringView(property),
                 mobility_ == ComponentMobility::Static ? "Static" : "Stationary",
                 DiagStaticComponentDataChanged);
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void>
SceneComponent::CheckChangeBeforeGameInitialization(const char* property) const noexcept {
    if (!IsPastGameInitialization()) {
        return Result<void>::Success();
    }

    LogContextScope context("World", GetWorld().Name());
    Log::Warning(LogWorld,
                 "Scene component #{Slot} cannot change {Property} after initialization ({Code})",
                 Handle().slot, StringView(property), DiagComponentDataLockedInGame);
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

void SceneComponent::MarkRenderStateDirtyIfPresent() noexcept {
    if (HasSystemState(SystemStateKind::Render)) {
        auto marked = MarkSystemStateDirty(SystemStateKind::Render);
        (void)marked;
    }
}

Result<void> SceneComponent::CheckMovable(const char* action) const noexcept {
    // UE CheckStaticMobilityAndWarn(SceneComponent.cpp:3383): 등록된 게임 World Component이고 소유
    // Actor가 초기화된 뒤에만 막는다(Spawn·OnAwake 안의 배치는 된다).
    if (mobility_ == ComponentMobility::Movable || !IsPastGameInitialization()) {
        return Result<void>::Success();
    }

    World& world = GetWorld();
    LogContextScope context("World", world.Name());
    Log::Warning(LogWorld,
                 "Scene component #{Slot} must be Movable to {Action}; mobility is {Mobility} "
                 "({Code})",
                 Handle().slot, StringView(action),
                 mobility_ == ComponentMobility::Static ? "Static" : "Stationary",
                 DiagStaticComponentMoved);
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

void SceneComponent::NotifyTransformUpdated() noexcept {
    PropagateTransformUpdated();
    // 루트 통지 끝에 overlap을 갱신한다(UE는 transform setter가 MoveComponent를 거쳐 갱신한다 —
    // PhysicsModule 7.1.11절). 자식 전파 중에는 부르지 않는다(루트의 갱신이 자식으로 내려간다).
    if (overlapUpdateDeferred_) {
        overlapUpdateSkipped_ = true;
        return;
    }

    UpdateOverlaps();
}

void SceneComponent::UpdateOverlaps() noexcept {
    if (children_.Size() == 0 || !HasWorld()) {
        return;
    }

    World& world = GetWorld();
    auto state = world.GetActorState(GetOwner());
    if (!state || state.Value() != ActorState::Playing) {
        return;
    }

    // overlap 알림이 자식 목록을 바꿀 수 있으므로 복사본으로 돈다(UE AttachedChildren 복사). 복사
    // 할당이 실패하면 handle을 하나씩 다시 읽는다(발행 중 목록이 줄면 일부를 건너뛸 수 있다).
    Array<ObjectHandle> children(children_.GetAllocator());
    if (children.TryAppend(children_.View())) {
        for (const ObjectHandle child : children) {
            (void)world.With<SceneComponent>(
                child, [](SceneComponent& scene) noexcept { scene.UpdateOverlaps(); });
        }

        return;
    }

    for (usize index = 0; index < children_.Size(); ++index) {
        const ObjectHandle child = children_[index];
        (void)world.With<SceneComponent>(
            child, [](SceneComponent& scene) noexcept { scene.UpdateOverlaps(); });
    }
}

void SceneComponent::PropagateTransformUpdated() noexcept {
    boundsDirty_ = true;
    if (HasSystemState(SystemStateKind::Render)) {
        auto marked = MarkSystemStateDirty(SystemStateKind::Render);
        (void)marked;
    }

    OnTransformUpdated();
    if (children_.Size() == 0 || !HasWorld()) {
        return;
    }

    World& world = GetWorld();
    for (usize index = 0; index < children_.Size(); ++index) {
        const ObjectHandle child = children_[index];
        auto notified = world.With<SceneComponent>(child, [](SceneComponent& component) noexcept {
            component.PropagateTransformUpdated();
        });
        (void)notified;
    }
}

Result<void> SceneComponent::SetRelativeLocation(const Vec3d& location) noexcept {
    Transform transform = relative_;
    transform.translation = location;
    return SetRelativeTransform(transform);
}

Result<void> SceneComponent::SetRelativeRotation(const Quatd& rotation) noexcept {
    Transform transform = relative_;
    transform.rotation = rotation;
    return SetRelativeTransform(transform);
}

Result<void> SceneComponent::SetRelativeScale(const Vec3d& scale) noexcept {
    Transform transform = relative_;
    transform.scale = scale;
    return SetRelativeTransform(transform);
}

void SceneComponent::MarkDirty() noexcept {
    boundsDirty_ = true;
    // 깨끗한 노드의 조상은 모두 깨끗하다(계산이 부모부터 이뤄지므로). 따라서 이미 dirty인 노드의
    // 자손은 모두 dirty이며 더 내려갈 필요가 없다.
    if (worldDirty_) {
        return;
    }

    worldDirty_ = true;
    // 자식은 World가 붙인 경우에만 있다(장부 밖 객체는 children이 비어 있다).
    if (children_.Size() == 0 || !HasWorld()) {
        return;
    }

    World& world = GetWorld();
    for (ObjectHandle child : children_) {
        auto marked = world.With<SceneComponent>(
            child, [](SceneComponent& component) noexcept { component.MarkDirty(); });
        (void)marked;
    }
}

Mat4d SceneComponent::ParentFrameMatrix() const noexcept {
    // 부모는 World가 붙인 경우에만 있다(장부 밖 객체는 루트로 계산한다).
    if (parent_.IsNull() || !HasWorld()) {
        return Mat4d();
    }

    const StringView socket = attachSocket_.View();
    auto frame =
        GetWorld().With<SceneComponent>(parent_, [socket](SceneComponent& parent) noexcept {
            Mat4d matrix = parent.WorldMatrix();
            auto local = parent.GetSocketTransform(socket, TransformSpace::Component);
            if (local) {
                matrix = local.Value().ToMatrix() * matrix;
            }

            return matrix;
        });
    return frame ? frame.Value() : Mat4d();
}

Result<void> SceneComponent::SetWorldLocationAndRotation(const Vec3d& location,
                                                         const Quatd& rotation) noexcept {
    // UE InternalSetWorldLocationAndRotation(SceneComponent.cpp:3265): 월드 값을 부모 frame 기준
    // 상대 값으로 바꾼다. 위치는 부모 frame 행렬의 역(Rocks 월드 행렬과 짝), 회전은 (부모 회전 ×
    // socket 회전)⁻¹ × 회전이다. 절대 성분은 월드 값을 그대로 둔다.
    auto unit = rotation.TryNormalize();
    if (!unit || !math::IsFinite(location.x) || !math::IsFinite(location.y) ||
        !math::IsFinite(location.z)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Transform relative = relative_;
    relative.translation = location;
    relative.rotation = unit.Value();
    if (!parent_.IsNull() && HasWorld()) {
        if (!absoluteLocation_) {
            auto inverse = ParentFrameMatrix().TryInverse();
            if (!inverse) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            relative.translation = inverse.Value().TransformPoint(location);
        }

        if (!absoluteRotation_) {
            const StringView socket = attachSocket_.View();
            auto frameRotation =
                GetWorld().With<SceneComponent>(parent_, [socket](SceneComponent& parent) noexcept {
                    Quatd parentRotation = parent.WorldQuaternion();
                    auto local = parent.GetSocketTransform(socket, TransformSpace::Component);
                    return local ? parentRotation * local.Value().rotation : parentRotation;
                });
            if (frameRotation) {
                auto normalized = (frameRotation.Value().Conjugate() * unit.Value()).TryNormalize();
                relative.rotation = normalized ? normalized.Value() : unit.Value();
            }
        }
    }

    return SetRelativeTransform(relative);
}

void SceneComponent::NotifyChildSocketsChanged() noexcept {
    // UE UpdateChildTransforms: 자식의 월드 행렬을 무효로 하고 Transform 통지를 보낸다.
    if (children_.Size() == 0 || !HasWorld()) {
        return;
    }

    World& world = GetWorld();
    for (usize index = 0; index < children_.Size(); ++index) {
        auto notified =
            world.With<SceneComponent>(children_[index], [](SceneComponent& component) noexcept {
                component.MarkDirty();
                component.NotifyTransformUpdated();
            });
        (void)notified;
    }
}

Mat4d SceneComponent::WorldMatrix() const noexcept {
    if (!worldDirty_) {
        return worldMatrix_;
    }

    // 부모 frame = 부모 기준 socket Transform × 부모 월드다. socket 이름이 비어도 부모에게
    // 묻는다(UE — SpringArm은 이름 없는 자식도 팔 끝에 둔다, Docs/Design/EngineRuntime 26.2절).
    const Mat4d parentMatrix = ParentFrameMatrix();

    // 행 벡터 규약: 상대 변환을 먼저, 부모 월드 변환을 나중에 적용한다(UE NewRelativeTransform *
    // ParentToWorld). 절대 성분이 있으면 TRS로 합성해 그 성분만 상대 값으로 바꾼다(23.3절 ERD31 —
    // 부모 행렬이 TRS로 분해되지 않으면 절대 성분 없이 행렬 곱이다).
    worldMatrix_ = relative_.ToMatrix() * parentMatrix;
    if ((absoluteLocation_ || absoluteRotation_ || absoluteScale_) && !parent_.IsNull()) {
        // 곱을 TRS로 분해하고, shear로 실패하면 부모 TRS와 성분별로 합성한다(UE ComposeTransforms —
        // 절대 성분은 shear 없는 TRS 의미다). 부모도 분해되지 않으면 절대 성분 없이 행렬 곱이다.
        auto composed = Transform::TryFromMatrix(worldMatrix_);
        if (!composed) {
            auto parentTrs = Transform::TryFromMatrix(parentMatrix);
            if (parentTrs) {
                composed = Result<Transform>::Success(ComposeTrs(relative_, parentTrs.Value()));
            }
        }

        if (composed) {
            Transform world = composed.Value();
            if (absoluteLocation_) {
                world.translation = relative_.translation;
            }

            if (absoluteRotation_) {
                world.rotation = relative_.rotation;
            }

            if (absoluteScale_) {
                world.scale = relative_.scale;
            }

            worldMatrix_ = world.ToMatrix();
        }
    }

    worldDirty_ = false;
    return worldMatrix_;
}

Result<Transform> SceneComponent::GetSocketTransform(StringView,
                                                     TransformSpace space) const noexcept {
    // 기본 Component에는 socket이 없다: 자신의 Transform이다(UE 없는 socket 처리와 같다).
    if (space == TransformSpace::Component) {
        return Result<Transform>::Success(Transform::Identity());
    }

    return TryWorldTransform();
}

Result<void> SceneComponent::SetAbsolute(bool location, bool rotation, bool scale) noexcept {
    // 월드 Transform이 바뀌므로 이동과 같은 Mobility 검사를 한다.
    if (auto movable = CheckMovable("change absolute flags"); !movable) {
        return movable;
    }

    absoluteLocation_ = location;
    absoluteRotation_ = rotation;
    absoluteScale_ = scale;
    worldDirty_ = false;
    MarkDirty();
    NotifyTransformUpdated();
    return Result<void>::Success();
}

bool SceneComponent::IsVisible() const noexcept {
    if (!visible_) {
        return false;
    }

    const bool gameWorld = HasWorld() && (GetWorld().Kind() == WorldKind::Game ||
                                          GetWorld().Kind() == WorldKind::Tryout);
    return !(gameWorld && hiddenInGame_);
}

void SceneComponent::SetMobility(ComponentMobility mobility) noexcept {
    if (mobility_ == mobility) {
        return;
    }

    mobility_ = mobility;
    if (IsRegistered()) {
        for (uint32 index = 0; index < SystemStateKindCount; ++index) {
            // 실패는 RecreateSystemStateNow가 Warning으로 남긴다(값 변경은 유지 — ERD25).
            (void)RecreateSystemStateNow(static_cast<SystemStateKind>(index));
        }
    }

    // 자식의 다시 만들기 hook이 이 목록을 바꿀 수 있으므로 handle 사본을 색인으로 돈다(UE도
    // GetAttachChildren 사본을 돈다).
    if (mobility == ComponentMobility::Movable && HasWorld()) {
        for (usize index = 0; index < children_.Size(); ++index) {
            const ObjectHandle child = children_[index];
            (void)GetWorld().With<SceneComponent>(child, [](SceneComponent& scene) noexcept {
                scene.SetMobility(ComponentMobility::Movable);
            });
        }
    }
}

void SceneComponent::SetVisibility(bool visible, bool propagateToChildren) noexcept {
    ApplyVisibility(visible, hiddenInGame_, true, propagateToChildren);
}

void SceneComponent::SetHiddenInGame(bool hidden, bool propagateToChildren) noexcept {
    ApplyVisibility(visible_, hidden, false, propagateToChildren);
}

void SceneComponent::ApplyVisibility(bool visible, bool hidden, bool setVisible,
                                     bool propagate) noexcept {
    const bool changed = setVisible ? visible_ != visible : hiddenInGame_ != hidden;
    if (setVisible) {
        visible_ = visible;
    } else {
        hiddenInGame_ = hidden;
    }

    if (changed) {
        if (HasSystemState(SystemStateKind::Render)) {
            auto marked = MarkSystemStateDirty(SystemStateKind::Render);
            (void)marked;
        }

        OnVisibilityChanged();
    }

    if (!propagate || !HasWorld()) {
        return;
    }

    World& world = GetWorld();
    for (usize index = 0; index < children_.Size(); ++index) {
        auto applied =
            world.With<SceneComponent>(children_[index], [&](SceneComponent& child) noexcept {
                child.ApplyVisibility(visible, hidden, setVisible, true);
            });
        (void)applied;
    }
}

const ComponentBounds& SceneComponent::Bounds() const noexcept {
    if (boundsDirty_) {
        ComponentBounds local;
        if (TryLocalBounds(local)) {
            bounds_ = TransformComponentBounds(local, WorldMatrix(), LocalBoundsSphereRule());
            boundsDirty_ = false;
            return bounds_;
        }

        auto world = TryWorldTransform();
        Transform at = Transform::Identity();
        if (world) {
            at = world.Value();
        } else {
            at.translation = WorldLocation();
        }

        bounds_ = CalcBounds(at);
        boundsDirty_ = false;
    }

    return bounds_;
}

ComponentBounds TransformComponentBounds(const ComponentBounds& local, const Mat4d& localToWorld,
                                         BoundsSphereRule rule) noexcept {
    // UE TransformBy(FMatrix)와 같은 순서다.
    const Vec3d axisX = localToWorld.GetScaledAxis(0);
    const Vec3d axisY = localToWorld.GetScaledAxis(1);
    const Vec3d axisZ = localToWorld.GetScaledAxis(2);
    const Vec4d translation = localToWorld.Row(3);
    ComponentBounds world;
    world.origin = axisX * local.origin.x + axisY * local.origin.y + axisZ * local.origin.z +
                   Vec3d(translation.x, translation.y, translation.z);
    const Vec3d spanX = axisX * local.boxExtent.x;
    const Vec3d spanY = axisY * local.boxExtent.y;
    const Vec3d spanZ = axisZ * local.boxExtent.z;
    world.boxExtent = Vec3d(math::Abs(spanX.x) + math::Abs(spanY.x) + math::Abs(spanZ.x),
                            math::Abs(spanX.y) + math::Abs(spanY.y) + math::Abs(spanZ.y),
                            math::Abs(spanX.z) + math::Abs(spanY.z) + math::Abs(spanZ.z));
    if (rule == BoundsSphereRule::BoxExtent) {
        world.sphereRadius = world.boxExtent.Length();
        return world;
    }

    // UE는 행 0–2를 성분별로 제곱해 더한다(m0² + m1² + m2²) — 3×3 부분의 열 길이 제곱이다. 그
    // 최댓값을 쓴다.
    const Vec3d columnSquared(axisX.x * axisX.x + axisY.x * axisY.x + axisZ.x * axisZ.x,
                              axisX.y * axisX.y + axisY.y * axisY.y + axisZ.y * axisZ.y,
                              axisX.z * axisX.z + axisY.z * axisY.z + axisZ.z * axisZ.z);
    const double maxAxisSquared =
        math::Max(math::Max(columnSquared.x, columnSquared.y), columnSquared.z);
    // 비균일 scale에서는 box에서 얻은 구가 더 작다(UE 주석).
    world.sphereRadius =
        math::Min(math::Sqrt(maxAxisSquared) * local.sphereRadius, world.boxExtent.Length());
    return world;
}

ComponentBounds SceneComponent::CalcBounds(const Transform& world) const noexcept {
    ComponentBounds bounds;
    bounds.origin = world.translation;
    return bounds;
}

Vec3d SceneComponent::WorldScale3D() const noexcept {
    if (absoluteScale_ || parent_.IsNull() || !HasWorld()) {
        return relative_.scale;
    }

    // UE Relative * Socket * Parent: scale은 성분별 곱이다(socket은 이름이 비어도 묻는다 — 한 번의
    // 빌림으로 부모 월드 scale과 socket scale을 함께 얻는다).
    const StringView socket = attachSocket_.View();
    auto frame =
        GetWorld().With<SceneComponent>(parent_, [socket](SceneComponent& parent) noexcept {
            Vec3d scale = parent.WorldScale3D();
            if (auto local = parent.GetSocketTransform(socket, TransformSpace::Component)) {
                scale = local.Value().scale * scale;
            }

            return scale;
        });
    return frame ? relative_.scale * frame.Value() : relative_.scale;
}

Quatd SceneComponent::WorldQuaternion() const noexcept {
    if (absoluteRotation_ || parent_.IsNull() || !HasWorld()) {
        return relative_.rotation;
    }

    // UE Relative * Socket * Parent: 회전은 Parent.rot × Socket.rot × Relative.rot이다(socket은
    // 이름이 비어도 묻는다).
    const StringView socket = attachSocket_.View();
    auto frame =
        GetWorld().With<SceneComponent>(parent_, [socket](SceneComponent& parent) noexcept {
            Quatd rotation = parent.WorldQuaternion();
            if (auto local = parent.GetSocketTransform(socket, TransformSpace::Component)) {
                rotation = rotation * local.Value().rotation;
            }

            return rotation;
        });
    return frame ? frame.Value() * relative_.rotation : relative_.rotation;
}

Result<Transform> SceneComponent::TryWorldTransform() const noexcept {
    return Transform::TryFromMatrix(WorldMatrix());
}

Vec3d SceneComponent::WorldLocation() const noexcept {
    return WorldMatrix().GetTranslation();
}

Result<void> SceneComponent::AddChild(ObjectHandle child) noexcept {
    return children_.TryPushBack(child);
}

void SceneComponent::RemoveChild(ObjectHandle child) noexcept {
    for (usize index = 0; index < children_.Size(); ++index) {
        if (children_[index] == child) {
            auto removed = children_.TryRemoveAt(index);
            (void)removed;
            return;
        }
    }
}

Result<void> SceneComponent::AttachTo(ObjectHandle parent) noexcept {
    return AttachTo(parent, AttachmentRules::KeepRelativeTransform(), StringView());
}

Result<void> SceneComponent::AttachTo(ObjectHandle parent, const AttachmentRules& rules,
                                      StringView socket) noexcept {
    if (!HasWorld() || parent.IsNull() || parent == Handle()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto movable = CheckMovable("attach"); !movable) {
        return movable;
    }

    // 성분마다 새 상대 값을 정한다(UE AttachToComponent — KeepWorld는 새 부모 socket 기준으로 다시
    // 계산한다). 링크 전에 계산하고 실패하면 아무것도 바꾸지 않는다.
    // Static은 Static 부모에만 붙는다(UE AttachToComponent — SceneComponent.cpp:2381).
    if (mobility_ == ComponentMobility::Static && IsRegistered()) {
        auto parentMobility = GetWorld().With<SceneComponent>(
            parent, [](SceneComponent& component) noexcept { return component.Mobility(); });
        if (parentMobility && parentMobility.Value() != ComponentMobility::Static) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    Transform relative = relative_;
    const bool keepWorld = rules.location == AttachmentRule::KeepWorld ||
                           rules.rotation == AttachmentRule::KeepWorld ||
                           rules.scale == AttachmentRule::KeepWorld;
    if (keepWorld) {
        auto current = TryWorldTransform();
        auto target =
            GetWorld().With<SceneComponent>(parent, [socket](SceneComponent& component) noexcept {
                return component.GetSocketTransform(socket, TransformSpace::World);
            });
        if (!current || !target) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 정확한 역변환(Rocks 월드 행렬과 짝 — 월드 Transform을 그대로 지킨다)을 먼저 쓰고, 상대
        // 값이 TRS로 표현되지 않으면(축에 맞지 않은 비균일 부모 — shear) UE GetRelativeTransform
        // 식으로 계산한다(23.3절).
        Result<Transform> exact = Result<Transform>::Failure(ErrorCode::InvalidArgument);
        if (auto inverse = target.Value().TryInverse()) {
            exact = Transform::TryMultiply(current.Value(), inverse.Value());
        }

        const bool useExact = exact.IsOk();
        auto local = useExact ? Move(exact) : RelativeTrs(current.Value(), target.Value());
        if (!local) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 절대 성분은 부모를 보지 않으므로 현재 월드 값이 곧 상대 값이다(UE rule switch).
        if (rules.location == AttachmentRule::KeepWorld) {
            relative.translation =
                absoluteLocation_ ? current.Value().translation : local.Value().translation;
        }

        if (rules.rotation == AttachmentRule::KeepWorld) {
            relative.rotation =
                absoluteRotation_ ? current.Value().rotation : local.Value().rotation;
        }

        // 분해한 상대 scale은 그 분해의 회전과 짝일 때만 월드 scale을 지킨다. 회전도 KeepWorld이고
        // 정확한 역변환이면 그 값을, 아니면 성분별로 월드 scale ÷ 부모 scale이다(UE).
        if (rules.scale == AttachmentRule::KeepWorld) {
            if (absoluteScale_) {
                relative.scale = current.Value().scale;
            } else if (useExact && rules.rotation == AttachmentRule::KeepWorld) {
                relative.scale = local.Value().scale;
            } else {
                if (HasZeroComponent(target.Value().scale)) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                relative.scale = current.Value().scale / target.Value().scale;
            }
        }
    }

    if (rules.location == AttachmentRule::SnapToTarget) {
        relative.translation = Vec3d();
    }

    if (rules.rotation == AttachmentRule::SnapToTarget) {
        relative.rotation = Quatd::Identity();
    }

    if (rules.scale == AttachmentRule::SnapToTarget) {
        relative.scale = Vec3d::One();
    }

    if (!relative.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto linked = LinkParent(parent, socket); !linked) {
        return linked;
    }

    relative_ = relative;
    worldDirty_ = false;
    MarkDirty();
    NotifyTransformUpdated();
    return Result<void>::Success();
}

Result<void> SceneComponent::LinkParent(ObjectHandle parent, StringView socket) noexcept {
    const ObjectHandle self = Handle();
    // World 장부 밖 객체(그래프 로드 등)는 계층을 관리할 World가 없다.
    if (!HasWorld() || parent.IsNull() || parent == self) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (parent == parent_ && socket == attachSocket_.View()) {
        return Result<void>::Success();
    }

    // socket 이름을 먼저 복사한다(할당 실패 시 기존 계층 유지).
    String socketName(attachSocket_.GetAllocator());
    if (auto copied = socketName.TryAssign(socket); !copied) {
        return copied;
    }

    World& world = GetWorld();
    // 새 부모의 조상 사슬에 자신이 있으면 순환이다. 부모가 SceneComponent가 아니거나 다른 World면
    // With가 TypeMismatch·StaleHandle을 돌려준다. 같은 registry에 있어도 World 장부 밖 객체(직접
    // 생성 등)는 부모가 될 수 없다(그 객체의 자식 목록을 World가 정리하지 못한다).
    ObjectHandle current = parent;
    for (uint32 depth = 0; !current.IsNull(); ++depth) {
        if (current == self || depth >= MaxHierarchyDepth) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto next = world.With<SceneComponent>(current, [](SceneComponent& component) noexcept {
            return component.HasWorld() ? Result<ObjectHandle>::Success(component.parent_)
                                        : Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
        });
        if (!next) {
            return Result<void>::Failure(next.Error());
        }

        current = next.Value();
    }

    // 같은 부모의 다른 socket으로 옮기면 자식 목록은 그대로다.
    if (parent == parent_) {
        attachSocket_ = Move(socketName);
        worldDirty_ = false;
        MarkDirty();
        return Result<void>::Success();
    }

    // 새 부모의 자식 목록에 먼저 넣는다(할당 실패 시 기존 계층 유지).
    auto added = world.With<SceneComponent>(
        parent, [self](SceneComponent& component) noexcept { return component.AddChild(self); });
    if (!added) {
        return added;
    }

    if (!parent_.IsNull()) {
        auto removed = world.With<SceneComponent>(
            parent_, [self](SceneComponent& component) noexcept { component.RemoveChild(self); });
        (void)removed;
    }

    // 자식 hook이 부모의 같은 hook 뒤에 실행되게 한다(UE AttachToComponent의 tick 선행).
    const ObjectHandle oldParent = parent_;
    world.UpdateAttachmentTicks(self, oldParent, parent);
    parent_ = parent;
    attachParent_ = WeakObjectRef<SceneComponent>(parent);
    attachSocket_ = Move(socketName);
    // 부모가 바뀌면 이미 dirty였더라도 새 부모 기준으로 다시 계산하도록 자손까지 표시한다.
    worldDirty_ = false;
    MarkDirty();
    return Result<void>::Success();
}

Result<void> SceneComponent::Detach() noexcept {
    return Detach(AttachmentRule::KeepRelative);
}

Result<void> SceneComponent::Detach(AttachmentRule rule) noexcept {
    if (!HasWorld()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // UE EDetachmentRule은 KeepRelative·KeepWorld 둘이다.
    if (rule == AttachmentRule::SnapToTarget) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (parent_.IsNull()) {
        // 로드한 영속 필드만 남은 경우도 비운다.
        attachParent_ = {};
        attachSocket_.Clear();
        return Result<void>::Success();
    }

    if (auto movable = CheckMovable("detach"); !movable) {
        return movable;
    }

    // KeepWorld는 뗀 뒤 상대 값 = 지금 월드 값이다. SnapToTarget은 원점·단위다(UE와 같다).
    Transform relative = relative_;
    if (rule == AttachmentRule::KeepWorld) {
        auto current = TryWorldTransform();
        if (!current) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        relative = current.Value();
    }

    const ObjectHandle self = Handle();
    auto removed = GetWorld().With<SceneComponent>(
        parent_, [self](SceneComponent& component) noexcept { component.RemoveChild(self); });
    (void)removed;
    GetWorld().UpdateAttachmentTicks(self, parent_, {});
    parent_ = {};
    attachParent_ = {};
    attachSocket_.Clear();
    relative_ = relative;
    worldDirty_ = false;
    MarkDirty();
    NotifyTransformUpdated();
    return Result<void>::Success();
}

void SceneComponent::DetachForDestroy() noexcept {
    // World가 정리하는 Component만 부른다(장부 밖 객체는 DestroyComponent가 거부한다).
    if (!HasWorld()) {
        return;
    }

    // 파괴 정리는 Mobility와 무관하게 뗀다(상대 값 유지).
    if (!parent_.IsNull()) {
        const ObjectHandle self = Handle();
        auto removed = GetWorld().With<SceneComponent>(
            parent_, [self](SceneComponent& component) noexcept { component.RemoveChild(self); });
        (void)removed;
        GetWorld().UpdateAttachmentTicks(self, parent_, {});
        parent_ = {};
        attachParent_ = {};
        attachSocket_.Clear();
        worldDirty_ = false;
        MarkDirty();
    }

    // 자식은 루트가 된다(상대 Transform 유지). 이미 계산된 월드 행렬은 죽은 부모를 포함하므로 다시
    // 계산하도록 자손까지 dirty로 표시한다.
    // 통지(overlap 알림 포함 — PhysicsModule 7.1.11절)가 다른 부착을 바꿀 수 있으므로 목록을 떼어
    // 낸 뒤 모두 고아로 만들고 나서 통지한다.
    World& world = GetWorld();
    const ObjectHandle self = Handle();
    Array<ObjectHandle> orphans(Move(children_));
    children_ = Array<ObjectHandle>(orphans.GetAllocator());
    for (const ObjectHandle child : orphans) {
        world.UpdateAttachmentTicks(child, self, {});
        auto orphaned = world.With<SceneComponent>(child, [](SceneComponent& component) noexcept {
            component.parent_ = {};
            component.attachParent_ = {};
            component.attachSocket_.Clear();
            component.worldDirty_ = false;
            component.MarkDirty();
        });
        (void)orphaned;
    }

    // 루트가 되어 월드 Transform이 바뀌었다(23.3절 통지).
    for (const ObjectHandle child : orphans) {
        (void)world.With<SceneComponent>(
            child, [](SceneComponent& component) noexcept { component.NotifyTransformUpdated(); });
    }
}

} // namespace rocks

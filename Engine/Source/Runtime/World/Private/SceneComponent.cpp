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

// 부모 사슬 탐색 상한이다. 정상 계층은 이보다 훨씬 얕다(순환 검사의 안전장치).
constexpr uint32 MaxHierarchyDepth = 4096;

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

Result<void> SceneComponent::CheckMovable(const char* action) const noexcept {
    // UE CheckStaticMobilityAndWarn(SceneComponent.cpp:3383): 등록된 게임 World Component이고 소유
    // Actor가 초기화된 뒤에만 막는다(Spawn·OnAwake 안의 배치는 된다).
    if (mobility_ == ComponentMobility::Movable || !HasWorld() || !IsRegistered()) {
        return Result<void>::Success();
    }

    World& world = GetWorld();
    if (world.Kind() != WorldKind::Game && world.Kind() != WorldKind::Tryout) {
        return Result<void>::Success();
    }

    auto state = world.GetActorState(GetOwner());
    if (!state ||
        (state.Value() != ActorState::Initialized && state.Value() != ActorState::Playing)) {
        return Result<void>::Success();
    }

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
        auto notified = world.With<SceneComponent>(
            child, [](SceneComponent& component) noexcept { component.NotifyTransformUpdated(); });
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

Mat4d SceneComponent::WorldMatrix() const noexcept {
    if (!worldDirty_) {
        return worldMatrix_;
    }

    Mat4d parentMatrix;
    // 부모는 World가 붙인 경우에만 있다(장부 밖 객체는 루트로 계산한다).
    if (!parent_.IsNull() && HasWorld()) {
        auto parentWorld = GetWorld().With<SceneComponent>(
            parent_, [](SceneComponent& parent) noexcept { return parent.WorldMatrix(); });
        if (parentWorld) {
            parentMatrix = parentWorld.Value();
        }
    }

    // 부착 socket이 있으면 부모 기준 socket Transform을 부모 월드 앞에 둔다.
    if (!attachSocket_.IsEmpty() && !parent_.IsNull() && HasWorld()) {
        const StringView socket = attachSocket_.View();
        auto socketLocal =
            GetWorld().With<SceneComponent>(parent_, [socket](SceneComponent& parent) noexcept {
                return parent.GetSocketTransform(socket, TransformSpace::Component);
            });
        if (socketLocal) {
            parentMatrix = socketLocal.Value().ToMatrix() * parentMatrix;
        }
    }

    // 행 벡터 규약: 상대 변환을 먼저, 부모 월드 변환을 나중에 적용한다(UE NewRelativeTransform *
    // ParentToWorld). 절대 성분이 있으면 TRS로 합성해 그 성분만 상대 값으로 바꾼다(23.3절 ERD31 —
    // 부모 행렬이 TRS로 분해되지 않으면 절대 성분 없이 행렬 곱이다).
    worldMatrix_ = relative_.ToMatrix() * parentMatrix;
    if ((absoluteLocation_ || absoluteRotation_ || absoluteScale_) && !parent_.IsNull()) {
        auto composed = Transform::TryFromMatrix(worldMatrix_);
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

Result<Transform> SceneComponent::ParentSocketWorld() const noexcept {
    if (parent_.IsNull() || !HasWorld()) {
        return Result<Transform>::Success(Transform::Identity());
    }

    const StringView socket = attachSocket_.View();
    return GetWorld().With<SceneComponent>(parent_, [socket](SceneComponent& parent) noexcept {
        return parent.GetSocketTransform(socket, TransformSpace::World);
    });
}

void SceneComponent::SetAbsolute(bool location, bool rotation, bool scale) noexcept {
    absoluteLocation_ = location;
    absoluteRotation_ = rotation;
    absoluteScale_ = scale;
    worldDirty_ = false;
    MarkDirty();
    NotifyTransformUpdated();
}

bool SceneComponent::IsVisible() const noexcept {
    if (!visible_) {
        return false;
    }

    const bool gameWorld = HasWorld() && (GetWorld().Kind() == WorldKind::Game ||
                                          GetWorld().Kind() == WorldKind::Tryout);
    return !(gameWorld && hiddenInGame_);
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

ComponentBounds SceneComponent::CalcBounds(const Transform& world) const noexcept {
    ComponentBounds bounds;
    bounds.origin = world.translation;
    return bounds;
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

        auto inverse = target.Value().TryInverse();
        if (!inverse) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto local = Transform::TryMultiply(current.Value(), inverse.Value());
        if (!local) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (rules.location == AttachmentRule::KeepWorld) {
            relative.translation = local.Value().translation;
        }

        if (rules.rotation == AttachmentRule::KeepWorld) {
            relative.rotation = local.Value().rotation;
        }

        // 분해한 상대 scale은 그 분해의 회전과 짝일 때만 월드 scale을 지킨다. 회전도 KeepWorld면 그
        // 값을 쓰고, 아니면 UE GetRelativeTransform처럼 성분별로 월드 scale ÷ 부모
        // scale이다(23.3절).
        if (rules.scale == AttachmentRule::KeepWorld) {
            if (rules.rotation == AttachmentRule::KeepWorld) {
                relative.scale = local.Value().scale;
            } else {
                const Vec3d& parentScale = target.Value().scale;
                const Vec3d& worldScale = current.Value().scale;
                if (parentScale.x == 0.0 || parentScale.y == 0.0 || parentScale.z == 0.0) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                relative.scale = Vec3d(worldScale.x / parentScale.x, worldScale.y / parentScale.y,
                                       worldScale.z / parentScale.z);
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

    if (parent_.IsNull()) {
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
    } else if (rule == AttachmentRule::SnapToTarget) {
        relative = Transform::Identity();
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
    World& world = GetWorld();
    const ObjectHandle self = Handle();
    for (ObjectHandle child : children_) {
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

    children_.Clear();
}

} // namespace rocks

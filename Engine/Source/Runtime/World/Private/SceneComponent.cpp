#include <World/SceneComponent.h>
#include <World/World.h>

namespace rocks {

namespace {

// 부모 사슬 탐색 상한이다. 정상 계층은 이보다 훨씬 얕다(순환 검사의 안전장치).
constexpr uint32 MaxHierarchyDepth = 4096;

} // namespace

Result<void> SceneComponent::SetRelativeTransform(const Transform& transform) noexcept {
    if (!transform.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    relative_ = transform;
    MarkDirty();
    return Result<void>::Success();
}

Result<void> SceneComponent::SetRelativeLocation(const Vec3d& location) noexcept {
    Transform transform = relative_;
    transform.translation = location;
    return SetRelativeTransform(transform);
}

Result<void> SceneComponent::SetRelativeRotation(const Quat& rotation) noexcept {
    Transform transform = relative_;
    transform.rotation = rotation;
    return SetRelativeTransform(transform);
}

Result<void> SceneComponent::SetRelativeScale(const Vec3& scale) noexcept {
    Transform transform = relative_;
    transform.scale = scale;
    return SetRelativeTransform(transform);
}

void SceneComponent::MarkDirty() noexcept {
    // 깨끗한 노드의 조상은 모두 깨끗하다(계산이 부모부터 이뤄지므로). 따라서 이미 dirty인 노드의
    // 자손은 모두 dirty이며 더 내려갈 필요가 없다.
    if (worldDirty_) {
        return;
    }

    worldDirty_ = true;
    if (children_.Size() == 0) {
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
        return world_;
    }

    Mat4d parentMatrix;
    if (!parent_.IsNull()) {
        auto parentWorld = GetWorld().With<SceneComponent>(
            parent_, [](SceneComponent& parent) noexcept { return parent.WorldMatrix(); });
        if (parentWorld) {
            parentMatrix = parentWorld.Value();
        }
    }

    world_ = parentMatrix * relative_.ToMatrix();
    worldDirty_ = false;
    return world_;
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
    const ObjectHandle self = Handle();
    if (parent.IsNull() || parent == self) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (parent == parent_) {
        return Result<void>::Success();
    }

    World& world = GetWorld();
    // 새 부모의 조상 사슬에 자신이 있으면 순환이다. 부모가 SceneComponent가 아니거나 다른 World면
    // With가 TypeMismatch·StaleHandle을 돌려준다.
    ObjectHandle current = parent;
    for (uint32 depth = 0; !current.IsNull(); ++depth) {
        if (current == self || depth >= MaxHierarchyDepth) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        auto next = world.With<SceneComponent>(
            current, [](SceneComponent& component) noexcept { return component.parent_; });
        if (!next) {
            return Result<void>::Failure(next.Error());
        }
        current = next.Value();
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

    parent_ = parent;
    // 부모가 바뀌면 이미 dirty였더라도 새 부모 기준으로 다시 계산하도록 자손까지 표시한다.
    worldDirty_ = false;
    MarkDirty();
    return Result<void>::Success();
}

Result<void> SceneComponent::Detach() noexcept {
    if (parent_.IsNull()) {
        return Result<void>::Success();
    }

    const ObjectHandle self = Handle();
    auto removed = GetWorld().With<SceneComponent>(
        parent_, [self](SceneComponent& component) noexcept { component.RemoveChild(self); });
    (void)removed;
    parent_ = {};
    worldDirty_ = false;
    MarkDirty();
    return Result<void>::Success();
}

void SceneComponent::DetachForDestroy() noexcept {
    auto detached = Detach();
    (void)detached;
    // 자식은 루트가 된다(상대 Transform 유지). 이미 계산된 월드 행렬은 죽은 부모를 포함하므로 다시
    // 계산하도록 자손까지 dirty로 표시한다.
    World& world = GetWorld();
    for (ObjectHandle child : children_) {
        auto orphaned = world.With<SceneComponent>(child, [](SceneComponent& component) noexcept {
            component.parent_ = {};
            component.worldDirty_ = false;
            component.MarkDirty();
        });
        (void)orphaned;
    }

    children_.Clear();
}

} // namespace rocks

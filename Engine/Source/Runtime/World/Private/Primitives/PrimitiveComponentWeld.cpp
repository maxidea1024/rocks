// PrimitiveComponent의 weld다(Docs/Design/PhysicsModule 2.8절·7.1.23절 — UE
// PrimitiveComponentPhysics.cpp:960-1252 WeldToImplementation·UnWeldFromParent·UnWeldChildren,
// BodyInstance.cpp:1940-2033 Weld·2641-2665 ApplyWeldOnChildren).
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <World/Component.generated.h>
#include <World/Physics/BodyGeometry.h>
#include <World/Physics/BodyWeldDesc.h>
#include <World/Physics/PhysicsScene.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/WorldLog.h>

namespace rocks {

namespace {

// 0인 scale 축의 역수를 막는 값이다(형상 규칙과 같은 KindaSmallNumber — UE는 0 축의 상대 scale을
// 0으로 둔다, 7.1.23절 차이).
double SafeScaleAxis(double axis) noexcept {
    if (math::Abs(axis) >= math::KindaSmallNumber<double>) {
        return axis;
    }

    return axis < 0.0 ? -math::KindaSmallNumber<double> : math::KindaSmallNumber<double>;
}

// self의 parent 기준 상대 transform이다(UE FTransform::GetRelativeTransform — scale 나눗셈. 음수
// scale의 UE 행렬 경로는 따르지 않는다 — 7.1.23절 차이).
Transform WeldRelativeTransform(const Transform& self, const Transform& parent) noexcept {
    const Vec3d scale(SafeScaleAxis(parent.scale.x), SafeScaleAxis(parent.scale.y),
                      SafeScaleAxis(parent.scale.z));
    Transform relative;
    relative.rotation = parent.rotation.Conjugate() * self.rotation;
    relative.scale = self.scale / scale;
    relative.translation =
        parent.rotation.UnrotateVector(self.translation - parent.translation) / scale;
    return relative;
}

// 실효 충돌 켬이 물리 body를 합칠 수 있는 값인지다(UE: QueryOnly·NoCollision은 합치지 않는다).
bool CanWeldCollision(CollisionEnabled enabled) noexcept {
    return enabled != CollisionEnabled::QueryOnly && enabled != CollisionEnabled::NoCollision;
}

// root와 그 자손(깊이 우선, root 먼저)을 out 끝에 더한다. 오류: 할당 실패(out은 일부일 수 있다).
Result<void> CollectSubtree(World& world, ObjectHandle root, Array<ObjectHandle>& out) noexcept {
    if (auto pushed = out.TryPushBack(root); !pushed) {
        return pushed;
    }

    Result<void> result = Result<void>::Success();
    (void)world.With<SceneComponent>(root, [&](SceneComponent& scene) noexcept {
        for (const ObjectHandle child : scene.Children()) {
            if (result) {
                result = CollectSubtree(world, child, out);
            }
        }
    });
    return result;
}

bool Contains(const Array<ObjectHandle>& handles, ObjectHandle handle) noexcept {
    for (const ObjectHandle candidate : handles) {
        if (candidate == handle) {
            return true;
        }
    }

    return false;
}

} // namespace

void PrimitiveComponent::WeldTo(ObjectHandle parent, StringView socket,
                                bool weldToKinematicParent) noexcept {
    // 붙어 있지 않으면 월드 transform을 유지하며 붙인다(UE WeldTo — KeepWorldTransform).
    if (Parent() != parent || AttachSocketName() != socket) {
        if (auto attached = AttachTo(parent, AttachmentRules::KeepWorldTransform(), socket);
            !attached) {
            return;
        }
    }

    (void)WeldToImplementation(parent, true, weldToKinematicParent);
}

bool PrimitiveComponent::WeldToImplementation(ObjectHandle parent, bool weldSimulatedChild,
                                              bool weldToKinematicParent) noexcept {
    if (!HasWorld() || parent.IsNull() || parent != Parent()) {
        return false;
    }

    if (bodySettings_.simulatePhysics && !weldSimulatedChild) {
        return false;
    }

    // 시뮬레이션하지 않던 것이 weld로 시뮬레이션하지 않게 한다(UE — QueryOnly·NoCollision).
    auto resolved = TryResolveCollision();
    if (!resolved || !CanWeldCollision(resolved.Value().collisionEnabled)) {
        return false;
    }

    UnWeldFromParent();

    // 루트: 부모부터 위로 primitive를 따라가며 자기 body가 없거나 weld된 것은 지나친다(UE
    // GetRootWelded — :960-998).
    World& world = GetWorld();
    ObjectHandle root;
    PhysicsBodyHandle rootBody;
    Transform rootWorld;
    BodyMotion rootMotion = BodyMotion::Static;
    ObjectHandle current = parent;
    while (!current.IsNull()) {
        bool primitive = false;
        bool skip = false;
        ObjectHandle next;
        (void)world.With<PrimitiveComponent>(current, [&](PrimitiveComponent& component) noexcept {
            primitive = true;
            root = current;
            rootBody = component.bodyHandle_;
            rootWorld = component.BodyWorldTransform();
            rootMotion = component.PhysicsBodyMotion();
            skip = component.bodyHandle_.IsNull() || component.IsWelded();
            next = component.Parent();
        });
        if (!primitive || !skip) {
            break;
        }

        current = next;
    }

    if (root.IsNull() || rootBody.IsNull()) {
        return false;
    }

    if (weldRoot_ == root) {
        return true;
    }

    // 루트가 시뮬레이션하지 않으면 자기도 시뮬레이션하지 않고 자기 body로 남는다(UE).
    if (rootMotion != BodyMotion::Dynamic && !weldToKinematicParent) {
        SetSimulatePhysics(false);
        return false;
    }

    return WeldInto(root, rootBody, rootWorld);
}

bool PrimitiveComponent::WeldInto(ObjectHandle root, PhysicsBodyHandle rootBody,
                                  const Transform& rootWorld) noexcept {
    World& world = GetWorld();
    PhysicsScene* scene = world.GetPhysicsScene();
    auto resolved = TryResolveCollision();
    BodyGeometry geometry(overlappingComponents_.GetAllocator());
    Result<void> welded = Result<void>::Failure(ErrorCode::NotReady);
    if (scene != nullptr && resolved) {
        welded = BuildBodyGeometry(geometry);
        if (welded && geometry.IsEmpty()) {
            welded = Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (welded) {
        BodyWeldDesc desc;
        desc.component = Handle();
        desc.actor = GetOwner();
        desc.geometry = geometry.View();
        desc.relative = WeldRelativeTransform(BodyWorldTransform(), rootWorld);
        desc.collisionEnabled = resolved.Value().collisionEnabled;
        desc.settings = &bodySettings_;
        welded = scene->WeldBody(rootBody, desc);
    }

    if (!welded) {
        LogContextScope context("World", world.Name());
        Log::Warning(LogWorld,
                     "Primitive component #{Slot} could not weld to component #{Root}: {Error} "
                     "({Code})",
                     Handle().slot, root.slot, ErrorCodeName(welded.Error()), DiagWeldFailed);
        // weld가 아니고 body도 없으면 자기 body로 둔다(조용히 body 없이 두지 않는다).
        EnsureOwnBody();
        return false;
    }

    // 내 body에 weld돼 있던 자손을 새 루트로 옮긴다(UE Weld — 상대의 ShapeToBodiesMap을 다시 weld,
    // BodyInstance.cpp:1990-2006). 내 body를 지우기 전에 한다.
    if (!bodyHandle_.IsNull()) {
        Array<ObjectHandle> subtree(overlappingComponents_.GetAllocator());
        (void)CollectSubtree(world, Handle(), subtree);
        const ObjectHandle self = Handle();
        for (usize index = 1; index < subtree.Size(); ++index) {
            (void)world.With<PrimitiveComponent>(
                subtree[index], [&](PrimitiveComponent& child) noexcept {
                    if (child.weldRoot_ == self) {
                        child.weldRoot_ = {};
                        child.weldBody_ = {};
                        (void)child.WeldInto(root, rootBody, rootWorld);
                    }
                });
        }

        // 합쳤으면 자기 body를 지운다(UE Weld — ReleaseActor).
        scene->DestroyBody(bodyHandle_);
        bodyHandle_ = {};
    }

    weldRoot_ = root;
    weldBody_ = rootBody;
    return true;
}

bool PrimitiveComponent::CanRecreateBody() const noexcept {
    if (!HasWorld() || !IsRegistered() || !HasSystemState(SystemStateKind::Physics)) {
        return false;
    }

    // 파괴 대기·World 정리 중이면 곧 지울 body를 만들지 않는다(UE IsValidChecked·GExitPurge).
    World& world = GetWorld();
    auto state = world.GetComponentState(Handle());
    return world.State() != WorldState::TearingDown && state &&
           state.Value() != ComponentState::PendingDestroy;
}

void PrimitiveComponent::EnsureOwnBody() noexcept {
    if (IsWelded() || !bodyHandle_.IsNull() || !CanRecreateBody()) {
        return;
    }

    if (auto created = CreateOwnBody(); !created) {
        LogContextScope context("World", GetWorld().Name());
        Log::Warning(LogWorld,
                     "Primitive component #{Slot} could not create its own body after a weld "
                     "change: {Error} ({Code})",
                     Handle().slot, ErrorCodeName(created.Error()), DiagWeldFailed);
    }
}

void PrimitiveComponent::ReweldToRoot() noexcept {
    if (!IsWelded()) {
        return;
    }

    // 루트가 아직 조상인지 본다(아니면 — 계층이 바뀌었으면 — 푼다).
    World& world = GetWorld();
    bool ancestor = false;
    Transform rootWorld;
    ObjectHandle current = Parent();
    while (!current.IsNull() && !ancestor) {
        ObjectHandle next;
        (void)world.With<SceneComponent>(
            current, [&](SceneComponent& scene) noexcept { next = scene.Parent(); });
        if (current == weldRoot_) {
            ancestor = true;
            (void)world.With<PrimitiveComponent>(current, [&](PrimitiveComponent& root) noexcept {
                rootWorld = root.BodyWorldTransform();
            });
        }

        current = next;
    }

    if (!ancestor || !WeldInto(weldRoot_, weldBody_, rootWorld)) {
        // 다시 합치지 못하면 풀고 자기 body로 둔다(이전 자리의 부분이 남지 않게).
        UnWeldFromParent();
    }
}

void PrimitiveComponent::UnWeldFromParent() noexcept {
    UnWeldInternal(true);
}

void PrimitiveComponent::UnWeldInternal(bool recreateBody) noexcept {
    if (!IsWelded() || !HasWorld()) {
        return;
    }

    const ObjectHandle root = weldRoot_;
    World& world = GetWorld();
    if (PhysicsScene* scene = world.GetPhysicsScene(); scene != nullptr) {
        scene->UnweldBody(weldBody_, Handle());
    }

    weldRoot_ = {};
    weldBody_ = {};
    // 자기 body를 다시 만든다(UE InitBody — 상태 파괴·파괴 대기·World 정리 중이 아니면).
    if (recreateBody) {
        EnsureOwnBody();
    }

    // 이 컴포넌트 아래에서 같은 루트에 weld된 자손도 푼다(UE GetWeldedBodies — body 없는 중간
    // 컴포넌트를 거친 자손 포함. 자기 body를 다시 만든다: UE와 다른 점, 7.1.23절).
    Array<ObjectHandle> subtree(overlappingComponents_.GetAllocator());
    (void)CollectSubtree(world, Handle(), subtree);
    for (usize index = 1; index < subtree.Size(); ++index) {
        (void)world.With<PrimitiveComponent>(subtree[index],
                                             [&](PrimitiveComponent& child) noexcept {
                                                 if (child.weldRoot_ == root) {
                                                     child.UnWeldInternal(true);
                                                 }
                                             });
    }

    // 이제 루트가 된 이 컴포넌트가 시뮬레이션하면 자손을 자기 body에 합친다(UE).
    if (recreateBody) {
        ApplyWeldOnChildren();
    }
}

void PrimitiveComponent::UnWeldChildren() noexcept {
    if (!HasWorld()) {
        return;
    }

    World& world = GetWorld();
    for (usize index = 0; index < Children().Size(); ++index) {
        (void)world.With<PrimitiveComponent>(
            Children()[index],
            [](PrimitiveComponent& child) noexcept { child.UnWeldFromParent(); });
    }
}

void PrimitiveComponent::UnWeldFromThis() noexcept {
    // 내 body에 weld된 자손을 모두 푼다(직접 자식만이 아니라 body 없는 중간 컴포넌트를 거친 자손도
    // — UE UnWeldChildren은 직접 자식만, 7.1.23절).
    if (!HasWorld()) {
        return;
    }

    World& world = GetWorld();
    Array<ObjectHandle> subtree(overlappingComponents_.GetAllocator());
    (void)CollectSubtree(world, Handle(), subtree);
    const ObjectHandle self = Handle();
    for (usize index = 1; index < subtree.Size(); ++index) {
        (void)world.With<PrimitiveComponent>(subtree[index],
                                             [&](PrimitiveComponent& child) noexcept {
                                                 if (child.weldRoot_ == self) {
                                                     child.UnWeldInternal(true);
                                                 }
                                             });
    }
}

void PrimitiveComponent::ApplyWeldOnChildren() noexcept {
    if (bodyHandle_.IsNull() || PhysicsBodyMotion() != BodyMotion::Dynamic || !HasWorld()) {
        return;
    }

    // 대상을 먼저 모은다(UE GetWeldedBodies(…, true) — weld됐거나 autoWeld인 자식을 따라 내려간다).
    World& world = GetWorld();
    Array<ObjectHandle> targets(overlappingComponents_.GetAllocator());
    CollectWeldTargets(targets);
    const ObjectHandle self = Handle();
    const PhysicsBodyHandle body = bodyHandle_;
    const Transform rootWorld = BodyWorldTransform();
    for (const ObjectHandle target : targets) {
        (void)world.With<PrimitiveComponent>(target, [&](PrimitiveComponent& child) noexcept {
            auto resolved = child.TryResolveCollision();
            const bool physics =
                resolved && CollisionEnabledHasPhysics(resolved.Value().collisionEnabled);
            if (!physics || !child.HasSystemState(SystemStateKind::Physics) ||
                child.weldRoot_ == self) {
                return;
            }

            // 다른 루트에 있으면 먼저 빼고(자기 body 없이) 이 루트에 합친다(UE Weld — 자손의
            // 시뮬레이션 표지는 보지 않는다). 실패하면 WeldInto가 자기 body를 만든다.
            child.UnWeldInternal(false);
            (void)child.WeldInto(self, body, rootWorld);
        });
    }
}

void PrimitiveComponent::CollectWeldTargets(Array<ObjectHandle>& targets) noexcept {
    World& world = GetWorld();
    for (usize index = 0; index < Children().Size(); ++index) {
        const ObjectHandle child = Children()[index];
        (void)world.With<PrimitiveComponent>(child, [&](PrimitiveComponent& primitive) noexcept {
            if (!primitive.IsWelded() && !primitive.bodySettings_.autoWeld) {
                return;
            }

            if (targets.TryPushBack(child)) {
                primitive.CollectWeldTargets(targets);
            }
        });
    }
}

void PrimitiveComponent::OnDetachingFromParent() noexcept {
    SceneComponent::OnDetachingFromParent();
    if (!HasWorld()) {
        return;
    }

    // 떼는 부분 트리 안에서 루트가 그 밖에 있는 weld를 푼다(UE DetachFromComponent →
    // UnWeldFromParent — 자기가 weld가 아니어도 body 없는 이 컴포넌트를 거친 자손이 있다).
    World& world = GetWorld();
    Array<ObjectHandle> subtree(overlappingComponents_.GetAllocator());
    (void)CollectSubtree(world, Handle(), subtree);
    for (const ObjectHandle member : subtree) {
        (void)world.With<PrimitiveComponent>(member, [&](PrimitiveComponent& primitive) noexcept {
            if (primitive.IsWelded() && !Contains(subtree, primitive.weldRoot_)) {
                primitive.UnWeldFromParent();
            }
        });
    }
}

void PrimitiveComponent::OnAttachedToParent(const AttachmentRules& rules) noexcept {
    SceneComponent::OnAttachedToParent(rules);
    // UE AttachToComponent: bWeldSimulatedBodies면 WeldToImplementation(…, true).
    if (rules.weldSimulatedBodies && IsRegistered() && HasSystemState(SystemStateKind::Physics)) {
        (void)WeldToImplementation(Parent(), true, false);
    }
}

} // namespace rocks

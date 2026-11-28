#include <Base/NumericLimits.h>
#include <Collision/BodySettings.generated.h>
#include <Collision/CollisionShape.h>
#include <Collision/CollisionSettings.generated.h>
#include <Collision/CollisionTypes.generated.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Physics/HitResult.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Physics/BodyCreateDesc.h>
#include <World/Physics/PhysicsScene.h>
#include <World/Physics/WorldCollision.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/WorldLog.h>
#include <World/ComponentValueChecks.h>

// PrimitiveComponent다(Docs/Design/EngineRuntime 24.3.3–24.3.5절, D2). 데이터만 있고 렌더·충돌
// 연결은 RS1·CR16이다.
namespace rocks {

Result<void> PrimitiveComponent::SetMinDrawDistance(float distance) noexcept {
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    Change(minDrawDistance_, distance);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetDesiredMaxDrawDistance(float distance) noexcept {
    // UE SetCullDistance(PrimitiveComponent.cpp:2461)는 음수를 무시한다 — Rocks는 거부한다.
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    Change(desiredMaxDrawDistance_, distance);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCustomDepthStencilValue(int32 value) noexcept {
    if (value < 0 || value > MaxCustomDepthStencilValue) {
        return component_checks::Invalid();
    }

    Change(customDepthStencilValue_, value);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCustomDepthStencilWriteMask(RendererStencilMask mask) noexcept {
    if (static_cast<uint8>(mask) > static_cast<uint8>(RendererStencilMask::Mask128)) {
        return component_checks::Invalid();
    }

    Change(customDepthStencilWriteMask_, mask);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetTranslucencySortDistanceOffset(float offset) noexcept {
    if (!math::IsFinite(offset)) {
        return component_checks::Invalid();
    }

    Change(translucencySortDistanceOffset_, offset);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetBoundsScale(float scale) noexcept {
    if (!math::IsFinite(scale) || scale <= 0.0f) {
        return component_checks::Invalid();
    }

    if (boundsScale_ != scale) {
        boundsScale_ = scale;
        InvalidateBounds();
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCanCharacterStepUpOn(rocks::CanBeCharacterBase value) noexcept {
    if (static_cast<uint8>(value) > static_cast<uint8>(rocks::CanBeCharacterBase::Owner)) {
        return component_checks::Invalid();
    }

    Change(canCharacterStepUpOn_, value);
    return Result<void>::Success();
}

StringView PrimitiveComponent::CollisionProfileName() const noexcept {
    return collisionProfileName_.IsEmpty() ? DefaultCollisionProfileName()
                                           : collisionProfileName_.View();
}

namespace {

// 해석한 값이 표의 profile이면 body 사용자 지정 칸에 옮긴다(UE ReadConfig가 body에 복사 —
// CollisionData 5절).
Result<void> CopyResolvedProfile(const CollisionProfileTable& table,
                                 const ResolvedCollision& resolved, BodySettings& body) noexcept {
    if (!resolved.fromProfile) {
        return Result<void>::Success();
    }

    return MakeCustomCollision(table, resolved, body);
}

// MoveComponent 상수다(UE PrimitiveComponent.cpp — 7.1.10절).
// sweep 최소 이동 배율(UE 4·KINDA_SMALL_NUMBER — :3281).
constexpr double MinSweepMovementScale = 4.0;
// pullback(UE PullBackHit — :3095-3099): hit time에서 clamp(PullbackTime, PullbackTime/D, 1/D) +
// PullbackTimeSlack을 뺀다.
constexpr double PullbackTime = 0.1;
constexpr double PullbackTimeSlack = 0.001;
// 시작 겹침에서 빠져나가는지 보는 내적 허용 오차와 시작 겹침으로 보는 hit 거리(UE CVar 기본값 0 —
// :125·:134).
constexpr double InitialOverlapTolerance = 0.0;
constexpr double HitDistanceTolerance = 0.0;
// 같은 회전으로 보는 성분 허용 오차(UE SCENECOMPONENT_QUAT_TOLERANCE — 최소 이동 판정).
constexpr double MoveQuaternionTolerance = 1.0e-8;
// 움직였는지 보는 위치·회전 허용 오차(UE InternalSetWorldLocationAndRotation의 FVector::Equals·
// FQuat::Equals 기본값 KINDA_SMALL_NUMBER — SceneComponent.cpp).
constexpr double MovedTolerance = math::KindaSmallNumber<double>;

// 이동 중 body 이동 방식이다(자손의 OnTransformUpdated도 같은 방식 — UE UpdateComponentToWorld가
// teleport를 자식에 넘긴다). 이동은 World 게임 thread에서 하므로 thread 범위 값이다. 한계: 범위
// 안에서 자손의 OnTransformUpdated가 MoveComponent가 아닌 setter로 관계없는 컴포넌트를 옮기면 그
// body도 이 방식을 받는다(안쪽 MoveComponent는 범위를 저장·복원한다).
thread_local TeleportMode t_moveTeleport = TeleportMode::None;

class MoveTeleportScope {
public:
    explicit MoveTeleportScope(TeleportMode teleport) noexcept : previous_(t_moveTeleport) {
        t_moveTeleport = teleport;
    }

    ~MoveTeleportScope() {
        t_moveTeleport = previous_;
    }

    MoveTeleportScope(const MoveTeleportScope&) = delete;
    MoveTeleportScope& operator=(const MoveTeleportScope&) = delete;

private:
    TeleportMode previous_;
};

// UE FQuat::Equals: 성분마다 허용 오차 안이면 같고, 부호가 반대인 같은 회전도 같다.
bool SameMoveRotation(const Quatd& a, const Quatd& b,
                      double tolerance = MoveQuaternionTolerance) noexcept {
    auto near = [tolerance](const Quatd& p, const Quatd& q, double sign) noexcept {
        return math::Abs(p.x - sign * q.x) <= tolerance &&
               math::Abs(p.y - sign * q.y) <= tolerance &&
               math::Abs(p.z - sign * q.z) <= tolerance && math::Abs(p.w - sign * q.w) <= tolerance;
    };
    return near(a, b, 1.0) || near(a, b, -1.0);
}

// UE FVector::Equals: 성분마다 허용 오차 안이면 같다.
bool SameMoveLocation(const Vec3d& a, const Vec3d& b) noexcept {
    return math::Abs(a.x - b.x) <= MovedTolerance && math::Abs(a.y - b.y) <= MovedTolerance &&
           math::Abs(a.z - b.z) <= MovedTolerance;
}

// UE PullBackHit: 막는 면에 붙지 않게 hit time을 조금 앞으로 당긴다.
void PullBackHit(HitResult& hit, double distance) noexcept {
    const double desiredTimeBack =
        math::Clamp(PullbackTime, PullbackTime / distance, 1.0 / distance) + PullbackTimeSlack;
    hit.time = math::Clamp(hit.time - desiredTimeBack, 0.0, 1.0);
}

// UE ShouldIgnoreHitResult(:3115-3165): 시작 겹침에서 빠져나가는 막는 hit는 무시한다.
bool ShouldIgnoreHit(const HitResult& hit, const Vec3d& moveDirection,
                     const MoveOptions& options) noexcept {
    if (!hit.blockingHit || options.neverIgnoreBlockingOverlaps) {
        return false;
    }

    if (hit.distance < HitDistanceTolerance || hit.startPenetrating) {
        return Dot(hit.impactNormal, moveDirection) > InitialOverlapTolerance;
    }

    return false;
}

// include면 handle을 더하고(이미 있거나 null이면 그대로) 아니면 뺀다. 오류: 할당 실패.
Result<void> SetHandleInList(Array<ObjectHandle>& list, ObjectHandle handle,
                             bool include) noexcept {
    for (usize index = 0; index < list.Size(); ++index) {
        if (list[index] == handle) {
            if (!include) {
                list[index] = list[list.Size() - 1];
                (void)list.TryPopBack();
            }

            return Result<void>::Success();
        }
    }

    if (!include || handle.IsNull()) {
        return Result<void>::Success();
    }

    return list.TryPushBack(handle);
}

// overlap 알림의 시작·끝이다.
enum class OverlapEdge : uint8 {
    Begin,
    End
};

WeakObjectRef<Actor> ActorRef(ObjectHandle actor) noexcept {
    return WeakObjectRef<Actor>(actor);
}

WeakObjectRef<PrimitiveComponent> PrimitiveRef(ObjectHandle component) noexcept {
    return WeakObjectRef<PrimitiveComponent>(component);
}

// 알림을 받을 수 있는 Actor인지다(UE IsActorValidToNotify — IsValid). Rocks의 파괴 대기는 UE의
// MarkAsGarbage 뒤에 해당한다. 자기 파괴 정리 중(destroying)이면 파괴 대기여도 받는다(UE는 정리가
// MarkAsGarbage보다 먼저다).
bool CanNotifyActor(World& world, ObjectHandle actor, bool destroying) noexcept {
    auto state = world.GetActorState(actor);
    return state && (destroying || state.Value() != ActorState::PendingDestroy);
}

bool CanNotifyComponent(World& world, ObjectHandle component, bool destroying) noexcept {
    auto state = world.GetComponentState(component);
    return state && (destroying || state.Value() != ComponentState::PendingDestroy);
}

// actor의 overlap 가상 hook이다(UE NotifyActorBeginOverlap·NotifyActorEndOverlap).
void CallActorOverlapHook(World& world, ObjectHandle actor, ObjectHandle other, OverlapEdge edge,
                          bool destroying) noexcept {
    if (!CanNotifyActor(world, actor, destroying)) {
        return;
    }

    (void)world.With<Actor>(actor, [&](Actor& target) noexcept {
        if (edge == OverlapEdge::Begin) {
            target.OnBeginOverlap(other);
        } else {
            target.OnEndOverlap(other);
        }
    });
}

// actor의 overlap 이벤트다(UE OnActorBeginOverlap·OnActorEndOverlap).
void BroadcastActorOverlap(World& world, ObjectHandle actor, ObjectHandle other, OverlapEdge edge,
                           bool destroying) noexcept {
    if (!CanNotifyActor(world, actor, destroying)) {
        return;
    }

    (void)world.With<Actor>(actor, [&](Actor& target) noexcept {
        const EventContext context{world.Objects(), world.Access(), actor};
        if (edge == OverlapEdge::Begin) {
            target.actorOverlapBegan.Broadcast(context, ActorRef(actor), ActorRef(other));
        } else {
            target.actorOverlapEnded.Broadcast(context, ActorRef(actor), ActorRef(other));
        }
    });
}

// hit 한쪽을 알린다(UE AActor::InternalDispatchBlockingHit — Actor.cpp:3525-3551,
// DispatchPhysicsCollisionHit — 7.1.14절): Actor가 알림을 받을 수 있으면 OnHit, 그 뒤에도 받을 수
// 있으면 actorHit, 컴포넌트가 아직 살아 있으면 componentHit다(hook이 자기를 파괴하면 이어지는
// 이벤트는 나지 않는다).
void NotifyHitSide(World& world, ObjectHandle actor, ObjectHandle myComponent,
                   ObjectHandle otherActor, ObjectHandle otherComponent, bool selfMoved,
                   const Vec3d& normalImpulse, const HitResult& hit) noexcept {
    if (CanNotifyActor(world, actor, false)) {
        (void)world.With<Actor>(actor, [&](Actor& target) noexcept {
            target.OnHit(myComponent, otherActor, otherComponent, selfMoved, normalImpulse, hit);
        });
    }

    if (CanNotifyActor(world, actor, false)) {
        (void)world.With<Actor>(actor, [&](Actor& target) noexcept {
            target.actorHit.Broadcast({world.Objects(), world.Access(), actor}, ActorRef(actor),
                                      ActorRef(otherActor), normalImpulse, hit);
        });
    }

    if (CanNotifyComponent(world, myComponent, false)) {
        (void)world.With<PrimitiveComponent>(
            myComponent, [&](PrimitiveComponent& component) noexcept {
                component.componentHit.Broadcast({world.Objects(), world.Access(), myComponent},
                                                 PrimitiveRef(myComponent), ActorRef(otherActor),
                                                 PrimitiveRef(otherComponent), normalImpulse, hit);
            });
    }
}

// sweep이 아닌 겹침의 hit다(UE FOverlapInfo(Component) — 컴포넌트·Actor만 채운다).
HitResult OverlapInfoHit(ObjectHandle component, ObjectHandle actor) noexcept {
    HitResult hit;
    hit.component = component;
    hit.actor = actor;
    return hit;
}

bool KnownCollisionEnabled(CollisionEnabled value) noexcept {
    return static_cast<uint32>(value) < CollisionEnabledCount;
}

bool KnownCollisionChannel(CollisionChannel channel) noexcept {
    return static_cast<uint32>(channel) < CollisionChannelCount;
}

bool KnownCollisionResponse(CollisionResponse response) noexcept {
    return static_cast<uint32>(response) < CollisionResponseCount;
}

} // namespace

Result<void> PrimitiveComponent::SetCollisionProfileName(StringView name) noexcept {
    // 클래스 기본과 같으면 빈 값(기본 따름)으로 둔다(24.3.4절 — UE FName처럼 대소문자 무시).
    String previous = Move(collisionProfileName_);
    collisionProfileName_ = String(previous.GetAllocator());
    if (!EqualsIgnoringAsciiCase(name, DefaultCollisionProfileName())) {
        if (auto assigned = collisionProfileName_.TryAssign(name); !assigned) {
            collisionProfileName_ = Move(previous);
            return assigned;
        }
    }

    if (!HasWorld()) {
        OnCollisionProfileNameSet();
        return Result<void>::Success();
    }

    // 표에 있으면 profile 값을 body에 옮긴다. 없으면 body는 마지막 해석 값 그대로다.
    const CollisionProfileTable& table = GetWorld().CollisionProfiles();
    const ResolvedCollision resolved =
        ResolveCollision(table, CollisionProfileName(), bodySettings_);
    if (auto copied = CopyResolvedProfile(table, resolved, bodySettings_); !copied) {
        collisionProfileName_ = Move(previous);
        return copied;
    }

    UpdateBodyCollision();
    OnCollisionProfileNameSet();
    return Result<void>::Success();
}

Result<ResolvedCollision> PrimitiveComponent::TryResolveCollision() const noexcept {
    if (!HasWorld()) {
        return Result<ResolvedCollision>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<ResolvedCollision>::Success(
        ResolveCollision(GetWorld().CollisionProfiles(), CollisionProfileName(), bodySettings_));
}

template <class Edit>
Result<void> PrimitiveComponent::EditCustomCollision(Edit&& edit) noexcept {
    if (!HasWorld()) {
        return component_checks::Invalid();
    }

    const CollisionProfileTable& table = GetWorld().CollisionProfiles();
    ResolvedCollision resolved = ResolveCollision(table, CollisionProfileName(), bodySettings_);
    if (!edit(resolved)) {
        return Result<void>::Success();
    }

    // 이름을 먼저 준비해 할당 실패 때 아무것도 바꾸지 않는다.
    String custom(collisionProfileName_.GetAllocator());
    if (auto assigned = custom.TryAssign(CustomCollisionProfileName); !assigned) {
        return assigned;
    }

    if (auto made = MakeCustomCollision(table, resolved, bodySettings_); !made) {
        return made;
    }

    collisionProfileName_ = Move(custom);
    UpdateBodyCollision();
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCollisionEnabled(CollisionEnabled enabled) noexcept {
    if (!KnownCollisionEnabled(enabled)) {
        return component_checks::Invalid();
    }

    return EditCustomCollision([enabled](ResolvedCollision& resolved) noexcept {
        if (resolved.collisionEnabled == enabled) {
            return false;
        }

        resolved.collisionEnabled = enabled;
        return true;
    });
}

Result<void> PrimitiveComponent::SetCollisionObjectType(CollisionChannel channel) noexcept {
    if (!KnownCollisionChannel(channel)) {
        return component_checks::Invalid();
    }

    // UE FBodyInstance::SetObjectType(BodyInstance.cpp:675)은 값을 비교하지 않고 언제나 Custom으로
    // 바꾼다(Enabled·응답과 다르다).
    return EditCustomCollision([channel](ResolvedCollision& resolved) noexcept {
        resolved.objectType = channel;
        return true;
    });
}

Result<void>
PrimitiveComponent::SetCollisionResponseToChannel(CollisionChannel channel,
                                                  CollisionResponse response) noexcept {
    if (!KnownCollisionChannel(channel) || !KnownCollisionResponse(response)) {
        return component_checks::Invalid();
    }

    return EditCustomCollision([channel, response](ResolvedCollision& resolved) noexcept {
        return resolved.responses.Set(channel, response);
    });
}

Result<void>
PrimitiveComponent::SetCollisionResponseToAllChannels(CollisionResponse response) noexcept {
    if (!KnownCollisionResponse(response)) {
        return component_checks::Invalid();
    }

    return EditCustomCollision([response](ResolvedCollision& resolved) noexcept {
        return resolved.responses.SetAll(response);
    });
}

Result<void> PrimitiveComponent::BuildBodyGeometry(BodyGeometry&) const noexcept {
    return Result<void>::Success();
}

bool PrimitiveComponent::IsZeroExtent() const noexcept {
    return false;
}

Transform PrimitiveComponent::BodyWorldTransform() const noexcept {
    // 행렬 분해(TryWorldTransform)는 0 scale·shear에서 실패하고 반사 부호를 x축으로 옮긴다. UE처럼
    // 월드 위치·회전·scale을 따로 쓴다(shear·0 scale·반사에서도 값이 있다 — UE
    // GetComponentTransform).
    return Transform(WorldQuaternion(), WorldLocation(), WorldScale3D());
}

BodyMotion PrimitiveComponent::PhysicsBodyMotion() const noexcept {
    if (Mobility() != ComponentMobility::Movable) {
        return BodyMotion::Static;
    }

    return bodySettings_.simulatePhysics ? BodyMotion::Dynamic : BodyMotion::Kinematic;
}

bool PrimitiveComponent::WantsSystemState(SystemStateKind kind) const noexcept {
    if (kind != SystemStateKind::Physics) {
        return SceneComponent::WantsSystemState(kind);
    }

    if (!HasWorld() || GetWorld().GetPhysicsScene() == nullptr) {
        return false;
    }

    // 할당 실패면 상태를 원한다(만들기가 그 오류를 낸다 — 조용히 body 없이 두지 않는다).
    BodyGeometry geometry(overlappingComponents_.GetAllocator());
    if (auto built = BuildBodyGeometry(geometry); built && geometry.IsEmpty()) {
        return false;
    }

    auto resolved = TryResolveCollision();
    return resolved && resolved.Value().collisionEnabled != CollisionEnabled::NoCollision;
}

Result<void> PrimitiveComponent::CreateSystemState(SystemStateKind kind) noexcept {
    if (kind != SystemStateKind::Physics) {
        return SceneComponent::CreateSystemState(kind);
    }

    // 자동 weld(UE InitBody — BodyInstance.cpp:1525-1541, 7.1.23절): 게임 World에서 Movable이고
    // 질의 전용이 아니면 부모 쪽 루트 body에 합쳐 본다. 합쳤으면 자기 body를 만들지 않는다.
    if (bodySettings_.autoWeld && PhysicsBodyMotion() != BodyMotion::Static && !Parent().IsNull()) {
        const WorldKind worldKind = GetWorld().Kind();
        auto resolved = TryResolveCollision();
        const bool gameWorld = worldKind == WorldKind::Game || worldKind == WorldKind::Tryout;
        if (gameWorld && resolved &&
            resolved.Value().collisionEnabled != CollisionEnabled::QueryOnly &&
            WeldToImplementation(Parent(), false, false)) {
            return Result<void>::Success();
        }
    }

    return CreateOwnBody();
}

Result<void> PrimitiveComponent::CreateOwnBody() noexcept {
    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    auto resolved = TryResolveCollision();
    if (scene == nullptr || !resolved) {
        return Result<void>::Failure(!resolved ? resolved.Error() : ErrorCode::NotReady);
    }

    BodyGeometry geometry(overlappingComponents_.GetAllocator());
    if (auto built = BuildBodyGeometry(geometry); !built) {
        return built;
    }

    if (geometry.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    BodyCreateDesc desc;
    desc.geometry = geometry.View();

    desc.component = Handle();
    desc.actor = GetOwner();
    desc.worldTransform = BodyWorldTransform();
    desc.collision = resolved.Value();
    desc.settings = &bodySettings_;
    desc.motion = PhysicsBodyMotion();
    if (desc.motion == BodyMotion::Static && bodySettings_.simulatePhysics) {
        LogContextScope context("World", GetWorld().Name());
        Log::Warning(LogWorld,
                     "Primitive component #{Slot} simulates physics but is not Movable; creating "
                     "a static body ({Code})",
                     Handle().slot, DiagSimulateOnStaticBody);
    }

    // 시작 잠듦: startAwake가 거짓이고 소유 Actor가 시작 전이면 잠든 채 넣는다(2.3절).
    auto ownerState = GetWorld().GetActorState(GetOwner());
    const bool ownerPlaying = ownerState && ownerState.Value() == ActorState::Playing;
    desc.startAwake = bodySettings_.startAwake || ownerPlaying;
    auto created = scene->CreateBody(desc);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    bodyHandle_ = created.Value();
    return Result<void>::Success();
}

void PrimitiveComponent::DestroySystemState(SystemStateKind kind) noexcept {
    if (kind != SystemStateKind::Physics) {
        SceneComponent::DestroySystemState(kind);
        return;
    }

    // weld를 푼다(UE OnDestroyPhysicsState — PrimitiveComponent.cpp:1148-1150): 자기는 body를 다시
    // 만들지 않고, 내 body에 weld된 자손은 자기 body를 다시 만든다(7.1.23절).
    UnWeldInternal(false);
    UnWeldFromThis();
    if (!bodyHandle_.IsNull()) {
        if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
            scene->DestroyBody(bodyHandle_);
        }

        bodyHandle_ = {};
    }
}

void PrimitiveComponent::OnSystemStateChanged(SystemStateKind kind, bool created) noexcept {
    SceneComponent::OnSystemStateChanged(kind, created);
    if (kind != SystemStateKind::Physics) {
        return;
    }

    // UE OnCreatePhysicsState·OnDestroyPhysicsState 끝(PrimitiveComponent.cpp:971·1169 — bit 확정
    // 뒤, 7.1.19절).
    componentPhysicsStateChanged.Broadcast(
        {GetWorld().Objects(), GetWorld().Access(), Handle()}, PrimitiveRef(Handle()),
        created ? ComponentPhysicsStateChange::Created : ComponentPhysicsStateChange::Destroyed);
}

void PrimitiveComponent::DispatchWakeEvents(bool awake) noexcept {
    World& world = GetWorld();
    // UE ShouldDispatchWakeEvents — 발행 때의 bGenerateWakeEvents(PrimitiveComponent.cpp:3652).
    if (bodySettings_.generateWakeEvents && CanNotifyComponent(world, Handle(), false)) {
        const EventContext context{world.Objects(), world.Access(), Handle()};
        if (awake) {
            componentWake.Broadcast(context, PrimitiveRef(Handle()));
        } else {
            componentSleep.Broadcast(context, PrimitiveRef(Handle()));
        }
    }

    // 이 컴포넌트에 직접 weld된 자식에 전한다(UE DispatchWakeEvents — :3623-3637, 7.1.23절).
    // handler가 계층을 바꿀 수 있어 하나씩 다시 읽는다.
    if (!HasWorld()) {
        return;
    }

    const ObjectHandle self = Handle();
    for (usize index = 0; index < Children().Size(); ++index) {
        (void)world.With<PrimitiveComponent>(Children()[index],
                                             [&](PrimitiveComponent& child) noexcept {
                                                 if (child.weldRoot_ == self) {
                                                     child.DispatchWakeEvents(awake);
                                                 }
                                             });
    }
}

void PrimitiveComponent::SetSimulatePhysics(bool simulate) noexcept {
    // 켤 때 부모에서 뗀다(UE FBodyInstance::SetInstanceSimulatePhysics —
    // BodyInstance.cpp:2671-2686). 떼기가 실패하면(KeepWorld를 표현할 수 없는 부모 scale) 붙은 채
    // 시뮬레이션한다.
    if (simulate && IsRegistered() && !Parent().IsNull()) {
        (void)Detach(AttachmentRule::KeepWorld);
    }

    const bool wasSimulating = bodySettings_.simulatePhysics;
    bodySettings_.simulatePhysics = simulate;
    if (bodyHandle_.IsNull()) {
        return;
    }

    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    if (scene == nullptr) {
        return;
    }

    const BodyMotion motion = PhysicsBodyMotion();
    if (motion == BodyMotion::Static) {
        if (simulate && !wasSimulating) {
            LogContextScope context("World", GetWorld().Name());
            Log::Warning(
                LogWorld,
                "Primitive component #{Slot} simulates physics but is not Movable; keeping "
                "a static body ({Code})",
                Handle().slot, DiagSimulateOnStaticBody);
        }

        return;
    }

    // 다시 만들지 않고 바꾼다(UE UpdateInstanceSimulatePhysics — 켜면 startAwake일 때 깨운다).
    scene->SetBodyMotion(bodyHandle_, motion, bodySettings_.startAwake);
    // Kinematic → 시뮬레이션이면 자손을 합친다(UE SetInstanceSimulatePhysics →
    // ApplyWeldOnChildren — BodyInstance.cpp:2681-2685, 7.1.23절).
    if (simulate && !wasSimulating) {
        ApplyWeldOnChildren();
    }
}

bool PrimitiveComponent::ComputePenetration(MtdResult& out, const CollisionShape& shape,
                                            const Vec3d& position,
                                            const Quatd& rotation) const noexcept {
    // UE: body가 없으면 거짓이다.
    if (bodyHandle_.IsNull() || !HasWorld()) {
        return false;
    }

    const PhysicsScene* scene = GetWorld().GetPhysicsScene();
    return scene != nullptr &&
           scene->ComputePenetration(bodyHandle_, shape, position, rotation, out);
}

CollisionShape PrimitiveComponent::GetCollisionShape(double inflation) const noexcept {
    // UE: 형상 클래스가 덮어쓰는 큰 bounds 상자다.
    return CollisionShape::Box(Bounds().boxExtent).Inflate(inflation);
}

void PrimitiveComponent::SetNotifyRigidBodyCollision(bool notify) noexcept {
    bodySettings_.notifyRigidBodyCollision = notify;
    // weld면 루트 body의 부분 표지를 바꾼다(UE UpdatePhysicsFilterData → WeldParent — 7.1.24절).
    if (IsWelded()) {
        ReweldToRoot();
        return;
    }

    if (bodyHandle_.IsNull()) {
        return;
    }

    if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
        scene->SetBodyNotifyContacts(bodyHandle_, notify);
    }
}

void PrimitiveComponent::UpdateBodyCollision() noexcept {
    if (!IsRegistered()) {
        return;
    }

    // NoCollision 경계를 넘으면 그 자리에서 만들거나 지운다(UE PrimitiveComponentPhysics.cpp:
    // 1376-1393). 실패는 RecreateSystemStateNow가 Warning으로 남기고 값은 바뀐 채 둔다(ERD25).
    if (WantsSystemState(SystemStateKind::Physics) != HasSystemState(SystemStateKind::Physics)) {
        (void)RecreateSystemStateNow(SystemStateKind::Physics);
        // 다시 만든 루트는 풀렸던 자손을 합친다(UE SetCollisionEnabled → ApplyWeldOnChildren —
        // PrimitiveComponentPhysics.cpp:1386-1391, 7.1.23절).
        ApplyWeldOnChildren();
    } else if (IsWelded()) {
        // weld 부분의 충돌 켬을 바꾼다(UE UpdatePhysicsFilterData → WeldParent — 7.1.23절).
        ReweldToRoot();
    } else if (!bodyHandle_.IsNull()) {
        PhysicsScene* scene = GetWorld().GetPhysicsScene();
        auto resolved = TryResolveCollision();
        if (scene != nullptr && resolved) {
            scene->SetBodyCollision(bodyHandle_, resolved.Value());
        }
    }

    // 응답·켬이 바뀌면 겹침이 달라진다(UE OnComponentCollisionSettingsChanged —
    // PrimitiveComponentPhysics.cpp:1434-1447, PhysicsModule 7.1.11절).
    UpdateOverlaps();
}

void PrimitiveComponent::UpdateBodyGeometry() noexcept {
    // weld면 새 형상으로 다시 합친다(7.1.23절).
    if (IsWelded()) {
        ReweldToRoot();
        UpdateOverlaps();
        return;
    }

    PhysicsScene* scene = bodyHandle_.IsNull() ? nullptr : GetWorld().GetPhysicsScene();
    BodyGeometry geometry(overlappingComponents_.GetAllocator());
    if (scene != nullptr) {
        auto built = BuildBodyGeometry(geometry);
        Result<void> updated = built ? scene->SetBodyGeometry(bodyHandle_, geometry.View())
                                     : Result<void>::Failure(built.Error());
        if (!updated) {
            LogContextScope context("World", GetWorld().Name());
            Log::Warning(
                LogWorld,
                "Primitive component #{Slot} body shape could not be updated: {Error}; keeping "
                "the previous shape ({Code})",
                Handle().slot, ErrorCodeName(updated.Error()), DiagBodyShapeUpdateFailed);
        }
    }

    // 형상 크기가 바뀌면 겹침이 달라진다(UE SetBoxExtent·SetSphereRadius·SetCapsuleSize의
    // bUpdateOverlaps 기본 참 — 7.1.11절).
    UpdateOverlaps();
}

void PrimitiveComponent::OnStart() noexcept {
    SceneComponent::OnStart();
    // 시뮬레이션하는 컴포넌트는 부착될 수 없다(UE UPrimitiveComponent::BeginPlay —
    // PrimitiveComponent.cpp:4378-4392). UE는 이 떼기의 overlap 갱신을 끈다 — 여기서는 소유 Actor가
    // 아직 Playing이 아니라 갱신하지 않고, Playing이 된 뒤 초기 겹침을 계산한다(7.1.11절).
    if (bodySettings_.simulatePhysics && !Parent().IsNull()) {
        (void)Detach(AttachmentRule::KeepWorld);
    }
}

Result<void> PrimitiveComponent::IgnoreActorWhenMoving(ObjectHandle actor, bool ignore) noexcept {
    return SetHandleInList(moveIgnoreActors_, actor, ignore);
}

Result<void> PrimitiveComponent::IgnoreComponentWhenMoving(ObjectHandle component,
                                                           bool ignore) noexcept {
    return SetHandleInList(moveIgnoreComponents_, component, ignore);
}

Result<bool> PrimitiveComponent::MoveComponent(const Vec3d& delta, const Quatd& rotation,
                                               bool sweep, HitResult* outHit,
                                               const MoveOptions& options) noexcept {
    const Vec3d traceStart = WorldLocation();
    const Vec3d traceEnd = traceStart + delta;
    if (outHit != nullptr) {
        *outHit = HitResult{};
        outHit->traceStart = traceStart;
        outHit->traceEnd = traceEnd;
    }

    // UE는 등록 뒤 Static 이동을 경고 뒤 거부한다(:3264-3271).
    if (auto movable = CheckMovable("move"); !movable) {
        return Result<bool>::Failure(movable.Error());
    }

    auto unit = rotation.TryNormalize();
    if (!unit || !math::IsFinite(delta.x) || !math::IsFinite(delta.y) || !math::IsFinite(delta.z)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    const Quatd initialRotation = WorldQuaternion();
    double deltaSizeSquared = (traceEnd - traceStart).LengthSquared();
    // sweep은 아주 작은 이동을 하지 않는다(질의가 그 거리를 다루지 않는다 — :3281-3296).
    const double minSweepDistance = MinSweepMovementScale * math::KindaSmallNumber<double>;
    const double minMovementSquared = sweep ? minSweepDistance * minSweepDistance : 0.0;
    if (deltaSizeSquared <= minMovementSquared) {
        if (SameMoveRotation(unit.Value(), initialRotation)) {
            return Result<bool>::Success(true);
        }

        deltaSizeSquared = 0.0;
    }

    HitResult blocking;
    bool hasBlocking = false;
    Vec3d newLocation = traceEnd;
    // sweep이 지나며 만난 겹침이다(UE PendingOverlaps — 7.1.11절). 할당 실패로 빠진 겹침은 끝 위치
    // 갱신이 다시 찾는다(지나치기만 한 겹침의 시작·끝은 잃는다).
    Array<PendingOverlap> pendingOverlaps(moveIgnoreActors_.GetAllocator());
    if (sweep) {
        newLocation = traceStart;
        auto resolved = TryResolveCollision();
        const bool queryEnabled =
            resolved && CollisionEnabledHasQuery(resolved.Value().collisionEnabled);
        if (HasWorld() && queryEnabled && deltaSizeSquared > 0.0) {
            QueryParams params;
            params.ignoreActors = moveIgnoreActors_.View();
            params.ignoreComponents = moveIgnoreComponents_.View();
            // 자기가 overlap 이벤트를 내지 않으면 겹침을 볼 일이 없다(:3351).
            params.ignoreTouches = !generateOverlapEvents_;
            Array<HitResult> hits(moveIgnoreActors_.GetAllocator());
            auto traced = ComponentTraceAll(GetWorld(), Handle(), traceStart, traceEnd,
                                            initialRotation, params, hits);
            if (!traced) {
                return Result<bool>::Failure(traced.Error());
            }

            const double deltaSize = math::Sqrt(deltaSizeSquared);
            for (HitResult& hit : hits) {
                PullBackHit(hit, deltaSize);
            }

            // 시작 겹침은 법선이 이동에 가장 맞서는 것, 없으면 첫 막음이다(:3380-3398). 막음이 아닌
            // hit는 대기 겹침이다(:3399-3420 — 막는 hit 뒤의 것은 보지 않는다).
            const Vec3d moveDirection = delta * (1.0 / deltaSize);
            usize blockingIndex = IndexNone;
            double blockingDot = NumericLimits<double>::Max();
            usize firstNonInitialOverlap = IndexNone;
            for (usize index = 0; index < hits.Size(); ++index) {
                const HitResult& hit = hits[index];
                if (hit.blockingHit) {
                    if (ShouldIgnoreHit(hit, moveDirection, options)) {
                        continue;
                    }

                    if (hit.startPenetrating) {
                        const double normalDotDelta = Dot(hit.impactNormal, delta);
                        if (normalDotDelta < blockingDot) {
                            blockingDot = normalDotDelta;
                            blockingIndex = index;
                        }
                    } else if (blockingIndex == IndexNone) {
                        blockingIndex = index;
                        break;
                    }
                } else if (generateOverlapEvents_ &&
                           IsOverlapCandidate(GetWorld(), hit.component, hit.actor)) {
                    if (blockingIndex != IndexNone && hit.time > hits[blockingIndex].time) {
                        break;
                    }

                    if (firstNonInitialOverlap == IndexNone && hit.time > 0.0) {
                        firstNonInitialOverlap = pendingOverlaps.Size();
                    }

                    bool known = false;
                    for (const PendingOverlap& pending : pendingOverlaps) {
                        known = known || pending.component == hit.component;
                    }

                    if (!known) {
                        (void)pendingOverlaps.TryPushBack(PendingOverlap{hit.component, true, hit});
                    }
                }
            }

            if (blockingIndex != IndexNone) {
                blocking = hits[blockingIndex];
                hasBlocking = true;
                newLocation = traceStart + (traceEnd - traceStart) * blocking.time;
                // 아주 작은 이동으로 면 위·안에 들어가지 않는다(:3434-3456). 시작에 머물면 지나가지
                // 않은 겹침은 버린다(시작 겹침만 남는다).
                if ((newLocation - traceStart).LengthSquared() <= minMovementSquared) {
                    newLocation = traceStart;
                    blocking.time = 0.0;
                    if (firstNonInitialOverlap != IndexNone) {
                        (void)pendingOverlaps.TryResize(firstNonInitialOverlap);
                    }
                }
            } else {
                newLocation = traceEnd;
            }
        } else if (deltaSizeSquared > 0.0) {
            // 질의 충돌이 꺼져 있으면 그냥 옮긴다(:3480-3485).
            newLocation = traceStart + delta;
        }
    }

    // 바뀌지 않으면 transform을 설정하지 않는다(UE InternalSetWorldLocationAndRotation — 같으면
    // UpdateComponentToWorld 없이 false, 자손 body·Render·bounds도 건드리지 않는다).
    const bool moved = !SameMoveLocation(newLocation, traceStart) ||
                       !SameMoveRotation(unit.Value(), initialRotation, MovedTolerance);
    if (moved) {
        // teleport 방식·skipPhysicsMove는 옮기기에만 적용한다(overlap hook의 다른 이동에 새지
        // 않는다 — UE는 인자로만 넘긴다).
        Result<void> placed = Result<void>::Success();
        {
            MoveTeleportScope teleport(options.teleport);
            const bool previousSkip = applyingPhysicsResult_;
            const bool previousDeferred = overlapUpdateDeferred_;
            applyingPhysicsResult_ = previousSkip || options.skipPhysicsMove;
            // 옮기는 setter의 자동 overlap 갱신 대신 대기 겹침으로 아래에서 갱신한다(7.1.11절).
            overlapUpdateDeferred_ = true;
            placed = SetWorldLocationAndRotation(newLocation, unit.Value());
            applyingPhysicsResult_ = previousSkip;
            overlapUpdateDeferred_ = previousDeferred;
        }

        if (!placed) {
            return Result<bool>::Failure(placed.Error());
        }

        // overlap 알림이 막는 hit 알림보다 먼저다(UE :3519-3535).
        UpdateOverlapsWith(pendingOverlaps.View());
    }

    // 막는 hit 발행(:3537-3550): 시작 겹침은 disableBlockingOverlapDispatch면 내지 않는다.
    if (hasBlocking && (!blocking.startPenetrating || !options.disableBlockingOverlapDispatch) &&
        GetState() != ComponentState::PendingDestroy) {
        DispatchBlockingHit(blocking);
    }

    if (outHit != nullptr && hasBlocking) {
        *outHit = blocking;
    }

    return Result<bool>::Success(moved);
}

void PrimitiveComponent::UpdateOverlaps() noexcept {
    UpdateOverlapsWith({});
}

void PrimitiveComponent::UpdateOverlapsWith(Span<const PendingOverlap> pending) noexcept {
    if (!HasWorld()) {
        return;
    }

    // 시작 전 Actor는 하지 않는다(자식도 — UE HasActorBegunPlay, :4475).
    World& world = GetWorld();
    auto ownerState = world.GetActorState(GetOwner());
    if (!ownerState || ownerState.Value() != ActorState::Playing) {
        return;
    }

    auto resolved = TryResolveCollision();
    const bool queryEnabled =
        resolved && CollisionEnabledHasQuery(resolved.Value().collisionEnabled);
    if (generateOverlapEvents_ && queryEnabled && IsRegistered()) {
        for (const PendingOverlap& overlap : pending) {
            BeginComponentOverlap(overlap);
        }

        // 대기 겹침 알림이 자기를 파괴하거나 표지를 껐을 수 있다(:4517).
        if (generateOverlapEvents_ && GetState() != ComponentState::PendingDestroy) {
            RefreshOverlaps(world);
        }
    } else {
        // 방금 꺼졌을 수 있다(:4620-4635).
        ClearComponentOverlaps();
    }

    SceneComponent::UpdateOverlaps();
}

void PrimitiveComponent::RefreshOverlaps(World& world) noexcept {
    const ObjectHandle self = Handle();
    const ObjectHandle owner = GetOwner();
    auto root =
        world.With<Actor>(owner, [](Actor& actor) noexcept { return actor.RootComponent(); });
    // 루트는 자기 Actor 전체를 뺀다(자식은 자기 차례에 갱신한다 — 7.1.11절).
    const bool isRoot = root && root.Value() == self;
    Allocator& allocator = overlappingComponents_.GetAllocator();
    Array<ObjectHandle> ignoreActors(allocator);
    if (!ignoreActors.TryAppend(moveIgnoreActors_.View()) ||
        (isRoot && !ignoreActors.TryPushBack(owner))) {
        return;
    }

    // 막음은 overlap 이벤트가 아니다(UE Params.bIgnoreBlocks — :4527).
    QueryParams params;
    params.ignoreBlocks = true;
    params.ignoreActors = ignoreActors.View();
    params.ignoreComponents = moveIgnoreComponents_.View();
    Array<OverlapResult> results(allocator);
    if (!ComponentOverlapAll(world, self, WorldLocation(), WorldQuaternion(), params, results)) {
        return;
    }

    Array<OverlapResult> current(allocator);
    if (!current.TryReserve(results.Size())) {
        return;
    }

    auto inCurrent = [&current](ObjectHandle component) noexcept {
        for (const OverlapResult& entry : current) {
            if (entry.component == component) {
                return true;
            }
        }

        return false;
    };

    for (const OverlapResult& result : results) {
        if (IsOverlapCandidate(world, result.component, result.actor) &&
            !inCurrent(result.component)) {
            (void)current.TryPushBack(result);
        }
    }

    // 끝이 먼저다(:4580-4610). 알림이 장부를 바꾸므로 옛 목록의 복사본으로 돈다.
    Array<ObjectHandle> previous(allocator);
    if (!previous.TryAppend(overlappingComponents_.View())) {
        return;
    }

    for (const ObjectHandle other : previous) {
        if (inCurrent(other)) {
            continue;
        }

        auto otherOwner = world.With<Component>(
            other, [](Component& component) noexcept { return component.GetOwner(); });
        if (!otherOwner) {
            // 사라진 컴포넌트는 알림 없이 지운다(UE의 무효 weak pointer 정리).
            (void)SetHandleInList(overlappingComponents_, other, false);
            continue;
        }

        // 루트 질의는 자기 Actor를 보지 않으므로 같은 Actor 겹침은 자식 쪽 갱신이 맡는다.
        if (isRoot && otherOwner.Value() == owner) {
            continue;
        }

        EndComponentOverlap(other);
    }

    for (const OverlapResult& entry : current) {
        BeginComponentOverlap(
            PendingOverlap{entry.component, false, OverlapInfoHit(entry.component, entry.actor)});
    }
}

bool PrimitiveComponent::IsOverlapCandidate(World& world, ObjectHandle component,
                                            ObjectHandle actor) const noexcept {
    if (component.IsNull() || component == Handle() || actor.IsNull()) {
        return false;
    }

    auto actorState = world.GetActorState(actor);
    if (!actorState || actorState.Value() == ActorState::Constructed) {
        return false;
    }

    auto generates = world.With<PrimitiveComponent>(
        component, [](PrimitiveComponent& other) noexcept { return other.generateOverlapEvents_; });
    return generates && generates.Value();
}

bool PrimitiveComponent::CanGenerateOverlapWith(const PrimitiveComponent& other) const noexcept {
    // UE CanComponentsGenerateOverlap(:224-231): 양쪽 표지, 양쪽 충돌 켬, 서로의 object type에 대한
    // 응답의 최소가 Overlap.
    if (!generateOverlapEvents_ || !other.generateOverlapEvents_) {
        return false;
    }

    auto mine = TryResolveCollision();
    auto theirs = other.TryResolveCollision();
    if (!mine || !theirs || mine.Value().collisionEnabled == CollisionEnabled::NoCollision ||
        theirs.Value().collisionEnabled == CollisionEnabled::NoCollision) {
        return false;
    }

    const CollisionResponse toOther = mine.Value().responses.Get(theirs.Value().objectType);
    const CollisionResponse toSelf = theirs.Value().responses.Get(mine.Value().objectType);
    const CollisionResponse weaker = toOther < toSelf ? toOther : toSelf;
    return weaker == CollisionResponse::Overlap;
}

void PrimitiveComponent::BeginComponentOverlap(const PendingOverlap& overlap) noexcept {
    // UE BeginComponentOverlap(:4100-4172).
    const ObjectHandle other = overlap.component;
    if (!HasWorld() || other.IsNull() || other == Handle() ||
        GetState() == ComponentState::PendingDestroy || IsOverlappingComponent(other)) {
        return;
    }

    World& world = GetWorld();
    struct OtherSide {
        bool allowed = false;
        ObjectHandle actor;
    };

    auto side = world.With<PrimitiveComponent>(other, [this](PrimitiveComponent& target) noexcept {
        OtherSide value;
        value.allowed =
            target.GetState() != ComponentState::PendingDestroy && CanGenerateOverlapWith(target);
        value.actor = target.GetOwner();
        return value;
    });
    if (!side || !side.Value().allowed) {
        return;
    }

    // 새 Actor 쌍인지는 장부에 넣기 전에 본다.
    const ObjectHandle self = Handle();
    const ObjectHandle owner = GetOwner();
    const ObjectHandle otherActor = side.Value().actor;
    const bool newActorPair = owner != otherActor && !IsOverlappingActor(world, owner, otherActor);
    // 할당 실패면 기록하지 않는다(다음 갱신 때 다시 시도한다).
    if (!overlappingComponents_.TryPushBack(other)) {
        return;
    }

    auto recorded =
        world.With<PrimitiveComponent>(other, [self](PrimitiveComponent& target) noexcept {
            return SetHandleInList(target.overlappingComponents_, self, true);
        });
    if (!recorded) {
        (void)SetHandleInList(overlappingComponents_, other, false);
        return;
    }

    // 알림은 World가 Playing일 때만이다(UE HasBegunPlay — :4131). 컴포넌트 이벤트(자기 → 상대) →
    // 새 Actor 쌍이면 Actor hook(자기 → 상대) → Actor 이벤트(자기 → 상대)다(:4133-4166).
    if (world.State() != WorldState::Playing) {
        return;
    }

    componentOverlapBegan.Broadcast({world.Objects(), world.Access(), self}, PrimitiveRef(self),
                                    ActorRef(otherActor), PrimitiveRef(other), overlap.fromSweep,
                                    overlap.hit);
    // 상대에게는 sweep이면 뒤집은 hit(이 쪽이 움직였다), 아니면 같은 hit다(UE).
    HitResult otherHit = overlap.hit;
    if (overlap.fromSweep) {
        otherHit.normal = overlap.hit.normal * -1.0;
        otherHit.impactNormal = overlap.hit.impactNormal * -1.0;
    }

    // 상대가 그 사이 파괴 대기가 됐으면 내지 않는다(UE IsValid(OtherComp)).
    if (CanNotifyComponent(world, other, false)) {
        (void)world.With<PrimitiveComponent>(other, [&](PrimitiveComponent& target) noexcept {
            target.componentOverlapBegan.Broadcast({world.Objects(), world.Access(), other},
                                                   PrimitiveRef(other), ActorRef(owner),
                                                   PrimitiveRef(self), overlap.fromSweep, otherHit);
        });
    }

    if (newActorPair) {
        CallActorOverlapHook(world, owner, otherActor, OverlapEdge::Begin, false);
        CallActorOverlapHook(world, otherActor, owner, OverlapEdge::Begin, false);
        BroadcastActorOverlap(world, owner, otherActor, OverlapEdge::Begin, false);
        BroadcastActorOverlap(world, otherActor, owner, OverlapEdge::Begin, false);
    }
}

void PrimitiveComponent::EndComponentOverlap(ObjectHandle other) noexcept {
    // UE EndComponentOverlap(:4174-4231): 상대 장부에서 빼고 자기 장부에서 뺀다.
    if (!HasWorld()) {
        return;
    }

    World& world = GetWorld();
    const ObjectHandle self = Handle();
    auto otherActor =
        world.With<PrimitiveComponent>(other, [self](PrimitiveComponent& target) noexcept {
            (void)SetHandleInList(target.overlappingComponents_, self, false);
            return target.GetOwner();
        });
    const bool recorded = IsOverlappingComponent(other);
    (void)SetHandleInList(overlappingComponents_, other, false);
    if (!recorded || !otherActor || otherActor.Value().IsNull() ||
        world.State() != WorldState::Playing) {
        return;
    }

    // 컴포넌트 이벤트(자기 → 상대) 뒤, 마지막 Actor 쌍 겹침이었으면 Actor마다 hook → 이벤트다(UE
    // :4206-4228). 파괴 대기 쪽은 받지 않는다(UE IsPrimCompValidAndAlive) — 단 자기 OnDestroy의
    // 모두 끝내기는 자기와 자기 Actor도 받는다(UE는 MarkAsGarbage 전에 끝낸다).
    const ObjectHandle owner = GetOwner();
    const ObjectHandle otherOwner = otherActor.Value();
    const bool destroying = clearingForDestroy_;
    if (CanNotifyComponent(world, self, destroying)) {
        componentOverlapEnded.Broadcast({world.Objects(), world.Access(), self}, PrimitiveRef(self),
                                        ActorRef(otherOwner), PrimitiveRef(other));
    }

    if (CanNotifyComponent(world, other, false)) {
        (void)world.With<PrimitiveComponent>(other, [&](PrimitiveComponent& target) noexcept {
            target.componentOverlapEnded.Broadcast({world.Objects(), world.Access(), other},
                                                   PrimitiveRef(other), ActorRef(owner),
                                                   PrimitiveRef(self));
        });
    }

    if (owner == otherOwner || IsOverlappingActor(world, owner, otherOwner)) {
        return;
    }

    CallActorOverlapHook(world, owner, otherOwner, OverlapEdge::End, destroying);
    BroadcastActorOverlap(world, owner, otherOwner, OverlapEdge::End, destroying);
    CallActorOverlapHook(world, otherOwner, owner, OverlapEdge::End, false);
    BroadcastActorOverlap(world, otherOwner, owner, OverlapEdge::End, false);
}

void PrimitiveComponent::ClearComponentOverlaps() noexcept {
    // UE ClearComponentOverlaps(:4690-4720): 알림이 장부를 바꾸므로 복사본으로 돈다. 복사 할당이
    // 실패하면 끝 칸부터 처음 개수만큼 끝낸다(알림이 더한 겹침은 다음 갱신 몫이다).
    Array<ObjectHandle> previous(overlappingComponents_.GetAllocator());
    if (previous.TryAppend(overlappingComponents_.View())) {
        for (const ObjectHandle other : previous) {
            EndComponentOverlap(other);
        }

        return;
    }

    for (usize remaining = overlappingComponents_.Size();
         remaining > 0 && overlappingComponents_.Size() > 0; --remaining) {
        EndComponentOverlap(overlappingComponents_[overlappingComponents_.Size() - 1]);
    }
}

bool PrimitiveComponent::IsOverlappingActor(World& world, ObjectHandle actor,
                                            ObjectHandle other) noexcept {
    if (actor.IsNull() || other.IsNull()) {
        return false;
    }

    auto found = world.With<Actor>(actor, [&world, other](Actor& owner) noexcept {
        for (const ObjectHandle component : owner.Components()) {
            auto overlapping = world.With<PrimitiveComponent>(
                component, [&world, other](PrimitiveComponent& primitive) noexcept {
                    for (const ObjectHandle entry : primitive.overlappingComponents_) {
                        auto entryOwner = world.With<Component>(
                            entry, [](Component& value) noexcept { return value.GetOwner(); });
                        if (entryOwner && entryOwner.Value() == other) {
                            return true;
                        }
                    }

                    return false;
                });
            if (overlapping && overlapping.Value()) {
                return true;
            }
        }

        return false;
    });
    return found && found.Value();
}

void PrimitiveComponent::OnDestroy(EndPlayReason reason) noexcept {
    SceneComponent::OnDestroy(reason);
    // UE OnComponentDestroyed(:2026-2044): 다시 겹치지 않게 표지를 끄고 모두 끝낸다(알림 있음).
    generateOverlapEvents_ = false;
    clearingForDestroy_ = true;
    ClearComponentOverlaps();
    clearingForDestroy_ = false;
}

void PrimitiveComponent::DispatchBlockingHit(const HitResult& hit) noexcept {
    // UE DispatchBlockingHit(:3588-3607)·AActor::InternalDispatchBlockingHit(Actor.cpp:3525-3556).
    if (hit.component.IsNull() || !HasWorld()) {
        return;
    }

    World& world = GetWorld();
    const ObjectHandle self = Handle();
    const ObjectHandle owner = GetOwner();
    // sweep hit의 충격량은 0이다.
    NotifyHitSide(world, owner, self, hit.actor, hit.component, true, Vec3d::Zero(), hit);
    // 발행 중 상대 컴포넌트가 파괴될 수 있다(UE IsValid(BlockingHitComponent)).
    auto otherState = world.GetComponentState(hit.component);
    if (!otherState || otherState.Value() == ComponentState::PendingDestroy) {
        return;
    }

    // 상대에게는 뒤집은 hit다(UE FHitResult::GetReversedHit — 법선 부호만 바꾼다).
    HitResult reversed = hit;
    reversed.normal = hit.normal * -1.0;
    reversed.impactNormal = hit.impactNormal * -1.0;
    NotifyHitSide(world, hit.actor, hit.component, owner, self, false, Vec3d::Zero(), reversed);
}

void PrimitiveComponent::DispatchPhysicsHit(World& world, const PhysicsContactHit& hit) noexcept {
    // 앞선 hit의 hook이 자기 컴포넌트·Actor를 파괴했을 수 있다(UE Info0.Actor.Get() 약한 참조,
    // IsActorValidToNotify).
    auto component = world.GetComponentState(hit.myComponent);
    if (!component || component.Value() == ComponentState::PendingDestroy) {
        return;
    }

    auto actor = world.GetActorState(hit.myActor);
    if (!actor || actor.Value() == ActorState::PendingDestroy) {
        return;
    }

    NotifyHitSide(world, hit.myActor, hit.myComponent, hit.otherActor, hit.otherComponent, true,
                  hit.normalImpulse, hit.hit);
}

Result<void> PrimitiveComponent::ApplyPhysicsTransform(const Vec3d& location,
                                                       const Quatd& rotation) noexcept {
    // overlap은 옮기기를 끝내고 표지를 되돌린 뒤 갱신한다: hook이 이 컴포넌트를 옮기면 body도
    // 따라가야 한다(UE MOVECOMP_SkipPhysicsMove는 인자라 hook에 새지 않는다 — 7.1.11절).
    const bool previousDeferred = overlapUpdateDeferred_;
    applyingPhysicsResult_ = true;
    overlapUpdateDeferred_ = true;
    overlapUpdateSkipped_ = false;
    auto moved = SetWorldLocationAndRotation(location, rotation);
    applyingPhysicsResult_ = false;
    overlapUpdateDeferred_ = previousDeferred;
    if (overlapUpdateSkipped_) {
        overlapUpdateSkipped_ = false;
        UpdateOverlaps();
    }

    return moved;
}

void PrimitiveComponent::OnTransformUpdated() noexcept {
    SceneComponent::OnTransformUpdated();
    // weld면 직접 바뀐 변경만 루트에 다시 합친다(UE OnUpdateTransform의 PropagateFromParent·
    // SetWeldedBodyTransform — 7.1.23절). 부모에서 전해진 변경은 루트 body가 이미 옮긴다.
    if (IsWelded()) {
        if (!IsTransformUpdateFromParent() && !applyingPhysicsResult_) {
            ReweldToRoot();
        }

        return;
    }

    if (bodyHandle_.IsNull() || applyingPhysicsResult_) {
        return;
    }

    if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
        // MoveComponent 범위 안이면 그 이동의 teleport 방식이다(자손도 같다 — 7.1.10절).
        scene->SetBodyTransform(bodyHandle_, BodyWorldTransform(), t_moveTeleport);
    }
}

Result<void> PrimitiveComponent::OnRegister() noexcept {
    if (auto base = SceneComponent::OnRegister(); !base) {
        return base;
    }

    const CollisionProfileTable& table = GetWorld().CollisionProfiles();
    const ResolvedCollision resolved =
        ResolveCollision(table, CollisionProfileName(), bodySettings_);
    if (resolved.profileMissing) {
        // UE는 경고 없이 이름을 Custom으로 바꾼다. Rocks는 이름을 두고 한 번 알린다(CollisionData
        // 5절).
        LogContextScope context("World", GetWorld().Name());
        Log::Warning(LogWorld,
                     "Primitive component #{Slot} collision profile '{Profile}' is not in the "
                     "profile table; using its custom collision values ({Code})",
                     Handle().slot, CollisionProfileName(), DiagCollisionProfileMissing);
        return Result<void>::Success();
    }

    return CopyResolvedProfile(table, resolved, bodySettings_);
}

} // namespace rocks

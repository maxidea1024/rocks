#include <Base/NumericLimits.h>
#include <Collision/BodySettings.generated.h>
#include <Collision/CollisionSettings.generated.h>
#include <Collision/CollisionTypes.generated.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
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

bool PrimitiveComponent::BuildBodyGeometry(BodyGeometry&) const noexcept {
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

    BodyGeometry geometry;
    if (!BuildBodyGeometry(geometry)) {
        return false;
    }

    auto resolved = TryResolveCollision();
    return resolved && resolved.Value().collisionEnabled != CollisionEnabled::NoCollision;
}

Result<void> PrimitiveComponent::CreateSystemState(SystemStateKind kind) noexcept {
    if (kind != SystemStateKind::Physics) {
        return SceneComponent::CreateSystemState(kind);
    }

    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    auto resolved = TryResolveCollision();
    if (scene == nullptr || !resolved) {
        return Result<void>::Failure(!resolved ? resolved.Error() : ErrorCode::NotReady);
    }

    BodyCreateDesc desc;
    if (!BuildBodyGeometry(desc.geometry)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

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

    if (!bodyHandle_.IsNull()) {
        if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
            scene->DestroyBody(bodyHandle_);
        }

        bodyHandle_ = {};
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
}

void PrimitiveComponent::UpdateBodyCollision() noexcept {
    if (!IsRegistered()) {
        return;
    }

    // NoCollision 경계를 넘으면 그 자리에서 만들거나 지운다(UE PrimitiveComponentPhysics.cpp:
    // 1376-1393). 실패는 RecreateSystemStateNow가 Warning으로 남기고 값은 바뀐 채 둔다(ERD25).
    if (WantsSystemState(SystemStateKind::Physics) != HasSystemState(SystemStateKind::Physics)) {
        (void)RecreateSystemStateNow(SystemStateKind::Physics);
        return;
    }

    if (bodyHandle_.IsNull()) {
        return;
    }

    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    auto resolved = TryResolveCollision();
    if (scene != nullptr && resolved) {
        scene->SetBodyCollision(bodyHandle_, resolved.Value());
    }
}

void PrimitiveComponent::UpdateBodyGeometry() noexcept {
    if (bodyHandle_.IsNull()) {
        return;
    }

    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    BodyGeometry geometry;
    if (scene == nullptr || !BuildBodyGeometry(geometry)) {
        return;
    }

    if (auto updated = scene->SetBodyGeometry(bodyHandle_, geometry); !updated) {
        LogContextScope context("World", GetWorld().Name());
        Log::Warning(
            LogWorld,
            "Primitive component #{Slot} body shape could not be updated: {Error}; keeping "
            "the previous shape ({Code})",
            Handle().slot, ErrorCodeName(updated.Error()), DiagBodyShapeUpdateFailed);
    }
}

void PrimitiveComponent::OnStart() noexcept {
    SceneComponent::OnStart();
    // 시뮬레이션하는 컴포넌트는 부착될 수 없다(UE UPrimitiveComponent::BeginPlay —
    // PrimitiveComponent.cpp:4378-4392). UE는 이 떼기의 overlap 갱신을 끈다 — overlap은 PH4.
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

            // 시작 겹침은 법선이 이동에 가장 맞서는 것, 없으면 첫 막음이다(:3380-3398).
            const Vec3d moveDirection = delta * (1.0 / deltaSize);
            usize blockingIndex = IndexNone;
            double blockingDot = NumericLimits<double>::Max();
            for (usize index = 0; index < hits.Size(); ++index) {
                const HitResult& hit = hits[index];
                if (!hit.blockingHit || ShouldIgnoreHit(hit, moveDirection, options)) {
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
            }

            if (blockingIndex != IndexNone) {
                blocking = hits[blockingIndex];
                hasBlocking = true;
                newLocation = traceStart + (traceEnd - traceStart) * blocking.time;
                // 아주 작은 이동으로 면 위·안에 들어가지 않는다(:3434-3456).
                if ((newLocation - traceStart).LengthSquared() <= minMovementSquared) {
                    newLocation = traceStart;
                    blocking.time = 0.0;
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
        MoveTeleportScope teleport(options.teleport);
        const bool previousSkip = applyingPhysicsResult_;
        applyingPhysicsResult_ = previousSkip || options.skipPhysicsMove;
        auto placed = SetWorldLocationAndRotation(newLocation, unit.Value());
        applyingPhysicsResult_ = previousSkip;
        if (!placed) {
            return Result<bool>::Failure(placed.Error());
        }
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

void PrimitiveComponent::DispatchBlockingHit(const HitResult& hit) noexcept {
    // UE DispatchBlockingHit(:3588-3607)·AActor::InternalDispatchBlockingHit(Actor.cpp:3525-3556).
    if (hit.component.IsNull() || !HasWorld()) {
        return;
    }

    World& world = GetWorld();
    const ObjectHandle self = Handle();
    const ObjectHandle owner = GetOwner();
    auto notify = [&world](ObjectHandle actor, ObjectHandle myComponent, ObjectHandle otherActor,
                           ObjectHandle otherComponent, bool selfMoved,
                           const HitResult& value) noexcept {
        // 파괴 대기 Actor에는 부르지 않는다(UE IsActorValidToNotify).
        auto state = world.GetActorState(actor);
        if (!state || state.Value() == ActorState::PendingDestroy) {
            return;
        }

        (void)world.With<Actor>(actor, [&](Actor& target) noexcept {
            target.OnHit(myComponent, otherActor, otherComponent, selfMoved, Vec3d::Zero(), value);
        });
    };

    notify(owner, self, hit.actor, hit.component, true, hit);
    // 발행 중 상대 컴포넌트가 파괴될 수 있다(UE IsValid(BlockingHitComponent)).
    auto otherState = world.GetComponentState(hit.component);
    if (!otherState || otherState.Value() == ComponentState::PendingDestroy) {
        return;
    }

    // 상대에게는 뒤집은 hit다(UE FHitResult::GetReversedHit — 법선 부호만 바꾼다).
    HitResult reversed = hit;
    reversed.normal = hit.normal * -1.0;
    reversed.impactNormal = hit.impactNormal * -1.0;
    notify(hit.actor, hit.component, owner, self, false, reversed);
}

Result<void> PrimitiveComponent::ApplyPhysicsTransform(const Vec3d& location,
                                                       const Quatd& rotation) noexcept {
    applyingPhysicsResult_ = true;
    auto moved = SetWorldLocationAndRotation(location, rotation);
    applyingPhysicsResult_ = false;
    return moved;
}

void PrimitiveComponent::OnTransformUpdated() noexcept {
    SceneComponent::OnTransformUpdated();
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

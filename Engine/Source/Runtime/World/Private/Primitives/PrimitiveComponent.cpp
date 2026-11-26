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
        scene->SetBodyTransform(bodyHandle_, BodyWorldTransform(), TeleportMode::None);
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

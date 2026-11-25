// PhysicsScene의 Jolt 구현(Docs/Design/PhysicsModule 2장·3.1절, 7.1.1절).
#include "JoltPhysicsScene.h"

#include "JoltBodyBuild.h"
#include "JoltConversions.h"

#include "PhysicsLog.h"

#include <Core/Math/MathUtility.h>
#include <Platform/FloatingPointEnvironment.h>

// clang-format off
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
// clang-format on
#include <Platform/Fatal.h>

namespace rocks::physics {

namespace {

// 한 step의 collision step 수다(3.1절 — 고정 스텝마다 1).
constexpr int CollisionStepsPerStep = 1;

// Jolt body 잠금 수다(0 = Jolt 기본값).
constexpr JPH::uint BodyMutexCount = 0;

} // namespace

JoltPhysicsScene::JoltPhysicsScene(Allocator& allocator, JoltRuntimeLease&& runtime,
                                   const PhysicsSettings& settings,
                                   WorkerScheduler* scheduler) noexcept
    : runtime_(Move(runtime)), temp_(settings.tempAllocatorBytes),
      jobs_(scheduler, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers), slots_(allocator),
      freeSlots_(allocator) {
    system_.Init(settings.maxBodies, BodyMutexCount, settings.maxBodyPairs,
                 settings.maxContactConstraints, broadPhaseLayers_, objectVsBroadPhase_,
                 layerPairs_);
    system_.SetGravity(
        ToJoltAcceleration(Vec3d(0.0, 0.0, static_cast<double>(settings.defaultGravityZ))));
}

JoltPhysicsScene::~JoltPhysicsScene() {
    // Jolt WaitForJobs가 끝난 뒤에도 Task가 job 참조를 놓는 중일 수 있다(6.7절).
    jobs_.WaitForOutstandingTasks();
}

Result<UniquePtr<JoltPhysicsScene>> JoltPhysicsScene::Create(Allocator& allocator,
                                                             const PhysicsSettings& settings,
                                                             WorkerScheduler* scheduler) noexcept {
    if (auto valid = PhysicsSettings::Validate(settings); !valid) {
        return Result<UniquePtr<JoltPhysicsScene>>::Failure(valid.Error());
    }

    auto runtime = JoltRuntimeLease::Acquire();
    if (!runtime) {
        return Result<UniquePtr<JoltPhysicsScene>>::Failure(runtime.Error());
    }

    return UniquePtr<JoltPhysicsScene>::TryMake(allocator, allocator, Move(runtime.Value()),
                                                settings, scheduler);
}

PhysicsSceneStats JoltPhysicsScene::Stats() const noexcept {
    PhysicsSceneStats stats;
    stats.bodies = system_.GetNumBodies();
    stats.activeBodies = system_.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    stats.steps = steps_;
    return stats;
}

const JoltPhysicsScene::BodySlot*
JoltPhysicsScene::FindSlot(PhysicsBodyHandle body) const noexcept {
    if (body.IsNull() || body.index >= slots_.Size()) {
        return nullptr;
    }

    const BodySlot& slot = slots_[body.index];
    return slot.live && slot.generation == body.generation ? &slot : nullptr;
}

Result<PhysicsBodyHandle> JoltPhysicsScene::CreateBody(const BodyCreateDesc& desc) noexcept {
    if (!desc.worldTransform.IsValid()) {
        return Result<PhysicsBodyHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto shape = MakeJoltShape(desc.geometry, desc.worldTransform.scale);
    if (!shape) {
        return Result<PhysicsBodyHandle>::Failure(shape.Error());
    }

    // 칸을 먼저 마련한다(Jolt body를 만든 뒤 할당이 실패하지 않게).
    uint32 index = 0;
    if (!freeSlots_.IsEmpty()) {
        auto popped = freeSlots_.TryPopBack();
        index = popped.Value();
    } else {
        // 빈 칸 목록도 칸 수만큼 미리 잡아 DestroyBody의 반납이 실패하지 않게 한다.
        if (auto reserved = freeSlots_.TryReserve(slots_.Size() + 1); !reserved) {
            return Result<PhysicsBodyHandle>::Failure(reserved.Error());
        }

        if (auto pushed = slots_.TryPushBack(BodySlot{}); !pushed) {
            return Result<PhysicsBodyHandle>::Failure(pushed.Error());
        }

        index = static_cast<uint32>(slots_.Size() - 1);
    }

    JPH::BodyCreationSettings settings(
        shape.Value(), ToJoltPosition(desc.worldTransform.translation, origin_),
        ToJoltRotation(desc.worldTransform.rotation), ToJoltMotion(desc.motion),
        ObjectLayerFor(desc.collision, desc.motion));
    // body 값(PH2b 몫 — 7.1.3절). Jolt 기본 감쇠 0.05에 맡기지 않는다. settings가 없으면
    // BodySettings 기본값(중력 켬, 선형 감쇠 DefaultLinearDamping, 각감쇠 0)이다.
    settings.mGravityFactor =
        desc.settings == nullptr || desc.settings->enableGravity ? 1.0f : 0.0f;
    settings.mLinearDamping =
        desc.settings != nullptr ? desc.settings->linearDamping : DefaultLinearDamping;
    settings.mAngularDamping = desc.settings != nullptr ? desc.settings->angularDamping : 0.0f;

    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    const JPH::BodyID created =
        bodies.CreateAndAddBody(settings, desc.motion == BodyMotion::Dynamic && desc.startAwake
                                              ? JPH::EActivation::Activate
                                              : JPH::EActivation::DontActivate);
    if (created.IsInvalid()) {
        (void)freeSlots_.TryPushBack(index);
        return Result<PhysicsBodyHandle>::Failure(ErrorCode::CapacityExceeded);
    }

    BodySlot& slot = slots_[index];
    slot.body = created;
    slot.live = true;
    return Result<PhysicsBodyHandle>::Success(PhysicsBodyHandle{index, slot.generation});
}

void JoltPhysicsScene::DestroyBody(PhysicsBodyHandle body) noexcept {
    if (FindSlot(body) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[body.index];
    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    bodies.RemoveBody(slot.body);
    bodies.DestroyBody(slot.body);
    slot.body = JPH::BodyID();
    slot.live = false;
    // 세대 0은 "없음"이라 건너뛴다.
    slot.generation = slot.generation + 1 == 0 ? 1 : slot.generation + 1;
    // CreateBody가 칸 수만큼 미리 잡았으므로 실패하지 않는다.
    (void)freeSlots_.TryPushBack(body.index);
}

void JoltPhysicsScene::SetBodyTransform(PhysicsBodyHandle body, const Transform& worldTransform,
                                        TeleportMode) noexcept {
    const BodySlot* slot = FindSlot(body);
    if (slot == nullptr || !worldTransform.IsValid()) {
        return;
    }

    // PH2b: 모든 운동 종류를 즉시 옮긴다(Dynamic 속도 유지 — 3.3절). 잠든 Dynamic은 깨운다(UE
    // SetGlobalPose가 잠든 입자를 깨운다 — ChaosEngineInterface.cpp:2559, Jolt는 Static이면
    // 무시한다). Kinematic MoveKinematic은 PH2c.
    system_.GetBodyInterface().SetPositionAndRotation(
        slot->body, ToJoltPosition(worldTransform.translation, origin_),
        ToJoltRotation(worldTransform.rotation), JPH::EActivation::Activate);
}

Result<BodyKinematics> JoltPhysicsScene::ReadKinematics(PhysicsBodyHandle body) const noexcept {
    const BodySlot* slot = FindSlot(body);
    if (slot == nullptr) {
        return Result<BodyKinematics>::Failure(ErrorCode::StaleHandle);
    }

    const JPH::BodyInterface& bodies = system_.GetBodyInterface();
    BodyKinematics kinematics;
    kinematics.location = FromJoltPosition(bodies.GetPosition(slot->body), origin_);
    kinematics.rotation = FromJoltRotation(bodies.GetRotation(slot->body));
    kinematics.linearVelocity = FromJoltLinearVelocity(bodies.GetLinearVelocity(slot->body));
    kinematics.angularVelocityRadians =
        FromJoltAngularVelocity(bodies.GetAngularVelocity(slot->body));
    kinematics.awake = bodies.IsActive(slot->body);
    return Result<BodyKinematics>::Success(kinematics);
}

Result<void> JoltPhysicsScene::Step(float seconds) noexcept {
    if (!(seconds > 0.0f) || !math::IsFinite(seconds)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // Jolt는 모든 실행 thread의 FP 상태가 같아야 한다(어댑터 경계 10.4절). worker job과 같은 결정적
    // 환경에서 step한다(barrier를 기다리는 이 thread도 job을 실행한다).
    struct StepCall {
        JoltPhysicsScene* scene;
        float seconds;
        JPH::EPhysicsUpdateError error;
    };

    StepCall call{this, seconds, JPH::EPhysicsUpdateError::None};
    if (platform::RunWithDeterministicFloatingPoint(
            [](void* context) noexcept {
                auto& step = *static_cast<StepCall*>(context);
                step.error = step.scene->system_.Update(step.seconds, CollisionStepsPerStep,
                                                        &step.scene->temp_, &step.scene->jobs_);
            },
            &call)) {
        ++floatingPointCorrections_;
    }

    const JPH::EPhysicsUpdateError error = call.error;
    // step이 끝난 뒤에도 Task·미룬 목록이 job 참조를 쥐고 있을 수 있다. step마다 놓아 다음 step이
    // 쓸 job 칸이 언제나 남게 한다(타이밍과 무관한 상한 — 7.1.1절).
    jobs_.WaitForOutstandingTasks();
    ++steps_;
    if (error != JPH::EPhysicsUpdateError::None) {
        Log::Warning(LogPhysics,
                     "Physics step {Step} exceeded a Jolt capacity (error bits {Bits}); some body "
                     "pairs or contacts were ignored",
                     steps_, static_cast<uint32>(error));
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

} // namespace rocks::physics

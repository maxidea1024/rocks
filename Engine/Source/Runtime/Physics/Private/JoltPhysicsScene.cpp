// PhysicsScene의 Jolt 구현(Docs/Design/PhysicsModule 2장·3.1절, 7.1.1절).
#include "JoltPhysicsScene.h"

#include "JoltBodyBuild.h"
#include "JoltConversions.h"

#include "PhysicsLog.h"

#include <Core/Algorithms.h>
#include <Core/Math/MathUtility.h>
#include <Platform/FloatingPointEnvironment.h>

// clang-format off
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
// clang-format on
#include <Platform/Fatal.h>

namespace rocks::physics {

namespace {

// Jolt body user data에 칸 번호(위 32 bit)와 세대(아래 32 bit)를 넣는다.
constexpr uint32 UserDataIndexShift = 32;

JPH::uint64 PackUserData(uint32 index, uint32 generation) noexcept {
    return (static_cast<JPH::uint64>(index) << UserDataIndexShift) | generation;
}

uint32 UserDataIndex(JPH::uint64 userData) noexcept {
    return static_cast<uint32>(userData >> UserDataIndexShift);
}

uint32 UserDataGeneration(JPH::uint64 userData) noexcept {
    return static_cast<uint32>(userData);
}

// 잠든 body 목록 용량 배수다: 한 step 사이에 칸 하나에서 지운 body와 새 body가 함께 잠들 수 있다.
constexpr usize DeactivationSlotsPerBody = 2;

// 한 step의 collision step 수다(3.1절 — 고정 스텝마다 1).
constexpr int CollisionStepsPerStep = 1;

// Jolt body 잠금 수다(0 = Jolt 기본값).
constexpr JPH::uint BodyMutexCount = 0;

} // namespace

JoltPhysicsScene::JoltPhysicsScene(Allocator& allocator, JoltRuntimeLease&& runtime,
                                   const PhysicsSettings& settings,
                                   WorkerScheduler* scheduler) noexcept
    : runtime_(Move(runtime)), temp_(settings.tempAllocatorBytes),
      jobs_(scheduler, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers), activations_(allocator),
      slots_(allocator), freeSlots_(allocator), pendingResults_(allocator),
      kinematicTargets_(allocator), kinematicMoving_(allocator), slept_(allocator),
      moved_(allocator) {
    system_.Init(settings.maxBodies, BodyMutexCount, settings.maxBodyPairs,
                 settings.maxContactConstraints, broadPhaseLayers_, objectVsBroadPhase_,
                 layerPairs_);
    system_.SetBodyActivationListener(&activations_);
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

        if (auto reserved = activations_.Reserve((slots_.Size() + 1) * DeactivationSlotsPerBody);
            !reserved) {
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

    settings.mUserData = PackUserData(index, slots_[index].generation);

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
    slot.component = desc.component;
    slot.motion = desc.motion;
    slot.live = true;
    slot.pendingResult = false;
    slot.queuedTarget = false;
    slot.hasTarget = false;
    slot.movedLastStep = false;
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
    slot.component = {};
    slot.live = false;
    slot.pendingResult = false;
    slot.hasTarget = false;
    slot.movedLastStep = false;
    // 세대 0은 "없음"이라 건너뛴다.
    slot.generation = slot.generation + 1 == 0 ? 1 : slot.generation + 1;
    // CreateBody가 칸 수만큼 미리 잡았으므로 실패하지 않는다.
    (void)freeSlots_.TryPushBack(body.index);
}

void JoltPhysicsScene::SetBodyTransform(PhysicsBodyHandle body, const Transform& worldTransform,
                                        TeleportMode mode) noexcept {
    if (FindSlot(body) == nullptr || !worldTransform.IsValid()) {
        return;
    }

    BodySlot& slot = slots_[body.index];
    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    const JPH::RVec3 position = ToJoltPosition(worldTransform.translation, origin_);
    const JPH::Quat rotation = ToJoltRotation(worldTransform.rotation);
    if (slot.motion == BodyMotion::Kinematic) {
        // 그 자리에서 옮긴다(질의가 바로 본다). step하는 장면의 물리적 이동은 시작 자세를 기록해
        // 다음 step이 MoveKinematic으로 속도를 준다(3.3절). teleport·step 없는 장면은 속도 0.
        if (simulates_ && mode == TeleportMode::None) {
            if (!slot.hasTarget) {
                // 목록에 한 번만 넣고, 다음 step에 움직임 목록에 옮길 용량도 함께 잡는다.
                const bool queued =
                    slot.queuedTarget || (kinematicTargets_.TryPushBack(body.index) &&
                                          kinematicMoving_.TryReserve(kinematicTargets_.Size()));
                if (!queued) {
                    // 목표를 기록하지 못하면 teleport로 둔다(속도 없이 도착 — 할당 실패 한정).
                    bodies.SetPositionAndRotation(slot.body, position, rotation,
                                                  JPH::EActivation::DontActivate);
                    bodies.SetLinearAndAngularVelocity(slot.body, JPH::Vec3::sZero(),
                                                       JPH::Vec3::sZero());
                    return;
                }

                slot.queuedTarget = true;
                slot.startPosition = bodies.GetPosition(slot.body);
                slot.startRotation = bodies.GetRotation(slot.body);
                slot.hasTarget = true;
            }

            slot.targetPosition = position;
            slot.targetRotation = rotation;
            bodies.SetPositionAndRotation(slot.body, position, rotation,
                                          JPH::EActivation::DontActivate);
            return;
        }

        slot.hasTarget = false;
        bodies.SetPositionAndRotation(slot.body, position, rotation,
                                      JPH::EActivation::DontActivate);
        bodies.SetLinearAndAngularVelocity(slot.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        return;
    }

    // Static·Dynamic은 즉시 옮긴다(Dynamic 속도 유지 — 3.3절). 잠든 Dynamic은 깨운다(UE
    // SetGlobalPose가 잠든 입자를 깨운다 — ChaosEngineInterface.cpp:2559, Jolt는 Static이면
    // 무시한다).
    bodies.SetPositionAndRotation(slot.body, position, rotation, JPH::EActivation::Activate);
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

void JoltPhysicsScene::ActivationListener::OnBodyDeactivated(const JPH::BodyID&,
                                                             JPH::uint64 userData) {
    platform::LockGuard lock(mutex_);
    // 할당하지 않는다(클래스 설명) — 용량이 모자라면 세기만 한다.
    if (deactivated_.Size() < deactivated_.Capacity()) {
        (void)deactivated_.TryPushBack(userData);
    } else {
        ++dropped_;
    }
}

Result<void> JoltPhysicsScene::ActivationListener::Reserve(usize capacity) noexcept {
    platform::LockGuard lock(mutex_);
    return deactivated_.TryReserve(capacity);
}

uint64 JoltPhysicsScene::ActivationListener::TakeInto(Array<JPH::uint64>& out) noexcept {
    platform::LockGuard lock(mutex_);
    for (JPH::uint64 userData : deactivated_) {
        if (!out.TryPushBack(userData)) {
            ++dropped_;
        }
    }

    deactivated_.Clear();
    const uint64 dropped = dropped_;
    dropped_ = 0;
    return dropped;
}

void JoltPhysicsScene::MarkMoved(JPH::uint64 userData) noexcept {
    const PhysicsBodyHandle handle{UserDataIndex(userData), UserDataGeneration(userData)};
    if (FindSlot(handle) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[handle.index];
    if (slot.motion != BodyMotion::Dynamic || slot.pendingResult) {
        return;
    }

    if (pendingResults_.TryPushBack(handle)) {
        slot.pendingResult = true;
    }
}

void JoltPhysicsScene::ApplyKinematicTargets(float seconds) noexcept {
    JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
    // 지난 step에 움직였고 이번 step에 목표가 없는 Kinematic은 멈춘다(Jolt 속도가 남는다 — 3.3절의
    // "현재 자세로 MoveKinematic"과 같은 결과).
    for (uint32 index : kinematicMoving_) {
        BodySlot& slot = slots_[index];
        if (slot.live && slot.movedLastStep && !slot.hasTarget) {
            bodies.SetLinearAndAngularVelocity(slot.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }

        slot.movedLastStep = false;
    }

    // kinematicMoving_ 용량은 목표를 넣을 때 kinematicTargets_만큼 잡았으므로 아래 넣기는 실패하지
    // 않는다.
    kinematicMoving_.Clear();
    for (uint32 index : kinematicTargets_) {
        BodySlot& slot = slots_[index];
        slot.queuedTarget = false;
        if (!slot.live || !slot.hasTarget) {
            continue;
        }

        bodies.SetPositionAndRotation(slot.body, slot.startPosition, slot.startRotation,
                                      JPH::EActivation::DontActivate);
        bodies.MoveKinematic(slot.body, slot.targetPosition, slot.targetRotation, seconds);
        slot.hasTarget = false;
        if (kinematicMoving_.TryPushBack(index)) {
            slot.movedLastStep = true;
        }
    }

    kinematicTargets_.Clear();
}

void JoltPhysicsScene::CollectMovedBodies() noexcept {
    // step 밖 게임 thread이므로 잠금 없는 interface로 읽는다.
    system_.GetActiveBodies(JPH::EBodyType::RigidBody, activeBodies_);
    const JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
    for (const JPH::BodyID& id : activeBodies_) {
        MarkMoved(bodies.GetUserData(id));
    }

    slept_.Clear();
    if (const uint64 dropped = activations_.TakeInto(slept_); dropped != 0) {
        Log::Warning(LogPhysics,
                     "Physics step {Step} lost {Count} body sleep notices; those bodies keep their "
                     "previous component pose",
                     steps_, dropped);
    }

    for (JPH::uint64 userData : slept_) {
        MarkMoved(userData);
    }
}

Result<Span<const MovedBody>> JoltPhysicsScene::TakeMovedBodies() noexcept {
    moved_.Clear();
    // 미리 잡아 두어 중간 실패로 일부만 꺼내는 일이 없게 한다(실패하면 목록을 그대로 둔다).
    if (auto reserved = moved_.TryReserve(pendingResults_.Size()); !reserved) {
        return Result<Span<const MovedBody>>::Failure(reserved.Error());
    }

    algo::Sort(pendingResults_.View(),
               [](const PhysicsBodyHandle& a, const PhysicsBodyHandle& b) noexcept {
                   return a.index < b.index;
               });
    const JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
    for (const PhysicsBodyHandle& handle : pendingResults_) {
        // 그 사이 지워졌거나 칸이 재사용됐으면 세대가 다르다.
        if (FindSlot(handle) == nullptr) {
            continue;
        }

        BodySlot& slot = slots_[handle.index];
        slot.pendingResult = false;
        if (slot.motion != BodyMotion::Dynamic) {
            continue;
        }

        MovedBody moved;
        moved.component = slot.component;
        moved.location = FromJoltPosition(bodies.GetPosition(slot.body), origin_);
        moved.rotation = FromJoltRotation(bodies.GetRotation(slot.body));
        (void)moved_.TryPushBack(moved);
    }

    pendingResults_.Clear();
    return Result<Span<const MovedBody>>::Success(moved_.View());
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
                // MoveKinematic의 속도 계산도 같은 FP 환경에서 한다.
                step.scene->ApplyKinematicTargets(step.seconds);
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
    CollectMovedBodies();
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

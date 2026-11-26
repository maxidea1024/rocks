// PhysicsScene의 Jolt 구현(Docs/Design/PhysicsModule 2장·3.1절, 7.1.1절).
#include "JoltPhysicsScene.h"

#include "JoltBodyBuild.h"
#include "JoltConversions.h"

#include "PhysicsLog.h"

#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>
#include <Platform/FloatingPointEnvironment.h>

// clang-format off
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
// clang-format on
#include <Platform/Fatal.h>

namespace rocks::physics {

namespace {

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
    : runtime_(Move(runtime)), responseSets_(allocator), layerPairs_(responseSets_),
      temp_(settings.tempAllocatorBytes),
      jobs_(scheduler, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers), activations_(allocator),
      slots_(allocator), freeSlots_(allocator), pendingResults_(allocator),
      kinematicTargets_(allocator), kinematicMoving_(allocator), slept_(allocator),
      moved_(allocator), queryHits_(allocator) {
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
    stats.responseSets = responseSets_.Count();
    return stats;
}

void JoltPhysicsScene::AssertNotStepping() const noexcept {
    ROCKS_ASSERT(!stepping_);
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
    AssertNotStepping();
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

    auto responseSet = responseSets_.Acquire(desc.collision.responses);
    if (!responseSet) {
        (void)freeSlots_.TryPushBack(index);
        if (responseSet.Error() == ErrorCode::CapacityExceeded) {
            Log::Warning(LogPhysics,
                         "Physics scene ran out of collision response sets; the body was not "
                         "created ({Code})",
                         DiagResponseSetCapacity);
        }

        return Result<PhysicsBodyHandle>::Failure(responseSet.Error());
    }

    JPH::BodyCreationSettings settings(
        shape.Value(), ToJoltPosition(desc.worldTransform.translation, origin_),
        ToJoltRotation(desc.worldTransform.rotation), ToJoltMotion(desc.motion),
        ObjectLayerFor(desc.collision, desc.motion, responseSet.Value()));
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
        responseSets_.Release(responseSet.Value());
        (void)freeSlots_.TryPushBack(index);
        return Result<PhysicsBodyHandle>::Failure(ErrorCode::CapacityExceeded);
    }

    BodySlot& slot = slots_[index];
    slot.body = created;
    slot.component = desc.component;
    slot.actor = desc.actor;
    slot.motion = desc.motion;
    slot.live = true;
    slot.responseSet = responseSet.Value();
    slot.geometry = desc.geometry;
    slot.scale = desc.worldTransform.scale;
    slot.updateMassOnShapeChange =
        desc.settings == nullptr || desc.settings->updateMassWhenScaleChanges;
    slot.pendingResult = false;
    slot.queuedTarget = false;
    slot.hasTarget = false;
    slot.movedLastStep = false;
    return Result<PhysicsBodyHandle>::Success(PhysicsBodyHandle{index, slot.generation});
}

void JoltPhysicsScene::DestroyBody(PhysicsBodyHandle body) noexcept {
    AssertNotStepping();
    if (FindSlot(body) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[body.index];
    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    bodies.RemoveBody(slot.body);
    bodies.DestroyBody(slot.body);
    responseSets_.Release(slot.responseSet);
    slot.responseSet = 0;
    slot.body = JPH::BodyID();
    slot.component = {};
    slot.actor = {};
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
    AssertNotStepping();
    if (FindSlot(body) == nullptr || !worldTransform.IsValid()) {
        return;
    }

    BodySlot& slot = slots_[body.index];
    // scale이 바뀌면 형상부터 바꾼다(UE UpdateBodyScale — 7.1.5절, 비교는 UE Scale3D.Equals처럼
    // 성분마다 KindaSmallNumber 허용 오차). 유효한 transform이라 실패하지 않는다.
    const Vec3d scaleChange = worldTransform.scale - slot.scale;
    const double tolerance = math::KindaSmallNumber<double>;
    if (math::Abs(scaleChange.x) > tolerance || math::Abs(scaleChange.y) > tolerance ||
        math::Abs(scaleChange.z) > tolerance) {
        (void)ReplaceShape(slot, slot.geometry, worldTransform.scale);
    }

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

void JoltPhysicsScene::SetBodyMotion(PhysicsBodyHandle body, BodyMotion motion,
                                     bool wake) noexcept {
    AssertNotStepping();
    if (FindSlot(body) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[body.index];
    // Static이 끼는 전환은 Mobility 변경이라 body를 다시 만든다(2.5절 — Jolt Static body는 운동
    // 속성이 없다).
    if (slot.motion == motion || slot.motion == BodyMotion::Static ||
        motion == BodyMotion::Static) {
        return;
    }

    // 대기 Kinematic 목표·정지 표지는 Kinematic에만 뜻이 있다(목록에 남은 칸은 다음 step이
    // 건너뛴다). 결과 목록 표지는 둔다: Dynamic이던 step의 자세가 아직 컴포넌트에 가지 않았을 수
    // 있다.
    slot.hasTarget = false;
    slot.movedLastStep = false;
    slot.motion = motion;
    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    if (motion == BodyMotion::Kinematic) {
        // UE SetIsKinematic은 Kinematic 전환 때 속도를 0으로
        // 둔다(ChaosEngineInterface.cpp:536-541). Jolt Kinematic은 남은 속도로 계속 움직이므로
        // 반드시 지운다.
        bodies.SetMotionType(slot.body, JPH::EMotionType::Kinematic,
                             JPH::EActivation::DontActivate);
        bodies.SetLinearAndAngularVelocity(slot.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        return;
    }

    bodies.SetMotionType(slot.body, JPH::EMotionType::Dynamic,
                         wake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
}

void JoltPhysicsScene::SetBodyCollision(PhysicsBodyHandle body,
                                        const ResolvedCollision& collision) noexcept {
    AssertNotStepping();
    if (FindSlot(body) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[body.index];
    // 새 묶음을 먼저 얻고 층을 바꾼 뒤 옛 묶음을 놓는다(같은 묶음이면 같은 칸 — 7.1.6절). 새 묶음을
    // 얻지 못하면 옛 묶음 색인을 둔 채 나머지 bit(object type·참여 표지)만 바꾼다.
    uint32 responseSet = slot.responseSet;
    auto acquired = responseSets_.Acquire(collision.responses);
    if (acquired) {
        responseSet = acquired.Value();
    } else if (acquired.Error() == ErrorCode::CapacityExceeded) {
        Log::Warning(
            LogPhysics,
            "Physics scene ran out of collision response sets; the body keeps its previous "
            "responses ({Code})",
            DiagResponseSetCapacity);
    } else {
        Log::Warning(LogPhysics,
                     "Physics body collision responses were not updated: {Error}; the body keeps "
                     "its previous responses",
                     ErrorCodeName(acquired.Error()));
    }

    // broadphase 층이 바뀌면 Jolt가 body를 옮긴다. UE는 충돌 필터가 바뀌면 body를 늘 깨운다
    // (BodyInstance.cpp:1125-1126 — Static은 깨울 것이 없다).
    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    bodies.SetObjectLayer(slot.body, ObjectLayerFor(collision, slot.motion, responseSet));
    if (slot.motion != BodyMotion::Static) {
        bodies.ActivateBody(slot.body);
    }

    if (acquired) {
        responseSets_.Release(slot.responseSet);
        slot.responseSet = responseSet;
    }
}

Result<void> JoltPhysicsScene::SetBodyGeometry(PhysicsBodyHandle body,
                                               const BodyGeometry& geometry) noexcept {
    AssertNotStepping();
    if (FindSlot(body) == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    BodySlot& slot = slots_[body.index];
    return ReplaceShape(slot, geometry, slot.scale);
}

Result<void> JoltPhysicsScene::ReplaceShape(BodySlot& slot, const BodyGeometry& geometry,
                                            const Vec3d& scale) noexcept {
    auto shape = MakeJoltShape(geometry, scale);
    if (!shape) {
        return Result<void>::Failure(shape.Error());
    }

    // UE UpdateBodyScale: 형상을 바꾸고 깨우며, updateMassWhenScaleChanges면 질량을 다시 계산한다
    // (BodyInstance.cpp:2545-2575). Jolt는 Static을 깨우지 않는다.
    system_.GetBodyInterface().SetShape(slot.body, shape.Value().GetPtr(),
                                        slot.updateMassOnShapeChange, JPH::EActivation::Activate);
    slot.geometry = geometry;
    slot.scale = scale;
    return Result<void>::Success();
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
    AssertNotStepping();
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

        // 그 사이 Kinematic이 된 칸도 쓴다(Dynamic이던 step의 자세 — Kinematic은 컴포넌트 자세와
        // 같으므로 써도 바뀌지 않는다).
        BodySlot& slot = slots_[handle.index];
        slot.pendingResult = false;
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
    stepping_ = true;
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

    stepping_ = false;
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

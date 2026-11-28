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
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
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

// 도/s → rad/s 배율이다.
constexpr double RadiansPerDegree = math::Pi<double> / 180.0;

// 두 값의 결합이다(UE EFrictionCombineMode — 7.1.15절).
float CombineValues(CombineMode mode, float a, float b) noexcept {
    switch (mode) {
    case CombineMode::Min:
        return a < b ? a : b;
    case CombineMode::Multiply:
        return a * b;
    case CombineMode::Max:
        return a < b ? b : a;
    case CombineMode::Average:
        break;
    }

    return 0.5f * (a + b);
}

// Jolt 결합 함수(상태 없는 함수 포인터 — 장면마다 넣는다)다: 마찰·반발 × 결합 방식.
template <bool Friction, CombineMode Mode>
float CombineBodies(const JPH::Body& a, const JPH::SubShapeID&, const JPH::Body& b,
                    const JPH::SubShapeID&) {
    return Friction ? CombineValues(Mode, a.GetFriction(), b.GetFriction())
                    : CombineValues(Mode, a.GetRestitution(), b.GetRestitution());
}

template <bool Friction>
JPH::ContactConstraintManager::CombineFunction CombineFunctionOf(CombineMode mode) noexcept {
    switch (mode) {
    case CombineMode::Min:
        return &CombineBodies<Friction, CombineMode::Min>;
    case CombineMode::Multiply:
        return &CombineBodies<Friction, CombineMode::Multiply>;
    case CombineMode::Max:
        return &CombineBodies<Friction, CombineMode::Max>;
    case CombineMode::Average:
        break;
    }

    return &CombineBodies<Friction, CombineMode::Average>;
}

// 접촉 listener 용량의 하한, 지난 step 대비 배수, 알림 body 하나당 잡는 수다(7.1.12절).
constexpr usize MinContactNoticeCapacity = 64;
constexpr usize ContactNoticeGrowth = 2;
constexpr usize ContactNoticesPerNotifyBody = 4;

// 세 값 가운데 가장 큰 것이다(math::Max는 실수 전용).
constexpr usize LargestOf(usize a, usize b, usize c) noexcept {
    const usize ab = a > b ? a : b;
    return ab > c ? ab : c;
}

// CCD(LinearCast) body인지다 — 추정 충격량은 CCD로만 닿은 쌍에만 쓰인다(6.4절).
bool UsesLinearCast(const JPH::Body& body) noexcept {
    return !body.IsStatic() && body.GetMotionPropertiesUnchecked()->GetMotionQuality() ==
                                   JPH::EMotionQuality::LinearCast;
}

// Jolt 벡터 둘의 사전식 비교다(-1·0·1).
int CompareVectors(JPH::Vec3Arg a, JPH::Vec3Arg b) noexcept {
    for (int axis = 0; axis < 3; ++axis) {
        if (a[axis] != b[axis]) {
            return a[axis] < b[axis] ? -1 : 1;
        }
    }

    return 0;
}

// probe 참여 표지가 있는 body인지다(그 접촉은 반작용이 없다 — 6.4절). compound 층은 잎 단계에서
// 본다(7.1.24절).
bool IsProbeBody(const JPH::Body& body) noexcept {
    const JPH::ObjectLayer layer = body.GetObjectLayer();
    return !IsCompoundLayer(layer) &&
           (ObjectLayerParticipation(layer) & LayerParticipationProbe) != 0;
}

// object 층 참여 bit의 body 충돌 켬이다(원소 값과 교집합을 낼 때 — 7.1.21절).
CollisionEnabled LayerCollisionEnabled(JPH::ObjectLayer layer) noexcept {
    const uint32 participation = ObjectLayerParticipation(layer);
    const bool query = (participation & LayerParticipationQuery) != 0;
    if ((participation & LayerParticipationPhysics) != 0) {
        return query ? CollisionEnabled::QueryAndPhysics : CollisionEnabled::PhysicsOnly;
    }

    if ((participation & LayerParticipationProbe) != 0) {
        return query ? CollisionEnabled::QueryAndProbe : CollisionEnabled::ProbeOnly;
    }

    return query ? CollisionEnabled::QueryOnly : CollisionEnabled::NoCollision;
}

// 접촉한 하위 형상의 부분 번호다(표지가 없으면 루트 0).
uint32 ContactPart(const JPH::Body& body, const JPH::SubShapeID& subShape) noexcept {
    ElementTag tag;
    return UnpackElementUserData(body.GetShape()->GetSubShapeUserData(subShape), tag) ? tag.part
                                                                                      : 0;
}

// 접촉 정렬 순서다(7.1.12절): 칸 번호가 작은 쪽·큰 쪽, 하위 형상 ID 둘, 깊이, 접촉점 좌표, 법선,
// 추정 충격량(값이 모두 같으면 어느 쪽이 먼저여도 결과가 같다).
template <class Notice>
bool ContactNoticeBefore(const Notice& a, const Notice& b) noexcept {
    const uint32 a1 = UserDataIndex(a.userData1);
    const uint32 a2 = UserDataIndex(a.userData2);
    const uint32 b1 = UserDataIndex(b.userData1);
    const uint32 b2 = UserDataIndex(b.userData2);
    const uint32 aLow = a1 < a2 ? a1 : a2;
    const uint32 bLow = b1 < b2 ? b1 : b2;
    if (aLow != bLow) {
        return aLow < bLow;
    }

    const uint32 aHigh = a1 < a2 ? a2 : a1;
    const uint32 bHigh = b1 < b2 ? b2 : b1;
    if (aHigh != bHigh) {
        return aHigh < bHigh;
    }

    // 같은 body 쌍 안에서는 부분 쌍 순이다(결정적 — 묶음의 첫 통지가 hit 컴포넌트를 정한다,
    // 7.1.22절).
    if (a.part1 != b.part1) {
        return a.part1 < b.part1;
    }

    if (a.part2 != b.part2) {
        return a.part2 < b.part2;
    }

    if (a.subShape1.GetValue() != b.subShape1.GetValue()) {
        return a.subShape1.GetValue() < b.subShape1.GetValue();
    }

    if (a.subShape2.GetValue() != b.subShape2.GetValue()) {
        return a.subShape2.GetValue() < b.subShape2.GetValue();
    }

    if (a.depth != b.depth) {
        return a.depth < b.depth;
    }

    for (int axis = 0; axis < 3; ++axis) {
        if (a.point[axis] != b.point[axis]) {
            return a.point[axis] < b.point[axis];
        }
    }

    if (const int normal = CompareVectors(a.normal, b.normal); normal != 0) {
        return normal < 0;
    }

    return CompareVectors(a.estimatedImpulse, b.estimatedImpulse) < 0;
}

} // namespace

JoltPhysicsScene::JoltPhysicsScene(Allocator& allocator, JoltRuntimeLease&& runtime,
                                   const PhysicsSettings& settings,
                                   WorkerScheduler* scheduler) noexcept
    : runtime_(Move(runtime)), responseSets_(allocator), layerPairs_(responseSets_),
      temp_(settings.tempAllocatorBytes),
      jobs_(scheduler, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers), activations_(allocator),
      contacts_(*this, allocator), slots_(allocator), freeSlots_(allocator),
      pendingResults_(allocator), kinematicTargets_(allocator), kinematicMoving_(allocator),
      wakeEventBodies_(allocator), wakeEvents_(allocator), transitionData_(allocator),
      transitionAwake_(allocator), pendingImpulses_(allocator), frameForces_(allocator),
      slept_(allocator), moved_(allocator), contactHits_(allocator), takenContactHits_(allocator),
      minDeltaVelocityForHitEvents_(static_cast<double>(settings.minDeltaVelocityForHitEvents)),
      maxAngularVelocityDegrees_(static_cast<double>(settings.maxAngularVelocity)),
      maxAngularVelocityRadians_(
          static_cast<float>(static_cast<double>(settings.maxAngularVelocity) * RadiansPerDegree)),
      maxLinearVelocityMeters_(static_cast<float>(
          math::CentimetersToMeters(static_cast<double>(settings.maxLinearVelocity)))),
      restitutionThreshold_(static_cast<double>(settings.restitutionThreshold)),
      defaultDegreesOfFreedom_(settings.defaultDegreesOfFreedom), queryHits_(allocator) {
    shapeOptions_.collisionMarginFraction = static_cast<double>(settings.collisionMarginFraction);
    shapeOptions_.collisionMarginMax = static_cast<double>(settings.collisionMarginMax);
    system_.Init(settings.maxBodies, BodyMutexCount, settings.maxBodyPairs,
                 settings.maxContactConstraints, broadPhaseLayers_, objectVsBroadPhase_,
                 layerPairs_);
    system_.SetBodyActivationListener(&activations_);
    system_.SetContactListener(&contacts_);
    system_.SetSimShapeFilter(&simShapeFilter_);
    system_.SetGravity(
        ToJoltAcceleration(Vec3d(0.0, 0.0, static_cast<double>(settings.defaultGravityZ))));
    // 결합 방식(6.6절·7.1.15절 — Jolt 기본 마찰 기하평균·반발 최대 대신 UE). 반발 문턱은 step dt에
    // 따라 Step이 넣는다.
    system_.SetCombineFriction(CombineFunctionOf<true>(settings.frictionCombineMode));
    system_.SetCombineRestitution(CombineFunctionOf<false>(settings.restitutionCombineMode));
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

    const BodyMassValues mass = MassValuesOf(desc.settings);
    if (!AreMassValuesValid(mass)) {
        return Result<PhysicsBodyHandle>::Failure(ErrorCode::InvalidArgument);
    }

    // 형상과 칸에 둘 원소 값·부분 표·잎 형상을 먼저 만든다(Jolt body를 만든 뒤 할당이 실패하지
    // 않게). 부분은 루트 자신 하나다(7.1.22절).
    Array<BodyShapeElement> geometry(slots_.GetAllocator());
    Array<ElementShape> elementShapes(slots_.GetAllocator());
    Array<BodyPart> parts(slots_.GetAllocator());
    if (auto reserved = geometry.TryReserve(desc.geometry.Size()); !reserved) {
        return Result<PhysicsBodyHandle>::Failure(reserved.Error());
    }

    for (const BodyShapeElement& element : desc.geometry) {
        (void)geometry.TryPushBack(element);
    }

    BodyPart root;
    root.component = desc.component;
    root.actor = desc.actor;
    root.count = desc.geometry.Size();
    root.mass = mass;
    root.collisionEnabled = desc.collision.collisionEnabled;
    root.objectType = desc.collision.objectType;
    root.notifyContacts = desc.settings != nullptr && desc.settings->notifyRigidBodyCollision;
    if (auto pushed = parts.TryPushBack(root); !pushed) {
        return Result<PhysicsBodyHandle>::Failure(pushed.Error());
    }

    auto shape = MakeBodyShape(desc.geometry, parts.View(), desc.worldTransform.scale,
                               shapeOptions_, &elementShapes);
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

        if (auto pushed = slots_.TryPushBack(BodySlot(slots_.GetAllocator())); !pushed) {
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

    const BodyMass bodyMass = ComputeBodyMass(shape.Value().inner, elementShapes.View(),
                                              parts.View(), desc.worldTransform.scale);
    JPH::BodyCreationSettings settings(
        bodyMass.shape, ToJoltPosition(desc.worldTransform.translation, origin_),
        ToJoltRotation(desc.worldTransform.rotation), ToJoltMotion(desc.motion),
        ObjectLayerFor(desc.collision, desc.motion, responseSet.Value()));
    // body 값(PH2b 몫 — 7.1.3절). Jolt 기본 감쇠 0.05에 맡기지 않는다. settings가 없으면
    // BodySettings 기본값(중력 켬, 선형 감쇠 DefaultLinearDamping, 각감쇠 0)이다.
    settings.mGravityFactor =
        desc.settings == nullptr || desc.settings->enableGravity ? 1.0f : 0.0f;
    settings.mLinearDamping =
        desc.settings != nullptr ? desc.settings->linearDamping : DefaultLinearDamping;
    settings.mAngularDamping = desc.settings != nullptr ? desc.settings->angularDamping : 0.0f;

    // CCD(6.5절 — UE bUseCCD).
    if (desc.settings != nullptr && desc.settings->continuousCollision) {
        settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    }

    // 질량·관성은 UE 규칙으로 계산해 넣는다(7.1.15절 — Jolt 형상 기본 계산 대신).
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
    settings.mMassPropertiesOverride = bodyMass.properties;
    // 기본 물리 재질(6.6절 — 재질 에셋 전).
    settings.mFriction = DefaultPhysicalMaterialFriction;
    settings.mRestitution = DefaultPhysicalMaterialRestitution;
    // 속도 상한·solver·그 밖(6.5절 표).
    settings.mMaxAngularVelocity = maxAngularVelocityRadians_;
    if (desc.settings != nullptr && desc.settings->overrideMaxAngularVelocity) {
        settings.mMaxAngularVelocity = static_cast<float>(
            static_cast<double>(desc.settings->maxAngularVelocity) * RadiansPerDegree);
    }

    settings.mMaxLinearVelocity = maxLinearVelocityMeters_;
    if (desc.settings != nullptr) {
        if (desc.settings->overrideSolverIterations) {
            settings.mNumPositionStepsOverride = desc.settings->positionIterations;
            settings.mNumVelocityStepsOverride = desc.settings->velocityIterations;
        }

        settings.mApplyGyroscopicForce = desc.settings->gyroscopicTorque;
        settings.mEnhancedInternalEdgeRemoval = desc.settings->smoothEdgeCollisions;
    }

    // 자유도 잠금(7.1.16절 — Dynamic만, 모두 잠긴 조합은 잠그지 않는다).
    const DofLock dof = ResolveDofLock(desc.settings, defaultDegreesOfFreedom_);
    if (desc.motion == BodyMotion::Dynamic) {
        settings.mAllowedDOFs = dof.allowed;
    }

    // gyroscopic 힘은 회전 축을 잠그면 끈다(Jolt ApplyGyroscopicForceInternal이 각속도를 축
    // 마스크로 거르지 않아 잠긴 축으로 샌다 — 7.1.16절 한계).
    if (dof.LocksRotation()) {
        settings.mApplyGyroscopicForce = false;
    }

    settings.mUserData = PackUserData(index, slots_[index].generation);
    // 원소 충돌 켬이 섞이면 manifold reduction을 끈다(원소별 probe — 7.1.21절).
    settings.mUseManifoldReduction = !shape.Value().mixedCollision;

    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    // 생성의 활성은 깨움 이벤트가 아니다(UE 생성 상태 — 7.1.19절).
    activations_.SetRecordTransitions(false);
    const JPH::BodyID created =
        bodies.CreateAndAddBody(settings, desc.motion == BodyMotion::Dynamic && desc.startAwake
                                              ? JPH::EActivation::Activate
                                              : JPH::EActivation::DontActivate);
    activations_.SetRecordTransitions(true);
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
    slot.geometry = Move(geometry);
    slot.elementShapes = Move(elementShapes);
    slot.parts = Move(parts);
    slot.parts[0].responseSet = slot.responseSet;
    slot.parts[0].layer = settings.mObjectLayer;
    slot.inner = shape.Value().inner;
    slot.massShift = bodyMass.shift;
    slot.scale = desc.worldTransform.scale;
    slot.updateMassOnShapeChange =
        desc.settings == nullptr || desc.settings->updateMassWhenScaleChanges;
    slot.mass = mass;
    slot.massProperties = settings.mMassPropertiesOverride;
    slot.dof = dof;
    slot.dofConstraint = nullptr;
    if (desc.motion == BodyMotion::Dynamic) {
        ApplyDegreesOfFreedom(slot);
    }

    slot.pendingResult = false;
    slot.notifyContacts = desc.settings != nullptr && desc.settings->notifyRigidBodyCollision;
    slot.stepStartStamp = 0;
    if (slot.notifyContacts) {
        ++notifyBodies_;
    }

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
    // 구속을 body보다 먼저 뗀다(구속이 body를 가리킨다).
    DetachDofConstraint(slot);
    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    // 삭제의 비활성은 잠듦 이벤트가 아니다(세대 검사로 버려질 기록이 용량을 쓰지 않게 끈다).
    activations_.SetRecordTransitions(false);
    bodies.RemoveBody(slot.body);
    activations_.SetRecordTransitions(true);
    bodies.DestroyBody(slot.body);
    responseSets_.Release(slot.responseSet);
    slot.responseSet = 0;
    // weld 부분의 응답 묶음을 놓는다(루트 부분은 칸 것 — 7.1.24절).
    for (usize part = 1; part < slot.parts.Size(); ++part) {
        responseSets_.Release(slot.parts[part].responseSet);
    }

    slot.body = JPH::BodyID();
    slot.component = {};
    slot.actor = {};
    slot.live = false;
    slot.pendingResult = false;
    if (slot.notifyContacts) {
        --notifyBodies_;
    }

    slot.notifyContacts = false;
    slot.inner = nullptr;
    slot.elementShapes.Clear();
    slot.geometry.Clear();
    slot.parts.Clear();
    slot.hasTarget = false;
    slot.movedLastStep = false;
    slot.wakeEvent = WakeTransition::None;
    slot.queuedWakeEvent = false;
    DropPendingImpulse(slot);
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
    // 성분마다 KindaSmallNumber 허용 오차). 실패(할당·Jolt 형상)하면 이전 형상을 두고 알린다.
    const Vec3d scaleChange = worldTransform.scale - slot.scale;
    const double tolerance = math::KindaSmallNumber<double>;
    if (math::Abs(scaleChange.x) > tolerance || math::Abs(scaleChange.y) > tolerance ||
        math::Abs(scaleChange.z) > tolerance) {
        const Span<const BodyShapeElement> rootGeometry(slot.geometry.Data(), slot.parts[0].count);
        if (auto replaced = ReplaceShape(slot, rootGeometry, worldTransform.scale); !replaced) {
            Log::Warning(LogPhysics,
                         "Physics body #{Slot} kept its previous shape; rebuilding it for the new "
                         "scale failed: {Error} ({Code})",
                         body.index, ErrorCodeName(replaced.Error()), DiagBodyShapeRebuildFailed);
        }
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
    // 자유도 구속은 옮긴 자세를 새 기준으로 다시 붙인다(PMD19 — teleport한 값을 유지한다, 축 bit
    // 잠금과 같다).
    if (slot.dofConstraint != nullptr) {
        DetachDofConstraint(slot);
        ApplyDegreesOfFreedom(slot);
    }
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

    // 앞선 전이를 옛 운동 종류로 옮긴다(Kinematic이 되면 깨움·잠듦 이벤트가 없다 — 7.1.19절).
    CollectWakeTransitions();
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
        // 대기 충격량·프레임 힘을 버린다(Chaos Kinematic은 impulse velocity·힘을 적분하지 않는다 —
        // 7.1.17절). Jolt 누적 힘은 SetMotionType(Kinematic)이 지운다(Body.cpp:62-65). 쌓인 깨움·
        // 잠듦 이벤트도 버린다(Kinematic은 이벤트가 없다 — 7.1.19절).
        DropPendingImpulse(slot);
        slot.wakeEvent = WakeTransition::None;

        // Kinematic은 자유도를 막지 않는다(7.1.16절 — Jolt 허용 자유도는 MoveKinematic 속도도
        // 막는다).
        ApplyDegreesOfFreedom(slot);
        return;
    }

    // 전환의 깨움은 이벤트가 아니고 쌓인 이벤트를 지운다(UE SetInstanceSimulatePhysics →
    // WakeInstance — 7.1.19절).
    WithoutWakeEvents(slot, wake, [&]() noexcept {
        bodies.SetMotionType(slot.body, JPH::EMotionType::Dynamic,
                             wake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    });
    ApplyDegreesOfFreedom(slot);
}

void JoltPhysicsScene::ApplyDegreesOfFreedom(BodySlot& slot) noexcept {
    const bool dynamic = slot.motion == BodyMotion::Dynamic;
    {
        JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
        if (!lock.Succeeded() || lock.GetBody().IsStatic()) {
            return;
        }

        // 허용 자유도는 질량 속성과 함께 넣고(Jolt가 잠긴 축의 역질량·역관성을 0으로 둔다), 남은
        // 속도를 새 마스크로 거른다(SetMassProperties는 속도를 거르지 않는다 — Body.cpp:424의
        // 순서).
        JPH::MotionProperties* motion = lock.GetBody().GetMotionProperties();
        motion->SetMassProperties(dynamic ? slot.dof.allowed : JPH::EAllowedDOFs::All,
                                  slot.massProperties);
        motion->SetLinearVelocityClamped(motion->GetLinearVelocity());
        motion->SetAngularVelocityClamped(motion->GetAngularVelocity());
    }

    if (!dynamic || !slot.dof.constraint) {
        DetachDofConstraint(slot);
        return;
    }

    if (slot.dofConstraint != nullptr) {
        return;
    }

    // body 대 월드 6자유도 구속(월드 공간, 위치 = 지금 body 원점 — UE CreateDOFLock의 Pos2, X축 =
    // 법선). 기준은 붙이는 때의 자세다(만들 때·Dynamic 전환·teleport·형상 변경 — PMD19).
    JPH::SixDOFConstraintSettings constraint;
    constraint.mSpace = JPH::EConstraintSpace::WorldSpace;
    const JPH::Vec3 normal =
        slot.dof.lockAll ? JPH::Vec3::sAxisX() : ToJoltNormal(slot.dof.constraintNormal);
    constraint.mAxisX1 = normal;
    constraint.mAxisX2 = normal;
    constraint.mAxisY1 = normal.GetNormalizedPerpendicular();
    constraint.mAxisY2 = constraint.mAxisY1;
    if (slot.dof.lockAll) {
        for (int axis = 0; axis < static_cast<int>(JPH::SixDOFConstraintSettings::EAxis::Num);
             ++axis) {
            constraint.MakeFixedAxis(static_cast<JPH::SixDOFConstraintSettings::EAxis>(axis));
        }
    } else {
        if (slot.dof.lockNormalTranslation) {
            constraint.MakeFixedAxis(JPH::SixDOFConstraintSettings::EAxis::TranslationX);
        }

        if (slot.dof.lockPlaneRotation) {
            constraint.MakeFixedAxis(JPH::SixDOFConstraintSettings::EAxis::RotationY);
            constraint.MakeFixedAxis(JPH::SixDOFConstraintSettings::EAxis::RotationZ);
        }
    }

    {
        JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
        if (!lock.Succeeded()) {
            return;
        }

        constraint.mPosition1 = lock.GetBody().GetPosition();
        constraint.mPosition2 = constraint.mPosition1;
        slot.dofConstraint = constraint.Create(JPH::Body::sFixedToWorld, lock.GetBody());
    }

    system_.AddConstraint(slot.dofConstraint.GetPtr());
}

void JoltPhysicsScene::DetachDofConstraint(BodySlot& slot) noexcept {
    if (slot.dofConstraint == nullptr) {
        return;
    }

    system_.RemoveConstraint(slot.dofConstraint.GetPtr());
    slot.dofConstraint = nullptr;
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
    // (BodyInstance.cpp:1125-1126 — Static은 깨울 것이 없다). 루트 부분 값을 바꾸고 부분 층·body
    // 층(weld면 compound)을 다시 만든다(7.1.24절).
    const uint32 previousSet = slot.responseSet;
    slot.responseSet = responseSet;
    slot.parts[0].responseSet = responseSet;
    slot.parts[0].collisionEnabled = collision.collisionEnabled;
    slot.parts[0].objectType = collision.objectType;
    RefreshPartLayers(slot);
    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    // 깨움 이벤트는 없고 쌓인 이벤트를 지운다(UE WakeUp_AssumesLocked — 7.1.19절).
    if (slot.motion != BodyMotion::Static) {
        WithoutWakeEvents(slot, true, [&]() noexcept { bodies.ActivateBody(slot.body); });
    }

    if (acquired) {
        responseSets_.Release(previousSet);
    }
}

Result<void> JoltPhysicsScene::SetBodyGeometry(PhysicsBodyHandle body,
                                               Span<const BodyShapeElement> geometry) noexcept {
    AssertNotStepping();
    if (FindSlot(body) == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    BodySlot& slot = slots_[body.index];
    return ReplaceShape(slot, geometry, slot.scale);
}

Result<void> JoltPhysicsScene::CopyParts(const BodySlot& slot, usize skip,
                                         const Span<const BodyShapeElement>* replaceRoot,
                                         Array<BodyShapeElement>& values,
                                         Array<BodyPart>& parts) const noexcept {
    usize total = 0;
    for (usize index = 0; index < slot.parts.Size(); ++index) {
        if (index != skip) {
            total += index == 0 && replaceRoot != nullptr ? replaceRoot->Size()
                                                          : slot.parts[index].count;
        }
    }

    if (auto reserved = values.TryEnsureCapacity(values.Size() + total); !reserved) {
        return reserved;
    }

    if (auto reserved = parts.TryEnsureCapacity(parts.Size() + slot.parts.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < slot.parts.Size(); ++index) {
        if (index == skip) {
            continue;
        }

        BodyPart part = slot.parts[index];
        const Span<const BodyShapeElement> source =
            index == 0 && replaceRoot != nullptr
                ? *replaceRoot
                : Span<const BodyShapeElement>(slot.geometry.Data() + part.first, part.count);
        part.first = values.Size();
        part.count = source.Size();
        for (const BodyShapeElement& element : source) {
            (void)values.TryPushBack(element);
        }

        (void)parts.TryPushBack(part);
    }

    return Result<void>::Success();
}

Result<void> JoltPhysicsScene::ReplaceShape(BodySlot& slot, Span<const BodyShapeElement> geometry,
                                            const Vec3d& scale) noexcept {
    // 새 값을 다 만든 뒤 바꾼다(실패하면 이전 형상·값을 둔다). geometry가 slot.geometry 자신일 수
    // 있어 사본을 먼저 만든다. weld 부분은 그대로 잇는다.
    Array<BodyShapeElement> values(slots_.GetAllocator());
    Array<BodyPart> parts(slots_.GetAllocator());
    if (auto copied = CopyParts(slot, IndexNone, &geometry, values, parts); !copied) {
        return copied;
    }

    return ApplyShape(slot, Move(values), Move(parts), scale, ShapeChange::Resize);
}

Result<void> JoltPhysicsScene::ApplyShape(BodySlot& slot, Array<BodyShapeElement>&& values,
                                          Array<BodyPart>&& parts, const Vec3d& scale,
                                          ShapeChange change) noexcept {
    Array<ElementShape> elementShapes(slots_.GetAllocator());
    parts[0].mass = slot.mass;
    auto shape = MakeBodyShape(values.View(), parts.View(), scale, shapeOptions_, &elementShapes);
    if (!shape) {
        return Result<void>::Failure(shape.Error());
    }

    const BodyMass bodyMass =
        ComputeBodyMass(shape.Value().inner, elementShapes.View(), parts.View(), scale);
    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    if (change == ShapeChange::Resize) {
        // UE UpdateBodyScale: 형상을 바꾸고 깨운다(BodyInstance.cpp:2545-2575). Jolt는 Static을
        // 깨우지 않는다. 형상 변경의 깨움은 이벤트가 아니고 쌓인 이벤트를 지운다(UE UpdateBodyScale
        // → WakeUp_AssumesLocked — 7.1.19절).
        WithoutWakeEvents(slot, true, [&]() noexcept {
            bodies.SetShape(slot.body, bodyMass.shape.GetPtr(), false, JPH::EActivation::Activate);
        });
    } else {
        // UE Weld·UnWeld는 깨우지 않는다(형상 교체와 PostShapeChange뿐 — 7.1.22절).
        bodies.SetShape(slot.body, bodyMass.shape.GetPtr(), false, JPH::EActivation::DontActivate);
    }

    // 질량은 Jolt 기본 계산 대신 UE 규칙으로 넣는다(7.1.15절): 크기 변경은
    // updateMassWhenScaleChanges일 때, weld·unweld는 늘(UE PostShapeChange → UpdateMassProperties).
    const bool updateMass = change == ShapeChange::Weld || slot.updateMassOnShapeChange;
    if (updateMass && slot.motion != BodyMotion::Static) {
        JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
        if (lock.Succeeded()) {
            slot.massProperties = bodyMass.properties;
            lock.GetBody().GetMotionProperties()->SetMassProperties(
                slot.motion == BodyMotion::Dynamic ? slot.dof.allowed : JPH::EAllowedDOFs::All,
                slot.massProperties);
        }
    }

    // 형상이 바뀌면 질량 중심이 옮겨질 수 있다(질량 중심 이동 × scale) — 구속은 body 변경을
    // 모르므로 다시 붙인다(Jolt Constraint.h:162-166).
    if (slot.dofConstraint != nullptr) {
        DetachDofConstraint(slot);
        ApplyDegreesOfFreedom(slot);
    }

    slot.geometry = Move(values);
    slot.parts = Move(parts);
    slot.elementShapes = Move(elementShapes);
    slot.inner = shape.Value().inner;
    slot.massShift = bodyMass.shift;
    slot.scale = scale;
    // 부분이 바뀌면 부분 층·body 층(compound)·알림 표지를 다시 만든다(7.1.24절).
    RefreshPartLayers(slot);
    return Result<void>::Success();
}

Result<void> JoltPhysicsScene::WeldBody(PhysicsBodyHandle root,
                                        const BodyWeldDesc& child) noexcept {
    AssertNotStepping();
    if (FindSlot(root) == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    BodySlot& slot = slots_[root.index];
    if (child.component.IsNull() || child.component == slot.parts[0].component ||
        child.geometry.IsEmpty() || !child.relative.IsValid() ||
        !AreMassValuesValid(MassValuesOf(child.settings))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 같은 컴포넌트가 이미 부분이면 그 부분을 새 값으로 바꾼다(UE SetWeldedBodyTransform =
    // UnWeld + Weld — 맨 뒤로 간다).
    usize existing = IndexNone;
    for (usize index = 1; index < slot.parts.Size(); ++index) {
        if (slot.parts[index].component == child.component) {
            existing = index;
        }
    }

    Array<BodyShapeElement> values(slots_.GetAllocator());
    Array<BodyPart> parts(slots_.GetAllocator());
    if (auto copied = CopyParts(slot, existing, nullptr, values, parts); !copied) {
        return copied;
    }

    if (parts.Size() >= BodyWeldMaxParts ||
        values.Size() + child.geometry.Size() > BodyGeometryMaxElements) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 부분 응답 묶음을 얻는다(실패하면 아무것도 바꾸지 않는다 — 7.1.24절).
    auto responseSet = responseSets_.Acquire(child.responses);
    if (!responseSet) {
        if (responseSet.Error() == ErrorCode::CapacityExceeded) {
            Log::Warning(LogPhysics,
                         "Physics scene ran out of collision response sets; the part was not "
                         "welded ({Code})",
                         DiagResponseSetCapacity);
        }

        return Result<void>::Failure(responseSet.Error());
    }

    const uint32 replacedSet = existing != IndexNone ? slot.parts[existing].responseSet : 0;
    BodyPart part;
    part.component = child.component;
    part.actor = child.actor;
    part.mass = MassValuesOf(child.settings);
    part.objectType = child.objectType;
    part.responseSet = responseSet.Value();
    part.notifyContacts = child.settings != nullptr && child.settings->notifyRigidBodyCollision;
    part.first = values.Size();
    part.count = child.geometry.Size();
    part.relative = child.relative;
    part.welded = true;
    part.collisionEnabled = child.collisionEnabled;
    if (auto reserved = values.TryEnsureCapacity(values.Size() + child.geometry.Size());
        !reserved) {
        responseSets_.Release(responseSet.Value());
        return reserved;
    }

    for (const BodyShapeElement& element : child.geometry) {
        (void)values.TryPushBack(element);
    }

    Result<void> applied = parts.TryPushBack(part);
    if (applied) {
        applied = ApplyShape(slot, Move(values), Move(parts), slot.scale, ShapeChange::Weld);
    }

    // 실패면 새 묶음을, 성공이면 바꾼 부분의 옛 묶음을 놓는다.
    responseSets_.Release(applied ? replacedSet : responseSet.Value());
    return applied;
}

void JoltPhysicsScene::UnweldBody(PhysicsBodyHandle root, ObjectHandle child) noexcept {
    AssertNotStepping();
    if (FindSlot(root) == nullptr || child.IsNull()) {
        return;
    }

    BodySlot& slot = slots_[root.index];
    for (usize index = 1; index < slot.parts.Size(); ++index) {
        if (slot.parts[index].component != child) {
            continue;
        }

        Array<BodyShapeElement> values(slots_.GetAllocator());
        Array<BodyPart> parts(slots_.GetAllocator());
        const uint32 removedSet = slot.parts[index].responseSet;
        auto applied = CopyParts(slot, index, nullptr, values, parts);
        if (applied) {
            applied = ApplyShape(slot, Move(values), Move(parts), slot.scale, ShapeChange::Weld);
        }

        // 뺀 부분의 응답 묶음을 놓는다(7.1.24절).
        if (applied) {
            responseSets_.Release(removedSet);
        }

        if (!applied) {
            Log::Warning(LogPhysics,
                         "Physics body #{Slot} kept a welded part; rebuilding its shape without "
                         "the part failed: {Error} ({Code})",
                         root.index, ErrorCodeName(applied.Error()), DiagBodyShapeRebuildFailed);
        }

        return;
    }
}

const JoltPhysicsScene::BodyPart& JoltPhysicsScene::PartOf(const BodySlot& slot,
                                                           uint32 part) noexcept {
    return part < slot.parts.Size() ? slot.parts[part] : slot.parts[0];
}

void JoltPhysicsScene::SetBodyNotifyContacts(PhysicsBodyHandle body, bool notify) noexcept {
    AssertNotStepping();
    if (FindSlot(body) == nullptr) {
        return;
    }

    // 루트 부분 표지를 바꾸고 칸 표지(부분 합)를 다시 만든다(7.1.24절).
    BodySlot& slot = slots_[body.index];
    slot.parts[0].notifyContacts = notify;
    RefreshPartLayers(slot);
}

bool JoltPhysicsScene::WantsContacts(JPH::uint64 userData) const noexcept {
    const BodySlot* slot =
        FindSlot(PhysicsBodyHandle{UserDataIndex(userData), UserDataGeneration(userData)});
    return slot != nullptr && slot->notifyContacts;
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
    kinematics.centerOfMass = FromJoltPosition(bodies.GetCenterOfMassPosition(slot->body), origin_);
    // UE는 rigid body(Dynamic·Kinematic)만 질량·관성을 낸다(GetMass_AssumesLocked).
    if (slot->motion != BodyMotion::Static) {
        kinematics.mass = static_cast<double>(slot->massProperties.mMass);
        kinematics.inertia = PrincipalInertia(slot->massProperties.mInertia);
    }
    kinematics.linearVelocity = FromJoltLinearVelocity(bodies.GetLinearVelocity(slot->body));
    kinematics.angularVelocityRadians =
        FromJoltAngularVelocity(bodies.GetAngularVelocity(slot->body));
    kinematics.awake = bodies.IsActive(slot->body);
    return Result<BodyKinematics>::Success(kinematics);
}

void JoltPhysicsScene::ActivationListener::OnBodyActivated(const JPH::BodyID&,
                                                           JPH::uint64 userData) {
    platform::LockGuard lock(mutex_);
    RecordTransition(userData, true);
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

    RecordTransition(userData, false);
}

void JoltPhysicsScene::ActivationListener::RecordTransition(JPH::uint64 userData,
                                                            bool awake) noexcept {
    // mutex_ 아래에서 부른다. 할당하지 않는다(용량이 모자라면 센다).
    if (!recordTransitions_) {
        return;
    }

    if (transitionData_.Size() < transitionData_.Capacity() &&
        transitionAwake_.Size() < transitionAwake_.Capacity()) {
        (void)transitionData_.TryPushBack(userData);
        (void)transitionAwake_.TryPushBack(awake);
    } else {
        ++droppedTransitions_;
    }
}

void JoltPhysicsScene::ActivationListener::SetRecordTransitions(bool record) noexcept {
    platform::LockGuard lock(mutex_);
    recordTransitions_ = record;
}

uint64 JoltPhysicsScene::ActivationListener::TakeTransitionsInto(Array<JPH::uint64>& userData,
                                                                 Array<bool>& awake) noexcept {
    platform::LockGuard lock(mutex_);
    for (usize index = 0; index < transitionData_.Size(); ++index) {
        if (!userData.TryPushBack(transitionData_[index]) ||
            !awake.TryPushBack(transitionAwake_[index])) {
            ++droppedTransitions_;
        }
    }

    transitionData_.Clear();
    transitionAwake_.Clear();
    const uint64 dropped = droppedTransitions_;
    droppedTransitions_ = 0;
    return dropped;
}

Result<void> JoltPhysicsScene::ActivationListener::Reserve(usize capacity) noexcept {
    platform::LockGuard lock(mutex_);
    if (auto reserved = deactivated_.TryReserve(capacity); !reserved) {
        return reserved;
    }

    if (auto reserved = transitionData_.TryReserve(capacity); !reserved) {
        return reserved;
    }

    return transitionAwake_.TryReserve(capacity);
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

bool JoltPhysicsScene::ElementQueryFilter::ShouldCollide(const JPH::Shape* shape2,
                                                         const JPH::SubShapeID&) const {
    // body 값의 질의 켬은 층 필터가 이미 봤다 — 원소 값만 본다(교집합의 질의 = 둘 다 질의).
    ElementTag tag;
    if (!UnpackElementUserData(shape2->GetUserData(), tag)) {
        return true;
    }

    if (!CollisionEnabledHasQuery(tag.enabled)) {
        return false;
    }

    // 부분이 둘 이상인 body는 무시 목록·제외를 부분마다 본다(body 필터가 미룬다 — 7.1.22절).
    const JPH::uint64 userData = scene_.system_.GetBodyInterfaceNoLock().GetUserData(mBodyID2);
    const BodySlot* slot = scene_.FindSlot({UserDataIndex(userData), UserDataGeneration(userData)});
    if (slot == nullptr || slot->parts.Size() <= 1) {
        return true;
    }

    // 부분 층으로 채널·응답을 다시 판정한다(compound body — 7.1.24절).
    const BodyPart& part = PartOf(*slot, tag.part);
    if (!layerFilter_.ShouldCollide(part.layer)) {
        return false;
    }

    return !scene_.IsExcludedPart(part, params_, exclusion_);
}

bool JoltPhysicsScene::ElementQueryFilter::ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&,
                                                         const JPH::Shape* shape2,
                                                         const JPH::SubShapeID& subShape2) const {
    return ShouldCollide(shape2, subShape2);
}

bool JoltPhysicsScene::ElementSimFilter::ShouldCollide(
    const JPH::Body& body1, const JPH::Shape* shape1, const JPH::SubShapeID&,
    const JPH::Body& body2, const JPH::Shape* shape2, const JPH::SubShapeID&) const {
    if (!scene_.ElementSimulates(body1, shape1) || !scene_.ElementSimulates(body2, shape2)) {
        return false;
    }

    // compound body(weld)는 두 잎의 부분 층 응답 쌍을 본다(UE 형상 filter data — 7.1.24절).
    // 아직 잎이 아닌 단계(compound 형상 자체)는 다음 단계로 미룬다.
    if (!IsCompoundLayer(body1.GetObjectLayer()) && !IsCompoundLayer(body2.GetObjectLayer())) {
        return true;
    }

    ElementTag tag1;
    ElementTag tag2;
    if (!UnpackElementUserData(shape1->GetUserData(), tag1) ||
        !UnpackElementUserData(shape2->GetUserData(), tag2)) {
        return true;
    }

    return scene_.layerPairs_.ShouldCollide(scene_.PartLayer(body1, tag1.part),
                                            scene_.PartLayer(body2, tag2.part));
}

JPH::ObjectLayer JoltPhysicsScene::PartLayer(const JPH::Body& body, uint32 part) const noexcept {
    const JPH::ObjectLayer layer = body.GetObjectLayer();
    if (!IsCompoundLayer(layer)) {
        return layer;
    }

    const JPH::uint64 userData = body.GetUserData();
    const BodySlot* slot = FindSlot({UserDataIndex(userData), UserDataGeneration(userData)});
    return slot != nullptr && part < slot->parts.Size() ? slot->parts[part].layer : layer;
}

bool JoltPhysicsScene::ElementEffective(const JPH::Body& body, JPH::uint64 shapeUserData,
                                        CollisionEnabled& effective) const noexcept {
    ElementTag tag;
    if (!UnpackElementUserData(shapeUserData, tag)) {
        return false;
    }

    effective =
        CollisionEnabledIntersection(LayerCollisionEnabled(PartLayer(body, tag.part)), tag.enabled);
    return true;
}

bool JoltPhysicsScene::IsProbeElement(const JPH::Body& body,
                                      const JPH::SubShapeID& subShape) const noexcept {
    CollisionEnabled effective = CollisionEnabled::NoCollision;
    return ElementEffective(body, body.GetShape()->GetSubShapeUserData(subShape), effective) &&
           CollisionEnabledHasProbe(effective);
}

bool JoltPhysicsScene::ElementSimulates(const JPH::Body& body,
                                        const JPH::Shape* shape) const noexcept {
    CollisionEnabled effective = CollisionEnabled::NoCollision;
    if (!ElementEffective(body, shape->GetUserData(), effective)) {
        return true;
    }

    return CollisionEnabledHasPhysics(effective) || CollisionEnabledHasProbe(effective);
}

void JoltPhysicsScene::RefreshPartLayers(BodySlot& slot) noexcept {
    bool notify = false;
    bool changed = false;
    bool simulates = false;
    bool mixed = false;
    bool firstEnabled = true;
    CollisionEnabled reference = CollisionEnabled::NoCollision;
    for (usize index = 0; index < slot.parts.Size(); ++index) {
        BodyPart& part = slot.parts[index];
        const uint32 responseSet = index == 0 ? slot.responseSet : part.responseSet;
        const JPH::ObjectLayer layer =
            ObjectLayerFor(part.collisionEnabled, part.objectType, slot.motion, responseSet);
        changed = changed || layer != part.layer;
        part.layer = layer;
        notify = notify || part.notifyContacts;
        simulates = simulates || ObjectLayerMotion(layer) != LayerMotion::QueryOnly;
        // 잎의 실효 충돌 켬(부분 값 ∩ 원소 값)이 섞이면 manifold reduction을 끈다(7.1.21·
        // 7.1.24절 — 합친 manifold의 하위 형상 ID 하나로 부분별 probe가 섞이지 않게).
        for (usize element = part.first; element < part.first + part.count; ++element) {
            const CollisionEnabled effective = CollisionEnabledIntersection(
                part.collisionEnabled, slot.geometry[element].collisionEnabled);
            if (firstEnabled) {
                reference = effective;
                firstEnabled = false;
            }

            mixed = mixed || effective != reference;
        }
    }

    // compound 층: 시뮬레이션하는 부분이 없으면 QueryOnly 묶음이다(쌍 필터·broadphase가 바로
    // 거른다).
    const LayerMotion group = !simulates                          ? LayerMotion::QueryOnly
                              : slot.motion == BodyMotion::Static ? LayerMotion::Static
                                                                  : LayerMotion::Moving;
    const JPH::ObjectLayer bodyLayer =
        slot.parts.Size() > 1 ? MakeCompoundLayer(group) : slot.parts[0].layer;
    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    if (bodies.GetObjectLayer(slot.body) != bodyLayer) {
        bodies.SetObjectLayer(slot.body, bodyLayer);
        changed = true;
    }

    if (bodies.GetUseManifoldReduction(slot.body) == mixed) {
        bodies.SetUseManifoldReduction(slot.body, !mixed);
    }

    // 층이 바뀌면 접촉 cache를 버린다(Jolt는 cache가 맞으면 narrowphase·SimShapeFilter를 건너뛴다 —
    // PhysicsSystem.cpp:1098-1099, compound 쌍은 잎 단계에서만 걸러진다).
    if (changed) {
        bodies.InvalidateContactCache(slot.body);
    }

    if (slot.notifyContacts != notify) {
        notifyBodies_ = notify ? notifyBodies_ + 1 : notifyBodies_ - 1;
        slot.notifyContacts = notify;
    }
}

void JoltPhysicsScene::ContactCollector::OnContactAdded(const JPH::Body& body1,
                                                        const JPH::Body& body2,
                                                        const JPH::ContactManifold& manifold,
                                                        JPH::ContactSettings& settings) {
    Record(body1, body2, manifold, settings);
}

void JoltPhysicsScene::ContactCollector::OnContactPersisted(const JPH::Body& body1,
                                                            const JPH::Body& body2,
                                                            const JPH::ContactManifold& manifold,
                                                            JPH::ContactSettings& settings) {
    Record(body1, body2, manifold, settings);
}

void JoltPhysicsScene::ContactCollector::Record(const JPH::Body& body1, const JPH::Body& body2,
                                                const JPH::ContactManifold& manifold,
                                                JPH::ContactSettings& settings) noexcept {
    // probe는 접촉 데이터만 내고 밀어내지 않는다(Jolt는 콜백마다 설정을 새로 채운다 — 6.4절).
    // 원소의 실효 probe(body 값과 교집합 — 7.1.21절)도 같다.
    const bool probe = IsProbeBody(body1) || IsProbeBody(body2) ||
                       scene_.IsProbeElement(body1, manifold.mSubShapeID1) ||
                       scene_.IsProbeElement(body2, manifold.mSubShapeID2);
    if (probe) {
        settings.mIsSensor = true;
    }

    if (manifold.mRelativeContactPointsOn1.empty() ||
        (!scene_.WantsContacts(body1.GetUserData()) &&
         !scene_.WantsContacts(body2.GetUserData()))) {
        return;
    }

    // probe 접촉은 실제로 겹친 것만 낸다: Jolt manifold는 speculative 거리 안의 떨어진 접촉도
    // 주지만 UE는 probe를 거리 0으로 다시 검출한다(Chaos DetectProbeCollisions).
    if (probe && manifold.mPenetrationDepth < 0.0f) {
        return;
    }

    ContactNotice notice;
    notice.userData1 = body1.GetUserData();
    notice.userData2 = body2.GetUserData();
    notice.subShape1 = manifold.mSubShapeID1;
    notice.subShape2 = manifold.mSubShapeID2;
    notice.part1 = ContactPart(body1, manifold.mSubShapeID1);
    notice.part2 = ContactPart(body2, manifold.mSubShapeID2);
    // 두 표면의 첫 점 가운데다(UE Chaos 접촉 위치 — 두 형상 접촉점의 평균).
    notice.point = manifold.mBaseOffset + 0.5f * (manifold.mRelativeContactPointsOn1[0] +
                                                  manifold.mRelativeContactPointsOn2[0]);
    notice.normal = manifold.mWorldSpaceNormal;
    notice.depth = manifold.mPenetrationDepth;
    notice.probe = probe;
    if (!probe && (UsesLinearCast(body1) || UsesLinearCast(body2))) {
        // CCD로만 닿은 쌍은 solver 충격량이 없어 이 추정값을 쓴다(6.4절 — 알림 접촉마다 속도 반복을
        // 돌지 않게 CCD body가 낀 접촉만 계산한다). 콜백의 속도는 해 풀기 전 값이다
        // (ContactListener.h:104-106).
        const JPH::PhysicsSettings& physics = scene_.system_.GetPhysicsSettings();
        JPH::CollisionEstimationResult estimate;
        JPH::EstimateCollisionResponse(body1, body2, manifold, estimate, settings,
                                       physics.mMinVelocityForRestitution,
                                       physics.mNumVelocitySteps);
        float total = 0.0f;
        for (float impulse : estimate.mContactImpulse) {
            total += impulse;
        }

        notice.estimatedImpulse = total * manifold.mWorldSpaceNormal;
    }

    platform::LockGuard lock(mutex_);
    // 할당하지 않는다(클래스 설명) — 용량이 모자라면 세기만 한다.
    if (notices_.Size() < notices_.Capacity()) {
        (void)notices_.TryPushBack(notice);
    } else {
        ++dropped_;
    }
}

Result<void> JoltPhysicsScene::ContactCollector::Reserve(usize capacity) noexcept {
    platform::LockGuard lock(mutex_);
    return notices_.TryReserve(capacity);
}

uint64 JoltPhysicsScene::ContactCollector::TakeDropped() noexcept {
    platform::LockGuard lock(mutex_);
    const uint64 dropped = dropped_;
    dropped_ = 0;
    return dropped;
}

void JoltPhysicsScene::CollectContactHits() noexcept {
    Array<ContactNotice>& notices = contacts_.Notices();
    const uint64 dropped = contacts_.TakeDropped();
    lastContactNotices_ = notices.Size() + static_cast<usize>(dropped);
    if (dropped != 0) {
        Log::Warning(LogPhysics,
                     "Physics step {Step} lost {Count} contact notices; some contact hits were "
                     "not reported",
                     steps_, dropped);
    }

    if (notices.IsEmpty()) {
        return;
    }

    algo::Sort(notices.View(), [](const ContactNotice& a, const ContactNotice& b) noexcept {
        return ContactNoticeBefore(a, b);
    });
    usize begin = 0;
    while (begin < notices.Size()) {
        usize end = begin + 1;
        while (end < notices.Size() && notices[end].userData1 == notices[begin].userData1 &&
               notices[end].userData2 == notices[begin].userData2) {
            ++end;
        }

        EmitContactPair(Span<const ContactNotice>(notices.Data() + begin, end - begin));
        begin = end;
    }

    notices.Clear();
    if (lostContactHits_ != 0) {
        Log::Warning(LogPhysics,
                     "Physics step {Step} could not store {Count} contact hits (out of memory)",
                     steps_, lostContactHits_);
        lostContactHits_ = 0;
    }
}

void JoltPhysicsScene::RecordStepStartVelocities() noexcept {
    system_.GetActiveBodies(JPH::EBodyType::RigidBody, activeBodies_);
    const JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
    for (const JPH::BodyID& id : activeBodies_) {
        const JPH::uint64 userData = bodies.GetUserData(id);
        const PhysicsBodyHandle handle{UserDataIndex(userData), UserDataGeneration(userData)};
        if (FindSlot(handle) == nullptr || slots_[handle.index].motion != BodyMotion::Dynamic) {
            continue;
        }

        BodySlot& slot = slots_[handle.index];
        slot.stepStartVelocity = bodies.GetLinearVelocity(id);
        slot.stepStartStamp = steps_ + 1;
    }
}

bool JoltPhysicsScene::IsVelocityChangeUnderThreshold(const BodySlot& slot) const noexcept {
    // Dynamic만 본다(UE도 rigid 입자의 V − PreV만 계산한다). step 시작에 잠들어 있었으면 0에서
    // 시작한다.
    if (slot.motion != BodyMotion::Dynamic) {
        return true;
    }

    const JPH::Vec3 before =
        slot.stepStartStamp == steps_ + 1 ? slot.stepStartVelocity : JPH::Vec3::sZero();
    const Vec3d change = FromJoltLinearVelocity(
        system_.GetBodyInterfaceNoLock().GetLinearVelocity(slot.body) - before);
    // UE FVector::IsNearlyZero(threshold)(cm/s, 성분마다)다.
    const double threshold = minDeltaVelocityForHitEvents_;
    return math::Abs(change.x) <= threshold && math::Abs(change.y) <= threshold &&
           math::Abs(change.z) <= threshold;
}

void JoltPhysicsScene::EmitContactPair(Span<const ContactNotice> group) noexcept {
    const ContactNotice& first = group[0];
    const PhysicsBodyHandle handle1{UserDataIndex(first.userData1),
                                    UserDataGeneration(first.userData1)};
    const PhysicsBodyHandle handle2{UserDataIndex(first.userData2),
                                    UserDataGeneration(first.userData2)};
    const BodySlot* slot1 = FindSlot(handle1);
    const BodySlot* slot2 = FindSlot(handle2);
    if (slot1 == nullptr || slot2 == nullptr) {
        return;
    }

    bool solid = false;
    JPH::Vec3 estimated = JPH::Vec3::sZero();
    for (const ContactNotice& notice : group) {
        solid = solid || !notice.probe;
        estimated += notice.estimatedImpulse;
    }

    // body 2가 받은 쌍 충격량이다: solver 값(CCD로만 닿았으면 추정값 — 6.4절). probe 접촉만이면
    // 없다.
    JPH::Vec3 impulse = JPH::Vec3::sZero();
    if (solid) {
        JPH::Vec3 applied = JPH::Vec3::sZero();
        JPH::uint appliedPoints = 0;
        const bool solved =
            system_.GetAppliedContactImpulse(slot1->body, slot2->body, applied, appliedPoints);
        impulse = solved && appliedPoints != 0 ? applied : estimated;
    }

    const bool underThreshold = minDeltaVelocityForHitEvents_ > 0.0 &&
                                IsVelocityChangeUnderThreshold(*slot1) &&
                                IsVelocityChangeUnderThreshold(*slot2);
    // hit는 받는 쪽마다 하나이고, 받는 쪽 부분이 알림 표지를 켠 첫 통지(정렬 순 — 부분 쌍 순)로
    // 정한다(UE GetPendingCollisionForContactPair·GetBodyInstanceFromProxyAndShape — 형상 filter
    // data의 알림 bit, PhysScene_Chaos.cpp:1065·1110-1114, 7.1.22·7.1.24절). sign은 Jolt
    // 법선·충격량(body 2 기준)을 받는 쪽 기준으로 바꾼다(body 1이면 −1).
    auto emit = [&](bool receiverIsFirst) noexcept {
        const BodySlot& meSlot = receiverIsFirst ? *slot1 : *slot2;
        const BodySlot& otherSlot = receiverIsFirst ? *slot2 : *slot1;
        const ContactNotice* chosen = nullptr;
        for (const ContactNotice& notice : group) {
            const uint32 mePart = receiverIsFirst ? notice.part1 : notice.part2;
            if (PartOf(meSlot, mePart).notifyContacts) {
                chosen = &notice;
                break;
            }
        }

        if (chosen == nullptr) {
            return;
        }

        // UE는 누적 충격량이 정확히 0인 접촉(닿지 않은 speculative 접촉)을 내지 않는다. probe는
        // 반작용이 없어 충격량 0이고 속도 변화로 거르지 않는다.
        if (!chosen->probe && (impulse == JPH::Vec3::sZero() || underThreshold)) {
            return;
        }

        const float sign = receiverIsFirst ? -1.0f : 1.0f;
        const BodyPart& me = PartOf(meSlot, receiverIsFirst ? chosen->part1 : chosen->part2);
        const BodyPart& other = PartOf(otherSlot, receiverIsFirst ? chosen->part2 : chosen->part1);
        PhysicsContactHit hit;
        hit.myComponent = me.component;
        hit.myActor = me.actor;
        hit.otherComponent = other.component;
        hit.otherActor = other.actor;
        hit.normalImpulse = chosen->probe ? Vec3d::Zero() : FromJoltLinearImpulse(sign * impulse);
        hit.hit.location = FromJoltPosition(chosen->point, origin_);
        hit.hit.impactPoint = hit.hit.location;
        hit.hit.normal = FromJoltNormal(sign * chosen->normal);
        hit.hit.impactNormal = hit.hit.normal;
        hit.hit.penetrationDepth = math::Max(FromJoltLength(chosen->depth), 0.0);
        hit.hit.blockingHit = true;
        hit.hit.component = other.component;
        hit.hit.actor = other.actor;
        if (!contactHits_.TryPushBack(hit)) {
            ++lostContactHits_;
        }
    };

    // 칸 번호가 작은 쪽 먼저다.
    if (handle1.index < handle2.index) {
        emit(true);
        emit(false);
    } else {
        emit(false);
        emit(true);
    }
}

Span<const PhysicsContactHit> JoltPhysicsScene::TakeContactHits() noexcept {
    AssertNotStepping();
    // 두 버퍼를 바꿔 쥐어 쌓는 쪽이 지난 프레임 용량을 다시 쓴다.
    Array<PhysicsContactHit> previous = Move(takenContactHits_);
    takenContactHits_ = Move(contactHits_);
    contactHits_ = Move(previous);
    contactHits_.Clear();
    return takenContactHits_.View();
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
    // 반발 문턱 = 가속도 문턱 × step dt(UE Chaos — PBDCollisionContainerSolver.cpp:89, 7.1.15절).
    // dt가 바뀔 때만 다시 넣는다.
    if (seconds != restitutionStepSeconds_) {
        restitutionStepSeconds_ = seconds;
        JPH::PhysicsSettings joltSettings = system_.GetPhysicsSettings();
        joltSettings.mMinVelocityForRestitution = static_cast<float>(
            math::CentimetersToMeters(restitutionThreshold_ * static_cast<double>(seconds)));
        system_.SetPhysicsSettings(joltSettings);
    }

    // listener는 step 중 할당하지 않으므로 지난 step 수에 맞춰 미리 늘린다(실패하면 넘친 몫을
    // 센다).
    (void)contacts_.Reserve(LargestOf(MinContactNoticeCapacity,
                                      ContactNoticeGrowth * lastContactNotices_,
                                      ContactNoticesPerNotifyBody * notifyBodies_));
    if (minDeltaVelocityForHitEvents_ > 0.0) {
        RecordStepStartVelocities();
    }

    // step 전 게임 thread 전이(힘의 깨움 등)를 먼저 칸에 옮긴다(마지막 전이 순서 — 7.1.19절).
    CollectWakeTransitions();
    stepping_ = true;
    if (platform::RunWithDeterministicFloatingPoint(
            [](void* context) noexcept {
                auto& step = *static_cast<StepCall*>(context);
                // 대기 충격량·MoveKinematic의 속도 계산도 같은 FP 환경에서 한다.
                step.scene->ApplyPendingImpulses();
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
    CollectWakeTransitions();
    CollectContactHits();
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

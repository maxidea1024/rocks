// Jolt Physics(Engine/Source/ThirdParty/Jolt)의 연결 시험이다(Docs/Design/PhysicsJoltAdapter.ko.md
// 10.5절, JL1). 실행 파일이 라이브러리를 직접 링크하고 Jolt 헤더를 경고 0으로 include한다.
// 좌표 사상과 단위 변환은 Physics 모듈 adapter의 몫이라 여기서는 Jolt 좌표(오른손 Y-up, m)를
// 그대로 쓴다.
#include <Base/NumericLimits.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정과 STL include).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/Collision/Shape/TriangleShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
// clang-format on
#include <string.h>

using namespace rocks;
namespace third_party_jolt_tests_file {

namespace {

// Jolt 기본 Allocate는 malloc이다. 64-bit 대상의 malloc 정렬과 같게 둔다.
constexpr usize DefaultAllocationAlignment = 16;

// 임시 할당기 블록 크기(Jolt 예제 HelloWorld와 같다).
constexpr usize TempAllocatorBytes = 10u * 1024u * 1024u;

constexpr JPH::uint MaxBodies = 16;
constexpr JPH::uint BodyMutexCount = 0; // 0 = Jolt 기본값
constexpr JPH::uint MaxBodyPairs = 64;
constexpr JPH::uint MaxContactConstraints = 64;
constexpr JPH::uint JobCapacity = 256;

constexpr JPH::ObjectLayer StaticLayer = 0;
constexpr JPH::ObjectLayer MovingLayer = 1;
constexpr JPH::uint ObjectLayerCount = 2;
constexpr JPH::uint BroadPhaseLayerCount = 2;

constexpr float StepSeconds = 1.0f / 60.0f;
constexpr int FreeFallSteps = 30;
constexpr int SettleSteps = 240;
constexpr int CollisionSteps = 1;

// 어댑터가 넣을 중력(Rocks 기본 −980 cm/s²를 m/s²로 바꾼 값, 어댑터 경계 3절).
constexpr float GravityMetersPerSecondSquared = 9.8f;

constexpr float SphereRadius = 0.5f;
constexpr float SphereStartHeight = 10.0f;
constexpr float FloorHalfExtent = 10.0f;
constexpr float FloorHalfThickness = 0.5f;

// 반암시 Euler 적분과 float 누적 오차를 덮는 허용 오차(m, m/s).
constexpr float FreeFallTolerance = 1.0e-3f;

// 쌓여 정지한 구의 높이 허용 오차: Jolt mPenetrationSlop 0.02 m + 여유.
constexpr float RestHeightTolerance = 0.03f;
constexpr float RestSpeedTolerance = 0.05f;

constexpr float FractionTolerance = 1.0e-4f;
constexpr float NormalTolerance = 1.0e-3f;

// 접촉 충격량 시험(물리 모듈 6.4절, PMU2). 스텝마다 충격량 = m·(v후 − v전 + g·dt)(수직 성분,
// 감쇠 0·반발 0·마찰 방향 운동 없음). 허용 오차는 질량 약 524–1000 kg·속도 float 오차를
// 덮는다(N·s).
constexpr int ImpulseSteps = FreeFallSteps * 3 + SettleSteps;
constexpr float ImpulseTolerance = 0.1f;
// 수평 성분 허용 오차는 수직 크기에 비례한다(캐시 법선을 body 회전으로 되돌린 float 오차 —
// 상자 면 충돌 13100 N·s에서 0.3 N·s 정도).
constexpr float ImpulseDirectionTolerance = 1.0e-4f;
// 선속도 상한 시험(m/s). Jolt BodyCreationSettings::mMaxLinearVelocity 기본값과 그보다 빠른 속도.
constexpr float JoltDefaultMaxLinearVelocity = 500.0f;
constexpr float FastBodySpeed = 10000.0f;
constexpr float MetersPerCentimeter = 0.01f;
constexpr float ImpactSpeedFloor = 1.0f; // m/s
constexpr float BoxHalfExtent = 0.5f;
constexpr JPH::uint SphereContactPoints = 1;
constexpr JPH::uint BoxFaceContactPoints = 4;
constexpr int TwoCollisionSteps = 2;

constexpr float CylinderHalfHeight = 0.5f;
constexpr float CylinderRadius = 0.25f;
constexpr float QueryStartHeight = 5.0f;
constexpr float QueryLength = 10.0f;
constexpr float QueryOffsetX = 5.0f;

// 할당 hook 계수. 시험은 단일 thread job 시스템만 쓴다.
struct AllocationCounters {
    usize live = 0;
    usize total = 0;
};

AllocationCounters g_counters;
int g_traceCalls = 0;
int g_assertFailures = 0;

SystemAllocator& HookAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

void* CountingAllocateAligned(size_t size, size_t alignment) {
    Result<void*> block = HookAllocator().Allocate(size, alignment);
    if (!block) {
        return nullptr;
    }

    ++g_counters.live;
    ++g_counters.total;
    return block.Value();
}

void CountingFree(void* block) {
    if (block == nullptr) {
        return;
    }

    (void)HookAllocator().Deallocate(block);
    --g_counters.live;
}

void* CountingAllocate(size_t size) {
    return CountingAllocateAligned(size, DefaultAllocationAlignment);
}

void* CountingReallocate(void* block, size_t oldSize, size_t newSize) {
    void* resized = CountingAllocate(newSize);
    if (resized != nullptr && block != nullptr) {
        memcpy(resized, block, oldSize < newSize ? oldSize : newSize);
        CountingFree(block);
    }

    return resized;
}

void CountingTrace(const char* /*format*/, ...) {
    ++g_traceCalls;
}

bool CountingAssertFailed(const char* /*expression*/, const char* /*message*/, const char* /*file*/,
                          JPH::uint /*line*/) {
    ++g_assertFailures;
    return false;
}

void InstallHooks() noexcept {
    JPH::Allocate = CountingAllocate;
    JPH::Reallocate = CountingReallocate;
    JPH::Free = CountingFree;
    JPH::AlignedAllocate = CountingAllocateAligned;
    JPH::AlignedFree = CountingFree;
    JPH::Trace = CountingTrace;
    JPH::AssertFailed = CountingAssertFailed;
}

bool Near(float value, float expected, float tolerance) noexcept {
    const float difference = value - expected;
    return difference <= tolerance && -difference <= tolerance;
}

// 레이어 표. ObjectVsBroadPhaseLayerFilterTable은 생성할 때 이 표를 읽어 자기 표를 만들므로
// 필터보다 먼저 채워져 있어야 한다.
struct Layers {
    JPH::BroadPhaseLayerInterfaceTable broadPhaseLayers{ObjectLayerCount, BroadPhaseLayerCount};
    JPH::ObjectLayerPairFilterTable layerPairs{ObjectLayerCount};

    Layers() {
        broadPhaseLayers.MapObjectToBroadPhaseLayer(StaticLayer, JPH::BroadPhaseLayer(0));
        broadPhaseLayers.MapObjectToBroadPhaseLayer(MovingLayer, JPH::BroadPhaseLayer(1));
        layerPairs.EnableCollision(StaticLayer, MovingLayer);
        layerPairs.EnableCollision(MovingLayer, MovingLayer);
    }
};

// 정적 바닥 하나와 동적 구 하나의 세계. 바닥 윗면은 y = 0이다.
struct Scene {
    Layers layers;
    JPH::ObjectVsBroadPhaseLayerFilterTable objectVsBroadPhase{
        layers.broadPhaseLayers, BroadPhaseLayerCount, layers.layerPairs, ObjectLayerCount};
    JPH::PhysicsSystem system;
    JPH::BodyID floor;
    JPH::BodyID sphere;

    // 잠들면 접촉이 사라지므로 충격량 시험은 잠듦을 끈다. box면 구 대신 정육면체를 떨어뜨린다.
    explicit Scene(bool allowSleeping = true, bool box = false) {
        system.Init(MaxBodies, BodyMutexCount, MaxBodyPairs, MaxContactConstraints,
                    layers.broadPhaseLayers, objectVsBroadPhase, layers.layerPairs);
        system.SetGravity(JPH::Vec3(0.0f, -GravityMetersPerSecondSquared, 0.0f));

        JPH::BodyInterface& bodies = system.GetBodyInterface();
        JPH::BodyCreationSettings floorSettings(
            new JPH::BoxShape(JPH::Vec3(FloorHalfExtent, FloorHalfThickness, FloorHalfExtent)),
            JPH::RVec3(0.0f, -FloorHalfThickness, 0.0f), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, StaticLayer);
        floor = bodies.CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);

        JPH::RefConst<JPH::Shape> fallingShape;
        if (box) {
            fallingShape = new JPH::BoxShape(JPH::Vec3::sReplicate(BoxHalfExtent));
        } else {
            fallingShape = new JPH::SphereShape(SphereRadius);
        }

        JPH::BodyCreationSettings sphereSettings(
            fallingShape, JPH::RVec3(0.0f, SphereStartHeight, 0.0f), JPH::Quat::sIdentity(),
            JPH::EMotionType::Dynamic, MovingLayer);
        // 감쇠를 끄면 자유 낙하가 반암시 Euler 식과 같아진다.
        sphereSettings.mLinearDamping = 0.0f;
        sphereSettings.mAngularDamping = 0.0f;
        sphereSettings.mAllowSleeping = allowSleeping;
        sphere = bodies.CreateAndAddBody(sphereSettings, JPH::EActivation::Activate);
        system.OptimizeBroadPhase();
    }

    void Step(int count, JPH::TempAllocator& temp, JPH::JobSystem& jobs,
              int collisionSteps = CollisionSteps) {
        for (int index = 0; index < count; ++index) {
            const JPH::EPhysicsUpdateError error =
                system.Update(StepSeconds, collisionSteps, &temp, &jobs);
            CHECK(error == JPH::EPhysicsUpdateError::None);
        }
    }

    ~Scene() {
        JPH::BodyInterface& bodies = system.GetBodyInterface();
        bodies.RemoveBody(sphere);
        bodies.DestroyBody(sphere);
        bodies.RemoveBody(floor);
        bodies.DestroyBody(floor);
    }
};

// 낙하·정지·질의를 확인하고 마지막 상태를 recorder에 남긴다.
void RunScene(JPH::TempAllocator& temp, JPH::JobSystem& jobs, JPH::StateRecorderImpl& state) {
    Scene scene;
    JPH::BodyInterface& bodies = scene.system.GetBodyInterface();

    // 반암시 Euler: v_n = −g·n·dt, y_n = y_0 − g·dt²·n(n + 1)/2.
    scene.Step(FreeFallSteps, temp, jobs);
    const float steps = static_cast<float>(FreeFallSteps);
    const float expectedSpeed = -GravityMetersPerSecondSquared * steps * StepSeconds;
    const float expectedHeight = SphereStartHeight - GravityMetersPerSecondSquared * StepSeconds *
                                                         StepSeconds * steps * (steps + 1.0f) *
                                                         0.5f;
    const JPH::Vec3 fallingVelocity = bodies.GetLinearVelocity(scene.sphere);
    const JPH::RVec3 fallingPosition = bodies.GetCenterOfMassPosition(scene.sphere);
    CHECK(Near(fallingVelocity.GetY(), expectedSpeed, FreeFallTolerance));
    CHECK(Near(static_cast<float>(fallingPosition.GetY()), expectedHeight, FreeFallTolerance));
    CHECK(Near(static_cast<float>(fallingPosition.GetX()), 0.0f, FreeFallTolerance));

    // 바닥 위에 멈춘다(중심 높이 = 반지름).
    scene.Step(SettleSteps, temp, jobs);
    const JPH::RVec3 restPosition = bodies.GetCenterOfMassPosition(scene.sphere);
    CHECK(Near(static_cast<float>(restPosition.GetY()), SphereRadius, RestHeightTolerance));
    CHECK(bodies.GetLinearVelocity(scene.sphere).Length() < RestSpeedTolerance);

    // ray: 방향 벡터 길이가 최대 거리, fraction은 그 길이의 비율이다(J13).
    const JPH::RRayCast ray{JPH::RVec3(QueryOffsetX, QueryStartHeight, 0.0f),
                            JPH::Vec3(0.0f, -QueryLength, 0.0f)};
    JPH::RayCastResult rayHit;
    CHECK(scene.system.GetNarrowPhaseQuery().CastRay(ray, rayHit));
    CHECK(rayHit.mBodyID == scene.floor);
    CHECK(Near(rayHit.mFraction, QueryStartHeight / QueryLength, FractionTolerance));

    // cylinder(convex radius 0)를 질의 형상으로 쓴 shape cast(J16). 아랫면이 바닥에 닿는 비율과
    // ImpactNormal = −mPenetrationAxis.Normalized()가 바닥 바깥(+Y)인지(J11) 본다.
    JPH::RefConst<JPH::Shape> cylinder =
        new JPH::CylinderShape(CylinderHalfHeight, CylinderRadius, 0.0f);
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        cylinder, JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(JPH::RVec3(-QueryOffsetX, QueryStartHeight, 0.0f)),
        JPH::Vec3(0.0f, -QueryLength, 0.0f));
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> castHits;
    scene.system.GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings(),
                                                 JPH::RVec3::sZero(), castHits);
    CHECK(castHits.HadHit());
    if (castHits.HadHit()) {
        CHECK(castHits.mHit.mBodyID2 == scene.floor);
        CHECK(Near(castHits.mHit.mFraction, (QueryStartHeight - CylinderHalfHeight) / QueryLength,
                   FractionTolerance));
        const JPH::Vec3 impactNormal = -castHits.mHit.mPenetrationAxis.Normalized();
        CHECK(Near(impactNormal.GetY(), 1.0f, NormalTolerance));
    }

    scene.system.SaveState(state);
}

} // namespace

TEST(ThirdParty_JoltSimulatesQueriesAndReleasesHookedMemory) {
    g_counters = {};
    g_traceCalls = 0;
    g_assertFailures = 0;
    InstallHooks();
    // RegisterTypes는 정의가 다르면 진단 없이 abort하므로 먼저 확인한다(어댑터 경계 10.4절).
    REQUIRE(JPH::VerifyJoltVersionID());
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    {
        JPH::TempAllocatorImpl temp(TempAllocatorBytes);
        JPH::JobSystemSingleThreaded jobs(JobCapacity);
        JPH::StateRecorderImpl first;
        JPH::StateRecorderImpl second;
        RunScene(temp, jobs, first);
        RunScene(temp, jobs, second);
        // 같은 binary·같은 호출 순서면 상태 byte가 같다(어댑터 경계 J10, PC03의 최소형).
        CHECK(first.GetDataSize() != 0);
        CHECK(first.IsEqual(second));
    }

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    // hook으로 받은 할당의 잔여만 본다. StateRecorderImpl(std::stringstream)과 JobFunction
    // (std::function)은 전역 operator new를 쓰므로 모든 할당이 hook으로 오지는 않는다.
    CHECK(g_counters.total != 0);
    CHECK(g_counters.live == 0);
    CHECK(g_traceCalls == 0);
    CHECK(g_assertFailures == 0);
    JPH::RegisterDefaultAllocator();
}

namespace {

// 동적 body의 질량(kg). body가 없으면 0이다.
float BodyMass(const JPH::PhysicsSystem& system, JPH::BodyID body) {
    JPH::BodyLockRead lock(system.GetBodyLockInterface(), body);
    CHECK(lock.Succeeded());
    if (!lock.Succeeded()) {
        return 0.0f;
    }

    return 1.0f / lock.GetBody().GetMotionProperties()->GetInverseMass();
}

// 낙하 body 바닥면이 처음 y = 0 아래로 들어가는 스텝(반암시 Euler 식, 1부터 센다).
int FirstPenetratingStep(float halfHeight) {
    for (int step = 1;; ++step) {
        const float n = static_cast<float>(step);
        const float height = SphereStartHeight - GravityMetersPerSecondSquared * StepSeconds *
                                                     StepSeconds * n * (n + 1.0f) * 0.5f;
        if (height - halfHeight <= 0.0f) {
            return step;
        }
    }
}

// 낙하·충돌·정지의 모든 스텝에서 돌려받은 충격량이 운동량 변화와 같은지 본다.
void CheckAppliedImpulseFollowsMomentum(bool box, JPH::TempAllocator& temp, JPH::JobSystem& jobs) {
    Scene scene(false, box);
    JPH::BodyInterface& bodies = scene.system.GetBodyInterface();
    const float mass = BodyMass(scene.system, scene.sphere);
    const JPH::uint expectedPoints = box ? BoxFaceContactPoints : SphereContactPoints;

    int firstContact = 0;
    int contactSteps = 0;
    int mismatches = 0;
    JPH::Vec3 lastImpulse = JPH::Vec3::sZero();
    float largestImpulse = 0.0f;
    for (int step = 1; step <= ImpulseSteps; ++step) {
        const float before = bodies.GetLinearVelocity(scene.sphere).GetY();
        scene.Step(1, temp, jobs);
        const float after = bodies.GetLinearVelocity(scene.sphere).GetY();
        const float expected =
            mass * (after - before + GravityMetersPerSecondSquared * StepSeconds);

        JPH::Vec3 impulse = JPH::Vec3::sReplicate(-1.0f);
        JPH::uint points = 0;
        const bool touching =
            scene.system.GetAppliedContactImpulse(scene.floor, scene.sphere, impulse, points);
        // 반대 순서로 물으면 바닥이 받은 충격량이다(작용·반작용).
        JPH::Vec3 onFloor = JPH::Vec3::sReplicate(-1.0f);
        JPH::uint floorPoints = 0;
        CHECK(scene.system.GetAppliedContactImpulse(scene.sphere, scene.floor, onFloor,
                                                    floorPoints) == touching);
        CHECK(onFloor == -impulse);
        CHECK(floorPoints == points);
        if (touching) {
            firstContact = firstContact == 0 ? step : firstContact;
            ++contactSteps;
            CHECK(points == expectedPoints);
        } else {
            CHECK(impulse == JPH::Vec3::sZero());
            CHECK(points == 0u);
        }

        // 수평 성분은 없고 수직 성분은 운동량 변화와 같다.
        const float horizontalTolerance =
            ImpulseTolerance + ImpulseDirectionTolerance * (expected < 0.0f ? -expected : expected);
        if (!Near(impulse.GetY(), expected, ImpulseTolerance) ||
            !Near(impulse.GetX(), 0.0f, horizontalTolerance) ||
            !Near(impulse.GetZ(), 0.0f, horizontalTolerance)) {
            ++mismatches;
        }

        lastImpulse = impulse;
        largestImpulse = impulse.GetY() > largestImpulse ? impulse.GetY() : largestImpulse;
    }

    // 첫 접촉은 실제로 파고드는 스텝이거나 그 한 스텝 전(speculative contact)이고, 반발 0·잠듦
    // 없음이라 그 뒤로 떨어지지 않는다.
    const int penetrating = FirstPenetratingStep(box ? BoxHalfExtent : SphereRadius);
    CHECK(firstContact >= penetrating - 1);
    CHECK(firstContact <= penetrating);
    CHECK(contactSteps == ImpulseSteps - firstContact + 1);
    CHECK(mismatches == 0);
    // 충돌 스텝이 들어 있다: 10 m 낙하(약 14 m/s)를 몇 스텝 안에 멈춘다.
    CHECK(largestImpulse > mass * ImpactSpeedFloor);
    // 정지: 중력 한 스텝분을 받친다.
    CHECK(Near(lastImpulse.GetY(), mass * GravityMetersPerSecondSquared * StepSeconds,
               ImpulseTolerance));
}

} // namespace

// Rocks 수정 GetAppliedContactImpulse(Jolt manifest modifications)가 solver가 실제로 쓴 충격량을
// 돌려준다(물리 모듈 6.4절, PMU2, 어댑터 경계 10.6절). 구(접촉점 1)와 상자(면 접촉 4점)에서
// 모든 스텝이 운동량 변화와 같고, collision step이 둘이면 마지막 collision step 값이다.
TEST(ThirdParty_JoltReportsAppliedContactImpulse) {
    InstallHooks();
    REQUIRE(JPH::VerifyJoltVersionID());
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    {
        JPH::TempAllocatorImpl temp(TempAllocatorBytes);
        JPH::JobSystemSingleThreaded jobs(JobCapacity);
        CheckAppliedImpulseFollowsMomentum(false, temp, jobs);
        CheckAppliedImpulseFollowsMomentum(true, temp, jobs);

        // collision step 2: 정지한 뒤 마지막 collision step(dt/2)의 충격량만 남는다.
        Scene scene(false);
        scene.Step(ImpulseSteps, temp, jobs, TwoCollisionSteps);
        JPH::Vec3 impulse = JPH::Vec3::sZero();
        JPH::uint points = 0;
        CHECK(scene.system.GetAppliedContactImpulse(scene.floor, scene.sphere, impulse, points));
        const float mass = BodyMass(scene.system, scene.sphere);
        const float halfStep = StepSeconds / static_cast<float>(TwoCollisionSteps);
        CHECK(Near(impulse.GetY(), mass * GravityMetersPerSecondSquared * halfStep,
                   ImpulseTolerance));
    }

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    JPH::RegisterDefaultAllocator();
}

// 선속도 상한(물리 모듈 PMD18): Jolt 기본 500 m/s는 속도를 줄이고, UE처럼 √FLT_MAX cm/s(m/s로
// 0.01배)를 넣으면 빠른 body가 그대로 날아간다. 상한 제곱이 float 안에 있어 clamp 계산이 넘치지
// 않고 assert도 없다.
TEST(ThirdParty_JoltUnlimitedLinearVelocityKeepsFastBodies) {
    g_assertFailures = 0;
    InstallHooks();
    REQUIRE(JPH::VerifyJoltVersionID());
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    {
        JPH::TempAllocatorImpl temp(TempAllocatorBytes);
        JPH::JobSystemSingleThreaded jobs(JobCapacity);
        Scene scene;
        JPH::BodyInterface& bodies = scene.system.GetBodyInterface();
        scene.system.SetGravity(JPH::Vec3::sZero());
        const JPH::Vec3 fast(FastBodySpeed, 0.0f, 0.0f);

        bodies.SetLinearVelocity(scene.sphere, fast);
        CHECK(Near(bodies.GetLinearVelocity(scene.sphere).GetX(), JoltDefaultMaxLinearVelocity,
                   FreeFallTolerance));

        const float unlimited = math::Sqrt(NumericLimits<float>::Max()) * MetersPerCentimeter;
        bodies.SetMaxLinearVelocity(scene.sphere, unlimited);
        bodies.SetLinearVelocity(scene.sphere, fast);
        scene.Step(1, temp, jobs);
        CHECK(bodies.GetLinearVelocity(scene.sphere) == fast);
    }

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    CHECK(g_assertFailures == 0);
    JPH::RegisterDefaultAllocator();
}

// 삼각형 앞면은 (V2 − V1) × (V3 − V1) 쪽이다(어댑터 경계 J5). 뒷면을 무시하면 법선과 같은 방향의
// ray는 맞지 않는다.
TEST(ThirdParty_JoltTriangleFrontFaceFollowsRightHandCross) {
    g_counters = {};
    InstallHooks();
    // Jolt 객체는 hook을 되돌리기 전에 모두 해제한다(hook으로 받은 블록을 기본 free가 받지 않게).
    {
        // (1, 0, 0) × (0, 0, −1) = (0, 1, 0): 앞면이 +Y를 본다.
        JPH::RefConst<JPH::Shape> triangle = new JPH::TriangleShape(
            JPH::Vec3(0.0f, 0.0f, 0.0f), JPH::Vec3(1.0f, 0.0f, 0.0f), JPH::Vec3(0.0f, 0.0f, -1.0f));
        JPH::RayCastSettings settings;
        settings.SetBackFaceMode(JPH::EBackFaceMode::IgnoreBackFaces);
        const JPH::Vec3 inside(0.25f, 0.0f, -0.25f);
        const JPH::Vec3 up(0.0f, 1.0f, 0.0f);

        JPH::AllHitCollisionCollector<JPH::CastRayCollector> fromAbove;
        triangle->CastRay(JPH::RayCast{inside + up, -2.0f * up}, settings, JPH::SubShapeIDCreator(),
                          fromAbove);
        CHECK(fromAbove.mHits.size() == 1u);

        JPH::AllHitCollisionCollector<JPH::CastRayCollector> fromBelow;
        triangle->CastRay(JPH::RayCast{inside - up, 2.0f * up}, settings, JPH::SubShapeIDCreator(),
                          fromBelow);
        CHECK(fromBelow.mHits.empty());
    }

    CHECK(g_counters.total != 0);
    CHECK(g_counters.live == 0);
    JPH::RegisterDefaultAllocator();
}

} // namespace third_party_jolt_tests_file

// Jolt Physics(Engine/Source/ThirdParty/Jolt)의 연결 시험이다(Docs/Design/PhysicsJoltAdapter.ko.md
// 10.5절, JL1). 실행 파일이 라이브러리를 직접 링크하고 Jolt 헤더를 경고 0으로 include한다.
// 좌표 사상과 단위 변환은 Physics 모듈 adapter의 몫이라 여기서는 Jolt 좌표(오른손 Y-up, m)를
// 그대로 쓴다.
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

    Scene() {
        system.Init(MaxBodies, BodyMutexCount, MaxBodyPairs, MaxContactConstraints,
                    layers.broadPhaseLayers, objectVsBroadPhase, layers.layerPairs);
        system.SetGravity(JPH::Vec3(0.0f, -GravityMetersPerSecondSquared, 0.0f));

        JPH::BodyInterface& bodies = system.GetBodyInterface();
        JPH::BodyCreationSettings floorSettings(
            new JPH::BoxShape(JPH::Vec3(FloorHalfExtent, FloorHalfThickness, FloorHalfExtent)),
            JPH::RVec3(0.0f, -FloorHalfThickness, 0.0f), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, StaticLayer);
        floor = bodies.CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);

        JPH::BodyCreationSettings sphereSettings(
            new JPH::SphereShape(SphereRadius), JPH::RVec3(0.0f, SphereStartHeight, 0.0f),
            JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, MovingLayer);
        // 감쇠를 끄면 자유 낙하가 반암시 Euler 식과 같아진다.
        sphereSettings.mLinearDamping = 0.0f;
        sphereSettings.mAngularDamping = 0.0f;
        sphere = bodies.CreateAndAddBody(sphereSettings, JPH::EActivation::Activate);
        system.OptimizeBroadPhase();
    }

    void Step(int count, JPH::TempAllocator& temp, JPH::JobSystem& jobs) {
        for (int index = 0; index < count; ++index) {
            const JPH::EPhysicsUpdateError error =
                system.Update(StepSeconds, CollisionSteps, &temp, &jobs);
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

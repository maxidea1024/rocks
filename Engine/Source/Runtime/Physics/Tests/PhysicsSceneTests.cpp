// Physics 모듈 PH1 장면 시험이다(Docs/Design/PhysicsModule 7.1절 완료 기준, 7.1.1절): 장면
// 생성·파괴·빈 step, Jolt 공유 자원 참조 수와 hook 할당 잔여, job 제출·포화 시 미룬 job을
// barrier 대기 thread가 실행·종료 중 job, scheduler 유무와 상관없이 같은 결과(FP 상태가 다르면
// broadphase가 어긋난다 — 6.7절). Jolt를 직접 쓰므로 Monolithic 시험(EngineTests)에서만
// 돈다(BuildTargets 3.8절 BD29 시험 실행 파일 예외).
#include "../Private/JoltLayers.h"
#include "../Private/JoltPhysicsScene.h"
#include "../Private/JoltRuntime.h"

#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Physics/PhysicsSettings.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/Test.h>
#include <World/Physics/PhysicsScene.h>
#include <math.h>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/StateRecorderImpl.h>
// clang-format on

using namespace rocks;
namespace physics_scene_tests_file {

namespace {

constexpr double StepSeconds = 1.0 / 60.0;
constexpr int EmptySteps = 8;
constexpr int StackSteps = 120;

// 시험 장면 용량(작게 — 기본값 65536은 시험마다 큰 표를 잡는다).
constexpr uint32 TestMaxBodies = 256;
constexpr uint32 TestMaxBodyPairs = 1024;
constexpr uint32 TestMaxContactConstraints = 1024;
constexpr uint32 TestTempAllocatorBytes = 4u * 1024u * 1024u;

constexpr uint32 StackSpheres = 24;
constexpr float SphereRadius = 0.5f;
constexpr float SphereSpacing = 1.25f;
constexpr float FloorHalfExtent = 20.0f;
constexpr float FloorHalfThickness = 0.5f;

constexpr uint32 SchedulerWorkers = 2;
constexpr uint32 SchedulerInFlight = 256;
// 미룬 job 경로 시험의 한도다. 유일한 칸을 이벤트로 먼저 쥐어 모든 제출이 CapacityExceeded가 되게
// 한다(타이밍과 무관하게 미룬다).
constexpr uint32 SaturatedInFlight = 1;

PhysicsSettings TestSettings() noexcept {
    PhysicsSettings settings;
    settings.maxBodies = TestMaxBodies;
    settings.maxBodyPairs = TestMaxBodyPairs;
    settings.maxContactConstraints = TestMaxContactConstraints;
    settings.tempAllocatorBytes = TestTempAllocatorBytes;
    return settings;
}

const JPH::ObjectLayer StaticLayer = physics::MakeObjectLayer(
    0, physics::LayerMotion::Static,
    physics::LayerParticipationPhysics | physics::LayerParticipationQuery, 0);
const JPH::ObjectLayer MovingLayer = physics::MakeObjectLayer(
    1, physics::LayerMotion::Moving,
    physics::LayerParticipationPhysics | physics::LayerParticipationQuery, 0);

// 바닥 위로 구를 겹쳐 떨어뜨리는 장면을 채운다(job이 여럿 생기도록 body를 넉넉히).
void FillStack(physics::JoltPhysicsScene& scene) {
    JPH::BodyInterface& bodies = scene.System().GetBodyInterface();
    JPH::BodyCreationSettings floor(
        new JPH::BoxShape(JPH::Vec3(FloorHalfExtent, FloorHalfThickness, FloorHalfExtent)),
        JPH::RVec3(0.0f, -FloorHalfThickness, 0.0f), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, StaticLayer);
    CHECK(!bodies.CreateAndAddBody(floor, JPH::EActivation::DontActivate).IsInvalid());
    for (uint32 index = 0; index < StackSpheres; ++index) {
        const float x = static_cast<float>(index % 4) * SphereSpacing;
        const float y = 1.0f + static_cast<float>(index / 4) * SphereSpacing;
        JPH::BodyCreationSettings sphere(new JPH::SphereShape(SphereRadius),
                                         JPH::RVec3(x, y, 0.1f * static_cast<float>(index % 3)),
                                         JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic,
                                         MovingLayer);
        CHECK(!bodies.CreateAndAddBody(sphere, JPH::EActivation::Activate).IsInvalid());
    }

    scene.System().OptimizeBroadPhase();
}

// RunStack이 돌려주는 진단 수치다.
struct StackCounters {
    uint64 deferredJobs = 0;
    uint64 sceneFloatingPointCorrections = 0;
};

// 장면을 만들어 쌓기 장면을 돌린 뒤 상태 byte를 남긴다.
void RunStack(WorkerScheduler* scheduler, JPH::StateRecorderImpl& state, StackCounters* counters) {
    SystemAllocator allocator;
    auto created = physics::JoltPhysicsScene::Create(allocator, TestSettings(), scheduler);
    REQUIRE(created);
    physics::JoltPhysicsScene& scene = *created.Value();
    FillStack(scene);
    for (int step = 0; step < StackSteps; ++step) {
        CHECK(scene.Step(static_cast<float>(StepSeconds)));
    }

    CHECK(scene.Stats().bodies == StackSpheres + 1);
    scene.System().SaveState(state);
    scene.Jobs().WaitForOutstandingTasks();
    CHECK(scene.Jobs().OutstandingTasks() == 0u);
    if (counters != nullptr) {
        counters->deferredJobs = scene.Jobs().DeferredJobs();
        counters->sceneFloatingPointCorrections = scene.FloatingPointCorrections();
    }

    // 남은 body는 장면 파괴(Jolt PhysicsSystem 소멸자)가 지운다 — 호출 쪽이 할당 잔여 0을 본다.
}

} // namespace

// 장면을 만들고 빈 step을 돈 뒤 파괴하면 Jolt 공유 자원 참조가 0으로 돌아가고 hook 할당 잔여가
// 만들기 전과 같다(7.1절 완료 기준 "할당 잔여 0").
TEST(Physics_SceneCreatesStepsEmptyAndReleasesJolt) {
    const uint64 baseline = physics::JoltLiveAllocations();
    REQUIRE(physics::JoltRuntimeReferenceCount() == 0u);
    {
        SystemAllocator allocator;
        auto created = physics::JoltPhysicsScene::Create(allocator, TestSettings(), nullptr);
        REQUIRE(created);
        CHECK(physics::JoltRuntimeReferenceCount() == 1u);
        CHECK(physics::JoltLiveAllocations() > baseline);
        physics::JoltPhysicsScene& scene = *created.Value();
        for (int step = 0; step < EmptySteps; ++step) {
            CHECK(scene.Step(static_cast<float>(StepSeconds)));
        }

        const PhysicsSceneStats stats = scene.Stats();
        CHECK(stats.bodies == 0u);
        CHECK(stats.activeBodies == 0u);
        CHECK(stats.steps == static_cast<uint64>(EmptySteps));
    }

    CHECK(physics::JoltRuntimeReferenceCount() == 0u);
    CHECK(physics::JoltLiveAllocations() == baseline);
}

// 장면 둘이 공유 자원 하나를 함께 쥔다. 하나를 지워도 다른 장면은 그대로 돈다.
TEST(Physics_ScenesShareJoltRuntimeByReference) {
    const uint64 baseline = physics::JoltLiveAllocations();
    {
        SystemAllocator allocator;
        auto first = physics::JoltPhysicsScene::Create(allocator, TestSettings(), nullptr);
        auto second = physics::JoltPhysicsScene::Create(allocator, TestSettings(), nullptr);
        REQUIRE(first);
        REQUIRE(second);
        CHECK(physics::JoltRuntimeReferenceCount() == 2u);
        first.Value().Reset();
        CHECK(physics::JoltRuntimeReferenceCount() == 1u);
        FillStack(*second.Value());
        CHECK(second.Value()->Step(static_cast<float>(StepSeconds)));
        CHECK(second.Value()->Stats().activeBodies == StackSpheres);
    }

    CHECK(physics::JoltRuntimeReferenceCount() == 0u);
    CHECK(physics::JoltLiveAllocations() == baseline);
}

// 설정 검증과 step 인자 검사다.
TEST(Physics_SceneRejectsInvalidSettingsAndSteps) {
    SystemAllocator allocator;
    PhysicsSettings zeroBodies = TestSettings();
    zeroBodies.maxBodies = 0;
    CHECK(physics::JoltPhysicsScene::Create(allocator, zeroBodies, nullptr).Error() ==
          ErrorCode::InvalidArgument);
    PhysicsSettings badGravity = TestSettings();
    badGravity.defaultGravityZ = NAN;
    CHECK(PhysicsSettings::Validate(badGravity).Error() == ErrorCode::InvalidArgument);
    CHECK(PhysicsSettings::Validate(PhysicsSettings{}));
    CHECK(physics::JoltRuntimeReferenceCount() == 0u);

    auto created = physics::JoltPhysicsScene::Create(allocator, TestSettings(), nullptr);
    REQUIRE(created);
    physics::JoltPhysicsScene& scene = *created.Value();
    CHECK(scene.Step(0.0f).Error() == ErrorCode::InvalidArgument);
    CHECK(scene.Step(-1.0f).Error() == ErrorCode::InvalidArgument);
    CHECK(scene.Step(NAN).Error() == ErrorCode::InvalidArgument);
    CHECK(scene.Step(INFINITY).Error() == ErrorCode::InvalidArgument);
    CHECK(scene.Stats().steps == 0u);
}

// 중력은 defaultGravityZ(cm/s²)를 m/s²로 바꿔 Jolt +Y에 둔다(축 A).
TEST(Physics_SceneGravityFollowsSettingsOnJoltY) {
    SystemAllocator allocator;
    PhysicsSettings settings = TestSettings();
    settings.defaultGravityZ = -490.0f;
    auto created = physics::JoltPhysicsScene::Create(allocator, settings, nullptr);
    REQUIRE(created);
    const JPH::Vec3 gravity = created.Value()->System().GetGravity();
    CHECK(gravity.GetX() == 0.0f);
    CHECK(gravity.GetY() == -4.9f);
    CHECK(gravity.GetZ() == 0.0f);
}

// scheduler에 job을 제출해도, 제출이 막혀 미룬 job을 barrier 대기 thread·worker가 실행해도,
// scheduler 없이 돌려도 같은 장면은 같은 상태 byte다(job 배정이 결과를 바꾸지 않는다 — PC06·6.7절).
TEST(Physics_JobsOnSchedulerMatchInlineAndDeferredRuns) {
    const uint64 baseline = physics::JoltLiveAllocations();
    JPH::StateRecorderImpl inlineState;
    RunStack(nullptr, inlineState, nullptr);

    SystemAllocator schedulerAllocator;
    JPH::StateRecorderImpl scheduledState;
    {
        WorkerScheduler scheduler(schedulerAllocator);
        WorkerSchedulerConfig config;
        config.workerCount = SchedulerWorkers;
        config.maxInFlight = SchedulerInFlight;
        REQUIRE(scheduler.Start(config));
        RunStack(&scheduler, scheduledState, nullptr);
        CHECK(scheduler.Shutdown());
    }

    JPH::StateRecorderImpl deferredState;
    StackCounters deferred;
    {
        WorkerScheduler scheduler(schedulerAllocator);
        WorkerSchedulerConfig config;
        config.workerCount = SchedulerWorkers;
        config.maxInFlight = SaturatedInFlight;
        REQUIRE(scheduler.Start(config));
        // 유일한 칸을 쥐어 모든 Submit이 CapacityExceeded로 거부된다(Pending 이벤트는 칸 하나를
        // 쓴다 — WorkerScheduler.h).
        auto occupied = scheduler.TryCreateEvent();
        REQUIRE(occupied);
        RunStack(&scheduler, deferredState, &deferred);
        CHECK(occupied.Value().TryTrigger());
        CHECK(scheduler.Shutdown());
    }

    CHECK(inlineState.GetDataSize() != 0);
    CHECK(inlineState.IsEqual(scheduledState));
    CHECK(inlineState.IsEqual(deferredState));
    CHECK(deferred.deferredJobs != 0u);
    CHECK(physics::JoltRuntimeReferenceCount() == 0u);
    CHECK(physics::JoltLiveAllocations() == baseline);
}

// 장면 thread가 다른 FP 환경(FTZ — 실시간 오디오 환경)에 있어도 step은 결정적 환경에서 돌아 같은
// 상태 byte이고, 환경을 바꾼 횟수가 step마다 잡힌다(어댑터 경계 10.4절 FP 상태, 7.1.1절).
TEST(Physics_StepRunsInDeterministicFloatingPointEnvironment) {
    JPH::StateRecorderImpl reference;
    StackCounters referenceCounters;
    RunStack(nullptr, reference, &referenceCounters);
    CHECK(referenceCounters.sceneFloatingPointCorrections == 0u);

    struct FlushedRun {
        JPH::StateRecorderImpl state;
        StackCounters counters;
    };

    FlushedRun flushed;
    platform::RunWithRealtimeAudioFloatingPoint(
        [](void* context) noexcept {
            auto& run = *static_cast<FlushedRun*>(context);
            RunStack(nullptr, run.state, &run.counters);
        },
        &flushed);
    CHECK(flushed.counters.sceneFloatingPointCorrections == static_cast<uint64>(StackSteps));
    CHECK(reference.IsEqual(flushed.state));
}

// 종료 중인 scheduler(Shutdown 뒤 Submit 거부)에 이어 step해도 모든 job을 미루고 barrier 대기
// thread가 실행해 step이 끝난다(종료 중 job — PC06).
TEST(Physics_StepAfterSchedulerShutdownDefersJobsToBarrier) {
    SystemAllocator schedulerAllocator;
    WorkerScheduler scheduler(schedulerAllocator);
    WorkerSchedulerConfig config;
    config.workerCount = SchedulerWorkers;
    REQUIRE(scheduler.Start(config));
    {
        SystemAllocator allocator;
        auto created = physics::JoltPhysicsScene::Create(allocator, TestSettings(), &scheduler);
        REQUIRE(created);
        physics::JoltPhysicsScene& scene = *created.Value();
        FillStack(scene);
        CHECK(scene.Step(static_cast<float>(StepSeconds)));
        scene.Jobs().WaitForOutstandingTasks();
        REQUIRE(scheduler.Shutdown());
        const uint64 before = scene.Jobs().DeferredJobs();
        CHECK(scene.Step(static_cast<float>(StepSeconds)));
        CHECK(scene.Jobs().DeferredJobs() > before);
        CHECK(scene.Jobs().OutstandingTasks() == 0u);
    }

    CHECK(physics::JoltRuntimeReferenceCount() == 0u);
}

} // namespace physics_scene_tests_file

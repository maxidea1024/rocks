// Physics 모듈 PH1 World 접점 시험이다(Docs/Design/PhysicsModule 2.1·2.2절, 7.1.1절): Subsystem이
// 장면을 꽂고 떼는 수명, World 종류별 장면·step, World::SetPhysicsScene 규칙. 공개 API만 쓰므로
// Modular 시험에서도 돈다.
#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Physics/PhysicsWorldSubsystem.h>
#include <TestSupport/Test.h>
#include <World/Physics/PhysicsScene.h>
#include <World/World.h>

using namespace rocks;
namespace physics_world_tests_file {

namespace {

constexpr double StepSeconds = 1.0 / 60.0;

// WorldSubsystemTests와 같은 수명 순서의 World 고정 장치다.
struct WorldFixture {
    WorldFixture(Span<const WorldSubsystemType> subsystems, WorldKind kind,
                 WorkerScheduler* scheduler = nullptr) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk();
        WorldDesc desc;
        desc.name = "PhysicsTest";
        desc.context = {0x5048595349435331ull, 1};
        desc.timeStep = {StepSeconds, 0.25, 8};
        desc.subsystems = subsystems;
        desc.kind = kind;
        desc.gcScheduler = scheduler;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~WorldFixture() {
        world.Reset();
        CHECK(modules.Unregister().IsOk());
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

// Game World: Subsystem이 장면을 꽂고 step한다. 정리하면 접점이 비고 공유 자원이 풀린다. 진행 중
// World에는 접점을 바꿀 수 없다.
TEST(Physics_GameWorldPlugsSceneAndSimulates) {
    const WorldSubsystemType subsystems[] = {PhysicsWorldSubsystem::Type()};
    WorldFixture fixture(subsystems, WorldKind::Game);
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    CHECK(world.GetPhysicsScene() == nullptr);
    REQUIRE(world.Initialize());
    auto* physics = world.GetSubsystem<PhysicsWorldSubsystem>();
    REQUIRE(physics != nullptr);
    REQUIRE(world.GetPhysicsScene() != nullptr);
    CHECK(world.GetPhysicsScene() == physics->Scene());
    CHECK(physics->Simulates());
    CHECK(physics->SimulateStep(StepSeconds));
    CHECK(physics->SimulateStep(0.0).Error() == ErrorCode::InvalidArgument);
    CHECK(world.GetPhysicsScene()->Stats().steps == 1u);

    REQUIRE(world.BeginPlay());
    CHECK(world.SetPhysicsScene(nullptr).Error() == ErrorCode::InvalidArgument);
    REQUIRE(world.Shutdown());
    CHECK(world.GetPhysicsScene() == nullptr);
}

// Editor·Preview World는 장면(질의용)만 있고 step하지 않는다. Inactive World는 장면이 없다.
TEST(Physics_EditorAndPreviewWorldsHaveSceneWithoutSteps) {
    const WorldSubsystemType subsystems[] = {PhysicsWorldSubsystem::Type()};
    for (WorldKind kind : {WorldKind::Editor, WorldKind::Preview}) {
        WorldFixture fixture(subsystems, kind);
        REQUIRE(fixture.world);
        REQUIRE(fixture.world->Initialize());
        auto* physics = fixture.world->GetSubsystem<PhysicsWorldSubsystem>();
        REQUIRE(physics != nullptr);
        CHECK(fixture.world->GetPhysicsScene() != nullptr);
        CHECK(!physics->Simulates());
        CHECK(physics->SimulateStep(StepSeconds).Error() == ErrorCode::WrongContext);
        REQUIRE(fixture.world->Shutdown());
    }

    WorldFixture inactive(subsystems, WorldKind::Inactive);
    REQUIRE(inactive.world);
    REQUIRE(inactive.world->Initialize());
    CHECK(inactive.world->GetSubsystem<PhysicsWorldSubsystem>() == nullptr);
    CHECK(inactive.world->GetPhysicsScene() == nullptr);
    REQUIRE(inactive.world->Shutdown());
}

// 접점 규칙: Initialized에서 다른 장면을 겹쳐 꽂으면 AlreadyExists, Created에서는 InvalidArgument.
TEST(Physics_WorldSetPhysicsSceneRules) {
    struct NullScene final : PhysicsScene {
        PhysicsSceneStats Stats() const noexcept override {
            return {};
        }
    };

    NullScene first;
    NullScene second;
    WorldFixture fixture({}, WorldKind::Game);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    CHECK(world.SetPhysicsScene(&first).Error() == ErrorCode::InvalidArgument);
    REQUIRE(world.Initialize());
    CHECK(world.SetPhysicsScene(&first));
    CHECK(world.SetPhysicsScene(&first));
    CHECK(world.SetPhysicsScene(&second).Error() == ErrorCode::AlreadyExists);
    CHECK(world.GetPhysicsScene() == &first);
    CHECK(world.SetPhysicsScene(nullptr));
    CHECK(world.SetPhysicsScene(&second));
    REQUIRE(world.Shutdown());
    CHECK(world.GetPhysicsScene() == nullptr);
}

} // namespace physics_world_tests_file

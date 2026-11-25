#include <Base/InitializerList.h>
#include <Core/Math/Rotator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Meta/TypeSchema.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/MovementComponent.generated.h>
#include <World/MovementComponent.h>
#include <World/PathMoverComponent.generated.h>
#include <World/PathMoverComponent.h>
#include <World/ProjectileMovementComponent.generated.h>
#include <World/ProjectileMovementComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SpringArmComponent.generated.h>
#include <World/SpringArmComponent.h>
#include <World/World.h>
#include <math.h>
#include <stdio.h>

// D4 이동 컴포넌트 시험이다(Docs/Design/EngineRuntime 26.8절): 갱신 대상·평면 제약·이동, 회전 이동,
// 발사체(Verlet·중력·최대 속도·homing·회전 따름·정지), PathMover 네 동작, SpringArm(이름 없는 부착·
// offset·inherit·지연·최대 거리·view 회전), 리플렉션·저장 왕복. 모두 실제 World Tick으로 구동한다.
using namespace rocks;
namespace world_movement_component_tests_file {

namespace {

constexpr double Tolerance = 1.0e-3;
constexpr double Step = 0.1;

bool Near(double value, double expected, double tolerance = Tolerance) noexcept {
    const bool same = fabs(value - expected) <= tolerance;
    if (!same) {
        printf("  value %.9g, expected %.9g\n", value, expected);
    }

    return same;
}

bool NearVector(const Vec3d& value, double x, double y, double z,
                double tolerance = Tolerance) noexcept {
    const bool same = fabs(value.x - x) <= tolerance && fabs(value.y - y) <= tolerance &&
                      fabs(value.z - z) <= tolerance;
    if (!same) {
        printf("  vector (%.9g, %.9g, %.9g), expected (%g, %g, %g)\n", value.x, value.y, value.z, x,
               y, z);
    }

    return same;
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

// 바라보는 회전을 주는 Actor다(Pawn control rotation 대신 — 26.2절).
class ViewActor final : public Actor {
    ROCKS_NATIVE_TYPE(ViewActor, Actor, 0x4e7b2c90a5d13f68ull, 0xb1d6e84f20c93a57ull)

public:
    explicit ViewActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    bool TryGetViewRotation(Rotator<double>& out) noexcept override {
        out = Rotator<double>(0.0, 45.0, 0.0);
        return true;
    }
};

// 정지 hook 횟수를 센다.
class CountingProjectile final : public ProjectileMovementComponent {
    ROCKS_NATIVE_TYPE(CountingProjectile, ProjectileMovementComponent, 0x91c3f0a7e25b4d86ull,
                      0x3a8d5e1fb7c64029ull)

public:
    explicit CountingProjectile(Allocator& allocator) noexcept
        : ProjectileMovementComponent(allocator) {
    }

    int32 stops = 0;

protected:
    void OnProjectileStop() noexcept override {
        ++stops;
    }
};

// 정지·반전·되돌림 hook을 센다.
class CountingPathMover final : public PathMoverComponent {
    ROCKS_NATIVE_TYPE(CountingPathMover, PathMoverComponent, 0x5f0e92c4a1b73d68ull,
                      0xe7a4c3019d6b5f82ull)

public:
    explicit CountingPathMover(Allocator& allocator) noexcept : PathMoverComponent(allocator) {
    }

    int32 stops = 0;
    int32 reverses = 0;
    int32 resets = 0;
    float lastStopTime = -1.0f;

protected:
    void OnPathStop(float time) noexcept override {
        ++stops;
        lastStopTime = time;
    }

    void OnPathReverse(float) noexcept override {
        ++reverses;
    }

    void OnPathReset(float) noexcept override {
        ++resets;
    }
};

// Tick 순서를 기록한다('a' Actor, 's' SceneComponent, 'm' 이동).
char tickOrder[8] = {};
int tickOrderCount = 0;

void RecordTick(char who) noexcept {
    if (tickOrderCount < 8) {
        tickOrder[tickOrderCount++] = who;
    }
}

class OrderActor final : public Actor {
    ROCKS_NATIVE_TYPE(OrderActor, Actor, 0x7c21e5a09b3d4f68ull, 0x2d9f0b7e6a1c4835ull)

public:
    explicit OrderActor(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().tick = true;
    }

    void OnTick(const TickContext&) noexcept override {
        RecordTick('a');
    }
};

class OrderScene final : public SceneComponent {
    ROCKS_NATIVE_TYPE(OrderScene, SceneComponent, 0x3e8a6b2c0d5f4917ull, 0x9b0c7d2e4f6a1385ull)

public:
    explicit OrderScene(Allocator& allocator) noexcept : SceneComponent(allocator) {
        PrimaryTick().tick = true;
    }

    void OnTick(const TickContext&) noexcept override {
        RecordTick('s');
    }
};

class OrderMover final : public RotatingMovementComponent {
    ROCKS_NATIVE_TYPE(OrderMover, RotatingMovementComponent, 0x5a1d8e3f7b2c4096ull,
                      0xc4e0b9a2d7f63158ull)

public:
    explicit OrderMover(Allocator& allocator) noexcept : RotatingMovementComponent(allocator) {
    }

protected:
    void OnTick(const TickContext& context) noexcept override {
        RecordTick('m');
        RotatingMovementComponent::OnTick(context);
    }
};

struct Fixture {
    explicit Fixture(WorldKind kind = WorldKind::Game) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && Register<ViewActor>() &&
                     Register<CountingProjectile>() && Register<CountingPathMover>() &&
                     Register<OrderActor>() && Register<OrderScene>() && Register<OrderMover>();
        WorldDesc desc;
        desc.name = "Movers";
        desc.context = {0x4d4f564eull, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.kind = kind;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~Fixture() {
        world.Reset();
        while (tokenCount != 0) {
            CHECK(types.Unregister(tokens[--tokenCount]).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    template <class T>
    bool Register() noexcept {
        auto token = RegisterNativeClass<T>(types);
        if (!token || tokenCount == MaxTokens) {
            return false;
        }

        tokens[tokenCount++] = token.Value();
        return true;
    }

    // root SceneComponent를 가진 Actor다(A = Actor 타입).
    template <class A = Actor>
    ObjectHandle SpawnWithRoot(ObjectHandle& root, ObjectHandle level = {}) noexcept {
        SpawnParams params;
        params.level = level;
        auto actor = world->SpawnActor<A>(params);
        if (!actor) {
            return {};
        }

        auto scene = world->AddComponent<SceneComponent>(actor.Value());
        if (!scene) {
            return {};
        }

        root = scene.Value();
        auto rooted = world->With<Actor>(
            actor.Value(), [&](Actor& value) noexcept { return value.SetRootComponent(root); });
        return rooted ? actor.Value() : ObjectHandle{};
    }

    Vec3d LocationOf(ObjectHandle scene) noexcept {
        auto location = world->With<SceneComponent>(
            scene, [](SceneComponent& value) noexcept { return value.WorldLocation(); });
        return location ? location.Value() : Vec3d(-1.0, -1.0, -1.0);
    }

    Rotator<double> RotationOf(ObjectHandle scene) noexcept {
        auto rotation = world->With<SceneComponent>(scene, [](SceneComponent& value) noexcept {
            auto rotator = Rotator<double>::TryFromQuaternion(value.WorldQuaternion());
            return rotator ? rotator.Value() : Rotator<double>();
        });
        return rotation ? rotation.Value() : Rotator<double>();
    }

    bool TickTimes(int count, double seconds = Step) noexcept {
        for (int index = 0; index < count; ++index) {
            if (!world->Tick(seconds)) {
                return false;
            }
        }

        return true;
    }

    static constexpr usize MaxTokens = 8;
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken tokens[MaxTokens];
    usize tokenCount = 0;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

// 26.3절: 갱신 대상 자동 등록, 평면 제약, 이동, 속도·최대 속도, Static 대상.
TEST(Movement_UpdatedComponentAndPlaneConstraint) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle root;
    const ObjectHandle actor = fixture.SpawnWithRoot(root);
    REQUIRE(!actor.IsNull());
    auto mover = world.AddComponent<RotatingMovementComponent>(actor);
    REQUIRE(mover);
    CHECK(world.With<RotatingMovementComponent>(
        mover.Value(), [&](RotatingMovementComponent& m) noexcept {
            // 기본값(UE 생성자).
            CHECK(m.UpdatedComponent() == root);
            CHECK(m.AutoRegistersUpdatedComponent() && m.AutoUpdatesTickRegistration() &&
                  m.TicksBeforeOwner());
            CHECK(!m.IsConstrainedToPlane() && !m.SnapsToPlaneAtStart());
            CHECK(m.PlaneConstraintAxisSetting() == PlaneConstraintAxis::Custom);
            CHECK(m.GetTickSettings().tick && m.GetTickSettings().phase == TickPhase::PrePhysics);
            CHECK(m.GravityZ() == DefaultGravityZ && m.MaxSpeed() == 0.0);
            CHECK(Near(m.RotationRate().yaw, 180.0) && m.IsRotationInLocalSpace());
            // 평면 제약: 꺼져 있으면 그대로.
            CHECK(NearVector(m.ConstrainDirectionToPlane(Vec3d(1.0, 2.0, 3.0)), 1.0, 2.0, 3.0));
            m.SetConstrainToPlane(true);
            CHECK(m.SetPlaneConstraintAxisSetting(PlaneConstraintAxis::Z));
            CHECK(NearVector(m.PlaneConstraintNormal(), 0.0, 0.0, 1.0));
            CHECK(NearVector(m.ConstrainDirectionToPlane(Vec3d(1.0, 2.0, 3.0)), 1.0, 2.0, 0.0));
            CHECK(m.SetPlaneConstraintOrigin(Vec3d(0.0, 0.0, 5.0)));
            CHECK(NearVector(m.ConstrainLocationToPlane(Vec3d(1.0, 2.0, 3.0)), 1.0, 2.0, 5.0));
            CHECK(NearVector(m.ConstrainNormalToPlane(Vec3d(1.0, 0.0, 1.0)), 1.0, 0.0, 0.0));
            // Custom 법선은 정규화하고, 전역 설정은 제약 없음이다.
            CHECK(m.SetPlaneConstraintNormal(Vec3d(0.0, 2.0, 0.0)));
            CHECK(m.PlaneConstraintAxisSetting() == PlaneConstraintAxis::Custom);
            CHECK(NearVector(m.PlaneConstraintNormal(), 0.0, 1.0, 0.0));
            CHECK(m.SetPlaneConstraintFromVectors(Vec3d(1.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0)));
            CHECK(NearVector(m.PlaneConstraintNormal(), 0.0, 1.0, 0.0));
            CHECK(m.SetPlaneConstraintAxisSetting(PlaneConstraintAxis::UseGlobalPhysicsSetting));
            CHECK(NearVector(m.ConstrainDirectionToPlane(Vec3d(1.0, 2.0, 3.0)), 1.0, 2.0, 3.0));
            CHECK(ErrorOf(m.SetPlaneConstraintAxisSetting(static_cast<PlaneConstraintAxis>(9))) ==
                  ErrorCode::InvalidArgument);
            // 이동: Z 평면 제약이면 Z 성분을 버리고 회전을 바꾼다.
            CHECK(m.SetPlaneConstraintAxisSetting(PlaneConstraintAxis::Z));
            const Quatd yaw = Rotator<double>(0.0, 30.0, 0.0).ToQuaternion();
            CHECK(m.MoveUpdatedComponent(Vec3d(10.0, 0.0, 5.0), yaw));
            // 속도.
            CHECK(m.SetVelocity(Vec3d(30.0, 40.0, 0.0)));
            CHECK(m.IsExceedingMaxSpeed(49.0) && !m.IsExceedingMaxSpeed(50.0));
            m.UpdateComponentVelocity();
            CHECK(ErrorOf(m.SetVelocity(Vec3d(NAN, 0.0, 0.0))) == ErrorCode::InvalidArgument);
            m.SetConstrainToPlane(false);
        }));
    CHECK(NearVector(fixture.LocationOf(root), 10.0, 0.0, 0.0));
    CHECK(Near(fixture.RotationOf(root).yaw, 30.0));
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& scene) noexcept {
        CHECK(NearVector(scene.ComponentVelocity(), 30.0, 40.0, 0.0));
    }));
    CHECK(world.With<RotatingMovementComponent>(mover.Value(),
                                                [](RotatingMovementComponent& m) noexcept {
                                                    m.StopMovementImmediately();
                                                    CHECK(NearVector(m.Velocity(), 0.0, 0.0, 0.0));
                                                }));
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& scene) noexcept {
        CHECK(NearVector(scene.ComponentVelocity(), 0.0, 0.0, 0.0));
        // Static 대상: 게임 World 초기화 뒤에는 움직일 수 없다.
        scene.SetMobility(ComponentMobility::Static);
    }));
    CHECK(world.With<RotatingMovementComponent>(
        mover.Value(), [](RotatingMovementComponent& m) noexcept {
            CHECK(m.ShouldSkipUpdate());
            CHECK(ErrorOf(m.MoveUpdatedComponent(Vec3d(1.0, 0.0, 0.0), Quatd::Identity())) ==
                  ErrorCode::InvalidArgument);
            // 대상 해제는 Tick을 끈다.
            CHECK(m.SetUpdatedComponent({}));
            CHECK(m.UpdatedComponent().IsNull() && m.ShouldSkipUpdate());
            CHECK(ErrorOf(m.MoveUpdatedComponent(Vec3d(), Quatd::Identity())) ==
                  ErrorCode::InvalidArgument);
        }));
    REQUIRE(world.Shutdown());
}

// 26.3절: 파괴된 갱신 대상은 다음 Tick에 풀린다(회전·발사체 — UE Super::TickComponent), 파괴 대기
// 대상은 받지 않는다, 사라진 homing 대상은 substep 조건이 아니다.
TEST(Movement_DestroyedTargetsAreReleased) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle root;
    const ObjectHandle actor = fixture.SpawnWithRoot(root);
    REQUIRE(!actor.IsNull());
    auto spare = world.AddComponent<SceneComponent>(actor);
    auto homing = world.AddComponent<SceneComponent>(actor);
    auto spin = world.AddComponent<RotatingMovementComponent>(actor);
    auto shot = world.AddComponent<CountingProjectile>(actor);
    REQUIRE(spare && homing && spin && shot);
    CHECK(world.With<RotatingMovementComponent>(spin.Value(),
                                                [&](RotatingMovementComponent& m) noexcept {
                                                    return m.SetUpdatedComponent(spare.Value());
                                                }));
    CHECK(world.With<CountingProjectile>(shot.Value(), [&](CountingProjectile& p) noexcept {
        p.SetHomingProjectile(true);
        p.SetHomingTargetComponent(homing.Value());
        CHECK(p.SetProjectileGravityScale(0.0));
        CHECK(p.ShouldUseSubStepping());
        return p.SetUpdatedComponent(spare.Value());
    }));
    REQUIRE(world.BeginPlay());
    REQUIRE(world.DestroyComponent(homing.Value()));
    REQUIRE(fixture.TickTimes(1));
    CHECK(world.With<CountingProjectile>(
        shot.Value(), [](CountingProjectile& p) noexcept { CHECK(!p.ShouldUseSubStepping()); }));
    REQUIRE(world.DestroyComponent(spare.Value()));
    // 파괴 대기 대상은 새 갱신 대상이 될 수 없다.
    CHECK(world.With<RotatingMovementComponent>(
        spin.Value(), [&](RotatingMovementComponent& m) noexcept {
            CHECK(ErrorOf(m.SetUpdatedComponent(spare.Value())) == ErrorCode::InvalidArgument);
        }));
    REQUIRE(fixture.TickTimes(2));
    CHECK(world.With<RotatingMovementComponent>(
        spin.Value(),
        [](RotatingMovementComponent& m) noexcept { CHECK(m.UpdatedComponent().IsNull()); }));
    CHECK(world.With<CountingProjectile>(shot.Value(), [](CountingProjectile& p) noexcept {
        CHECK(p.UpdatedComponent().IsNull() && p.HasStoppedSimulation());
    }));
    REQUIRE(world.Shutdown());
}

// 26.3절: Tick 간선 — 갱신 대상·소유 Actor의 OnTick이 이동 컴포넌트 뒤다(UE AddPrerequisite).
TEST(Movement_TickRunsBeforeOwnerAndTarget) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActor<OrderActor>();
    REQUIRE(actor);
    auto root = world.AddComponent<OrderScene>(actor.Value());
    REQUIRE(root);
    REQUIRE(world.With<Actor>(actor.Value(), [&](Actor& value) noexcept {
        return value.SetRootComponent(root.Value());
    }));
    auto mover = world.AddComponent<OrderMover>(actor.Value());
    REQUIRE(mover);
    REQUIRE(world.BeginPlay());
    tickOrderCount = 0;
    REQUIRE(fixture.TickTimes(1));
    REQUIRE(tickOrderCount == 3);
    CHECK(tickOrder[0] == 'm');
    CHECK((tickOrder[1] == 'a' && tickOrder[2] == 's') ||
          (tickOrder[1] == 's' && tickOrder[2] == 'a'));
    REQUIRE(world.Shutdown());
}

// 26.2·26.3절: 부모가 있는 대상의 월드 설정(회전·scale한 부모, socket 부모, 절대 성분, 역변환
// 실패·Mobility 실패는 그대로), snapToPlaneAtStart.
TEST(Movement_SetWorldLocationWithParents) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle root;
    const ObjectHandle actor = fixture.SpawnWithRoot(root);
    REQUIRE(!actor.IsNull());
    auto child = world.AddComponent<SceneComponent>(actor);
    REQUIRE(child);
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        if (auto rotated =
                value.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion());
            !rotated) {
            return rotated;
        }

        if (auto scaled = value.SetRelativeScale(Vec3d(2.0, 2.0, 2.0)); !scaled) {
            return scaled;
        }

        return value.SetRelativeLocation(Vec3d(10.0, 0.0, 0.0));
    }));
    CHECK(world.With<SceneComponent>(
        child.Value(), [&](SceneComponent& value) noexcept { return value.AttachTo(root); }));
    const Quatd yaw45 = Rotator<double>(0.0, 45.0, 0.0).ToQuaternion();
    CHECK(world.With<SceneComponent>(child.Value(), [&](SceneComponent& value) noexcept {
        CHECK(value.SetWorldLocationAndRotation(Vec3d(10.0, 100.0, 0.0), yaw45));
        // 상대 = 부모 frame의 역: 위치 (100/2, 0, 0)을 yaw −90으로 → (50, 0, 0), 회전 yaw −45.
        CHECK(NearVector(value.RelativeTransform().translation, 50.0, 0.0, 0.0));
        CHECK(NearVector(value.WorldLocation(), 10.0, 100.0, 0.0));
        auto rotator = Rotator<double>::TryFromQuaternion(value.WorldQuaternion());
        CHECK(rotator && Near(rotator.Value().yaw, 45.0));
        // 절대 위치: 월드 값 그대로.
        CHECK(value.SetAbsolute(true, false, false));
        CHECK(value.SetWorldLocationAndRotation(Vec3d(1.0, 2.0, 3.0), yaw45));
        CHECK(NearVector(value.RelativeTransform().translation, 1.0, 2.0, 3.0));
        CHECK(value.SetAbsolute(false, false, false));
        // 비유한 값은 거부하고 그대로 둔다.
        const Transform before = value.RelativeTransform();
        CHECK(ErrorOf(value.SetWorldLocationAndRotation(Vec3d(NAN, 0.0, 0.0), yaw45)) ==
              ErrorCode::InvalidArgument);
        CHECK(value.RelativeTransform().translation == before.translation);
    }));
    // 역변환할 수 없는 부모(scale 0)는 InvalidArgument, 바뀌지 않는다.
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeScale(Vec3d(0.0, 1.0, 1.0));
    }));
    CHECK(world.With<SceneComponent>(child.Value(), [&](SceneComponent& value) noexcept {
        const Vec3d before = value.RelativeTransform().translation;
        CHECK(ErrorOf(value.SetWorldLocationAndRotation(Vec3d(5.0, 5.0, 5.0), yaw45)) ==
              ErrorCode::InvalidArgument);
        CHECK(value.RelativeTransform().translation == before);
        // Static이면 Mobility가 거부한다(게임 World 초기화 뒤).
        value.SetMobility(ComponentMobility::Static);
    }));
    CHECK(world.With<SceneComponent>(
        root, [](SceneComponent& value) noexcept { return value.SetRelativeScale(Vec3d::One()); }));
    CHECK(world.With<SceneComponent>(child.Value(), [&](SceneComponent& value) noexcept {
        CHECK(ErrorOf(value.SetWorldLocationAndRotation(Vec3d(5.0, 5.0, 5.0), yaw45)) ==
              ErrorCode::InvalidArgument);
        value.SetMobility(ComponentMobility::Movable);
    }));

    // socket 부모(SpringArm 팔 끝): 월드 위치를 팔 끝 기준 상대 값으로 바꾼다.
    auto arm = world.AddComponent<SpringArmComponent>(actor);
    auto tip = world.AddComponent<SceneComponent>(actor);
    REQUIRE(arm && tip);
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        if (auto rotated = value.SetRelativeRotation(Quatd::Identity()); !rotated) {
            return rotated;
        }

        return value.SetRelativeLocation(Vec3d());
    }));
    CHECK(world.With<SpringArmComponent>(arm.Value(), [&](SpringArmComponent& value) noexcept {
        if (auto attached = value.AttachTo(root); !attached) {
            return attached;
        }

        value.UpdateDesiredArmLocation(false, false, 0.0);
        return Result<void>::Success();
    }));
    CHECK(world.With<SceneComponent>(tip.Value(), [&](SceneComponent& value) noexcept {
        if (auto attached = value.AttachTo(arm.Value(), AttachmentRules::KeepRelativeTransform(),
                                           StringView("SpringEndpoint"));
            !attached) {
            return attached;
        }

        CHECK(NearVector(value.WorldLocation(), -300.0, 0.0, 0.0));
        CHECK(value.SetWorldLocationAndRotation(Vec3d(-290.0, 0.0, 0.0), Quatd::Identity()));
        CHECK(NearVector(value.RelativeTransform().translation, 10.0, 0.0, 0.0));
        return Result<void>::Success();
    }));

    // snapToPlaneAtStart: 갱신 대상을 정할 때 평면에 붙인다.
    auto mover = world.AddComponent<RotatingMovementComponent>(actor);
    REQUIRE(mover);
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeLocation(Vec3d(1.0, 2.0, 30.0));
    }));
    CHECK(world.With<RotatingMovementComponent>(
        mover.Value(), [&](RotatingMovementComponent& m) noexcept {
            m.SetConstrainToPlane(true);
            m.SetSnapToPlaneAtStart(true);
            CHECK(m.SetPlaneConstraintAxisSetting(PlaneConstraintAxis::Z));
            CHECK(m.SetPlaneConstraintOrigin(Vec3d(0.0, 0.0, 5.0)));
            CHECK(m.SetUpdatedComponent({}));
            return m.SetUpdatedComponent(root);
        }));
    CHECK(NearVector(fixture.LocationOf(root), 1.0, 2.0, 5.0));
    REQUIRE(world.Shutdown());
}

// 26.4절: yaw 180°/s, local·world, pivot.
TEST(Movement_RotatingMovement) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle root;
    const ObjectHandle actor = fixture.SpawnWithRoot(root);
    REQUIRE(!actor.IsNull());
    auto mover = world.AddComponent<RotatingMovementComponent>(actor);
    REQUIRE(mover);
    REQUIRE(world.BeginPlay());
    // 0.5초 → yaw 90°.
    REQUIRE(fixture.TickTimes(5));
    CHECK(Near(fixture.RotationOf(root).yaw, 90.0, 1.0e-6));
    CHECK(NearVector(fixture.LocationOf(root), 0.0, 0.0, 0.0));
    // pivot (100, 0, 0): pivot 둘레로 돈다 — Δ = Q0·p − Q1·p.
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& scene) noexcept {
        return scene.SetRelativeRotation(Quatd::Identity());
    }));
    CHECK(world.With<RotatingMovementComponent>(
        mover.Value(), [](RotatingMovementComponent& m) noexcept {
            return m.SetPivotTranslation(Vec3d(100.0, 0.0, 0.0));
        }));
    REQUIRE(fixture.TickTimes(5));
    CHECK(NearVector(fixture.LocationOf(root), 100.0, -100.0, 0.0, 1.0e-6));
    CHECK(Near(fixture.RotationOf(root).yaw, 90.0, 1.0e-6));
    // world 공간: roll 90°인 대상에서 yaw 회전은 월드 Z축 둘레다(ΔQ × Q — local이면 Q × ΔQ로 대상
    // 축 둘레).
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& scene) noexcept {
        if (auto moved = scene.SetRelativeLocation(Vec3d()); !moved) {
            return moved;
        }

        return scene.SetRelativeRotation(Rotator<double>(0.0, 0.0, 90.0).ToQuaternion());
    }));
    CHECK(world.With<RotatingMovementComponent>(mover.Value(),
                                                [](RotatingMovementComponent& m) noexcept {
                                                    m.SetRotationInLocalSpace(false);
                                                    return m.SetPivotTranslation(Vec3d());
                                                }));
    REQUIRE(fixture.TickTimes(5));
    const Rotator<double> worldSpace = fixture.RotationOf(root);
    CHECK(Near(worldSpace.yaw, 90.0, 1.0e-6) && Near(worldSpace.roll, 90.0, 1.0e-6));
    REQUIRE(world.Shutdown());
}

// 26.5절: 초기 속도(local), 중력 궤적(Verlet — 손계산), 최대 속도, homing, 회전 따름, 정지.
TEST(Movement_ProjectileTrajectory) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());

    // 1) root yaw 90°, initialSpeed 1000, 중력 0 → velocity (0, 1000, 0), 0.1초에 100 cm. (첫
    // Actor의 root는 아래 homing 대상이다.)
    ObjectHandle root;
    const ObjectHandle actor = fixture.SpawnWithRoot(root);
    REQUIRE(!actor.IsNull());
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& scene) noexcept {
        return scene.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion());
    }));
    // 초기 속도는 OnAwake(UE InitializeComponent)에서 정해지므로 Actor 초기화 전에 붙인다.
    auto deferred = world.SpawnActorDeferred<Actor>();
    REQUIRE(deferred);
    auto deferredRoot = world.AddComponent<SceneComponent>(deferred.Value());
    auto shot = world.AddComponent<CountingProjectile>(deferred.Value());
    REQUIRE(deferredRoot && shot);
    CHECK(world.With<Actor>(deferred.Value(), [&](Actor& value) noexcept {
        return value.SetRootComponent(deferredRoot.Value());
    }));
    CHECK(world.With<SceneComponent>(deferredRoot.Value(), [](SceneComponent& scene) noexcept {
        return scene.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion());
    }));
    CHECK(world.With<CountingProjectile>(shot.Value(), [](CountingProjectile& p) noexcept {
        // 기본값(UE 생성자).
        CHECK(NearVector(p.Velocity(), 1.0, 0.0, 0.0));
        CHECK(p.InitialVelocityInLocalSpace() && p.IsSimulationEnabled() && p.SweepsCollision());
        CHECK(Near(p.Bounciness(), 0.6) && Near(p.Friction(), 0.2));
        CHECK(Near(p.MaxSimulationTimeStep(), 0.05) && p.MaxSimulationIterations() == 4);
        CHECK(Near(p.ProjectileGravityScale(), 1.0) && Near(p.GravityZ(), -980.0));
        CHECK(p.SetInitialSpeed(1000.0));
        return p.SetProjectileGravityScale(0.0);
    }));
    REQUIRE(world.FinishSpawning(deferred.Value()));
    CHECK(world.With<CountingProjectile>(shot.Value(), [](CountingProjectile& p) noexcept {
        CHECK(NearVector(p.Velocity(), 0.0, 1000.0, 0.0, 1.0e-6));
        CHECK(p.GravityZ() == 0.0 && !p.ShouldUseSubStepping());
    }));
    REQUIRE(world.BeginPlay());
    REQUIRE(fixture.TickTimes(1));
    CHECK(NearVector(fixture.LocationOf(deferredRoot.Value()), 0.0, 100.0, 0.0, 1.0e-6));

    // 2) 중력: v (1000, 0, 0), 0.1초 → x 100, z = −½·980·0.01 = −4.9(Verlet은 등가속도에 정확),
    // vz = −98. substep(중력이 있으면 쓴다)에도 같다.
    CHECK(world.With<SceneComponent>(deferredRoot.Value(), [](SceneComponent& scene) noexcept {
        if (auto moved = scene.SetRelativeLocation(Vec3d()); !moved) {
            return moved;
        }

        return scene.SetRelativeRotation(Quatd::Identity());
    }));
    CHECK(world.With<CountingProjectile>(shot.Value(), [](CountingProjectile& p) noexcept {
        CHECK(p.SetProjectileGravityScale(1.0) && p.SetVelocity(Vec3d(1000.0, 0.0, 0.0)));
        CHECK(p.ShouldUseSubStepping());
        CHECK(Near(p.SimulationTimeStep(0.3, 1), 0.05) && Near(p.SimulationTimeStep(0.3, 4), 0.3));
        CHECK(Near(p.SimulationTimeStep(0.06, 1), 0.03) &&
              Near(p.SimulationTimeStep(0.0, 1), 1.0e-6));
        p.SetRotationFollowsVelocity(true);
    }));
    REQUIRE(fixture.TickTimes(1));
    CHECK(NearVector(fixture.LocationOf(deferredRoot.Value()), 100.0, 0.0, -4.9, 1.0e-6));
    CHECK(world.With<CountingProjectile>(shot.Value(), [](CountingProjectile& p) noexcept {
        CHECK(NearVector(p.Velocity(), 1000.0, 0.0, -98.0, 1.0e-6));
    }));
    // 회전이 속도를 따른다(아래로 숙인다).
    CHECK(fixture.RotationOf(deferredRoot.Value()).pitch < 0.0);
    // 수직 유지면 yaw만 남는다.
    CHECK(world.With<CountingProjectile>(
        shot.Value(), [](CountingProjectile& p) noexcept { p.SetRotationRemainsVertical(true); }));
    REQUIRE(fixture.TickTimes(1));
    CHECK(Near(fixture.RotationOf(deferredRoot.Value()).pitch, 0.0, 1.0e-6));

    // 3) 최대 속도: 중력 0, 500 제한, v0 1000 → v1 500, 이동 = 1000·0.1 + (500 − 1000)·0.05 = 75.
    CHECK(world.With<SceneComponent>(deferredRoot.Value(), [](SceneComponent& scene) noexcept {
        return scene.SetRelativeLocation(Vec3d());
    }));
    CHECK(world.With<CountingProjectile>(shot.Value(), [](CountingProjectile& p) noexcept {
        p.SetRotationFollowsVelocity(false);
        CHECK(p.SetProjectileGravityScale(0.0) && p.SetMaxSpeed(500.0));
        return p.SetVelocity(Vec3d(1000.0, 0.0, 0.0));
    }));
    REQUIRE(fixture.TickTimes(1));
    CHECK(NearVector(fixture.LocationOf(deferredRoot.Value()), 75.0, 0.0, 0.0, 1.0e-6));
    CHECK(world.With<CountingProjectile>(shot.Value(), [](CountingProjectile& p) noexcept {
        CHECK(NearVector(p.Velocity(), 500.0, 0.0, 0.0, 1.0e-6));
        // homing: 대상(+Y 1000)으로 가속한다.
        CHECK(p.SetMaxSpeed(0.0) && p.SetVelocity(Vec3d()));
        p.SetHomingProjectile(true);
        CHECK(p.SetHomingAccelerationMagnitude(1000.0));
    }));
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeLocation(Vec3d(0.0, 1000.0, 0.0));
    }));
    CHECK(world.With<CountingProjectile>(shot.Value(), [&](CountingProjectile& p) noexcept {
        p.SetHomingTargetComponent(root);
        CHECK(p.ShouldUseSubStepping());
    }));
    REQUIRE(fixture.TickTimes(1));
    CHECK(world.With<CountingProjectile>(shot.Value(), [](CountingProjectile& p) noexcept {
        CHECK(p.Velocity().y > 0.0);
        // 정지: 대상 해제·hook 한 번.
        p.StopSimulating();
        CHECK(p.HasStoppedSimulation() && p.stops == 1);
        CHECK(NearVector(p.Velocity(), 0.0, 0.0, 0.0));
        CHECK(ErrorOf(p.SetMaxSimulationIterations(0)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(p.SetMaxSimulationTimeStep(0.01)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(p.SetInitialSpeed(-1.0)) == ErrorCode::InvalidArgument);
    }));
    const Vec3d stoppedAt = fixture.LocationOf(deferredRoot.Value());
    REQUIRE(fixture.TickTimes(1));
    CHECK(NearVector(fixture.LocationOf(deferredRoot.Value()), stoppedAt.x, stoppedAt.y,
                     stoppedAt.z));
    REQUIRE(world.Shutdown());
}

// 26.6절: PathMover 네 동작(OneShot·PingPong·LoopReset·OneShotReverse), 절대 점, speedMultiplier.
TEST(Movement_PathMoverBehaviours) {
    struct Case {
        PathMoverBehaviour behaviour;
        int ticks;        // 0.1초 단위
        double expectedX; // 최종 x(cm)
        bool stopped;
        int32 stops;
        int32 reverses;
        int32 resets;
    };

    // 점 (0)·(100) 상대, duration 1: 시간 t의 x = 100·t.
    const Case cases[] = {
        {PathMoverBehaviour::OneShot, 5, 50.0, false, 0, 0, 0},
        {PathMoverBehaviour::OneShot, 12, 100.0, true, 1, 0, 0},
        {PathMoverBehaviour::PingPong, 15, 50.0, false, 0, 1, 0},
        {PathMoverBehaviour::LoopReset, 13, 30.0, false, 0, 0, 1},
        {PathMoverBehaviour::OneShotReverse, 25, 0.0, true, 1, 1, 0},
    };
    for (const Case& value : cases) {
        Fixture fixture;
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        ObjectHandle root;
        const ObjectHandle actor = fixture.SpawnWithRoot(root);
        REQUIRE(!actor.IsNull());
        auto mover = world.AddComponent<CountingPathMover>(actor);
        REQUIRE(mover);
        CHECK(world.With<CountingPathMover>(mover.Value(), [&](CountingPathMover& path) noexcept {
            // 기본값(UE 생성자).
            CHECK(path.Duration() == 1.0f && path.Behaviour() == PathMoverBehaviour::OneShot);
            CHECK(path.SpeedMultiplier() == 0.0f && path.MaxSimulationIterations() == 8);
            CHECK(!path.PausesOnImpact() && path.Sweeps());
            CHECK(path.AddControlPoint(Vec3d(), true));
            CHECK(path.AddControlPoint(Vec3d(100.0, 0.0, 0.0), true));
            return path.SetBehaviour(value.behaviour);
        }));
        REQUIRE(world.BeginPlay());
        REQUIRE(fixture.TickTimes(value.ticks));
        CHECK(Near(fixture.LocationOf(root).x, value.expectedX, 1.0e-3));
        CHECK(world.With<CountingPathMover>(mover.Value(), [&](CountingPathMover& path) noexcept {
            CHECK(path.IsStopped() == value.stopped);
            CHECK(path.stops == value.stops && path.reverses == value.reverses &&
                  path.resets == value.resets);
        }));
        REQUIRE(world.Shutdown());
    }

    // 절대 점과 speedMultiplier 2: 0.25초에 반(시작 위치와 무관한 절대 좌표).
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle root;
    const ObjectHandle actor = fixture.SpawnWithRoot(root);
    REQUIRE(!actor.IsNull());
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& scene) noexcept {
        return scene.SetRelativeLocation(Vec3d(0.0, 0.0, 77.0));
    }));
    auto mover = world.AddComponent<CountingPathMover>(actor);
    REQUIRE(mover);
    CHECK(world.With<CountingPathMover>(mover.Value(), [](CountingPathMover& path) noexcept {
        CHECK(path.AddControlPoint(Vec3d(0.0, 0.0, 0.0), false));
        CHECK(path.AddControlPoint(Vec3d(0.0, 200.0, 0.0), false));
        CHECK(ErrorOf(path.SetDuration(0.05f)) == ErrorCode::InvalidArgument);
        return path.SetSpeedMultiplier(2.0f);
    }));
    REQUIRE(world.BeginPlay());
    // 시작 때 점 0(원점)으로 옮긴다.
    CHECK(NearVector(fixture.LocationOf(root), 0.0, 0.0, 0.0));
    REQUIRE(fixture.TickTimes(1, 0.25));
    CHECK(NearVector(fixture.LocationOf(root), 0.0, 100.0, 0.0));
    CHECK(world.With<CountingPathMover>(mover.Value(), [](CountingPathMover& path) noexcept {
        CHECK(Near(path.CurrentTime(), 0.5));
        path.StopMovementImmediately();
        CHECK(path.IsStopped() && path.stops == 1 && Near(path.lastStopTime, 0.5));
        path.RestartMovement();
        CHECK(!path.IsStopped() && path.CurrentTime() == 0.0f);
    }));
    REQUIRE(world.Shutdown());
}

// 26.7절: 이름 없는 자식이 팔 끝에, offset·inherit·지연·최대 거리·view 회전.
TEST(Movement_SpringArm) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle root;
    const ObjectHandle actor = fixture.SpawnWithRoot<ViewActor>(root);
    REQUIRE(!actor.IsNull());
    auto arm = world.AddComponent<SpringArmComponent>(actor);
    auto camera = world.AddComponent<SceneComponent>(actor);
    REQUIRE(arm && camera);
    CHECK(world.With<SpringArmComponent>(arm.Value(), [&](SpringArmComponent& value) noexcept {
        // 기본값(UE 생성자).
        CHECK(Near(value.TargetArmLength(), 300.0) && Near(value.ProbeSize(), 12.0));
        CHECK(value.DoesCollisionTest() && !value.UsesPawnControlRotation());
        CHECK(value.InheritsPitch() && value.InheritsYaw() && value.InheritsRoll());
        CHECK(!value.HasCameraLag() && !value.HasCameraRotationLag() &&
              value.UsesCameraLagSubstepping());
        CHECK(Near(value.CameraLagSpeed(), 10.0) && Near(value.CameraLagMaxTimeStep(), 1.0 / 60.0));
        CHECK(value.GetTickSettings().phase == TickPhase::PostPhysics);
        CHECK(value.HasSocket(StringView("SpringEndpoint")) && !value.HasSocket(StringView("X")));
        return value.AttachTo(root);
    }));
    CHECK(world.With<SceneComponent>(camera.Value(), [&](SceneComponent& value) noexcept {
        return value.AttachTo(arm.Value());
    }));
    REQUIRE(world.BeginPlay());
    REQUIRE(fixture.TickTimes(1));
    // 이름 없는 부착: 팔 끝(뒤로 300).
    CHECK(NearVector(fixture.LocationOf(camera.Value()), -300.0, 0.0, 0.0));
    // socketOffset(팔 회전 공간)·targetOffset(월드).
    CHECK(world.With<SpringArmComponent>(arm.Value(), [](SpringArmComponent& value) noexcept {
        CHECK(value.SetSocketOffset(Vec3d(0.0, 0.0, 50.0)));
        return value.SetTargetOffset(Vec3d(0.0, 0.0, 100.0));
    }));
    REQUIRE(fixture.TickTimes(1));
    CHECK(NearVector(fixture.LocationOf(camera.Value()), -300.0, 0.0, 150.0));
    // root yaw 90°: 팔도 돈다 → (0, −300, 150). yaw 상속을 끄면 상대 회전(0)이다.
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion());
    }));
    REQUIRE(fixture.TickTimes(1));
    CHECK(NearVector(fixture.LocationOf(camera.Value()), 0.0, -300.0, 150.0));
    CHECK(world.With<SpringArmComponent>(arm.Value(), [](SpringArmComponent& value) noexcept {
        value.SetInherit(true, false, true);
    }));
    REQUIRE(fixture.TickTimes(1));
    CHECK(NearVector(fixture.LocationOf(camera.Value()), -300.0, 0.0, 150.0));
    // view 회전(소유 Actor — yaw 45°).
    CHECK(world.With<SpringArmComponent>(arm.Value(), [](SpringArmComponent& value) noexcept {
        value.SetInherit(true, true, true);
        value.SetUsePawnControlRotation(true);
        CHECK(value.SetSocketOffset(Vec3d()));
        return value.SetTargetOffset(Vec3d());
    }));
    REQUIRE(fixture.TickTimes(1));
    const double diagonal = 300.0 * sqrt(0.5);
    CHECK(NearVector(fixture.LocationOf(camera.Value()), -diagonal, -diagonal, 0.0));

    // 위치 지연: root를 +X 100 옮기고 0.01초(substep 없음) → 시작이 10%만 따라간다.
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeRotation(Quatd::Identity());
    }));
    CHECK(world.With<SpringArmComponent>(arm.Value(), [](SpringArmComponent& value) noexcept {
        value.SetUsePawnControlRotation(false);
    }));
    REQUIRE(fixture.TickTimes(1));
    CHECK(world.With<SpringArmComponent>(
        arm.Value(), [](SpringArmComponent& value) noexcept { value.SetEnableCameraLag(true); }));
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeLocation(Vec3d(100.0, 0.0, 0.0));
    }));
    REQUIRE(fixture.TickTimes(1, 0.01));
    CHECK(NearVector(fixture.LocationOf(camera.Value()), 10.0 - 300.0, 0.0, 0.0));
    // 최대 거리 20: 거의 안 따라가도 시작에서 20 이내로 자른다.
    CHECK(world.With<SpringArmComponent>(arm.Value(), [](SpringArmComponent& value) noexcept {
        return value.SetCameraLagMaxDistance(20.0);
    }));
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeLocation(Vec3d(1000.0, 0.0, 0.0));
    }));
    REQUIRE(fixture.TickTimes(1, 0.001));
    CHECK(NearVector(fixture.LocationOf(camera.Value()), 980.0 - 300.0, 0.0, 0.0));
    // 회전 지연: yaw 90°로 돌리고 0.01초 → QInterpTo 10% = yaw 9°.
    CHECK(world.With<SpringArmComponent>(arm.Value(), [](SpringArmComponent& value) noexcept {
        value.SetEnableCameraLag(false);
        value.SetEnableCameraRotationLag(true);
        return value.SetCameraLagMaxDistance(0.0);
    }));
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion());
    }));
    REQUIRE(fixture.TickTimes(1, 0.01));
    CHECK(Near(fixture.RotationOf(camera.Value()).yaw, 9.0, 1.0e-6));
    // substep(0.05초 > 1/60): 세 번에 나눠 따라가 한 번보다 더 많이 돈다.
    REQUIRE(fixture.TickTimes(1, 0.05));
    // 1/60초 세 번, 목표가 27°씩 간다: 9 → 13.5 → 21.75 → 33.125(QInterpTo 1/6씩).
    CHECK(Near(fixture.RotationOf(camera.Value()).yaw, 33.125, 1.0e-6));
    CHECK(world.With<SpringArmComponent>(arm.Value(), [](SpringArmComponent& value) noexcept {
        CHECK(ErrorOf(value.SetCameraLagSpeed(1001.0)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(value.SetCameraLagMaxTimeStep(0.001)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(value.SetCameraLagMaxDistance(-1.0)) == ErrorCode::InvalidArgument);
    }));
    REQUIRE(world.Shutdown());
}

// 26.7절: 지연을 처음부터 켜도 시작 때 기준값을 다시 채워 원점에서 쓸려 오지 않는다, 위치 지연
// substep.
TEST(Movement_SpringArmLagSeededAtStart) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle root;
    const ObjectHandle actor = fixture.SpawnWithRoot(root);
    REQUIRE(!actor.IsNull());
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeLocation(Vec3d(500.0, 0.0, 0.0));
    }));
    auto arm = world.AddComponent<SpringArmComponent>(actor);
    auto camera = world.AddComponent<SceneComponent>(actor);
    REQUIRE(arm && camera);
    CHECK(world.With<SpringArmComponent>(arm.Value(), [&](SpringArmComponent& value) noexcept {
        value.SetEnableCameraLag(true);
        value.SetEnableCameraRotationLag(true);
        return value.AttachTo(root);
    }));
    CHECK(world.With<SceneComponent>(camera.Value(), [&](SceneComponent& value) noexcept {
        return value.AttachTo(arm.Value());
    }));
    REQUIRE(world.BeginPlay());
    REQUIRE(fixture.TickTimes(1, 0.01));
    CHECK(NearVector(fixture.LocationOf(camera.Value()), 200.0, 0.0, 0.0));
    // 위치 지연 substep(0.05초 > 1/60): 세 번 1/6씩, 목표는 100/3씩 간다.
    CHECK(world.With<SceneComponent>(root, [](SceneComponent& value) noexcept {
        return value.SetRelativeLocation(Vec3d(600.0, 0.0, 0.0));
    }));
    REQUIRE(fixture.TickTimes(1, 0.05));
    double expected = 500.0;
    double target = 500.0;
    for (int index = 0; index < 3; ++index) {
        target += 100.0 / 3.0;
        expected += (target - expected) / 6.0;
    }

    CHECK(Near(fixture.LocationOf(camera.Value()).x, expected - 300.0, 1.0e-6));
    REQUIRE(world.Shutdown());
}

// 26.8절: 리플렉션 형태와 저장 왕복.
TEST(Movement_ReflectionAndSaveRoundTrip) {
    Fixture fixture(WorldKind::Editor);
    REQUIRE(fixture.registered);
    struct Expected {
        TypeId id;
        TypeId base;
        usize fields;
    };

    // 필드 수는 상속분 포함(Component 1, SceneComponent 10). Movement 9, Rotating 3, Projectile 20,
    // PathMover 9, SpringArm 16.
    const Expected expected[] = {
        {ObjectTypeIdOf<MovementComponent>(), ObjectTypeIdOf<Component>(), 10},
        {ObjectTypeIdOf<RotatingMovementComponent>(), ObjectTypeIdOf<MovementComponent>(), 13},
        {ObjectTypeIdOf<ProjectileMovementComponent>(), ObjectTypeIdOf<MovementComponent>(), 30},
        {ObjectTypeIdOf<PathMoverComponent>(), ObjectTypeIdOf<MovementComponent>(), 19},
        {ObjectTypeIdOf<SpringArmComponent>(), ObjectTypeIdOf<SceneComponent>(), 26},
    };
    for (const Expected& entry : expected) {
        auto lease = fixture.types.Acquire(entry.id);
        REQUIRE(lease);
        const TypeSchema& schema = lease.Value().Schema();
        CHECK(HasFlag(schema.flags, TypeFlags::WorldOnly));
        CHECK(schema.baseType == entry.base);
        CHECK(schema.fields.Size() == entry.fields);
    }

    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto level = world.CreateLevel("Moving");
    REQUIRE(level);
    ObjectHandle root;
    const ObjectHandle actor = fixture.SpawnWithRoot(root, level.Value());
    REQUIRE(!actor.IsNull());
    auto spin = world.AddComponent<RotatingMovementComponent>(actor);
    auto path = world.AddComponent<PathMoverComponent>(actor);
    REQUIRE(spin && path);
    CHECK(world.With<RotatingMovementComponent>(
        spin.Value(), [](RotatingMovementComponent& m) noexcept {
            return m.SetRotationRate(Rotator<double>(10.0, 20.0, 30.0));
        }));
    CHECK(world.With<PathMoverComponent>(path.Value(), [](PathMoverComponent& m) noexcept {
        CHECK(m.AddControlPoint(Vec3d(1.0, 2.0, 3.0), false));
        CHECK(m.SetBehaviour(PathMoverBehaviour::PingPong));
        return m.SetDuration(4.0f);
    }));

    Array<uint8> saved(fixture.allocator);
    {
        auto bytes =
            SaveObjectGraph(fixture.allocator, world.Objects(), world.Access(), level.Value());
        REQUIRE(bytes);
        saved = Move(bytes.Value());
    }

    {
        ObjectRegistry loaded(fixture.allocator, fixture.types, {0x4d4f5645ull, 2});
        const auto access = loaded.OwnerAccess();
        auto rootObject =
            LoadObjectGraph(fixture.allocator, loaded, fixture.types, access, saved.View());
        REQUIRE(rootObject);
        ObjectHandle loadedActor;
        {
            auto borrowed = loaded.Resolve<Level>(access, rootObject.Value().Handle());
            REQUIRE(borrowed);
            Level* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr && pointer->Actors().Size() == 1);
            loadedActor = pointer->Actors()[0];
        }

        auto borrowedActor = loaded.Resolve<Actor>(access, loadedActor);
        REQUIRE(borrowedActor);
        Actor* owner = Get(borrowedActor.Value());
        REQUIRE(owner != nullptr && owner->Components().Size() == 3);
        int found = 0;
        for (usize index = 0; index < 3; ++index) {
            const ObjectHandle component = owner->Components()[index];
            if (auto rotating = loaded.Resolve<RotatingMovementComponent>(access, component)) {
                RotatingMovementComponent* value = Get(rotating.Value());
                REQUIRE(value != nullptr);
                CHECK(Near(value->RotationRate().roll, 30.0));
                ++found;
            } else if (auto mover = loaded.Resolve<PathMoverComponent>(access, component)) {
                PathMoverComponent* value = Get(mover.Value());
                REQUIRE(value != nullptr);
                CHECK(value->ControlPoints().Size() == 1);
                CHECK(NearVector(value->ControlPoints()[0].position, 1.0, 2.0, 3.0));
                CHECK(!value->ControlPoints()[0].positionIsRelative);
                CHECK(value->Behaviour() == PathMoverBehaviour::PingPong);
                CHECK(value->Duration() == 4.0f);
                ++found;
            }
        }

        CHECK(found == 2);
    }

    REQUIRE(world.Shutdown());
}

} // namespace world_movement_component_tests_file

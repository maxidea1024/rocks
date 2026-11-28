// Physics 모듈 PH5d 컴포넌트 시험이다(Docs/Design/PhysicsModule 2.7절, 7.1.19절): componentWake·
// componentSleep 발행(generateWakeEvents일 때만, 결과 반영 때), WakeRigidBody·PutRigidBodyToSleep·
// RigidBodyIsAwake, componentPhysicsStateChanged Created·Destroyed(등록·충돌 켬 재생성·Actor 파괴).
// World·컴포넌트 공개 API만 쓰므로 Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

using namespace rocks;
namespace physics_sleep_tests_file {

using namespace physics_test_support;

namespace {

// 떠 있는 body가 잠들기에 넉넉한 스텝 수다(Jolt 기본 mTimeBeforeSleep 0.5 s = 30 스텝 + 여유 —
// 고정 스텝이라 시간 의존 없음, 이 시험은 Jolt를 모르는 공개 API만 쓴다).
constexpr int SleepSteps = 45;

// 컴포넌트마다 받은 이벤트 수다(정적 — lambda가 잡지 않는다).
struct Received {
    static inline ObjectHandle watched;
    static inline int wakes = 0;
    static inline int sleeps = 0;
    static inline int created = 0;
    static inline int destroyed = 0;

    static void Reset(ObjectHandle component) noexcept {
        watched = component;
        wakes = 0;
        sleeps = 0;
        created = 0;
        destroyed = 0;
    }
};

// 물리 상태 통지 안에서 충돌을 끄는 구다(handler의 다시 만들기가 동작하는지 — 7.1.19절). 상태
// bit가 확정된 뒤 통지되는지도 기록한다.
class StateProbeSphere final : public SphereComponent {
    ROCKS_NATIVE_TYPE(StateProbeSphere, SphereComponent, 0x7d3a91c5e2b04f68ull,
                      0x2c8e5f1b6a947d03ull)

public:
    explicit StateProbeSphere(Allocator& allocator) noexcept : SphereComponent(allocator) {
    }

    static inline bool disableOnCreate = false;
    static inline int consistent = 0;
    static inline int notices = 0;

protected:
    void OnSystemStateChanged(SystemStateKind kind, bool created) noexcept override {
        SphereComponent::OnSystemStateChanged(kind, created);
        if (kind != SystemStateKind::Physics) {
            return;
        }

        ++notices;
        if (HasSystemState(kind) == created && IsRegistered()) {
            ++consistent;
        }

        if (created && disableOnCreate) {
            disableOnCreate = false;
            (void)SetCollisionEnabled(CollisionEnabled::NoCollision);
        }
    }
};

bool StartWorld(BodyWorldFixture& fixture) noexcept {
    return fixture.registered && fixture.world && fixture.world->Initialize() &&
           fixture.world->BeginPlay();
}

void Subscribe(PrimitiveComponent& primitive) noexcept {
    (void)primitive.componentWake.TryAddLambda(
        [](WeakObjectRef<PrimitiveComponent> component) noexcept {
            if (component.Handle() == Received::watched) {
                ++Received::wakes;
            }
        });
    (void)primitive.componentSleep.TryAddLambda(
        [](WeakObjectRef<PrimitiveComponent> component) noexcept {
            if (component.Handle() == Received::watched) {
                ++Received::sleeps;
            }
        });
    (void)primitive.componentPhysicsStateChanged.TryAddLambda(
        [](WeakObjectRef<PrimitiveComponent>, ComponentPhysicsStateChange change) noexcept {
            if (change == ComponentPhysicsStateChange::Created) {
                ++Received::created;
            } else {
                ++Received::destroyed;
            }
        });
}

// 중력 없이 떠 있는 시뮬레이션 구다(generate면 깨움·잠듦 이벤트를 낸다).
ObjectHandle SpawnFloating(BodyWorldFixture& fixture, double x, bool generate) noexcept {
    return fixture.SpawnShape<SphereComponent>([&](SphereComponent& sphere) noexcept {
        (void)sphere.SetSphereRadius(static_cast<float>(SphereRadius));
        (void)sphere.SetRelativeLocation(Vec3d(x, 0.0, DropHeight));
        sphere.SetSimulatePhysics(true);
        sphere.SetEnableGravity(false);
        sphere.SetGenerateWakeEvents(generate);
        Subscribe(sphere);
    });
}

} // namespace

// 떠 있는 구가 잠들면 componentSleep, 힘으로 깨우면 componentWake다(결과 반영 때). WakeRigidBody·
// PutRigidBodyToSleep은 이벤트가 없다. generateWakeEvents가 거짓이면 이벤트가 없다.
TEST(PhysicsSleep_WakeAndSleepEvents) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    Received::Reset({});
    const ObjectHandle ball = SpawnFloating(fixture, 0.0, true);
    const ObjectHandle quiet = SpawnFloating(fixture, SideOffset, false);
    REQUIRE(!ball.IsNull() && !quiet.IsNull());
    Received::Reset(ball);
    fixture.Step(SleepSteps);
    CHECK(Received::sleeps == 1);
    CHECK(Received::wakes == 0);
    bool awake = true;
    REQUIRE(Apply<PrimitiveComponent>(world, ball, [&](PrimitiveComponent& sphere) noexcept {
        awake = sphere.RigidBodyIsAwake();
        sphere.AddForce(Vec3d::Zero());
    }));
    CHECK(!awake);
    fixture.Step(1);
    CHECK(Received::wakes == 1);

    REQUIRE(Apply<PrimitiveComponent>(world, ball, [&](PrimitiveComponent& sphere) noexcept {
        sphere.PutRigidBodyToSleep();
        awake = sphere.RigidBodyIsAwake();
        sphere.WakeRigidBody();
    }));
    CHECK(!awake);
    fixture.Step(1);
    CHECK(Received::wakes == 1);
    CHECK(Received::sleeps == 1);

    Received::Reset(quiet);
    REQUIRE(Apply<PrimitiveComponent>(
        world, quiet, [](PrimitiveComponent& sphere) noexcept { sphere.AddForce(Vec3d::Zero()); }));
    fixture.Step(SleepSteps);
    CHECK(Received::sleeps == 0);
    CHECK(Received::wakes == 0);
}

// RigidBodyIsAwake는 Dynamic만 참이다(Kinematic·body 없음 거짓).
TEST(PhysicsSleep_IsAwakeOnlyForDynamic) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle kinematic =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& sphere) noexcept {
            (void)sphere.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
        });
    REQUIRE(!kinematic.IsNull());
    REQUIRE(Apply<PrimitiveComponent>(world, kinematic, [](PrimitiveComponent& sphere) noexcept {
        CHECK(!sphere.RigidBodyIsAwake());
        sphere.WakeRigidBody();
        CHECK(!sphere.RigidBodyIsAwake());
    }));
}

// 물리 상태 이벤트: 등록이 Created, 충돌 끔(NoCollision)이 Destroyed, 다시 켜면 Created, Actor
// 파괴가 Destroyed다.
TEST(PhysicsSleep_PhysicsStateChangedEvents) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    Received::Reset({});
    const ObjectHandle ball = SpawnFloating(fixture, 0.0, false);
    REQUIRE(!ball.IsNull());
    CHECK(Received::created == 1);
    CHECK(Received::destroyed == 0);
    REQUIRE(Apply<PrimitiveComponent>(world, ball, [](PrimitiveComponent& sphere) noexcept {
        (void)sphere.SetCollisionEnabled(CollisionEnabled::NoCollision);
    }));
    CHECK(Received::destroyed == 1);
    REQUIRE(Apply<PrimitiveComponent>(world, ball, [](PrimitiveComponent& sphere) noexcept {
        (void)sphere.SetCollisionEnabled(CollisionEnabled::QueryAndPhysics);
    }));
    CHECK(Received::created == 2);

    ObjectHandle owner;
    REQUIRE(Apply<PrimitiveComponent>(
        world, ball, [&](PrimitiveComponent& sphere) noexcept { owner = sphere.GetOwner(); }));
    REQUIRE(world.DestroyActor(owner));
    fixture.Step(1);
    CHECK(Received::destroyed == 2);
}

// 물리 상태 통지는 상태 bit·등록이 확정된 뒤 온다: 등록 통지 안에서 충돌을 끄면 body가 바로
// 지워진다(다시 만들기가 Busy·무시되지 않는다).
TEST(PhysicsSleep_StateNoticeAllowsRecreate) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    auto token = RegisterNativeClass<StateProbeSphere>(fixture.types);
    REQUIRE(token);
    StateProbeSphere::disableOnCreate = true;
    StateProbeSphere::consistent = 0;
    StateProbeSphere::notices = 0;
    const ObjectHandle probe =
        fixture.SpawnShape<StateProbeSphere>([](StateProbeSphere& sphere) noexcept {
            (void)sphere.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
        });
    REQUIRE(!probe.IsNull());
    CHECK(fixture.Body(probe).IsNull());
    CHECK(fixture.BodyCount() == 0u);
    // Created(등록) → Destroyed(통지 안의 충돌 끔) 둘 다 확정 뒤다.
    CHECK(StateProbeSphere::notices == 2);
    CHECK(StateProbeSphere::consistent == 2);
    (void)fixture.world->Shutdown();
    CHECK(fixture.types.Unregister(token.Value()).IsOk());
}

} // namespace physics_sleep_tests_file

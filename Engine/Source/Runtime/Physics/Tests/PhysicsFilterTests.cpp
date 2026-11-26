// Physics 모듈 PH3a 시험이다(Docs/Design/PhysicsModule 6.1–6.2절, 7.1.6절): 응답 묶음 표와
// 시뮬레이션 쌍 필터 — 두 body가 서로의 object type에 대한 응답의 최소가 Block일 때만 접촉한다.
// World·컴포넌트 공개 API만 쓰므로 Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

using namespace rocks;
namespace physics_filter_tests_file {

using namespace physics_test_support;

namespace {

// 바닥 밑으로 빠졌다고 볼 높이(cm — 바닥 아랫면보다 아래).
constexpr double FellThroughHeight = -FloorHalfThickness * 2.0;

Result<void> SetResponse(World& world, ObjectHandle component, CollisionChannel channel,
                         CollisionResponse response) noexcept {
    return world.With<PrimitiveComponent>(component, [&](PrimitiveComponent& primitive) noexcept {
        return primitive.SetCollisionResponseToChannel(channel, response);
    });
}

// Pawn object type의 구를 떨어뜨린다.
ObjectHandle DropPawnBall(BodyWorldFixture& fixture, double x, double z) noexcept {
    return fixture.SpawnShape<SphereComponent>([x, z](SphereComponent& shape) noexcept {
        (void)shape.SetSphereRadius(static_cast<float>(SphereRadius));
        shape.SetSimulatePhysics(true);
        (void)shape.SetCollisionObjectType(CollisionChannel::Pawn);
        (void)shape.SetRelativeLocation(Vec3d(x, 0.0, z));
    });
}

// Pawn object type의 상자(반폭 SphereRadius)를 떨어뜨린다(쌓아도 구르지 않는다).
ObjectHandle DropPawnBox(BodyWorldFixture& fixture, double z) noexcept {
    return fixture.SpawnShape<BoxComponent>([z](BoxComponent& shape) noexcept {
        (void)shape.SetBoxExtent(Vec3d(SphereRadius, SphereRadius, SphereRadius));
        shape.SetSimulatePhysics(true);
        (void)shape.SetCollisionObjectType(CollisionChannel::Pawn);
        (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, z));
    });
}

bool StartWorld(BodyWorldFixture& fixture) noexcept {
    return fixture.registered && fixture.world && fixture.world->Initialize() &&
           fixture.world->BeginPlay();
}

} // namespace

// 바닥의 Pawn 응답이 Ignore·Overlap이면 Pawn 구가 빠지고 Block이면 선다(시뮬레이션은 Block만 —
// 6.2절 3단계).
TEST(PhysicsFilter_FloorResponseDecidesContact) {
    const CollisionResponse responses[] = {CollisionResponse::Ignore, CollisionResponse::Overlap,
                                           CollisionResponse::Block};
    for (const CollisionResponse response : responses) {
        BodyWorldFixture fixture;
        REQUIRE(StartWorld(fixture));
        World& world = *fixture.world;
        const ObjectHandle floor = fixture.SpawnFloor();
        REQUIRE(!floor.IsNull());
        REQUIRE(SetResponse(world, floor, CollisionChannel::Pawn, response));
        const ObjectHandle ball = DropPawnBall(fixture, 0.0, DropHeight);
        REQUIRE(!ball.IsNull());
        fixture.Step(SettleSteps);
        const double z = fixture.Kinematics(ball).location.z;
        if (response == CollisionResponse::Block) {
            CHECK(Near(z, SphereRadius, RestHeightTolerance));
        } else {
            CHECK(z < FellThroughHeight);
        }
    }
}

// 응답은 양쪽의 최소다: 바닥이 Block이어도 구가 바닥 object type(WorldStatic)을 Ignore하면 빠진다.
TEST(PhysicsFilter_EitherSideIgnoreSkipsContact) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    REQUIRE(!fixture.SpawnFloor().IsNull());
    const ObjectHandle ball = DropPawnBall(fixture, 0.0, DropHeight);
    REQUIRE(!ball.IsNull());
    REQUIRE(SetResponse(world, ball, CollisionChannel::WorldStatic, CollisionResponse::Ignore));
    fixture.Step(SettleSteps);
    CHECK(fixture.Kinematics(ball).location.z < FellThroughHeight);
}

// 떨어지는 중에 바꾼 응답은 다음 step부터 따른다(SetBodyCollision이 층의 묶음 색인을 바꾼다).
TEST(PhysicsFilter_ResponseChangeAppliesNextStep) {
    constexpr int BeforeLanding = 10;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle floor = fixture.SpawnFloor();
    REQUIRE(!floor.IsNull());
    REQUIRE(SetResponse(world, floor, CollisionChannel::Pawn, CollisionResponse::Ignore));
    const ObjectHandle landing = DropPawnBall(fixture, -SideOffset, DropHeight);
    REQUIRE(!landing.IsNull());
    fixture.Step(BeforeLanding);
    REQUIRE(fixture.Kinematics(landing).location.z > SphereRadius);
    REQUIRE(SetResponse(world, floor, CollisionChannel::Pawn, CollisionResponse::Block));
    fixture.Step(SettleSteps);
    CHECK(Near(fixture.Kinematics(landing).location.z, SphereRadius, RestHeightTolerance));

    const ObjectHandle falling = DropPawnBall(fixture, SideOffset, DropHeight);
    REQUIRE(!falling.IsNull());
    fixture.Step(BeforeLanding);
    REQUIRE(fixture.Kinematics(falling).location.z > SphereRadius);
    REQUIRE(SetResponse(world, falling, CollisionChannel::WorldStatic, CollisionResponse::Ignore));
    fixture.Step(SettleSteps);
    CHECK(fixture.Kinematics(falling).location.z < FellThroughHeight);
}

// 이미 닿아 있는 쌍도 응답을 바꾸면 다음 step부터 따른다(Jolt 접촉 캐시도 쌍 필터를 다시 거친다).
// 잠든 구는 바닥 응답만 바꾸면 그대로 있고(UE도 Static을 깨워 봐야 효과가 없다), 자기 응답을
// 바꾸면 깨어서 빠진다(UE — 충돌 필터가 바뀌면 body를 깨운다).
TEST(PhysicsFilter_TouchingAndSleepingBodies) {
    constexpr int Touching = 10;
    {
        BodyWorldFixture fixture;
        REQUIRE(StartWorld(fixture));
        World& world = *fixture.world;
        const ObjectHandle floor = fixture.SpawnFloor();
        const ObjectHandle ball = DropPawnBall(fixture, 0.0, SphereRadius);
        REQUIRE(!floor.IsNull() && !ball.IsNull());
        fixture.Step(Touching);
        REQUIRE(fixture.Kinematics(ball).awake);
        REQUIRE(Near(fixture.Kinematics(ball).location.z, SphereRadius, RestHeightTolerance));
        REQUIRE(SetResponse(world, floor, CollisionChannel::Pawn, CollisionResponse::Ignore));
        fixture.Step(SettleSteps);
        CHECK(fixture.Kinematics(ball).location.z < FellThroughHeight);
    }

    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle floor = fixture.SpawnFloor();
    const ObjectHandle ball = DropPawnBall(fixture, 0.0, SphereRadius);
    REQUIRE(!floor.IsNull() && !ball.IsNull());
    fixture.Step(SettleSteps);
    REQUIRE(!fixture.Kinematics(ball).awake);
    REQUIRE(SetResponse(world, floor, CollisionChannel::Pawn, CollisionResponse::Ignore));
    fixture.Step(Touching);
    const BodyKinematics resting = fixture.Kinematics(ball);
    CHECK(!resting.awake);
    CHECK(Near(resting.location.z, SphereRadius, RestHeightTolerance));
    REQUIRE(SetResponse(world, ball, CollisionChannel::WorldStatic, CollisionResponse::Ignore));
    CHECK(fixture.Kinematics(ball).awake);
    fixture.Step(SettleSteps);
    CHECK(fixture.Kinematics(ball).location.z < FellThroughHeight);
}

// 같은 응답의 body는 묶음 하나를 공유한다. 바꾸면 새 묶음이고, 마지막 사용자가 놓으면 칸이
// 사라진다. 컴포넌트 기본 응답은 Gizmo 채널이 Ignore(UE 기본)라 늘 있는 칸 0(모두 Block)이 아닌
// 칸 하나다.
TEST(PhysicsFilter_ResponseSetsAreShared) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    auto sets = [&]() noexcept { return world.GetPhysicsScene()->Stats().responseSets; };
    const ObjectHandle first = DropPawnBall(fixture, -SideOffset, DropHeight);
    const ObjectHandle second = DropPawnBall(fixture, SideOffset, DropHeight);
    REQUIRE(!first.IsNull() && !second.IsNull());
    auto disable = [&](ObjectHandle component) noexcept {
        REQUIRE(
            world.With<PrimitiveComponent>(component, [](PrimitiveComponent& primitive) noexcept {
                return primitive.SetCollisionEnabled(CollisionEnabled::NoCollision);
            }));
        CHECK(fixture.Body(component).IsNull());
    };

    // 칸 0 + 두 구가 공유하는 기본 응답 칸.
    CHECK(sets() == 2u);
    REQUIRE(SetResponse(world, first, CollisionChannel::Visibility, CollisionResponse::Ignore));
    CHECK(sets() == 3u);
    // 둘째도 같은 응답이 되면 첫째의 칸을 공유하고, 아무도 안 쓰는 기본 칸은 사라진다.
    REQUIRE(SetResponse(world, second, CollisionChannel::Visibility, CollisionResponse::Ignore));
    CHECK(sets() == 2u);
    disable(first);
    CHECK(sets() == 2u);
    disable(second);
    CHECK(sets() == 1u);

    // 놓인 칸을 다시 쓴 묶음은 옛 응답(Visibility Ignore)을 남기지 않는다: 바닥 묶음(Pawn Ignore)과
    // 구의 기본 묶음이 빈 칸을 쓰고, 구는 바닥을 지나 빠진다.
    const ObjectHandle floor = fixture.SpawnFloor();
    REQUIRE(!floor.IsNull());
    REQUIRE(SetResponse(world, floor, CollisionChannel::Pawn, CollisionResponse::Ignore));
    const ObjectHandle ball = DropPawnBall(fixture, 0.0, DropHeight);
    REQUIRE(!ball.IsNull());
    CHECK(sets() == 3u);
    fixture.Step(SettleSteps);
    CHECK(fixture.Kinematics(ball).location.z < FellThroughHeight);
}

// Dynamic 대 Dynamic도 같은 규칙이다: 위 상자가 Pawn을 Ignore하면 바닥의 Pawn 상자를 지나 바닥에
// 서고(두 상자가 겹친다), Block이면 그 위에 얹힌다.
TEST(PhysicsFilter_DynamicPairsFollowResponses) {
    const CollisionResponse responses[] = {CollisionResponse::Ignore, CollisionResponse::Block};
    for (const CollisionResponse response : responses) {
        BodyWorldFixture fixture;
        REQUIRE(StartWorld(fixture));
        World& world = *fixture.world;
        REQUIRE(!fixture.SpawnFloor().IsNull());
        const ObjectHandle lower = DropPawnBox(fixture, SphereRadius);
        const ObjectHandle upper = DropPawnBox(fixture, DropHeight);
        REQUIRE(!lower.IsNull() && !upper.IsNull());
        REQUIRE(SetResponse(world, upper, CollisionChannel::Pawn, response));
        fixture.Step(SettleSteps);
        const double z = fixture.Kinematics(upper).location.z;
        const double expected =
            response == CollisionResponse::Block ? SphereRadius * 3.0 : SphereRadius;
        CHECK(Near(z, expected, RestHeightTolerance));
    }
}

} // namespace physics_filter_tests_file

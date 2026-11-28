// Physics 모듈 PH6a 컴포넌트 시험이다(Docs/Design/PhysicsModule 7.1.21절): 원소 둘을 주는
// 컴포넌트의 body·컴포넌트 sweep(원소마다 — UE ComponentSweepMultiByChannel), scale 변경의 compound
// 다시 만들기, IsZeroExtent(UE — Box·Sphere·Capsule). World·컴포넌트 공개 API만 쓰므로 Modular
// 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <World/Physics/QueryFilter.h>
#include <World/Physics/WorldCollision.h>

using namespace rocks;
namespace physics_compound_tests_file {

using namespace physics_test_support;

namespace {

constexpr double TwinRadius = 10.0;
constexpr double TwinUpperZ = 100.0;
constexpr double TargetHalf = 20.0;
constexpr double TargetX = 200.0;
constexpr double SweepLength = 500.0;
constexpr double TraceTop = 1000.0;
constexpr double ScaleUp = 2.0;
constexpr double DistanceTolerance = 1.0e-2; // cm

// 반경 TwinRadius 구 원소 둘(중심 0, 중심 +Z TwinUpperZ)을 주는 컴포넌트다.
class TwinSphere final : public SphereComponent {
    ROCKS_NATIVE_TYPE(TwinSphere, SphereComponent, 0x3e91a7c4d25b6f08ull, 0x6a0d4c2e9b17f853ull)

public:
    explicit TwinSphere(Allocator& allocator) noexcept : SphereComponent(allocator) {
    }

    Result<void> BuildBodyGeometry(BodyGeometry& geometry) const noexcept override {
        BodyShapeElement upper = BodyShapeElement::Sphere(TwinRadius);
        upper.center = Vec3d(0.0, 0.0, TwinUpperZ);
        if (auto added = geometry.Add(BodyShapeElement::Sphere(TwinRadius)); !added) {
            return added;
        }

        return geometry.Add(upper);
    }
};

bool StartWorld(BodyWorldFixture& fixture) noexcept {
    return fixture.registered && fixture.world && fixture.world->Initialize() &&
           fixture.world->BeginPlay();
}

} // namespace

// 원소 둘 컴포넌트의 sweep은 위 원소가 그 높이의 표적에 닿는다(UE는 형상마다 쓴다). scale을
// 바꾸면 compound를 다시 만든다(위 원소 중심 × scale).
TEST(PhysicsCompound_ComponentSweepAndScale) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    auto token = RegisterNativeClass<TwinSphere>(fixture.types);
    REQUIRE(token);
    const ObjectHandle twin = fixture.SpawnShape<TwinSphere>([](TwinSphere&) noexcept {});
    REQUIRE(!twin.IsNull());
    REQUIRE(!fixture.Body(twin).IsNull());
    const ObjectHandle target = fixture.SpawnShape<BoxComponent>([](BoxComponent& box) noexcept {
        box.SetMobility(ComponentMobility::Static);
        (void)box.SetBoxExtent(Vec3d(TargetHalf, TargetHalf, TargetHalf));
        (void)box.SetRelativeLocation(Vec3d(TargetX, 0.0, TwinUpperZ));
    });
    REQUIRE(!target.IsNull());

    Array<HitResult> hits(fixture.allocator);
    auto traced = ComponentTraceAll(*fixture.world, twin, Vec3d::Zero(),
                                    Vec3d(SweepLength, 0.0, 0.0), Quatd::Identity(), {}, hits);
    REQUIRE(traced);
    REQUIRE(hits.Size() == 1u);
    CHECK(hits[0].component == target);
    CHECK(math::Abs(hits[0].distance - (TargetX - TargetHalf - TwinRadius)) <= DistanceTolerance);

    // scale 2: 위 원소 중심 200, 반경 20 — 위 끝 220.
    REQUIRE(fixture.world
                ->With<TwinSphere>(twin,
                                   [](TwinSphere& sphere) noexcept {
                                       return sphere.SetRelativeScale(
                                           Vec3d(ScaleUp, ScaleUp, ScaleUp));
                                   })
                .IsOk());
    HitResult top;
    const QueryFilter filter =
        QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::WorldStatic));
    REQUIRE(
        LineTraceFirst(*fixture.world, Vec3d(0.0, 0.0, TraceTop), Vec3d::Zero(), filter, {}, top));
    CHECK(top.component == twin);
    CHECK(top.elementIndex == 1);
    CHECK(math::Abs(top.impactPoint.z - (TwinUpperZ + TwinRadius) * ScaleUp) <= DistanceTolerance);
    (void)fixture.world->Shutdown();
    CHECK(fixture.types.Unregister(token.Value()).IsOk());
}

// IsZeroExtent는 UE와 같다: 기본 거짓, Box extent 0, Sphere 반경 0, Capsule 반경·반높이 모두 0.
TEST(PhysicsCompound_IsZeroExtent) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    const ObjectHandle box = fixture.SpawnShape<BoxComponent>(
        [](BoxComponent& shape) noexcept { (void)shape.SetBoxExtent(Vec3d::Zero()); });
    const ObjectHandle sphere = fixture.SpawnShape<SphereComponent>(
        [](SphereComponent& shape) noexcept { (void)shape.SetSphereRadius(0.0f); });
    const ObjectHandle capsule = fixture.SpawnShape<CapsuleComponent>(
        [](CapsuleComponent& shape) noexcept { (void)shape.SetCapsuleSize(0.0f, 0.0f); });
    const ObjectHandle radiusOnly = fixture.SpawnShape<CapsuleComponent>(
        [](CapsuleComponent& shape) noexcept { (void)shape.SetCapsuleSize(0.0f, TwinRadius); });
    const ObjectHandle normal =
        fixture.SpawnShape<SphereComponent>([](SphereComponent&) noexcept {});
    auto zero = [&](ObjectHandle component) noexcept {
        bool result = false;
        (void)fixture.world->With<PrimitiveComponent>(component,
                                                      [&](PrimitiveComponent& primitive) noexcept {
                                                          result = primitive.IsZeroExtent();
                                                          return Result<void>::Success();
                                                      });
        return result;
    };

    CHECK(zero(box));
    CHECK(zero(sphere));
    CHECK(zero(capsule));
    CHECK(!zero(radiusOnly));
    CHECK(!zero(normal));
}

// 크기 0 컴포넌트의 sweep은 line trace다(UE ComponentSweepMultiByChannel): 자기 컴포넌트만 빼고
// 같은 Actor의 다른 컴포넌트는 맞는다(형상 sweep은 자기 Actor 전체를 뺀다).
TEST(PhysicsCompound_ZeroExtentSweepIsLineTrace) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle zeroBox;
    ObjectHandle sibling;
    const bool spawned = fixture.SpawnAttached<BoxComponent, BoxComponent>(
        zeroBox, sibling,
        [](BoxComponent& shape) noexcept { (void)shape.SetBoxExtent(Vec3d::Zero()); },
        [](BoxComponent& shape) noexcept {
            (void)shape.SetBoxExtent(Vec3d(TargetHalf, TargetHalf, TargetHalf));
            (void)shape.SetRelativeLocation(Vec3d(TargetX, 0.0, 0.0));
        });
    REQUIRE(spawned);
    Array<HitResult> hits(fixture.allocator);
    auto traced = ComponentTraceAll(*fixture.world, zeroBox, Vec3d::Zero(),
                                    Vec3d(SweepLength, 0.0, 0.0), Quatd::Identity(), {}, hits);
    REQUIRE(traced);
    REQUIRE(hits.Size() == 1u);
    CHECK(hits[0].component == sibling);
    CHECK(math::Abs(hits[0].distance - (TargetX - TargetHalf)) <= DistanceTolerance);
}

} // namespace physics_compound_tests_file

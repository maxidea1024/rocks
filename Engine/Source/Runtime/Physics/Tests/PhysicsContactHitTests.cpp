// Physics 모듈 PH4c 시험이다(Docs/Design/PhysicsModule 5.4절·6.4절, 7.1.12절): 시뮬레이션 접촉
// hit(notifyRigidBodyCollision을 켠 쪽만, 스텝마다 따로, 법선·충격량은 받는 쪽 기준), 실행 중 표지
// 바꾸기, probe(반작용 없음·충격량 0 hit), 응답 Overlap 쌍은 접촉 없음, 발행 중 파괴.
// World·컴포넌트 공개 API만 쓰므로 Modular 시험에서도 돈다(문턱·CCD는 PhysicsContactSceneTests).
#include "PhysicsBodyFixture.h"

using namespace rocks;
namespace physics_contact_hit_tests_file {

using namespace physics_test_support;

namespace {

constexpr int MaxRecords = 64;
// 바닥에 닿기까지 넉넉한 스텝 수다(200 cm 낙하 ≈ 0.64 s).
constexpr int MaxFallSteps = 90;
// 닿은 뒤 쉬는 접촉을 보는 스텝 수다(Jolt 잠듦 0.5 s보다 짧게).
constexpr int RestingSteps = 6;
// Jolt 기본 밀도(kg/m³ — UE 기본 1 g/cm³와 같다)로 반경 20 cm 구의 질량(kg)이다.
constexpr double JoltDefaultDensity = 1000.0;
constexpr double CentimetersPerMeter = 100.0;
// 쉬는 접촉 충격량(m·g·dt)의 상대 허용 오차다(감쇠·위치 보정 몫).
constexpr double RestingImpulseTolerance = 0.05;
// 법선 성분 허용 오차다.
constexpr double NormalTolerance = 1.0e-3;
// probe 구가 바닥을 다 지나간 높이(cm)다.
constexpr double BelowFloor = -3.0 * FloorHalfThickness;
constexpr int ProbeFallSteps = 150;

double SphereMass(double radiusCentimeters) noexcept {
    const double radius = radiusCentimeters / CentimetersPerMeter;
    return 4.0 / 3.0 * math::Pi<double> * radius * radius * radius * JoltDefaultDensity;
}

// 쉬는 구가 한 스텝에 받는 충격량(kg·cm/s)이다: m·|g|·dt.
double RestingImpulse() noexcept {
    return SphereMass(SphereRadius) * -GravityZ * StepSeconds;
}

// OnHit 기록 하나다.
struct HitRecord {
    ObjectHandle actor;
    ObjectHandle myComponent;
    ObjectHandle otherActor;
    ObjectHandle otherComponent;
    bool selfMoved = false;
    Vec3d impulse;
    HitResult hit;
};

// OnHit를 기록하는 Actor다(기록은 시험 하나 안에서만 쓴다).
class HitRecordingActor final : public Actor {
    ROCKS_NATIVE_TYPE(HitRecordingActor, Actor, 0x6c1f9a3e52d8b047ull, 0xa47e0d2b9c53f618ull)

public:
    explicit HitRecordingActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    void OnHit(ObjectHandle myComponent, ObjectHandle otherActor, ObjectHandle otherComponent,
               bool selfMoved, const Vec3d& normalImpulse, const HitResult& hit) noexcept override {
        if (count < MaxRecords) {
            records[count] = {Handle(),  myComponent,   otherActor, otherComponent,
                              selfMoved, normalImpulse, hit};
        }

        ++count;
        // 발행 중 다른 Actor를 파괴하는 hook이다(한 번만).
        if (Handle() == hookActor && !destroyTarget.IsNull()) {
            (void)GetWorld().DestroyActor(destroyTarget);
            destroyTarget = {};
        }
    }

    static inline HitRecord records[MaxRecords];
    static inline int count = 0;
    static inline ObjectHandle hookActor;
    static inline ObjectHandle destroyTarget;

    static void Reset() noexcept {
        count = 0;
        hookActor = {};
        destroyTarget = {};
    }

    // actor가 받은 hit 수다.
    static int CountFor(ObjectHandle actor) noexcept {
        int found = 0;
        for (int index = 0; index < count && index < MaxRecords; ++index) {
            if (records[index].actor == actor) {
                ++found;
            }
        }

        return found;
    }

    // actor가 받은 마지막 hit다(없으면 nullptr).
    static const HitRecord* LastFor(ObjectHandle actor) noexcept {
        for (int index = (count < MaxRecords ? count : MaxRecords) - 1; index >= 0; --index) {
            if (records[index].actor == actor) {
                return &records[index];
            }
        }

        return nullptr;
    }
};

// 기록 Actor 클래스를 등록하고 시작한 물리 World다.
struct ContactWorld {
    ContactWorld() noexcept {
        auto registeredToken = RegisterNativeClass<HitRecordingActor>(fixture.types);
        registered = registeredToken.IsOk();
        if (registeredToken) {
            token = registeredToken.Value();
        }

        HitRecordingActor::Reset();
        started = fixture.registered && registered && fixture.world &&
                  fixture.world->Initialize() && fixture.world->BeginPlay();
    }

    ~ContactWorld() {
        HitRecordingActor::Reset();
        if (fixture.world) {
            (void)fixture.world->Shutdown();
        }

        if (registered) {
            CHECK(fixture.types.Unregister(token).IsOk());
        }
    }

    // 윗면이 z = 0인 정적 바닥이다.
    ObjectHandle Floor(bool notify) noexcept {
        return fixture.SpawnShapeOn<HitRecordingActor, BoxComponent>(
            [notify](BoxComponent& floor) noexcept {
                floor.SetMobility(ComponentMobility::Static);
                (void)floor.SetBoxExtent(
                    Vec3d(FloorHalfExtent, FloorHalfExtent, FloorHalfThickness));
                (void)floor.SetRelativeLocation(Vec3d(0.0, 0.0, -FloorHalfThickness));
                floor.SetNotifyRigidBodyCollision(notify);
            });
    }

    // DropHeight에서 떨어지는 구다. configure는 등록 전에 부른다.
    template <class Configure>
    ObjectHandle Ball(bool notify, Configure&& configure) noexcept {
        return fixture.SpawnShapeOn<HitRecordingActor, SphereComponent>(
            [&](SphereComponent& ball) noexcept {
                (void)ball.SetSphereRadius(static_cast<float>(SphereRadius));
                ball.SetSimulatePhysics(true);
                (void)ball.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
                ball.SetNotifyRigidBodyCollision(notify);
                configure(ball);
            });
    }

    ObjectHandle Ball(bool notify) noexcept {
        return Ball(notify, [](SphereComponent&) noexcept {});
    }

    ObjectHandle Owner(ObjectHandle component) noexcept {
        auto owner = fixture.world->With<Component>(
            component, [](Component& value) noexcept { return value.GetOwner(); });
        return owner ? owner.Value() : ObjectHandle{};
    }

    // 기록이 생길 때까지 한 스텝씩 간다. 돌려주는 값은 기록이 생겼는지다.
    bool StepUntilHit(int maxSteps) noexcept {
        for (int step = 0; step < maxSteps; ++step) {
            fixture.Step(1);
            if (HitRecordingActor::count != 0) {
                return true;
            }
        }

        return false;
    }

    // 구가 닿을 때까지 떨어뜨린다(표지가 없으면 바닥 근처 높이까지).
    void FallUntilNear(ObjectHandle ball) noexcept {
        for (int step = 0; step < MaxFallSteps; ++step) {
            fixture.Step(1);
            if (fixture.ComponentPose(ball).translation.z <= SphereRadius + RestHeightTolerance) {
                return;
            }
        }
    }

    bool SetNotify(ObjectHandle component, bool notify) noexcept {
        return Apply<PrimitiveComponent>(*fixture.world, component,
                                         [notify](PrimitiveComponent& primitive) noexcept {
                                             primitive.SetNotifyRigidBodyCollision(notify);
                                         });
    }

    BodyWorldFixture fixture;
    TypeRegistrationToken token;
    bool registered = false;
    bool started = false;
};

} // namespace

// 표지 켠 구가 바닥에 닿으면 구 Actor가 hit를 받는다: 상대는 바닥, 법선은 +Z(바닥에서 구 쪽),
// 충격량은 +Z, 접촉점은 바닥 면, selfMoved 참. 쉬는 동안 스텝마다 하나씩이고 충격량은 m·g·dt다.
TEST(PhysicsContactHit_FallingSphereHitsFloor) {
    ContactWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle floor = scene.Floor(false);
    const ObjectHandle ball = scene.Ball(true);
    REQUIRE(!floor.IsNull() && !ball.IsNull());
    const ObjectHandle ballActor = scene.Owner(ball);
    const ObjectHandle floorActor = scene.Owner(floor);

    REQUIRE(scene.StepUntilHit(MaxFallSteps));
    const HitRecord& first = HitRecordingActor::records[0];
    CHECK(first.actor == ballActor);
    CHECK(first.myComponent == ball);
    CHECK(first.otherActor == floorActor);
    CHECK(first.otherComponent == floor);
    CHECK(first.selfMoved);
    CHECK(first.hit.blockingHit);
    CHECK(first.hit.component == floor);
    CHECK(first.hit.actor == floorActor);
    CHECK(Near(first.hit.normal.z, 1.0, NormalTolerance));
    CHECK(first.hit.impactNormal == first.hit.normal);
    CHECK(first.hit.impactPoint == first.hit.location);
    CHECK(Near(first.hit.location.z, 0.0, RestHeightTolerance));
    CHECK(first.hit.penetrationDepth >= 0.0);
    CHECK(first.impulse.z > 0.0);
    CHECK(HitRecordingActor::CountFor(floorActor) == 0);

    // 쉬는 접촉: 스텝마다 hit 하나, 충격량 m·g·dt(+Z).
    for (int step = 0; step < RestingSteps; ++step) {
        HitRecordingActor::Reset();
        scene.fixture.Step(1);
        CHECK(HitRecordingActor::count == 1);
    }

    const HitRecord* resting = HitRecordingActor::LastFor(ballActor);
    REQUIRE(resting != nullptr);
    CHECK(Near(resting->impulse.z, RestingImpulse(), RestingImpulse() * RestingImpulseTolerance));
    CHECK(Near(resting->impulse.x, 0.0, RestingImpulse() * RestingImpulseTolerance));
    CHECK(Near(resting->impulse.y, 0.0, RestingImpulse() * RestingImpulseTolerance));
}

// 바닥 쪽만 켜면 바닥 Actor가 받는다(법선 −Z — 구에서 바닥 쪽, 충격량 −Z). 둘 다 켜면 둘 다
// 받는다(칸 번호가 작은 쪽 — 먼저 만든 바닥 — 먼저).
TEST(PhysicsContactHit_EachNotifyingSideGetsItsOwnHit) {
    {
        ContactWorld scene;
        REQUIRE(scene.started);
        const ObjectHandle floor = scene.Floor(true);
        const ObjectHandle ball = scene.Ball(false);
        REQUIRE(!floor.IsNull() && !ball.IsNull());
        REQUIRE(scene.StepUntilHit(MaxFallSteps));
        const HitRecord& first = HitRecordingActor::records[0];
        CHECK(first.actor == scene.Owner(floor));
        CHECK(first.myComponent == floor);
        CHECK(first.otherComponent == ball);
        CHECK(first.otherActor == scene.Owner(ball));
        CHECK(Near(first.hit.normal.z, -1.0, NormalTolerance));
        CHECK(first.impulse.z < 0.0);
        CHECK(HitRecordingActor::CountFor(scene.Owner(ball)) == 0);
    }

    ContactWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle floor = scene.Floor(true);
    const ObjectHandle ball = scene.Ball(true);
    REQUIRE(!floor.IsNull() && !ball.IsNull());
    REQUIRE(scene.StepUntilHit(MaxFallSteps));
    REQUIRE(HitRecordingActor::count == 2);
    const HitRecord* records = HitRecordingActor::records;
    CHECK(records[0].actor == scene.Owner(floor));
    CHECK(records[1].actor == scene.Owner(ball));
    // 같은 접촉의 두 쪽이다: 법선·충격량이 반대, 접촉점은 같다.
    CHECK(records[0].hit.normal == records[1].hit.normal * -1.0);
    CHECK(records[0].impulse == records[1].impulse * -1.0);
    CHECK(records[0].hit.location == records[1].hit.location);
}

// 표지가 없으면 hit가 없고, 실행 중 켜면 다음 step부터, 끄면 다시 없다(UE
// SetNotifyRigidBodyCollision).
TEST(PhysicsContactHit_NotifyFlagTogglesAtRuntime) {
    ContactWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Floor(false).IsNull());
    const ObjectHandle ball = scene.Ball(false);
    REQUIRE(!ball.IsNull());
    scene.FallUntilNear(ball);
    scene.fixture.Step(1);
    CHECK(HitRecordingActor::count == 0);

    REQUIRE(scene.SetNotify(ball, true));
    scene.fixture.Step(1);
    CHECK(HitRecordingActor::count == 1);

    REQUIRE(scene.SetNotify(ball, false));
    HitRecordingActor::Reset();
    scene.fixture.Step(1);
    CHECK(HitRecordingActor::count == 0);
}

// 한 Tick에 고정 스텝이 둘이면 hit도 둘이다(UE key는 쌍과 solver 시간 — 스텝마다 따로).
TEST(PhysicsContactHit_EachFixedStepReportsSeparately) {
    ContactWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Floor(false).IsNull());
    const ObjectHandle ball = scene.Ball(true);
    REQUIRE(!ball.IsNull());
    REQUIRE(scene.StepUntilHit(MaxFallSteps));
    scene.fixture.Step(1);

    HitRecordingActor::Reset();
    CHECK(scene.fixture.world->Tick(StepSeconds * 2.0));
    REQUIRE(HitRecordingActor::count == 2);
    const HitRecord* records = HitRecordingActor::records;
    CHECK(Near(records[0].impulse.z, RestingImpulse(), RestingImpulse() * RestingImpulseTolerance));
    CHECK(Near(records[1].impulse.z, RestingImpulse(), RestingImpulse() * RestingImpulseTolerance));
}

// probe(ProbeOnly) 구는 바닥을 지나가며(반작용 없음 — 6.4절) 충격량 0 hit를 받는다.
TEST(PhysicsContactHit_ProbePassesThroughWithZeroImpulse) {
    ContactWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle floor = scene.Floor(false);
    const ObjectHandle ball = scene.Ball(true, [](SphereComponent& shape) noexcept {
        (void)shape.SetCollisionEnabled(CollisionEnabled::ProbeOnly);
    });
    REQUIRE(!floor.IsNull() && !ball.IsNull());
    scene.fixture.Step(ProbeFallSteps);

    CHECK(scene.fixture.ComponentPose(ball).translation.z < BelowFloor);
    REQUIRE(HitRecordingActor::count > 0);
    const int recorded =
        (HitRecordingActor::count < MaxRecords ? HitRecordingActor::count : MaxRecords);
    for (int index = 0; index < recorded; ++index) {
        const HitRecord& record = HitRecordingActor::records[index];
        CHECK(record.otherComponent == floor);
        CHECK(record.impulse == Vec3d::Zero());
        // 실제로 겹친 접촉만이다(speculative 거리의 떨어진 접촉은 내지 않는다).
        CHECK(record.hit.penetrationDepth > 0.0);
    }
}

// 같은 스텝의 여러 쌍은 칸 번호 순으로 발행한다(바닥·구 A 쌍이 바닥·구 B 쌍보다 먼저).
TEST(PhysicsContactHit_PairsDispatchInSlotOrder) {
    ContactWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Floor(false).IsNull());
    const ObjectHandle first = scene.Ball(true);
    const ObjectHandle second = scene.Ball(true, [](SphereComponent& shape) noexcept {
        (void)shape.SetRelativeLocation(Vec3d(SideOffset, 0.0, DropHeight));
    });
    REQUIRE(!first.IsNull() && !second.IsNull());
    REQUIRE(scene.StepUntilHit(MaxFallSteps));
    // 같은 높이에서 떨어져 같은 스텝에 닿는다.
    REQUIRE(HitRecordingActor::count == 2);
    CHECK(HitRecordingActor::records[0].myComponent == first);
    CHECK(HitRecordingActor::records[1].myComponent == second);
}

// 바닥 채널 응답이 Overlap이면 시뮬레이션 접촉이 없다(지나가고 hit도 없다 — 6.2절).
TEST(PhysicsContactHit_OverlapResponseMakesNoContact) {
    ContactWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Floor(true).IsNull());
    const ObjectHandle ball = scene.Ball(true, [](SphereComponent& shape) noexcept {
        (void)shape.SetCollisionResponseToAllChannels(CollisionResponse::Overlap);
    });
    REQUIRE(!ball.IsNull());
    scene.fixture.Step(ProbeFallSteps);

    CHECK(scene.fixture.ComponentPose(ball).translation.z < BelowFloor);
    CHECK(HitRecordingActor::count == 0);
}

// 발행 중 hook이 다음 hit의 Actor를 파괴하면 그 Actor는 받지 않는다(파괴 대기 — UE
// IsActorValidToNotify).
TEST(PhysicsContactHit_DestroyDuringDispatchSkipsDestroyedActor) {
    ContactWorld scene;
    REQUIRE(scene.started);
    // 구를 먼저 만들어 칸 번호가 작게 한다(구 hit가 먼저).
    const ObjectHandle ball = scene.Ball(true);
    const ObjectHandle floor = scene.Floor(true);
    REQUIRE(!ball.IsNull() && !floor.IsNull());
    const ObjectHandle ballActor = scene.Owner(ball);
    const ObjectHandle floorActor = scene.Owner(floor);
    HitRecordingActor::hookActor = ballActor;
    HitRecordingActor::destroyTarget = floorActor;

    REQUIRE(scene.StepUntilHit(MaxFallSteps));
    CHECK(HitRecordingActor::CountFor(ballActor) == 1);
    CHECK(HitRecordingActor::CountFor(floorActor) == 0);
    CHECK(HitRecordingActor::destroyTarget.IsNull());
    // 바닥이 사라진 뒤에도 step·발행이 안전하다.
    scene.fixture.Step(RestingSteps);
    CHECK(HitRecordingActor::CountFor(floorActor) == 0);
}

} // namespace physics_contact_hit_tests_file

// Physics 모듈 PH4b 시험이다(Docs/Design/PhysicsModule 5.3절, 7.1.11절): PrimitiveComponent의
// overlap 장부(대칭 기록·시작·끝), Actor 알림(OnBeginOverlap·OnEndOverlap — 자기 → 상대, 같은 Actor
// 제외, Actor 쌍 하나에 한 번), 갱신하는 때(sweep 대기 겹침·위치 setter·결과 반영·Actor 시작·충돌
// 설정 변경·파괴·표지 끄기), 겹침이 아닌 경우(Block·표지 거짓), 발행 중 파괴. World·컴포넌트 공개
// API만 쓰므로 Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <World/Physics/MoveOptions.h>

using namespace rocks;
namespace physics_overlap_event_tests_file {

using namespace physics_test_support;

namespace {

constexpr double BoxHalf = 10.0;
constexpr double TriggerX = 100.0;
constexpr double MoveDistance = 200.0;
// 겹침 상자를 지나 끝에서 겹치는 이동 거리다(이동 상자 중심이 상자 중심에 닿는다).
constexpr double IntoTrigger = TriggerX;
// 서로 겹치지 않는 먼 자리다.
constexpr double FarX = 1000.0;

// overlap 알림 기록 하나다.
struct OverlapRecord {
    ObjectHandle actor;
    ObjectHandle other;
    bool begin = false;
};

constexpr int MaxRecords = 16;

// OnBeginOverlap·OnEndOverlap을 기록하는 Actor다(기록은 시험 하나 안에서만 쓴다).
class OverlapRecordingActor final : public Actor {
    ROCKS_NATIVE_TYPE(OverlapRecordingActor, Actor, 0x3d9a6e1c7f24b850ull, 0x8b15c4e0a7d3f962ull)

public:
    explicit OverlapRecordingActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    void OnBeginOverlap(ObjectHandle other) noexcept override {
        Record(other, true);
        if (Handle() != hookActor) {
            return;
        }

        // 발행 중 장부·배치를 바꾸는 hook이다(장부가 망가지지 않아야 한다). 한 번만 한다.
        World& world = GetWorld();
        if (!destroyTarget.IsNull()) {
            (void)world.DestroyComponent(destroyTarget);
            destroyTarget = {};
        }

        if (!disableTarget.IsNull()) {
            (void)world.With<PrimitiveComponent>(
                disableTarget, [](PrimitiveComponent& primitive) noexcept {
                    return primitive.SetCollisionEnabled(CollisionEnabled::NoCollision);
                });
            disableTarget = {};
        }

        if (!teleportTarget.IsNull()) {
            (void)world.With<SceneComponent>(teleportTarget, [](SceneComponent& scene) noexcept {
                const Vec3d at = scene.WorldLocation();
                return scene.SetWorldLocationAndRotation(Vec3d(at.x, at.y, teleportZ),
                                                         scene.WorldQuaternion());
            });
            teleportTarget = {};
        }

        if (destroySelf) {
            (void)world.DestroyActor(Handle());
            destroySelf = false;
        }
    }

    void OnEndOverlap(ObjectHandle other) noexcept override {
        Record(other, false);
    }

    void Record(ObjectHandle other, bool begin) noexcept {
        if (count < MaxRecords) {
            records[count] = {Handle(), other, begin};
        }

        ++count;
    }

    static inline OverlapRecord records[MaxRecords];
    static inline int count = 0;
    // hookActor가 시작 알림을 받을 때 할 일이다.
    static inline ObjectHandle hookActor;
    static inline ObjectHandle destroyTarget;
    static inline ObjectHandle disableTarget;
    static inline ObjectHandle teleportTarget;
    static inline double teleportZ = 0.0;
    static inline bool destroySelf = false;

    static void Reset() noexcept {
        count = 0;
        hookActor = {};
        destroyTarget = {};
        disableTarget = {};
        teleportTarget = {};
        teleportZ = 0.0;
        destroySelf = false;
    }

    // actor가 other에 대해 받은 시작(begin) 또는 끝 알림 수다.
    static int Count(ObjectHandle actor, ObjectHandle other, bool begin) noexcept {
        int found = 0;
        for (int index = 0; index < count && index < MaxRecords; ++index) {
            if (records[index].actor == actor && records[index].other == other &&
                records[index].begin == begin) {
                ++found;
            }
        }

        return found;
    }
};

// 기록 Actor 클래스를 등록한 물리 World다. 시작한 World에서 Spawn한 Actor는 다음 프레임 시작에
// Playing이 된다(Settle).
struct OverlapWorld {
    OverlapWorld() noexcept {
        auto registeredToken = RegisterNativeClass<OverlapRecordingActor>(fixture.types);
        registered = registeredToken.IsOk();
        if (registeredToken) {
            token = registeredToken.Value();
        }

        OverlapRecordingActor::Reset();
        started = fixture.registered && registered && fixture.world &&
                  fixture.world->Initialize() && fixture.world->BeginPlay();
    }

    ~OverlapWorld() {
        OverlapRecordingActor::Reset();
        if (fixture.world) {
            (void)fixture.world->Shutdown();
        }

        if (registered) {
            CHECK(fixture.types.Unregister(token).IsOk());
        }
    }

    // x에 반폭 BoxHalf 상자를 기록 Actor로 둔다(기본 응답 — 모두 Block).
    template <class Configure>
    ObjectHandle Box(double x, Configure&& configure) noexcept {
        return fixture.SpawnShapeOn<OverlapRecordingActor, BoxComponent>(
            [&](BoxComponent& box) noexcept {
                (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
                (void)box.SetRelativeLocation(Vec3d(x, 0.0, 0.0));
                configure(box);
            });
    }

    ObjectHandle Box(double x) noexcept {
        return Box(x, [](BoxComponent&) noexcept {});
    }

    // 모든 채널에 Overlap인 상자다(trigger).
    template <class Configure>
    ObjectHandle Trigger(double x, Configure&& configure) noexcept {
        return Box(x, [&](BoxComponent& box) noexcept {
            (void)box.SetCollisionResponseToAllChannels(CollisionResponse::Overlap);
            configure(box);
        });
    }

    ObjectHandle Trigger(double x) noexcept {
        return Trigger(x, [](BoxComponent&) noexcept {});
    }

    // 부모·자식 상자를 가진 기록 Actor다(자식 상대 x = childOffset). setRoot면 부모를 Actor 루트로
    // 정한다.
    bool Pair(double x, double childOffset, ObjectHandle& parent, ObjectHandle& child,
              bool setRoot = false) noexcept {
        World& world = *fixture.world;
        auto actor = world.SpawnActorDeferred<OverlapRecordingActor>();
        if (!actor) {
            return false;
        }

        auto made = world.AddComponent<BoxComponent>(actor.Value());
        auto attached = world.AddComponent<BoxComponent>(actor.Value());
        if (!made || !attached) {
            return false;
        }

        (void)world.With<BoxComponent>(made.Value(), [x](BoxComponent& box) noexcept {
            (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)box.SetRelativeLocation(Vec3d(x, 0.0, 0.0));
        });
        auto linked = world.With<BoxComponent>(attached.Value(), [&](BoxComponent& box) noexcept {
            (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)box.SetRelativeLocation(Vec3d(childOffset, 0.0, 0.0));
            return box.AttachTo(made.Value());
        });
        if (!linked) {
            return false;
        }

        if (setRoot) {
            auto rooted = world.With<Actor>(actor.Value(), [&](Actor& owner) noexcept {
                return owner.SetRootComponent(made.Value());
            });
            if (!rooted) {
                return false;
            }
        }

        if (!world.FinishSpawning(actor.Value())) {
            return false;
        }

        parent = made.Value();
        child = attached.Value();
        return true;
    }

    // Spawn한 Actor를 Playing으로 만든다(한 프레임).
    void Settle() noexcept {
        fixture.Step(1);
    }

    // 이동 무시 Actor를 더한다.
    bool IgnoreActor(ObjectHandle component, ObjectHandle actor) noexcept {
        auto ignored = fixture.world->With<PrimitiveComponent>(
            component, [actor](PrimitiveComponent& primitive) noexcept {
                return primitive.IgnoreActorWhenMoving(actor, true);
            });
        return ignored.IsOk();
    }

    Result<bool> Sweep(ObjectHandle component, const Vec3d& delta) noexcept {
        Result<bool> moved = Result<bool>::Failure(ErrorCode::NotFound);
        (void)fixture.world->With<PrimitiveComponent>(
            component, [&](PrimitiveComponent& primitive) noexcept {
                moved =
                    primitive.MoveComponent(delta, primitive.WorldQuaternion(), true, nullptr, {});
                return Result<void>::Success();
            });
        return moved;
    }

    // sweep 없이 루트를 옮긴다(위치 setter).
    bool Place(ObjectHandle component, double x) noexcept {
        auto placed =
            fixture.world->With<SceneComponent>(component, [x](SceneComponent& scene) noexcept {
                return scene.SetRelativeLocation(Vec3d(x, 0.0, 0.0));
            });
        return placed.IsOk();
    }

    ObjectHandle Owner(ObjectHandle component) noexcept {
        auto owner = fixture.world->With<Component>(
            component, [](Component& value) noexcept { return value.GetOwner(); });
        return owner ? owner.Value() : ObjectHandle{};
    }

    bool Overlapping(ObjectHandle component, ObjectHandle other) noexcept {
        auto found = fixture.world->With<PrimitiveComponent>(
            component, [other](PrimitiveComponent& primitive) noexcept {
                return primitive.IsOverlappingComponent(other);
            });
        return found && found.Value();
    }

    usize LedgerSize(ObjectHandle component) noexcept {
        auto size = fixture.world->With<PrimitiveComponent>(
            component, [](PrimitiveComponent& primitive) noexcept {
                return primitive.OverlappingComponents().Size();
            });
        return size ? size.Value() : 0;
    }

    template <class T, class Call>
    bool Edit(ObjectHandle component, Call&& call) noexcept {
        return Apply<T>(*fixture.world, component, static_cast<Call&&>(call));
    }

    BodyWorldFixture fixture;
    TypeRegistrationToken token;
    bool registered = false;
    bool started = false;
};

} // namespace

// sweep이 겹침 상자를 지나 끝에서 겹치지 않으면 시작·끝이 한 번씩이다(자기 → 상대 순서).
TEST(PhysicsOverlapEvent_SweepThroughBeginsAndEnds) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.Settle();
    REQUIRE(OverlapRecordingActor::count == 0);
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);

    auto moved = scene.Sweep(mover, Vec3d(MoveDistance, 0.0, 0.0));
    REQUIRE(moved);
    CHECK(moved.Value());
    REQUIRE(OverlapRecordingActor::count == 4);
    const OverlapRecord* records = OverlapRecordingActor::records;
    CHECK((records[0].actor == moverActor && records[0].other == triggerActor && records[0].begin));
    CHECK((records[1].actor == triggerActor && records[1].other == moverActor && records[1].begin));
    CHECK(
        (records[2].actor == moverActor && records[2].other == triggerActor && !records[2].begin));
    CHECK(
        (records[3].actor == triggerActor && records[3].other == moverActor && !records[3].begin));
    CHECK(scene.LedgerSize(mover) == 0);
    CHECK(scene.LedgerSize(trigger) == 0);
}

// 끝에서 겹치면 시작만 남고 양쪽 장부가 대칭이다(Actor 단위 조회도 양쪽 참).
TEST(PhysicsOverlapEvent_SweepEndingInsideKeepsOverlap) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.Settle();
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);

    REQUIRE(scene.Sweep(mover, Vec3d(IntoTrigger, 0.0, 0.0)));
    CHECK(OverlapRecordingActor::count == 2);
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, true) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, true) == 1);
    CHECK(scene.Overlapping(mover, trigger));
    CHECK(scene.Overlapping(trigger, mover));
    CHECK(scene.LedgerSize(mover) == 1);
    CHECK(scene.LedgerSize(trigger) == 1);
    World& world = *scene.fixture.world;
    CHECK(PrimitiveComponent::IsOverlappingActor(world, moverActor, triggerActor));
    CHECK(PrimitiveComponent::IsOverlappingActor(world, triggerActor, moverActor));
    CHECK(!PrimitiveComponent::IsOverlappingActor(world, moverActor, ObjectHandle{}));

    // 겹친 채 조금 더 가면 아무 알림도 없다.
    REQUIRE(scene.Sweep(mover, Vec3d(BoxHalf * 0.5, 0.0, 0.0)));
    CHECK(OverlapRecordingActor::count == 2);
}

// sweep 없는 위치 setter도 갱신한다: 들어가면 시작, 나오면 끝.
TEST(PhysicsOverlapEvent_PlacementUpdatesOverlaps) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.Settle();
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);

    REQUIRE(scene.Place(mover, TriggerX));
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, true) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, true) == 1);
    CHECK(scene.Overlapping(trigger, mover));

    REQUIRE(scene.Place(mover, FarX));
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, false) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, false) == 1);
    CHECK(OverlapRecordingActor::count == 4);
    CHECK(scene.LedgerSize(trigger) == 0);
}

// 같은 Actor의 컴포넌트끼리는 장부에만 남고 Actor 알림이 없다(루트는 자기 Actor를 질의하지 않고
// 자식 쪽 갱신이 맡는다).
TEST(PhysicsOverlapEvent_SameActorComponentsDoNotNotify) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    ObjectHandle parent;
    ObjectHandle child;
    // 자식이 부모와 겹치는 자리다(반폭보다 가깝다).
    REQUIRE(scene.Pair(0.0, BoxHalf, parent, child));
    REQUIRE(scene.Edit<BoxComponent>(child, [](BoxComponent& box) noexcept {
        (void)box.SetCollisionResponseToAllChannels(CollisionResponse::Overlap);
    }));
    scene.Settle();

    CHECK(scene.Overlapping(child, parent));
    CHECK(scene.Overlapping(parent, child));
    CHECK(OverlapRecordingActor::count == 0);

    // 루트를 다시 갱신해도(위치 setter) 같은 Actor 겹침은 끝나지 않는다.
    REQUIRE(scene.Place(parent, BoxHalf * 0.25));
    CHECK(scene.Overlapping(parent, child));
    CHECK(OverlapRecordingActor::count == 0);
}

// 한 Actor의 두 컴포넌트가 같은 상대와 겹쳐도 Actor 시작·끝은 한 번씩이다.
TEST(PhysicsOverlapEvent_ActorPairNotifiesOnce) {
    constexpr double ChildOffset = BoxHalf * 3.0;
    constexpr double TriggerHalf = BoxHalf * 5.0;
    OverlapWorld scene;
    REQUIRE(scene.started);
    ObjectHandle parent;
    ObjectHandle child;
    REQUIRE(scene.Pair(0.0, ChildOffset, parent, child));
    const ObjectHandle trigger = scene.Trigger(FarX, [](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(TriggerHalf, TriggerHalf, TriggerHalf));
    });
    REQUIRE(!trigger.IsNull());
    scene.Settle();
    const ObjectHandle pairActor = scene.Owner(parent);
    const ObjectHandle triggerActor = scene.Owner(trigger);

    // 두 상자가 모두 trigger 안에 든다.
    REQUIRE(scene.Place(parent, FarX - ChildOffset * 0.5));
    CHECK(scene.Overlapping(trigger, parent));
    CHECK(scene.Overlapping(trigger, child));
    CHECK(scene.LedgerSize(trigger) == 2);
    CHECK(OverlapRecordingActor::Count(pairActor, triggerActor, true) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, pairActor, true) == 1);

    REQUIRE(scene.Place(parent, 0.0));
    CHECK(scene.LedgerSize(trigger) == 0);
    CHECK(OverlapRecordingActor::Count(pairActor, triggerActor, false) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, pairActor, false) == 1);
    CHECK(OverlapRecordingActor::count == 4);
}

// 응답이 Block이거나 한쪽 generateOverlapEvents가 거짓이면 겹침이 아니다.
TEST(PhysicsOverlapEvent_BlockOrDisabledIsNotOverlap) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle blocker = scene.Box(TriggerX);
    const ObjectHandle quietTrigger = scene.Trigger(
        -TriggerX, [](BoxComponent& box) noexcept { box.SetGenerateOverlapEvents(false); });
    const ObjectHandle trigger = scene.Trigger(TriggerX * 2.0);
    const ObjectHandle quietMover =
        scene.Box(FarX, [](BoxComponent& box) noexcept { box.SetGenerateOverlapEvents(false); });
    REQUIRE(!mover.IsNull() && !blocker.IsNull() && !quietTrigger.IsNull() && !trigger.IsNull() &&
            !quietMover.IsNull());
    scene.Settle();

    REQUIRE(scene.Place(mover, TriggerX));
    CHECK(scene.LedgerSize(mover) == 0);
    REQUIRE(scene.Place(mover, -TriggerX));
    CHECK(scene.LedgerSize(mover) == 0);
    CHECK(scene.LedgerSize(quietTrigger) == 0);
    REQUIRE(scene.Place(quietMover, TriggerX * 2.0));
    CHECK(scene.LedgerSize(trigger) == 0);
    CHECK(OverlapRecordingActor::count == 0);
}

// Actor가 Playing이 되기 전에는 겹침이 없고, 시작 때 이미 겹친 것을 시작한다.
TEST(PhysicsOverlapEvent_InitialOverlapsOnStart) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle trigger = scene.Trigger(0.0);
    const ObjectHandle mover = scene.Box(BoxHalf * 0.5);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    CHECK(scene.LedgerSize(trigger) == 0);
    CHECK(OverlapRecordingActor::count == 0);

    scene.Settle();
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);
    CHECK(scene.Overlapping(mover, trigger));
    CHECK(scene.Overlapping(trigger, mover));
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, true) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, true) == 1);
    CHECK(OverlapRecordingActor::count == 2);
}

// 물리 결과 반영(떨어지는 Dynamic 구)도 갱신한다.
TEST(PhysicsOverlapEvent_SimulatedBodyEntersTrigger) {
    constexpr double TriggerHalf = 40.0;
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle trigger = scene.Trigger(0.0, [](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(TriggerHalf, TriggerHalf, TriggerHalf));
    });
    const ObjectHandle ball = scene.fixture.SpawnShapeOn<OverlapRecordingActor, SphereComponent>(
        [](SphereComponent& sphere) noexcept {
            (void)sphere.SetSphereRadius(static_cast<float>(SphereRadius));
            (void)sphere.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight * 0.5));
            sphere.SetSimulatePhysics(true);
        });
    REQUIRE(!trigger.IsNull() && !ball.IsNull());
    scene.Settle();
    CHECK(!scene.Overlapping(trigger, ball));

    // 0.5 s면 약 120 cm 떨어져 trigger 안에 있다(trigger는 Overlap이라 막지 않는다).
    scene.fixture.Step(FallSteps);
    CHECK(scene.fixture.ComponentPose(ball).translation.z < TriggerHalf);
    CHECK(scene.Overlapping(trigger, ball));
    CHECK(scene.Overlapping(ball, trigger));
    CHECK(OverlapRecordingActor::Count(scene.Owner(ball), scene.Owner(trigger), true) == 1);
    CHECK(OverlapRecordingActor::Count(scene.Owner(trigger), scene.Owner(ball), true) == 1);
    CHECK(OverlapRecordingActor::count == 2);
}

// 결과 반영 중 시작 알림에서 구를 옮기면 body도 따라간다(결과 반영 표지가 hook에 새지 않는다).
TEST(PhysicsOverlapEvent_HookTeleportsSimulatedBody) {
    constexpr double TriggerHalf = 40.0;
    constexpr double TeleportHeight = DropHeight * 2.0;
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle trigger = scene.Trigger(0.0, [](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(TriggerHalf, TriggerHalf, TriggerHalf));
    });
    const ObjectHandle ball = scene.fixture.SpawnShapeOn<OverlapRecordingActor, SphereComponent>(
        [](SphereComponent& sphere) noexcept {
            (void)sphere.SetSphereRadius(static_cast<float>(SphereRadius));
            (void)sphere.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight * 0.5));
            sphere.SetSimulatePhysics(true);
        });
    REQUIRE(!trigger.IsNull() && !ball.IsNull());
    scene.Settle();
    OverlapRecordingActor::hookActor = scene.Owner(ball);
    OverlapRecordingActor::teleportTarget = ball;
    OverlapRecordingActor::teleportZ = TeleportHeight;

    scene.fixture.Step(FallSteps);
    REQUIRE(OverlapRecordingActor::teleportTarget.IsNull());
    const double componentZ = scene.fixture.ComponentPose(ball).translation.z;
    CHECK(componentZ > TriggerHalf);
    CHECK(Near(scene.fixture.Kinematics(ball).location.z, componentZ, PositionTolerance));
    CHECK(!scene.Overlapping(ball, trigger));
    CHECK(OverlapRecordingActor::Count(scene.Owner(ball), scene.Owner(trigger), false) == 1);
}

// 충돌을 Ignore로 바꾸거나 NoCollision으로 끄면 끝난다.
TEST(PhysicsOverlapEvent_CollisionChangeEndsOverlap) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(TriggerX);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    const ObjectHandle other = scene.Box(-TriggerX);
    const ObjectHandle secondTrigger = scene.Trigger(-TriggerX);
    REQUIRE(!mover.IsNull() && !trigger.IsNull() && !other.IsNull() && !secondTrigger.IsNull());
    scene.Settle();
    REQUIRE(scene.Overlapping(trigger, mover));
    REQUIRE(scene.Overlapping(secondTrigger, other));
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);

    REQUIRE(scene.Edit<BoxComponent>(trigger, [](BoxComponent& box) noexcept {
        CHECK(box.SetCollisionResponseToAllChannels(CollisionResponse::Ignore));
    }));
    CHECK(!scene.Overlapping(trigger, mover));
    CHECK(!scene.Overlapping(mover, trigger));
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, false) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, false) == 1);

    REQUIRE(scene.Edit<BoxComponent>(secondTrigger, [](BoxComponent& box) noexcept {
        CHECK(box.SetCollisionEnabled(CollisionEnabled::NoCollision));
    }));
    CHECK(scene.LedgerSize(secondTrigger) == 0);
    CHECK(scene.LedgerSize(other) == 0);
    CHECK(OverlapRecordingActor::Count(scene.Owner(other), scene.Owner(secondTrigger), false) == 1);
}

// 컴포넌트를 파괴하면 양쪽에 끝 알림, Actor를 파괴하면 남은 쪽만 끝 알림을 받는다.
TEST(PhysicsOverlapEvent_DestroyEndsOverlap) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(0.0);
    const ObjectHandle other = scene.Box(FarX);
    const ObjectHandle secondTrigger = scene.Trigger(FarX);
    REQUIRE(!mover.IsNull() && !trigger.IsNull() && !other.IsNull() && !secondTrigger.IsNull());
    scene.Settle();
    REQUIRE(scene.Overlapping(mover, trigger));
    REQUIRE(scene.Overlapping(other, secondTrigger));
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);
    const ObjectHandle otherActor = scene.Owner(other);
    const ObjectHandle secondActor = scene.Owner(secondTrigger);
    World& world = *scene.fixture.world;

    REQUIRE(world.DestroyComponent(trigger));
    scene.Settle();
    CHECK(scene.LedgerSize(mover) == 0);
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, false) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, false) == 1);

    REQUIRE(world.DestroyActor(secondActor));
    scene.Settle();
    CHECK(scene.LedgerSize(other) == 0);
    CHECK(OverlapRecordingActor::Count(otherActor, secondActor, false) == 1);
    // 파괴되는 Actor도 자기 정리 중에는 끝 알림을 받는다(UE DestroyActor는 겹침을 MarkAsGarbage
    // 전에 끝낸다 — PhysicsModule 7.1.14절).
    CHECK(OverlapRecordingActor::Count(secondActor, otherActor, false) == 1);
}

// generateOverlapEvents를 끄면 바로가 아니라 다음 갱신 때 끝난다.
TEST(PhysicsOverlapEvent_DisablingEventsEndsOnNextUpdate) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(0.0);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.Settle();
    REQUIRE(scene.Overlapping(mover, trigger));
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);

    REQUIRE(scene.Edit<BoxComponent>(
        mover, [](BoxComponent& box) noexcept { box.SetGenerateOverlapEvents(false); }));
    CHECK(scene.Overlapping(mover, trigger));
    REQUIRE(scene.Place(mover, BoxHalf * 0.25));
    CHECK(!scene.Overlapping(mover, trigger));
    CHECK(!scene.Overlapping(trigger, mover));
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, false) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, false) == 1);
}

// 시작 알림 안에서 상대 컴포넌트를 파괴해도 장부가 망가지지 않는다(끝 알림이 한 번 온다).
TEST(PhysicsOverlapEvent_DestroyDuringDispatch) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.Settle();
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);
    OverlapRecordingActor::hookActor = moverActor;
    OverlapRecordingActor::destroyTarget = trigger;

    REQUIRE(scene.Sweep(mover, Vec3d(IntoTrigger, 0.0, 0.0)));
    scene.Settle();
    REQUIRE(OverlapRecordingActor::destroyTarget.IsNull());
    CHECK(scene.LedgerSize(mover) == 0);
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, true) == 1);
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, false) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, true) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, false) == 1);
    CHECK(OverlapRecordingActor::count == 4);
}

// 시작 알림 안에서 상대의 충돌을 바로 끄면(장부가 즉시 바뀐다) 그 자리에서 끝나고 장부가 비어 있다.
TEST(PhysicsOverlapEvent_HookDisablesCollisionDuringDispatch) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.Settle();
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);
    OverlapRecordingActor::hookActor = moverActor;
    OverlapRecordingActor::disableTarget = trigger;

    REQUIRE(scene.Sweep(mover, Vec3d(IntoTrigger, 0.0, 0.0)));
    CHECK(OverlapRecordingActor::disableTarget.IsNull());
    CHECK(scene.LedgerSize(mover) == 0);
    CHECK(scene.LedgerSize(trigger) == 0);
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, true) == 1);
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, false) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, false) == 1);
    // 더 움직여도 다시 시작하지 않는다(trigger는 충돌이 없다).
    REQUIRE(scene.Sweep(mover, Vec3d(BoxHalf * 0.5, 0.0, 0.0)));
    CHECK(OverlapRecordingActor::Count(moverActor, triggerActor, true) == 1);
}

// 루트를 정한 Actor: 루트 질의는 자기 Actor를 빼고, 같은 Actor 겹침은 자식 쪽 갱신이 맡아 루트
// 갱신으로 끝나지 않는다. 바깥 trigger와는 Actor 알림이 한 번씩이다.
TEST(PhysicsOverlapEvent_RootComponentPath) {
    constexpr double TriggerHalf = BoxHalf * 5.0;
    OverlapWorld scene;
    REQUIRE(scene.started);
    ObjectHandle parent;
    ObjectHandle child;
    REQUIRE(scene.Pair(0.0, BoxHalf, parent, child, true));
    REQUIRE(scene.Edit<BoxComponent>(child, [](BoxComponent& box) noexcept {
        (void)box.SetCollisionResponseToAllChannels(CollisionResponse::Overlap);
    }));
    const ObjectHandle trigger = scene.Trigger(FarX, [](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(TriggerHalf, TriggerHalf, TriggerHalf));
    });
    REQUIRE(!trigger.IsNull());
    scene.Settle();
    const ObjectHandle pairActor = scene.Owner(parent);
    const ObjectHandle triggerActor = scene.Owner(trigger);
    REQUIRE(scene.Overlapping(child, parent));
    REQUIRE(scene.Overlapping(parent, child));

    REQUIRE(scene.Place(parent, FarX));
    CHECK(scene.Overlapping(parent, child));
    CHECK(scene.Overlapping(trigger, parent));
    CHECK(scene.Overlapping(trigger, child));
    CHECK(OverlapRecordingActor::Count(pairActor, triggerActor, true) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, pairActor, true) == 1);

    REQUIRE(scene.Place(parent, 0.0));
    CHECK(scene.Overlapping(parent, child));
    CHECK(scene.LedgerSize(trigger) == 0);
    CHECK(OverlapRecordingActor::Count(pairActor, triggerActor, false) == 1);
    CHECK(OverlapRecordingActor::count == 4);
}

// trigger를 지나 벽에 막히면 trigger는 시작·끝, 벽 너머 trigger는 막는 hit 뒤라 아무것도 없다.
TEST(PhysicsOverlapEvent_SweepStopsAtWallAfterTrigger) {
    constexpr double WallX = TriggerX + BoxHalf * 6.0;
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    const ObjectHandle wall = scene.Box(WallX);
    const ObjectHandle beyond = scene.Trigger(WallX + BoxHalf * 4.0);
    REQUIRE(!mover.IsNull() && !trigger.IsNull() && !wall.IsNull() && !beyond.IsNull());
    scene.Settle();
    const ObjectHandle moverActor = scene.Owner(mover);

    REQUIRE(scene.Sweep(mover, Vec3d(MoveDistance, 0.0, 0.0)));
    CHECK(scene.fixture.ComponentPose(mover).translation.x < WallX - BoxHalf * 2.0);
    CHECK(OverlapRecordingActor::Count(moverActor, scene.Owner(trigger), true) == 1);
    CHECK(OverlapRecordingActor::Count(moverActor, scene.Owner(trigger), false) == 1);
    CHECK(OverlapRecordingActor::Count(moverActor, scene.Owner(beyond), true) == 0);
    CHECK(scene.LedgerSize(beyond) == 0);
}

// 이동 무시 Actor는 overlap에도 적용된다(sweep 대기 겹침과 끝 위치 질의 모두).
TEST(PhysicsOverlapEvent_MoveIgnoreAppliesToOverlaps) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.Settle();
    REQUIRE(scene.IgnoreActor(mover, scene.Owner(trigger)));

    REQUIRE(scene.Sweep(mover, Vec3d(IntoTrigger, 0.0, 0.0)));
    CHECK(scene.LedgerSize(mover) == 0);
    REQUIRE(scene.Sweep(mover, Vec3d(IntoTrigger, 0.0, 0.0)));
    CHECK(OverlapRecordingActor::count == 0);
}

// 형상 크기를 바꾸면 갱신한다: 키우면 시작, 줄이면 끝.
TEST(PhysicsOverlapEvent_ShapeResizeUpdatesOverlaps) {
    constexpr double GrownHalf = TriggerX;
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.Settle();
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);

    REQUIRE(scene.Edit<BoxComponent>(trigger, [](BoxComponent& box) noexcept {
        CHECK(box.SetBoxExtent(Vec3d(GrownHalf, BoxHalf, BoxHalf)));
    }));
    CHECK(scene.Overlapping(trigger, mover));
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, true) == 1);

    REQUIRE(scene.Edit<BoxComponent>(trigger, [](BoxComponent& box) noexcept {
        CHECK(box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf)));
    }));
    CHECK(!scene.Overlapping(trigger, mover));
    CHECK(OverlapRecordingActor::Count(triggerActor, moverActor, false) == 1);
    CHECK(OverlapRecordingActor::count == 4);
}

// 다른 Actor의 컴포넌트에 붙은 Actor도 시작 때 초기 겹침을 계산한다(무기를 든 캐릭터).
TEST(PhysicsOverlapEvent_AttachedToOtherActorStartsOverlaps) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle holder = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(TriggerX);
    REQUIRE(!holder.IsNull() && !trigger.IsNull());
    scene.Settle();
    const ObjectHandle held =
        scene.Box(TriggerX, [holder](BoxComponent& box) noexcept { CHECK(box.AttachTo(holder)); });
    REQUIRE(!held.IsNull());
    REQUIRE(scene.fixture.Parent(held) == holder);
    scene.Settle();

    const ObjectHandle heldActor = scene.Owner(held);
    CHECK(scene.Overlapping(held, trigger));
    CHECK(OverlapRecordingActor::Count(heldActor, scene.Owner(trigger), true) == 1);
    CHECK(OverlapRecordingActor::Count(scene.Owner(holder), scene.Owner(trigger), true) == 0);
}

// 시작 때 겹친 픽업이 시작 알림에서 자기를 파괴해도 남은 쪽 장부가 비고 양쪽이 끝 알림을 받는다.
TEST(PhysicsOverlapEvent_SelfDestroyOnStart) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle trigger = scene.Trigger(0.0);
    REQUIRE(!trigger.IsNull());
    scene.Settle();
    const ObjectHandle pickup = scene.Box(BoxHalf * 0.5);
    REQUIRE(!pickup.IsNull());
    const ObjectHandle pickupActor = scene.Owner(pickup);
    const ObjectHandle triggerActor = scene.Owner(trigger);
    OverlapRecordingActor::hookActor = pickupActor;
    OverlapRecordingActor::destroySelf = true;

    scene.Settle();
    CHECK(!OverlapRecordingActor::destroySelf);
    scene.Settle();
    CHECK(scene.LedgerSize(trigger) == 0);
    CHECK(OverlapRecordingActor::Count(triggerActor, pickupActor, true) == 1);
    CHECK(OverlapRecordingActor::Count(triggerActor, pickupActor, false) == 1);
    // 파괴되는 픽업도 자기 정리 중에는 끝 알림을 받는다(UE — PhysicsModule 7.1.14절).
    CHECK(OverlapRecordingActor::Count(pickupActor, triggerActor, false) == 1);
    auto state = scene.fixture.world->GetActorState(pickupActor);
    CHECK((!state || state.Value() == ActorState::PendingDestroy));
}

// World가 Playing이 아니면(EndPlay 정리) 끝 알림을 보내지 않는다.
TEST(PhysicsOverlapEvent_EndPlayDoesNotNotify) {
    OverlapWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle trigger = scene.Trigger(0.0);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.Settle();
    REQUIRE(OverlapRecordingActor::count == 2);

    REQUIRE(scene.fixture.world->EndPlay(EndPlayReason::Quit));
    CHECK(OverlapRecordingActor::count == 2);
}

} // namespace physics_overlap_event_tests_file

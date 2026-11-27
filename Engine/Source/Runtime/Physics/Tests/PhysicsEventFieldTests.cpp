// Physics 모듈 PH4e 시험이다(Docs/Design/PhysicsModule 5.5절, 7.1.14절): hit·overlap 이벤트
// 필드(componentHit·componentOverlapBegan·componentOverlapEnded, actorHit·actorOverlapBegan·
// actorOverlapEnded)의 발행 순서(가상 hook과의 순서 — UE), 인자(sweep 결과·뒤집은 hit·충격량),
// 같은 Actor 컴포넌트, 파괴·EndPlay 중 발행, HitResult 리플렉션 schema. World·컴포넌트 공개 API만
// 쓰므로 Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <Core/Text/StringView.h>
#include <World/Physics/HitResult.generated.h>

using namespace rocks;
namespace physics_event_field_tests_file {

using namespace physics_test_support;

namespace {

constexpr double BoxHalf = 10.0;
constexpr double WallX = 100.0;
constexpr double TriggerX = 100.0;
constexpr double MoveDistance = 200.0;
// 같은 Actor 두 상자가 겹치는 자식 offset이다.
constexpr double OverlappingChildOffset = 5.0;
constexpr int MaxFallSteps = 90;
constexpr int MaxEntries = 32;
constexpr double NormalTolerance = 1.0e-6;
// HitResult의 리플렉션 값 칸 수다(component·actor는 NATIVE).
constexpr usize HitResultReflectedFields = 12;

// 기록 종류다.
enum class EntryKind : uint8 {
    HitHook,
    BeginHook,
    EndHook,
    ActorHit,
    ActorBegan,
    ActorEnded,
    ComponentHit,
    ComponentBegan,
    ComponentEnded
};

// 기록 하나다: 발행자(Actor hook·Actor 이벤트는 Actor, 컴포넌트 이벤트는 컴포넌트)와 인자.
struct Entry {
    EntryKind kind = EntryKind::HitHook;
    ObjectHandle publisher;
    ObjectHandle otherActor;
    ObjectHandle otherComponent;
    bool fromSweep = false;
    Vec3d impulse;
    HitResult hit;
};

struct Log {
    static inline Entry entries[MaxEntries];
    static inline int count = 0;

    static void Add(const Entry& entry) noexcept {
        if (count < MaxEntries) {
            entries[count] = entry;
        }

        ++count;
    }

    static void Reset() noexcept {
        count = 0;
    }

    static int Count(EntryKind kind, ObjectHandle publisher) noexcept {
        int found = 0;
        for (int index = 0; index < count && index < MaxEntries; ++index) {
            if (entries[index].kind == kind && entries[index].publisher == publisher) {
                ++found;
            }
        }

        return found;
    }
};

// 가상 hook을 기록하는 Actor다.
class EventRecordingActor final : public Actor {
    ROCKS_NATIVE_TYPE(EventRecordingActor, Actor, 0x58c2e7a14f0b9d36ull, 0xd1f3a6b82e47c059ull)

public:
    explicit EventRecordingActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    void OnHit(ObjectHandle, ObjectHandle otherActor, ObjectHandle otherComponent, bool,
               const Vec3d& normalImpulse, const HitResult& hit) noexcept override {
        Log::Add(
            {EntryKind::HitHook, Handle(), otherActor, otherComponent, false, normalImpulse, hit});
        if (Handle() == destroySelfOnHit) {
            destroySelfOnHit = {};
            (void)GetWorld().DestroyActor(Handle());
        }
    }

    // 이 Actor가 OnHit에서 자기를 파괴한다(한 번 — 발행 중 파괴 시험).
    static inline ObjectHandle destroySelfOnHit;

    void OnBeginOverlap(ObjectHandle otherActor) noexcept override {
        Log::Add({EntryKind::BeginHook, Handle(), otherActor, {}, false, {}, {}});
    }

    void OnEndOverlap(ObjectHandle otherActor) noexcept override {
        Log::Add({EntryKind::EndHook, Handle(), otherActor, {}, false, {}, {}});
    }
};

// 기록 Actor 클래스를 등록하고 시작한 World다. 컴포넌트와 그 Actor의 이벤트를 구독한다.
struct EventWorld {
    EventWorld() noexcept {
        auto registeredToken = RegisterNativeClass<EventRecordingActor>(fixture.types);
        registered = registeredToken.IsOk();
        if (registeredToken) {
            token = registeredToken.Value();
        }

        Log::Reset();
        EventRecordingActor::destroySelfOnHit = {};
        started = fixture.registered && registered && fixture.world &&
                  fixture.world->Initialize() && fixture.world->BeginPlay();
    }

    ~EventWorld() {
        Log::Reset();
        if (fixture.world) {
            (void)fixture.world->Shutdown();
        }

        if (registered) {
            CHECK(fixture.types.Unregister(token).IsOk());
        }
    }

    template <class T, class Configure>
    ObjectHandle Spawn(Configure&& configure) noexcept {
        const ObjectHandle component =
            fixture.SpawnShapeOn<EventRecordingActor, T>(static_cast<Configure&&>(configure));
        if (!component.IsNull()) {
            Subscribe(component);
        }

        return component;
    }

    // x에 반폭 BoxHalf 상자다. overlap이면 모든 채널 Overlap(trigger)이다.
    ObjectHandle Box(double x, bool overlap) noexcept {
        return Spawn<BoxComponent>([x, overlap](BoxComponent& box) noexcept {
            (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)box.SetRelativeLocation(Vec3d(x, 0.0, 0.0));
            if (overlap) {
                (void)box.SetCollisionResponseToAllChannels(CollisionResponse::Overlap);
            }
        });
    }

    // 컴포넌트와 그 소유 Actor의 이벤트 여섯을 기록한다.
    void Subscribe(ObjectHandle component) noexcept {
        World& world = *fixture.world;
        const ObjectHandle owner = Owner(component);
        (void)world.With<PrimitiveComponent>(
            component, [&](PrimitiveComponent& primitive) noexcept {
                (void)primitive.componentHit.TryAddLambda(
                    [](WeakObjectRef<PrimitiveComponent> self, WeakObjectRef<Actor> otherActor,
                       WeakObjectRef<PrimitiveComponent> otherComponent, Vec3d impulse,
                       const HitResult& hit) noexcept {
                        Log::Add({EntryKind::ComponentHit, self.Handle(), otherActor.Handle(),
                                  otherComponent.Handle(), false, impulse, hit});
                    });
                (void)primitive.componentOverlapBegan.TryAddLambda(
                    [](WeakObjectRef<PrimitiveComponent> selfRef,
                       WeakObjectRef<Actor> otherActorRef,
                       WeakObjectRef<PrimitiveComponent> otherComponentRef, bool fromSweep,
                       const HitResult& hit) noexcept {
                        const ObjectHandle self = selfRef.Handle();
                        const ObjectHandle otherActor = otherActorRef.Handle();
                        const ObjectHandle otherComponent = otherComponentRef.Handle();
                        Log::Add({EntryKind::ComponentBegan,
                                  self,
                                  otherActor,
                                  otherComponent,
                                  fromSweep,
                                  {},
                                  hit});
                    });
                (void)primitive.componentOverlapEnded.TryAddLambda(
                    [](WeakObjectRef<PrimitiveComponent> selfRef,
                       WeakObjectRef<Actor> otherActorRef,
                       WeakObjectRef<PrimitiveComponent> otherComponentRef) noexcept {
                        const ObjectHandle self = selfRef.Handle();
                        const ObjectHandle otherActor = otherActorRef.Handle();
                        const ObjectHandle otherComponent = otherComponentRef.Handle();
                        Log::Add({EntryKind::ComponentEnded,
                                  self,
                                  otherActor,
                                  otherComponent,
                                  false,
                                  {},
                                  {}});
                    });
            });
        (void)world.With<Actor>(owner, [&](Actor& actor) noexcept {
            (void)actor.actorHit.TryAddLambda([](WeakObjectRef<Actor> self,
                                                 WeakObjectRef<Actor> otherActor, Vec3d impulse,
                                                 const HitResult& hit) noexcept {
                Log::Add({EntryKind::ActorHit,
                          self.Handle(),
                          otherActor.Handle(),
                          {},
                          false,
                          impulse,
                          hit});
            });
            (void)actor.actorOverlapBegan.TryAddLambda([](WeakObjectRef<Actor> self,
                                                          WeakObjectRef<Actor>
                                                              otherActor) noexcept {
                Log::Add(
                    {EntryKind::ActorBegan, self.Handle(), otherActor.Handle(), {}, false, {}, {}});
            });
            (void)actor.actorOverlapEnded.TryAddLambda([](WeakObjectRef<Actor> self,
                                                          WeakObjectRef<Actor>
                                                              otherActor) noexcept {
                Log::Add(
                    {EntryKind::ActorEnded, self.Handle(), otherActor.Handle(), {}, false, {}, {}});
            });
        });
    }

    ObjectHandle Owner(ObjectHandle component) noexcept {
        auto owner = fixture.world->With<Component>(
            component, [](Component& value) noexcept { return value.GetOwner(); });
        return owner ? owner.Value() : ObjectHandle{};
    }

    Result<bool> Sweep(ObjectHandle component, const Vec3d& delta) noexcept {
        Result<bool> moved = Result<bool>::Failure(ErrorCode::NotFound);
        (void)fixture.world->With<PrimitiveComponent>(
            component, [&](PrimitiveComponent& primitive) noexcept {
                moved =
                    primitive.MoveComponent(delta, primitive.WorldQuaternion(), true, nullptr, {});
            });
        return moved;
    }

    // 기록 index의 종류·발행자가 맞는지다.
    static bool Is(int index, EntryKind kind, ObjectHandle publisher) noexcept {
        return index < Log::count && index < MaxEntries && Log::entries[index].kind == kind &&
               Log::entries[index].publisher == publisher;
    }

    BodyWorldFixture fixture;
    TypeRegistrationToken token;
    bool registered = false;
    bool started = false;
};

} // namespace

// 막는 sweep hit: 한쪽마다 OnHit → actorHit → componentHit, 자기 쪽 → 상대 쪽(뒤집은 법선)이다.
// sweep hit의 충격량은 0이다(UE InternalDispatchBlockingHit).
TEST(PhysicsEventField_SweepHitOrderAndArguments) {
    EventWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0, false);
    const ObjectHandle wall = scene.Box(WallX, false);
    REQUIRE(!mover.IsNull() && !wall.IsNull());
    scene.fixture.Step(1);
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle wallActor = scene.Owner(wall);
    Log::Reset();

    REQUIRE(scene.Sweep(mover, Vec3d(MoveDistance, 0.0, 0.0)));
    REQUIRE(Log::count == 6);
    CHECK(EventWorld::Is(0, EntryKind::HitHook, moverActor));
    CHECK(EventWorld::Is(1, EntryKind::ActorHit, moverActor));
    CHECK(EventWorld::Is(2, EntryKind::ComponentHit, mover));
    CHECK(EventWorld::Is(3, EntryKind::HitHook, wallActor));
    CHECK(EventWorld::Is(4, EntryKind::ActorHit, wallActor));
    CHECK(EventWorld::Is(5, EntryKind::ComponentHit, wall));
    const Entry* entries = Log::entries;
    CHECK(entries[1].otherActor == wallActor);
    CHECK(entries[2].otherActor == wallActor);
    CHECK(entries[2].otherComponent == wall);
    CHECK(entries[2].impulse == Vec3d::Zero());
    CHECK(entries[2].hit.blockingHit);
    CHECK(entries[2].hit.component == wall);
    CHECK(entries[5].otherActor == moverActor);
    CHECK(entries[5].otherComponent == mover);
    // 이동 쪽 법선은 −X(벽에서 나를 향함), 상대는 뒤집혀 +X다.
    CHECK(entries[2].hit.impactNormal.x < 0.0);
    CHECK(Near(entries[5].hit.impactNormal.x, -entries[2].hit.impactNormal.x, NormalTolerance));
}

// 시뮬레이션 hit도 OnHit → actorHit → componentHit이고 충격량을 싣는다.
TEST(PhysicsEventField_PhysicsHitCarriesImpulse) {
    EventWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle floor = scene.Spawn<BoxComponent>([](BoxComponent& box) noexcept {
        box.SetMobility(ComponentMobility::Static);
        (void)box.SetBoxExtent(Vec3d(FloorHalfExtent, FloorHalfExtent, FloorHalfThickness));
        (void)box.SetRelativeLocation(Vec3d(0.0, 0.0, -FloorHalfThickness));
    });
    const ObjectHandle ball = scene.Spawn<SphereComponent>([](SphereComponent& sphere) noexcept {
        (void)sphere.SetSphereRadius(static_cast<float>(SphereRadius));
        sphere.SetSimulatePhysics(true);
        (void)sphere.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
        sphere.SetNotifyRigidBodyCollision(true);
    });
    REQUIRE(!floor.IsNull() && !ball.IsNull());
    const ObjectHandle ballActor = scene.Owner(ball);
    for (int step = 0; step < MaxFallSteps && Log::count == 0; ++step) {
        scene.fixture.Step(1);
    }

    REQUIRE(Log::count == 3);
    CHECK(EventWorld::Is(0, EntryKind::HitHook, ballActor));
    CHECK(EventWorld::Is(1, EntryKind::ActorHit, ballActor));
    CHECK(EventWorld::Is(2, EntryKind::ComponentHit, ball));
    CHECK(Log::entries[2].otherComponent == floor);
    CHECK(Log::entries[2].impulse.z > 0.0);
    CHECK(Log::entries[1].impulse == Log::entries[2].impulse);
}

// sweep으로 trigger를 지나가면: 시작은 컴포넌트 이벤트(자기 → 상대, fromSweep 참·상대는 뒤집은
// hit) → Actor hook 둘 → Actor 이벤트 둘, 끝은 컴포넌트 이벤트 둘 → Actor마다 hook → 이벤트다(UE).
TEST(PhysicsEventField_SweepOverlapOrderAndSweepResult) {
    EventWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0, false);
    const ObjectHandle trigger = scene.Box(TriggerX, true);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.fixture.Step(1);
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle triggerActor = scene.Owner(trigger);
    Log::Reset();

    REQUIRE(scene.Sweep(mover, Vec3d(MoveDistance + TriggerX, 0.0, 0.0)));
    REQUIRE(Log::count == 12);
    CHECK(EventWorld::Is(0, EntryKind::ComponentBegan, mover));
    CHECK(EventWorld::Is(1, EntryKind::ComponentBegan, trigger));
    CHECK(EventWorld::Is(2, EntryKind::BeginHook, moverActor));
    CHECK(EventWorld::Is(3, EntryKind::BeginHook, triggerActor));
    CHECK(EventWorld::Is(4, EntryKind::ActorBegan, moverActor));
    CHECK(EventWorld::Is(5, EntryKind::ActorBegan, triggerActor));
    CHECK(EventWorld::Is(6, EntryKind::ComponentEnded, mover));
    CHECK(EventWorld::Is(7, EntryKind::ComponentEnded, trigger));
    CHECK(EventWorld::Is(8, EntryKind::EndHook, moverActor));
    CHECK(EventWorld::Is(9, EntryKind::ActorEnded, moverActor));
    CHECK(EventWorld::Is(10, EntryKind::EndHook, triggerActor));
    CHECK(EventWorld::Is(11, EntryKind::ActorEnded, triggerActor));
    const Entry* entries = Log::entries;
    CHECK(entries[0].fromSweep);
    CHECK(entries[0].otherActor == triggerActor);
    CHECK(entries[0].otherComponent == trigger);
    CHECK(entries[0].hit.component == trigger);
    CHECK(!entries[0].hit.blockingHit);
    CHECK(entries[0].hit.actor == triggerActor);
    CHECK(entries[0].hit.impactNormal.x != 0.0);
    CHECK(entries[1].fromSweep);
    CHECK(entries[1].otherComponent == mover);
    CHECK(Near(entries[1].hit.impactNormal.x, -entries[0].hit.impactNormal.x, NormalTolerance));
    CHECK(entries[4].otherActor == triggerActor);
    CHECK(entries[7].otherComponent == mover);
}

// sweep 없이 자리에 놓아 겹치면 fromSweep 거짓, sweepResult는 상대 컴포넌트·Actor만 채운 hit이고
// 상대도 같은 hit를 받는다(UE OtherOverlap.OverlapInfo).
TEST(PhysicsEventField_PlacementOverlapIsNotFromSweep) {
    EventWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0, false);
    const ObjectHandle trigger = scene.Box(TriggerX, true);
    REQUIRE(!mover.IsNull() && !trigger.IsNull());
    scene.fixture.Step(1);
    const ObjectHandle triggerActor = scene.Owner(trigger);
    Log::Reset();

    REQUIRE(
        Apply<SceneComponent>(*scene.fixture.world, mover, [](SceneComponent& component) noexcept {
            (void)component.SetRelativeLocation(Vec3d(TriggerX, 0.0, 0.0));
        }));
    REQUIRE(Log::count >= 2);
    CHECK(EventWorld::Is(0, EntryKind::ComponentBegan, mover));
    CHECK(EventWorld::Is(1, EntryKind::ComponentBegan, trigger));
    const Entry* entries = Log::entries;
    CHECK(!entries[0].fromSweep);
    CHECK(entries[0].hit.component == trigger);
    CHECK(entries[0].hit.actor == triggerActor);
    CHECK(entries[0].hit.time == 1.0);
    CHECK(!entries[1].fromSweep);
    CHECK(entries[1].hit.component == trigger);
}

// 같은 Actor 컴포넌트끼리는 컴포넌트 이벤트만 나고 Actor hook·이벤트는 없다.
TEST(PhysicsEventField_SameActorComponentsGetComponentEventsOnly) {
    EventWorld scene;
    REQUIRE(scene.started);
    World& world = *scene.fixture.world;
    auto actor = world.SpawnActorDeferred<EventRecordingActor>();
    REQUIRE(actor);
    auto first = world.AddComponent<BoxComponent>(actor.Value());
    auto second = world.AddComponent<BoxComponent>(actor.Value());
    REQUIRE(first && second);
    auto configure = [](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
        (void)box.SetCollisionResponseToAllChannels(CollisionResponse::Overlap);
    };
    REQUIRE(Apply<BoxComponent>(world, first.Value(), configure));
    REQUIRE(Apply<BoxComponent>(world, second.Value(), [&](BoxComponent& box) noexcept {
        configure(box);
        (void)box.SetRelativeLocation(Vec3d(OverlappingChildOffset, 0.0, 0.0));
    }));
    REQUIRE(world.FinishSpawning(actor.Value()));
    scene.Subscribe(first.Value());
    scene.Subscribe(second.Value());
    Log::Reset();
    scene.fixture.Step(1);

    CHECK(Log::Count(EntryKind::ComponentBegan, first.Value()) == 1);
    CHECK(Log::Count(EntryKind::ComponentBegan, second.Value()) == 1);
    CHECK(Log::Count(EntryKind::ActorBegan, actor.Value()) == 0);
    CHECK(Log::Count(EntryKind::BeginHook, actor.Value()) == 0);
}

// 컴포넌트를 파괴하면 자기 끝 이벤트는 나지 않고(파괴 요청된 발행자) 상대는 받는다. EndPlay 중
// 끝내기는 이벤트가 없다(World가 Playing이 아니다).
TEST(PhysicsEventField_DestroyAndEndPlay) {
    {
        EventWorld scene;
        REQUIRE(scene.started);
        const ObjectHandle mover = scene.Box(0.0, false);
        const ObjectHandle trigger = scene.Box(0.0, true);
        REQUIRE(!mover.IsNull() && !trigger.IsNull());
        scene.fixture.Step(1);
        REQUIRE(Log::Count(EntryKind::ComponentBegan, trigger) == 1);
        Log::Reset();

        // 파괴는 프레임 끝 정리 단계에서 처리된다(OnDestroy → 모두 끝내기). UE처럼 파괴되는 쪽도
        // 끝 이벤트를 받는다(MarkAsGarbage 전에 끝낸다).
        const ObjectHandle triggerActor = scene.Owner(trigger);
        const ObjectHandle moverActor = scene.Owner(mover);
        REQUIRE(scene.fixture.world->DestroyComponent(trigger));
        scene.fixture.Step(1);
        CHECK(Log::Count(EntryKind::ComponentEnded, trigger) == 1);
        CHECK(Log::Count(EntryKind::ComponentEnded, mover) == 1);
        CHECK(Log::Count(EntryKind::ActorEnded, triggerActor) == 1);
        CHECK(Log::Count(EntryKind::ActorEnded, moverActor) == 1);
    }

    {
        // Actor 파괴도 같다: 파괴되는 Actor가 hook·이벤트를 받는다.
        EventWorld scene;
        REQUIRE(scene.started);
        const ObjectHandle mover = scene.Box(0.0, false);
        const ObjectHandle trigger = scene.Box(0.0, true);
        REQUIRE(!mover.IsNull() && !trigger.IsNull());
        scene.fixture.Step(1);
        const ObjectHandle triggerActor = scene.Owner(trigger);
        Log::Reset();
        REQUIRE(scene.fixture.world->DestroyActor(triggerActor));
        scene.fixture.Step(1);
        CHECK(Log::Count(EntryKind::EndHook, triggerActor) == 1);
        CHECK(Log::Count(EntryKind::ActorEnded, triggerActor) == 1);
        CHECK(Log::Count(EntryKind::ComponentEnded, mover) == 1);
    }

    // EndPlay의 정리는 World가 Playing이 아니라 이벤트가 없다(장부는 비운다).
    EventWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0, false);
    REQUIRE(!mover.IsNull());
    REQUIRE(!scene.Box(0.0, true).IsNull());
    scene.fixture.Step(1);
    REQUIRE(Log::Count(EntryKind::ComponentBegan, mover) == 1);
    Log::Reset();
    REQUIRE(scene.fixture.world->EndPlay(EndPlayReason::Quit));
    CHECK(Log::count == 0);
    auto ledger = scene.fixture.world->With<PrimitiveComponent>(
        mover, [](PrimitiveComponent& primitive) noexcept {
            return primitive.OverlappingComponents().Size();
        });
    CHECK((!ledger || ledger.Value() == 0));
}

// OnHit hook이 자기 Actor를 파괴하면 이어지는 actorHit·componentHit는 나지 않는다(UE
// InternalDispatchBlockingHit의 재검사). 상대 쪽은 그대로 받는다.
TEST(PhysicsEventField_HookDestroyingSelfStopsHitEvents) {
    EventWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0, false);
    const ObjectHandle wall = scene.Box(WallX, false);
    REQUIRE(!mover.IsNull() && !wall.IsNull());
    scene.fixture.Step(1);
    const ObjectHandle moverActor = scene.Owner(mover);
    const ObjectHandle wallActor = scene.Owner(wall);
    EventRecordingActor::destroySelfOnHit = moverActor;
    Log::Reset();

    REQUIRE(scene.Sweep(mover, Vec3d(MoveDistance, 0.0, 0.0)));
    CHECK(Log::Count(EntryKind::HitHook, moverActor) == 1);
    CHECK(Log::Count(EntryKind::ActorHit, moverActor) == 0);
    CHECK(Log::Count(EntryKind::ComponentHit, mover) == 0);
    CHECK(Log::Count(EntryKind::ActorHit, wallActor) == 1);
    CHECK(Log::Count(EntryKind::ComponentHit, wall) == 1);
}

// 같은 Actor의 두 컴포넌트가 한 trigger와 겹치면 컴포넌트 이벤트는 둘, Actor 이벤트는 쌍마다 한
// 번이다.
TEST(PhysicsEventField_ActorEventsOncePerActorPair) {
    EventWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle trigger = scene.Box(0.0, true);
    REQUIRE(!trigger.IsNull());
    World& world = *scene.fixture.world;
    auto actor = world.SpawnActorDeferred<EventRecordingActor>();
    REQUIRE(actor);
    auto first = world.AddComponent<BoxComponent>(actor.Value());
    auto second = world.AddComponent<BoxComponent>(actor.Value());
    REQUIRE(first && second);
    auto configure = [](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
    };
    REQUIRE(Apply<BoxComponent>(world, first.Value(), configure));
    REQUIRE(Apply<BoxComponent>(world, second.Value(), [&](BoxComponent& box) noexcept {
        configure(box);
        (void)box.SetRelativeLocation(Vec3d(OverlappingChildOffset, 0.0, 0.0));
        // 같은 Actor 두 상자끼리는 막음이라 겹침이 아니다(trigger만 본다).
    }));
    REQUIRE(world.FinishSpawning(actor.Value()));
    scene.Subscribe(first.Value());
    scene.Subscribe(second.Value());
    const ObjectHandle triggerActor = scene.Owner(trigger);
    Log::Reset();
    scene.fixture.Step(1);

    CHECK(Log::Count(EntryKind::ComponentBegan, trigger) == 2);
    // 두 컴포넌트를 구독했으므로 Actor 이벤트 구독도 둘이다 — 발행은 한 번이라 기록은 둘이다.
    CHECK(Log::Count(EntryKind::BeginHook, actor.Value()) == 1);
    CHECK(Log::Count(EntryKind::ActorBegan, actor.Value()) == 2);
    CHECK(Log::Count(EntryKind::BeginHook, triggerActor) == 1);
    CHECK(Log::Count(EntryKind::ActorBegan, triggerActor) == 1);
}

// 이벤트 필드는 등록된 타입 schema에 이벤트로 실리고, 인자는 Actor·컴포넌트 약한 참조·값·HitResult
// struct다(리플렉션 바인딩·Sparks 노출의 서명 — SparksBinding B14).
TEST(PhysicsEventField_EventSchemaParameters) {
    EventWorld scene;
    REQUIRE(scene.started);
    auto lease = scene.fixture.types.Acquire(ObjectTypeIdOf<PrimitiveComponent>());
    REQUIRE(lease);
    const TypeSchema& schema = lease.Value().Schema();
    const FieldSchema* hit = nullptr;
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == StringView("componentHit")) {
            hit = &field;
        }
    }

    REQUIRE(hit != nullptr);
    CHECK(hit->kind == FieldKind::Event);
    REQUIRE(hit->eventParameters.Size() == 5);
    CHECK(hit->eventParameters[0].value.kind == ValueKind::WeakObjectReference);
    CHECK(hit->eventParameters[1].value.kind == ValueKind::WeakObjectReference);
    CHECK(hit->eventParameters[2].value.kind == ValueKind::WeakObjectReference);
    CHECK(hit->eventParameters[4].value.kind == ValueKind::Struct);
    CHECK(hit->eventParameters[4].value.type == TypeIdOf<HitResult>());
    CHECK(lease.Value().Ops().events.Size() == 3);
}

// HitResult 리플렉션 schema는 값 칸만 싣는다(component·actor는 NATIVE).
TEST(PhysicsEventField_HitResultSchemaHasValueFields) {
    const TypeSchema& schema = GeneratedType<HitResult>::Schema();
    CHECK(schema.fields.Size() == HitResultReflectedFields);
    bool hasTime = false;
    bool hasComponent = false;
    for (const FieldSchema& field : schema.fields) {
        const StringView name(field.name.Data(), field.name.Size());
        hasTime = hasTime || name == StringView("time");
        hasComponent = hasComponent || name == StringView("component");
    }

    CHECK(hasTime);
    CHECK(!hasComponent);
}

} // namespace physics_event_field_tests_file

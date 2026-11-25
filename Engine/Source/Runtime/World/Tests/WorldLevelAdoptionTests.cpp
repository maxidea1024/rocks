#include <Base/InitializerList.h>
#include <Collision/BodySettings.generated.h>
#include <Collision/CollisionSettings.generated.h>
#include <Collision/CollisionTypes.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/Lights/LightingChannels.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/LightUnits.generated.h>
#include <World/Lights/LocalLightComponent.generated.h>
#include <World/Lights/PointLight.generated.h>
#include <World/Lights/PointLight.h>
#include <World/Lights/PointLightComponent.generated.h>
#include <World/Lights/PointLightComponent.h>
#include <World/Lights/Light.generated.h>
#include <World/Primitives/BoxComponent.generated.h>
#include <World/Primitives/BoxComponent.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveTypes.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/TriggerBase.generated.h>
#include <World/Primitives/TriggerBox.generated.h>
#include <World/Primitives/TriggerBox.h>
#include <World/SceneComponent.generated.h>
#include <World/Private/LevelGraphTestAccess.h>
#include <World/World.h>
#include <math.h>
#include <stdio.h>

// 레벨 편입 시험이다(Docs/Design/EngineRuntime 29절): 저장한 Level 그래프를 같은 World registry로
// 읽어 AdoptLevel — 레코드·소유·부착·수명, 실패 Actor 계수, 거부 조건.
using namespace rocks;
namespace world_level_adoption_tests_file {

namespace {

constexpr double AdoptionTolerance = 1.0e-9;

bool NearAdoptionVector(const Vec3d& value, double x, double y, double z) noexcept {
    const bool same = fabs(value.x - x) <= AdoptionTolerance &&
                      fabs(value.y - y) <= AdoptionTolerance &&
                      fabs(value.z - z) <= AdoptionTolerance;
    if (!same) {
        printf("  vector (%g, %g, %g), expected (%g, %g, %g)\n", value.x, value.y, value.z, x, y,
               z);
    }

    return same;
}

struct AdoptionFixture {
    AdoptionFixture() noexcept : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk();
        WorldDesc desc;
        desc.name = "Adoption";
        desc.context = {0x41444f50ull, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.kind = WorldKind::Game;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~AdoptionFixture() {
        world.Reset();
        CHECK(modules.Unregister().IsOk());
    }

    // root에서 저장한 그래프를 이 World registry로 다시 읽는다(편입 전 — 장부 밖).
    Result<RootRef<Object>> SaveAndReload(ObjectHandle root) noexcept {
        auto bytes = SaveObjectGraph(allocator, world->Objects(), world->Access(), root);
        if (!bytes) {
            return Result<RootRef<Object>>::Failure(bytes.Error());
        }

        return LoadObjectGraph(allocator, world->Objects(), types, world->Access(),
                               bytes.Value().View());
    }

    ObjectHandle RootOf(ObjectHandle actor) noexcept {
        auto root =
            world->With<Actor>(actor, [](Actor& value) noexcept { return value.RootComponent(); });
        return root ? root.Value() : ObjectHandle{};
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    bool registered = false;
    UniquePtr<World> world;
};

// 편입 전 그래프의 handle이다: Actor A(root S1 · 자식 S2), Actor B(root S3).
struct LoadedGraph {
    RootRef<Object> root;
    ObjectHandle level;
    ObjectHandle actorA;
    ObjectHandle actorB;
    ObjectHandle s1;
    ObjectHandle s2;
    ObjectHandle s3;
};

// 원본 Level을 만들어 저장하고 다시 읽는다. 다시 읽은 객체는 장부 밖이다.
bool BuildLoadedGraph(AdoptionFixture& fixture, LoadedGraph& graph) noexcept {
    World& world = *fixture.world;
    auto source = world.CreateLevel("Graph");
    if (!source) {
        return false;
    }

    SpawnParams params;
    params.level = source.Value();
    auto a = world.SpawnActor<Actor>(params);
    auto b = world.SpawnActor<Actor>(params);
    if (!a || !b) {
        return false;
    }

    auto s1 = world.AddComponent<SceneComponent>(a.Value());
    auto s2 = world.AddComponent<SceneComponent>(a.Value());
    auto s3 = world.AddComponent<SceneComponent>(b.Value());
    if (!s1 || !s2 || !s3) {
        return false;
    }

    const bool built =
        world
            .With<Actor>(a.Value(),
                         [&](Actor& actor) noexcept { return actor.SetRootComponent(s1.Value()); })
            .IsOk() &&
        world
            .With<Actor>(b.Value(),
                         [&](Actor& actor) noexcept { return actor.SetRootComponent(s3.Value()); })
            .IsOk() &&
        world
            .With<SceneComponent>(
                s2.Value(), [&](SceneComponent& c) noexcept { return c.AttachTo(s1.Value()); })
            .IsOk();
    if (!built) {
        return false;
    }

    auto reloaded = fixture.SaveAndReload(source.Value());
    if (!reloaded) {
        return false;
    }

    graph.root = Move(reloaded.Value());
    graph.level = graph.root.Handle();
    // 저장 순서는 BFS(Level → actors → components)라 원본과 같은 배열 순서다.
    bool found = world
                     .With<Level>(graph.level,
                                  [&](Level& level) noexcept {
                                      if (level.Actors().Size() != 2) {
                                          return false;
                                      }

                                      graph.actorA = level.Actors()[0];
                                      graph.actorB = level.Actors()[1];
                                      return true;
                                  })
                     .IsOk();
    found = found && world
                         .With<Actor>(graph.actorA,
                                      [&](Actor& actor) noexcept {
                                          graph.s1 = actor.Components()[0];
                                          graph.s2 = actor.Components()[1];
                                      })
                         .IsOk();
    found =
        found && world
                     .With<Actor>(graph.actorB,
                                  [&](Actor& actor) noexcept { graph.s3 = actor.Components()[0]; })
                     .IsOk();
    return found;
}

// 편입되지 않았는지(장부 밖 그대로)다.
bool StillUnadopted(World& world, const LoadedGraph& graph) noexcept {
    bool listed = false;
    world.ForEachLevel(
        [&](ObjectHandle level) noexcept { listed = listed || level == graph.level; });
    return !listed && !world.GetActorState(graph.actorA) && !world.GetActorState(graph.actorB) &&
           !world.GetComponents(graph.actorA, Span<ObjectHandle>());
}

} // namespace

// 29.2절: 저장 → 다시 읽기 → 편입하면 Spawn한 Level처럼 레코드·소유·부착·수명이 맞는다.
TEST(LevelAdoption_RoundTripMatchesSpawnedLevel) {
    AdoptionFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto source = world.CreateLevel("Source");
    REQUIRE(source);
    SpawnParams params;
    params.level = source.Value();

    // Actor + root SceneComponent(100, 0, 0) + 자식 SceneComponent 상대 (0, 50, 0).
    auto holder = world.SpawnActor<Actor>(params);
    REQUIRE(holder);
    auto parent = world.AddComponent<SceneComponent>(holder.Value());
    auto child = world.AddComponent<SceneComponent>(holder.Value());
    REQUIRE(parent && child);
    REQUIRE(world.With<Actor>(holder.Value(), [&](Actor& actor) noexcept {
        return actor.SetRootComponent(parent.Value());
    }));
    REQUIRE(world.With<SceneComponent>(parent.Value(), [](SceneComponent& c) noexcept {
        return c.SetRelativeLocation(Vec3d(100.0, 0.0, 0.0));
    }));
    REQUIRE(world.With<SceneComponent>(child.Value(), [&](SceneComponent& c) noexcept {
        if (auto attached = c.AttachTo(parent.Value()); !attached) {
            return attached;
        }

        return c.SetRelativeLocation(Vec3d(0.0, 50.0, 0.0));
    }));
    auto zone = world.SpawnActor<TriggerBox>(params);
    REQUIRE(zone);

    auto reloaded = fixture.SaveAndReload(source.Value());
    REQUIRE(reloaded);
    const ObjectHandle loadedLevel = reloaded.Value().Handle();
    CHECK(loadedLevel != source.Value());
    auto adopted = world.AdoptLevel(Move(reloaded.Value()));
    REQUIRE(adopted);
    CHECK(adopted.Value().level == loadedLevel);
    CHECK(adopted.Value().actors == 2 && adopted.Value().failedActors == 0);
    CHECK(adopted.Value().droppedEntries == 0);

    // Level 레코드: 이 World의 Level이고 Spawn할 수 있다.
    bool listed = false;
    world.ForEachLevel(
        [&](ObjectHandle level) noexcept { listed = listed || level == loadedLevel; });
    CHECK(listed);
    SpawnParams into;
    into.level = loadedLevel;
    CHECK(world.SpawnActor<Actor>(into));

    // Actor 레코드·소유·수명.
    ObjectHandle loadedHolder;
    ObjectHandle loadedZone;
    REQUIRE(world.With<Level>(loadedLevel, [&](Level& level) noexcept {
        CHECK(level.Actors().Size() == 3);
        for (const ObjectHandle actor : level.Actors()) {
            if (world.With<TriggerBox>(actor, [](TriggerBox&) noexcept {})) {
                loadedZone = actor;
            } else if (fixture.RootOf(actor) != ObjectHandle{}) {
                loadedHolder = actor;
            }
        }
    }));
    REQUIRE(!loadedHolder.IsNull() && !loadedZone.IsNull());
    for (const ObjectHandle actor : {loadedHolder, loadedZone}) {
        auto state = world.GetActorState(actor);
        CHECK(state && state.Value() == ActorState::Initialized);
    }

    ObjectHandle components[4];
    auto count = world.GetComponents(loadedHolder, Span<ObjectHandle>(components, 4));
    REQUIRE(count && count.Value() == 2);
    const ObjectHandle loadedParent = fixture.RootOf(loadedHolder);
    const ObjectHandle loadedChild = components[0] == loadedParent ? components[1] : components[0];
    for (const ObjectHandle component : {loadedParent, loadedChild}) {
        auto state = world.GetComponentState(component);
        CHECK(state && state.Value() == ComponentState::Initialized);
    }

    // 부착 재구성: 부모·자식 목록·월드 위치.
    CHECK(world.With<SceneComponent>(loadedChild, [&](SceneComponent& c) noexcept {
        CHECK(c.Parent() == loadedParent);
        CHECK(NearAdoptionVector(c.WorldLocation(), 100.0, 50.0, 0.0));
    }));
    CHECK(world.With<SceneComponent>(loadedParent, [&](SceneComponent& c) noexcept {
        REQUIRE(c.Children().Size() == 1);
        CHECK(c.Children()[0] == loadedChild);
    }));
    // 부모를 옮기면 자식도 따라간다(계층이 살아 있다).
    REQUIRE(world.With<SceneComponent>(loadedParent, [](SceneComponent& c) noexcept {
        return c.SetRelativeLocation(Vec3d(0.0, 0.0, 10.0));
    }));
    CHECK(world.With<SceneComponent>(loadedChild, [](SceneComponent& c) noexcept {
        CHECK(NearAdoptionVector(c.WorldLocation(), 0.0, 50.0, 10.0));
    }));

    // 엔진 Actor의 저장 root를 재사용한다(TriggerBox — 같은 타입 root, profile 그대로).
    CHECK(world.With<BoxComponent>(fixture.RootOf(loadedZone), [](BoxComponent& box) noexcept {
        CHECK(box.CollisionProfileName() == StringView("Trigger"));
    }));
    ObjectHandle zoneComponents[2];
    auto zoneCount = world.GetComponents(loadedZone, Span<ObjectHandle>(zoneComponents, 2));
    CHECK(zoneCount && zoneCount.Value() == 1);

    // 시작·정리: BeginPlay가 시작하고 DestroyLevel이 정리한다.
    REQUIRE(world.BeginPlay());
    auto playing = world.GetActorState(loadedHolder);
    CHECK(playing && playing.Value() == ActorState::Playing);
    REQUIRE(world.DestroyLevel(loadedLevel));
    REQUIRE(world.Tick(1.0 / 60.0));
    CHECK(!world.GetActorState(loadedHolder));
    REQUIRE(world.Shutdown());
}

// 29.2절 6: FinishSpawning이 실패한 Actor(다른 타입 root의 광원)는 정리하고 계속한다.
TEST(LevelAdoption_FailedActorIsCountedAndRemoved) {
    AdoptionFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto source = world.CreateLevel("Broken");
    REQUIRE(source);
    SpawnParams params;
    params.level = source.Value();
    // 지연 Spawn으로 PointLight에 SceneComponent root를 둔 채 저장한다(PreAwake가 거부한다).
    auto broken = world.SpawnActorDeferred<PointLight>(params);
    REQUIRE(broken);
    auto wrongRoot = world.AddComponent<SceneComponent>(broken.Value());
    REQUIRE(wrongRoot);
    REQUIRE(world.With<Actor>(broken.Value(), [&](Actor& actor) noexcept {
        return actor.SetRootComponent(wrongRoot.Value());
    }));
    auto healthy = world.SpawnActor<PointLight>(params);
    REQUIRE(healthy);

    auto reloaded = fixture.SaveAndReload(source.Value());
    REQUIRE(reloaded);
    auto adopted = world.AdoptLevel(Move(reloaded.Value()));
    REQUIRE(adopted);
    CHECK(adopted.Value().actors == 2);
    CHECK(adopted.Value().failedActors == 1);
    uint32 alive = 0;
    REQUIRE(world.With<Level>(adopted.Value().level, [&](Level& level) noexcept {
        for (const ObjectHandle actor : level.Actors()) {
            auto state = world.GetActorState(actor);
            if (state && state.Value() == ActorState::Initialized) {
                ++alive;
                CHECK(world.With<PointLightComponent>(fixture.RootOf(actor),
                                                      [](PointLightComponent&) noexcept {}));
            }
        }
    }));
    CHECK(alive == 1);
    REQUIRE(world.Shutdown());
}

// 29.2절 1: Created World·Level이 아닌 루트·이미 편입한 Level은 거부하고 아무것도 바꾸지 않는다.
TEST(LevelAdoption_RejectsInvalidRoots) {
    AdoptionFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto source = world.CreateLevel("Source");
    REQUIRE(source);
    SpawnParams params;
    params.level = source.Value();
    auto actor = world.SpawnActor<Actor>(params);
    REQUIRE(actor);
    auto scene = world.AddComponent<SceneComponent>(actor.Value());
    REQUIRE(scene);

    // Level이 아닌 루트(Actor에서 저장한 그래프).
    auto notLevel = fixture.SaveAndReload(actor.Value());
    REQUIRE(notLevel);
    const ObjectHandle loadedActor = notLevel.Value().Handle();
    auto mismatched = world.AdoptLevel(Move(notLevel.Value()));
    CHECK(!mismatched && mismatched.Error() == ErrorCode::TypeMismatch);
    CHECK(!world.GetActorState(loadedActor));

    // 이미 장부에 있는 Level(만든 Level 자체).
    auto pinned = world.Objects().TryPin<Object>(world.Access(), source.Value());
    REQUIRE(pinned);
    auto duplicate = world.AdoptLevel(Move(pinned.Value()));
    CHECK(!duplicate && duplicate.Error() == ErrorCode::AlreadyExists);

    // 한 번 편입한 Level을 다시 편입할 수 없다.
    auto reloaded = fixture.SaveAndReload(source.Value());
    REQUIRE(reloaded);
    const ObjectHandle loadedLevel = reloaded.Value().Handle();
    REQUIRE(world.AdoptLevel(Move(reloaded.Value())));
    auto again = world.Objects().TryPin<Object>(world.Access(), loadedLevel);
    REQUIRE(again);
    auto twice = world.AdoptLevel(Move(again.Value()));
    CHECK(!twice && twice.Error() == ErrorCode::AlreadyExists);
    REQUIRE(world.Shutdown());

    // 초기화하지 않은 World는 편입하지 않는다.
    AdoptionFixture created;
    REQUIRE(created.registered);
    RootRef<Object> empty;
    auto early = created.world->AdoptLevel(Move(empty));
    CHECK(!early && early.Error() == ErrorCode::InvalidArgument);
}

// 29.2절 3: null 원소·root 밖을 순서를 지키며 정리하고, 정리 뒤 위치로 파괴 경로가 맞는다.
TEST(LevelAdoption_CleansNullsAndOutsideRoot) {
    AdoptionFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    LoadedGraph graph;
    REQUIRE(BuildLoadedGraph(fixture, graph));
    using Access = detail::LevelGraphTestAccess;
    // Level 앞쪽에 null을 끼우려고 끝에 null을 둔 뒤 B 앞에도 null이 오게 한다: [A, B, null].
    REQUIRE(Access::AppendActor(world, graph.level, ObjectHandle{}));
    // A components에 null, B root를 A의 S2(B components 밖)로.
    REQUIRE(Access::AppendComponent(world, graph.actorA, ObjectHandle{}));
    REQUIRE(Access::SetRoot(world, graph.actorB, graph.s2));

    auto adopted = world.AdoptLevel(Move(graph.root));
    REQUIRE(adopted);
    CHECK(adopted.Value().actors == 2 && adopted.Value().failedActors == 0);
    CHECK(adopted.Value().droppedEntries == 3);
    CHECK(world.With<Level>(graph.level,
                            [](Level& level) noexcept { CHECK(level.Actors().Size() == 2); }));
    ObjectHandle components[4];
    auto count = world.GetComponents(graph.actorA, Span<ObjectHandle>(components, 4));
    CHECK(count && count.Value() == 2);
    CHECK(components[0] == graph.s1 && components[1] == graph.s2);
    CHECK(fixture.RootOf(graph.actorB).IsNull());
    CHECK(fixture.RootOf(graph.actorA) == graph.s1);
    // 앞 Actor를 지워도(바꿔 떼기 위치 갱신) Fatal 없이 정리된다.
    REQUIRE(world.DestroyActor(graph.actorA));
    REQUIRE(world.Tick(1.0 / 60.0));
    CHECK(!world.GetActorState(graph.actorA));
    auto remaining = world.GetActorState(graph.actorB);
    CHECK(remaining && remaining.Value() != ActorState::PendingDestroy);
    REQUIRE(world.DestroyActor(graph.actorB));
    REQUIRE(world.Tick(1.0 / 60.0));
    REQUIRE(world.Shutdown());
}

// 29.2절 2·5: 다른 Actor Component에 붙은 부착은 다시 만들고, 이 Level 밖 부모·Mobility 위반은
// 끊으며, 부모 없이 남은 socket은 비운다.
TEST(LevelAdoption_AttachmentRulesOnLoad) {
    AdoptionFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    LoadedGraph graph;
    REQUIRE(BuildLoadedGraph(fixture, graph));
    using Access = detail::LevelGraphTestAccess;
    // B의 root S3를 A의 S2에 붙인다(다른 Actor의 Component — 유효).
    REQUIRE(Access::SetAttachParent(world, graph.s3, graph.s2, "Grip"));
    // 장부 안(다른 Level) Component를 부모로 가리키게 만든 손상: S2의 부모를 바깥 Component로.
    auto outsider = world.SpawnActor<Actor>();
    REQUIRE(outsider);
    auto outsiderScene = world.AddComponent<SceneComponent>(outsider.Value());
    REQUIRE(outsiderScene);
    REQUIRE(Access::SetAttachParent(world, graph.s2, outsiderScene.Value(), ""));

    auto adopted = world.AdoptLevel(Move(graph.root));
    REQUIRE(adopted);
    CHECK(adopted.Value().droppedEntries == 1);
    CHECK(world.With<SceneComponent>(graph.s3, [&](SceneComponent& c) noexcept {
        CHECK(c.Parent() == graph.s2);
        CHECK(c.AttachSocketName() == StringView("Grip"));
    }));
    CHECK(world.With<SceneComponent>(graph.s2, [&](SceneComponent& c) noexcept {
        CHECK(c.Parent().IsNull());
        CHECK(c.Children().Size() == 1 && c.Children()[0] == graph.s3);
    }));
    CHECK(world.With<SceneComponent>(
        outsiderScene.Value(), [](SceneComponent& c) noexcept { CHECK(c.Children().IsEmpty()); }));
    REQUIRE(world.Shutdown());

    // Static 자식이 Movable 부모에 붙은 데이터는 끊고, 부모 없는 socket은 비운다.
    AdoptionFixture second;
    REQUIRE(second.registered);
    World& other = *second.world;
    REQUIRE(other.Initialize());
    LoadedGraph mobility;
    REQUIRE(BuildLoadedGraph(second, mobility));
    REQUIRE(other.With<SceneComponent>(
        mobility.s2, [](SceneComponent& c) noexcept { c.SetMobility(ComponentMobility::Static); }));
    REQUIRE(Access::SetAttachParent(other, mobility.s3, ObjectHandle{}, "Stray"));
    auto moved = other.AdoptLevel(Move(mobility.root));
    REQUIRE(moved);
    CHECK(moved.Value().droppedEntries == 1);
    CHECK(other.With<SceneComponent>(
        mobility.s2, [](SceneComponent& c) noexcept { CHECK(c.Parent().IsNull()); }));
    CHECK(other.With<SceneComponent>(
        mobility.s3, [](SceneComponent& c) noexcept { CHECK(c.AttachSocketName().IsEmpty()); }));
    REQUIRE(other.Shutdown());
}

// 29.2절 2: 손상된 그래프(중복 Actor·중복 Component·잘못된 Transform·부착 순환)는 거부하고 아무것도
// 바꾸지 않는다.
TEST(LevelAdoption_RejectsCorruptGraphsUnchanged) {
    using Access = detail::LevelGraphTestAccess;
    enum class Corruption {
        DuplicateActor,
        DuplicateComponent,
        InvalidTransform,
        Cycle
    };
    for (Corruption corruption : {Corruption::DuplicateActor, Corruption::DuplicateComponent,
                                  Corruption::InvalidTransform, Corruption::Cycle}) {
        AdoptionFixture fixture;
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        LoadedGraph graph;
        REQUIRE(BuildLoadedGraph(fixture, graph));
        switch (corruption) {
        case Corruption::DuplicateActor:
            REQUIRE(Access::AppendActor(world, graph.level, graph.actorA));
            break;
        case Corruption::DuplicateComponent:
            REQUIRE(Access::AppendComponent(world, graph.actorB, graph.s1));
            break;
        case Corruption::InvalidTransform: {
            Transform broken;
            broken.rotation = Quatd(0.0, 0.0, 0.0, 0.0);
            REQUIRE(Access::SetRelative(world, graph.s3, broken));
            break;
        }
        case Corruption::Cycle:
            REQUIRE(Access::SetAttachParent(world, graph.s1, graph.s2, ""));
            break;
        }

        // 거부 뒤에도 그래프를 살펴보려고 루트를 하나 더 잡아 둔다.
        auto keep = world.Objects().TryPin<Object>(world.Access(), graph.level);
        REQUIRE(keep);
        auto adopted = world.AdoptLevel(Move(graph.root));
        CHECK(!adopted && adopted.Error() == ErrorCode::InvalidArgument);
        CHECK(StillUnadopted(world, graph));
        CHECK(world.With<Level>(graph.level, [&](Level& level) noexcept {
            CHECK(level.Actors().Size() == (corruption == Corruption::DuplicateActor ? 3u : 2u));
        }));
        keep.Value().Reset();
        REQUIRE(world.Shutdown());
    }
}

// 29.2절 1·6: Playing 중에도 편입하고, 편입한 Actor는 다음 Tick에 시작한다.
TEST(LevelAdoption_WhilePlaying) {
    AdoptionFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    LoadedGraph graph;
    REQUIRE(BuildLoadedGraph(fixture, graph));
    REQUIRE(world.BeginPlay());
    auto adopted = world.AdoptLevel(Move(graph.root));
    REQUIRE(adopted);
    auto initialized = world.GetActorState(graph.actorA);
    CHECK(initialized && initialized.Value() == ActorState::Initialized);
    REQUIRE(world.Tick(1.0 / 60.0));
    auto playing = world.GetActorState(graph.actorA);
    CHECK(playing && playing.Value() == ActorState::Playing);
    auto component = world.GetComponentState(graph.s2);
    CHECK(component && component.Value() == ComponentState::Playing);
    REQUIRE(world.Shutdown());
}

} // namespace world_level_adoption_tests_file

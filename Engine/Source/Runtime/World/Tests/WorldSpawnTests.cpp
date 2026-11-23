#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>
#include <stdio.h>
#include <string.h>

// Spawn 인자·Owner·Instigator·Component 시작 준비·LifeSpan
// 시험이다(Docs/Design/EngineRuntime 17.3–17.5절, S9b).
using namespace rocks;
namespace world_spawn_tests_file {

namespace {

constexpr uint32 MaxEvents = 32;
char tags[MaxEvents + 1] = {};
uint32 tagCount = 0;

void Tag(char value) noexcept {
    if (tagCount < MaxEvents) {
        tags[tagCount++] = value;
        tags[tagCount] = '\0';
    }
}

void ResetTags() noexcept {
    tagCount = 0;
    tags[0] = '\0';
}

bool TagsAre(const char* expected) noexcept {
    const bool same = strcmp(tags, expected) == 0;
    if (!same) {
        printf("  tags = \"%s\", expected \"%s\"\n", tags, expected);
    }

    return same;
}

// 시작('S')·늦은 Tick('L')·파괴(표지)를 남기는 Actor다.
class SpawnActor final : public Actor {
    ROCKS_NATIVE_TYPE(SpawnActor, Actor, 0x1f8c3a6e0b9d4275ull, 0xc3e71d58a20f9b46ull)

public:
    explicit SpawnActor(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().lateTick = true;
    }

    void OnStart() noexcept override {
        Tag('S');
    }

    void OnLateTick(const TickContext&) noexcept override {
        Tag('L');
    }

    void OnDestroy(EndPlayReason) noexcept override {
        Tag(tag);
    }

    char tag = 'D';
};

// 준비 여부를 바꿀 수 있는 Component다.
class WaitingComponent final : public Component {
    ROCKS_NATIVE_TYPE(WaitingComponent, Component, 0x8a2e5f1c7d304b96ull, 0x41b9c0e6f27d83a5ull)

public:
    bool IsReadyToStart() const noexcept override {
        return ready;
    }

    void OnStart() noexcept override {
        Tag('c');
    }

    bool ready = false;
};

struct Fixture {
    Fixture() noexcept : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && Register<SpawnActor>() &&
                     Register<WaitingComponent>();
        WorldDesc desc;
        desc.name = "Test";
        desc.context = {0x574f524c44ull, 13};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
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
        if (!token || tokenCount == 4) {
            return false;
        }

        tokens[tokenCount++] = token.Value();
        return true;
    }

    bool Frame() noexcept {
        return world->Tick(1.0 / 60.0).IsOk();
    }

    ObjectHandle Spawn(char tag) noexcept {
        auto actor = world->SpawnActor<SpawnActor>();
        if (!actor) {
            return {};
        }

        auto tagged = world->With<SpawnActor>(
            actor.Value(), [tag](SpawnActor& value) noexcept { value.tag = tag; });
        return tagged ? actor.Value() : ObjectHandle{};
    }

    void SetReady(ObjectHandle component, bool ready) noexcept {
        auto set = world->With<WaitingComponent>(
            component, [ready](WaitingComponent& value) noexcept { value.ready = ready; });
        CHECK(set.IsOk());
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken tokens[4];
    uint32 tokenCount = 0;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

// 17.3절: Spawn 인자의 Transform은 FinishSpawning이 root SceneComponent에 쓴다(root가 없으면 무시).
TEST(WorldSpawn_TransformAppliesToRoot) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    const Transform at(Quatd::Identity(), Vec3d(100.0, -50.0, 25.0), Vec3d::One());
    auto actor = world.SpawnActorDeferred<SpawnActor>(SpawnParams::At(at));
    REQUIRE(actor);
    auto root = world.AddComponent<SceneComponent>(actor.Value());
    REQUIRE(root);
    REQUIRE(world.With<Actor>(actor.Value(), [&](Actor& value) noexcept {
        return value.SetRootComponent(root.Value());
    }));
    REQUIRE(world.FinishSpawning(actor.Value()));
    auto location = world.With<SceneComponent>(root.Value(), [](SceneComponent& scene) noexcept {
        return scene.RelativeTransform().translation;
    });
    REQUIRE(location);
    CHECK(location.Value().x == 100.0 && location.Value().y == -50.0 && location.Value().z == 25.0);

    // root가 없으면 무시하고 Spawn은 성공한다.
    CHECK(world.SpawnActor<SpawnActor>(SpawnParams::At(at)).IsOk());
    REQUIRE(world.Shutdown());
}

// 17.3절 ERD15: 소유 고리(자기 자신 포함)는 거부, 없는 대상은 StaleHandle. Spawn 인자의 잘못된
// 소유자는 Spawn을 실패시킨다. Instigator는 그대로 보관한다.
TEST(WorldSpawn_OwnerRejectsCyclesAndInstigatorIsKept) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    const ObjectHandle a = fixture.Spawn('a');
    const ObjectHandle b = fixture.Spawn('b');
    const ObjectHandle c = fixture.Spawn('c');
    REQUIRE(!a.IsNull() && !b.IsNull() && !c.IsNull());
    auto setOwner = [&](ObjectHandle actor, ObjectHandle owner) noexcept {
        return world.With<Actor>(actor,
                                 [owner](Actor& value) noexcept { return value.SetOwner(owner); });
    };
    REQUIRE(setOwner(a, b));
    REQUIRE(setOwner(b, c));
    CHECK(setOwner(c, a).Error() == ErrorCode::InvalidArgument);
    CHECK(setOwner(a, a).Error() == ErrorCode::InvalidArgument);
    REQUIRE(setOwner(a, ObjectHandle{}));
    REQUIRE(setOwner(c, a));

    SpawnParams params;
    params.owner = c;
    params.instigator = b;
    auto owned = world.SpawnActor<SpawnActor>(params);
    REQUIRE(owned);
    auto links = world.With<Actor>(owned.Value(), [](Actor& value) noexcept {
        return value.Owner().slot * 1000 + value.Instigator().slot;
    });
    CHECK(links.Value() == c.slot * 1000 + b.slot);

    REQUIRE(world.DestroyActor(b));
    REQUIRE(fixture.Frame());
    SpawnParams stale;
    stale.owner = b;
    CHECK(world.SpawnActor<SpawnActor>(stale).Error() == ErrorCode::StaleHandle);
    REQUIRE(world.Shutdown());
}

// 17.4절 ERD18: Actor는 켜진 Component가 모두 준비될 때까지 시작하지 않는다. 이미 Playing인 Actor에
// 나중에 추가한 Component는 자기 준비만 기다린다.
TEST(WorldSpawn_ComponentReadinessGatesStart) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActorDeferred<SpawnActor>();
    REQUIRE(actor);
    auto waiting = world.AddComponent<WaitingComponent>(actor.Value());
    REQUIRE(waiting);
    REQUIRE(world.FinishSpawning(actor.Value()));
    ResetTags();
    REQUIRE(fixture.Frame());
    REQUIRE(fixture.Frame());
    CHECK(TagsAre(""));
    fixture.SetReady(waiting.Value(), true);
    REQUIRE(fixture.Frame());
    CHECK(TagsAre("cSL"));

    auto late = world.AddComponent<WaitingComponent>(actor.Value());
    REQUIRE(late);
    ResetTags();
    REQUIRE(fixture.Frame());
    REQUIRE(fixture.Frame());
    CHECK(TagsAre("LL"));
    fixture.SetReady(late.Value(), true);
    ResetTags();
    REQUIRE(fixture.Frame());
    CHECK(TagsAre("cL"));
    REQUIRE(world.Shutdown());
}

// 17.5절 ERD16: LifeSpan은 게임 시간으로 만료되어 LateUpdate 시작(Timers)에서 파괴된다(그 프레임
// 늦은 Tick 없음). 다시 설정·해제·pause, 같은 만료 시각은 설정 순서, 비유한 값 거부.
TEST(WorldSpawn_LifeSpanExpiresAtTimersStage) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle first = fixture.Spawn('1');
    const ObjectHandle second = fixture.Spawn('2');
    const ObjectHandle kept = fixture.Spawn('k');
    REQUIRE(!first.IsNull() && !second.IsNull() && !kept.IsNull());
    REQUIRE(fixture.Frame());
    auto lifeSpan = [&](ObjectHandle actor, double seconds) noexcept {
        return world.With<Actor>(
            actor, [seconds](Actor& value) noexcept { return value.SetLifeSpan(seconds); });
    };
    // 같은 만료 시각: 먼저 설정한 second가 먼저 파괴된다.
    REQUIRE(lifeSpan(second, 2.0 / 60.0));
    REQUIRE(lifeSpan(first, 2.0 / 60.0));
    REQUIRE(lifeSpan(kept, 1.0 / 60.0));
    REQUIRE(lifeSpan(kept, 0.0));
    volatile double zero = 0.0;
    CHECK(lifeSpan(kept, 1.0 / zero).Error() == ErrorCode::InvalidArgument);
    ResetTags();
    REQUIRE(fixture.Frame());
    CHECK(TagsAre("LLL"));
    // pause 중에는 흐르지 않는다.
    world.SetPaused(true);
    ResetTags();
    REQUIRE(fixture.Frame());
    REQUIRE(fixture.Frame());
    CHECK(TagsAre(""));
    world.SetPaused(false);
    ResetTags();
    REQUIRE(fixture.Frame());
    CHECK(TagsAre("L21"));
    CHECK(!world.HasActor(first) && !world.HasActor(second) && world.HasActor(kept));
    REQUIRE(world.Shutdown());
}

} // namespace world_spawn_tests_file

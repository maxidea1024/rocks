#include <Base/InitializerList.h>
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

// 컴포넌트 기반 보강 CM0a 시험이다(Docs/Design/EngineRuntime 23.2절): 시스템 상태 수명·부분 등록
// 해제·다시 보내기, 타입 규칙(unique·required·excluded), EditorOnly.
using namespace rocks;
namespace world_component_foundation_tests_file {

namespace {

constexpr uint32 MaxEvents = 48;
const char* events[MaxEvents] = {};
uint32 eventCount = 0;

void Record(const char* text) noexcept {
    if (eventCount < MaxEvents) {
        events[eventCount++] = text;
    }
}

void ResetEvents() noexcept {
    eventCount = 0;
}

template <uint32 Count>
bool EventsAre(const char* const (&expected)[Count]) noexcept {
    bool same = eventCount == Count;
    for (uint32 index = 0; same && index < Count; ++index) {
        same = strcmp(events[index], expected[index]) == 0;
    }

    if (!same) {
        printf("  events:");
        for (uint32 index = 0; index < eventCount; ++index) {
            printf(" %s", events[index]);
        }

        printf("\n");
    }

    return same;
}

const char* StateEvent(bool create, SystemStateKind kind) noexcept {
    static const char* const created[] = {"create:Render", "create:Physics", "create:Audio"};
    static const char* const destroyed[] = {"destroy:Render", "destroy:Physics", "destroy:Audio"};
    const uint32 index = static_cast<uint32>(kind);
    return index >= SystemStateKindCount ? "?" : create ? created[index] : destroyed[index];
}

class EmptyActor final : public Actor {
    ROCKS_NATIVE_TYPE(EmptyActor, Actor, 0x6a2d9e1c4f07b385ull, 0x3e81c5b0d92a7f64ull)

public:
    explicit EmptyActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

// Render·Audio 시스템 상태를 갖는 Component다. failKind면 그 종류의 생성이 실패한다.
class StatefulComponent final : public Component {
    ROCKS_NATIVE_TYPE(StatefulComponent, Component, 0x91c7e3a05d2b4f86ull, 0x5b0f8d2e7a1c4639ull)

public:
    Result<void> OnAwake() noexcept override {
        Record("awake");
        return Result<void>::Success();
    }

    void OnEnable() noexcept override {
        Record("enable");
    }

    void OnStart() noexcept override {
        Record("start");
    }

    void OnDisable() noexcept override {
        Record("disable");
    }

    void OnDestroy(EndPlayReason) noexcept override {
        Record("destroy");
    }

    static inline bool wantPhysics = false;
    static inline int32 failKind = -1;
    static inline uint32 failOnCreateCount = 0;

protected:
    Result<void> OnRegister() noexcept override {
        Record("register");
        return Result<void>::Success();
    }

    void OnUnregister() noexcept override {
        Record("unregister");
    }

    bool WantsSystemState(SystemStateKind kind) const noexcept override {
        return kind == SystemStateKind::Render || kind == SystemStateKind::Audio ||
               (kind == SystemStateKind::Physics && wantPhysics);
    }

    Result<void> CreateSystemState(SystemStateKind kind) noexcept override {
        Record(StateEvent(true, kind));
        if (static_cast<int32>(kind) == failKind) {
            if (failOnCreateCount == 0) {
                return Result<void>::Failure(ErrorCode::DependencyFailed);
            }

            --failOnCreateCount;
        }

        return Result<void>::Success();
    }

    void DestroySystemState(SystemStateKind kind) noexcept override {
        Record(StateEvent(false, kind));
    }
};

// 규칙 시험용 Component 타입들이다.
class UniqueComponent : public Component {
    ROCKS_NATIVE_TYPE(UniqueComponent, Component, 0x2e7b4c19a0d6f853ull, 0xd94f1a63c0b8e725ull)

public:
    ComponentTypeRules TypeRules() const noexcept override {
        ComponentTypeRules rules;
        rules.unique = true;
        return rules;
    }

    Result<void> OnRegister() noexcept override {
        Record("unique.register");
        return Result<void>::Success();
    }
};

class DerivedUnique final : public UniqueComponent {
    ROCKS_NATIVE_TYPE(DerivedUnique, UniqueComponent, 0x7c0a5e2d9b14f368ull, 0x16e9b4d8f2a07c53ull)
};

class NeedsScene final : public Component {
    ROCKS_NATIVE_TYPE(NeedsScene, Component, 0xb3f61d8e2a5c0974ull, 0x48a2e0c7f19d5b36ull)

public:
    ComponentTypeRules TypeRules() const noexcept override {
        static const TypeId needs[] = {ObjectTypeIdOf<SceneComponent>()};
        ComponentTypeRules rules;
        rules.required = needs;
        return rules;
    }
};

class Blocker final : public Component {
    ROCKS_NATIVE_TYPE(Blocker, Component, 0x0d8e3b6f1c92a475ull, 0xe57c2a9d4b16f083ull)

public:
    ComponentTypeRules TypeRules() const noexcept override {
        static const TypeId blocked[] = {ObjectTypeIdOf<NeedsScene>()};
        ComponentTypeRules rules;
        rules.excluded = blocked;
        return rules;
    }
};

class EditorMarker final : public Component {
    ROCKS_NATIVE_TYPE(EditorMarker, Component, 0x5f19c0e84a7d2b36ull, 0x9b2d6e0f3c48a517ull)

public:
    bool IsEditorOnly() const noexcept override {
        return true;
    }
};

struct Fixture {
    explicit Fixture(WorldKind kind = WorldKind::Game) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && Register<EmptyActor>() &&
                     Register<StatefulComponent>() && Register<UniqueComponent>() &&
                     Register<DerivedUnique>() && Register<NeedsScene>() && Register<Blocker>() &&
                     Register<EditorMarker>();
        WorldDesc desc;
        desc.name = "Test";
        desc.context = {0x434d3061ull, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.kind = kind;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }

        StatefulComponent::wantPhysics = false;
        StatefulComponent::failKind = -1;
        StatefulComponent::failOnCreateCount = 0;
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

    bool Frame() noexcept {
        return world->Tick(1.0 / 60.0).IsOk();
    }

    static constexpr uint32 MaxTokens = 8;
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken tokens[MaxTokens];
    uint32 tokenCount = 0;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

// 23.2절 CR1: 등록 → 시스템 상태(Render → Audio) → Awake → Enable → Start → Disable → Destroy →
// 시스템 상태 역순 삭제 → OnUnregister. 종류마다 한 번씩이다.
TEST(ComponentFoundation_SystemStateLifecycleOrder) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    ResetEvents();
    auto component = world.AddComponent<StatefulComponent>(actor.Value());
    REQUIRE(component);
    REQUIRE(fixture.Frame());
    auto states = world.With<Component>(component.Value(), [](Component& value) noexcept {
        return value.HasSystemState(SystemStateKind::Render) &&
               value.HasSystemState(SystemStateKind::Audio) &&
               !value.HasSystemState(SystemStateKind::Physics);
    });
    CHECK(states.Value());
    REQUIRE(world.DestroyComponent(component.Value()));
    REQUIRE(fixture.Frame());
    const char* const expected[] = {"register",      "create:Render",  "create:Audio", "awake",
                                    "enable",        "start",          "disable",      "destroy",
                                    "destroy:Audio", "destroy:Render", "unregister"};
    CHECK(EventsAre(expected));
    REQUIRE(world.Shutdown());
}

// 23.2절 CR1: 시스템 상태 하나가 실패하면 만든 것을 역순으로 지우고 OnUnregister 뒤 등록이 실패한다
// (Awake 없음). Actor Spawn 중 실패면 그 Component만 정리되고 Spawn은 계속된다.
TEST(ComponentFoundation_SystemStateFailureRollsBackRegistration) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    StatefulComponent::wantPhysics = true;
    StatefulComponent::failKind = static_cast<int32>(SystemStateKind::Audio);
    ResetEvents();
    CHECK(world.AddComponent<StatefulComponent>(actor.Value()).Error() ==
          ErrorCode::DependencyFailed);
    const char* const rolledBack[] = {"register",     "create:Render",   "create:Physics",
                                      "create:Audio", "destroy:Physics", "destroy:Render",
                                      "unregister"};
    CHECK(EventsAre(rolledBack));
    REQUIRE(fixture.Frame());
    auto components = world.With<Actor>(
        actor.Value(), [](Actor& value) noexcept { return value.Components().Size(); });
    CHECK(components.Value() == 0);
    REQUIRE(world.Shutdown());
}

// 23.2절: 다시 보내기는 PreRender 끝에 한 번(중복 표시 무시), 없는 종류는 무시, 다시 만들기 실패는
// 그 종류 없이 남는다(Warning SystemStateRecreateFailed). 범위 밖 종류는 InvalidArgument.
TEST(ComponentFoundation_MarkSystemStateDirtyRecreatesOncePerFrame) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    auto component = world.AddComponent<StatefulComponent>(actor.Value());
    REQUIRE(component);
    REQUIRE(fixture.Frame());
    auto mark = [&](SystemStateKind kind) noexcept {
        return world.With<Component>(component.Value(), [kind](Component& value) noexcept {
            return value.MarkSystemStateDirty(kind);
        });
    };
    ResetEvents();
    REQUIRE(mark(SystemStateKind::Render));
    REQUIRE(mark(SystemStateKind::Render));
    REQUIRE(mark(SystemStateKind::Physics));
    CHECK(eventCount == 0);
    REQUIRE(fixture.Frame());
    const char* const recreated[] = {"destroy:Render", "create:Render"};
    CHECK(EventsAre(recreated));
    CHECK(mark(SystemStateKind::Count).Error() == ErrorCode::InvalidArgument);

    StatefulComponent::failKind = static_cast<int32>(SystemStateKind::Audio);
    ResetEvents();
    REQUIRE(mark(SystemStateKind::Audio));
    REQUIRE(fixture.Frame());
    const char* const failed[] = {"destroy:Audio", "create:Audio"};
    CHECK(EventsAre(failed));
    auto audio = world.With<Component>(component.Value(), [](Component& value) noexcept {
        return value.HasSystemState(SystemStateKind::Audio);
    });
    CHECK(!audio.Value());
    // 없는 Audio는 해제 때도 지우지 않는다.
    ResetEvents();
    REQUIRE(world.DestroyComponent(component.Value()));
    REQUIRE(mark(SystemStateKind::Render));
    REQUIRE(fixture.Frame());
    const char* const destroyed[] = {"disable", "destroy", "destroy:Render", "unregister"};
    CHECK(EventsAre(destroyed));
    REQUIRE(world.Shutdown());
}

// 23.2절 CR6: unique(파생 포함)·required·excluded(어느 쪽 규칙이든). 거부는 hook을 부르지 않는다.
TEST(ComponentFoundation_TypeRules) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    const ObjectHandle owner = actor.Value();
    ResetEvents();
    REQUIRE(world.AddComponent<UniqueComponent>(owner));
    CHECK(world.AddComponent<UniqueComponent>(owner).Error() == ErrorCode::AlreadyExists);
    CHECK(world.AddComponent<DerivedUnique>(owner).Error() == ErrorCode::AlreadyExists);
    const char* const once[] = {"unique.register"};
    CHECK(EventsAre(once));

    CHECK(world.AddComponent<NeedsScene>(owner).Error() == ErrorCode::DependencyFailed);
    REQUIRE(world.AddComponent<SceneComponent>(owner));
    auto needs = world.AddComponent<NeedsScene>(owner);
    REQUIRE(needs);
    // Blocker는 NeedsScene을 막는다: 있는 NeedsScene 때문에 Blocker 거부.
    CHECK(world.AddComponent<Blocker>(owner).Error() == ErrorCode::InvalidArgument);
    // 반대로 Blocker가 먼저 있으면 NeedsScene 거부(기존 Component의 규칙).
    auto other = world.SpawnActor<EmptyActor>();
    REQUIRE(other);
    REQUIRE(world.AddComponent<SceneComponent>(other.Value()));
    REQUIRE(world.AddComponent<Blocker>(other.Value()));
    CHECK(world.AddComponent<NeedsScene>(other.Value()).Error() == ErrorCode::InvalidArgument);
    // 파괴 대기 Component는 규칙에서 빠진다.
    REQUIRE(world.DestroyComponent(needs.Value()));
    REQUIRE(world.AddComponent<Blocker>(owner));
    REQUIRE(world.Shutdown());
}

// 23.2절 CR5: EditorOnly Component는 Game·Tryout World에서 Unsupported, Editor World에서는 만든다.
TEST(ComponentFoundation_EditorOnlyComponentsStayOutOfGameWorlds) {
    for (WorldKind kind :
         {WorldKind::Game, WorldKind::Tryout, WorldKind::Editor, WorldKind::Preview}) {
        Fixture fixture(kind);
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        auto actor = world.SpawnActor<EmptyActor>();
        REQUIRE(actor);
        auto added = world.AddComponent<EditorMarker>(actor.Value());
        const bool game = kind == WorldKind::Game || kind == WorldKind::Tryout;
        CHECK(game ? added.Error() == ErrorCode::Unsupported : added.IsOk());
        REQUIRE(world.Shutdown());
    }
}

} // namespace world_component_foundation_tests_file

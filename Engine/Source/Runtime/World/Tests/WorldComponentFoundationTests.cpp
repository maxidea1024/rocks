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
    static inline bool pingPong = false;
    static inline int32 failKind = -1;
    static inline uint32 failOnCreateCount = 0;
    // Create 안에서 같은 종류를 즉시 다시 만들어 본다(재진입 — Busy여야 한다).
    static inline bool recreateInside = false;
    static inline ErrorCode insideError = ErrorCode::None;

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
        if (recreateInside) {
            auto inside = RecreateSystemStateNow(kind);
            insideError = inside ? ErrorCode::None : inside.Error();
        }
        // 다시 만들기 중 다른 종류를 표시한다(Render ↔ Physics 순환 — 처리 중 표시는 다음 지점).
        if (pingPong && IsRegistered()) {
            const SystemStateKind other = kind == SystemStateKind::Render ? SystemStateKind::Physics
                                                                          : SystemStateKind::Render;
            auto marked = MarkSystemStateDirty(other);
            (void)marked;
        }

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

// unique인 SceneComponent 파생이다(기반 SceneComponent와는 겹치지 않는다).
class UniqueScene final : public SceneComponent {
    ROCKS_NATIVE_TYPE(UniqueScene, SceneComponent, 0x3a6f1e9c0b5d2487ull, 0x8e4c7b2a1f0d9356ull)

public:
    explicit UniqueScene(Allocator& allocator) noexcept : SceneComponent(allocator) {
    }

    ComponentTypeRules TypeRules() const noexcept override {
        ComponentTypeRules rules;
        rules.unique = true;
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
                     Register<EditorMarker>() && Register<UniqueScene>();
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
        StatefulComponent::pingPong = false;
        StatefulComponent::failKind = -1;
        StatefulComponent::failOnCreateCount = 0;
        StatefulComponent::recreateInside = false;
        StatefulComponent::insideError = ErrorCode::None;
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

    static constexpr uint32 MaxTokens = 9;
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

// 23.2절 즉시 다시 만들기: 그 자리에서 Destroy → (원하면) Create, 없던 상태도 만들고 원하지 않으면
// 지우기만 한다. 대기 표시는 지워져 갱신 지점에서 두 번 하지 않는다. 실패는 그 종류 없이 오류,
// 같은 종류 재진입은 Busy, 범위 밖은 InvalidArgument, 파괴 대기는 아무 일 없음.
TEST(ComponentFoundation_RecreateSystemStateNow) {
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
    auto recreate = [&](SystemStateKind kind) noexcept {
        return world.With<Component>(component.Value(), [kind](Component& value) noexcept {
            return value.RecreateSystemStateNow(kind);
        });
    };
    auto mark = [&](SystemStateKind kind) noexcept {
        return world.With<Component>(component.Value(), [kind](Component& value) noexcept {
            return value.MarkSystemStateDirty(kind);
        });
    };
    auto has = [&](SystemStateKind kind) noexcept {
        auto read = world.With<Component>(component.Value(), [kind](Component& value) noexcept {
            return value.HasSystemState(kind);
        });
        return read && read.Value();
    };

    // 표시한 뒤 즉시 다시 만들면 그 자리에서 한 번, 갱신 지점에서는 다시 하지 않는다.
    ResetEvents();
    REQUIRE(mark(SystemStateKind::Render));
    REQUIRE(recreate(SystemStateKind::Render));
    const char* const now[] = {"destroy:Render", "create:Render"};
    CHECK(EventsAre(now));
    REQUIRE(fixture.Frame());
    CHECK(EventsAre(now));
    // 목록 표시는 따로라 다음 표시도 한 번만 처리한다.
    ResetEvents();
    REQUIRE(mark(SystemStateKind::Render));
    REQUIRE(mark(SystemStateKind::Render));
    REQUIRE(fixture.Frame());
    CHECK(EventsAre(now));

    // 원하게 되면 없던 상태를 만들고, 원하지 않게 되면 지우기만 한다.
    ResetEvents();
    StatefulComponent::wantPhysics = true;
    REQUIRE(recreate(SystemStateKind::Physics));
    CHECK(has(SystemStateKind::Physics));
    StatefulComponent::wantPhysics = false;
    REQUIRE(recreate(SystemStateKind::Physics));
    CHECK(!has(SystemStateKind::Physics));
    const char* const physics[] = {"create:Physics", "destroy:Physics"};
    CHECK(EventsAre(physics));

    // 실패: 그 종류 없이 오류.
    StatefulComponent::failKind = static_cast<int32>(SystemStateKind::Audio);
    auto failed = recreate(SystemStateKind::Audio);
    CHECK(!failed && failed.Error() == ErrorCode::DependencyFailed);
    CHECK(!has(SystemStateKind::Audio));
    StatefulComponent::failKind = -1;
    REQUIRE(recreate(SystemStateKind::Audio));
    CHECK(has(SystemStateKind::Audio));

    // 재진입: Create 안의 같은 종류는 Busy, 바깥은 성공.
    StatefulComponent::recreateInside = true;
    REQUIRE(recreate(SystemStateKind::Render));
    StatefulComponent::recreateInside = false;
    CHECK(StatefulComponent::insideError == ErrorCode::Busy);
    CHECK(has(SystemStateKind::Render));

    // 갱신 지점의 Create 안에서 같은 종류도 Busy다(이중 생성 없음).
    ResetEvents();
    REQUIRE(mark(SystemStateKind::Render));
    StatefulComponent::recreateInside = true;
    StatefulComponent::insideError = ErrorCode::None;
    REQUIRE(fixture.Frame());
    StatefulComponent::recreateInside = false;
    CHECK(StatefulComponent::insideError == ErrorCode::Busy);
    CHECK(EventsAre(now));

    // 표시 → 즉시 다시 만들기(비트 0) → 같은 프레임 다시 표시: 목록에는 한 번이라, 처리 중 hook이
    // 남긴 표시(pingPong)는 그 지점에서 다시 처리되지 않는다.
    StatefulComponent::wantPhysics = true;
    REQUIRE(recreate(SystemStateKind::Physics));
    StatefulComponent::pingPong = true;
    REQUIRE(mark(SystemStateKind::Render));
    REQUIRE(recreate(SystemStateKind::Render));
    REQUIRE(mark(SystemStateKind::Render));
    ResetEvents();
    REQUIRE(fixture.Frame());
    const char* const onePass[] = {"destroy:Render", "create:Render", "destroy:Physics",
                                   "create:Physics"};
    CHECK(EventsAre(onePass));
    StatefulComponent::pingPong = false;
    REQUIRE(fixture.Frame());
    REQUIRE(fixture.Frame());
    StatefulComponent::wantPhysics = false;
    REQUIRE(recreate(SystemStateKind::Physics));
    CHECK(recreate(SystemStateKind::Count).Error() == ErrorCode::InvalidArgument);

    // 파괴 대기면 아무 일도 없다.
    REQUIRE(world.DestroyComponent(component.Value()));
    ResetEvents();
    auto pending = recreate(SystemStateKind::Render);
    CHECK(pending);
    CHECK(eventCount == 0);
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

// 리뷰 보강(23.2절): unique는 기반 타입과 겹치지 않는다(기반이 먼저든 나중이든).
TEST(ComponentFoundation_UniqueDoesNotCoverBaseTypes) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto first = world.SpawnActor<EmptyActor>();
    auto second = world.SpawnActor<EmptyActor>();
    REQUIRE(first && second);
    REQUIRE(world.AddComponent<SceneComponent>(first.Value()));
    REQUIRE(world.AddComponent<UniqueScene>(first.Value()));
    CHECK(world.AddComponent<UniqueScene>(first.Value()).Error() == ErrorCode::AlreadyExists);
    REQUIRE(world.AddComponent<UniqueScene>(second.Value()));
    REQUIRE(world.AddComponent<SceneComponent>(second.Value()));
    REQUIRE(world.Shutdown());
}

// 리뷰 보강(23.2절): 다시 만들기 hook이 다른 종류를 표시하면 다음 지점에 처리한다(순환해도
// 프레임마다 한 번).
TEST(ComponentFoundation_DirtyMarksDuringFlushWaitForTheNextFrame) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    StatefulComponent::wantPhysics = true;
    StatefulComponent::pingPong = true;
    auto component = world.AddComponent<StatefulComponent>(actor.Value());
    REQUIRE(component);
    REQUIRE(fixture.Frame());
    REQUIRE(world.With<Component>(component.Value(), [](Component& value) noexcept {
        return value.MarkSystemStateDirty(SystemStateKind::Render);
    }));
    ResetEvents();
    REQUIRE(fixture.Frame());
    const char* const first[] = {"destroy:Render", "create:Render"};
    CHECK(EventsAre(first));
    ResetEvents();
    REQUIRE(fixture.Frame());
    const char* const second[] = {"destroy:Physics", "create:Physics"};
    CHECK(EventsAre(second));
    StatefulComponent::pingPong = false;
    REQUIRE(world.Shutdown());
}

// 리뷰 보강(23.2절): FinishSpawning의 Component 등록 루프에서도 시스템 상태 실패는 그 Component만
// 되돌리고 Spawn은 성공한다.
TEST(ComponentFoundation_SystemStateFailureDuringFinishSpawning) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActorDeferred<EmptyActor>();
    REQUIRE(actor);
    REQUIRE(world.AddComponent<StatefulComponent>(actor.Value()));
    StatefulComponent::failKind = static_cast<int32>(SystemStateKind::Render);
    ResetEvents();
    REQUIRE(world.FinishSpawning(actor.Value()));
    const char* const rolledBack[] = {"register", "create:Render", "unregister"};
    CHECK(EventsAre(rolledBack));
    REQUIRE(fixture.Frame());
    auto components = world.With<Actor>(
        actor.Value(), [](Actor& value) noexcept { return value.Components().Size(); });
    CHECK(components.Value() == 0);
    CHECK(world.GetActorState(actor.Value()).Value() == ActorState::Initialized);
    REQUIRE(world.Shutdown());
}
} // namespace world_component_foundation_tests_file

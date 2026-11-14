#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>
#include <World/SceneComponent.h>
#include <World/World.h>
#include <World/World.module.generated.h>
#include <WorldTestFixtures/WorldTestFixtures.module.generated.h>

// World 타입 리플렉션 시험이다(Docs/Design/ReflectionFoundations.ko.md 9.1절 B8b "시험").
// Level·Actor·Component·SceneComponent를 생성 코드(ROCKS_CLASS(WorldOnly))로 등록하고 다른 모듈
// fixture(WorldTestFixtures — SaveActor : Actor, SaveComponent : SceneComponent)와 함께 등록 형태,
// GC(생성 Trace·root 참조·Marking 중 newborn shade와 삽입 장벽), Level 그래프 저장·로드·재저장,
// 장부 밖 객체의 API 거부, null 원소 순회, 경로 접근을 본다. native 시험 파생이 필요한 GC·저장
// 시험은 WorldTests.cpp에 있다.
using namespace rocks;
using world_fixture::SaveActor;
using world_fixture::SaveComponent;

namespace {

// 로드된 목록의 null 원소를 흉내 내는 탐침이다. World 필드는 ReadOnly라 경로로 null을 넣을 수 없고
// World API는 null을 넣지 않으므로, ROCKS_GENERATED_BODY가 모든 GeneratedType 특수화를 friend로
// 두는 점을 써 시험에서만 private 목록에 Registry 컨테이너 API로 null을 넣는다.
struct NullSlotProbe {};

// hook 호출 수를 세는 native Component다(null 원소 순회 시험이 활성화·시작 순회를 구분한다).
class HookCounter final : public Component {
    ROCKS_NATIVE_TYPE(HookCounter, Component, 0x5446d4231cba4af9ull, 0xb767b4a0e73a19daull)

public:
    void OnEnable() noexcept override {
        ++enables;
    }

    void OnStart() noexcept override {
        ++starts;
    }

    static inline uint32 enables = 0;
    static inline uint32 starts = 0;
};

} // namespace

namespace rocks {

template <>
struct GeneratedType<NullSlotProbe> {
    static Result<void> InsertNull(World& world, ObjectHandle actor, usize index) noexcept {
        return world.With<Actor>(actor, [&](Actor& owner) noexcept {
            return world.Objects().InsertReferenceAt(world.Access(), actor, owner.components_,
                                                     index, ObjectHandle{});
        });
    }
};

} // namespace rocks

namespace {

// 리플렉션 World 시험 환경이다. 수명 순서는 Engine과 같다: World(ObjectRegistry) ->
// ModuleRegistration -> TypeRegistry(멤버 선언 역순으로 소멸). native 타입을 등록하지 않으므로
// World 타입을 Unregister할 수 있다.
struct ReflectedWorld {
    explicit ReflectedWorld(uint64 context) noexcept : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() &&
                     generated::WorldTestFixtures::RegisterModuleTypes(modules).IsOk();
        WorldDesc desc;
        desc.name = "Reflection";
        desc.context = {0x5752464c43ull, context};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~ReflectedWorld() {
        world.Reset();
        CHECK(modules.Unregister().IsOk());
    }

    bool Ready() const noexcept {
        return registered && world.Get() != nullptr;
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    bool registered = false;
    UniquePtr<World> world;
};

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 실효 도구 이름(Name 지정자, 없으면 C++ 이름)이 name인 필드다.
const FieldSchema* FindField(const TypeSchema& schema, const char* name) noexcept {
    const StringView wanted(name);
    for (const FieldSchema& field : schema.fields) {
        const Span<const char> tool = EffectiveToolName(field);
        if (StringView(tool.Data(), tool.Size()) == wanted) {
            return &field;
        }
    }

    return nullptr;
}

// 진행 중인 증분 수집을 끝내고 회수할 것이 없을 때까지 전체 수집한다(World::Shutdown과 같은 순서).
bool CollectAll(World& world) noexcept {
    ObjectRegistry& objects = world.Objects();
    const ObjectAccessContext access = world.Access();
    if (objects.CollectionPhase() == GcPhase::Marking && !objects.CancelCollection(access)) {
        return false;
    }

    for (uint32 attempt = 0; attempt < 64 && objects.CollectionPhase() != GcPhase::Idle;
         ++attempt) {
        if (!objects.StepCollection(access, GcBudget{1u << 20, 0})) {
            return false;
        }
    }

    for (uint32 attempt = 0; attempt < 8; ++attempt) {
        auto collected = objects.Collect(access);
        if (!collected) {
            return false;
        }

        if (collected.Value().reclaimed == 0) {
            return true;
        }
    }

    return false;
}

// 증분 수집을 객체 하나만큼 진행한다.
bool StepOnce(World& world) noexcept {
    GcBudget budget;
    budget.maxObjects = 1;
    budget.maxNanoseconds = 0;
    return static_cast<bool>(world.Objects().StepCollection(world.Access(), budget));
}

// Trace가 방문한 handle과 횟수다(시험 객체 수는 작다 — 넘치면 Fatal).
struct VisitLog {
    ObjectHandle handles[8];
    uint32 counts[8] = {};
    usize size = 0;
    uint32 total = 0;

    static void Record(void* state, ObjectHandle handle) noexcept {
        auto& log = *static_cast<VisitLog*>(state);
        ++log.total;
        for (usize index = 0; index < log.size; ++index) {
            if (log.handles[index] == handle) {
                ++log.counts[index];
                return;
            }
        }

        if (log.size == 8) {
            platform::Fatal("too many traced handles");
        }

        log.handles[log.size] = handle;
        log.counts[log.size++] = 1;
    }

    uint32 CountOf(ObjectHandle handle) const noexcept {
        for (usize index = 0; index < size; ++index) {
            if (handles[index] == handle) {
                return counts[index];
            }
        }

        return 0;
    }
};

Result<void> SetRoot(World& world, ObjectHandle actor, ObjectHandle component) noexcept {
    return world.With<Actor>(
        actor, [component](Actor& value) noexcept { return value.SetRootComponent(component); });
}

ObjectHandle RootOf(World& world, ObjectHandle actor) noexcept {
    auto root =
        world.With<Actor>(actor, [](Actor& value) noexcept { return value.RootComponent(); });
    return root ? root.Value() : ObjectHandle{};
}

FieldValue BoolValue(Allocator& allocator, bool value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Bool;
    result.boolean = value;
    return result;
}

FieldValue Int32Value(Allocator& allocator, int32 value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Int32;
    result.signedInteger = value;
    return result;
}

} // namespace

// 네 World 타입은 생성 코드로 등록한 WorldOnly 리플렉션 클래스이고(SceneComponent는 상속), 필드는
// 표대로 ReadOnly + 도구 이름이다. 다른 모듈 fixture는 상속 필드를 같은 ID로 담고 WorldOnly를
// 물려받는다. 등록은 멱등이다(같은 registration은 다시 등록하지 않고, 다른 registration은 lease를
// 붙잡는다).
TEST(WorldReflection_TypesAreWorldOnlyReflectedClasses) {
    ReflectedWorld fixture(1);
    REQUIRE(fixture.Ready());
    TypeRegistry& types = fixture.types;
    const uint32 readOnly = static_cast<uint32>(FieldFlags::ReadOnly);
    const uint32 required = static_cast<uint32>(FieldFlags::Required);
    struct Expected {
        TypeId id;
        TypeId base;
        usize fields;
    };

    const Expected expected[] = {
        {ObjectTypeIdOf<Level>(), TypeId{}, 2},
        {ObjectTypeIdOf<Actor>(), TypeId{}, 3},
        {ObjectTypeIdOf<Component>(), TypeId{}, 1},
        {ObjectTypeIdOf<SceneComponent>(), ObjectTypeIdOf<Component>(), 2},
        {ObjectTypeIdOf<SaveActor>(), ObjectTypeIdOf<Actor>(), 6},
        {ObjectTypeIdOf<SaveComponent>(), ObjectTypeIdOf<SceneComponent>(), 4},
    };
    for (const Expected& entry : expected) {
        auto lease = types.Acquire(entry.id);
        REQUIRE(lease);
        const TypeSchema& schema = lease.Value().Schema();
        CHECK(HasFlag(schema.flags, TypeFlags::WorldOnly));
        CHECK(!HasFlag(schema.flags, TypeFlags::Abstract));
        CHECK(schema.baseType == entry.base);
        CHECK(schema.fields.Size() == entry.fields);
        CHECK(lease.Value().Ops().completeFieldLayout);
        CHECK(lease.Value().Ops().parallelTraceSafe);
    }

    {
        auto level = types.Acquire(ObjectTypeIdOf<Level>());
        REQUIRE(level);
        const FieldSchema* actors = FindField(level.Value().Schema(), "actors");
        const FieldSchema* name = FindField(level.Value().Schema(), "name");
        REQUIRE(actors != nullptr && name != nullptr);
        CHECK(actors->value.kind == ValueKind::Array);
        CHECK(actors->element.kind == ValueKind::ObjectReference);
        CHECK(actors->element.type == ObjectTypeIdOf<Actor>());
        CHECK(actors->flags == (readOnly | required));
        CHECK(name->value.kind == ValueKind::Utf8String && name->flags == readOnly);
        // 도구 이름이 있으면 C++ 이름은 실효 이름이 아니다.
        CHECK(FindField(level.Value().Schema(), "actors_") == nullptr);
    }

    Guid componentsId{};
    {
        auto actor = types.Acquire(ObjectTypeIdOf<Actor>());
        REQUIRE(actor);
        const FieldSchema* components = FindField(actor.Value().Schema(), "components");
        const FieldSchema* root = FindField(actor.Value().Schema(), "rootComponent");
        const FieldSchema* enabled = FindField(actor.Value().Schema(), "enabled");
        REQUIRE(components != nullptr && root != nullptr && enabled != nullptr);
        CHECK(components->value.kind == ValueKind::Array);
        CHECK(components->element.type == ObjectTypeIdOf<Component>());
        CHECK(components->flags == (readOnly | required));
        CHECK(root->kind == FieldKind::ObjectReference);
        CHECK(root->value.kind == ValueKind::ObjectReference);
        CHECK(root->value.type == ObjectTypeIdOf<SceneComponent>());
        CHECK(root->flags == readOnly);
        CHECK(enabled->value.kind == ValueKind::Bool && enabled->flags == readOnly);
        componentsId = components->id;
    }

    Guid enabledId{};
    {
        auto component = types.Acquire(ObjectTypeIdOf<Component>());
        REQUIRE(component);
        const FieldSchema* enabled = FindField(component.Value().Schema(), "enabled");
        REQUIRE(enabled != nullptr);
        CHECK(enabled->value.kind == ValueKind::Bool && enabled->flags == readOnly);
        enabledId = enabled->id;
    }

    {
        auto scene = types.Acquire(ObjectTypeIdOf<SceneComponent>());
        REQUIRE(scene);
        const FieldSchema* relative = FindField(scene.Value().Schema(), "relativeTransform");
        const FieldSchema* enabled = FindField(scene.Value().Schema(), "enabled");
        REQUIRE(relative != nullptr && enabled != nullptr);
        CHECK(relative->value.kind == ValueKind::Struct);
        CHECK(relative->value.type == GeneratedType<Transform>::Id());
        CHECK(relative->flags == readOnly);
        CHECK(enabled->id == enabledId);
    }

    {
        // 다른 모듈 파생은 상속 필드를 같은 ID로 담는다(import descriptor).
        auto saver = types.Acquire(ObjectTypeIdOf<SaveActor>());
        REQUIRE(saver);
        const FieldSchema* components = FindField(saver.Value().Schema(), "components");
        REQUIRE(components != nullptr);
        CHECK(components->id == componentsId);
        CHECK(FindField(saver.Value().Schema(), "score") != nullptr);
        CHECK(FindField(saver.Value().Schema(), "asset") != nullptr);
    }

    const TypeStats before = types.Stats();
    const usize records = fixture.modules.Size();
    REQUIRE(RegisterWorldTypes(fixture.modules));
    CHECK(fixture.modules.Size() == records);
    {
        ModuleRegistration other(types, fixture.allocator);
        REQUIRE(RegisterWorldTypes(other));
        CHECK(other.Size() != 0);
        CHECK(types.Stats().registeredTypes == before.registeredTypes);
        CHECK(types.Stats().leases > before.leases);
    }

    CHECK(types.Stats().leases == before.leases);
}

// Spawn한 Actor와 Component는 Level -> Actor -> Component 생성 Trace 간선으로 살아 있고(World
// 루트는 Level뿐), 파괴한 Actor와 그 Component는 정리 뒤 회수된다.
TEST(WorldReflection_GcKeepsSpawnedObjectsAndReclaimsDestroyedOnes) {
    ReflectedWorld fixture(2);
    REQUIRE(fixture.Ready());
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    constexpr uint32 ActorCount = 4;
    ObjectHandle actors[ActorCount];
    ObjectHandle roots[ActorCount];
    for (uint32 index = 0; index < ActorCount; ++index) {
        auto actor = world.SpawnActorDeferred<SaveActor>();
        REQUIRE(actor);
        auto root = world.AddComponent<SceneComponent>(actor.Value());
        auto extra = world.AddComponent<SaveComponent>(actor.Value());
        REQUIRE(root && extra);
        REQUIRE(SetRoot(world, actor.Value(), root.Value()));
        actors[index] = actor.Value();
        roots[index] = root.Value();
    }

    REQUIRE(CollectAll(world));
    CHECK(world.Objects().Stats().roots == 1);
    CHECK(world.Objects().Stats().liveObjects == 1 + ActorCount * 3);
    REQUIRE(world.DestroyActor(actors[0]));
    REQUIRE(world.Tick(0.016));
    REQUIRE(CollectAll(world));
    CHECK(world.Objects().Stats().liveObjects == 1 + (ActorCount - 1) * 3);
    CHECK(world.Resolve<Actor>(actors[0]).Error() == ErrorCode::StaleHandle);
    CHECK(world.Resolve<SceneComponent>(roots[0]).Error() == ErrorCode::StaleHandle);
    for (uint32 index = 1; index < ActorCount; ++index) {
        CHECK(world.Resolve<SaveActor>(actors[index]).IsOk());
        CHECK(RootOf(world, actors[index]) == roots[index]);
    }

    REQUIRE(world.Shutdown());
}

// Marking 중 AddComponent로 만든 Component는 그 cycle에서 회수되지 않는다: 새 객체는 생성 Commit이
// Marking이면 newborn으로 shade하므로(소유 Actor가 이미 검게 스캔됐어도) 산다. 진행 단계 수를 늘려
// 가며 Actor들이 스캔 전·후인 모든 지점에서 넣는다. 기존(흰) 객체를 검은 객체의 필드에 넣는 삽입
// 장벽은 WorldReflection_MarkingReferenceWriteIsShadedByBarrier가 본다.
TEST(WorldReflection_MarkingAddComponentSurvivesAsShadedNewborn) {
    ReflectedWorld fixture(3);
    REQUIRE(fixture.Ready());
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    constexpr uint32 ActorCount = 6;
    ObjectHandle actors[ActorCount];
    for (ObjectHandle& actor : actors) {
        auto spawned = world.SpawnActorDeferred<Actor>();
        REQUIRE(spawned);
        actor = spawned.Value();
    }

    // Level과 Actor 수의 세 배까지 단계를 늘린다(넣을 때마다 객체가 늘어 Marking이 끝나기 전에
    // 넣는다).
    ObjectRegistry& objects = world.Objects();
    uint32 added = 0;
    for (uint32 steps = 0; steps <= 3 * (ActorCount + 1); ++steps) {
        REQUIRE(CollectAll(world));
        REQUIRE(objects.BeginCollection(world.Access()));
        bool marking = true;
        for (uint32 step = 0; step < steps && marking; ++step) {
            REQUIRE(StepOnce(world));
            marking = objects.CollectionPhase() == GcPhase::Marking;
        }

        if (!marking) {
            break;
        }

        auto component = world.AddComponent<SceneComponent>(actors[steps % ActorCount]);
        REQUIRE(component);
        ++added;
        for (uint32 guard = 0; guard < 64 && objects.CollectionPhase() != GcPhase::Idle; ++guard) {
            REQUIRE(objects.StepCollection(world.Access(), GcBudget{1u << 20, 0}));
        }

        REQUIRE(objects.CollectionPhase() == GcPhase::Idle);
        CHECK(world.Resolve<SceneComponent>(component.Value()).IsOk());
        CHECK(objects.Stats().liveObjects == 1 + ActorCount + added);
    }

    // Level·Actor 모두가 스캔된 뒤까지 넣어 봤다(스캔 순서와 무관하게 검은 Actor가 있었다).
    CHECK(added > ActorCount);
    REQUIRE(CollectAll(world));
    CHECK(objects.Stats().liveObjects == 1 + ActorCount + added);
    REQUIRE(world.Shutdown());
}

// Marking 중 리플렉션 참조 필드 쓰기(Registry SetReference)는 삽입 장벽을 거친다: 수집 시작 전에
// 만든 흰 객체를 이미 검게 스캔된 Component의 필드(SaveComponent::sibling)에 넣고, 같은 cycle
// 안에서 그 객체의 다른 유일한 경로(아직 스캔되지 않은 Component의 sibling)를 끊어도 회수되지
// 않는다. 진행 단계 수를 늘려 가며 앞 Component는 검고 뒤 Component는 아직 스캔 전인 지점을
// 지난다(그 밖의 지점에서는 장벽 없이도 산다 — 앞 Component가 나중에 스캔되거나 뒤 Component가 이미
// 대상을 shade했다).
TEST(WorldReflection_MarkingReferenceWriteIsShadedByBarrier) {
    ReflectedWorld fixture(10);
    REQUIRE(fixture.Ready());
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto front = world.SpawnActorDeferred<Actor>();
    auto back = world.SpawnActorDeferred<Actor>();
    REQUIRE(front && back);
    auto early = world.AddComponent<SaveComponent>(front.Value());
    auto late = world.AddComponent<SaveComponent>(back.Value());
    REQUIRE(early && late);
    ObjectRegistry& objects = world.Objects();
    const ObjectAccessContext access = world.Access();
    auto link = [&](ObjectHandle owner, ObjectHandle target) noexcept {
        return world.With<SaveComponent>(owner, [&](SaveComponent& component) noexcept {
            return objects.SetReference(access, owner, component.sibling, target);
        });
    };

    uint32 rounds = 0;
    for (uint32 steps = 0; steps < 16; ++steps) {
        // 이번 cycle의 대상: World 장부 밖의 SaveComponent를 만들어 뒤 Component의 sibling으로만
        // 닿게 한다(루트는 수집 시작 전에 놓는다 — 시작할 때 흰 기존 객체).
        ObjectHandle target;
        {
            auto created =
                objects.NewObject<SaveComponent>(access, ObjectTypeIdOf<SaveComponent>());
            REQUIRE(created);
            target = created.Value().Handle();
            REQUIRE(link(late.Value(), target));
            REQUIRE(link(early.Value(), ObjectHandle{}));
        }

        REQUIRE(CollectAll(world));
        REQUIRE(world.Resolve<SaveComponent>(target).IsOk());
        REQUIRE(objects.BeginCollection(access));
        bool marking = true;
        for (uint32 step = 0; step < steps && marking; ++step) {
            REQUIRE(StepOnce(world));
            marking = objects.CollectionPhase() == GcPhase::Marking;
        }

        if (!marking) {
            break;
        }

        // 앞 Component의 필드에 넣고(장벽) 뒤 Component의 경로를 끊는다(제거에는 장벽이 없다).
        REQUIRE(link(early.Value(), target));
        REQUIRE(link(late.Value(), ObjectHandle{}));
        ++rounds;
        for (uint32 guard = 0; guard < 64 && objects.CollectionPhase() != GcPhase::Idle; ++guard) {
            REQUIRE(objects.StepCollection(access, GcBudget{1u << 20, 0}));
        }

        REQUIRE(objects.CollectionPhase() == GcPhase::Idle);
        CHECK(world.Resolve<SaveComponent>(target).IsOk());
        // Level·Actor 둘·Component 둘과 대상 하나만 산다(앞 cycle의 대상은 회수됐다).
        CHECK(objects.Stats().liveObjects == 6);
    }

    // FIFO 스캔 순서(Level, 앞 Actor, 뒤 Actor, 앞 Component, 뒤 Component)의 모든 지점을 지났다.
    CHECK(rounds >= 5);
    // 마지막 대상(Marking이 끝나 넣지 않은 cycle의 대상 포함)을 모두 끊으면 회수된다.
    REQUIRE(link(early.Value(), ObjectHandle{}));
    REQUIRE(link(late.Value(), ObjectHandle{}));
    REQUIRE(CollectAll(world));
    CHECK(objects.Stats().liveObjects == 5);
    REQUIRE(world.Shutdown());
}

// rootComponent는 영속 강한 참조다: 생성 Trace가 components와 함께 방문하고(root는 늘 components
// 안이라 두 번), 소유하지 않거나 SceneComponent가 아니면 거부하며, root Component를 파괴하면 대표가
// 비어 저장소가 회수된다(강한 참조가 좀비를 남기지 않는다).
TEST(WorldReflection_RootComponentIsTracedAndClearedWithItsComponent) {
    ReflectedWorld fixture(4);
    REQUIRE(fixture.Ready());
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActorDeferred<Actor>();
    auto stranger = world.SpawnActorDeferred<Actor>();
    REQUIRE(actor && stranger);
    auto root = world.AddComponent<SceneComponent>(actor.Value());
    auto other = world.AddComponent<SceneComponent>(actor.Value());
    auto plain = world.AddComponent<Component>(actor.Value());
    auto foreign = world.AddComponent<SceneComponent>(stranger.Value());
    REQUIRE(root && other && plain && foreign);
    CHECK(ErrorOf(SetRoot(world, actor.Value(), plain.Value())) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(SetRoot(world, actor.Value(), foreign.Value())) == ErrorCode::InvalidArgument);
    CHECK(RootOf(world, actor.Value()).IsNull());
    REQUIRE(SetRoot(world, actor.Value(), root.Value()));
    CHECK(RootOf(world, actor.Value()) == root.Value());
    {
        auto lease = fixture.types.Acquire(ObjectTypeIdOf<Actor>());
        auto borrowed = world.Resolve<Actor>(actor.Value());
        REQUIRE(lease && borrowed);
        Actor* pointer = Get(borrowed.Value());
        REQUIRE(pointer != nullptr);
        VisitLog log;
        TraceVisitor visitor(&log, &VisitLog::Record);
        lease.Value().Ops().trace(*pointer, visitor);
        CHECK(log.total == 4);
        CHECK(log.CountOf(root.Value()) == 2);
        CHECK(log.CountOf(other.Value()) == 1 && log.CountOf(plain.Value()) == 1);
    }

    REQUIRE(world.DestroyComponent(root.Value()));
    REQUIRE(world.Tick(0.016));
    CHECK(RootOf(world, actor.Value()).IsNull());
    REQUIRE(CollectAll(world));
    CHECK(world.Resolve<SceneComponent>(root.Value()).Error() == ErrorCode::StaleHandle);
    CHECK(world.Objects().Stats().liveObjects == 1 + 2 + 3);
    REQUIRE(SetRoot(world, actor.Value(), other.Value()));
    REQUIRE(SetRoot(world, actor.Value(), ObjectHandle{}));
    CHECK(RootOf(world, actor.Value()).IsNull());
    REQUIRE(world.Shutdown());
}

// Level 그래프 저장(구조 시험): Level을 저장해 같은 TypeRegistry의 별도 Registry에 로드하면
// 필드·간선(components·sibling)·root·소프트 참조가 돌아오고 다시 저장한 byte가 같다. NATIVE 멤버는
// 기본값이고 부착 계층(parent·children)은 저장되지 않는다. 로드한 객체는 World 장부 밖이라
// 계층·World API가 InvalidArgument다(상대 Transform 설정과 월드 행렬 계산은 된다).
TEST(WorldReflection_LevelGraphRoundTripsWithoutHierarchy) {
    ReflectedWorld fixture(5);
    REQUIRE(fixture.Ready());
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto level = world.CreateLevel("Saved");
    REQUIRE(level);
    SpawnParams params;
    params.level = level.Value();
    auto saver = world.SpawnActorDeferred<SaveActor>(params);
    auto plain = world.SpawnActorDeferred<Actor>(params);
    REQUIRE(saver && plain);
    auto first = world.AddComponent<SaveComponent>(saver.Value());
    auto second = world.AddComponent<SaveComponent>(saver.Value());
    auto scene = world.AddComponent<SceneComponent>(saver.Value());
    auto bare = world.AddComponent<Component>(plain.Value());
    REQUIRE(first && second && scene && bare);
    const AssetId asset{Guid{0x0a1b2c3d4e5f6071ull, 0x8293a4b5c6d7e8f9ull}};
    auto filled = world.With<SaveActor>(saver.Value(), [&](SaveActor& actor) noexcept {
        actor.score = 42;
        actor.lease.token = 77;
        if (auto named = actor.label.TryAssign("저장 시험"); !named) {
            return named;
        }

        if (auto soft = actor.asset.TryAssign(asset, "Body.Socket"); !soft) {
            return soft;
        }

        if (auto disabled = actor.SetEnabled(false); !disabled) {
            return disabled;
        }

        return actor.SetRootComponent(second.Value());
    });
    REQUIRE(filled);
    auto linked = world.With<SaveComponent>(first.Value(), [&](SaveComponent& component) noexcept {
        if (auto pushed = component.values.TryPushBack(int32{-7}); !pushed) {
            return pushed;
        }

        if (auto pushed = component.values.TryPushBack(int32{11}); !pushed) {
            return pushed;
        }

        if (auto moved = component.SetRelativeLocation(Vec3d(1.0, 2.0, 3.0)); !moved) {
            return moved;
        }

        return world.Objects().SetReference(world.Access(), first.Value(), component.sibling,
                                            second.Value());
    });
    REQUIRE(linked);
    auto quiet = world.With<SaveComponent>(second.Value(), [](SaveComponent& component) noexcept {
        return component.SetEnabled(false);
    });
    REQUIRE(quiet);
    auto attached =
        world.With<SceneComponent>(scene.Value(), [&](SceneComponent& component) noexcept {
            if (auto scaled = component.SetRelativeScale(Vec3(2.0f, 2.0f, 2.0f)); !scaled) {
                return scaled;
            }

            return component.AttachTo(first.Value());
        });
    REQUIRE(attached);

    // 저장 안전 지점(Tick 밖·정리 대기 없음)에서 Level을 루트로 저장한다.
    Array<uint8> saved(fixture.allocator);
    {
        auto bytes =
            SaveObjectGraph(fixture.allocator, world.Objects(), world.Access(), level.Value());
        REQUIRE(bytes);
        saved = Move(bytes.Value());
    }

    {
        ObjectRegistry loaded(fixture.allocator, fixture.types, {0x574c4f4144ull, 5});
        const auto access = loaded.OwnerAccess();
        {
            auto root =
                LoadObjectGraph(fixture.allocator, loaded, fixture.types, access, saved.View());
            REQUIRE(root);
            ObjectHandle loadedSaver;
            ObjectHandle loadedPlain;
            {
                auto borrowed = loaded.Resolve<Level>(access, root.Value().Handle());
                REQUIRE(borrowed);
                Level* pointer = Get(borrowed.Value());
                REQUIRE(pointer != nullptr);
                CHECK(pointer->Name() == StringView("Saved"));
                CHECK(!pointer->IsPersistent() && !pointer->IsPendingDestroy());
                REQUIRE(pointer->Actors().Size() == 2);
                loadedSaver = pointer->Actors()[0];
                loadedPlain = pointer->Actors()[1];
            }

            ObjectHandle components[3];
            {
                auto borrowed = loaded.Resolve<SaveActor>(access, loadedSaver);
                REQUIRE(borrowed);
                SaveActor* actor = Get(borrowed.Value());
                REQUIRE(actor != nullptr);
                CHECK(actor->score == 42);
                CHECK(actor->label.View() == StringView("저장 시험"));
                CHECK(actor->asset.asset == asset);
                CHECK(actor->asset.objectPath.View() == StringView("Body.Socket"));
                CHECK(!actor->IsEnabled());
                // NATIVE 멤버는 저장하지 않으므로 기본값이다.
                CHECK(actor->lease.token == 0);
                CHECK(!actor->HasWorld() && actor->GetLevel().IsNull());
                CHECK(actor->GetState() == ActorState::Constructed);
                REQUIRE(actor->Components().Size() == 3);
                for (usize index = 0; index < 3; ++index) {
                    components[index] = actor->Components()[index];
                }

                CHECK(actor->RootComponent() == components[1]);
                // 장부 밖 Actor의 World API는 Fatal 대신 InvalidArgument다.
                CHECK(ErrorOf(actor->SetRootComponent(components[0])) ==
                      ErrorCode::InvalidArgument);
                CHECK(ErrorOf(actor->SetRootComponent(ObjectHandle{})) ==
                      ErrorCode::InvalidArgument);
                CHECK(ErrorOf(actor->Destroy()) == ErrorCode::InvalidArgument);
                CHECK(actor->RootComponent() == components[1]);
            }

            {
                auto borrowed = loaded.Resolve<SaveComponent>(access, components[0]);
                REQUIRE(borrowed);
                SaveComponent* component = Get(borrowed.Value());
                REQUIRE(component != nullptr);
                REQUIRE(component->values.Size() == 2);
                CHECK(component->values[0] == -7 && component->values[1] == 11);
                CHECK(component->sibling.Handle() == components[1]);
                CHECK(component->RelativeTransform().translation == Vec3d(1.0, 2.0, 3.0));
                CHECK(component->IsEnabled() && component->GetOwner().IsNull());
                CHECK(component->GetState() == ComponentState::Constructed);
                // 부착 계층은 저장하지 않는다(scene의 부모였지만 자식 목록이 비어 있다).
                CHECK(component->Children().Size() == 0 && component->Parent().IsNull());
                CHECK(!component->HasWorld());
                CHECK(ErrorOf(component->Destroy()) == ErrorCode::InvalidArgument);
            }

            {
                auto borrowed = loaded.Resolve<SaveComponent>(access, components[1]);
                REQUIRE(borrowed);
                SaveComponent* component = Get(borrowed.Value());
                REQUIRE(component != nullptr);
                CHECK(!component->IsEnabled());
                CHECK(component->sibling.Handle().IsNull() && component->values.Size() == 0);
            }

            {
                auto borrowed = loaded.Resolve<SceneComponent>(access, components[2]);
                REQUIRE(borrowed);
                SceneComponent* component = Get(borrowed.Value());
                REQUIRE(component != nullptr);
                CHECK(component->RelativeTransform().scale == Vec3(2.0f, 2.0f, 2.0f));
                CHECK(component->Parent().IsNull());
                // 장부 밖 SceneComponent의 계층 API는 InvalidArgument, 값 설정·월드 행렬은 된다.
                CHECK(ErrorOf(component->AttachTo(components[0])) == ErrorCode::InvalidArgument);
                CHECK(ErrorOf(component->Detach()) == ErrorCode::InvalidArgument);
                REQUIRE(component->SetRelativeLocation(Vec3d(0.0, 0.0, 5.0)));
                CHECK(component->WorldLocation() == Vec3d(0.0, 0.0, 5.0));
                REQUIRE(component->SetRelativeLocation(Vec3d(0.0, 0.0, 0.0)));
            }

            {
                auto borrowed = loaded.Resolve<Actor>(access, loadedPlain);
                REQUIRE(borrowed);
                Actor* actor = Get(borrowed.Value());
                REQUIRE(actor != nullptr);
                CHECK(actor->Components().Size() == 1 && actor->RootComponent().IsNull());
                CHECK(actor->IsEnabled());
            }

            auto resaved =
                SaveObjectGraph(fixture.allocator, loaded, access, root.Value().Handle());
            REQUIRE(resaved);
            CHECK(SameBytes(saved.View(), resaved.Value().View()));
        }

        REQUIRE(loaded.Shutdown(access));
    }

    REQUIRE(world.Shutdown());
}

// World registry에 있지만 World API를 거치지 않고 만든 객체(장부 밖 — world_ 없음)는 World의
// 정리·계층 API에 들어오지 못한다: DestroyComponent·AttachTo(어느 쪽이든)·SetRootComponent는
// InvalidArgument, 수명 기록이 필요한 Actor API는 StaleHandle이다. 장부 안 객체는 영향을 받지
// 않는다.
TEST(WorldReflection_ObjectsOutsideWorldLedgerRejectWorldApis) {
    ReflectedWorld fixture(6);
    REQUIRE(fixture.Ready());
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActorDeferred<Actor>();
    REQUIRE(actor);
    auto owned = world.AddComponent<SceneComponent>(actor.Value());
    REQUIRE(owned);
    {
        auto strayScene = world.Objects().NewObject<SceneComponent>(
            world.Access(), ObjectTypeIdOf<SceneComponent>());
        auto strayActor = world.Objects().NewObject<Actor>(world.Access(), ObjectTypeIdOf<Actor>());
        REQUIRE(strayScene && strayActor);
        const ObjectHandle scene = strayScene.Value().Handle();
        const ObjectHandle stray = strayActor.Value().Handle();
        CHECK(world.DestroyComponent(scene).Error() == ErrorCode::InvalidArgument);
        auto attachForeign =
            world.With<SceneComponent>(scene, [&](SceneComponent& component) noexcept {
                return component.AttachTo(owned.Value());
            });
        CHECK(ErrorOf(attachForeign) == ErrorCode::InvalidArgument);
        auto attachToForeign =
            world.With<SceneComponent>(owned.Value(), [&](SceneComponent& component) noexcept {
                return component.AttachTo(scene);
            });
        CHECK(ErrorOf(attachToForeign) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(SetRoot(world, stray, ObjectHandle{})) == ErrorCode::InvalidArgument);
        CHECK(world.DestroyActor(stray).Error() == ErrorCode::StaleHandle);
        CHECK(world.AddComponent<SceneComponent>(stray).Error() == ErrorCode::StaleHandle);
        ObjectHandle listed[2];
        CHECK(world.GetComponents(stray, listed).Error() == ErrorCode::StaleHandle);
        CHECK(world.FindComponent<SceneComponent>(stray).IsNull());
        auto selfDestroy =
            world.With<Actor>(stray, [](Actor& value) noexcept { return value.Destroy(); });
        CHECK(ErrorOf(selfDestroy) == ErrorCode::InvalidArgument);
    }

    // 장부 안 객체는 그대로 동작한다.
    auto parent = world.AddComponent<SceneComponent>(actor.Value());
    REQUIRE(parent);
    auto attached =
        world.With<SceneComponent>(owned.Value(), [&](SceneComponent& component) noexcept {
            return component.AttachTo(parent.Value());
        });
    CHECK(attached.IsOk());
    REQUIRE(world.DestroyComponent(owned.Value()));
    REQUIRE(world.Tick(0.016));
    REQUIRE(world.Shutdown());
}

// 컴포넌트 목록 순회는 원소 수 기준이고 null 원소를 건너뛴다(로드된 목록의 null — Required는
// 로드에서 진단만 한다): FindComponent·FinishSpawning·활성화·시작·DestroyActor가 null 뒤의
// Component까지 처리한다.
TEST(WorldReflection_TraversalsSkipNullComponentSlots) {
    ReflectedWorld fixture(7);
    REQUIRE(fixture.Ready());
    auto token = RegisterNativeClass<HookCounter>(fixture.types);
    REQUIRE(token);
    HookCounter::enables = 0;
    HookCounter::starts = 0;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActorDeferred<Actor>();
    REQUIRE(actor);
    REQUIRE(GeneratedType<NullSlotProbe>::InsertNull(world, actor.Value(), 0));
    auto scene = world.AddComponent<SceneComponent>(actor.Value());
    REQUIRE(scene);
    REQUIRE(GeneratedType<NullSlotProbe>::InsertNull(world, actor.Value(), 2));
    auto plain = world.AddComponent<HookCounter>(actor.Value());
    REQUIRE(plain);
    ObjectHandle listed[4];
    auto count = world.GetComponents(actor.Value(), listed);
    REQUIRE(count);
    REQUIRE(count.Value() == 4);
    CHECK(listed[0].IsNull() && listed[1] == scene.Value());
    CHECK(listed[2].IsNull() && listed[3] == plain.Value());
    CHECK(world.FindComponent<SceneComponent>(actor.Value()) == scene.Value());
    CHECK(world.FindComponent<Component>(actor.Value()) == scene.Value());

    REQUIRE(world.FinishSpawning(actor.Value()));
    CHECK(world.GetComponentState(scene.Value()).Value() == ComponentState::Initialized);
    CHECK(world.GetComponentState(plain.Value()).Value() == ComponentState::Initialized);
    // FinishSpawning 전에 붙은 Component의 OnEnable은 Actor 활성화 순회(EnableActor)만 부른다(진행
    // 단계에서는 Actor가 아직 활성이 아니다). 두 null 뒤의 Component가 한 번 받았다.
    CHECK(HookCounter::enables == 1 && HookCounter::starts == 0);
    REQUIRE(world.Tick(0.016));
    // 시작(OnStart) 순회도 null 뒤까지 진행되어 Playing이다.
    CHECK(HookCounter::starts == 1);
    CHECK(world.GetActorState(actor.Value()).Value() == ActorState::Playing);
    CHECK(world.GetComponentState(scene.Value()).Value() == ComponentState::Playing);
    CHECK(world.GetComponentState(plain.Value()).Value() == ComponentState::Playing);

    REQUIRE(world.DestroyActor(actor.Value()));
    CHECK(world.GetComponentState(plain.Value()).Value() == ComponentState::PendingDestroy);
    REQUIRE(world.Tick(0.016));
    CHECK(!world.HasActor(actor.Value()));
    CHECK(world.Stats().components == 0);
    REQUIRE(world.Shutdown());
    // native 파생을 먼저 해제해야 고정물이 World 타입을 해제한다(파생이 남으면 Busy).
    CHECK(fixture.types.Unregister(token.Value()).IsOk());
}

// World 필드는 경로로 읽을 수 있고(components[0]·rootComponent·relativeTransform.translation.x —
// 도구 이름) ReadOnly라 경로 쓰기·배열 편집은 Unsupported(ReadOnly)다. fixture의 영속 필드는 경로로
// 쓰고 옛 값을 받는다.
TEST(WorldReflection_PathReadsWorldFieldsAndWritesOnlyFixtureFields) {
    ReflectedWorld fixture(8);
    REQUIRE(fixture.Ready());
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActorDeferred<SaveActor>();
    REQUIRE(actor);
    auto root = world.AddComponent<SceneComponent>(actor.Value());
    auto extra = world.AddComponent<SaveComponent>(actor.Value());
    REQUIRE(root && extra);
    REQUIRE(SetRoot(world, actor.Value(), root.Value()));
    auto moved = world.With<SceneComponent>(root.Value(), [](SceneComponent& component) noexcept {
        return component.SetRelativeLocation(Vec3d(4.5, 0.0, 0.0));
    });
    REQUIRE(moved);

    Allocator& allocator = fixture.allocator;
    ObjectRegistry& objects = world.Objects();
    const ObjectAccessContext access = world.Access();
    auto actorType = fixture.types.Acquire(ObjectTypeIdOf<SaveActor>());
    auto sceneType = fixture.types.Acquire(ObjectTypeIdOf<SceneComponent>());
    auto saveType = fixture.types.Acquire(ObjectTypeIdOf<SaveComponent>());
    auto levelType = fixture.types.Acquire(ObjectTypeIdOf<Level>());
    REQUIRE(actorType && sceneType && saveType && levelType);
    PropertyPathBuffer path;
    PropertyPathFailure failure;
    FieldValue read(allocator);

    REQUIRE(ParsePropertyPath(actorType.Value(), StringView("components[0]"), path));
    REQUIRE(PathRead(objects, access, actor.Value(), path.View(), read));
    CHECK(read.kind == ValueKind::ObjectReference && read.reference == root.Value());
    REQUIRE(ParsePropertyPath(actorType.Value(), StringView("components[1]"), path));
    REQUIRE(PathRead(objects, access, actor.Value(), path.View(), read));
    CHECK(read.reference == extra.Value());
    REQUIRE(ParsePropertyPath(actorType.Value(), StringView("rootComponent"), path));
    REQUIRE(PathRead(objects, access, actor.Value(), path.View(), read));
    CHECK(read.kind == ValueKind::ObjectReference && read.reference == root.Value());
    REQUIRE(
        ParsePropertyPath(sceneType.Value(), StringView("relativeTransform.translation.x"), path));
    REQUIRE(PathRead(objects, access, root.Value(), path.View(), read));
    CHECK(read.kind == ValueKind::Float64 && read.float64 == 4.5);

    // World 필드 쓰기는 ReadOnly로 막힌다(World 장부·MarkDirty를 우회하지 않게).
    FieldValue x(allocator);
    x.kind = ValueKind::Float64;
    x.float64 = 9.0;
    CHECK(ErrorOf(PathWrite(objects, access, root.Value(), path.View(), x, nullptr, &failure)) ==
          ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::ReadOnly);
    REQUIRE(ParsePropertyPath(actorType.Value(), StringView("enabled"), path));
    CHECK(ErrorOf(PathWrite(objects, access, actor.Value(), path.View(),
                            BoolValue(allocator, false), nullptr, &failure)) ==
          ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::ReadOnly);
    FieldValue reference(allocator);
    reference.kind = ValueKind::ObjectReference;
    reference.reference = extra.Value();
    REQUIRE(ParsePropertyPath(actorType.Value(), StringView("rootComponent"), path));
    CHECK(ErrorOf(PathWrite(objects, access, actor.Value(), path.View(), reference, nullptr,
                            &failure)) == ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::ReadOnly);
    REQUIRE(ParsePropertyPath(actorType.Value(), StringView("components[0]"), path));
    CHECK(ErrorOf(PathWrite(objects, access, actor.Value(), path.View(), reference, nullptr,
                            &failure)) == ErrorCode::Unsupported);
    CHECK(ErrorOf(PathRemoveElement(objects, access, actor.Value(), path.View(), nullptr,
                                    &failure)) == ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::ReadOnly);
    REQUIRE(ParsePropertyPath(actorType.Value(), StringView("components"), path));
    CHECK(ErrorOf(PathClearArray(objects, access, actor.Value(), path.View(), &failure)) ==
          ErrorCode::Unsupported);
    auto length = PathArrayLength(objects, access, actor.Value(), path.View());
    CHECK(length && length.Value() == 2);
    REQUIRE(ParsePropertyPath(saveType.Value(), StringView("enabled"), path));
    CHECK(ErrorOf(PathWrite(objects, access, extra.Value(), path.View(),
                            BoolValue(allocator, false), nullptr, &failure)) ==
          ErrorCode::Unsupported);
    FieldValue name(allocator);
    name.kind = ValueKind::Utf8String;
    REQUIRE(name.string.TryAssign("이름"));
    REQUIRE(ParsePropertyPath(levelType.Value(), StringView("name"), path));
    CHECK(ErrorOf(PathWrite(objects, access, world.PersistentLevel(), path.View(), name, nullptr,
                            &failure)) == ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::ReadOnly);
    CHECK(RootOf(world, actor.Value()) == root.Value());

    // fixture 필드는 경로로 쓰고 옛 값을 받는다.
    FieldValue previous(allocator);
    REQUIRE(ParsePropertyPath(actorType.Value(), StringView("score"), path));
    REQUIRE(PathWrite(objects, access, actor.Value(), path.View(), Int32Value(allocator, 9),
                      &previous));
    CHECK(previous.kind == ValueKind::Int32 && previous.signedInteger == 0);
    REQUIRE(PathRead(objects, access, actor.Value(), path.View(), read));
    CHECK(read.signedInteger == 9);
    REQUIRE(ParsePropertyPath(actorType.Value(), StringView("label"), path));
    REQUIRE(PathWrite(objects, access, actor.Value(), path.View(), name, &previous));
    CHECK(previous.kind == ValueKind::Utf8String && previous.string.ByteCount() == 0);
    REQUIRE(ParsePropertyPath(saveType.Value(), StringView("values[0]"), path));
    FieldValue element = Int32Value(allocator, 5);
    REQUIRE(PathInsertElement(objects, access, extra.Value(), path.View(), &element));
    REQUIRE(PathWrite(objects, access, extra.Value(), path.View(), Int32Value(allocator, 6),
                      &previous));
    CHECK(previous.signedInteger == 5);
    auto values = world.With<SaveActor>(actor.Value(), [](SaveActor& value) noexcept {
        return value.score == 9 && value.label.View() == StringView("이름");
    });
    CHECK(values && values.Value());
    REQUIRE(world.Shutdown());
}

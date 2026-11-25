#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>
#include <World/WorldTypes.h>
#include <stdio.h>
#include <string.h>

// World의 Tick 의존 선언·부착 간선·실행 중 설정 변경·엔진 단계 자리 시험이다
// (Docs/Design/EngineRuntime 15.3·15.4절, S7b).
using namespace rocks;
namespace world_tick_dependency_tests_file {

namespace {

// 실행 기록은 "<표지><hook>" 두 글자다(hook: u = OnTick, l = OnLateTick, f = OnFixedTick, 엔진
// 단계는 대문자 한 글자 + '!').
constexpr uint32 MaxLog = 64;
char logText[MaxLog * 2 + 1] = {};
uint32 logCount = 0;

void Log(char tag, char hook) noexcept {
    if (logCount < MaxLog) {
        logText[logCount * 2] = tag;
        logText[logCount * 2 + 1] = hook;
        ++logCount;
        logText[logCount * 2] = '\0';
    }
}

void ResetLog() noexcept {
    logCount = 0;
    logText[0] = '\0';
}

bool LogIs(const char* expected) noexcept {
    const bool same = strcmp(logText, expected) == 0;
    if (!same) {
        printf("  log = \"%s\", expected \"%s\"\n", logText, expected);
    }

    return same;
}

class OrderActor final : public Actor {
    ROCKS_NATIVE_TYPE(OrderActor, Actor, 0x3c9e61d2a8f74b05ull, 0x9d14e0b7c25a6f38ull)

public:
    explicit OrderActor(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().tick = true;
        PrimaryTick().lateTick = true;
    }

    void Configure(char value, int32 priority = 0, TickPhase phase = TickPhase::PrePhysics,
                   bool fixed = false) noexcept {
        tag = value;
        PrimaryTick().priority = priority;
        PrimaryTick().phase = phase;
        PrimaryTick().fixedTick = fixed;
    }

    void OnTick(const TickContext&) noexcept override {
        Log(tag, 'u');
    }

    void OnLateTick(const TickContext&) noexcept override {
        Log(tag, 'l');
    }

    void OnFixedTick(const TickContext&) noexcept override {
        Log(tag, 'f');
    }

    char tag = '?';
};

class OrderScene final : public SceneComponent {
    ROCKS_NATIVE_TYPE(OrderScene, SceneComponent, 0x71b0d4f9e3c24a86ull, 0xa58e2f61c7d93b40ull)

public:
    explicit OrderScene(Allocator& allocator) noexcept : SceneComponent(allocator) {
        PrimaryTick().tick = true;
        PrimaryTick().lateTick = true;
    }

    void Configure(char value, int32 priority = 0,
                   TickPhase phase = TickPhase::PrePhysics) noexcept {
        tag = value;
        PrimaryTick().priority = priority;
        PrimaryTick().phase = phase;
    }

    void OnTick(const TickContext&) noexcept override {
        Log(tag, 'u');
    }

    void OnLateTick(const TickContext&) noexcept override {
        Log(tag, 'l');
    }

    char tag = '?';
};

// WorldTests.cpp의 Fixture와 같은 수명 순서다(World → native 토큰 → ModuleRegistration →
// TypeRegistry).
struct Fixture {
    static constexpr uint32 MaxTokens = 4;

    Fixture() noexcept : types(allocator), modules(types, allocator) {
        registered =
            RegisterWorldTypes(modules).IsOk() && Register<OrderActor>() && Register<OrderScene>();
        WorldDesc desc;
        desc.name = "Test";
        desc.context = {0x574f524c44ull, 7};
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
        if (!token || tokenCount == MaxTokens) {
            return false;
        }

        tokens[tokenCount++] = token.Value();
        return true;
    }

    // 지연 Spawn하고 설정한 뒤 FinishSpawning까지 한다(finish가 거짓이면 Constructed로 둔다).
    ObjectHandle Spawn(char tag, int32 priority = 0, TickPhase phase = TickPhase::PrePhysics,
                       bool finish = true, bool fixed = false) noexcept {
        auto actor = world->SpawnActorDeferred<OrderActor>();
        if (!actor) {
            return {};
        }

        auto configured = world->With<OrderActor>(actor.Value(), [&](OrderActor& value) noexcept {
            value.Configure(tag, priority, phase, fixed);
        });
        if (!configured || (finish && !world->FinishSpawning(actor.Value()))) {
            return {};
        }

        return actor.Value();
    }

    ObjectHandle AddScene(ObjectHandle actor, char tag, int32 priority = 0,
                          TickPhase phase = TickPhase::PrePhysics) noexcept {
        auto scene = world->AddComponent<OrderScene>(actor);
        if (!scene) {
            return {};
        }

        auto configured = world->With<OrderScene>(scene.Value(), [&](OrderScene& value) noexcept {
            value.Configure(tag, priority, phase);
        });
        return configured ? scene.Value() : ObjectHandle{};
    }

    Result<void> Attach(ObjectHandle child, ObjectHandle parent) noexcept {
        return world->With<SceneComponent>(child, [parent](SceneComponent& scene) noexcept {
            return parent.IsNull() ? scene.Detach() : scene.AttachTo(parent);
        });
    }

    // 한 프레임을 돌리고 기록을 본다.
    bool Frame(const char* expected) noexcept {
        ResetLog();
        const bool ticked = world->Tick(1.0 / 60.0).IsOk();
        CHECK(ticked);
        return ticked && LogIs(expected);
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken tokens[MaxTokens];
    uint32 tokenCount = 0;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

// ERD1: 소유 Actor 뒤에 두는 자동 간선이 없다. 같은 priority면 등록 순서로 Actor가 먼저지만,
// Component가 더 작은 priority면 Actor보다 앞선다.
TEST(WorldTick_ComponentIsNotForcedAfterOwnerActor) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto owner = world.SpawnActorDeferred<OrderActor>();
    REQUIRE(owner);
    REQUIRE(world.With<OrderActor>(owner.Value(),
                                   [](OrderActor& value) noexcept { value.Configure('A'); }));
    REQUIRE(!fixture.AddScene(owner.Value(), 'c', -1).IsNull());
    REQUIRE(!fixture.AddScene(owner.Value(), 'd', 0).IsNull());
    REQUIRE(world.FinishSpawning(owner.Value()));
    // 같은 단계 안: priority -1인 c가 먼저, 같은 priority의 A·d는 등록 순서(Actor 먼저).
    CHECK(fixture.Frame("cuAuduclAldl"));
    CHECK(fixture.Frame("cuAuduclAldl"));
    REQUIRE(world.Shutdown());
}

// Actor·Component·hook 조합의 의존과 제거(제거하면 원래 순서로 돌아간다).
TEST(WorldTick_PrerequisitesAcrossActorsComponentsAndHooks) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle a = fixture.Spawn('A');
    auto b = world.SpawnActorDeferred<OrderActor>();
    REQUIRE(!a.IsNull() && b);
    REQUIRE(world.With<OrderActor>(b.Value(),
                                   [](OrderActor& value) noexcept { value.Configure('B'); }));
    const ObjectHandle bc = fixture.AddScene(b.Value(), 'b');
    REQUIRE(!bc.IsNull());
    REQUIRE(world.FinishSpawning(b.Value()));
    CHECK(fixture.Frame("AuBubuAlBlbl"));

    // A의 OnTick이 B의 Component OnTick 뒤, B의 Component OnLateTick이 A의 OnLateTick 앞.
    REQUIRE(world.AddTickPrerequisite({a}, {bc}));
    REQUIRE(world.AddTickPrerequisite({a, TickHook::Late}, {bc, TickHook::Late}));
    CHECK(fixture.Frame("BubuAuBlblAl"));

    REQUIRE(world.RemoveTickPrerequisite({a}, {bc}));
    REQUIRE(world.RemoveTickPrerequisite({a, TickHook::Late}, {bc, TickHook::Late}));
    CHECK(world.RemoveTickPrerequisite({a}, {bc}).Error() == ErrorCode::NotFound);
    CHECK(fixture.Frame("AuBubuAlBlbl"));

    // 받지 않는 hook·같은 hook 자신·없는 대상은 거부한다.
    CHECK(world.AddTickPrerequisite({a, TickHook::Fixed}, {bc}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(world.AddTickPrerequisite({a}, {a}).Error() == ErrorCode::InvalidArgument);
    CHECK(world.AddTickPrerequisite({a}, {ObjectHandle{}}).Error() == ErrorCode::StaleHandle);
    REQUIRE(world.Shutdown());
}

// ERD5: 시작 전에 선언한 의존이 시작 때 적용되고, 끝내 시작하지 않는 선행은 효력이 없다. 시작 전
// 선언끼리의 순환도 호출 시점에 거부한다(ERD2).
TEST(WorldTick_DeclarationsBeforeStartApplyWhenRegistered) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle a = fixture.Spawn('A', 0, TickPhase::PrePhysics, false);
    const ObjectHandle b = fixture.Spawn('B', 0, TickPhase::PrePhysics, false);
    const ObjectHandle c = fixture.Spawn('C', 0, TickPhase::PrePhysics, false);
    const ObjectHandle never = fixture.Spawn('N', 0, TickPhase::PrePhysics, false);
    REQUIRE(!a.IsNull() && !b.IsNull() && !c.IsNull() && !never.IsNull());
    REQUIRE(world.AddTickPrerequisite({a}, {b}));
    REQUIRE(world.AddTickPrerequisite({b}, {c}));
    CHECK(world.AddTickPrerequisite({c}, {a}).Error() == ErrorCode::CycleDetected);
    REQUIRE(world.AddTickPrerequisite({c}, {never}));
    REQUIRE(world.FinishSpawning(a));
    REQUIRE(world.FinishSpawning(b));
    REQUIRE(world.FinishSpawning(c));
    // 등록 순서는 A, B, C지만 선언대로 C → B → A다. N은 시작하지 않아 C를 막지 않는다.
    CHECK(fixture.Frame("CuBuAuAlBlCl"));
    REQUIRE(world.Shutdown());
}

// ERD3: 선행이 더 늦은 단계면 의존하는 쪽이 다음 프레임부터 그 단계에서 선행 뒤에 실행된다. Fixed
// hook과 다른 hook 사이 의존은 거부한다.
TEST(WorldTick_LaterPhasePrerequisiteDemotesDependent) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle a = fixture.Spawn('A', 0, TickPhase::PrePhysics);
    const ObjectHandle b = fixture.Spawn('B', 0, TickPhase::PostPhysics, true, true);
    REQUIRE(!a.IsNull() && !b.IsNull());
    CHECK(fixture.Frame("AuBfBuAlBl"));
    REQUIRE(world.AddTickPrerequisite({a}, {b}));
    CHECK(fixture.Frame("BfBuAuAlBl"));
    CHECK(world.GetTickEffectivePhase({a}).Value() == TickPhase::PostPhysics);
    CHECK(world.AddTickPrerequisite({a}, {b, TickHook::Fixed}).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(world.RemoveTickPrerequisite({a}, {b}));
    CHECK(fixture.Frame("AuBfBuAlBl"));
    CHECK(world.GetTickEffectivePhase({a}).Value() == TickPhase::PrePhysics);
    REQUIRE(world.Shutdown());
}

// 부착된 SceneComponent는 부모의 같은 hook 뒤에 실행된다. 떼면 간선이 없어지고, 같은 간선을
// 사용자가 따로 걸었으면 그 간선은 남는다. 부모가 파괴돼도 자식은 계속 실행된다.
TEST(WorldTick_AttachedSceneComponentsTickAfterParent) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto owner = world.SpawnActorDeferred<OrderActor>();
    REQUIRE(owner);
    REQUIRE(world.With<OrderActor>(owner.Value(),
                                   [](OrderActor& value) noexcept { value.Configure('A'); }));
    const ObjectHandle child = fixture.AddScene(owner.Value(), 'c', -2);
    const ObjectHandle parent = fixture.AddScene(owner.Value(), 'p', -1);
    REQUIRE(!child.IsNull() && !parent.IsNull());
    REQUIRE(fixture.Attach(child, parent));
    REQUIRE(world.FinishSpawning(owner.Value()));
    CHECK(fixture.Frame("pucuAuplclAl"));

    REQUIRE(fixture.Attach(child, {}));
    CHECK(fixture.Frame("cupuAuclplAl"));

    REQUIRE(world.AddTickPrerequisite({child}, {parent}));
    REQUIRE(fixture.Attach(child, parent));
    REQUIRE(fixture.Attach(child, {}));
    // 사용자 간선(OnTick)은 남고, 부착 간선만 있던 OnLateTick은 원래 순서다.
    CHECK(fixture.Frame("pucuAuclplAl"));

    REQUIRE(fixture.Attach(child, parent));
    REQUIRE(world.DestroyComponent(parent));
    REQUIRE(world.Tick(1.0 / 60.0));
    CHECK(fixture.Frame("cuAuclAl"));
    REQUIRE(world.Shutdown());
}

// 자동 간선(부착)이 순환을 만들면 걸지 않고 성공한다(ERD2). 사용자 간선 순서가 유지된다.
TEST(WorldTick_AutomaticAttachmentCycleIsSkipped) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto owner = world.SpawnActorDeferred<OrderActor>();
    REQUIRE(owner);
    REQUIRE(world.With<OrderActor>(owner.Value(),
                                   [](OrderActor& value) noexcept { value.Configure('A'); }));
    const ObjectHandle child = fixture.AddScene(owner.Value(), 'c', -1);
    const ObjectHandle parent = fixture.AddScene(owner.Value(), 'p', -2);
    REQUIRE(!child.IsNull() && !parent.IsNull());
    REQUIRE(world.AddTickPrerequisite({parent}, {child}));
    REQUIRE(fixture.Attach(child, parent));
    REQUIRE(world.FinishSpawning(owner.Value()));
    // OnTick: 사용자 간선대로 c → p(부착 간선은 순환이라 생략). OnLateTick: 부착 간선대로 p → c.
    CHECK(fixture.Frame("cupuAuplclAl"));
    REQUIRE(world.Shutdown());
}

// ERD6: 시작 뒤 priority·단계·interval을 바꿀 수 있다.
TEST(WorldTick_SettingsChangeAfterStart) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle a = fixture.Spawn('A', 0);
    const ObjectHandle b = fixture.Spawn('B', 1);
    REQUIRE(!a.IsNull() && !b.IsNull());
    CHECK(fixture.Frame("AuBuAlBl"));
    REQUIRE(world.SetTickPriority(a, 2));
    CHECK(fixture.Frame("BuAuBlAl"));
    REQUIRE(world.SetTickPhase(b, TickPhase::PreRender));
    CHECK(fixture.Frame("AuBlAlBu"));
    CHECK(world.SetTickPhase(b, TickPhase::LateUpdate).Error() == ErrorCode::InvalidArgument);
    CHECK(world.SetTickPhase(b, TickPhase::Physics).Error() == ErrorCode::InvalidArgument);
    CHECK(world.SetTickInterval(b, -1.0).Error() == ErrorCode::InvalidArgument);
    // 2/60초 간격: 다음 프레임은 건너뛰고 그다음 프레임에 실행(누적은 바꿀 때 0).
    REQUIRE(world.SetTickInterval(b, 2.0 / 60.0));
    CHECK(fixture.Frame("AuAl"));
    CHECK(fixture.Frame("AuBlAlBu"));
    CHECK(world.SetTickPriority(ObjectHandle{}, 0).Error() == ErrorCode::StaleHandle);
    REQUIRE(world.Shutdown());
}

namespace {

struct StageLog {
    bool failCamera = false;
};

Result<void> RecordStage(World&, const TickContext& context, void* user) noexcept {
    // 표지는 user 포인터가 가리키는 글자다.
    Log(*static_cast<const char*>(user), context.paused ? 'p' : '!');
    return Result<void>::Success();
}

StageLog stageLog;

Result<void> FailingCamera(World&, const TickContext&, void*) noexcept {
    Log('C', 'x');
    return stageLog.failCamera ? Result<void>::Failure(ErrorCode::DependencyFailed)
                               : Result<void>::Success();
}

const char PhysicsTag = 'P';
const char SkeletalTag = 'S';
const char TimersTag = 'T';
const char TickablesTag = 'K';
const char CameraTag = 'C';

} // namespace

// ERD7: 엔진 단계는 Physics 끝(고정 스텝 수와 무관하게 프레임에 한 번)과 LateUpdate 시작에 정해진
// 순서로 돈다. 자리마다 하나, 실패는 Tick 실패와 통계로, pause 중 카메라는 생략, 단일 스텝은 실행.
TEST(WorldTick_EngineStagesRunInDocumentedOrder) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    REQUIRE(world.SetEngineStage(EngineStage::CameraUpdate, &RecordStage,
                                 const_cast<char*>(&CameraTag)));
    REQUIRE(world.SetEngineStage(EngineStage::TickableSubsystems, &RecordStage,
                                 const_cast<char*>(&TickablesTag)));
    REQUIRE(world.SetEngineStage(EngineStage::Timers, &RecordStage, const_cast<char*>(&TimersTag)));
    REQUIRE(world.SetEngineStage(EngineStage::SkeletalPhysicsFinalize, &RecordStage,
                                 const_cast<char*>(&SkeletalTag)));
    REQUIRE(world.SetEngineStage(EngineStage::PhysicsResults, &RecordStage,
                                 const_cast<char*>(&PhysicsTag)));
    CHECK(world.SetEngineStage(EngineStage::Timers, &RecordStage, nullptr).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(world.SetEngineStage(EngineStage::Count, &RecordStage, nullptr).Error() ==
          ErrorCode::InvalidArgument);
    const ObjectHandle a = fixture.Spawn('A');
    REQUIRE(!a.IsNull());
    CHECK(fixture.Frame("AuP!S!T!K!C!Al"));

    // 고정 스텝이 0번인 프레임에도 Physics 끝 단계는 한 번 돈다.
    ResetLog();
    REQUIRE(world.Tick(1.0 / 240.0));
    CHECK(LogIs("AuP!S!T!K!C!Al"));

    // pause: tickWhenPaused가 아닌 Actor는 멈추고, 카메라는 생략, 나머지 단계는 pause 표시로 돈다.
    world.SetPaused(true);
    CHECK(fixture.Frame("PpSpTpKp"));
    REQUIRE(world.Clock().RequestSingleStep());
    CHECK(fixture.Frame("AuP!S!T!K!C!Al"));
    world.SetPaused(false);

    // 실패는 그 프레임 Tick의 실패로 돌아가고 센다. 비우면 다시 둘 수 있다.
    world.ClearEngineStage(EngineStage::CameraUpdate);
    REQUIRE(world.SetEngineStage(EngineStage::CameraUpdate, &FailingCamera, nullptr));
    stageLog.failCamera = true;
    ResetLog();
    CHECK(world.Tick(1.0 / 60.0).Error() == ErrorCode::DependencyFailed);
    CHECK(LogIs("AuP!S!T!K!CxAl"));
    CHECK(world.Stats().engineStageFailures == 1);
    stageLog.failCamera = false;
    REQUIRE(world.Shutdown());
}

// 리뷰 보강: 같은 Component의 hook 사이 시작 전 선언도 적용된다(Late가 PreRender의 Update 뒤로
// 늦춰진다 — 15.3절 hook별 실효 단계).
TEST(WorldTick_SameComponentHooksDeclaredBeforeStart) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto owner = world.SpawnActorDeferred<OrderActor>();
    REQUIRE(owner);
    REQUIRE(world.With<OrderActor>(owner.Value(),
                                   [](OrderActor& value) noexcept { value.Configure('A'); }));
    const ObjectHandle scene = fixture.AddScene(owner.Value(), 'c', 0, TickPhase::PreRender);
    REQUIRE(!scene.IsNull());
    REQUIRE(world.AddTickPrerequisite({scene, TickHook::Late}, {scene, TickHook::Update}));
    REQUIRE(world.FinishSpawning(owner.Value()));
    CHECK(fixture.Frame("AuAlcucl"));
    CHECK(world.GetTickEffectivePhase({scene, TickHook::Late}).Value() == TickPhase::PreRender);
    REQUIRE(world.Shutdown());
}

// 정의되지 않은 출처 값(0·여러 비트)은 거부하고, 파괴 대기 대상에 대한 선언·설정 변경은
// Destroyed다. 파괴된 선행의 참조는 상대 선언에서 바로 지워져 남은 순서가 그대로 돈다.
TEST(WorldTick_InvalidSourceAndDestroyingTargetsAreRejected) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle a = fixture.Spawn('A', 1);
    const ObjectHandle b = fixture.Spawn('B', 0);
    REQUIRE(!a.IsNull() && !b.IsNull());
    CHECK(world.AddTickPrerequisite({a}, {b}, static_cast<TickEdgeSource>(0)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(world.AddTickPrerequisite({a}, {b}, static_cast<TickEdgeSource>(3)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(world.RemoveTickPrerequisite({a}, {b}, static_cast<TickEdgeSource>(0)).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(world.AddTickPrerequisite({b}, {a}));
    CHECK(fixture.Frame("AuBuBlAl"));

    REQUIRE(world.DestroyActor(a));
    CHECK(world.AddTickPrerequisite({b}, {a, TickHook::Late}).Error() == ErrorCode::Destroyed);
    CHECK(world.SetTickPriority(a, 5).Error() == ErrorCode::Destroyed);
    CHECK(world.SetTickPhase(a, TickPhase::PostPhysics).Error() == ErrorCode::Destroyed);
    CHECK(world.SetTickInterval(a, 1.0).Error() == ErrorCode::Destroyed);
    REQUIRE(world.Tick(1.0 / 60.0));
    CHECK(fixture.Frame("BuBl"));
    // 지운 선행을 가리키는 제거는 없다.
    CHECK(world.RemoveTickPrerequisite({b}, {a}).Error() == ErrorCode::NotFound);
    REQUIRE(world.Shutdown());
}

namespace {

struct StageWork {
    ObjectHandle victim;
    bool done = false;
};

Result<void> SpawnAndDestroyInStage(World& world, const TickContext&, void* user) noexcept {
    auto* work = static_cast<StageWork*>(user);
    Log('T', '!');
    if (work->done) {
        return Result<void>::Success();
    }

    work->done = true;
    auto spawned = world.SpawnActor<OrderActor>();
    if (!spawned) {
        return Result<void>::Failure(spawned.Error());
    }

    auto destroyed = world.DestroyActor(work->victim);
    if (!destroyed) {
        return destroyed;
    }

    // 파괴 대기 대상에 대한 선언은 Destroyed여야 한다.
    auto declared = world.AddTickPrerequisite({spawned.Value()}, {work->victim, TickHook::Late});
    return !declared && declared.Error() == ErrorCode::Destroyed
               ? Result<void>::Success()
               : Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

// 엔진 단계 콜백 안에서 Spawn·Destroy·의존 선언을 불러도 안전하다(Spawn은 다음 프레임 시작,
// Destroy는 곧바로 Tick에서 빠진다).
TEST(WorldTick_EngineStageCallbackMayUseWorldApis) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle a = fixture.Spawn('A');
    REQUIRE(!a.IsNull());
    StageWork work;
    work.victim = a;
    REQUIRE(world.SetEngineStage(EngineStage::Timers, &SpawnAndDestroyInStage, &work));
    CHECK(fixture.Frame("AuT!"));
    CHECK(fixture.Frame("?uT!?l"));
    REQUIRE(world.Shutdown());
}

// 다른 Actor의 Component에 부착한 뒤 부모 Actor를 파괴하면 부착 간선이 풀리고 자식은 계속 돈다.
TEST(WorldTick_AttachmentAcrossActorsSurvivesParentActorDestroy) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto x = world.SpawnActorDeferred<OrderActor>();
    auto y = world.SpawnActorDeferred<OrderActor>();
    REQUIRE(x && y);
    REQUIRE(world.With<OrderActor>(x.Value(),
                                   [](OrderActor& value) noexcept { value.Configure('X', 5); }));
    REQUIRE(world.With<OrderActor>(y.Value(),
                                   [](OrderActor& value) noexcept { value.Configure('Y', 5); }));
    const ObjectHandle parent = fixture.AddScene(x.Value(), 'p', 2);
    const ObjectHandle child = fixture.AddScene(y.Value(), 'c', 1);
    REQUIRE(!parent.IsNull() && !child.IsNull());
    REQUIRE(fixture.Attach(child, parent));
    REQUIRE(world.FinishSpawning(x.Value()));
    REQUIRE(world.FinishSpawning(y.Value()));
    CHECK(fixture.Frame("pucuXuYuplclXlYl"));
    REQUIRE(world.DestroyActor(x.Value()));
    REQUIRE(world.Tick(1.0 / 60.0));
    CHECK(fixture.Frame("cuYuclYl"));
    REQUIRE(world.Shutdown());
}

namespace {

bool Has(const String& text, const char* needle) noexcept {
    const StringView view = text.View();
    const usize length = strlen(needle);
    for (usize at = 0; at + length <= view.ByteCount(); ++at) {
        if (memcmp(view.Data() + at, needle, length) == 0) {
            return true;
        }
    }

    printf("  missing \"%s\" in:\n%.*s\n", needle, static_cast<int>(view.ByteCount()), view.Data());
    return false;
}

} // namespace

// 22.1절 DumpTickGraph: 단계마다 실행 순서대로 항목·선행·늦춰진 원래 단계·엔진 단계 자리를 쓴다.
// 순서는 그대로 두고(다시 만들지 않는다) 통계의 늦춤 수가 같은 사실을 센다.
TEST(WorldTick_DumpTickGraphListsOrderPrerequisitesAndDemotion) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle a = fixture.Spawn('A', 0, TickPhase::PrePhysics);
    const ObjectHandle b = fixture.Spawn('B', 0, TickPhase::PostPhysics, true, true);
    REQUIRE(!a.IsNull() && !b.IsNull());
    REQUIRE(world.AddTickPrerequisite({a}, {b}));
    CHECK(fixture.Frame("BfBuAuAlBl"));
    SystemAllocator allocator;
    String text(allocator);
    REQUIRE(world.DumpTickGraph(text));
    CHECK(Has(text, "World Test tick graph"));
    CHECK(Has(text, "OnTick priority=0 interval=0 demotedFrom=PrePhysics"));
    CHECK(Has(text, "      after "));
    CHECK(Has(text, "OnFixedTick"));
    CHECK(Has(text, "OnLateTick"));
    CHECK(Has(text, "[engine stage CameraUpdate at start empty]"));
    CHECK(world.Stats().ticks.demoted == 1);

    String subsystems(allocator);
    REQUIRE(world.DumpSubsystems(subsystems));
    CHECK(Has(subsystems, "World Test subsystems (0 initialized, 0 skipped)"));
    REQUIRE(world.Shutdown());
}

} // namespace world_tick_dependency_tests_file

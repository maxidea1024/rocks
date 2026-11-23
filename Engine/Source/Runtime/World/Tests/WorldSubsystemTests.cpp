#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <stdio.h>
#include <string.h>

// WorldSubsystem 조건부 생성·선택 의존·World 종류·hook 순서·바깥 조회 창·tickable 시험이다
// (Docs/Design/EngineRuntime 16절, S8).
using namespace rocks;
namespace world_subsystem_tests_file {

namespace {

constexpr uint32 MaxLog = 64;
char logText[MaxLog * 3 + 1] = {};
uint32 logCount = 0;

// 사건 하나는 세 글자(대상 표지 + 사건 두 글자)다.
void Log(char tag, const char* event) noexcept {
    if (logCount < MaxLog) {
        logText[logCount * 3] = tag;
        logText[logCount * 3 + 1] = event[0];
        logText[logCount * 3 + 2] = event[1];
        ++logCount;
        logText[logCount * 3] = '\0';
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

// 바깥 조회 창이 돌려주는 가짜 바깥 Subsystem이다.
struct OuterThing {
    int value = 42;
};

OuterThing outerThing;
const SubsystemKey OuterKey = &outerThing;

void* FindOuter(const void*, SubsystemKey key) noexcept {
    return key == OuterKey ? &outerThing : nullptr;
}

// hook마다 기록하는 Subsystem이다. 표지는 파생 타입이 정한다.
template <char Tag>
class Recorder : public WorldSubsystem {
public:
    Result<void> Initialize(World& world) noexcept override {
        Log(Tag, "in");
        sawOuterAtInitialize = world.FindOuterSubsystem(OuterKey) == &outerThing;
        return Result<void>::Success();
    }

    void PostInitialize(World&) noexcept override {
        Log(Tag, "po");
    }

    void OnWorldBeginPlay(World&) noexcept override {
        Log(Tag, "bp");
    }

    void OnWorldEndPlay(World& world) noexcept override {
        Log(Tag, "ep");
        sawOuterAtEndPlay = world.FindOuterSubsystem(OuterKey) == &outerThing;
    }

    void PreDeinitialize(World& world) noexcept override {
        Log(Tag, "pd");
        sawOuterAtPreDeinitialize = world.FindOuterSubsystem(OuterKey) != nullptr;
    }

    void Deinitialize(World&) noexcept override {
        Log(Tag, "de");
    }

    static inline bool sawOuterAtInitialize = false;
    static inline bool sawOuterAtEndPlay = false;
    static inline bool sawOuterAtPreDeinitialize = false;
};

class AlphaRecorder final : public Recorder<'A'> {};
class BetaRecorder final : public Recorder<'B'> {};
class GammaRecorder final : public Recorder<'G'> {};

class TickingRecorder final : public WorldSubsystem {
public:
    bool IsTickable() const noexcept override {
        return true;
    }

    bool TickWhenPaused() const noexcept override {
        return whenPaused;
    }

    void OnTick(World&, const TickContext& context) noexcept override {
        Log('K', context.paused ? "tp" : "tk");
    }

    static inline bool whenPaused = false;
};

class LateActor final : public Actor {
    ROCKS_NATIVE_TYPE(LateActor, Actor, 0x5b8a0f3e17c24d69ull, 0xb24c7e19d0a86f53ull)

public:
    explicit LateActor(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().lateTick = true;
    }

    void OnStart() noexcept override {
        Log('L', "st");
    }

    void OnLateTick(const TickContext&) noexcept override {
        Log('L', "lt");
    }

    void OnDestroy() noexcept override {
        Log('L', "ds");
    }
};

bool Never(const void*) noexcept {
    return false;
}

// WorldTests.cpp의 Fixture와 같은 수명 순서다.
struct Fixture {
    explicit Fixture(Span<const WorldSubsystemType> subsystems, WorldKind kind = WorldKind::Game,
                     OuterSubsystemLookup outer = {}) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk();
        auto token = RegisterNativeClass<LateActor>(types);
        registered = registered && token.IsOk();
        if (token) {
            actorToken = token.Value();
        }

        WorldDesc desc;
        desc.name = "Test";
        desc.context = {0x574f524c44ull, 9};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.subsystems = subsystems;
        desc.kind = kind;
        desc.outerSubsystems = outer;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~Fixture() {
        world.Reset();
        if (registered) {
            CHECK(types.Unregister(actorToken).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken actorToken;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

// ERD9: shouldCreate 거짓이면 만들지 않고, 그것을 필수 의존으로 둔 것도 생략한다(오류 아님). 선택
// 의존은 있으면 순서만 맞추고, 목록에 없어도 된다.
TEST(WorldSubsystem_ConditionalCreationAndOptionalDependencies) {
    static const SubsystemKey needsAlpha[] = {SubsystemKeyOf<AlphaRecorder>()};
    static const SubsystemKey prefersGamma[] = {SubsystemKeyOf<GammaRecorder>(),
                                                SubsystemKeyOf<TickingRecorder>()};
    const WorldSubsystemType types[] = {
        MakeSubsystemType<WorldSubsystem, BetaRecorder>("Beta", needsAlpha),
        MakeSubsystemType<WorldSubsystem, AlphaRecorder>("Alpha", {}, {}, &Never),
        MakeSubsystemType<WorldSubsystem, GammaRecorder>("Gamma"),
    };
    // Gamma가 Beta의 선택 의존이 되도록 다시 만든다(TickingRecorder는 목록에 없다).
    WorldSubsystemType ordered[] = {types[0], types[1], types[2]};
    ordered[0].optionalDependencies = prefersGamma;
    ordered[0].dependencies = {};
    Fixture fixture(ordered);
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    ResetLog();
    REQUIRE(world.Initialize());
    CHECK(world.GetSubsystem<AlphaRecorder>() == nullptr);
    CHECK(world.GetSubsystem<BetaRecorder>() != nullptr);
    // Gamma(선택 의존)가 Beta보다 먼저다.
    CHECK(LogIs("GinBinGpoBpo"));
    REQUIRE(world.SkippedSubsystems().Size() == 1);
    CHECK(strcmp(world.SkippedSubsystems()[0].name, "Alpha") == 0);
    CHECK(world.SkippedSubsystems()[0].reason == SubsystemSkipReason::NotWanted);
    REQUIRE(world.Shutdown());
}

TEST(WorldSubsystem_MissingRequiredDependencySkipsDependent) {
    static const SubsystemKey needsAlpha[] = {SubsystemKeyOf<AlphaRecorder>()};
    const WorldSubsystemType types[] = {
        MakeSubsystemType<WorldSubsystem, BetaRecorder>("Beta", needsAlpha),
        MakeSubsystemType<WorldSubsystem, AlphaRecorder>("Alpha", {}, {}, &Never),
    };
    Fixture fixture(types);
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    ResetLog();
    REQUIRE(world.Initialize());
    CHECK(world.GetSubsystem<BetaRecorder>() == nullptr);
    CHECK(LogIs(""));
    REQUIRE(world.SkippedSubsystems().Size() == 2);
    CHECK(world.SkippedSubsystems()[1].reason == SubsystemSkipReason::MissingDependency);
    CHECK(strcmp(world.SkippedSubsystems()[1].missing, "Alpha") == 0);

    // 목록에 없는 필수 의존은 선언 오류다.
    static const SubsystemKey needsTicking[] = {SubsystemKeyOf<TickingRecorder>()};
    const WorldSubsystemType broken[] = {
        MakeSubsystemType<WorldSubsystem, GammaRecorder>("Gamma", needsTicking)};
    Fixture brokenFixture(broken);
    REQUIRE(brokenFixture.registered);
    CHECK(brokenFixture.world->Initialize().Error() == ErrorCode::InvalidArgument);
    REQUIRE(world.Shutdown());
}

// 16.4절: shouldCreate가 없는 WorldSubsystem은 Game·Editor·Tryout World에만 생긴다.
TEST(WorldSubsystem_DefaultCreationFollowsWorldKind) {
    const WorldSubsystemType types[] = {MakeSubsystemType<WorldSubsystem, GammaRecorder>("Gamma")};
    const WorldKind kinds[] = {WorldKind::Game, WorldKind::Editor, WorldKind::Tryout,
                               WorldKind::Preview, WorldKind::Inactive};
    const bool expected[] = {true, true, true, false, false};
    for (uint32 at = 0; at < 5; ++at) {
        Fixture fixture(types, kinds[at]);
        REQUIRE(fixture.registered);
        REQUIRE(fixture.world->Initialize());
        CHECK(fixture.world->Kind() == kinds[at]);
        CHECK((fixture.world->GetSubsystem<GammaRecorder>() != nullptr) == expected[at]);
        REQUIRE(fixture.world->Shutdown());
    }
}

// 16.5절 hook 순서와 바깥 조회 창: Initialize → PostInitialize → OnWorldBeginPlay → (Actor OnStart)
// → … → Actor 정리 → OnWorldEndPlay(역순) → PreDeinitialize(역순) → Deinitialize(역순). 조회 창은
// OnWorldEndPlay까지 열려 있고 PreDeinitialize부터 닫힌다.
TEST(WorldSubsystem_HookOrderAndOuterLookupLifetime) {
    static const SubsystemKey needsAlpha[] = {SubsystemKeyOf<AlphaRecorder>()};
    const WorldSubsystemType types[] = {
        MakeSubsystemType<WorldSubsystem, BetaRecorder>("Beta", needsAlpha),
        MakeSubsystemType<WorldSubsystem, AlphaRecorder>("Alpha"),
    };
    OuterSubsystemLookup outer;
    outer.find = &FindOuter;
    Fixture fixture(types, WorldKind::Game, outer);
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    ResetLog();
    REQUIRE(world.Initialize());
    CHECK(AlphaRecorder::sawOuterAtInitialize && BetaRecorder::sawOuterAtInitialize);
    REQUIRE(world.SpawnActor<LateActor>());
    REQUIRE(world.BeginPlay());
    REQUIRE(world.Shutdown());
    CHECK(LogIs("AinBinApoBpoAbpBbpLstLdsBepAepBpdApdBdeAde"));
    CHECK(AlphaRecorder::sawOuterAtEndPlay);
    CHECK(!AlphaRecorder::sawOuterAtPreDeinitialize && !BetaRecorder::sawOuterAtPreDeinitialize);

    // BeginPlay하지 않은 World는 OnWorldEndPlay를 받지 않는다.
    Fixture idle(types);
    REQUIRE(idle.registered);
    ResetLog();
    REQUIRE(idle.world->Initialize());
    REQUIRE(idle.world->Shutdown());
    CHECK(LogIs("AinBinApoBpoBpdApdBdeAde"));
}

namespace {

const char TimersTag = 'T';
const char CameraTag = 'C';

Result<void> RecordStage(World&, const TickContext&, void* user) noexcept {
    const char event[2] = {'s', 'g'};
    Log(*static_cast<const char*>(user), event);
    return Result<void>::Success();
}

} // namespace

// 16.5절 tickable: LateUpdate 시작 엔진 단계에서 Timers 뒤·Camera 전에, 등록 항목(OnLateTick) 앞에
// 돈다. pause 중에는 TickWhenPaused인 것만.
TEST(WorldSubsystem_TickableRunsAtLateUpdateStart) {
    const WorldSubsystemType types[] = {
        MakeSubsystemType<WorldSubsystem, TickingRecorder>("Ticking")};
    Fixture fixture(types);
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    REQUIRE(world.SetEngineStage(EngineStage::Timers, &RecordStage, const_cast<char*>(&TimersTag)));
    REQUIRE(world.SetEngineStage(EngineStage::CameraUpdate, &RecordStage,
                                 const_cast<char*>(&CameraTag)));
    REQUIRE(world.SpawnActor<LateActor>());
    ResetLog();
    REQUIRE(world.Tick(1.0 / 60.0));
    CHECK(LogIs("LstTsgKtkCsgLlt"));

    world.SetPaused(true);
    ResetLog();
    REQUIRE(world.Tick(1.0 / 60.0));
    CHECK(LogIs("Tsg"));
    TickingRecorder::whenPaused = true;
    ResetLog();
    REQUIRE(world.Tick(1.0 / 60.0));
    CHECK(LogIs("TsgKtp"));
    TickingRecorder::whenPaused = false;
    REQUIRE(world.Shutdown());
}

} // namespace world_subsystem_tests_file

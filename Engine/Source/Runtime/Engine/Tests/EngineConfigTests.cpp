#include <Config/CVar.h>
#include <Config/Tests/ConfigTestSupport.h>
#include <Engine/Engine.h>

// 엔진의 설정 sync 지점 시험이다(Docs/Design/ConfigSystem.ko.md 5.3절). 엔진은 호스트의 설정 문맥을
// 빌려 쓰고, 프레임마다 시작 단계 전에 Sync한다. 그래서 프레임 사이에 넣은 변경은 다음 프레임
// 시작부터 보이고, 프레임 안에서 넣은 변경은 그 프레임 동안 값을 바꾸지 않는다.
using namespace rocks;
using rocks::test::config::ConfigFixture;

namespace {

// 프레임 시작 단계(client.OnEngineTick)에서 읽은 값을 남기고, 지정한 프레임에서 변경을 큐에 넣는다.
class SettingsClient final : public EngineClient {
public:
    explicit SettingsClient(config::ConfigContext& context) noexcept : context_(context) {
    }

    void OnEngineTick(Engine& engine) noexcept override {
        const uint64 frame = engine.Time().frameIndex;
        if (frame <= MaxFrames) {
            seen[frame] = context_.Get<config_fixture::GraphSettings>().immediate;
        }

        // 2번 프레임 안에서 끄기를 큐에 넣는다. 이 프레임 동안에는 값이 그대로다.
        if (frame == 2) {
            queuedInFrame = context_
                                .AddAssignment(config::ConfigLayer::Console, "frame",
                                               "render.graph.immediate", "false")
                                .IsOk();
            seenAfterQueue = context_.Get<config_fixture::GraphSettings>().immediate;
        }
    }

    static constexpr uint64 MaxFrames = 4;
    bool seen[MaxFrames + 1] = {};
    bool queuedInFrame = false;
    bool seenAfterQueue = false;

private:
    config::ConfigContext& context_;
};

} // namespace

TEST(Engine_ConfigSyncPointRunsBeforeFrameBegin) {
    ConfigFixture fixture;
    config::ConfigContext& settings = fixture.Context();
    SystemAllocator allocator;
    SettingsClient client(settings);
    EngineConfig config;
    config.settings = &settings;
    config.client = &client;
    config.fixedFrameDeltaSeconds = 1.0 / 60.0;
    Engine engine(allocator);
    REQUIRE(engine.Initialize(config));

    REQUIRE(engine.RunFrame());
    // 프레임 사이에 넣은 변경은 다음 프레임 시작 전 sync 지점에서 값이 된다.
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "between",
                                   "render.graph.immediate", "true"));
    CHECK(!settings.Get<config_fixture::GraphSettings>().immediate);
    REQUIRE(engine.RunFrame());
    REQUIRE(engine.RunFrame());
    CHECK(!client.seen[1]);
    CHECK(client.seen[2]);
    CHECK(client.queuedInFrame);
    CHECK(client.seenAfterQueue);
    // 2번 프레임 안에서 넣은 끄기는 3번 프레임 시작에 적용됐다(Console 층의 나중 입력이 이긴다).
    CHECK(!client.seen[3]);
    REQUIRE(engine.Shutdown());
}

namespace engine_config_test {

constinit CVar<int32> WorldViewDistance =
    cvar::Var("render.world_scope.distance", 100).Help("Per-world view distance");

// 초기화가 늘 실패하는 Subsystem이다(World Initialize 실패 경로).
class FailingSubsystem final : public WorldSubsystem {
public:
    Result<void> Initialize(World&) noexcept override {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
};

} // namespace engine_config_test

// World는 엔진 설정 문맥에 자기 CVar 범위를 만들고(Initialize — Global의 자식) Shutdown이 파괴한다
// (Docs/Design/CVarSystem.ko.md 14.2절). World 범위의 값은 그 World에서만 보이고, 플레이어 같은
// 자식 범위는 World 범위의 값을 물려받는다. World가 끝나면 범위 값은 버려진다(Global 값).
TEST(Engine_WorldOwnsCVarScope) {
    using engine_config_test::WorldViewDistance;
    ConfigFixture fixture;
    config::ConfigContext& settings = fixture.Context();
    REQUIRE(settings.Register(WorldViewDistance));
    SystemAllocator allocator;
    EngineConfig config;
    config.settings = &settings;
    config.fixedFrameDeltaSeconds = 1.0 / 60.0;
    Engine engine(allocator);
    REQUIRE(engine.Initialize(config));
    WorldCreateDesc desc;
    desc.name = "First";
    auto first = engine.CreateWorld(desc);
    desc.name = "Second";
    auto second = engine.CreateWorld(desc);
    REQUIRE(first && second);
    const cvar::CVarScopeId firstScope = first.Value()->CVarScope();
    const cvar::CVarScopeId secondScope = second.Value()->CVarScope();
    CHECK(!firstScope.IsGlobal() && !secondScope.IsGlobal() && firstScope != secondScope);
    auto player = settings.CreateScope(firstScope);
    REQUIRE(player);
    REQUIRE(WorldViewDistance.Set(300, {.scope = firstScope}));
    REQUIRE(engine.RunFrame());
    CHECK(WorldViewDistance.Get(firstScope) == 300 && WorldViewDistance.Get(player.Value()) == 300);
    CHECK(WorldViewDistance.Get(secondScope) == 100 && WorldViewDistance.Get() == 100);

    // World를 끝내면 범위는 파괴되고 자식은 Global에 다시 붙는다(다음 sync 지점부터).
    REQUIRE(engine.DestroyWorld(first.Value()));
    REQUIRE(engine.RunFrame());
    REQUIRE(engine.RunFrame());
    CHECK(WorldViewDistance.Get(firstScope) == 100 && WorldViewDistance.Get(player.Value()) == 100);
    REQUIRE(settings.DestroyScope(player.Value()));
    REQUIRE(engine.Shutdown());
    REQUIRE(settings.Sync());
    CHECK(WorldViewDistance.Get(secondScope) == 100);
}

// World Initialize가 실패하면(Subsystem 초기화 실패) 만든 CVar 범위를 돌려준다(CVarSystem 14.2절).
// 범위 id는 프로세스 카운터라 실패한 World의 범위는 앞뒤로 만든 범위 사이의 id다.
TEST(Engine_FailedWorldInitializeReleasesCVarScope) {
    ConfigFixture fixture;
    config::ConfigContext& settings = fixture.Context();
    SystemAllocator allocator;
    EngineConfig config;
    config.settings = &settings;
    config.fixedFrameDeltaSeconds = 1.0 / 60.0;
    Engine engine(allocator);
    REQUIRE(engine.Initialize(config));
    auto before = settings.CreateScope();
    REQUIRE(before);
    const WorldSubsystemType types[] = {
        MakeSubsystemType<WorldSubsystem, engine_config_test::FailingSubsystem>("Failing")};
    WorldCreateDesc desc;
    desc.name = "Failing";
    desc.subsystems = types;
    CHECK(!engine.CreateWorld(desc));
    auto after = settings.CreateScope();
    REQUIRE(after);
    CHECK(after.Value().value > before.Value().value + 1);
    for (uint64 id = before.Value().value + 1; id < after.Value().value; ++id) {
        CHECK(settings.DestroyScope(cvar::CVarScopeId{id}).Error() == ErrorCode::NotFound);
    }

    REQUIRE(settings.DestroyScope(before.Value()));
    REQUIRE(settings.DestroyScope(after.Value()));
    REQUIRE(engine.Shutdown());
}

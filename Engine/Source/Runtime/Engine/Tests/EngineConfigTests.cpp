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

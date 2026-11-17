#include "ConfigTestSupport.h"

#include <Platform/Thread.h>

// 설정 CF2 콘솔 설정 시험이다(Docs/Design/ConfigSystem.ko.md 5.1절): 등록 전 값(대기 값)의 이관과
// 그 검사, 플래그별 거부(콘솔 읽기 전용·품질 구성원·개발 전용·사용자 설정·시작 뒤 고정), 설정
// struct 필드의 콘솔 이름이 같은 값 저장소를 쓰는지, 종류·범위·허용 값, 이름 문법·영역 표, 소유
// 스레드 밖 호출.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace {

ConsoleSettingDefinition Define(StringView name, ConsoleValueType type, StringView defaultValue,
                                uint32 flags = 0) noexcept {
    ConsoleSettingDefinition definition;
    definition.name = name;
    definition.help = "test setting";
    definition.type = type;
    definition.defaultValue = defaultValue;
    definition.flags = flags;
    return definition;
}

uint32 Flags(ConsoleSettingFlags flag) noexcept {
    return static_cast<uint32>(flag);
}

const FieldValue& ValueOf(ConfigFixture& fixture, StringView name) noexcept {
    auto found = fixture.Context().FindConsoleSetting(name);
    if (!found) {
        platform::Fatal("console setting missing in test");
    }

    return fixture.Context().ConsoleValue(found.Value());
}

} // namespace

// 등록 전에 층으로 들어온 이름의 값은 대기 값으로 남았다가 등록 때 검사·적용된다. 형식이 맞지
// 않거나 범위 밖인 대기 값은 그 위치의 진단을 남기고 건너뛴다(등록은 성공하고 기본값을 쓴다).
TEST(Config_PendingValuesMoveAtRegistration) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine",
                                 "{\"Settings\": {\"stream.pool_mb\": 1024,\n"
                                 "  \"stream.lines\": \"many\", \"stream.cap\": 99999}}"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "stream.late", "7"));
    REQUIRE(context.Sync());
    auto missing = context.FindConsoleSetting("stream.pool_mb");
    CHECK(!missing && missing.Error() == ErrorCode::NotFound);

    ConsoleSettingDefinition pool = Define("stream.pool_mb", ConsoleValueType::Int32, "512");
    pool.hasRange = true;
    pool.minimum = 64;
    pool.maximum = 4096;
    auto poolId = context.RegisterConsoleSetting(pool);
    REQUIRE(poolId);
    CHECK(context.ConsoleValue(poolId.Value()).signedInteger == 1024);

    context.Diagnostics().Clear();
    REQUIRE(context.RegisterConsoleSetting(Define("stream.lines", ConsoleValueType::Int32, "3")));
    CHECK(ValueOf(fixture, "stream.lines").signedInteger == 3);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting, 2, 19));
    ConsoleSettingDefinition cap = Define("stream.cap", ConsoleValueType::Int32, "10");
    cap.hasRange = true;
    cap.minimum = 0;
    cap.maximum = 100;
    REQUIRE(context.RegisterConsoleSetting(cap));
    CHECK(ValueOf(fixture, "stream.cap").signedInteger == 10);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::ValueOutOfRange, 2, 41));
    REQUIRE(context.RegisterConsoleSetting(Define("stream.late", ConsoleValueType::Int64, "1")));
    CHECK(ValueOf(fixture, "stream.late").signedInteger == 7);

    // 등록 뒤의 값은 Sync에서만 바뀌고, 등록된 이름의 값은 들어올 때 검사한다.
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "stream.pool_mb", "2048"));
    CHECK(context.ConsoleValue(poolId.Value()).signedInteger == 1024);
    REQUIRE(context.Sync());
    CHECK(context.ConsoleValue(poolId.Value()).signedInteger == 2048);
    context.Diagnostics().Clear();
    auto outside = context.AddAssignment(ConfigLayer::Console, "console", "stream.pool_mb", "1");
    CHECK(!outside && fixture.HasDiagnostic(ConfigDiagnosticCode::ValueOutOfRange));
}

// 플래그가 층 쓰기를 거부한다: 콘솔 읽기 전용은 콘솔 층만, 품질 구성원이 아니면 품질 층을(설정
// struct 필드의 콘솔 이름은 구성원이 아니다), 사용자 설정이 아니면 사용자 층을, 시작 뒤 고정은
// CompleteStartup 뒤 새 쓰기를 거부한다. 다른 층은 받는다.
TEST(Config_ConsoleFlagsRejectLayers) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.RegisterConsoleSetting(Define("render.lock", ConsoleValueType::Bool, "false",
                                                  Flags(ConsoleSettingFlags::ConsoleReadOnly))));
    REQUIRE(context.RegisterConsoleSetting(Define("quality.shadow_bias", ConsoleValueType::Float,
                                                  "1", Flags(ConsoleSettingFlags::QualityMember))));
    REQUIRE(context.RegisterConsoleSetting(Define("render.bias", ConsoleValueType::Float, "1")));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render", "project",
                                 R"({"Settings": {"render.lock": true}})"));
    REQUIRE(context.AddLayerText(ConfigLayer::Quality, "shadow@High", "Render", "quality",
                                 R"({"Settings": {"quality.shadow_bias": 0.5}})"));
    REQUIRE(context.Sync());
    CHECK(ValueOf(fixture, "render.lock").boolean);
    CHECK(ValueOf(fixture, "quality.shadow_bias").float32 == 0.5f);

    context.Diagnostics().Clear();
    auto locked = context.AddAssignment(ConfigLayer::Console, "console", "render.lock", "false");
    CHECK(!locked && fixture.HasDiagnostic(ConfigDiagnosticCode::ConsoleReadOnly));
    CHECK(fixture.Rejects(ConfigLayer::Quality, "Render", R"({"Settings": {"render.bias": 2.0}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::NotQualityMember, 1, 30));
    CHECK(fixture.Rejects(ConfigLayer::Quality, "Render",
                          R"({"Settings": {"render.graph.immediate": true}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::NotQualityMember, 1, 41));
    // 사용자 층: UserConfig 필드(game.input.sensitivity)는 받고 아닌 것은 거부한다.
    REQUIRE(context.AddLayerText(ConfigLayer::User, "Game.json", "Game", "user",
                                 R"({"Settings": {"game.input.sensitivity": 2.5}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Rejects(ConfigLayer::User, "Game",
                          R"({"Settings": {"game.rules.score_limit": 5}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed, 1, 41));
    // 시작 뒤 고정: CompleteStartup 전에는 받고, 뒤에는 새 쓰기를 거부하며 값은 그대로다.
    REQUIRE(context.AddAssignment(ConfigLayer::Project, "seed", "game.rules.seed", "11"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    context.Diagnostics().Clear();
    auto late = context.AddAssignment(ConfigLayer::Console, "console", "game.rules.seed", "12");
    CHECK(!late && fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed));
    CHECK(fixture.Gameplay().seed == 11 && fixture.Gameplay().sensitivity == 2.5f);
}

// 출시 규칙에서 개발 전용 설정은 등록되지만 콘솔에 보이지 않고, 어느 층도 쓸 수 없어 기본값으로
// 고정된다 (독립 설정·설정 struct 필드 모두). 콘솔 입력 자체도 없다.
TEST(Config_DevelopmentOnlyIsFixedUnderReleaseRules) {
    ConfigContextOptions options;
    options.releaseRules = true;
    ConfigFixture fixture(options);
    ConfigContext& context = fixture.Context();
    auto debug =
        context.RegisterConsoleSetting(Define("render.debug_view", ConsoleValueType::Bool, "false",
                                              Flags(ConsoleSettingFlags::DevelopmentOnly)));
    REQUIRE(debug);
    auto hidden = context.FindConsoleSetting("render.debug_view");
    CHECK(!hidden && hidden.Error() == ErrorCode::NotFound);
    CHECK(!context.FindConsoleSetting("render.graph.debug_draw"));
    REQUIRE(context.Sync());
    CHECK(fixture.Rejects(ConfigLayer::Project, "Render",
                          R"({"Settings": {"render.debug_view": true}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed, 1, 36));
    CHECK(fixture.Rejects(ConfigLayer::Project, "Render",
                          R"({"Settings": {"render.graph.debug_draw": true}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed));
    REQUIRE(context.Sync());
    CHECK(!context.ConsoleValue(debug.Value()).boolean && !fixture.Graph().debugDraw);
    Array<StringView> names(fixture.system);
    REQUIRE(context.CompleteConsoleName("render.", names));
    for (const StringView name : names) {
        CHECK(name != "render.debug_view" && name != "render.graph.debug_draw");
    }

    auto console = context.ExecuteConsoleLine("render.graph.immediate", ConsoleOutput{});
    CHECK(!console && console.Error() == ErrorCode::Unsupported);
}

// 설정 struct 필드의 콘솔 이름(ConsoleName)은 그 필드와 값 저장소를 공유한다: `Settings` 섹션·콘솔
// 대입이 필드를 바꾸고, 출처 스택은 필드 경로의 것이다. JSON 값 종류가 필드와 맞지 않으면 그 위치로
// 거부한다.
TEST(Config_FieldConsoleNamesShareTheValue) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine",
                                 R"({"RenderGraph": {"immediate": false, "quality": "Low"}})"));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render", "project",
                                 R"({"Settings": {"render.graph.immediate": true}})"));
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "render.graph.quality", "high"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().immediate && fixture.Graph().quality == FixtureQuality::High);
    CHECK(ValueOf(fixture, "render.graph.immediate").boolean);
    CHECK(ValueOf(fixture, "render.graph.quality").signedInteger ==
          static_cast<int64>(FixtureQuality::High));
    const ConfigLayer stack[] = {ConfigLayer::Engine, ConfigLayer::Project};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "immediate", stack));
    const ConfigLayer quality[] = {ConfigLayer::Engine, ConfigLayer::Console};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "quality", quality));
    // 같은 파일 안에서는 섹션 값 뒤에 콘솔 이름 값이 적용된다.
    REQUIRE(context.AddLayerText(
        ConfigLayer::Feature, "both", "Game", "feature",
        R"({"Gameplay": {"scoreLimit": 5}, "Settings": {"game.rules.score_limit": 6}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Gameplay().scoreLimit == 6);
    CHECK(fixture.Rejects(ConfigLayer::Project, "Render",
                          R"({"Settings": {"render.graph.immediate": 1}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting, 1, 41));
}

// 콘솔 설정 종류(int64·double·String·Name·enum)와 허용 값, 기본값 표기, 이름 문법·영역 표·겹침
// 거부.
TEST(Config_ConsoleSettingTypesAndRegistration) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.RegisterConsoleSetting(Define("net.max_bytes", ConsoleValueType::Int64, "0")));
    REQUIRE(
        context.RegisterConsoleSetting(Define("physics.gravity", ConsoleValueType::Double, "")));
    const StringView channels[] = {"core", "render"};
    ConsoleSettingDefinition channel = Define("log.channel", ConsoleValueType::String, "core");
    channel.allowedValues = channels;
    REQUIRE(context.RegisterConsoleSetting(channel));
    REQUIRE(context.RegisterConsoleSetting(Define("game.level", ConsoleValueType::Name, "Lobby")));
    ConsoleSettingDefinition preset = Define("quality.preset", ConsoleValueType::Enum, "");
    preset.enumType = TypeIdOf<FixtureQuality>();
    const StringView presets[] = {"Low", "High"};
    preset.allowedValues = presets;
    REQUIRE(context.RegisterConsoleSetting(preset));
    CHECK(ValueOf(fixture, "physics.gravity").float64 == 0.0);
    CHECK(ValueOf(fixture, "quality.preset").signedInteger == 0);

    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "net.max_bytes", "9000000000"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "physics.gravity", "-9.81"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "log.channel", "render"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "game.level", "Arena"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "quality.preset", "high"));
    REQUIRE(context.Sync());
    CHECK(ValueOf(fixture, "net.max_bytes").signedInteger == 9000000000ll);
    CHECK(ValueOf(fixture, "physics.gravity").float64 == -9.81);
    CHECK(ValueOf(fixture, "log.channel").string.View() == "render");
    CHECK(ValueOf(fixture, "quality.preset").signedInteger ==
          static_cast<int64>(FixtureQuality::High));
    auto level = fixture.names.Acquire(ValueOf(fixture, "game.level").name);
    CHECK(level && level.Value().View() == "Arena");

    context.Diagnostics().Clear();
    CHECK(!context.AddAssignment(ConfigLayer::Session, "cli", "log.channel", "audio"));
    CHECK(!context.AddAssignment(ConfigLayer::Session, "cli", "quality.preset", "Medium"));
    CHECK(!context.AddAssignment(ConfigLayer::Session, "cli", "net.max_bytes", "lots"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::ValueOutOfRange) &&
          fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting));

    // 등록 거부: 이름 문법, 영역 표, help 없음, 범위 모양, 기본값 표기, 겹침(필드 콘솔 이름 포함).
    auto rejects = [&](const ConsoleSettingDefinition& definition, ErrorCode code) noexcept {
        auto registered = context.RegisterConsoleSetting(definition);
        return !registered && registered.Error() == code;
    };
    CHECK(rejects(Define("Render.X", ConsoleValueType::Bool, ""), ErrorCode::InvalidArgument));
    CHECK(rejects(Define("render", ConsoleValueType::Bool, ""), ErrorCode::InvalidArgument));
    CHECK(rejects(Define("weather.rain", ConsoleValueType::Bool, ""), ErrorCode::InvalidArgument));
    ConsoleSettingDefinition helpless = Define("render.helpless", ConsoleValueType::Bool, "");
    helpless.help = {};
    CHECK(rejects(helpless, ErrorCode::InvalidArgument));
    ConsoleSettingDefinition ranged = Define("render.ranged", ConsoleValueType::Bool, "");
    ranged.hasRange = true;
    CHECK(rejects(ranged, ErrorCode::InvalidArgument));
    CHECK(rejects(Define("render.typo", ConsoleValueType::Int32, "x"), ErrorCode::InvalidArgument));
    CHECK(rejects(Define("render.graph.immediate", ConsoleValueType::Bool, ""),
                  ErrorCode::AlreadyExists));
    REQUIRE(context.RegisterConsoleRegion("weather"));
    REQUIRE(context.RegisterConsoleSetting(Define("weather.rain", ConsoleValueType::Bool, "")));
    CHECK(context.RegisterConsoleRegion("weather").Error() == ErrorCode::AlreadyExists);
}

namespace {

struct ForeignCall {
    ConfigContext* context = nullptr;
    ErrorCode added = ErrorCode::None;
    ErrorCode synced = ErrorCode::None;
};

void CallFromOtherThread(void* argument) noexcept {
    auto& call = *static_cast<ForeignCall*>(argument);
    auto added = call.context->AddAssignment(ConfigLayer::Console, "console",
                                             "render.graph.immediate", "true");
    call.added = added ? ErrorCode::None : added.Error();
    auto synced = call.context->Sync();
    call.synced = synced ? ErrorCode::None : synced.Error();
}

} // namespace

// 값을 바꾸는 API는 소유 스레드(게임 lane)에서만 받는다. 다른 스레드의 호출은 WrongThread이고
// 값·큐는 그대로다(Join이 순서를 정한다 — 시간에 기대지 않는다).
TEST(Config_ForeignThreadCallsAreRejected) {
    ConfigFixture fixture;
    ForeignCall call;
    call.context = &fixture.Context();
    platform::Thread thread;
    REQUIRE(thread.Start(&CallFromOtherThread, &call) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(call.added == ErrorCode::WrongThread && call.synced == ErrorCode::WrongThread);
    CHECK(!fixture.Context().HasPendingChanges() && !fixture.Graph().immediate);
}

#include "ConfigTestSupport.h"

#include <Platform/Thread.h>

// 콘솔 설정 시험이다(Docs/Design/ConfigSystem.ko.md 5.1절, CVarSystem.ko.md 5절 — 동적 변수는
// RegisterDynamic, CVR7): 등록 전 값(대기 값)의 이관과 그 검사, 성질별 거부(콘솔 읽기
// 전용·품질 묶음 층·치트·사용자 설정·부팅 값), 설정 struct 필드의 콘솔 이름이 같은 값 저장소를
// 쓰는지, 타입·범위·enum, 이름 문법·영역 표·겹침, snapshot 읽기(ReadDynamic), 소유 스레드 밖 호출.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace {

DynamicVarDefinition Define(StringView name, ConsoleValueType type, StringView defaultValue,
                            uint32 flags = 0) noexcept {
    DynamicVarDefinition definition;
    definition.name = name;
    definition.help = "test setting";
    definition.type = type;
    definition.defaultValue = defaultValue;
    definition.flags = flags;
    return definition;
}

uint32 Flags(cvar::VarFlags flag) noexcept {
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

    DynamicVarDefinition pool = Define("stream.pool_mb", ConsoleValueType::Int32, "512");
    pool.hasRange = true;
    pool.minimum = 64;
    pool.maximum = 4096;
    auto poolIndex = context.RegisterDynamic(pool);
    REQUIRE(poolIndex);
    // 등록은 바로 보인다(새 칸을 더한 snapshot을 게시한다 — 같은 번호 체계의 CVar 칸).
    CHECK(ValueOf(fixture, "stream.pool_mb").signedInteger == 1024);
    CHECK(cvar::ReadDynamic<int32>(*context.Acquire(), poolIndex.Value(), 0) == 1024);

    context.Diagnostics().Clear();
    REQUIRE(context.RegisterDynamic(Define("stream.lines", ConsoleValueType::Int32, "3")));
    CHECK(ValueOf(fixture, "stream.lines").signedInteger == 3);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting, 2, 19));
    DynamicVarDefinition cap = Define("stream.cap", ConsoleValueType::Int32, "10");
    cap.hasRange = true;
    cap.minimum = 0;
    cap.maximum = 100;
    REQUIRE(context.RegisterDynamic(cap));
    CHECK(ValueOf(fixture, "stream.cap").signedInteger == 10);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::ValueOutOfRange, 2, 41));
    REQUIRE(context.RegisterDynamic(Define("stream.late", ConsoleValueType::Int64, "1")));
    CHECK(ValueOf(fixture, "stream.late").signedInteger == 7);

    // 등록 뒤의 값은 Sync에서만 바뀌고, 등록된 이름의 값은 들어올 때 검사한다.
    const SharedPtr<const ConfigSnapshot> before = context.Acquire();
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "stream.pool_mb", "2048"));
    CHECK(ValueOf(fixture, "stream.pool_mb").signedInteger == 1024);
    REQUIRE(context.Sync());
    CHECK(ValueOf(fixture, "stream.pool_mb").signedInteger == 2048);
    CHECK(cvar::ReadDynamic<int32>(*context.Acquire(), poolIndex.Value(), 0) == 2048);
    CHECK(cvar::ReadDynamic<int32>(*before, poolIndex.Value(), 0) == 1024);
    context.Diagnostics().Clear();
    auto outside = context.AddAssignment(ConfigLayer::Console, "console", "stream.pool_mb", "1");
    CHECK(!outside && fixture.HasDiagnostic(ConfigDiagnosticCode::ValueOutOfRange));
}

// 플래그가 층 쓰기를 거부한다: 콘솔 읽기 전용은 콘솔 층만, 사용자 설정이 아니면 사용자 층을
// 거부한다. 품질 묶음 층은 프리셋 묶음 펼침만 쓰므로 직접 쓰기를 모두 거부한다(LayerNotAllowed —
// 묶음 동작은 CVarGroupTests.cpp). 시작 뒤
// 고정은 CompleteStartup 뒤 콘솔 쓰기를 받되 값은 그대로이고 Warning BootValueIgnored를 한 번만
// 낸다(대기가 아니다). 다른 층은 받는다.
TEST(Config_ConsoleFlagsRejectLayers) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.RegisterDynamic(Define("render.lock", ConsoleValueType::Bool, "false",
                                           Flags(cvar::VarFlags::ConsoleReadOnly))));
    REQUIRE(context.RegisterDynamic(Define("render.bias", ConsoleValueType::Float, "1")));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render", "project",
                                 R"({"Settings": {"render.lock": true}})"));
    REQUIRE(context.Sync());
    CHECK(ValueOf(fixture, "render.lock").boolean);

    context.Diagnostics().Clear();
    auto locked = context.AddAssignment(ConfigLayer::Console, "console", "render.lock", "false");
    CHECK(!locked && fixture.HasDiagnostic(ConfigDiagnosticCode::ConsoleReadOnly));
    CHECK(fixture.Rejects(ConfigLayer::Quality, "Render", R"({"Settings": {"render.bias": 2.0}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed));
    context.Diagnostics().Clear();
    auto assigned = context.AddAssignment(ConfigLayer::Quality, "shadow", "render.bias", "2");
    CHECK(!assigned && fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed));
    CHECK(context.Unset(ConfigLayer::Quality, "shadow").Error() == ErrorCode::InvalidArgument);
    CHECK(context.ClearLayer(ConfigLayer::Quality).Error() == ErrorCode::InvalidArgument);
    // 사용자 층: UserConfig 필드(game.input.sensitivity)는 받고 아닌 것은 거부한다.
    REQUIRE(context.AddLayerText(ConfigLayer::User, "Game.json", "Game", "user",
                                 R"({"Settings": {"game.input.sensitivity": 2.5}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Rejects(ConfigLayer::User, "Game",
                          R"({"Settings": {"game.rules.score_limit": 5}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed, 1, 41));
    // 시작 뒤 고정: CompleteStartup 전에는 바로 적용된다. 뒤의 콘솔 쓰기는 넣을 때 BootSealed로
    // 거부한다(설정 struct 필드도 — CVar 20절 CVR6). 휘발 층(세션) 쓰기는 받지만 값은 그대로이고
    // 대기도 아니다(다음 실행에 오지 않는다). 진단은 같은 이름·출처에 한 번이다.
    REQUIRE(context.AddAssignment(ConfigLayer::Project, "seed", "game.rules.seed", "11"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    context.Diagnostics().Clear();
    auto console = context.AddAssignment(ConfigLayer::Console, "console", "game.rules.seed", "12");
    CHECK(!console && fixture.HasDiagnostic(ConfigDiagnosticCode::BootSealed));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "game.rules.seed", "12"));
    REQUIRE(context.Sync());
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "game.rules.title", "late"));
    REQUIRE(context.Sync());
    CHECK(fixture.Gameplay().seed == 11 && fixture.Gameplay().sensitivity == 2.5f);
    CHECK(fixture.Gameplay().title.View() == "late");
    usize ignored = 0;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        const ConfigDiagnostic item = context.Diagnostics().Get(index);
        ignored += item.code == ConfigDiagnosticCode::BootValueIgnored &&
                           item.severity == ConfigSeverity::Warning &&
                           item.path == "Game.Gameplay.seed"
                       ? 1
                       : 0;
    }

    CHECK(ignored == 1);
    Array<ConfigPendingRestart> pending(fixture.system);
    REQUIRE(context.GetPendingRestarts(pending));
    CHECK(pending.IsEmpty());

    // 파일 층(프로젝트)에서 온 시작 뒤 변경은 재시작 대기다(RestartRequired와 같은 규칙).
    REQUIRE(context.Unset(ConfigLayer::Session, "--set"));
    REQUIRE(context.AddAssignment(ConfigLayer::Project, "seed", "game.rules.seed", "13"));
    REQUIRE(context.Sync());
    CHECK(fixture.Gameplay().seed == 11);
    REQUIRE(context.GetPendingRestarts(pending));
    REQUIRE(pending.Size() == 1);
    CHECK(pending[0].name == "Game.Gameplay.seed" && pending[0].pending == "13" &&
          pending[0].current == "11");
}

// 출시 규칙에서 치트(개발 전용) 설정은 등록되지만 콘솔에 보이지 않고, 어느 층도 쓸 수 없어
// 기본값으로 고정된다(동적 변수·설정 struct 필드 모두). 콘솔 입력 자체도 없다.
TEST(Config_DevelopmentOnlyIsFixedUnderReleaseRules) {
    ConfigContextOptions options;
    options.releaseRules = true;
    ConfigFixture fixture(options);
    ConfigContext& context = fixture.Context();
    auto debug = context.RegisterDynamic(
        Define("render.debug_view", ConsoleValueType::Bool, "false", Flags(cvar::VarFlags::Cheat)));
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
    CHECK(!cvar::ReadDynamic<bool>(*context.Acquire(), debug.Value(), true) &&
          !fixture.Graph().debugDraw);
    Array<StringView> names(fixture.system);
    REQUIRE(context.CompleteConsoleName("render.", names));
    for (const StringView name : names) {
        CHECK(name != "render.debug_view" && name != "render.graph.debug_draw");
    }

    auto console = context.ExecuteConsoleLine("render.graph.immediate", SharedPtr<ConsoleSink>());
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

// 동적 변수의 타입(int64·double·String·Name·enum)과 기본값 표기, enum의 대소문자 무시 입력, 이름
// 문법·영역 표·성질 비트·범위 모양·겹침 거부(같은 타입 AlreadyExists·다른 타입 TypeMismatch).
TEST(Config_ConsoleSettingTypesAndRegistration) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.RegisterDynamic(Define("net.max_bytes", ConsoleValueType::Int64, "0")));
    auto gravity = context.RegisterDynamic(Define("physics.gravity", ConsoleValueType::Double, ""));
    REQUIRE(gravity);
    auto channel = context.RegisterDynamic(Define("log.channel", ConsoleValueType::String, "core"));
    REQUIRE(channel);
    REQUIRE(context.RegisterDynamic(Define("game.level", ConsoleValueType::Name, "Lobby")));
    DynamicVarDefinition preset = Define("quality.preset", ConsoleValueType::Enum, "");
    preset.enumType = TypeIdOf<FixtureQuality>();
    auto presetIndex = context.RegisterDynamic(preset);
    REQUIRE(presetIndex);
    CHECK(ValueOf(fixture, "physics.gravity").float64 == 0.0);
    CHECK(ValueOf(fixture, "quality.preset").signedInteger == 0);
    CHECK(cvar::ReadDynamic<String>(*context.Acquire(), channel.Value(), "") == "core");

    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "net.max_bytes", "9000000000"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "physics.gravity", "-9.81"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "log.channel", "render"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "game.level", "Arena"));
    // enum 이름은 대소문자를 가리지 않는다(CVar 4.4절 — 닫힌 값 집합은 enum이다).
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "quality.preset", "high"));
    REQUIRE(context.Sync());
    CHECK(ValueOf(fixture, "net.max_bytes").signedInteger == 9000000000ll);
    CHECK(ValueOf(fixture, "physics.gravity").float64 == -9.81);
    CHECK(ValueOf(fixture, "log.channel").string.View() == "render");
    CHECK(ValueOf(fixture, "quality.preset").signedInteger ==
          static_cast<int64>(FixtureQuality::High));
    const SharedPtr<const ConfigSnapshot> snapshot = context.Acquire();
    CHECK(cvar::ReadDynamic<double>(*snapshot, gravity.Value(), 0.0) == -9.81);
    CHECK(cvar::ReadDynamic<String>(*snapshot, channel.Value(), "") == "render");
    CHECK(cvar::ReadDynamic<int64>(*snapshot, presetIndex.Value(), 0) ==
          static_cast<int64>(FixtureQuality::High));
    CHECK(cvar::ReadDynamic<int32>(*snapshot, cvar::Index{}, 5) == 5);
    auto level = fixture.names.Acquire(ValueOf(fixture, "game.level").name);
    CHECK(level && level.Value().View() == "Arena");

    context.Diagnostics().Clear();
    CHECK(!context.AddAssignment(ConfigLayer::Session, "cli", "quality.preset", "Ultra"));
    CHECK(!context.AddAssignment(ConfigLayer::Session, "cli", "net.max_bytes", "lots"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting));

    // 등록 거부: 이름 문법, 영역 표, help 없음, 성질 비트, 범위 모양, 기본값 표기, enum 타입,
    // 겹침(필드 콘솔 이름·다른 타입·콘솔 명령 포함).
    auto rejects = [&](const DynamicVarDefinition& definition, ErrorCode code) noexcept {
        auto registered = context.RegisterDynamic(definition);
        return !registered && registered.Error() == code;
    };
    CHECK(rejects(Define("Render.X", ConsoleValueType::Bool, ""), ErrorCode::InvalidArgument));
    CHECK(rejects(Define("render", ConsoleValueType::Bool, ""), ErrorCode::InvalidArgument));
    CHECK(rejects(Define("weather.rain", ConsoleValueType::Bool, ""), ErrorCode::InvalidArgument));
    DynamicVarDefinition helpless = Define("render.helpless", ConsoleValueType::Bool, "");
    helpless.help = {};
    CHECK(rejects(helpless, ErrorCode::InvalidArgument));
    DynamicVarDefinition ranged = Define("render.ranged", ConsoleValueType::Bool, "");
    ranged.hasRange = true;
    CHECK(rejects(ranged, ErrorCode::InvalidArgument));
    DynamicVarDefinition inverted = Define("render.inverted", ConsoleValueType::Int32, "");
    inverted.hasRange = true;
    inverted.minimum = 5;
    inverted.maximum = 1;
    CHECK(rejects(inverted, ErrorCode::InvalidArgument));
    CHECK(
        rejects(Define("render.clamped", ConsoleValueType::Int32, "", Flags(cvar::VarFlags::Clamp)),
                ErrorCode::InvalidArgument));
    DynamicVarDefinition bootCheat =
        Define("render.boot_cheat", ConsoleValueType::Int32, "", Flags(cvar::VarFlags::Cheat));
    bootCheat.boot = true;
    CHECK(rejects(bootCheat, ErrorCode::InvalidArgument));
    CHECK(
        rejects(Define("render.untyped", ConsoleValueType::Enum, ""), ErrorCode::InvalidArgument));
    CHECK(rejects(Define("render.typo", ConsoleValueType::Int32, "x"), ErrorCode::InvalidArgument));
    CHECK(rejects(Define("render.graph.immediate", ConsoleValueType::Bool, ""),
                  ErrorCode::AlreadyExists));
    CHECK(rejects(Define("net.max_bytes", ConsoleValueType::Int64, "5"), ErrorCode::AlreadyExists));
    CHECK(rejects(Define("net.max_bytes", ConsoleValueType::Bool, ""), ErrorCode::TypeMismatch));
    CHECK(rejects(Define("cvar.find", ConsoleValueType::Bool, ""), ErrorCode::AlreadyExists));
    REQUIRE(context.RegisterConsoleRegion("weather"));
    REQUIRE(context.RegisterDynamic(Define("weather.rain", ConsoleValueType::Bool, "")));
    CHECK(context.RegisterConsoleRegion("weather").Error() == ErrorCode::AlreadyExists);
}

// 부팅 동적 변수는 BootCVar와 같은 규칙이다(CVar 9절): 봉인 뒤 바로 쓰는 층은 BootSealed, 파일 층은
// 재시작 대기, 봉인 뒤 snapshot은 부팅 블록 값을 읽는다. 봉인 뒤 등록한 부팅 변수도 그 값으로
// 굳는다.
TEST(Config_DynamicBootVariables) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    DynamicVarDefinition workers = Define("game.dynamic.workers", ConsoleValueType::Int32, "4",
                                          Flags(cvar::VarFlags::UserSetting));
    workers.boot = true;
    auto index = context.RegisterDynamic(workers);
    REQUIRE(index);
    REQUIRE(context.AddAssignment(ConfigLayer::Project, "project", "game.dynamic.workers", "6"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "game.dynamic.late", "9"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    CHECK(cvar::ReadDynamic<int32>(*context.Acquire(), index.Value(), 0) == 6);

    context.Diagnostics().Clear();
    CHECK(!context.AddAssignment(ConfigLayer::Console, "console", "game.dynamic.workers", "8"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::BootSealed));
    REQUIRE(context.AddAssignment(ConfigLayer::Project, "update", "game.dynamic.workers", "7"));
    REQUIRE(context.Sync());
    CHECK(cvar::ReadDynamic<int32>(*context.Acquire(), index.Value(), 0) == 6);
    Array<ConfigPendingRestart> pending(fixture.system);
    REQUIRE(context.GetPendingRestarts(pending));
    REQUIRE(pending.Size() == 1);
    CHECK(pending[0].name == "game.dynamic.workers" && pending[0].pending == "7");

    DynamicVarDefinition late = Define("game.dynamic.late", ConsoleValueType::Int32, "1");
    late.boot = true;
    auto lateIndex = context.RegisterDynamic(late);
    REQUIRE(lateIndex);
    CHECK(cvar::ReadDynamic<int32>(*context.Acquire(), lateIndex.Value(), 0) == 9);
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

#include "ConfigTestSupport.h"

#include <Config/CVar.h>

// CVar CVR1 시험이다(Docs/Design/CVarSystem.ko.md 4·5·19절): 핸들이 상수 초기화되는지(constinit),
// 1·4절 예시가 컴파일되는지, 등록과 번호(멱등·프로세스 공유 번호·이름 충돌·영역·지역 핸들·한꺼번에
// 검사), 등록한 변수에 층 값이 닿는지(대기 값·범위 보정·Validate·enum 이름 — ADL 표와 리플렉션
// enum), 성질 플래그(부팅 고정·치트·콘솔 읽기 전용·사용자 설정). 컴파일 실패 사례는 Rocks.ToolTests
// native(CVarCompilerTests)가 실제 컴파일러로 확인한다.
using namespace rocks;
namespace c_var_tests_file {

using namespace rocks::config;
using namespace rocks::test::config;
} // namespace c_var_tests_file

namespace cvar_test {

// 4.4절 예시 모양의 enum이다(ADL 이름 표 — 차례 값, `Count` 있음).
enum class ShadowQuality : int32 {
    Off,
    Low,
    Medium,
    High,
    Epic,
    Count
};

constexpr Span<const StringView> CVarEnumNames(ShadowQuality) noexcept {
    static constexpr StringView names[] = {"Off", "Low", "Medium", "High", "Epic"};
    return names;
}

// 값을 직접 정한 enum이다(이름·값 쌍 표).
enum class Tier : int32 {
    Low = 10,
    High = 20
};

constexpr Span<const cvar::EnumEntry> CVarEnumNames(Tier) noexcept {
    static constexpr cvar::EnumEntry entries[] = {{"Low", 10}, {"High", 20}};
    return entries;
}

// 1절 예시다(이름 그대로).
constinit CVar<float> ShadowDistance = cvar::Var("render.shadow.distance", 8000.f)
                                           .Help("Max shadow distance (cm)")
                                           .Range(500, 20000)
                                           .Clamp();

constinit CVar<ShadowQuality> ShadowQualityLevel =
    cvar::Var("render.shadow.quality", ShadowQuality::High).Help("Shadow quality level");

constinit BootCVar<String> GraphicsBackend =
    cvar::Boot("render.backend", "vulkan").Help("Graphics backend");

constinit CVar<bool> GodMode = cvar::Var("game.god", false).Help("Invulnerable player").Cheat();

// 4.2절 예시다.
constinit CVar<int32> Samples =
    cvar::Var("render.aa.samples", 4)
        .Help("MSAA sample count")
        .Validate([](const int32& value) noexcept -> StringView {
            return (value & (value - 1)) == 0 ? StringView{} : "must be a power of two";
        });

// 4.1절 표의 나머지 타입이다.
constinit CVar<int64> StreamBudget =
    cvar::Var("stream.budget", int64{1} << 40).Help("Streaming budget in bytes").Category("stream");

constinit CVar<double> RenderScale =
    cvar::Var("render.scale", 1.0).Help("Resolution scale").Range(0.25, 2.0);

constinit CVar<rocks::Name> ProfileName =
    cvar::Var<rocks::Name>("game.profile", "Default")
        .Help("Profile name")
        .Validate([](StringView text) noexcept -> StringView {
            return text.IsEmpty() ? "must not be empty" : StringView{};
        });

constinit CVar<String> LogStyle =
    cvar::Var("ui.log_style", "plain")
        .Help("Log panel style")
        .Validate([](StringView text) noexcept -> StringView {
            return text == "plain" || text == "rich" ? StringView{} : "must be plain or rich";
        })
        .UserSetting()
        .ConsoleReadOnly();

constinit CVar<Tier> TierLevel = cvar::Var("game.tier", Tier::High).Help("Content tier");

// 리플렉션 enum(ROCKS_ENUM — 생성 메타데이터의 열거자 이름)이다.
constinit CVar<config_fixture::FixtureQuality> FixtureLevel =
    cvar::Var("game.fixture_quality", config_fixture::FixtureQuality::Medium)
        .Help("Reflected enum quality");

constinit BootCVar<int32> WorkerThreads =
    cvar::Boot("game.threads", 4).Help("Worker thread count").Range(1, 64).UserSetting();

constinit CVar<bool> ReplicatedFlag =
    cvar::Var("net.replicated_flag", true).Help("Replicated test flag").Replicated();

// 이름 충돌 시험용이다(같은 이름·같은 타입, 같은 이름·다른 타입).
constinit CVar<float> ShadowDistanceTwin =
    cvar::Var("render.shadow.distance", 1000.f).Help("Same name and type");

constinit CVar<int32> ShadowDistanceInteger =
    cvar::Var("render.shadow.distance", 1000).Help("Same name, other type");

// 영역 표에 없는 첫 마디다.
constinit CVar<int32> WeatherRain = cvar::Var("weather.rain", 1).Help("Rain amount");

// 한 번도 등록하지 않는 핸들이다(번호 없음).
constinit CVar<int32> NeverRegistered = cvar::Var("game.never", 1).Help("Never registered");

} // namespace cvar_test

namespace c_var_tests_file {

namespace {

using namespace cvar_test;

const FieldValue& ValueOf(ConfigFixture& fixture, StringView name) noexcept {
    auto found = fixture.Context().FindConsoleSetting(name);
    if (!found) {
        platform::Fatal("CVar setting missing in test");
    }

    return fixture.Context().ConsoleValue(found.Value());
}

} // namespace

// 핸들의 상수 부분(이름·도움말·기본값)은 정의 그대로이고, 등록 전에는 번호가 없다.
TEST(CVar_HandleCarriesConstantDefinition) {
    CHECK(ShadowDistance.Name() == "render.shadow.distance");
    CHECK(ShadowDistance.Help() == "Max shadow distance (cm)");
    CHECK(ShadowDistance.DefaultValue() == 8000.f);
    CHECK(ShadowQualityLevel.DefaultValue() == ShadowQuality::High);
    CHECK(GraphicsBackend.DefaultValue() == "vulkan");
    CHECK(ProfileName.DefaultValue() == "Default");
    CHECK(StreamBudget.DefaultValue() == (int64{1} << 40));
    CHECK(TierLevel.DefaultValue() == Tier::High);
    CHECK(!NeverRegistered.Index().IsValid());
    if constexpr (IsReleaseBuild) {
        // 출시 빌드의 치트 변수는 이름·도움말을 담지 않는다(13.1절).
        CHECK(GodMode.Name().IsEmpty() && GodMode.Help().IsEmpty());
    } else {
        CHECK(GodMode.Name() == "game.god");
    }
}

// 번호는 처음 등록될 때 한 번 정해지고 문맥이 여럿이어도 같다. 같은 핸들을 다시(같은 호출 안 포함)
// 등록하면 성공이다. 등록한 변수는 기본값을 갖고 이름으로 찾을 수 있다.
TEST(CVar_RegisterAssignsOneProcessIndex) {
    ConfigFixture first;
    REQUIRE(first.Context().Register(ShadowDistance, ShadowQualityLevel, GraphicsBackend, Samples,
                                     StreamBudget, RenderScale, ProfileName, LogStyle, TierLevel,
                                     FixtureLevel, WorkerThreads, ReplicatedFlag));
    const cvar::Index index = ShadowDistance.Index();
    CHECK(index.IsValid());
    CHECK(ShadowQualityLevel.Index().IsValid() && ShadowQualityLevel.Index() != index);
    REQUIRE(first.Context().Register(ShadowDistance, ShadowDistance));
    CHECK(ShadowDistance.Index() == index);

    ConfigFixture second;
    REQUIRE(second.Context().Register(ShadowDistance));
    CHECK(ShadowDistance.Index() == index);

    REQUIRE(first.Context().Sync());
    CHECK(ValueOf(first, "render.shadow.distance").float32 == 8000.f);
    CHECK(ValueOf(first, "render.shadow.quality").signedInteger ==
          static_cast<int64>(ShadowQuality::High));
    CHECK(ValueOf(first, "render.backend").string.View() == "vulkan");
    CHECK(ValueOf(first, "stream.budget").signedInteger == (int64{1} << 40));
    CHECK(ValueOf(first, "render.scale").float64 == 1.0);
    CHECK(ValueOf(first, "game.tier").signedInteger == 20);
    CHECK(ValueOf(first, "game.fixture_quality").signedInteger ==
          static_cast<int64>(config_fixture::FixtureQuality::Medium));
    CHECK(!ValueOf(first, "game.profile").name.IsNone());
    CHECK(ValueOf(first, "game.threads").signedInteger == 4);
}

// 같은 이름을 다른 핸들이 쓰면 같은 타입 AlreadyExists, 다른 타입 TypeMismatch다(독립 콘솔 설정·
// 설정 struct 필드와의 충돌도 같다). 영역 표에 없는 이름은 InvalidArgument다. 넘긴 핸들을 모두
// 검사한 뒤 등록하므로 하나라도 거부되면 아무것도 등록되지 않는다.
TEST(CVar_RegisterRejectsConflicts) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    auto partial = context.Register(Samples, ShadowDistance, ShadowDistanceTwin);
    CHECK(!partial && partial.Error() == ErrorCode::AlreadyExists);
    CHECK(!context.FindConsoleSetting("render.aa.samples"));

    REQUIRE(context.Register(ShadowDistance));
    auto twin = context.Register(ShadowDistanceTwin);
    CHECK(!twin && twin.Error() == ErrorCode::AlreadyExists);
    auto integer = context.Register(ShadowDistanceInteger);
    CHECK(!integer && integer.Error() == ErrorCode::TypeMismatch);

    // 설정 struct 필드의 콘솔 이름(game.rules.seed는 int32 필드)과 독립 콘솔 설정과의 충돌이다.
    static constinit CVar<int32> SeedTwin =
        cvar::Var("game.rules.seed", 7).Help("Same name as a settings field");
    static constinit CVar<bool> SeedBool =
        cvar::Var("game.rules.seed", false).Help("Same name, other type");
    auto field = context.Register(SeedTwin);
    CHECK(!field && field.Error() == ErrorCode::AlreadyExists);
    auto fieldType = context.Register(SeedBool);
    CHECK(!fieldType && fieldType.Error() == ErrorCode::TypeMismatch);

    auto region = context.Register(WeatherRain);
    CHECK(!region && region.Error() == ErrorCode::InvalidArgument);
    REQUIRE(context.RegisterConsoleRegion("weather"));
    REQUIRE(context.Register(WeatherRain));
}

// 핸들은 정적 저장 기간이어야 한다: 함수 안 지역 핸들은 Debug·Development에서 InvalidArgument다.
// 함수 안 static 핸들은 받는다.
TEST(CVar_RegisterRejectsLocalHandles) {
    if constexpr (!IsReleaseBuild) {
        ConfigFixture fixture;
        CVar<int32> local = cvar::Var("game.local", 1).Help("Local handle");
        auto rejected = fixture.Context().Register(local);
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
        CHECK(!local.Index().IsValid());
        static constinit CVar<int32> StaticLocal =
            cvar::Var("game.static_local", 1).Help("Function static handle");
        REQUIRE(fixture.Context().Register(StaticLocal));
    }
}

// 등록 전에 들어온 값(대기 값)은 등록 때 적용된다: enum은 이름(ASCII 대소문자 무시)으로 읽고, 범위
// 밖 값은 `Clamp()`면 보정해 받고(Note Clamped) 아니면 거부하며, Validate가 거부한 값은
// ValidationFailed다. 등록 뒤의 입력은 넣을 때 같은 검사를 한다.
TEST(CVar_LayerValuesReachRegisteredVars) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine",
                                 "{\"Settings\": {\"render.shadow.quality\": \"low\",\n"
                                 "  \"render.shadow.distance\": 50000, \"render.aa.samples\": 3,\n"
                                 "  \"game.tier\": \"Low\", \"game.fixture_quality\": \"High\"}}"));
    REQUIRE(context.Sync());
    context.Diagnostics().Clear();
    REQUIRE(context.Register(ShadowQualityLevel, ShadowDistance, Samples, TierLevel, FixtureLevel,
                             RenderScale, LogStyle, ProfileName));
    CHECK(ValueOf(fixture, "render.shadow.quality").signedInteger ==
          static_cast<int64>(ShadowQuality::Low));
    CHECK(ValueOf(fixture, "render.shadow.distance").float32 == 20000.f);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::Clamped, 2, 29));
    CHECK(ValueOf(fixture, "render.aa.samples").signedInteger == 4);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::ValidationFailed, 2, 57));
    CHECK(ValueOf(fixture, "game.tier").signedInteger == 10);
    CHECK(ValueOf(fixture, "game.fixture_quality").signedInteger ==
          static_cast<int64>(config_fixture::FixtureQuality::High));

    // 등록 뒤: 범위 밖(Clamp 없음)·표 밖 enum·Validate 위반은 넣을 때 거부하고, 보정은 받는다.
    REQUIRE(context.Sync());
    CHECK(
        fixture.Rejects(ConfigLayer::Project, "Render", R"({"Settings": {"render.scale": 4.0}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::ValueOutOfRange, 1, 31));
    CHECK(fixture.Rejects(ConfigLayer::Project, "Render",
                          R"({"Settings": {"game.tier": "Middle"}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting, 1, 28));
    CHECK(fixture.Rejects(ConfigLayer::Project, "Render",
                          R"({"Settings": {"ui.log_style": "fancy"}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::ValidationFailed, 1, 31));
    CHECK(fixture.Rejects(ConfigLayer::Project, "Render", R"({"Settings": {"game.profile": ""}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::ValidationFailed, 1, 31));

    context.Diagnostics().Clear();
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "render.shadow.distance", "100"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::Clamped));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "render.aa.samples", "8"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "ui.log_style", "rich"));
    CHECK(ValueOf(fixture, "render.aa.samples").signedInteger == 4);
    REQUIRE(context.Sync());
    CHECK(ValueOf(fixture, "render.shadow.distance").float32 == 500.f);
    CHECK(ValueOf(fixture, "render.aa.samples").signedInteger == 8);
    CHECK(ValueOf(fixture, "ui.log_style").string.View() == "rich");
}

// 성질 플래그: 부팅 변수는 시작 뒤 이번 실행 값이 그대로다(StartupOnly 규칙 — 봉인 뒤 쓰기 거부는
// CVR4). ConsoleReadOnly는 콘솔 층만, UserSetting이 아니면 사용자 층을 거부한다.
TEST(CVar_FlagsFollowConsoleSettingRules) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(WorkerThreads, LogStyle, Samples));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "game.threads", "8"));
    REQUIRE(context.Sync());
    CHECK(ValueOf(fixture, "game.threads").signedInteger == 8);
    REQUIRE(context.CompleteStartup());
    // 봉인 뒤 콘솔 쓰기는 넣을 때 거부한다(BootSealed — 9절). 휘발 층(세션)은 받되 값은 그대로다.
    auto sealed = context.AddAssignment(ConfigLayer::Console, "console", "game.threads", "16");
    CHECK(!sealed && fixture.HasDiagnostic(ConfigDiagnosticCode::BootSealed));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "game.threads", "16"));
    REQUIRE(context.Sync());
    CHECK(ValueOf(fixture, "game.threads").signedInteger == 8);
    CHECK(WorkerThreads.Get() == 8);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::BootValueIgnored));

    context.Diagnostics().Clear();
    auto console = context.AddAssignment(ConfigLayer::Console, "console", "ui.log_style", "rich");
    CHECK(!console && fixture.HasDiagnostic(ConfigDiagnosticCode::ConsoleReadOnly));
    REQUIRE(context.AddLayerText(ConfigLayer::User, "Ui.json", "Game", "user",
                                 R"({"Settings": {"ui.log_style": "rich"}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Rejects(ConfigLayer::User, "Game", R"({"Settings": {"render.aa.samples": 2}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed));
    REQUIRE(context.Sync());
    CHECK(ValueOf(fixture, "ui.log_style").string.View() == "rich");
}

// 치트 변수는 출시 규칙에서 콘솔에 보이지 않는다(개발 빌드의 출시 규칙 시험 — 등록은 하되 숨김·
// 기본값 고정, 실제 출시 빌드는 등록하지 않는다 — 13.1절).
TEST(CVar_CheatVarsAreHiddenUnderReleaseRules) {
    ConfigContextOptions options;
    options.releaseRules = true;
    ConfigFixture fixture(options);
    REQUIRE(fixture.Context().Register(GodMode));
    auto hidden = fixture.Context().FindConsoleSetting("game.god");
    CHECK(!hidden && hidden.Error() == ErrorCode::NotFound);

    ConfigFixture development;
    REQUIRE(development.Context().Register(GodMode));
    if constexpr (IsReleaseBuild) {
        CHECK(!GodMode.Index().IsValid());
        CHECK(!development.Context().FindConsoleSetting("game.god"));
    } else {
        CHECK(GodMode.Index().IsValid());
        CHECK(!ValueOf(development, "game.god").boolean);
    }
}
} // namespace c_var_tests_file

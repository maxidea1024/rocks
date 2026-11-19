#include "ConfigTestSupport.h"

#include <Config/ConfigLoader.h>
#include <Config/UserConfig.h>
#include <Core/Text/StringBuilder.h>

// 설정 CF3 사용자 층 시험이다(Docs/Design/ConfigSystem.ko.md 8절): 저장 왕복과 차이만 기록(아래
// 층과 같은 값·세션 층 값은 쓰지 않는다, 값이 없어진 branch의 옛 파일은 지운다), 오래된 사용자
// 파일의 관대한 읽기(모르는 키는 Warning 후 버린다), 재시작 필요 값의 대기와 다음 시작 적용.
using namespace rocks;
namespace user_config_tests_file {

using namespace rocks::config;
using namespace rocks::test::config;

namespace {

// 시험 폴더 아래 엔진·프로젝트·사용자 폴더를 쓰는 Standard 모드 옵션이다.
struct UserTree {
    UserTree() noexcept : engine(allocator), project(allocator), user(allocator) {
    }

    bool Make() noexcept {
        if (!tree.Make()) {
            return false;
        }

        engine = tree.Path(allocator, "Engine");
        project = tree.Path(allocator, "Game");
        user = tree.Path(allocator, "User");
        return true;
    }

    ConfigContextOptions Options() const noexcept {
        ConfigContextOptions options;
        options.mode = ConfigMode::Standard;
        options.engineDir = engine.View();
        options.projectDir = project.View();
        options.projectName = "Game";
        options.userDataDir = user.View();
        return options;
    }

    SystemAllocator allocator;
    ScratchTree tree;
    String engine;
    String project;
    String user;
};

// 사용자 설정 동적 변수를 등록한다.
void RegisterVolume(ConfigContext& context) noexcept {
    DynamicVarDefinition volume;
    volume.name = "audio.master.volume";
    volume.help = "Master volume";
    volume.type = ConsoleValueType::Float;
    volume.defaultValue = "1";
    volume.flags = static_cast<uint32>(cvar::VarFlags::UserSetting);
    REQUIRE(context.RegisterDynamic(volume));
}

bool TextIs(StringView actual, StringView expected) noexcept {
    if (actual == expected) {
        return true;
    }

    printf("actual:\n%.*s\nexpected:\n%.*s\n", static_cast<int>(actual.ByteCount()), actual.Data(),
           static_cast<int>(expected.ByteCount()), expected.Data());
    return false;
}

// 진단 모음에 severity·code·path인 진단이 있는지 본다.
bool HasEntry(ConfigFixture& fixture, ConfigSeverity severity, ConfigDiagnosticCode code,
              StringView path, uint32 line = 0) noexcept {
    const ConfigDiagnostics& all = fixture.Context().Diagnostics();
    for (usize index = 0; index < all.Count(); ++index) {
        const ConfigDiagnostic item = all.Get(index);
        if (item.severity == severity && item.code == code && item.path == path &&
            (line == 0 || item.position.line == line)) {
            return true;
        }
    }

    fixture.PrintDiagnostics();
    return false;
}

// 진단 모음에서 code인 진단 수다.
usize CountEntries(ConfigFixture& fixture, ConfigDiagnosticCode code) noexcept {
    const ConfigDiagnostics& all = fixture.Context().Diagnostics();
    usize count = 0;
    for (usize index = 0; index < all.Count(); ++index) {
        count += all.Get(index).code == code ? 1 : 0;
    }

    return count;
}

} // namespace

// 저장은 사용자 층까지 합친 값과 그 아래 층 값이 다른 UserConfig 값만 쓴다(프로젝트 값과 같게 고친
// 값·세션 층 값·사용자 층이 쓸 수 없는 필드는 쓰지 않는다). 독립 콘솔 설정은 Engine branch 파일의
// `Settings`에 쓴다. 값이 없는 branch의 옛 파일은 지운다. 다시 읽으면 같은 값이고 다시 저장하면
// 같은 byte다.
TEST(Config_UserSaveRoundTripWritesDifferenceOnly) {
    UserTree paths;
    REQUIRE(paths.Make());
    REQUIRE(paths.tree.Write("Game/Config/Game.json",
                             R"({"Display": {"frameCap": 30, "textureBudget": 1024}})"));
    // 옛 사용자 파일(이번 저장에서 값이 없다 — 지워진다).
    REQUIRE(paths.tree.Write("User/Game/Config/Render.json", R"({"$version": 1})"));
    String first(paths.allocator);
    {
        ConfigFixture fixture(paths.Options());
        ConfigContext& context = fixture.Context();
        REQUIRE(context.RegisterSettings<DisplaySettings>());
        RegisterVolume(context);
        REQUIRE(LoadConfigFiles(context, platform::PhysicalFileSystem()));
        REQUIRE(SetUserValue(context, "Game.Display.textureBudget", "1024")); // 프로젝트와 같다
        REQUIRE(SetUserValue(context, "Game.Display.recentMaps", "Harbor,Mill"));
        REQUIRE(SetUserValue(context, "game.display.gamma", "1.8"));
        REQUIRE(SetUserValue(context, "audio.master.volume", "0.5"));
        // 세션 층 값은 저장하지 않는다.
        REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "game.display.gamma", "3"));
        // 사용자 층이 쓸 수 없는 필드는 대입부터 거부된다.
        CHECK(!SetUserValue(context, "Game.Display.frameCap", "10"));
        CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed));
        context.Diagnostics().Clear();
        REQUIRE(context.Sync());
        CHECK(context.Get<DisplaySettings>().gamma == 3.0f);
        CHECK(context.Get<DisplaySettings>().frameCap == 30);

        auto saved = SaveUserConfig(context, platform::PhysicalFileSystem());
        REQUIRE(saved);
        CHECK(saved.Value().written == 2 && saved.Value().removed == 1 &&
              saved.Value().values == 3);
        bool exists = true;
        static_cast<void>(
            paths.tree.Read(paths.allocator, "User/Game/Config/Render.json", &exists));
        CHECK(!exists);
        first = paths.tree.Read(paths.allocator, "User/Game/Config/Game.json");
        CHECK(TextIs(first.View(), "{\n"
                                   "  \"$version\": 1,\n"
                                   "  \"Display\": {\n"
                                   "    \"gamma\": 1.8,\n"
                                   "    \"recentMaps\": [\n"
                                   "      \"Harbor\",\n"
                                   "      \"Mill\"\n"
                                   "    ]\n"
                                   "  }\n"
                                   "}\n"));
        CHECK(TextIs(paths.tree.Read(paths.allocator, "User/Game/Config/Engine.json").View(),
                     "{\n"
                     "  \"$version\": 1,\n"
                     "  \"Settings\": {\n"
                     "    \"audio.master.volume\": 0.5\n"
                     "  }\n"
                     "}\n"));
        // 임시 파일은 남지 않는다.
        static_cast<void>(
            paths.tree.Read(paths.allocator, "User/Game/Config/Game.json.tmp", &exists));
        CHECK(!exists);
    }

    // 다음 실행: 사용자 파일을 읽으면 같은 값이고, 다시 저장하면 같은 byte다.
    ConfigFixture fixture(paths.Options());
    ConfigContext& context = fixture.Context();
    REQUIRE(context.RegisterSettings<DisplaySettings>());
    RegisterVolume(context);
    REQUIRE(LoadConfigFiles(context, platform::PhysicalFileSystem()));
    REQUIRE(context.Sync());
    CHECK(context.Diagnostics().Count() == 0);
    const DisplaySettings& display = context.Get<DisplaySettings>();
    CHECK(display.gamma == 1.8f && display.textureBudget == 1024 && display.frameCap == 30);
    CHECK(JoinedEquals(display.recentMaps, "Harbor,Mill"));
    auto volume = context.FindConsoleSetting("audio.master.volume");
    REQUIRE(volume);
    CHECK(context.ConsoleValue(volume.Value()).float32 == 0.5f);
    const ConfigLayer userTop[] = {ConfigLayer::User};
    CHECK(fixture.SourcesAre("Game", "Display", "gamma", userTop));
    REQUIRE(SaveUserConfig(context, platform::PhysicalFileSystem()));
    CHECK(TextIs(paths.tree.Read(paths.allocator, "User/Game/Config/Game.json").View(),
                 first.View()));

    // 사용자 값을 아래 층 값으로 되돌리면 그 값은 파일에서 빠진다.
    REQUIRE(SetUserValue(context, "game.display.gamma", "2.2"));
    REQUIRE(context.Sync());
    StringBuilder<512> text(paths.allocator);
    JsonWriter writer(text);
    auto written = WriteUserConfig(context, "Game", writer);
    REQUIRE(written && writer.Finish());
    CHECK(written.Value() == 1);
    CHECK(!text.View().Contains("gamma", StringComparison::CaseSensitive));
}

// 봉인 모드에는 사용자 층이 없어 저장할 곳이 없다.
TEST(Config_UserSaveNeedsStandardMode) {
    ConfigFixture fixture;
    auto saved = SaveUserConfig(fixture.Context(), platform::PhysicalFileSystem());
    CHECK(!saved && saved.Error() == ErrorCode::Unsupported);
}

// 오래된 사용자 파일은 키마다 검사해 받을 수 없는 것만 원문 위치의 Warning으로 버린다(입력은
// 실패하지 않는다). 같은 내용이 프로젝트 층이면 오류로 거부된다.
TEST(Config_OldUserFileDropsUnknownKeysWithWarnings) {
    ConfigContextOptions options;
    options.mode = ConfigMode::Standard;
    ConfigFixture fixture(options);
    ConfigContext& context = fixture.Context();
    REQUIRE(context.RegisterSettings<DisplaySettings>());
    constexpr StringView Old = "{\n"
                               "  \"$version\": 0,\n"
                               "  \"Display\": {\n"
                               "    \"gamma\": 2.0,\n"
                               "    \"removedField\": 3,\n"
                               "    \"frameCap\": 10,\n"
                               "    \"recentMaps\": 5\n"
                               "  },\n"
                               "  \"OldSection\": {\"x\": 1},\n"
                               "  \"Settings\": {\n"
                               "    \"game.input.sensitivity\": 3.5,\n"
                               "    \"audio.gone.setting\": 1,\n"
                               "    \"game.rules.title\": \"x\"\n"
                               "  }\n"
                               "}\n";
    REQUIRE(AddUserFileText(context, "Game.json", "Game", "user/Game.json", Old));
    CHECK(context.Diagnostics().ErrorCount() == 0);
    CHECK(HasEntry(fixture, ConfigSeverity::Warning, ConfigDiagnosticCode::UserFileVersion,
                   "$version", 2));
    CHECK(HasEntry(fixture, ConfigSeverity::Warning, ConfigDiagnosticCode::FieldValue,
                   "removedField", 5));
    CHECK(HasEntry(fixture, ConfigSeverity::Warning, ConfigDiagnosticCode::LayerNotAllowed,
                   "frameCap", 6));
    CHECK(HasEntry(fixture, ConfigSeverity::Warning, ConfigDiagnosticCode::FieldValue, "recentMaps",
                   7));
    CHECK(HasEntry(fixture, ConfigSeverity::Warning, ConfigDiagnosticCode::UnknownSection,
                   "OldSection", 9));
    CHECK(HasEntry(fixture, ConfigSeverity::Warning, ConfigDiagnosticCode::UnknownSetting,
                   "audio.gone.setting", 12));
    CHECK(HasEntry(fixture, ConfigSeverity::Warning, ConfigDiagnosticCode::LayerNotAllowed,
                   "game.rules.title", 13));
    REQUIRE(context.Sync());
    CHECK(context.Get<DisplaySettings>().gamma == 2.0f);
    CHECK(context.Get<DisplaySettings>().frameCap == 60);
    CHECK(fixture.Gameplay().sensitivity == 3.5f);
    CHECK(fixture.Gameplay().title.IsEmpty());

    // 깨진 사용자 파일은 통째로 버린다(Warning — 시작을 막지 않는다).
    context.Diagnostics().Clear();
    REQUIRE(AddUserFileText(context, "Game.json", "Game", "user/Game.json", "{\"Display\": "));
    CHECK(context.Diagnostics().ErrorCount() == 0);
    CHECK(HasEntry(fixture, ConfigSeverity::Warning, ConfigDiagnosticCode::JsonSyntax, {}));
    CHECK(!context.HasPendingChanges());

    // 엔진·프로젝트 파일의 모르는 키는 오류다.
    CHECK(fixture.Rejects(ConfigLayer::Project, "Game", R"({"Display": {"removedField": 3}})"));
}

// 시작 뒤 RestartRequired 값의 변경은 지금 값을 둔다(구독 콜백·snapshot은 바뀌지 않는다). 출처가
// 파일 층(사용자·프로젝트)이면 재시작 대기로 기록하고 사용자 값은 저장되어 다음 시작 때 적용된다.
// 세션 층 같은 휘발 층이면 대기 없이 Warning BootValueIgnored를 한 번만 낸다(콘솔 층 쓰기는 넣을
// 때 BootSealed로 거부한다). 되돌리면 대기가
// 없어진다. 시작 전 변경은 바로 적용된다.
TEST(Config_RestartRequiredChangeWaitsForNextStart) {
    UserTree paths;
    REQUIRE(paths.Make());
    {
        ConfigFixture fixture(paths.Options());
        ConfigContext& context = fixture.Context();
        REQUIRE(context.RegisterSettings<DisplaySettings>());
        DynamicVarDefinition port;
        port.name = "net.listen.port";
        port.help = "Listen port";
        port.type = ConsoleValueType::Int32;
        port.defaultValue = "7777";
        port.boot = true;
        REQUIRE(context.RegisterDynamic(port));
        // 시작 전에는 바로 적용된다.
        REQUIRE(context.AddAssignment(ConfigLayer::Project, "project", "Game.Display.textureBudget",
                                      "768"));
        REQUIRE(context.Sync());
        CHECK(context.Get<DisplaySettings>().textureBudget == 768);
        REQUIRE(context.CompleteStartup());

        // DisplaySettings 값이 바뀐 Sync를 센다(OnSynced의 바뀐 설정 struct 목록 — CVar 7.3절).
        int32 calls = 0;
        auto counted =
            context.OnSynced().TryAddLambda([&calls](const SyncChanges& changes) noexcept {
                for (const TypeId changed : changes.settings) {
                    calls += changed == GeneratedType<DisplaySettings>::Id() ? 1 : 0;
                }
            });
        REQUIRE(counted);
        REQUIRE(SetUserValue(context, "Game.Display.textureBudget", "2048"));
        // 봉인 뒤 부팅 독립 콘솔 설정의 콘솔 쓰기는 넣을 때 거부한다(BootSealed — CVar 9절).
        auto console =
            context.AddAssignment(ConfigLayer::Console, "console", "net.listen.port", "9000");
        CHECK(!console && console.Error() == ErrorCode::InvalidArgument);
        CHECK(HasEntry(fixture, ConfigSeverity::Error, ConfigDiagnosticCode::BootSealed,
                       "net.listen.port"));
        // 휘발 층(세션)의 변경은 받되 지금 값을 지킨다(BootValueIgnored).
        REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "net.listen.port", "9000"));
        REQUIRE(context.Sync());
        CHECK(context.Get<DisplaySettings>().textureBudget == 768);
        CHECK(calls == 0);
        auto portId = context.FindConsoleSetting("net.listen.port");
        REQUIRE(portId);
        CHECK(context.ConsoleValue(portId.Value()).signedInteger == 7777);
        Array<ConfigPendingRestart> pending(paths.allocator);
        REQUIRE(context.GetPendingRestarts(pending));
        REQUIRE(pending.Size() == 1);
        CHECK(pending[0].name == "Game.Display.textureBudget" && pending[0].pending == "2048" &&
              pending[0].current == "768");
        CHECK(HasEntry(fixture, ConfigSeverity::Note, ConfigDiagnosticCode::RestartRequired,
                       "Game.Display.textureBudget"));
        // 세션 값은 다음 실행에 다시 오지 않으므로 대기가 아니다(진단만).
        CHECK(HasEntry(fixture, ConfigSeverity::Warning, ConfigDiagnosticCode::BootValueIgnored,
                       "net.listen.port"));
        CHECK(CountEntries(fixture, ConfigDiagnosticCode::BootValueIgnored) == 1);
        // 출처는 지금 값의 출처(Project)다.
        const ConfigLayer projectTop[] = {ConfigLayer::Project};
        CHECK(fixture.SourcesAre("Game", "Display", "textureBudget", projectTop));

        // 다른 값의 변경은 그대로 바로 적용된다(대기 값은 유지된다).
        REQUIRE(SetUserValue(context, "game.display.gamma", "2.0"));
        REQUIRE(context.Sync());
        CHECK(context.Get<DisplaySettings>().gamma == 2.0f && calls == 1);
        CHECK(context.Get<DisplaySettings>().textureBudget == 768);
        REQUIRE(context.GetPendingRestarts(pending));
        CHECK(pending.Size() == 1);
        // 같은 이름·출처의 무시 진단은 다시 내지 않는다.
        CHECK(CountEntries(fixture, ConfigDiagnosticCode::BootValueIgnored) == 1);

        // 사용자 값은 저장된다(다음 시작 때 적용).
        REQUIRE(SaveUserConfig(context, platform::PhysicalFileSystem()));

        // 세션 값을 빼고 프로젝트 파일이 바꾸면(hot reload와 같은 파일 층) 그것은 대기가 된다.
        REQUIRE(context.Unset(ConfigLayer::Session, "--set"));
        REQUIRE(context.AddLayerText(ConfigLayer::Project, "Game.json", "Game", "project/Game.json",
                                     R"({"Settings": {"net.listen.port": 9100}})"));
        REQUIRE(context.Sync());
        REQUIRE(context.GetPendingRestarts(pending));
        REQUIRE(pending.Size() == 2);
        CHECK(pending[0].name == "Game.Display.textureBudget");
        CHECK(pending[1].name == "net.listen.port" && pending[1].pending == "9100" &&
              pending[1].current == "7777");

        // 되돌리면 그 대기는 없어진다.
        REQUIRE(context.Unset(ConfigLayer::Project, "Game.json"));
        REQUIRE(context.Sync());
        REQUIRE(context.GetPendingRestarts(pending));
        CHECK(pending.Size() == 1 && pending[0].name == "Game.Display.textureBudget");
    }

    // 다음 시작: 저장된 사용자 값이 시작 때 적용되고 대기는 없다.
    ConfigFixture fixture(paths.Options());
    ConfigContext& context = fixture.Context();
    REQUIRE(context.RegisterSettings<DisplaySettings>());
    REQUIRE(context.AddAssignment(ConfigLayer::Project, "project", "Game.Display.textureBudget",
                                  "768"));
    REQUIRE(LoadConfigFiles(context, platform::PhysicalFileSystem()));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    CHECK(context.Get<DisplaySettings>().textureBudget == 2048);
    Array<ConfigPendingRestart> pending(paths.allocator);
    REQUIRE(context.GetPendingRestarts(pending));
    CHECK(pending.IsEmpty());
}
} // namespace user_config_tests_file

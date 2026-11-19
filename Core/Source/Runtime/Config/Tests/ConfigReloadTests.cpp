#include "ConfigTestSupport.h"

#include <Config/ConfigLoader.h>
#include <Config/ConfigReload.h>

// 설정 CF3 hot reload 시험이다(Docs/Design/ConfigSystem.ko.md 8절): 감시 이벤트(NotifyChanged —
// 시험이 여는 gate)는 값을 바꾸지 않고 다음 sync 지점에서만 적용된다, 바뀐 파일만 그 (층, 태그)
// 입력을 바꾼다, 거부된 내용은 옛 값을 둔다, 지운 파일은 값을 뺀다, 플랫폼 폴더·사용자 파일·루트
// 전체 다시 읽기, 무시하는 경로, 출시 규칙의 거부, 실제 감시기 이벤트가 sync 지점에 닿는다.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace {

struct ReloadTree {
    ReloadTree() noexcept : engine(allocator), project(allocator), user(allocator) {
    }

    bool Make() noexcept {
        if (!tree.Make() ||
            !tree.Write("Engine/Config/Render.json", R"({"RenderGraph": {"maxPasses": 10}})") ||
            !tree.Write("Game/Config/Render.json", R"({"RenderGraph": {"maxPasses": 20}})") ||
            !tree.Write("User/Game/Config/Game.json",
                        R"({"$version": 1, "Settings": {"game.input.sensitivity": 2}})")) {
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
        options.platform = "Windows";
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

} // namespace

// 파일을 바꿔도 이벤트(gate)가 없으면 값은 그대로다. 이벤트를 넣어도 sync 지점 전에는 그대로이고
// 큐도 비어 있다(파일은 sync 지점에서 읽는다). Sync가 그 파일 입력만 바꾼다(옛 입력은 빠진다).
TEST(Config_HotReloadAppliesOnlyAtSyncPoint) {
    ReloadTree paths;
    REQUIRE(paths.Make());
    ConfigFixture fixture(DevelopmentRules(paths.Options()));
    ConfigContext& context = fixture.Context();
    REQUIRE(LoadConfigFiles(context, platform::PhysicalFileSystem()));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 20 && fixture.Gameplay().sensitivity == 2.0f);

    ConfigFileReloader reloader(context, platform::PhysicalFileSystem());
    REQUIRE(reloader.Start(false));
    auto again = reloader.Start(false);
    CHECK(!again && again.Error() == ErrorCode::AlreadyExists);

    REQUIRE(paths.tree.Write("Game/Config/Render.json", R"({"RenderGraph": {"maxPasses": 40}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 20 && reloader.LastReloadCount() == 0);

    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Render.json"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Render.json")); // 같은 표시는 하나
    CHECK(fixture.Graph().maxPasses == 20 && !context.HasPendingChanges());
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 40 && reloader.LastReloadCount() == 1);
    const ConfigLayer layers[] = {ConfigLayer::Engine, ConfigLayer::Project};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "maxPasses", layers));

    // 다음 Sync는 표시가 없으므로 아무것도 다시 읽지 않는다.
    REQUIRE(context.Sync());
    CHECK(reloader.LastReloadCount() == 0);
}

// 거부되는 내용(문법 오류·모르는 키)은 옛 값을 두고 Error 진단을 남긴다. 지운 파일은 그 값을 뺀다.
// 다시 만들면 다시 들어온다.
TEST(Config_HotReloadKeepsValuesOnRejectAndUnsetsRemovedFile) {
    ReloadTree paths;
    REQUIRE(paths.Make());
    ConfigFixture fixture(DevelopmentRules(paths.Options()));
    ConfigContext& context = fixture.Context();
    REQUIRE(LoadConfigFiles(context, platform::PhysicalFileSystem()));
    REQUIRE(context.Sync());
    ConfigFileReloader reloader(context, platform::PhysicalFileSystem());
    REQUIRE(reloader.Start(false));

    REQUIRE(paths.tree.Write("Game/Config/Render.json", "{\"RenderGraph\": {\"maxPasses\": "));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Render.json"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 20 && reloader.LastReloadCount() == 0);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::JsonSyntax));

    context.Diagnostics().Clear();
    REQUIRE(paths.tree.Write("Game/Config/Render.json", R"({"RenderGraph": {"unknown": 1}})"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Render.json"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 20);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 0, 0,
                                reflection_text::TextDiagnosticCode::UnknownProperty));

    REQUIRE(paths.tree.Remove("Game/Config/Render.json"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Render.json"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 10 && reloader.LastReloadCount() == 1);
    const ConfigLayer engineOnly[] = {ConfigLayer::Engine};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "maxPasses", engineOnly));

    REQUIRE(paths.tree.Write("Game/Config/Render.json", R"({"RenderGraph": {"maxPasses": 50}})"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Render.json"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 50);
}

// 플랫폼 폴더 파일은 플랫폼 층으로, 사용자 파일은 관대하게 읽는다. 루트 전체 표시(Overflow)는 그
// 루트의 모든 branch 파일을 다시 읽는다. 설정 파일이 아닌 경로·모르는 branch·모르는 플랫폼 폴더는
// 무시한다.
TEST(Config_HotReloadPlatformUserAndOverflow) {
    ReloadTree paths;
    REQUIRE(paths.Make());
    ConfigFixture fixture(DevelopmentRules(paths.Options()));
    ConfigContext& context = fixture.Context();
    REQUIRE(LoadConfigFiles(context, platform::PhysicalFileSystem()));
    REQUIRE(context.Sync());
    ConfigFileReloader reloader(context, platform::PhysicalFileSystem());
    REQUIRE(reloader.Start(false));

    REQUIRE(paths.tree.Write("Game/Config/Windows/Render.json",
                             R"({"RenderGraph": {"maxPasses": 60}})"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Windows/Render.json"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 60);
    const ConfigLayer platformTop[] = {ConfigLayer::Engine, ConfigLayer::Project,
                                       ConfigLayer::ProjectPlatform};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "maxPasses", platformTop));

    REQUIRE(paths.tree.Write("User/Game/Config/Game.json",
                             "{\"$version\": 1, \"Settings\": {\"game.input.sensitivity\": 4,\n"
                             "  \"old.setting.name\": 1}}"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::User, "Game.json"));
    REQUIRE(context.Sync());
    CHECK(fixture.Gameplay().sensitivity == 4.0f);
    CHECK(context.Diagnostics().ErrorCount() == 0);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::UnknownSetting));

    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Render.json.tmp"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Unknown.json"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Project, "Mac/Render.json"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::User, "Windows/Render.json"));
    REQUIRE(context.Sync());
    CHECK(reloader.LastReloadCount() == 0);

    // 루트 전체: 엔진 파일 두 개를 바꾸고 표시 하나로 둘 다 다시 읽는다(없는 파일은 Unset이므로
    // 그 루트 branch 수 × 폴더 수만큼 센다).
    REQUIRE(paths.tree.Write("Engine/Config/Render.json", R"({"RenderGraph": {"workers": 6}})"));
    REQUIRE(paths.tree.Write("Engine/Config/Game.json", R"({"Gameplay": {"lives": 9}})"));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Engine, {}));
    REQUIRE(reloader.NotifyChanged(ConfigFileRoot::Engine, "Render.json")); // 전체에 들어간다
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().workers == 6 && fixture.Gameplay().lives == 9);
    CHECK(reloader.LastReloadCount() ==
          2 * (sizeof(DefaultConfigBranches) / sizeof(DefaultConfigBranches[0])));
}

// 출시 규칙은 감시하지 않는다.
TEST(Config_HotReloadIsDevelopmentOnly) {
    ConfigContextOptions options;
    options.releaseRules = true;
    ConfigFixture fixture(options);
    ConfigFileReloader reloader(fixture.Context(), platform::PhysicalFileSystem());
    auto started = reloader.Start(false);
    CHECK(!started && started.Error() == ErrorCode::Unsupported);
}

// 실제 감시기: 파일을 고치면 감시 이벤트가 오고(이벤트를 기다린다 — 시간 경쟁이 아니라 이벤트가
// 조건이다. 기한은 멈춤을 끊는 장치일 뿐이다) 그 뒤 sync 지점에서 값이 바뀐다.
TEST(Config_HotReloadWatcherEventReachesSyncPoint) {
    ReloadTree paths;
    REQUIRE(paths.Make());
    ConfigFixture fixture(DevelopmentRules(paths.Options()));
    ConfigContext& context = fixture.Context();
    REQUIRE(LoadConfigFiles(context, platform::PhysicalFileSystem()));
    REQUIRE(context.Sync());
    ConfigFileReloader reloader(context, platform::PhysicalFileSystem());
    REQUIRE(reloader.Start(true));

    REQUIRE(paths.tree.Write("Game/Config/Render.json", R"({"RenderGraph": {"maxPasses": 70}})"));
    const uint64 deadline = platform::MonotonicNanoseconds() + 10000000000ull;
    bool applied = false;
    while (!applied && platform::MonotonicNanoseconds() < deadline) {
        if (!reloader.WaitForChanges(ConfigFileRoot::Project, 100)) {
            continue;
        }

        // 이벤트가 왔어도 sync 지점 전에는 값이 그대로다.
        CHECK(fixture.Graph().maxPasses == 20);
        REQUIRE(context.Sync());
        applied = fixture.Graph().maxPasses == 70;
    }

    CHECK(applied);
}

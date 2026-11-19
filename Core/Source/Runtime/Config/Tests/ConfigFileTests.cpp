#include "ConfigTestSupport.h"

#include <Config/ConfigDump.h>
#include <Config/ConfigLoader.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <string.h>

// 설정 CF1 파일 층·config-dump 시험이다(Docs/Design/ConfigSystem.ko.md 3.1·3.3·8절): 파일 층 순서와
// 플랫폼 부모 사슬(약한 것부터), 봉인 모드의 사용자 층 제외, 진단을 남긴 파일이 있어도 나머지를
// 읽기, config-dump golden과 Commandlet 실행.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::platform;
using namespace rocks::test::config;

namespace {

struct CapturedOutput {
    explicit CapturedOutput(Allocator& allocator) noexcept : output(allocator), error(allocator) {
    }

    static Result<void> Write(void* context, CommandletStream stream, StringView text) noexcept {
        auto& self = *static_cast<CapturedOutput*>(context);
        return (stream == CommandletStream::Output ? self.output : self.error).TryAppend(text);
    }

    String output;
    String error;
};

} // namespace

// 파일 층은 Engine → Engine 플랫폼(부모 사슬 → 플랫폼) → Project → Project 플랫폼 순서다. 다른
// 플랫폼 폴더는 읽지 않는다. 봉인 모드는 사용자 층 파일을 읽지 않고 Standard 모드는 읽는다. 진단을
// 남긴 파일이 있으면 InvalidArgument지만 다른 파일은 모두 넣는다.
TEST(Config_FileLayersFollowPlatformChain) {
    ScratchTree tree;
    REQUIRE(tree.Make());
    REQUIRE(tree.Write("Engine/Config/Render.json", R"({"RenderGraph": {"passes": ["engine"]}})"));
    REQUIRE(tree.Write("Engine/Config/Mobile/Render.json",
                       R"({"RenderGraph": {"passes": {"$append": ["mobile"]}}})"));
    REQUIRE(tree.Write("Engine/Config/Android/Render.json",
                       R"({"RenderGraph": {"passes": {"$append": ["android"]}}})"));
    REQUIRE(tree.Write("Engine/Config/Windows/Render.json",
                       R"({"RenderGraph": {"passes": {"$append": ["windows"]}}})"));
    REQUIRE(
        tree.Write("Game/Config/Render.json",
                   "// 프로젝트\n{\"RenderGraph\": {\"passes\": {\"$append\": [\"project\"]}}}"));
    REQUIRE(tree.Write("Game/Config/Android/Render.json",
                       R"({"RenderGraph": {"passes": {"$append": ["project-android"]}}})"));
    REQUIRE(tree.Write("User/Game/Config/Render.json", R"({"RenderGraph": {"maxPasses": 3}})"));

    SystemAllocator allocator;
    const String engine = tree.Path(allocator, "Engine");
    const String project = tree.Path(allocator, "Game");
    const String user = tree.Path(allocator, "User");
    const StringView parents[] = {"Mobile"};
    ConfigContextOptions options;
    options.engineDir = engine.View();
    options.projectDir = project.View();
    options.userDataDir = user.View();
    options.projectName = "Game";
    options.platform = "Android";
    options.platformParents = parents;
    {
        ConfigFixture fixture(options);
        REQUIRE(LoadConfigFiles(fixture.Context(), PhysicalFileSystem()));
        REQUIRE(fixture.Context().Sync());
        CHECK(
            JoinedEquals(fixture.Graph().passes, "engine,mobile,android,project,project-android"));
        CHECK(fixture.Graph().maxPasses == 64);
        Array<ConfigSourceRef> sources(allocator);
        REQUIRE(fixture.Context().GetSources("Render", "RenderGraph", "passes", sources));
        REQUIRE(sources.Size() == 5);
        CHECK(sources[0].layer == ConfigLayer::Engine && sources[0].tag == "Render.json");
        CHECK(sources[1].layer == ConfigLayer::EnginePlatform &&
              sources[1].tag == "Mobile/Render.json");
        CHECK(sources[2].layer == ConfigLayer::EnginePlatform &&
              sources[2].tag == "Android/Render.json");
        CHECK(sources[3].layer == ConfigLayer::Project && sources[3].tag == "Render.json");
        CHECK(sources[4].layer == ConfigLayer::ProjectPlatform &&
              sources[4].tag == "Android/Render.json");
        const String path = tree.Path(allocator, "Game/Config/Android/Render.json");
        CHECK(sources[4].sourceName == path.View());
    }

    options.mode = ConfigMode::Standard;
    {
        ConfigFixture fixture(options);
        REQUIRE(LoadConfigFiles(fixture.Context(), PhysicalFileSystem()));
        REQUIRE(fixture.Context().Sync());
        CHECK(fixture.Graph().maxPasses == 3);
    }

    // 모르는 키가 있는 파일은 거부하고(진단 출처 = 그 파일 경로) 나머지 파일은 넣는다.
    REQUIRE(tree.Write("Game/Config/Game.json", "{\n  \"Gameplay\": {\"livez\": 1}\n}"));
    options.mode = ConfigMode::Sealed;
    {
        ConfigFixture fixture(options);
        auto loaded = LoadConfigFiles(fixture.Context(), PhysicalFileSystem());
        CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
        CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 2, 25,
                                    reflection_text::TextDiagnosticCode::UnknownProperty));
        const String bad = tree.Path(allocator, "Game/Config/Game.json");
        CHECK(fixture.Context().Diagnostics().Get(0).source == bad.View());
        REQUIRE(fixture.Context().Sync());
        CHECK(
            JoinedEquals(fixture.Graph().passes, "engine,mobile,android,project,project-android"));
    }
}

namespace {

// config-dump golden이다(입력은 Config_DumpIsDeterministicGolden). 출력 규칙이 바뀌면 의도한
// 변경인지 확인한 뒤 갱신한다.
constexpr const char* DumpGolden = R"({
  "sections": [
    {
      "branch": "Game",
      "section": "Gameplay",
      "values": {
        "scoreLimit": 100,
        "title": "",
        "seed": 7,
        "lives": 5,
        "map": "None",
        "sensitivity": 1.0
      },
      "sources": [
        {
          "path": "lives",
          "stack": [
            {
              "layer": "Session",
              "tag": "cli",
              "source": "Game.Gameplay.lives=5"
            }
          ]
        }
      ]
    },
    {
      "branch": "Render",
      "section": "RenderGraph",
      "values": {
        "debugDraw": false,
        "lodBias": 1.0,
        "scales": [],
        "byName": {},
        "passes": [
          "a",
          "b",
          "c"
        ],
        "layers": [],
        "origin": {
          "y": 0,
          "x": 1
        },
        "maxPasses": 20,
        "points": [],
        "qualities": [],
        "workers": 2,
        "quality": "Medium",
        "tags": [],
        "immediate": false,
        "cacheDir": ""
      },
      "sources": [
        {
          "path": "maxPasses",
          "stack": [
            {
              "layer": "Engine",
              "tag": "Render.json",
              "source": "Engine/Config/Render.json"
            },
            {
              "layer": "Project",
              "tag": "Render.json",
              "source": "Game/Config/Render.json"
            }
          ]
        },
        {
          "path": "origin.x",
          "stack": [
            {
              "layer": "Engine",
              "tag": "Render.json",
              "source": "Engine/Config/Render.json"
            }
          ]
        },
        {
          "path": "passes",
          "stack": [
            {
              "layer": "Engine",
              "tag": "Render.json",
              "source": "Engine/Config/Render.json"
            },
            {
              "layer": "Project",
              "tag": "Render.json",
              "source": "Game/Config/Render.json"
            }
          ]
        }
      ]
    }
  ],
  "settings": [
    {
      "name": "cheat.enabled",
      "value": "true",
      "stack": []
    }
  ]
}
)";

void BuildDumpScenario(ConfigFixture& fixture) noexcept {
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render",
                                 "Engine/Config/Render.json",
                                 R"({"RenderGraph": {"maxPasses": 10, "passes": ["a", "b"],
                                     "origin": {"x": 1}}})"));
    REQUIRE(context.AddLayerText(
        ConfigLayer::Project, "Render.json", "Render", "Game/Config/Render.json",
        R"({"RenderGraph": {"maxPasses": 20, "passes": {"$add": ["c"]}}})"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "Game.Gameplay.lives", "5"));
    REQUIRE(context.Sync());
}

} // namespace

// config-dump는 섹션·출처를 이름 순서로 결정적 JSON에 쓴다(golden과 byte 단위로 같다).
// Commandlet으로 실행해도 같은 출력이고, 인자가 있으면 사용법 오류 2다.
TEST(Config_DumpIsDeterministicGolden) {
    ConfigFixture fixture;
    BuildDumpScenario(fixture);
    StringBuilder<4096> text(fixture.system);
    JsonWriter writer(text);
    REQUIRE(WriteConfigDump(fixture.Context(), writer));
    REQUIRE(writer.Finish());
    if (text.View() != DumpGolden) {
        printf("config-dump output:\n%.*s", static_cast<int>(text.View().ByteCount()),
               text.View().Data());
    }

    CHECK(text.View() == DumpGolden);

    CommandletRegistry registry;
    REQUIRE(RegisterConfigDumpCommandlet(registry, fixture.Context()));
    CapturedOutput captured(fixture.system);
    CommandletOutput output{&CapturedOutput::Write, &captured};
    const StringView dump[] = {"config-dump"};
    auto ran = registry.Run(dump, output);
    REQUIRE(ran);
    CHECK(ran.Value() == 0 && captured.output.View() == DumpGolden);
    const StringView extra[] = {"config-dump", "--all"};
    auto usage = registry.Run(extra, output);
    REQUIRE(usage);
    CHECK(usage.Value() == 2 && !captured.error.IsEmpty());
}

// 명령줄 `--set=<target>=<value>`는 나온 순서대로 세션 층 대입이 된다(값은 첫 `=` 뒤 전부). `=`가
// 없는 인자는 거부하되 나머지는 끝까지 넣는다.
TEST(Config_CommandLineAssignments) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    const StringView arguments[] = {"--frames=3", "--set=render.graph.immediate=true",
                                    "--set=Render.RenderGraph.passes=a,b", "--set=missing",
                                    "--set=Render.RenderGraph.maxPasses=7"};
    auto added = AddCommandLineAssignments(context, Span<const StringView>(arguments, 5));
    CHECK(!added && added.Error() == ErrorCode::InvalidArgument);
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().immediate);
    CHECK(fixture.Graph().maxPasses == 7);
    CHECK(JoinedEquals(fixture.Graph().passes, "a,b"));
    const ConfigLayer session[] = {ConfigLayer::Session};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "maxPasses", session));

    const StringView valid[] = {"--set=Render.RenderGraph.maxPasses=9"};
    auto again = AddCommandLineAssignments(context, Span<const StringView>(valid, 1));
    REQUIRE(again);
    CHECK(again.Value() == 1);
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 9);
}

#include <Config/ConfigLoader.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <LogHost/LogHost.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <TestSupport/Test.h>
#include <TraceControl/TraceController.h>
#include <TraceControl/TraceSettings.generated.h>

// 호스트 설정 부트 시험이다(Docs/Design/ConfigSystem.ko.md 5.3절): 등록 → 엔진 층 파일 → 추가 파일
// → --set → CompleteStartup 순서와, 시작 뒤 고정 값(log.mode)의 거부.
using namespace rocks;

namespace {

// 작업 디렉터리 아래 시험 전용 엔진 폴더(<root>/Engine/Config)다. 소멸 때 지운다.
struct EngineTree {
    EngineTree() noexcept {
        FixedStringBuilder text{Span<char>(root)};
        ready = text.TryAppendFormat("rocks-hostconfig-{}-{}", platform::CurrentProcessId(),
                                     platform::MonotonicTicks())
                    .IsOk();
        rootBytes = text.ByteCount();
        FixedStringBuilder engine{Span<char>(engineDir)};
        ready = ready && engine.TryAppendFormat("{}/Engine", StringView(root, rootBytes));
        engineBytes = engine.ByteCount();
        char config[256];
        FixedStringBuilder configText{Span<char>(config)};
        ready = ready &&
                configText.TryAppendFormat("{}/Config", StringView(engineDir, engineBytes)) &&
                platform::MakeDirectoryTree(platform::PhysicalFileSystem(), config,
                                            configText.ByteCount());
    }

    ~EngineTree() noexcept {
        (void)platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root, rootBytes);
    }

    EngineTree(const EngineTree&) = delete;
    EngineTree& operator=(const EngineTree&) = delete;

    // <root>/<relative>에 text를 쓴다. path에 NUL로 끝난 경로를 남긴다.
    bool Write(StringView relative, StringView text, char (&path)[256]) const noexcept {
        FixedStringBuilder pathText{Span<char>(path)};
        if (!pathText.TryAppendFormat("{}/{}", StringView(root, rootBytes), relative)) {
            return false;
        }

        platform::File file;
        return file.Open(path, platform::FileOpenMode::CreateAlways) &&
               file.WriteAll(text.Data(), text.ByteCount()).status && file.Close();
    }

    StringView EngineDir() const noexcept {
        return StringView(engineDir, engineBytes);
    }

    char root[128]{};
    usize rootBytes = 0;
    char engineDir[160]{};
    usize engineBytes = 0;
    bool ready = false;
};

} // namespace

// 엔진 층 파일(TracePresets·log.level) → 추가 파일(Project 층) → --set(Session 층) 순으로 합치고,
// CompleteStartup 뒤에는 log.mode(시작 뒤 고정) 변경을 거부한다.
TEST(HostSettings_BootOrderMergesLayers) {
    EngineTree tree;
    REQUIRE(tree.ready);
    char enginePath[256];
    REQUIRE(tree.Write("Engine/Config/Engine.json",
                       R"({ "TracePresets": { "default": ["cpu", "frame"] },
                            "Settings": { "log.level": "debug", "log.levels": "Engine=info" } })",
                       enginePath));
    char extraPath[256];
    REQUIRE(tree.Write("Extra.json",
                       R"({ "TracePresets": { "default": { "$append": ["stats"] } },
                            "Settings": { "log.level": "error" } })",
                       extraPath));

    SystemAllocator allocator;
    HostSettings settings(allocator);
    HostSettingsOptions options;
    options.engineDir = tree.EngineDir();
    REQUIRE(settings.Initialize(options));
    REQUIRE(settings.LoadFiles());
    REQUIRE(settings.AddFile(config::ConfigLayer::Project, "Engine", extraPath));
    const StringView arguments[] = {"--frames=2", "--set=log.level=warning", "--trace=cpu"};
    REQUIRE(settings.ApplyCommandLine(Span<const StringView>(arguments, 3)));
    REQUIRE(settings.CompleteStartup());

    config::ConfigContext& context = settings.Context();
    auto level = context.FindConsoleSetting(LogLevelSetting);
    REQUIRE(level);
    CHECK(context.ConsoleValue(level.Value()).string.View() == StringView("warning"));
    auto levels = context.FindConsoleSetting(LogLevelsSetting);
    REQUIRE(levels);
    CHECK(context.ConsoleValue(levels.Value()).string.View() == StringView("Engine=info"));
    const auto& presets = context.Get<trace_control::TracePresetSettings>();
    REQUIRE(presets.defaultChannels.Size() == 3);
    CHECK(presets.defaultChannels[2].View() == StringView("stats"));

    // log.mode는 시작 뒤 고정이다: 그 뒤의 쓰기는 값을 바꾸지 않는다.
    auto mode = context.FindConsoleSetting(LogModeSetting);
    REQUIRE(mode);
    const auto late =
        context.AddAssignment(config::ConfigLayer::Console, "late", LogModeSetting, "sync");
    (void)late;
    REQUIRE(context.Sync());
    CHECK(context.ConsoleValue(mode.Value()).string.View() == StringView("default"));
}

// 등록되지 않은 섹션이 있는 엔진 층 파일은 거부되고(진단을 기록한다), 다른 단계는 영향이 없다.
TEST(HostSettings_RejectedEngineFileReportsFailure) {
    EngineTree tree;
    REQUIRE(tree.ready);
    char enginePath[256];
    REQUIRE(
        tree.Write("Engine/Config/Engine.json", R"({ "NoSuchSection": { "x": 1 } })", enginePath));
    SystemAllocator allocator;
    HostSettings settings(allocator);
    HostSettingsOptions options;
    options.engineDir = tree.EngineDir();
    REQUIRE(settings.Initialize(options));
    CHECK(settings.LoadFiles().Error() == ErrorCode::InvalidArgument);
    REQUIRE(settings.CompleteStartup());
}

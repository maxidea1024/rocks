#include <Config/CVar.h>
#include <Config/ConfigLoader.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <LogHost/LogHost.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>
#include <TraceControl/TraceController.h>
#include <TraceControl/TraceSettings.generated.h>

// 호스트 설정 부트 시험이다(Docs/Design/ConfigSystem.ko.md 5.3절): 등록 → 엔진 층 파일 → 추가 파일
// → --set → CompleteStartup 순서와, 시작 뒤 고정 값(log.mode)의 거부, 시작 완료의 기기 프로필 적용,
// 품질 조합 검증 규칙(HostSettingsOptions::qualityRules)과 ApplyDeviceTraits, 프로젝트 폴더·사용자
// 데이터 폴더 찾기.
using namespace rocks;

namespace host_settings_test {

constinit CVar<int32> HostShadowDistance =
    cvar::Var("render.host.shadow_distance", 1).Help("Shadow distance");

constinit CVarGroup HostShadow = cvar::Group("quality.host.shadow", 1)
                                     .Help("Shadow scalability")
                                     .Level(0, {{HostShadowDistance, 10}})
                                     .Level(1, {{HostShadowDistance, 20}});

Result<void> RegisterHostVars(config::ConfigContext& config) noexcept {
    return config.Register(HostShadowDistance, HostShadow);
}

} // namespace host_settings_test

namespace host_settings_tests_file {

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
    // 게임 lane은 문맥의 최신 snapshot에 묶여 있다(핸들로 읽는다 — CVar 7.1절).
    CHECK(LogLevelVar.Get() == LogLevel::Warning);
    CHECK(LogLevelsVar.Get() == StringView("Engine=info"));
    const auto& presets = context.Get<trace_control::TracePresetSettings>();
    REQUIRE(presets.defaultChannels.Size() == 3);
    CHECK(presets.defaultChannels[2].View() == StringView("stats"));

    // log.mode는 시작 뒤 고정이다(BootCVar): 그 뒤의 콘솔 쓰기는 거부되고 값은 그대로다.
    CHECK(!context.AddAssignment(config::ConfigLayer::Console, "late", LogModeVar.Name(), "sync"));
    REQUIRE(context.Sync());
    CHECK(LogModeVar.Get() == LogHostMode::Default);
}

namespace {

// 묶음 없는 스레드에서 CVar를 쓴다(프로세스 기본 문맥으로 간다 — CVar 6.2절).
struct UnboundWrite {
    ErrorCode result = ErrorCode::None;
};

void WriteLevelUnbound(void* raw) noexcept {
    auto& write = *static_cast<UnboundWrite*>(raw);
    auto set = LogLevelVar.Set(LogLevel::Debug);
    write.result = set ? ErrorCode::None : set.Error();
}

ErrorCode WriteFromAnotherThread() noexcept {
    UnboundWrite write;
    write.result = ErrorCode::Cancelled;
    platform::Thread thread;
    const bool started =
        thread.Start(&WriteLevelUnbound, &write) == platform::ThreadStatus::Success;
    CHECK(started && thread.Join() == platform::ThreadStatus::Success);
    return write.result;
}

} // namespace

// HostSettings는 문맥을 프로세스 기본 문맥으로 정한다: 묶음 없는 스레드의 Set이 그 문맥의 쓰기 큐에
// 들고 다음 Sync에 적용된다. 호스트가 둘이면 두 번째 Initialize는 AlreadyExists이고, 호스트가
// 없어지면 묶음 없는 Set은 NotFound다(Join이 순서를 정한다).
TEST(HostSettings_ProcessDefaultContext) {
    EngineTree tree;
    REQUIRE(tree.ready);
    SystemAllocator allocator;
    {
        HostSettings settings(allocator);
        HostSettingsOptions options;
        options.engineDir = tree.EngineDir();
        REQUIRE(settings.Initialize(options));
        REQUIRE(settings.CompleteStartup());
        CHECK(WriteFromAnotherThread() == ErrorCode::None);
        CHECK(LogLevelVar.Get() == LogLevel::Info);
        REQUIRE(settings.Context().Sync());
        CHECK(LogLevelVar.Get() == LogLevel::Debug);

        HostSettings second(allocator);
        CHECK(second.Initialize(options).Error() == ErrorCode::AlreadyExists);
    }

    CHECK(WriteFromAnotherThread() == ErrorCode::NotFound);
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

// 시작 완료(5단계)가 기기 프로필을 고르고(플랫폼 기본 → 명령줄 --device-profile) 그 값을 적용한다.
// 플랫폼 특성(플랫폼 이름·시스템 메모리)은 Initialize가 채운다.
TEST(HostSettings_DeviceProfileAppliedAtStartup) {
    EngineTree tree;
    REQUIRE(tree.ready);
    char profilesPath[256];
    REQUIRE(tree.Write("Engine/Config/DeviceProfiles.json",
                       R"({ "DeviceProfiles": {
                              "platformDefaults": { "TestPlatform": "Bench" },
                              "profiles": {
                                "Bench": { "values": { "log.level": "error" } },
                                "Quiet": { "parent": "Bench", "values": { "log.levels": "Engine=warning" } } } } })",
                       profilesPath));
    for (int run = 0; run < 2; ++run) {
        SystemAllocator allocator;
        HostSettings settings(allocator);
        HostSettingsOptions options;
        options.engineDir = tree.EngineDir();
        options.platform = "TestPlatform";
        REQUIRE(settings.Initialize(options));
        config::DeviceTrait platform;
        REQUIRE(settings.DeviceTraits().Find(config::DeviceTraitNames::Platform, platform));
        CHECK(platform.text == StringView("TestPlatform"));
        config::DeviceTrait memory;
        REQUIRE(settings.DeviceTraits().Find(config::DeviceTraitNames::SystemMemoryMiB, memory));
        CHECK(memory.numeric && memory.number > 0);
        REQUIRE(settings.LoadFiles());
        const StringView arguments[] = {"--device-profile=Quiet"};
        REQUIRE(settings.ApplyCommandLine(Span<const StringView>(arguments, run == 0 ? 0 : 1)));
        REQUIRE(settings.CompleteStartup());
        CHECK(LogLevelVar.Get() == LogLevel::Error);
        if (run == 0) {
            CHECK(settings.DeviceProfile().profile.View() == StringView("Bench"));
            CHECK(settings.DeviceProfile().source == config::DeviceProfileSource::PlatformDefault);
        } else {
            CHECK(settings.DeviceProfile().profile.View() == StringView("Quiet"));
            CHECK(settings.DeviceProfile().source == config::DeviceProfileSource::CommandLine);
            CHECK(LogLevelsVar.Get() == StringView("Engine=warning"));
        }
    }
}

// 프로젝트 폴더는 시작 폴더부터 위로 올라가며 `*.rocksproject`를 가진 첫 폴더다(여럿이면 이름
// 순서로 첫 기술자). 사용자 층 뿌리는 OS 사용자 데이터 폴더다.
// 호스트가 넘긴 품질 규칙은 기기 특성으로 펼칠 등급을 낮추고(묶음 변수는 그대로), 특성을 바꿔
// ApplyDeviceTraits를 부르면 다음 Sync에 원래 선택이 펼쳐진다(프로필 정의가 없어도).
TEST(HostSettings_QualityRulesFollowDeviceTraits) {
    using namespace host_settings_test;
    EngineTree tree;
    REQUIRE(tree.ready);
    SystemAllocator allocator;
    config::DeviceCondition bigMemory(allocator);
    REQUIRE(bigMemory.trait.TryAssign(config::DeviceTraitNames::GpuMemoryMiB));
    bigMemory.atLeast = 8192.0;
    const config::QualityLevelRule shadowRules[] = {
        {1, Span<const config::DeviceCondition>(&bigMemory, 1), {}}};
    const config::QualityGroupRule rules[] = {{"quality.host.shadow", 0, shadowRules}};
    HostSettings settings(allocator);
    HostSettingsOptions options;
    options.engineDir = tree.EngineDir();
    options.registerSettings = &RegisterHostVars;
    options.qualityRules = rules;
    REQUIRE(settings.Initialize(options));
    REQUIRE(settings.DeviceTraits().SetNumber(config::DeviceTraitNames::GpuMemoryMiB, 4096));
    REQUIRE(settings.LoadFiles());
    REQUIRE(settings.CompleteStartup());
    CHECK(HostShadow.Get() == 1 && HostShadowDistance.Get() == 10);

    REQUIRE(settings.DeviceTraits().SetNumber(config::DeviceTraitNames::GpuMemoryMiB, 16384));
    REQUIRE(settings.ApplyDeviceTraits());
    REQUIRE(settings.Context().Sync());
    CHECK(HostShadow.Get() == 1 && HostShadowDistance.Get() == 20);
}

TEST(HostSettings_ProjectDirectoryAndUserData) {
    EngineTree tree;
    REQUIRE(tree.ready);
    char nested[256];
    FixedStringBuilder nestedText{Span<char>(nested)};
    REQUIRE(
        nestedText.TryAppendFormat("{}/Game/out/Debug/bin", StringView(tree.root, tree.rootBytes)));
    REQUIRE(platform::MakeDirectoryTree(platform::PhysicalFileSystem(), nested,
                                        nestedText.ByteCount()));
    char path[256];
    REQUIRE(tree.Write("Game/Zeta.rocksproject", "{}", path));
    REQUIRE(tree.Write("Game/Alpha.rocksproject", "{}", path));
    SystemAllocator allocator;
    String directory(allocator);
    String name(allocator);
    REQUIRE(FindProjectFrom(nestedText.View(), directory, name));
    CHECK(directory.View().EndsWith("/Game", StringComparison::CaseSensitive));
    CHECK(name.View() == StringView("Alpha"));

    String user(allocator);
    REQUIRE(UserDataDirectory(user));
    CHECK(!user.IsEmpty() && !user.View().Contains("\\", StringComparison::CaseSensitive));
    CHECK(!HostPlatformName().IsEmpty());
}
} // namespace host_settings_tests_file

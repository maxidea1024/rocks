#include <TestSupport/Test.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/StringBuilder.h>
#include <Crypto/Bytes.h>
#include <LogHost/LogHost.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>

using namespace rocks;

namespace {

constexpr LogCategory LogHostTests{"Tests.Host"};

// 타입 등록·Name pool·객체 Registry와 설정 문맥이다(log.* 설정을 등록해 둔다). 소멸 순서: 문맥 →
// Registry → pool → 타입.
struct LogSettingsFixture {
    LogSettingsFixture() noexcept
        : types(system), names(system),
          objects(system, types, {0x4c4f4748, 1}, MakeOptions(names)) {
        REQUIRE(names.Initialize());
        auto made = UniquePtr<config::ConfigContext>::TryMake(system, system, objects,
                                                              config::ConfigContextOptions{});
        REQUIRE(made);
        context = Move(made.Value());
        REQUIRE(context->Initialize());
        REQUIRE(RegisterLogSettings(*context));
        REQUIRE(context->Sync());
    }

    ~LogSettingsFixture() noexcept {
        context = nullptr;
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(names.Shutdown());
        CHECK(types.Shutdown());
    }

    static RegistryOptions MakeOptions(NamePool& pool) noexcept {
        RegistryOptions options;
        options.names = &pool;
        return options;
    }

    // Engine branch의 Settings 섹션으로 값을 넣고 Sync한다.
    void Apply(StringView settingsJson) noexcept {
        char storage[4096];
        FixedStringBuilder text{Span<char>(storage)};
        (void)text.TryAppendFormat("{{\"Settings\": {}}}", settingsJson);
        REQUIRE(context->AddLayerText(config::ConfigLayer::Project, "Log", "Engine", "<memory>",
                                      text.View()));
        REQUIRE(context->Sync());
    }

    SystemAllocator system;
    TypeRegistry types;
    NamePool names;
    ObjectRegistry objects;
    UniquePtr<config::ConfigContext> context;
};

// 작업 디렉터리 아래 시험 전용 폴더다(소멸 때 지운다).
struct ScratchDirectory {
    ScratchDirectory() noexcept {
        static uint32 counter = 0;
        FixedStringBuilder text{Span<char>(root)};
        (void)text.TryAppendFormat(
            "rocks-loghost-{}-{}", platform::MonotonicNanoseconds(),
            platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&counter, 1u));
        length = text.ByteCount();
    }

    ~ScratchDirectory() noexcept {
        (void)platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root, length);
    }

    StringView View() const noexcept {
        return StringView(root, length);
    }

    char root[160]{};
    usize length = 0;
};

bool CountFile(const platform::DirectoryEntry& entry, void* raw) noexcept {
    if (entry.info.type == platform::FileType::File) {
        ++*static_cast<usize*>(raw);
    }

    return true;
}

usize CountFiles(const ScratchDirectory& directory) noexcept {
    usize count = 0;
    (void)platform::PhysicalFileSystem().EnumerateDirectory(directory.root, directory.length,
                                                            &CountFile, &count);
    return count;
}

bool ReadAll(StringView path, Array<uint8>& bytes) noexcept {
    char native[MaxLogFilePathBytes];
    FixedStringBuilder text{Span<char>(native)};
    (void)text.TryAppend(path);
    platform::File file;
    uint64 size = 0;
    if (!file.Open(native, platform::FileOpenMode::ReadExisting) || !file.Size(size) ||
        !bytes.TryResize(static_cast<usize>(size))) {
        return false;
    }

    return static_cast<bool>(file.ReadExact(bytes.Data(), bytes.Size()).status);
}

Result<void> Collect(void* raw, Span<const uint8> plaintext) noexcept {
    return static_cast<Array<uint8>*>(raw)->TryAppend(plaintext);
}

bool Contains(const Array<uint8>& bytes, StringView needle) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size())
        .Contains(needle, StringComparison::CaseSensitive);
}

} // namespace

// 설정 값이 모드·전역 최소·카테고리 재정의를 바꾼다. 재정의 접두사와 레벨 이름은 대소문자를 가리지
// 않는다.
TEST(LogHostAppliesModeAndLevelsFromConfig) {
    LogSettingsFixture fixture;
    fixture.Apply(R"({"log.mode": "sync", "log.level": "warning",
                      "log.levels": "GRAPHICS=Error, engine.world = debug"})");
    LogHost host(fixture.system);
    REQUIRE(host.Start(LogSystemConfig::ForEngineHost(), fixture.context.Get(), "Test"));
    LogSystem& logs = host.System();
    constexpr LogCategory audio{"Audio"};
    constexpr LogCategory d3d12{"Graphics.D3D12"};
    constexpr LogCategory tick{"Engine.World.Tick"};
    CHECK(logs.Mode() == LogMode::Sync);
    CHECK(!logs.IsEnabled(LogLevel::Info, audio) && logs.IsEnabled(LogLevel::Warning, audio));
    CHECK(!logs.IsEnabled(LogLevel::Warning, d3d12) && logs.IsEnabled(LogLevel::Error, d3d12));
    CHECK(logs.IsEnabled(LogLevel::Debug, tick));
    CHECK(!host.IsFileLogging() && host.FileSetupError() == ErrorCode::None);
    host.Stop();
}

// 설정이 없으면 호스트 프리셋 그대로다(log.mode 기본값 default).
TEST(LogHostKeepsThePresetByDefault) {
    LogSettingsFixture fixture;
    LogHost host(fixture.system);
    REQUIRE(host.Start(LogSystemConfig::ForTool(), fixture.context.Get(), "Test"));
    CHECK(host.System().Mode() == LogMode::Sync);
    host.Stop();
    REQUIRE(host.Start(LogSystemConfig::ForEngineHost(), fixture.context.Get(), "Test"));
    CHECK(host.System().Mode() == LogMode::Async);
    host.Stop();
}

// 실행 중 log.level·log.levels 변경은 sync 지점에서 적용되고, 시작 뒤 고정 설정의 변경은 거부된다.
TEST(LogHostAppliesRuntimeLevelChangesAndRejectsStartupOnlyChanges) {
    LogSettingsFixture fixture;
    fixture.Apply(R"({"log.levels": "Audio=error"})");
    LogHost host(fixture.system);
    REQUIRE(host.Start(LogSystemConfig::ForEngineHost(), fixture.context.Get(), "Test"));
    REQUIRE(host.Watch(*fixture.context));
    REQUIRE(fixture.context->CompleteStartup());
    LogSystem& logs = host.System();
    constexpr LogCategory audio{"Audio"};
    constexpr LogCategory input{"Input"};
    CHECK(!logs.IsEnabled(LogLevel::Warning, audio) && logs.IsEnabled(LogLevel::Info, input));

    config::ConfigContext& context = *fixture.context;
    REQUIRE(
        context.AddAssignment(config::ConfigLayer::Console, "console", LogLevelSetting, "error"));
    REQUIRE(context.AddAssignment(config::ConfigLayer::Console, "console", LogLevelsSetting,
                                  "input=trace"));
    // sync 지점 전에는 그대로다.
    CHECK(logs.IsEnabled(LogLevel::Info, input));
    REQUIRE(context.Sync());
    CHECK(!logs.IsEnabled(LogLevel::Warning, LogCategory{"Physics"}));
    CHECK(logs.IsEnabled(LogLevel::Trace, input));
    // 앞의 재정의(Audio=error)는 지워져 전역 최소(error)를 따른다.
    CHECK(logs.IsEnabled(LogLevel::Error, audio) && !logs.IsEnabled(LogLevel::Warning, audio));

    context.Diagnostics().Clear();
    CHECK(!context.AddAssignment(config::ConfigLayer::Console, "console", LogModeSetting, "sync"));
    CHECK(!context.AddAssignment(config::ConfigLayer::Console, "console", LogFileEnabledSetting,
                                 "true"));
    CHECK(logs.Mode() == LogMode::Async);
    host.Stop();
}

// 암호화를 켰는데 공개키가 없으면(또는 틀리면) 파일을 쓰지 않는다 — 평문으로 내려가지 않는다.
TEST(LogHostWritesNoFileWhenEncryptionKeyIsMissing) {
    const StringView keys[] = {"", "bm90LWEta2V5"};
    for (StringView key : keys) {
        ScratchDirectory directory;
        LogSettingsFixture fixture;
        char settings[512];
        FixedStringBuilder text{Span<char>(settings)};
        (void)text.TryAppendFormat(
            R"({{"log.mode": "sync", "log.file.enabled": true, "log.file.directory": "{}",
                 "log.file.encrypt": true, "log.file.recipient_key": "{}"}})",
            directory.View(), key);
        fixture.Apply(text.View());
        LogHost host(fixture.system);
        REQUIRE(host.Start(LogSystemConfig::ForEngineHost(), fixture.context.Get(), "Test"));
        CHECK(!host.IsFileLogging());
        CHECK(host.FileSetupError() != ErrorCode::None);
        {
            LogInstallScope installed(host.System());
            Log::Info(LogHostTests, "secret value {Value}", "do-not-leak");
        }

        host.Stop();
        CHECK(CountFiles(directory) == 0);
    }
}

// 암호화를 켜고 공개키를 주면 봉인한 파일이 생기고 개인키로 열린다.
TEST(LogHostWritesASealedFileThatDecodes) {
    ScratchDirectory directory;
    auto pair = crypto::KemKeyPair::TryGenerate(crypto::KemAlgorithm::XWing);
    REQUIRE(pair);
    char base64[2048];
    auto encoded = crypto::TryToBase64(pair.Value().PublicKey().View(),
                                       crypto::Base64Variant::Standard, Span<char>(base64));
    REQUIRE(encoded);
    LogSettingsFixture fixture;
    char settings[4096];
    FixedStringBuilder text{Span<char>(settings)};
    (void)text.TryAppendFormat(
        R"({{"log.mode": "sync", "log.file.enabled": true, "log.file.directory": "{}",
             "log.file.format": "jsonlines", "log.file.encrypt": true,
             "log.file.recipient_key": "{}"}})",
        directory.View(), StringView(base64, encoded.Value()));
    fixture.Apply(text.View());
    Array<char> path(fixture.system);
    {
        LogHost host(fixture.system);
        REQUIRE(host.Start(LogSystemConfig::ForEngineHost(), fixture.context.Get(), "Test"));
        REQUIRE(host.IsFileLogging());
        REQUIRE(
            path.TryAppend(Span<const char>(host.FilePath().Data(), host.FilePath().ByteCount())));
        {
            LogInstallScope installed(host.System());
            Log::Info(LogHostTests, "sealed value {Value}", "visible-after-decode");
        }

        host.Stop();
    }

    Array<uint8> file(fixture.system);
    REQUIRE(ReadAll(StringView(path.Data(), path.Size()), file));
    CHECK(!Contains(file, "visible-after-decode"));
    Array<uint8> plain(fixture.system);
    auto summary =
        crypto::TryDecodeLogFile(pair.Value(), file.View(), fixture.system, &Collect, &plain);
    REQUIRE(summary);
    CHECK(summary.Value().endMarker);
    CHECK(Contains(plain, "\"msg\":\"sealed value visible-after-decode\""));
}

// 평문 파일 기록도 설정으로 켠다(형식 text, 폴더는 없으면 만든다).
TEST(LogHostWritesAPlainFileFromConfig) {
    ScratchDirectory directory;
    LogSettingsFixture fixture;
    char settings[512];
    FixedStringBuilder text{Span<char>(settings)};
    (void)text.TryAppendFormat(
        R"({{"log.mode": "sync", "log.file.enabled": true, "log.file.directory": "{}"}})",
        directory.View());
    fixture.Apply(text.View());
    Array<char> path(fixture.system);
    {
        LogHost host(fixture.system);
        REQUIRE(host.Start(LogSystemConfig::ForEngineHost(), fixture.context.Get(), "Test"));
        REQUIRE(host.IsFileLogging());
        REQUIRE(
            path.TryAppend(Span<const char>(host.FilePath().Data(), host.FilePath().ByteCount())));
        {
            LogInstallScope installed(host.System());
            Log::Warning(LogHostTests, "plain value {Value}", 7);
        }

        host.Stop();
    }

    Array<uint8> file(fixture.system);
    REQUIRE(ReadAll(StringView(path.Data(), path.Size()), file));
    CHECK(Contains(file, "[Warning][Tests.Host] plain value 7"));
}

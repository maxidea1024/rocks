#include <TestSupport/Test.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/StringBuilder.h>
#include <Compression/Compression.h>
#include <Crypto/Bytes.h>
#include <LogHost/LogHost.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Sync.h>

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

// log.level 이름은 대소문자를 가리지 않는다("Warning"). 모르는 이름은 설정 단계에서 거부되고 앞
// 값이 남는다.
TEST(LogHostAcceptsLevelNamesInAnyCaseAndRejectsUnknownOnes) {
    LogSettingsFixture fixture;
    fixture.Apply(R"({"log.level": "Warning"})");
    config::ConfigContext& context = *fixture.context;
    context.Diagnostics().Clear();
    CHECK(!context.AddAssignment(config::ConfigLayer::Console, "console", LogLevelSetting, "loud"));
    CHECK(context.Diagnostics().ErrorCount() != 0);
    REQUIRE(context.Sync());
    LogHost host(fixture.system);
    REQUIRE(host.Start(LogSystemConfig::ForEngineHost(), fixture.context.Get(), "Test"));
    constexpr LogCategory audio{"Audio"};
    CHECK(!host.System().IsEnabled(LogLevel::Info, audio));
    CHECK(host.System().IsEnabled(LogLevel::Warning, audio));
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

// 실행 중 log.level·log.levels 변경은 sync 지점에서 적용되고, 시작 뒤 고정 설정의 콘솔 변경은 값을
// 바꾸지 않는다(Warning BootValueIgnored — Docs/Design/ConfigSystem.ko.md 8절).
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
    REQUIRE(context.AddAssignment(config::ConfigLayer::Console, "console", LogModeSetting, "sync"));
    REQUIRE(context.AddAssignment(config::ConfigLayer::Console, "console", LogFileEnabledSetting,
                                  "true"));
    REQUIRE(context.Sync());
    CHECK(logs.Mode() == LogMode::Async);
    auto mode = context.FindConsoleSetting(LogModeSetting);
    REQUIRE(mode);
    CHECK(context.ConsoleValue(mode.Value()).string.View() != StringView("sync"));
    usize ignored = 0;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        ignored +=
            context.Diagnostics().Get(index).code == config::ConfigDiagnosticCode::BootValueIgnored
                ? 1
                : 0;
    }

    CHECK(ignored == 2);
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

// log.rate_limits 문법 검사도 상수 평가로 쓸 수 있다.
static_assert(CheckLogRateLimitsSyntax("").IsEmpty());
static_assert(CheckLogRateLimitsSyntax("Graphics=20, Engine.World.Tick=1000000").IsEmpty());
static_assert(CheckLogRateLimitsSyntax("Graphics=0") ==
              "a log.rate_limits value is not a whole number from 1 to 1000000");
static_assert(CheckLogRateLimitsSyntax("Graphics=1000001") ==
              "a log.rate_limits value is not a whole number from 1 to 1000000");
static_assert(CheckLogRateLimitsSyntax("Graphics") ==
              "a log.rate_limits entry is not <category prefix>=<records per second>");
static_assert(CheckLogRateLimitsSyntax("A=1,B=1,C=1,D=1,E=1,F=1,G=1,H=1,I=1,J=1,K=1,L=1,M=1,N=1,"
                                       "O=1,P=1,Q=1") ==
              "log.rate_limits has more than MaxLogRateLimits entries");

// 설정 log.rate_limits가 LogSystem에 적용되고, 잘못된 항목은 건너뛴다.
namespace {

// 늘 같은 시각을 돌려주는 시계다(rate limit 창을 결정적으로).
uint64 FixedClock() noexcept {
    constexpr uint64 FixedTicks = 1000;
    return FixedTicks;
}

} // namespace

// 설정 log.rate_limits가 LogSystem에 적용되고, 잘못된 항목은 건너뛴다. 같은 내용을 다시 적용하면
// 창을 초기화하지 않는다.
TEST(LogHostAppliesRateLimitsFromConfig) {
    LogSettingsFixture fixture;
    fixture.Apply(R"({"log.mode": "sync", "log.rate_limits": "Tests=2, Broken, Audio=0"})");
    LogSystemConfig preset = LogSystemConfig::ForEngineHost();
    preset.clock = &FixedClock;
    preset.clockFrequency = 1;
    LogHost host(fixture.system);
    REQUIRE(host.Start(preset, fixture.context.Get(), "Test"));
    REQUIRE(host.Watch(*fixture.context));
    {
        LogInstallScope installed(host.System());
        for (uint32 index = 0; index < 5; ++index) {
            Log::Info(LogHostTests, "rate limited {Index}", index);
        }

        // 다른 설정만 바뀐 sync 지점: rate limit 창은 그대로라 더 통과하지 않는다.
        REQUIRE(fixture.context->AddAssignment(config::ConfigLayer::Console, "console",
                                               LogLevelsSetting, "Audio=error"));
        REQUIRE(fixture.context->Sync());
        Log::Info(LogHostTests, "rate limited {Index}", 5);
    }

    CHECK(host.System().Stats().rateLimited == 4);
    host.Stop();
}

// log.levels 문법 검사는 상수 평가로 쓸 수 있다(설정 검증용). 이유는 영어 리터럴이다.
static_assert(CheckLogLevelsSyntax("").IsEmpty());
static_assert(CheckLogLevelsSyntax("Graphics=Warning, engine.world = debug,").IsEmpty());
static_assert(CheckLogLevelsSyntax("Graphics") ==
              "a log.levels entry is not <category prefix>=<level>");
static_assert(CheckLogLevelsSyntax("Graphics..D3D12=info") ==
              "a log.levels category prefix is not a valid category name");
static_assert(CheckLogLevelsSyntax("Graphics=loud") ==
              "a log.levels level is not trace, debug, info, warning, error or fatal");

namespace {

// 파일 하나에 레이아웃 설정을 주고 Warning 한 줄을 쓴 뒤 파일 내용을 돌려준다.
void WriteWithFileLayout(StringView layoutJson, Array<uint8>& file) noexcept {
    ScratchDirectory directory;
    LogSettingsFixture fixture;
    char settings[512];
    FixedStringBuilder text{Span<char>(settings)};
    (void)text.TryAppendFormat(R"({{"log.mode": "sync", "log.file.enabled": true, )"
                               R"("log.file.directory": "{}", "log.file.layout": {}}})",
                               directory.View(), layoutJson);
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
            Log::Warning(LogHostTests, "layout value {Value}", 9);
        }

        host.Stop();
    }

    REQUIRE(ReadAll(StringView(path.Data(), path.Size()), file));
}

} // namespace

// log.file.layout이 파일 줄 모양을 바꾸고, 잘못된 패턴이면 엔진은 계속하며 기본 레이아웃을 쓴다.
TEST(LogHostAppliesFileLayoutAndFallsBackOnInvalidPattern) {
    SystemAllocator allocator;
    Array<uint8> custom(allocator);
    WriteWithFileLayout(R"("<{Level:u3}|{Category}> {Message}{NewLine}")", custom);
    CHECK(Contains(custom, "<WRN|Tests.Host> layout value 9\n"));
    Array<uint8> fallback(allocator);
    WriteWithFileLayout(R"("{Level} {Mesage}")", fallback);
    CHECK(Contains(fallback, "[Warning][Tests.Host] layout value 9"));
}

namespace {

// 문이 열릴 때까지 Write 안에서 기다리는 sink다(처음에는 닫혀 있다).
class HoldingSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent&) noexcept override {
        entered.Signal();
        if (release.Wait(HoldingSinkSafetyMilliseconds) != platform::WaitResult::Signaled) {
            platform::Fatal("holding sink was never released");
        }

        return Result<void>::Success();
    }

    Result<void> Flush() noexcept override {
        return Result<void>::Success();
    }

    // 시험이 끝내 열지 못할 때만 쓰는 안전 상한이다(순서는 entered·release가 정한다).
    static constexpr uint32 HoldingSinkSafetyMilliseconds = 30000;

    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
};

} // namespace

// 멈춘 sink 때문에 출력 스레드가 남으면 Stop은 LogSystem을 파괴하지 않고 놓으며(소멸자의 Join이
// 끝나지 않는다) WriterAbandoned를 알린다. 그 뒤 Start는 Busy다.
TEST(LogHostStopLeavesTheLogSystemWhenTheWriterIsStuck) {
    // 남은 출력 스레드가 돌아오면 sink와 allocator를 쓴다. 시험 범위보다 오래 살도록 일부러 놓는다
    // (allocator도 파괴하지 않는다 — 소멸 때 남은 할당을 누수로 알린다).
    alignas(SystemAllocator) static uint8 allocatorStorage[sizeof(SystemAllocator)];
    static SystemAllocator* const leakedAllocator = ConstructAt<SystemAllocator>(allocatorStorage);
    SystemAllocator& leaked = *leakedAllocator;
    auto storage = leaked.Allocate(sizeof(HoldingSink), alignof(HoldingSink));
    REQUIRE(storage);
    HoldingSink& sink = *ConstructAt<HoldingSink>(storage.Value());
    LogSystemConfig preset = LogSystemConfig::ForEngineHost();
    preset.drainTimeoutMilliseconds = 10;
    LogHost host(leaked);
    REQUIRE(host.Start(preset, nullptr, "Test"));
    CHECK(!host.WriterAbandoned());
    REQUIRE(host.System().AddSink(sink));
    {
        LogInstallScope installed(host.System());
        Log::Warning(LogHostTests, "held by a stuck sink (expected in test log)");
        if (sink.entered.Wait(HoldingSink::HoldingSinkSafetyMilliseconds) !=
            platform::WaitResult::Signaled) {
            platform::Fatal("holding sink was not entered");
        }
    }

    // sink는 Stop이 반환한 뒤에야 열린다.
    host.Stop();
    CHECK(host.WriterAbandoned());
    auto restarted = host.Start(preset, nullptr, "Test");
    CHECK(!restarted && restarted.Error() == ErrorCode::Busy);
    host.Stop();
    sink.release.Signal();
}

namespace {

// 봉인한 묶음 평문(zstd frame)을 풀어 모은다.
Result<void> CollectInflated(void* raw, Span<const uint8> frame) noexcept {
    if (frame.Size() == 0) {
        return Result<void>::Success();
    }

    constexpr usize MaxInflatedBytes = 1u << 20;
    auto inflated = TryDecompressAppend(CompressionFormat::Zstd, frame, MaxInflatedBytes,
                                        *static_cast<Array<uint8>*>(raw));
    return inflated ? Result<void>::Success() : Result<void>::Failure(inflated.Error());
}

} // namespace

// log.file.compress: 묶음마다 독립 zstd frame(.log.zst)이고 Compression으로 풀린다. 봉인과 함께면
// 압축한 뒤 봉인하고(머리에 표시) 묶음마다 풀어 원문이 나온다.
TEST(LogHostWritesCompressedFiles) {
    constexpr bool SealModes[] = {false, true};
    for (const bool sealed : SealModes) {
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
                 "log.file.compress": true, "log.file.encrypt": {},
                 "log.file.recipient_key": "{}"}})",
            directory.View(), sealed ? "true" : "false", StringView(base64, encoded.Value()));
        fixture.Apply(text.View());
        Array<char> path(fixture.system);
        {
            LogHost host(fixture.system);
            REQUIRE(host.Start(LogSystemConfig::ForEngineHost(), fixture.context.Get(), "Test"));
            REQUIRE(host.IsFileLogging());
            REQUIRE(path.TryAppend(
                Span<const char>(host.FilePath().Data(), host.FilePath().ByteCount())));
            {
                LogInstallScope installed(host.System());
                for (uint32 index = 0; index < 3; ++index) {
                    Log::Warning(LogHostTests, "compressed value {Value}", index);
                }
            }

            host.Stop();
        }

        const StringView name(path.Data(), path.Size());
        CHECK(name.EndsWith(sealed ? ".log" : ".log.zst", StringComparison::CaseSensitive));
        Array<uint8> file(fixture.system);
        REQUIRE(ReadAll(name, file));
        CHECK(!Contains(file, "compressed value"));
        Array<uint8> plain(fixture.system);
        if (sealed) {
            CHECK(crypto::LogFileHasCompressedBatches(file.View()));
            auto summary = crypto::TryDecodeLogFile(pair.Value(), file.View(), fixture.system,
                                                    &CollectInflated, &plain);
            REQUIRE(summary);
            CHECK(summary.Value().compressedBatches && summary.Value().endMarker);
        } else {
            constexpr usize MaxInflatedBytes = 1u << 20;
            REQUIRE(
                TryDecompressAppend(CompressionFormat::Zstd, file.View(), MaxInflatedBytes, plain));
        }

        CHECK(Contains(plain, "[Warning][Tests.Host] compressed value 0"));
        CHECK(Contains(plain, "[Warning][Tests.Host] compressed value 2"));
    }
}

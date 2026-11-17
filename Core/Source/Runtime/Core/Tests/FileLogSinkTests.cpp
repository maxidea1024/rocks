#include <TestSupport/Test.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/FileLogSink.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>

using namespace rocks;

namespace rocks::test {
uint64 OperatorNewCalls() noexcept;
} // namespace rocks::test

namespace {

constexpr LogCategory LogFileTests{"Tests.File"};

// 작업 디렉터리 아래 시험 전용 폴더다(소멸 때 지운다). 이름은 실행·시험마다 다르다(시간은 이름에만
// 쓴다).
struct ScratchDirectory {
    ScratchDirectory() noexcept {
        static uint32 counter = 0;
        FixedStringBuilder text{Span<char>(root)};
        (void)text.TryAppendFormat(
            "rocks-filelog-{}-{}", platform::MonotonicNanoseconds(),
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

// 폴더의 일반 파일 수다.
usize CountFiles(const ScratchDirectory& directory) noexcept {
    usize count = 0;
    (void)platform::PhysicalFileSystem().EnumerateDirectory(directory.root, directory.length,
                                                            &CountFile, &count);
    return count;
}

// 파일 전체를 읽는다.
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

StringView AsText(const Array<uint8>& bytes) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
}

LogLayout PlainLayout() noexcept {
    return LogLayout::Parse("[{Level}][{Category}] {Message}{NewLine}").Value();
}

FileLogSinkConfig Config(const ScratchDirectory& directory) noexcept {
    FileLogSinkConfig config;
    config.directory = directory.View();
    config.hostName = "Test";
    config.layout = PlainLayout();
    return config;
}

LogSystemConfig SyncConfig() noexcept {
    LogSystemConfig config = LogSystemConfig::ForTool();
    config.globalMinimum = LogLevel::Trace;
    return config;
}

// 앞 시험이 설치 밖에서 남긴 버림 알림을 비운다(정확한 출력을 단언하는 시험이 먼저 부른다).
void ResetUninstalledDrops() noexcept {
    SystemAllocator allocator;
    LogSystem reset(allocator, SyncConfig());
    (void)reset.Start();
    LogInstallScope installed(reset);
}

// 실패하는 머리를 가진 봉인이다(키 준비 실패).
class FailingCipher final : public LogFileCipher {
public:
    usize HeaderBytes() const noexcept override {
        return 16;
    }

    usize SealOverhead() const noexcept override {
        return 16;
    }

    Result<usize> TryWriteHeader(Span<uint8>) noexcept override {
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    Result<usize> TrySeal(Span<const uint8>, bool, Span<uint8>) noexcept override {
        sealed = true;
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    bool sealed = false;
};

// 물리 파일 시스템에 넘기되, failOpens가 켜지면 새 파일 열기를 거부한다(회전 실패 주입).
class FlakyFileSystem final : public platform::FileSystem {
public:
    platform::FileStatus Open(const char* path, usize bytes, platform::FileOpenMode mode,
                              platform::FileHandle*& out) noexcept override {
        if (failOpens) {
            return {platform::FileCode::AccessDenied, 5};
        }

        return inner.Open(path, bytes, mode, out);
    }

    platform::FileStatus Exists(const char* path, usize bytes, bool& exists) noexcept override {
        return inner.Exists(path, bytes, exists);
    }

    platform::FileStatus Delete(const char* path, usize bytes) noexcept override {
        return inner.Delete(path, bytes);
    }

    platform::FileStatus Stat(const char* path, usize bytes,
                              platform::FileInfo& info) noexcept override {
        return inner.Stat(path, bytes, info);
    }

    platform::FileStatus MakeDirectory(const char* path, usize bytes) noexcept override {
        return inner.MakeDirectory(path, bytes);
    }

    platform::FileStatus EnumerateDirectory(const char* path, usize bytes,
                                            platform::DirectoryVisitor visitor,
                                            void* context) noexcept override {
        return inner.EnumerateDirectory(path, bytes, visitor, context);
    }

    platform::FileSystem& inner = platform::PhysicalFileSystem();
    bool failOpens = false;
};

LogEvent Event(StringView message) noexcept {
    LogEvent event;
    event.level = LogLevel::Info;
    event.category = "Tests.File";
    event.message = message;
    return event;
}

// 모든 할당을 센다.
class CountingAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&allocations, uint64{1});
        return inner.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner.Deallocate(address);
    }

    uint64 Allocations() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&allocations);
    }

    SystemAllocator inner;
    uint64 allocations = 0;
};

} // namespace

// 평문 왕복: LogSystem(Sync)에서 남긴 기록이 레이아웃 그대로 파일에 남는다.
TEST(FileLogSinkWritesPlaintextThroughLayout) {
    ResetUninstalledDrops();
    ScratchDirectory directory;
    SystemAllocator allocator;
    Array<char> path(allocator);
    {
        FileLogSink file(allocator, Config(directory));
        REQUIRE(file.Open().IsOk());
        CHECK(file.Open().Error() == ErrorCode::AlreadyExists);
        REQUIRE(path.TryAppend(
            Span<const char>(file.CurrentPath().Data(), file.CurrentPath().ByteCount())));
        CHECK(file.CurrentPath().StartsWith(directory.View(), StringComparison::CaseSensitive));
        CHECK(file.CurrentPath().EndsWith("Z.log", StringComparison::CaseSensitive));
        LogSystem logs(allocator, SyncConfig());
        REQUIRE(logs.AddSink(file).IsOk());
        REQUIRE(logs.Start().IsOk());
        {
            LogInstallScope installed(logs);
            Log::Info(LogFileTests, "first {Index}", 1);
            Log::Warning(LogFileTests, "second {Name}", "value");
        }

        logs.Stop();
        file.Close();
    }

    Array<uint8> bytes(allocator);
    REQUIRE(ReadAll(StringView(path.Data(), path.Size()), bytes));
    CHECK(AsText(bytes) == "[Info][Tests.File] first 1\n[Warning][Tests.File] second value\n");
}

// JSON lines 형식은 기록마다 JSON 객체 한 줄이다.
TEST(FileLogSinkWritesJsonLines) {
    ResetUninstalledDrops();
    ScratchDirectory directory;
    SystemAllocator allocator;
    Array<char> path(allocator);
    {
        FileLogSinkConfig config = Config(directory);
        config.format = LogFileFormat::JsonLines;
        FileLogSink file(allocator, config);
        REQUIRE(file.Open().IsOk());
        REQUIRE(path.TryAppend(
            Span<const char>(file.CurrentPath().Data(), file.CurrentPath().ByteCount())));
        REQUIRE(file.Write(Event("one")).IsOk());
        REQUIRE(file.Write(Event("two")).IsOk());
        REQUIRE(file.Flush().IsOk());
    }

    Array<uint8> bytes(allocator);
    REQUIRE(ReadAll(StringView(path.Data(), path.Size()), bytes));
    const StringView text = AsText(bytes);
    CHECK(text.StartsWith("{", StringComparison::CaseSensitive));
    CHECK(text.Contains("\"msg\":\"one\"", StringComparison::CaseSensitive));
    CHECK(text.Contains("\"msg\":\"two\"", StringComparison::CaseSensitive));
    usize lines = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        lines += text.Data()[index] == '\n' ? 1u : 0u;
    }

    CHECK(lines == 2);
}

// 크기 상한을 넘으면 새 파일로 회전하고, 폴더에는 보관 개수만 남는다(이전 실행의 파일 포함).
TEST(FileLogSinkRotatesAndKeepsRetentionCount) {
    ScratchDirectory directory;
    SystemAllocator allocator;
    REQUIRE(platform::PhysicalFileSystem().MakeDirectory(directory.root, directory.length));
    // 이전 실행이 남긴 파일 둘과 다른 호스트의 파일 하나다.
    // 이 호스트의 이전 실행 파일 둘, 이름이 "Test-"로 시작하는 다른 호스트(Test-Server) 하나, 다른
    // 호스트 하나다. 보관 정리는 이 호스트의 이름 모양만 센다.
    const char* const previous[] = {"Test-20260101T000000000Z.log",
                                    "Test-20260101T000000000Z-2.log",
                                    "Test-Server-20260101T000000000Z.log", "Other-keep.log"};
    for (const char* name : previous) {
        char path[256];
        FixedStringBuilder built{Span<char>(path)};
        (void)built.TryAppendFormat("{}/{}", directory.View(), StringView(name));
        platform::File old;
        REQUIRE(old.Open(path, platform::FileOpenMode::CreateNew));
        REQUIRE(old.Close());
    }

    {
        FileLogSinkConfig config = Config(directory);
        config.maxBytes = 64;
        config.retention = 2;
        FileLogSink file(allocator, config);
        REQUIRE(file.Open().IsOk());
        // 보관 2: 이전 파일 둘 중 오래된 것 하나를 지우고 새 파일을 더했다(다른 호스트 둘은
        // 그대로).
        CHECK(CountFiles(directory) == 4);
        for (uint32 index = 0; index < 10; ++index) {
            char message[64];
            FixedStringBuilder built{Span<char>(message)};
            (void)built.TryAppendFormat("record number {} with some padding", index);
            REQUIRE(file.Write(Event(built.View())).IsOk());
            REQUIRE(file.Flush().IsOk());
        }

        CHECK(file.Rotations() >= 3);
    }

    // 이 호스트 파일 2개(마지막 두 회전) + 다른 호스트 2개(Test-Server 포함)다.
    CHECK(CountFiles(directory) == 4);
    char server[256];
    FixedStringBuilder serverPath{Span<char>(server)};
    (void)serverPath.TryAppendFormat("{}/Test-Server-20260101T000000000Z.log", directory.View());
    bool serverKept = false;
    CHECK(platform::PhysicalFileSystem().Exists(server, serverPath.ByteCount(), serverKept) &&
          serverKept);
}

// Open 뒤의 기록·flush·회전은 엔진 allocator와 operator new를 쓰지 않는다.
TEST(FileLogSinkDoesNotAllocateAfterOpen) {
    ResetUninstalledDrops();
    ScratchDirectory directory;
    CountingAllocator allocator;
    FileLogSinkConfig config = Config(directory);
    config.maxBytes = 512;
    config.retention = 2;
    FileLogSink file(allocator, config);
    REQUIRE(file.Open().IsOk());
    LogSystemConfig logConfig = LogSystemConfig::ForEngineHost();
    logConfig.globalMinimum = LogLevel::Trace;
    LogSystem logs(allocator, logConfig);
    REQUIRE(logs.AddSink(file).IsOk());
    REQUIRE(logs.Start().IsOk());
    const uint64 allocations = allocator.Allocations();
    const uint64 news = test::OperatorNewCalls();
    {
        LogInstallScope installed(logs);
        for (uint32 index = 0; index < 200; ++index) {
            Log::Info(LogFileTests, "allocation free record {Index} {Ratio:.2f}", index, 0.5);
            if (index % 20 == 19) {
                REQUIRE(logs.Flush());
            }
        }

        REQUIRE(logs.Flush());
    }

    CHECK(allocator.Allocations() == allocations);
    CHECK(test::OperatorNewCalls() == news);
    CHECK(file.Rotations() > 0);
    logs.Stop();
    file.Close();
}

// 파일을 열지 못해도(회전 실패) 멈추지 않는다: 실패를 돌려주고, 열 수 있게 되면 새 파일로 이어
// 쓴다.
TEST(FileLogSinkSurvivesRotationFailure) {
    ScratchDirectory directory;
    SystemAllocator allocator;
    FlakyFileSystem system;
    FileLogSinkConfig config = Config(directory);
    config.maxBytes = 48;
    config.fileSystem = &system;
    FileLogSink file(allocator, config);
    REQUIRE(file.Open().IsOk());
    REQUIRE(file.Write(Event("before the failure")).IsOk());
    REQUIRE(file.Flush().IsOk());
    system.failOpens = true;
    REQUIRE(file.Write(Event("this record needs a new file")).IsOk());
    CHECK(file.Flush().Error() == ErrorCode::DependencyFailed);
    CHECK(file.Flush().Error() == ErrorCode::DependencyFailed);
    system.failOpens = false;
    CHECK(file.Flush().IsOk());
    CHECK(file.Rotations() == 1);
    file.Close();
    CHECK(CountFiles(directory) == 2);
}

// 봉인 머리(키 준비)가 실패하면 파일을 만들지 않고 sink가 꺼진다(평문으로 내려가지 않는다).
TEST(FileLogSinkDisablesItselfWhenCipherSetupFails) {
    ScratchDirectory directory;
    SystemAllocator allocator;
    FailingCipher cipher;
    FileLogSinkConfig config = Config(directory);
    config.cipher = &cipher;
    FileLogSink file(allocator, config);
    CHECK(file.Open().Error() == ErrorCode::AuthenticationFailed);
    CHECK(file.IsDisabled());
    CHECK(file.Write(Event("secret")).Error() == ErrorCode::Unsupported);
    CHECK(!cipher.sealed);
    CHECK(CountFiles(directory) == 0);
}

// 잘못된 설정은 Open 전에 거부한다.
TEST(FileLogSinkRejectsInvalidConfig) {
    ScratchDirectory directory;
    SystemAllocator allocator;
    FileLogSinkConfig config = Config(directory);
    config.hostName = "bad name";
    CHECK(FileLogSink(allocator, config).Open().Error() == ErrorCode::InvalidArgument);
    config = Config(directory);
    config.retention = 0;
    CHECK(FileLogSink(allocator, config).Open().Error() == ErrorCode::InvalidArgument);
    config = Config(directory);
    config.directory = {};
    CHECK(FileLogSink(allocator, config).Open().Error() == ErrorCode::InvalidArgument);
    CHECK(allocator.Stats().liveAllocations == 0);
}

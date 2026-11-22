#include <TestSupport/Test.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/FileLogSink.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/CrashReporting.h>
#include <Platform/File.h>
#include <Platform/Path.h>

using namespace rocks;

namespace rocks::test {
uint64 OperatorNewCalls() noexcept;
} // namespace rocks::test

// 크래시 처리가 칸을 읽는 PlatformBase 내부 함수다(Private/Common/CrashReportingInternal.h). 시험은
// 크래시 기록에 실제로 남을 값을 이것으로 확인한다.
namespace rocks::platform::detail {
bool ReadCrashAnnotation(CrashAnnotation slot, char* buffer, usize capacity, usize& bytes) noexcept;
} // namespace rocks::platform::detail

namespace file_log_sink_tests_file {

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

    platform::FileStatus FileExists(const char* path, usize bytes, bool& exists) noexcept override {
        return inner.FileExists(path, bytes, exists);
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
        CHECK(file.CurrentPath().StartsWith(file.Directory(), StringComparison::CaseSensitive));
        CHECK(file.Directory().EndsWith(directory.View(), StringComparison::CaseSensitive));
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
    CHECK(platform::PhysicalFileSystem().FileExists(server, serverPath.ByteCount(), serverKept) &&
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

namespace {

constexpr uint64 NanosecondsPerSecond = 1000000000;
constexpr uint64 MaxAgeSeconds = 10;

// 시험용 압축: 앞에 'Z' 한 byte를 붙여 복사한다(배선·확장자 확인용).
class MarkingCompressor final : public LogBatchCompressor {
public:
    usize Bound(usize batchBytes) const noexcept override {
        return batchBytes + 1;
    }

    Result<usize> TryCompress(Span<const uint8> batch, Span<uint8> out) noexcept override {
        if (out.Size() < batch.Size() + 1) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }

        out[0] = 'Z';
        platform::MemoryCopy(out.Data() + 1, batch.Data(), batch.Size());
        ++calls;
        return Result<usize>::Success(batch.Size() + 1);
    }

    uint32 calls = 0;
};

bool CollectZst(const platform::DirectoryEntry& entry, void* raw) noexcept {
    const StringView name(entry.name, entry.nameBytes);
    if (name.EndsWith(".log.zst", StringComparison::CaseSensitive)) {
        ++*static_cast<usize*>(raw);
    }

    return true;
}

} // namespace

// 시간 기준 회전은 기록의 단조 시각으로 판정한다: 파일 첫 기록 + 상한을 넘은 묶음이 새 파일로 간다.
// 시계를 주입하지 않고 LogEvent에 시각을 정해 넣으므로 결정적이다.
TEST(FileLogSinkRotatesByElapsedRecordTime) {
    ScratchDirectory directory;
    SystemAllocator allocator;
    FileLogSinkConfig config = Config(directory);
    config.maxAgeNanoseconds = MaxAgeSeconds * NanosecondsPerSecond;
    FileLogSink file(allocator, config);
    REQUIRE(file.Open().IsOk());
    const uint64 seconds[] = {0, 5, 11, 15, 22};
    for (uint64 second : seconds) {
        LogEvent event = Event("timed record");
        event.monotonicNanoseconds = second * NanosecondsPerSecond;
        REQUIRE(file.Write(event).IsOk());
        REQUIRE(file.Flush().IsOk());
    }

    // 0·5초는 첫 파일, 11·15초는 둘째(11초에 회전), 22초는 셋째(11 + 10을 넘음)다.
    CHECK(file.Rotations() == 2);
    file.Close();
    CHECK(CountFiles(directory) == 3);
}

// 압축이 있으면 묶음마다 압축하고, 봉인하지 않은 파일은 ".log.zst"다.
TEST(FileLogSinkCompressesBatches) {
    ScratchDirectory directory;
    SystemAllocator allocator;
    MarkingCompressor compressor;
    FileLogSinkConfig config = Config(directory);
    config.compressor = &compressor;
    Array<char> path(allocator);
    {
        FileLogSink file(allocator, config);
        REQUIRE(file.Open().IsOk());
        REQUIRE(file.Write(Event("compressed record")).IsOk());
        REQUIRE(file.Flush().IsOk());
        REQUIRE(path.TryAppend(
            Span<const char>(file.CurrentPath().Data(), file.CurrentPath().ByteCount())));
        file.Close();
    }

    CHECK(compressor.calls == 1);
    CHECK(
        StringView(path.Data(), path.Size()).EndsWith(".log.zst", StringComparison::CaseSensitive));
    usize compressed = 0;
    (void)platform::PhysicalFileSystem().EnumerateDirectory(directory.root, directory.length,
                                                            &CollectZst, &compressed);
    CHECK(compressed == 1);
    Array<uint8> bytes(allocator);
    REQUIRE(ReadAll(StringView(path.Data(), path.Size()), bytes));
    REQUIRE(bytes.Size() > 1);
    CHECK(bytes[0] == 'Z');
}

namespace {

// 크래시 기록 칸의 지금 값이다.
struct AnnotationText {
    StringView View() const noexcept {
        return StringView(text, bytes);
    }

    char text[platform::MaxCrashAnnotationBytes]{};
    usize bytes = 0;
    bool read = false;
};

AnnotationText ReadLogFileAnnotation() noexcept {
    AnnotationText value;
    value.read = platform::detail::ReadCrashAnnotation(platform::CrashAnnotation::LogFile,
                                                       value.text, sizeof(value.text), value.bytes);
    return value;
}

// 시험이 바꾼 LogFile 칸을 끝에 비운다(같은 프로세스의 뒤 시험·크래시 기록에 남지 않게).
struct LogFileAnnotationCleanup {
    ~LogFileAnnotationCleanup() noexcept {
        (void)platform::SetCrashAnnotation(platform::CrashAnnotation::LogFile, nullptr, 0);
    }
};

constexpr StringView PreviousAnnotation = "previous";

// 가상 폴더 이름의 byte 수다. 크래시 기록 칸보다 길어 그 아래 파일 경로는 칸에 들어가지 않는다.
constexpr usize LongDirectoryBytes = platform::MaxCrashAnnotationBytes + 64;

// 긴 가상 폴더 이름(LongDirectoryBytes byte의 'v')을 시험 폴더로 바꿔 실제 파일 시스템에 넘긴다.
// 크래시 기록 칸보다 긴 로그 경로를 OS 경로 길이 제한 없이 만든다.
class LongNameFileSystem final : public platform::FileSystem {
public:
    explicit LongNameFileSystem(const ScratchDirectory& directory) noexcept
        : directory_(directory) {
        platform::MemorySet(name_, static_cast<uint8>('v'), LongDirectoryBytes);
    }

    StringView Name() const noexcept {
        return StringView(name_, LongDirectoryBytes);
    }

    platform::FileStatus Open(const char* path, usize bytes, platform::FileOpenMode mode,
                              platform::FileHandle*& out) noexcept override {
        const MappedPath mapped = Map(path, bytes);
        return inner.Open(mapped.path, mapped.bytes, mode, out);
    }

    platform::FileStatus FileExists(const char* path, usize bytes, bool& exists) noexcept override {
        const MappedPath mapped = Map(path, bytes);
        return inner.FileExists(mapped.path, mapped.bytes, exists);
    }

    platform::FileStatus Delete(const char* path, usize bytes) noexcept override {
        const MappedPath mapped = Map(path, bytes);
        return inner.Delete(mapped.path, mapped.bytes);
    }

    platform::FileStatus Stat(const char* path, usize bytes,
                              platform::FileInfo& info) noexcept override {
        const MappedPath mapped = Map(path, bytes);
        return inner.Stat(mapped.path, mapped.bytes, info);
    }

    platform::FileStatus MakeDirectory(const char* path, usize bytes) noexcept override {
        const MappedPath mapped = Map(path, bytes);
        return inner.MakeDirectory(mapped.path, mapped.bytes);
    }

    platform::FileStatus EnumerateDirectory(const char* path, usize bytes,
                                            platform::DirectoryVisitor visitor,
                                            void* context) noexcept override {
        const MappedPath mapped = Map(path, bytes);
        return inner.EnumerateDirectory(mapped.path, mapped.bytes, visitor, context);
    }

    platform::FileSystem& inner = platform::PhysicalFileSystem();

private:
    struct MappedPath {
        char path[MaxLogFilePathBytes]{};
        usize bytes = 0;
    };

    // 가상 이름으로 시작하면 그 부분을 시험 폴더로 바꾼다(NUL 종료).
    MappedPath Map(const char* path, usize bytes) const noexcept {
        MappedPath mapped;
        FixedStringBuilder built{Span<char>(mapped.path)};
        if (bytes >= LongDirectoryBytes &&
            platform::MemoryEquals(path, name_, LongDirectoryBytes)) {
            (void)built.TryAppend(directory_.View());
            (void)built.TryAppend(
                StringView(path + LongDirectoryBytes, bytes - LongDirectoryBytes));
        } else {
            (void)built.TryAppend(StringView(path, bytes));
        }

        mapped.bytes = built.ByteCount();
        return mapped;
    }

    const ScratchDirectory& directory_;
    char name_[LongDirectoryBytes]{};
};

// 회전이 일어나도록 묶음 크기 상한을 넘는 기록 둘을 따로 flush한다(maxBytes = RotatingMaxBytes).
constexpr uint64 RotatingMaxBytes = 48;

void WriteUntilRotated(FileLogSink& file) noexcept {
    REQUIRE(file.Write(Event("first record before the rotation")).IsOk());
    REQUIRE(file.Flush().IsOk());
    REQUIRE(file.Write(Event("second record needs a new file")).IsOk());
    REQUIRE(file.Flush().IsOk());
    CHECK(file.Rotations() == 1);
}

} // namespace

// 상대 로그 폴더는 Open 때의 작업 디렉터리에 붙인 절대 경로가 되고, 절대 폴더는
// 그대로다(설계 9.1절).
TEST(FileLogSinkResolvesRelativeDirectoryAgainstWorkingDirectory) {
    ScratchDirectory directory;
    SystemAllocator allocator;
    char working[MaxLogFilePathBytes];
    usize workingBytes = 0;
    REQUIRE(platform::GetStandardDirectory(platform::StandardDirectory::Working, working,
                                           sizeof(working), workingBytes));
    const bool rootWorking = workingBytes != 0 && (working[workingBytes - 1] == '/' ||
                                                   working[workingBytes - 1] == '\\');
    char resolved[MaxLogFilePathBytes];
    usize resolvedBytes = 0;
    {
        FileLogSink file(allocator, Config(directory));
        CHECK(file.Directory() == directory.View());
        REQUIRE(file.Open().IsOk());
        CHECK(file.Directory().StartsWith(StringView(working, workingBytes),
                                          StringComparison::CaseSensitive));
        CHECK(file.Directory().EndsWith(directory.View(), StringComparison::CaseSensitive));
        CHECK(file.Directory().ByteCount() ==
              workingBytes + (rootWorking ? 0 : 1) + directory.length);
        CHECK(file.CurrentPath().StartsWith(file.Directory(), StringComparison::CaseSensitive));
        resolvedBytes = file.Directory().ByteCount();
        platform::MemoryCopy(resolved, file.Directory().Data(), resolvedBytes);
        file.Close();
    }

    FileLogSinkConfig config = Config(directory);
    config.directory = StringView(resolved, resolvedBytes);
    FileLogSink absolute(allocator, config);
    REQUIRE(absolute.Open().IsOk());
    CHECK(absolute.Directory() == StringView(resolved, resolvedBytes));
    absolute.Close();
}

// crashAnnotation을 켠 sink는 Open과 회전 때 크래시 기록의 LogFile 칸에 지금 파일을 알리고, Close는
// 칸을 비우지 않는다. 끈 sink(기본)는 칸을 건드리지 않는다(설계 14.1.8절, L29).
TEST(FileLogSinkAnnouncesCurrentFileForCrashRecords) {
    LogFileAnnotationCleanup cleanup;
    ScratchDirectory directory;
    SystemAllocator allocator;
    REQUIRE(platform::SetCrashAnnotation(platform::CrashAnnotation::LogFile,
                                         PreviousAnnotation.Data(),
                                         PreviousAnnotation.ByteCount()));
    {
        FileLogSinkConfig config = Config(directory);
        config.maxBytes = RotatingMaxBytes;
        FileLogSink quiet(allocator, config);
        REQUIRE(quiet.Open().IsOk());
        WriteUntilRotated(quiet);
        quiet.Close();
        const AnnotationText untouched = ReadLogFileAnnotation();
        CHECK(untouched.read);
        CHECK(untouched.View() == PreviousAnnotation);
    }

    Array<char> last(allocator);
    {
        FileLogSinkConfig config = Config(directory);
        config.maxBytes = RotatingMaxBytes;
        config.crashAnnotation = true;
        FileLogSink file(allocator, config);
        REQUIRE(file.Open().IsOk());
        const AnnotationText opened = ReadLogFileAnnotation();
        CHECK(opened.read);
        CHECK(opened.View() == file.CurrentPath());
        CHECK(opened.View().StartsWith(file.Directory(), StringComparison::CaseSensitive));
        WriteUntilRotated(file);
        const AnnotationText rotated = ReadLogFileAnnotation();
        CHECK(rotated.read);
        CHECK(rotated.View() == file.CurrentPath());
        CHECK(rotated.View() != opened.View());
        REQUIRE(last.TryAppend(
            Span<const char>(file.CurrentPath().Data(), file.CurrentPath().ByteCount())));
        file.Close();
    }

    const AnnotationText closed = ReadLogFileAnnotation();
    CHECK(closed.read);
    CHECK(closed.View() == StringView(last.Data(), last.Size()));
}

// 파일 경로가 칸(MaxCrashAnnotationBytes)보다 길면 자르지 않고 칸을 비운다. 앞의 값이 남아 다른
// 파일을 가리키지 않는다. 회전 뒤에도 비어 있다(Warning은 비상 경로에 한 번 — 시험 로그에 보인다).
TEST(FileLogSinkClearsCrashAnnotationWhenPathIsTooLong) {
    LogFileAnnotationCleanup cleanup;
    ScratchDirectory directory;
    SystemAllocator allocator;
    LongNameFileSystem system(directory);
    REQUIRE(platform::SetCrashAnnotation(platform::CrashAnnotation::LogFile,
                                         PreviousAnnotation.Data(),
                                         PreviousAnnotation.ByteCount()));
    FileLogSinkConfig config = Config(directory);
    config.directory = system.Name();
    config.fileSystem = &system;
    config.maxBytes = RotatingMaxBytes;
    config.crashAnnotation = true;
    FileLogSink file(allocator, config);
    REQUIRE(file.Open().IsOk());
    // 실제 파일 시스템이 아니므로 폴더를 절대 경로로 바꾸지 않는다.
    CHECK(file.Directory() == system.Name());
    CHECK(file.CurrentPath().ByteCount() > platform::MaxCrashAnnotationBytes);
    const AnnotationText opened = ReadLogFileAnnotation();
    CHECK(opened.read);
    CHECK(opened.bytes == 0);
    WriteUntilRotated(file);
    const AnnotationText rotated = ReadLogFileAnnotation();
    CHECK(rotated.read);
    CHECK(rotated.bytes == 0);
    file.Close();
    CHECK(CountFiles(directory) == 2);
}

} // namespace file_log_sink_tests_file

#include <TestSupport/Test.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSink.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>

using namespace rocks;

namespace {
constexpr LogCategory LogTests{"Tests"};
constexpr LogCategory LogTestsAlpha{"Tests.Alpha"};
constexpr LogCategory LogTestsBeta{"Tests.Beta"};
constexpr LogCategory LogTestsLogger{"Tests.Logger"};

// 시험이 멈추지 않게 거는 안전 상한이다(정상 진행은 이 시간에 기대지 않는다).
constexpr uint32 SafetyTimeoutMilliseconds = 30000;

struct CaptureSink final : RuntimeLogSink {
    uint32 count = 0;
    char lastCategory[MaxLogCategoryNameBytes]{};
    usize lastCategoryBytes = 0;
    LogLevel lastLevel = LogLevel::Trace;
    uint64 lastThread = 0;
    uint64 lastTime = 0;
    SourceLocation lastLocation;
    char bytes[4]{};
    usize byteCount = 0;
    bool fail = false;
    uint32* order = nullptr;
    uint32* orderCount = nullptr;
    uint32 id = 0;

    Result<void> Write(const LogEvent& event) noexcept override {
        ++count;
        if (order != nullptr && orderCount != nullptr) {
            order[(*orderCount)++] = id;
        }

        // 이름은 콜백 동안만 유효하므로 복사해 둔다.
        lastCategoryBytes = event.category.ByteCount();
        for (usize index = 0; index < lastCategoryBytes && index < sizeof(lastCategory); ++index) {
            lastCategory[index] = event.category.Data()[index];
        }

        lastLevel = event.level;
        lastThread = event.threadId;
        lastTime = event.monotonicNanoseconds;
        lastLocation = event.location;
        byteCount = event.message.ByteCount();
        for (usize index = 0; index < byteCount && index < sizeof(bytes); ++index) {
            bytes[index] = event.message.Data()[index];
        }

        return fail ? Result<void>::Failure(ErrorCode::OutOfMemory) : Result<void>::Success();
    }

    StringView LastCategory() const noexcept {
        return StringView(lastCategory, lastCategoryBytes);
    }
};

struct TextCapture {
    char bytes[128]{};
    usize count = 0;
    uint32 writes = 0;
    uint32 flushes = 0;
    uint32 failWrite = 0;
    bool failFlush = false;

    static Result<void> Write(void* raw, StringView view) noexcept {
        auto& capture = *static_cast<TextCapture*>(raw);
        ++capture.writes;
        if (capture.writes == capture.failWrite ||
            view.ByteCount() > sizeof(capture.bytes) - capture.count) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        for (usize index = 0; index < view.ByteCount(); ++index) {
            capture.bytes[capture.count++] = view.Data()[index];
        }

        return Result<void>::Success();
    }

    static Result<void> Flush(void* raw) noexcept {
        auto& capture = *static_cast<TextCapture*>(raw);
        ++capture.flushes;
        return capture.failFlush ? Result<void>::Failure(ErrorCode::DependencyFailed)
                                 : Result<void>::Success();
    }
};

// 시험마다 Sync 모드 LogSystem을 설치한다(설계 5절의 시험 격리). 앞선 시험이 남긴 설치 전 버림 수가
// 이 sink에 알림으로 들어가지 않도록, 설치 전에 한 번 비운다.
struct SyncLogs {
    explicit SyncLogs(LogLevel minimum = LogLevel::Trace) noexcept
        : logs(allocator, MakeConfig(minimum)) {
        {
            LogSystem reset(allocator, MakeConfig(minimum));
            (void)reset.Start();
            LogInstallScope drained(reset);
        }

        if (!logs.Start()) {
            platform::Fatal("Sync log system did not start");
        }
    }

    static LogSystemConfig MakeConfig(LogLevel minimum) noexcept {
        LogSystemConfig config = LogSystemConfig::ForTool();
        config.globalMinimum = minimum;
        return config;
    }

    SystemAllocator allocator;
    LogSystem logs;
};

LogEvent MakeEvent(LogLevel level, StringView category, StringView message) noexcept {
    LogEvent event;
    event.level = level;
    event.category = category;
    event.message = message;
    return event;
}

void Join(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Logger test worker did not finish");
    }
}

void WriteMany(void*) noexcept {
    for (uint32 index = 0; index < 400; ++index) {
        Log::Info(LogTestsAlpha, "worker {Index}", index);
    }
}
} // namespace

TEST(LogSystemFanoutFiltersAndOwnedBytes) {
    SyncLogs setup;
    CaptureSink first;
    CaptureSink second;
    REQUIRE(setup.logs.AddSink(first).IsOk());
    REQUIRE(setup.logs.AddSink(second, {LogLevel::Warning, LogTestsAlpha}).IsOk());
    CHECK(setup.logs.AddSink(first).Error() == ErrorCode::AlreadyExists);
    LogInstallScope installed(setup.logs);
    Log::Info(LogTestsAlpha, "info");
    CHECK(first.count == 1 && second.count == 0);
    const char embedded[] = {'A', '\0', 'B'};
    Log::Warning(LogTestsAlpha, "{}", StringView(embedded, sizeof(embedded)));
    CHECK(first.count == 2 && second.count == 1 && second.byteCount == 3);
    CHECK(second.bytes[0] == 'A' && second.bytes[1] == '\0' && second.bytes[2] == 'B');
    CHECK(second.LastCategory() == "Tests.Alpha" && second.lastLevel == LogLevel::Warning &&
          second.lastThread != 0 && second.lastTime != 0);
    Log::Error(LogTestsBeta, "other");
    CHECK(second.count == 1);
    CHECK(setup.logs.RemoveSink(second).IsOk());
    CHECK(setup.logs.RemoveSink(second).Error() == ErrorCode::NotFound);
    REQUIRE(setup.logs.AddSink(second, {LogLevel::Error, {}}).IsOk());
    Log::Warning(LogTestsAlpha, "filtered");
    Log::Error(LogTestsBeta, "accepted");
    CHECK(second.count == 2 && second.LastCategory() == "Tests.Beta");
}

// 한 sink가 실패해도 나머지는 등록 순서대로 받는다. 실패는 통계에 남는다.
TEST(LogSystemReportsSinkFailureAndKeepsOrder) {
    SyncLogs setup;
    CaptureSink failing;
    CaptureSink succeeding;
    failing.fail = true;
    uint32 order[2]{};
    uint32 orderCount = 0;
    failing.order = succeeding.order = order;
    failing.orderCount = succeeding.orderCount = &orderCount;
    failing.id = 1;
    succeeding.id = 2;
    REQUIRE(setup.logs.AddSink(failing).IsOk());
    REQUIRE(setup.logs.AddSink(succeeding).IsOk());
    LogInstallScope installed(setup.logs);
    Log::Error(LogTests, "failure");
    CHECK(failing.count == 1 && succeeding.count == 1);
    CHECK(orderCount == 2 && order[0] == 1 && order[1] == 2);
    CHECK(setup.logs.Stats().sinkFailures == 1);
}

TEST(TextLogSinkOutputAndFailures) {
    TextCapture output;
    TextLogSink sink({&output, TextCapture::Write, TextCapture::Flush});
    const char message[] = {static_cast<char>(0xed), static_cast<char>(0x95),
                            static_cast<char>(0x9c), '\0', 'X'};
    REQUIRE(sink.Write(MakeEvent(LogLevel::Warning, LogTestsLogger.Name(),
                                 StringView(message, sizeof(message))))
                .IsOk());
    // 접두사는 레벨 이름과 카테고리 이름이다. 메시지 byte(한글·NUL)는 그대로 지나간다.
    const char prefix[] = "[Warning][Tests.Logger] ";
    char expected[sizeof(prefix) - 1 + sizeof(message) + 1]{};
    usize expectedCount = 0;
    for (usize index = 0; index + 1 < sizeof(prefix); ++index) {
        expected[expectedCount++] = prefix[index];
    }

    for (usize index = 0; index < sizeof(message); ++index) {
        expected[expectedCount++] = message[index];
    }

    expected[expectedCount++] = '\n';
    REQUIRE(output.count == sizeof(expected));
    for (usize index = 0; index < sizeof(expected); ++index) {
        CHECK(output.bytes[index] == expected[index]);
    }

    CHECK(output.writes == 3 && output.flushes == 1);
    output = {};
    output.failWrite = 2;
    CHECK(sink.Write(MakeEvent(LogLevel::Info, "Tests", "x")).Error() ==
          ErrorCode::DependencyFailed);
    CHECK(output.writes == 2 && output.flushes == 0);
    output = {};
    output.failFlush = true;
    CHECK(sink.Write(MakeEvent(LogLevel::Info, "Tests", "x")).Error() ==
          ErrorCode::DependencyFailed);
    CHECK(output.writes == 3 && output.flushes == 1);
    const char invalid[] = {static_cast<char>(0xff)};
    CHECK(sink.Write(MakeEvent(LogLevel::Info, "Tests", StringView(invalid, 1))).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(sink.Write(MakeEvent(static_cast<LogLevel>(LogLevelCount), "Tests", "x")).Error() ==
          ErrorCode::InvalidArgument);
}

// 외부 프로세스가 stderr 원시 byte를 캡처하여 한글과 NUL까지 확인할 수 있는 전용 probe다.
TEST(RuntimeLoggerStderrOutputProbe) {
    SyncLogs setup;
    StderrLogSink stderrSink;
    REQUIRE(setup.logs.AddSink(stderrSink).IsOk());
    LogInstallScope installed(setup.logs);
    const char message[] = {static_cast<char>(0xed), static_cast<char>(0x95),
                            static_cast<char>(0x9c), '\0', 'X'};
    Log::Warning(LogTestsLogger, "{}", StringView(message, sizeof(message)));
    CHECK(setup.logs.Stats().sinkFailures == 0);
}

// Sync 모드는 여러 스레드의 기록을 sink lock으로 직렬화한다.
TEST(LogSystemSyncSerializesConcurrentWriters) {
    SyncLogs setup;
    CaptureSink capture;
    REQUIRE(setup.logs.AddSink(capture).IsOk());
    LogInstallScope installed(setup.logs);
    platform::Thread workers[4];
    for (platform::Thread& worker : workers) {
        if (worker.Start(WriteMany, nullptr) != platform::ThreadStatus::Success) {
            platform::Fatal("Logger test worker start failed");
        }
    }

    for (platform::Thread& worker : workers) {
        Join(worker);
    }

    CHECK(capture.count == 1600);
}

namespace {
constexpr LogCategory LogGraphics{"Graphics"};
constexpr LogCategory LogGraphicsD3D12{"Graphics.D3D12"};
constexpr LogCategory LogGraphicsX{"GraphicsX"};
constexpr LogCategory LogAudio{"Audio"};
constexpr LogCategory LogDeepest{"A.B.C.D.E.F.G.H"};
constexpr LogCategory LogLongest{"L23456789012345678901234567890123456789012345678901234567890123"};

// 기본 인자 SourceLocation::Current()가 호출 지점을 담는지 보는 도우미다.
SourceLocation CaptureCaller(SourceLocation where = SourceLocation::Current()) noexcept {
    return where;
}

struct CapturedCall {
    SourceLocation where;
    uint32 line;
};

// 두 함수는 각자 자기 줄에서 CaptureCaller를 부른다. where의 줄과 함수가 서로 달라야 한다.
CapturedCall CallFromFirstFunction() noexcept {
    return {CaptureCaller(), __LINE__};
}

CapturedCall CallFromSecondFunction() noexcept {
    return {CaptureCaller(), __LINE__};
}

constexpr bool PrefixHashesAreConsteval() noexcept {
    return LogGraphicsD3D12.PrefixCount() == 2 &&
           LogGraphicsD3D12.PrefixHash(0) == HashLogCategoryName("Graphics") &&
           LogGraphicsD3D12.PrefixHash(1) == HashLogCategoryName("Graphics.D3D12") &&
           LogGraphicsD3D12.NameHash() == LogGraphicsD3D12.PrefixHash(1) &&
           LogGraphics.NameHash() == LogGraphicsD3D12.PrefixHash(0);
}

static_assert(PrefixHashesAreConsteval(),
              "category prefix hashes must be computed at compile time");
static_assert(LogDeepest.PrefixCount() == MaxLogCategoryDepth,
              "a name with MaxLogCategoryDepth segments is accepted");
static_assert(LogLongest.Name().ByteCount() == MaxLogCategoryNameBytes,
              "a name of MaxLogCategoryNameBytes bytes is accepted");
} // namespace

TEST(LogCategoryPrefixHashesAndNames) {
    CHECK(LogGraphicsD3D12.Name() == "Graphics.D3D12");
    CHECK(LogGraphicsD3D12.PrefixName(0) == "Graphics");
    CHECK(LogGraphicsD3D12.PrefixName(1) == "Graphics.D3D12");
    CHECK(LogGraphicsD3D12.PrefixName(2).IsEmpty() &&
          LogGraphicsD3D12.PrefixHash(2) == InvalidLogCategoryHash);
    CHECK(LogGraphics.PrefixCount() == 1 && LogGraphics.Name() == "Graphics");

    // consteval로 미리 계산한 해시가 런타임 문자열 해시와 같아야 후속 재정의 표(런타임 이름)와
    // 맞물린다. 상수 접기를 피하려고 런타임 버퍼에서 해시한다.
    char runtimeName[] = "Graphics.D3D12";
    CHECK(HashLogCategoryName(StringView(runtimeName, sizeof("Graphics") - 1)) ==
          LogGraphicsD3D12.PrefixHash(0));
    CHECK(HashLogCategoryName(StringView(runtimeName, sizeof(runtimeName) - 1)) ==
          LogGraphicsD3D12.NameHash());
    // 설정 값은 대소문자를 무시하고 맞춘다(설계 6.2절). 표기만 다른 런타임 이름도 같은 해시다.
    char upperName[] = "GRAPHICS";
    char lowerName[] = "graphics.d3d12";
    CHECK(HashLogCategoryName(StringView(upperName, sizeof(upperName) - 1)) ==
          LogGraphicsD3D12.PrefixHash(0));
    CHECK(HashLogCategoryName(StringView(lowerName, sizeof(lowerName) - 1)) ==
          LogGraphicsD3D12.NameHash());
    CHECK(HashLogCategoryName("Audio") != LogGraphics.NameHash());

    const char* const deepestPrefixes[] = {
        "A",         "A.B",         "A.B.C",         "A.B.C.D",
        "A.B.C.D.E", "A.B.C.D.E.F", "A.B.C.D.E.F.G", "A.B.C.D.E.F.G.H"};
    for (usize index = 0; index < MaxLogCategoryDepth; ++index) {
        CHECK(LogDeepest.PrefixName(index) == deepestPrefixes[index]);
        CHECK(LogDeepest.PrefixHash(index) == HashLogCategoryName(deepestPrefixes[index]));
    }

    CHECK(LogLongest.PrefixCount() == 1 &&
          LogLongest.Name().ByteCount() == MaxLogCategoryNameBytes);
}

TEST(LogCategoryPrefixMatching) {
    const LogCategoryPrefix all;
    CHECK(all.MatchesAll() && all.Matches(LogGraphics) && all.Matches(LogAudio));

    const LogCategoryPrefix graphics = LogGraphics;
    CHECK(!graphics.MatchesAll());
    CHECK(graphics.Matches(LogGraphics) && graphics.Matches(LogGraphicsD3D12));
    // 문자열 접두사는 같지만 구간 경계가 아니다.
    CHECK(!graphics.Matches(LogGraphicsX));
    CHECK(!graphics.Matches(LogAudio));

    const LogCategoryPrefix d3d12 = LogGraphicsD3D12;
    CHECK(d3d12.Matches(LogGraphicsD3D12) && !d3d12.Matches(LogGraphics));

    // 해시가 같아도 이름이 다르면 일치하지 않는다. 64-bit 해시 충돌을 실제로 찾는 대신 접두사 칸을
    // 직접 채워 "Graphics"의 해시를 가진 다른 이름을 만든다.
    LogCategoryPrefix forgedAudio;
    forgedAudio.name = "Audio";
    forgedAudio.hash = LogGraphics.NameHash();
    forgedAudio.depth = 1;
    CHECK(!forgedAudio.Matches(LogGraphics) && !forgedAudio.Matches(LogGraphicsD3D12));
    // 해시를 맞춘 바이트 접두사("Graph")도 구간 이름이 다르므로 일치하지 않는다.
    LogCategoryPrefix forgedGraph;
    forgedGraph.name = "Graph";
    forgedGraph.hash = LogGraphics.NameHash();
    forgedGraph.depth = 1;
    CHECK(!forgedGraph.Matches(LogGraphics) && !forgedGraph.Matches(LogGraphicsD3D12));
    // 반대로 이름은 맞지만 해시가 다르면 해시 단계에서 거른다.
    LogCategoryPrefix wrongHash;
    wrongHash.name = "Graphics";
    wrongHash.hash = LogAudio.NameHash();
    wrongHash.depth = 1;
    CHECK(!wrongHash.Matches(LogGraphics));
    // 접두사가 카테고리보다 깊으면 일치하지 않는다.
    CHECK(!LogCategoryPrefix(LogDeepest).Matches(LogGraphics));

    // 설정에서 온 소문자 접두사도 일치한다(해시·이름 비교 모두 ASCII 대소문자 무시).
    LogCategoryPrefix configured;
    configured.name = "graphics";
    configured.hash = HashLogCategoryName("graphics");
    configured.depth = 1;
    CHECK(configured.Matches(LogGraphics) && configured.Matches(LogGraphicsD3D12));
    CHECK(!configured.Matches(LogGraphicsX) && !configured.Matches(LogAudio));
}

TEST(LogCategoryNameValidation) {
    CHECK(ValidateLogCategoryName("Graphics.D3D12") == LogCategoryNameError::None);
    CHECK(ValidateLogCategoryName("") == LogCategoryNameError::EmptyName);
    CHECK(ValidateLogCategoryName(".A") == LogCategoryNameError::EmptySegment);
    CHECK(ValidateLogCategoryName("A.") == LogCategoryNameError::EmptySegment);
    CHECK(ValidateLogCategoryName("A..B") == LogCategoryNameError::EmptySegment);
    CHECK(ValidateLogCategoryName("A-B") == LogCategoryNameError::InvalidCharacter);
    CHECK(ValidateLogCategoryName("A\nB") == LogCategoryNameError::InvalidCharacter);
    CHECK(ValidateLogCategoryName("A.B.C.D.E.F.G.H.I") == LogCategoryNameError::TooManySegments);
    char tooLong[MaxLogCategoryNameBytes + 2]{};
    for (usize index = 0; index + 1 < sizeof(tooLong); ++index) {
        tooLong[index] = 'L';
    }

    CHECK(ValidateLogCategoryName(tooLong) == LogCategoryNameError::NameTooLong);
    CHECK(ValidateLogCategoryName(LogLongest.Name()) == LogCategoryNameError::None);
}

TEST(LogSystemFiltersByCategoryPrefix) {
    SyncLogs setup;
    CaptureSink graphics;
    CaptureSink d3d12;
    CaptureSink forged;
    REQUIRE(setup.logs.AddSink(graphics, {LogLevel::Trace, LogGraphics}).IsOk());
    REQUIRE(setup.logs.AddSink(d3d12, {LogLevel::Trace, LogGraphicsD3D12}).IsOk());
    LogCategoryPrefix forgedAudio;
    forgedAudio.name = "Audio";
    forgedAudio.hash = LogGraphics.NameHash();
    forgedAudio.depth = 1;
    REQUIRE(setup.logs.AddSink(forged, {LogLevel::Trace, forgedAudio}).IsOk());
    LogInstallScope installed(setup.logs);

    Log::Info(LogGraphics, "parent");
    CHECK(graphics.count == 1 && d3d12.count == 0 && graphics.LastCategory() == "Graphics");
    Log::Info(LogGraphicsD3D12, "child");
    CHECK(graphics.count == 2 && d3d12.count == 1 && d3d12.LastCategory() == "Graphics.D3D12");
    Log::Info(LogGraphicsX, "sibling");
    Log::Info(LogAudio, "other");
    CHECK(graphics.count == 2 && d3d12.count == 1);
    // 위조 접두사는 Graphics 해시를 가졌지만 이름이 다르고, Audio 기록과는 해시가 달라 아무것도
    // 받지 않는다(출력 쪽도 이름의 해시와 이름을 함께 본다).
    CHECK(forged.count == 0);
}

// 어떤 필터도 Fatal을 막지 않는다(설계 7절). 카테고리 접두사가 다른 필터도 Fatal은 통과시킨다.
TEST(LogFilterAlwaysAcceptsFatal) {
    const LogFilter audioOnly{LogLevel::Trace, LogAudio};
    CHECK(!audioOnly.Accepts(MakeEvent(LogLevel::Error, "Graphics", "blocked")));
    CHECK(audioOnly.Accepts(MakeEvent(LogLevel::Fatal, "Graphics", "fatal")));
    CHECK(audioOnly.Accepts(MakeEvent(LogLevel::Info, "Audio", "accepted")));
    const LogFilter errorsOnly{LogLevel::Error, {}};
    CHECK(!errorsOnly.Accepts(MakeEvent(LogLevel::Warning, "Audio", "quiet")));
    CHECK(errorsOnly.Accepts(MakeEvent(LogLevel::Fatal, "Audio", "loud")));
    // 이름으로 거를 때도 구간 경계와 대소문자 무시를 지킨다.
    LogCategoryPrefix configured;
    configured.name = "graphics";
    configured.hash = HashLogCategoryName("graphics");
    configured.depth = 1;
    CHECK(configured.MatchesName("Graphics.D3D12") && configured.MatchesName("GRAPHICS"));
    CHECK(!configured.MatchesName("GraphicsX") && !configured.MatchesName("Audio.Graphics"));
    CHECK(LogCategoryPrefix(LogGraphicsD3D12).MatchesName("Graphics.D3D12.Validation"));
    CHECK(!LogCategoryPrefix(LogGraphicsD3D12).MatchesName("Graphics"));
}

// TextLogSink는 손으로 만든 LogEvent의 카테고리도 문법으로 검사한다. 줄바꿈을 넣어 가짜 기록 줄을
// 만들거나 접두사 버퍼를 넘기는 이름은 출력 전에 거부한다.
TEST(TextLogSinkRejectsInvalidCategoryNames) {
    TextCapture output;
    TextLogSink sink({&output, TextCapture::Write, TextCapture::Flush});
    LogEvent event;
    event.level = LogLevel::Info;
    event.message = "m";

    char tooLong[MaxLogCategoryNameBytes + 1]{};
    for (usize index = 0; index < sizeof(tooLong); ++index) {
        tooLong[index] = 'L';
    }

    event.category = StringView(tooLong, sizeof(tooLong));
    CHECK(sink.Write(event).Error() == ErrorCode::InvalidArgument);
    event.category = "Tests\n[Error][Forged] injected";
    CHECK(sink.Write(event).Error() == ErrorCode::InvalidArgument);
    const char invalidUtf8[] = {'T', static_cast<char>(0xff)};
    event.category = StringView(invalidUtf8, sizeof(invalidUtf8));
    CHECK(sink.Write(event).Error() == ErrorCode::InvalidArgument);
    event.category = StringView();
    CHECK(sink.Write(event).Error() == ErrorCode::InvalidArgument);
    CHECK(output.count == 0 && output.writes == 0);

    event.category = StringView(tooLong, MaxLogCategoryNameBytes);
    CHECK(sink.Write(event).IsOk());
    CHECK(output.count == sizeof("[Info][] m\n") - 1 + MaxLogCategoryNameBytes);
}

TEST(SourceLocationCapturesCaller) {
    const CapturedCall first = CallFromFirstFunction();
    const CapturedCall second = CallFromSecondFunction();
    CHECK(first.where.line == first.line && second.where.line == second.line);
    CHECK(first.where.line != second.where.line);
    CHECK(StringView(first.where.function) == "CallFromFirstFunction");
    CHECK(StringView(second.where.function) == "CallFromSecondFunction");
    CHECK(StringView(first.where.file).EndsWith("LoggerTests.cpp"));
    CHECK(StringView(second.where.file) == StringView(first.where.file));

    // 본문에서 직접 부르면 그 줄이다.
    const CapturedCall here = {SourceLocation::Current(), __LINE__};
    CHECK(here.where.line == here.line);
    CHECK(StringView(here.where.function) == "RocksTest_SourceLocationCapturesCaller");

    // 기본 생성 값은 모르는 위치다.
    const SourceLocation unknown;
    CHECK(StringView(unknown.file).IsEmpty() && StringView(unknown.function).IsEmpty() &&
          unknown.line == 0);
}

TEST(LogSystemPassesCallerLocation) {
    SyncLogs setup;
    CaptureSink capture;
    REQUIRE(setup.logs.AddSink(capture).IsOk());
    LogInstallScope installed(setup.logs);
    Log::Info(LogTests, "where");
    const uint32 writeLine = __LINE__ - 1;
    CHECK(capture.lastLocation.line == writeLine);
    CHECK(StringView(capture.lastLocation.file).EndsWith("LoggerTests.cpp"));
    CHECK(StringView(capture.lastLocation.function) == "RocksTest_LogSystemPassesCallerLocation");
}

TEST(LogLevelRoundTripsThroughSinkFilterAndBuffer) {
    const LogLevel levels[] = {LogLevel::Trace,   LogLevel::Debug, LogLevel::Info,
                               LogLevel::Warning, LogLevel::Error, LogLevel::Fatal};
    const char* const names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Fatal"};
    static_assert(sizeof(levels) / sizeof(levels[0]) == LogLevelCount,
                  "every LogLevel must be covered");
    CHECK(!IsValidLogLevel(static_cast<LogLevel>(LogLevelCount)));

    TextCapture output;
    TextLogSink text({&output, TextCapture::Write, TextCapture::Flush});
    LogBuffer buffer;
    const LogFilter all{};
    for (usize index = 0; index < LogLevelCount; ++index) {
        const LogLevel level = levels[index];
        CHECK(IsValidLogLevel(level) && static_cast<usize>(level) == index);
        CHECK(LogLevelName(level) == names[index]);
        CHECK(all.Accepts(MakeEvent(level, "Tests", "m")));

        output = {};
        REQUIRE(text.Write(MakeEvent(level, "Tests", "m")).IsOk());
        char expected[32]{};
        usize expectedCount = 0;
        const auto append = [&](const char* piece) noexcept {
            for (usize at = 0; piece[at] != '\0'; ++at) {
                expected[expectedCount++] = piece[at];
            }
        };

        append("[");
        append(names[index]);
        append("][Tests] m\n");
        CHECK(StringView(output.bytes, output.count) == StringView(expected, expectedCount));

        REQUIRE(buffer.Write(level, 0, "m").IsOk());
        REQUIRE(buffer.At(index) != nullptr);
        CHECK(buffer.At(index)->level == level);
    }

    // Log 진입 함수로 남긴 기록도 레벨이 그대로 sink까지 간다(컴파일에 포함된 레벨만).
    SyncLogs setup;
    CaptureSink capture;
    REQUIRE(setup.logs.AddSink(capture).IsOk());
    LogInstallScope installed(setup.logs);
    if constexpr (LogLevel::Trace >= LogCompiledMinimum) {
        Log::Trace(LogTests, "m");
        CHECK(capture.lastLevel == LogLevel::Trace);
    }

    if constexpr (LogLevel::Debug >= LogCompiledMinimum) {
        Log::Debug(LogTests, "m");
        CHECK(capture.lastLevel == LogLevel::Debug);
    }

    Log::Info(LogTests, "m");
    CHECK(capture.lastLevel == LogLevel::Info);
    Log::Warning(LogTests, "m");
    CHECK(capture.lastLevel == LogLevel::Warning);
    Log::Error(LogTests, "m");
    CHECK(capture.lastLevel == LogLevel::Error);
}

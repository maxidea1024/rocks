#include <TestSupport/Test.h>
#include <Core/Diagnostics/LogSink.h>
#include <Core/Diagnostics/Logger.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>

using namespace rocks;

namespace {
constexpr LogCategory LogTests{"Tests"};
constexpr LogCategory LogTestsAlpha{"Tests.Alpha"};
constexpr LogCategory LogTestsBeta{"Tests.Beta"};
constexpr LogCategory LogTestsLogger{"Tests.Logger"};

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
    Logger* reenter = nullptr;
    ErrorCode nestedWrite = ErrorCode::None;
    ErrorCode nestedRemove = ErrorCode::None;
    ErrorCode nestedAdd = ErrorCode::None;
    ErrorCode nestedFilter = ErrorCode::None;
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
        if (reenter != nullptr) {
            nestedWrite = reenter->Write(LogLevel::Info, LogTests, "nested").Error();
            nestedRemove = reenter->Remove(*this).Error();
            nestedAdd = reenter->Add(*this).Error();
            nestedFilter = reenter->SetFilter(*this, {}).Error();
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

void Join(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Logger test worker did not finish");
    }
}

struct WriterContext {
    Logger* logger;
    uint32 failures = 0;
};

void WriteMany(void* raw) noexcept {
    auto& context = *static_cast<WriterContext*>(raw);
    for (uint32 index = 0; index < 400; ++index) {
        if (!context.logger->Write(LogLevel::Info, LogTestsAlpha, "worker").IsOk()) {
            ++context.failures;
        }
    }
}

struct BlockingSink final : RuntimeLogSink {
    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
    Result<void> Write(const LogEvent&) noexcept override {
        entered.Signal();
        if (release.Wait(10000) != platform::WaitResult::Signaled) {
            platform::Fatal("Logger test callback did not release");
        }
        return Result<void>::Success();
    }
};

struct RemovalContext {
    RemovalContext(Logger& owner, BlockingSink& target) noexcept : logger(&owner), sink(&target) {
    }

    Logger* logger;
    BlockingSink* sink;
    platform::ManualResetEvent started;
    platform::ManualResetEvent finished;
    bool removed = false;
};

void WriteBlocking(void* raw) noexcept {
    auto& context = *static_cast<RemovalContext*>(raw);
    CHECK(context.logger->Write(LogLevel::Info, LogTests, "blocking").IsOk());
}

void RemoveBlocking(void* raw) noexcept {
    auto& context = *static_cast<RemovalContext*>(raw);
    context.started.Signal();
    context.removed = context.logger->Remove(*context.sink).IsOk();
    context.finished.Signal();
}
} // namespace

TEST(RuntimeLoggerFiltersFanoutAndOwnedBytes) {
    Logger logger;
    CaptureSink first;
    CaptureSink second;
    REQUIRE(logger.Add(first).IsOk());
    REQUIRE(logger.Add(second, {LogLevel::Warning, LogTestsAlpha}).IsOk());
    CHECK(logger.Add(first).Error() == ErrorCode::AlreadyExists);
    CHECK(logger.Write(LogLevel::Info, LogTestsAlpha, "info").IsOk());
    CHECK(first.count == 1 && second.count == 0);
    const char embedded[] = {'A', '\0', 'B'};
    REQUIRE(logger.Write(LogLevel::Warning, LogTestsAlpha, StringView(embedded, sizeof(embedded)))
                .IsOk());
    CHECK(first.count == 2 && second.count == 1 && second.byteCount == 3);
    CHECK(second.bytes[0] == 'A' && second.bytes[1] == '\0' && second.bytes[2] == 'B');
    CHECK(second.LastCategory() == "Tests.Alpha" && second.lastLevel == LogLevel::Warning &&
          second.lastThread != 0 && second.lastTime != 0);
    CHECK(logger.Write(LogLevel::Error, LogTestsBeta, "other").IsOk());
    CHECK(second.count == 1);
    REQUIRE(logger.SetFilter(second, {LogLevel::Error, {}}).IsOk());
    CHECK(logger.Write(LogLevel::Warning, LogTestsAlpha, "filtered").IsOk());
    CHECK(logger.Write(LogLevel::Error, LogTestsBeta, "accepted").IsOk());
    CHECK(second.count == 2 && second.LastCategory() == "Tests.Beta");
    CHECK(logger.Remove(second).IsOk());
    CHECK(logger.Remove(second).Error() == ErrorCode::NotFound);
    CHECK(logger.SetFilter(second, {}).Error() == ErrorCode::NotFound);
    CHECK(logger.Remove(first).IsOk());
    CHECK(logger.Write(LogLevel::Info, LogTests, "no sinks").IsOk());
}

TEST(RuntimeLoggerRejectsInvalidInputAndReportsSinkFailure) {
    Logger logger;
    CaptureSink failing;
    CaptureSink succeeding;
    CaptureSink invalidFilter;
    failing.fail = true;
    uint32 order[2]{};
    uint32 orderCount = 0;
    failing.order = succeeding.order = order;
    failing.orderCount = succeeding.orderCount = &orderCount;
    failing.id = 1;
    succeeding.id = 2;
    CHECK(logger.Add(invalidFilter, {static_cast<LogLevel>(255), {}}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(logger.Add(invalidFilter, {static_cast<LogLevel>(LogLevelCount), {}}).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(logger.Add(failing).IsOk());
    REQUIRE(logger.Add(succeeding).IsOk());
    const char invalid[] = {static_cast<char>(0xff)};
    CHECK(logger.Write(LogLevel::Info, LogTests, StringView(invalid, 1)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(logger.Write(static_cast<LogLevel>(255), LogTests, "invalid").Error() ==
          ErrorCode::InvalidArgument);
    CHECK(logger.Write(static_cast<LogLevel>(LogLevelCount), LogTests, "invalid").Error() ==
          ErrorCode::InvalidArgument);
    CHECK(logger.SetFilter(succeeding, {static_cast<LogLevel>(255), {}}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(failing.count == 0 && succeeding.count == 0);
    CHECK(logger.Write(LogLevel::Error, LogTests, "failure").Error() ==
          ErrorCode::DependencyFailed);
    CHECK(failing.count == 1 && succeeding.count == 1);
    CHECK(orderCount == 2 && order[0] == 1 && order[1] == 2);
    CHECK(logger.Remove(failing).IsOk());
    CHECK(logger.Remove(succeeding).IsOk());
}

TEST(RuntimeLoggerCapacityAndReentry) {
    Logger logger;
    Logger another;
    CaptureSink sinks[Logger::MaxSinks + 1];
    for (usize index = 0; index < Logger::MaxSinks; ++index) {
        REQUIRE(logger.Add(sinks[index]).IsOk());
    }
    CHECK(logger.Add(sinks[Logger::MaxSinks]).Error() == ErrorCode::CapacityExceeded);
    sinks[0].reenter = &logger;
    CHECK(logger.Write(LogLevel::Info, LogTests, "outer").IsOk());
    CHECK(sinks[0].nestedWrite == ErrorCode::Busy && sinks[0].nestedRemove == ErrorCode::Busy &&
          sinks[0].nestedAdd == ErrorCode::Busy && sinks[0].nestedFilter == ErrorCode::Busy);
    sinks[0].reenter = &another;
    CHECK(logger.Write(LogLevel::Info, LogTests, "other logger").IsOk());
    CHECK(sinks[0].nestedWrite == ErrorCode::Busy && sinks[0].nestedRemove == ErrorCode::Busy &&
          sinks[0].nestedAdd == ErrorCode::Busy && sinks[0].nestedFilter == ErrorCode::Busy);
    for (usize index = 0; index < Logger::MaxSinks; ++index) {
        CHECK(logger.Remove(sinks[index]).IsOk());
    }
    CHECK(logger.Add(sinks[Logger::MaxSinks]).IsOk());
    CHECK(logger.Remove(sinks[Logger::MaxSinks]).IsOk());
}

TEST(RuntimeLoggerTextOutputAndFailures) {
    TextCapture output;
    TextLogSink sink({&output, TextCapture::Write, TextCapture::Flush});
    Logger logger;
    REQUIRE(logger.Add(sink).IsOk());
    const char message[] = {static_cast<char>(0xed), static_cast<char>(0x95),
                            static_cast<char>(0x9c), '\0', 'X'};
    REQUIRE(logger.Write(LogLevel::Warning, LogTestsLogger, StringView(message, sizeof(message)))
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
    CHECK(logger.Write(LogLevel::Info, LogTests, "x").Error() == ErrorCode::DependencyFailed);
    CHECK(output.writes == 2 && output.flushes == 0);
    output = {};
    output.failFlush = true;
    CHECK(logger.Write(LogLevel::Info, LogTests, "x").Error() == ErrorCode::DependencyFailed);
    CHECK(output.writes == 3 && output.flushes == 1);
    CHECK(logger.Remove(sink).IsOk());
}

// 외부 프로세스가 stderr 원시 byte를 캡처하여 한글과 NUL까지 확인할 수 있는 전용 probe다.
TEST(RuntimeLoggerStderrOutputProbe) {
    Logger logger;
    StderrLogSink stderrSink;
    REQUIRE(logger.Add(stderrSink).IsOk());
    const char message[] = {static_cast<char>(0xed), static_cast<char>(0x95),
                            static_cast<char>(0x9c), '\0', 'X'};
    CHECK(logger.Write(LogLevel::Warning, LogTestsLogger, StringView(message, sizeof(message)))
              .IsOk());
    CHECK(logger.Remove(stderrSink).IsOk());
}

TEST(RuntimeLoggerConcurrentWritesAndRemovalWaitsForCallback) {
    Logger logger;
    CaptureSink capture;
    REQUIRE(logger.Add(capture).IsOk());
    platform::Thread workers[4];
    WriterContext contexts[4] = {{&logger}, {&logger}, {&logger}, {&logger}};
    for (usize index = 0; index < 4; ++index) {
        if (workers[index].Start(WriteMany, &contexts[index]) != platform::ThreadStatus::Success) {
            platform::Fatal("Logger test worker start failed");
        }
    }
    for (usize index = 0; index < 4; ++index) {
        Join(workers[index]);
        CHECK(contexts[index].failures == 0);
    }
    CHECK(capture.count == 1600);
    REQUIRE(logger.Remove(capture).IsOk());

    BlockingSink blocking;
    RemovalContext context(logger, blocking);
    REQUIRE(logger.Add(blocking).IsOk());
    platform::Thread writer;
    platform::Thread remover;
    if (writer.Start(WriteBlocking, &context) != platform::ThreadStatus::Success ||
        blocking.entered.Wait(10000) != platform::WaitResult::Signaled ||
        remover.Start(RemoveBlocking, &context) != platform::ThreadStatus::Success ||
        context.started.Wait(10000) != platform::WaitResult::Signaled) {
        platform::Fatal("Logger test setup failed");
    }
    CHECK(context.finished.Wait(20) == platform::WaitResult::TimedOut);
    blocking.release.Signal();
    Join(writer);
    Join(remover);
    CHECK(context.removed && context.finished.Wait(0) == platform::WaitResult::Signaled);
    CHECK(logger.Write(LogLevel::Info, LogTests, "after removal").IsOk());
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
           LogGraphicsD3D12.PrefixHash(0) == HashStringBytes("Graphics") &&
           LogGraphicsD3D12.PrefixHash(1) == HashStringBytes("Graphics.D3D12") &&
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
    CHECK(LogGraphicsD3D12.PrefixName(2).IsEmpty() && LogGraphicsD3D12.PrefixHash(2) == 0);
    CHECK(LogGraphics.PrefixCount() == 1 && LogGraphics.Name() == "Graphics");

    // consteval로 미리 계산한 해시가 런타임 문자열 해시와 같아야 후속 재정의 표(런타임 이름)와
    // 맞물린다. 상수 접기를 피하려고 런타임 버퍼에서 해시한다.
    char runtimeName[] = "Graphics.D3D12";
    CHECK(HashStringBytes(StringView(runtimeName, sizeof("Graphics") - 1)) ==
          LogGraphicsD3D12.PrefixHash(0));
    CHECK(HashStringBytes(StringView(runtimeName, sizeof(runtimeName) - 1)) ==
          LogGraphicsD3D12.NameHash());

    const char* const deepestPrefixes[] = {
        "A",         "A.B",         "A.B.C",         "A.B.C.D",
        "A.B.C.D.E", "A.B.C.D.E.F", "A.B.C.D.E.F.G", "A.B.C.D.E.F.G.H"};
    for (usize index = 0; index < MaxLogCategoryDepth; ++index) {
        CHECK(LogDeepest.PrefixName(index) == deepestPrefixes[index]);
        CHECK(LogDeepest.PrefixHash(index) == HashStringBytes(deepestPrefixes[index]));
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
}

TEST(RuntimeLoggerFiltersByCategoryPrefix) {
    Logger logger;
    CaptureSink graphics;
    CaptureSink d3d12;
    CaptureSink forged;
    REQUIRE(logger.Add(graphics, {LogLevel::Trace, LogGraphics}).IsOk());
    REQUIRE(logger.Add(d3d12, {LogLevel::Trace, LogGraphicsD3D12}).IsOk());
    LogCategoryPrefix forgedAudio;
    forgedAudio.name = "Audio";
    forgedAudio.hash = LogGraphics.NameHash();
    forgedAudio.depth = 1;
    REQUIRE(logger.Add(forged, {LogLevel::Trace, forgedAudio}).IsOk());

    CHECK(logger.Write(LogLevel::Info, LogGraphics, "parent").IsOk());
    CHECK(graphics.count == 1 && d3d12.count == 0 && graphics.LastCategory() == "Graphics");
    CHECK(logger.Write(LogLevel::Info, LogGraphicsD3D12, "child").IsOk());
    CHECK(graphics.count == 2 && d3d12.count == 1 && d3d12.LastCategory() == "Graphics.D3D12");
    CHECK(logger.Write(LogLevel::Info, LogGraphicsX, "sibling").IsOk());
    CHECK(logger.Write(LogLevel::Info, LogAudio, "other").IsOk());
    CHECK(graphics.count == 2 && d3d12.count == 1);
    // 위조 접두사는 Graphics 해시를 가졌지만 이름 비교에서 걸러져 아무것도 받지 않는다.
    CHECK(forged.count == 0);

    CHECK(logger.Remove(graphics).IsOk());
    CHECK(logger.Remove(d3d12).IsOk());
    CHECK(logger.Remove(forged).IsOk());
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

TEST(RuntimeLoggerPassesCallerLocation) {
    Logger logger;
    CaptureSink capture;
    REQUIRE(logger.Add(capture).IsOk());
    CHECK(logger.Write(LogLevel::Info, LogTests, "where").IsOk());
    const uint32 writeLine = __LINE__ - 1;
    CHECK(capture.lastLocation.line == writeLine);
    CHECK(StringView(capture.lastLocation.file).EndsWith("LoggerTests.cpp"));
    CHECK(logger.Remove(capture).IsOk());
}

TEST(LogLevelRoundTripsThroughLoggerTextSinkAndBuffer) {
    const LogLevel levels[] = {LogLevel::Trace,   LogLevel::Debug, LogLevel::Info,
                               LogLevel::Warning, LogLevel::Error, LogLevel::Fatal};
    const char* const names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Fatal"};
    static_assert(sizeof(levels) / sizeof(levels[0]) == LogLevelCount,
                  "every LogLevel must be covered");
    CHECK(!IsValidLogLevel(static_cast<LogLevel>(LogLevelCount)));

    Logger logger;
    CaptureSink capture;
    TextCapture output;
    TextLogSink text({&output, TextCapture::Write, TextCapture::Flush});
    REQUIRE(logger.Add(capture).IsOk());
    REQUIRE(logger.Add(text).IsOk());
    LogBuffer buffer;
    for (usize index = 0; index < LogLevelCount; ++index) {
        const LogLevel level = levels[index];
        CHECK(IsValidLogLevel(level) && static_cast<usize>(level) == index);

        // Fatal도 Logger에서는 일반 기록이다(종료하지 않는다).
        output = {};
        REQUIRE(logger.Write(level, LogTests, "m").IsOk());
        CHECK(capture.lastLevel == level);
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

    CHECK(capture.count == LogLevelCount);
    CHECK(logger.Remove(capture).IsOk());
    CHECK(logger.Remove(text).IsOk());
}

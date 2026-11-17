#include <TestSupport/Test.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

using namespace rocks;

namespace rocks::test {
uint64 OperatorNewCallsOnThisThread() noexcept;
} // namespace rocks::test

namespace {

constexpr LogCategory LogSystemTests{"Tests.System"};
constexpr LogCategory LogGraphics{"Graphics"};
constexpr LogCategory LogGraphicsD3D12{"Graphics.D3D12"};
constexpr LogCategory LogGraphicsVulkan{"Graphics.Vulkan"};
constexpr LogCategory LogAudio{"Audio"};

// 시험이 멈추지 않게 거는 안전 상한이다(정상 진행은 이벤트로 순서를 정하고 이 시간에 기대지
// 않는다).
constexpr uint32 SafetyTimeoutMilliseconds = 30000;

// 받은 기록을 "[레벨][카테고리] 메시지\n" 줄로 모은다(콜백 동안만 유효한 view를 복사한다).
class LineSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        ++records;
        lastFrame = event.frame;
        lastUtc = event.utcTicks;
        lastThreadNameBytes = event.threadName.ByteCount();
        for (usize index = 0; index < lastThreadNameBytes && index < sizeof(lastThreadName);
             ++index) {
            lastThreadName[index] = event.threadName.Data()[index];
        }

        if (captureWriterName) {
            writerNameBytes = platform::CopyCurrentThreadName(writerName, sizeof(writerName));
        }

        // 버퍼가 차면 더 모으지 않는다(FixedStringBuilder는 NUL 자리를 포함해 2 byte 이상 필요).
        if (sizeof(text) - bytes >= MinimumLogRenderBytes) {
            FixedStringBuilder line{Span<char>(text + bytes, sizeof(text) - bytes)};
            (void)line.TryAppendAscii('[');
            (void)line.TryAppend(LogLevelName(event.level));
            (void)line.TryAppend("][");
            (void)line.TryAppend(event.category);
            (void)line.TryAppend("] ");
            (void)line.TryAppend(event.message);
            (void)line.TryAppendAscii('\n');
            bytes += line.ByteCount();
        }
        const uint32 target = platform::AtomicLoad<platform::MemoryOrder::Acquire>(&signalAt);
        if (target != 0 && records >= target) {
            reached.Signal();
        }

        if (fail) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        return Result<void>::Success();
    }

    StringView Text() const noexcept {
        return StringView(text, bytes);
    }

    void Clear() noexcept {
        bytes = 0;
        records = 0;
    }

    char text[16384]{};
    usize bytes = 0;
    uint32 records = 0;
    uint64 lastFrame = 0;
    int64 lastUtc = 0;
    char lastThreadName[64]{};
    usize lastThreadNameBytes = 0;
    bool captureWriterName = false;
    char writerName[64]{};
    usize writerNameBytes = 0;
    bool fail = false;
    uint32 signalAt = 0;
    platform::ManualResetEvent reached;
};

// 모든 할당을 세는 allocator다(Start 뒤 0회 확인).
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

struct Wide {
    usize count = 0;
};

LogSystemConfig Config(LogMode mode) noexcept {
    LogSystemConfig config;
    config.mode = mode;
    config.globalMinimum = LogLevel::Trace;
    return config;
}

// Sync·Manual 비교용 기록 열이다.
void EmitSequence() noexcept {
    Log::Info(LogSystemTests, "hello {Name} {Count}", "world", 3);
    Log::Warning(LogGraphicsD3D12, "device {Index} lost: {Reason}", 0u, StringView("removed"));
    Log::Error(LogAudio, "{} + {} = {:>4}", 1, 2, 3);
    Log::Write(LogLevel::Info, LogSystemTests, RuntimeTemplate{StringView("runtime {A}")}, 9);
    Log::Write(LogLevel::Info, LogSystemTests, RuntimeTemplate{StringView("broken {A} {B}")}, 9);
}

// 앞선 시험이 설치 없이 남긴 낮은 레벨 기록의 버림 수를 비운다. 정확한 출력을 단언하는 시험은 먼저
// 부른다(그러지 않으면 설치 때 "버린 기록 수" 알림이 sink에 한 줄 더 들어간다).
void ResetUninstalledDrops() noexcept {
    SystemAllocator allocator;
    LogSystem reset(allocator, Config(LogMode::Manual));
    (void)reset.Start();
    LogInstallScope installed(reset);
}

void Join(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("log system test worker did not finish");
    }
}

} // namespace

namespace rocks {

template <>
struct Formatter<Wide> {
    static Result<void> Format(StringBuilderBase& out, const Wide& value, StringView) noexcept {
        return out.TryAppendRepeated('w', value.count);
    }
};

} // namespace rocks

TEST(LogSystemManualPumpRendersAndStamps) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Manual));
    LineSink sink;
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    CHECK(logs.Start().Error() == ErrorCode::InvalidArgument);
    LogInstallScope installed(logs);
    logs.SetFrame(42);
    REQUIRE(logs.SetThreadName("Main").IsOk());
    Log::Info(LogSystemTests, "hello {Name} {Count}", "world", 3);
    CHECK(sink.records == 0); // Manual: Pump 전에는 sink에 가지 않는다
    CHECK(logs.Pump() == 1);
    CHECK(sink.Text() == "[Info][Tests.System] hello world 3\n");
    CHECK(sink.lastFrame == 42 && sink.lastUtc > 0);
    CHECK(StringView(sink.lastThreadName, sink.lastThreadNameBytes) == "Main");
    CHECK(logs.Pump() == 0);
    CHECK(logs.Stats().written == 1);
}

// 같은 기록 열은 Sync와 Manual에서 같은 출력을 낸다(같은 렌더·fan-out 경로).
TEST(LogSystemSyncAndManualProduceIdenticalOutput) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LineSink syncSink;
    LineSink manualSink;
    {
        LogSystem logs(allocator, Config(LogMode::Sync));
        REQUIRE(logs.AddSink(syncSink).IsOk());
        REQUIRE(logs.Start().IsOk());
        LogInstallScope installed(logs);
        EmitSequence();
    }

    {
        LogSystem logs(allocator, Config(LogMode::Manual));
        REQUIRE(logs.AddSink(manualSink).IsOk());
        REQUIRE(logs.Start().IsOk());
        LogInstallScope installed(logs);
        EmitSequence();
        CHECK(logs.Pump() == 5);
    }

    CHECK(syncSink.records == 5 && manualSink.records == 5);
    CHECK(syncSink.Text() == manualSink.Text());
    CHECK(syncSink.Text() ==
          "[Info][Tests.System] hello world 3\n"
          "[Warning][Graphics.D3D12] device 0 lost: removed\n"
          "[Error][Audio] 1 + 2 =    3\n"
          "[Info][Tests.System] runtime 9\n"
          "[Info][Tests.System] broken {A} {B} [template error: ArgumentCountMismatch]\n");
}

// Async: Flush는 그 전에 넣은 기록이 모두 sink에 도달할 때까지 기다린다. 출력 스레드 이름은
// "LogWriter"다.
TEST(LogSystemAsyncFlushDeliversEverything) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Async));
    LineSink sink;
    sink.captureWriterName = true;
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    {
        LogInstallScope installed(logs);
        for (uint32 index = 0; index < 1000; ++index) {
            Log::Info(LogSystemTests, "record {Index}", index);
        }

        CHECK(logs.Flush(SafetyTimeoutMilliseconds));
        CHECK(sink.records == 1000);
    }

    logs.Stop();
    CHECK(StringView(sink.writerName, sink.writerNameBytes) == "LogWriter");
}

// 깨우기 유실이 없다: 생산자는 Flush 없이 기록 하나만 넣고, sink가 받았다는 이벤트를 기다린다. 잠든
// 출력 스레드를 깨우는 신호가 한 번이라도 사라지면 이 대기가 안전 상한까지 걸린다.
TEST(LogSystemAsyncWakeIsNeverLost) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Async));
    LineSink sink;
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    LogInstallScope installed(logs);
    bool delivered = true;
    for (uint32 index = 1; index <= 2000 && delivered; ++index) {
        sink.reached.Reset();
        platform::AtomicStore<platform::MemoryOrder::Release>(&sink.signalAt, index);
        Log::Info(LogSystemTests, "wake {Index}", index);
        delivered = sink.reached.Wait(SafetyTimeoutMilliseconds) == platform::WaitResult::Signaled;
    }

    CHECK(delivered);
}

// 포화: 사용량이 90% 이상이면 Info 이하를 버리고, 남은 칸은 Warning 이상이 쓴다. 공간이 돌아오면
// 버린 수를 한 줄로 알린다.
TEST(LogSystemSaturationKeepsWarningReserveAndReportsDrops) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystemConfig config = Config(LogMode::Manual);
    config.queueSlots = 16; // 90%는 14칸
    LogSystem logs(allocator, config);
    LineSink sink;
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    LogInstallScope installed(logs);
    for (uint32 index = 0; index < 20; ++index) {
        Log::Info(LogSystemTests, "info {Index}", index);
    }

    Log::Warning(LogSystemTests, "warning {Index}", 1);
    Log::Warning(LogSystemTests, "warning {Index}", 2);
    Log::Warning(LogSystemTests, "warning {Index}", 3); // 큐가 가득 참
    const LogSystemStats before = logs.Stats();
    CHECK(before.dropped[static_cast<usize>(LogLevel::Info)] == 6);
    CHECK(before.dropped[static_cast<usize>(LogLevel::Warning)] == 1);
    CHECK(logs.Pump() == 16);
    CHECK(sink.records == 17);
    CHECK(sink.Text().EndsWith(
        "[Warning][Log] 7 log records dropped (0 Trace, 0 Debug, 6 Info, 1 Warning, 0 Error)\n",
        StringComparison::CaseSensitive));
    CHECK(sink.Text().Contains("[Warning][Tests.System] warning 2\n",
                               StringComparison::CaseSensitive));
}

// sink가 연속 LogSinkFailureLimit번 실패하면 막고, LogSinkRetryBatches 묶음마다 다시 시도한다. 다른
// sink는 계속 받는다.
TEST(LogSystemBlocksFailingSinkAndRetries) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Manual));
    LineSink failing;
    LineSink healthy;
    failing.fail = true;
    REQUIRE(logs.AddSink(failing).IsOk());
    REQUIRE(logs.AddSink(healthy).IsOk());
    REQUIRE(logs.Start().IsOk());
    LogInstallScope installed(logs);
    for (uint32 index = 0; index < 10; ++index) {
        Log::Info(LogSystemTests, "record {Index}", index);
    }

    CHECK(logs.Pump() == 10);
    CHECK(failing.records == LogSinkFailureLimit && healthy.records == 10);
    CHECK(logs.Stats().sinkFailures == LogSinkFailureLimit && logs.Stats().sinkBlocks == 1);

    failing.fail = false;
    for (uint32 batch = 0; batch < LogSinkRetryBatches; ++batch) {
        (void)logs.Pump(); // 빈 묶음도 다시 시도까지의 묶음 수에 든다
    }

    Log::Info(LogSystemTests, "after retry");
    CHECK(logs.Pump() == 1);
    CHECK(failing.records == LogSinkFailureLimit + 1);
    CHECK(failing.Text().EndsWith("[Info][Tests.System] after retry\n",
                                  StringComparison::CaseSensitive));
}

namespace {

// 콜백 안에서 로그·등록·Flush를 시도한다.
class ReentrantSink final : public RuntimeLogSink {
public:
    explicit ReentrantSink(LogSystem& owner) noexcept : logs(owner) {
    }

    Result<void> Write(const LogEvent&) noexcept override {
        ++records;
        Log::Error(LogSystemTests, "logged from a sink callback");
        added = logs.AddSink(other).Error();
        removed = logs.RemoveSink(*this).Error();
        flushed = logs.Flush(0);
        return Result<void>::Success();
    }

    LogSystem& logs;
    LineSink other;
    uint32 records = 0;
    ErrorCode added = ErrorCode::None;
    ErrorCode removed = ErrorCode::None;
    bool flushed = true;
};

} // namespace

// sink 콜백 안의 로그는 큐에 들어가지 않고 비상 경로로만 간다(재귀 없음). 콜백 안의 등록·해제는
// Busy, Flush는 false다.
TEST(LogSystemSinkReentryGoesToEmergencyPath) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Manual));
    ReentrantSink sink(logs);
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    LogInstallScope installed(logs);
    Log::Info(LogSystemTests, "outer");
    CHECK(logs.Pump() == 1);
    CHECK(logs.Pump() == 0); // 콜백 안의 로그는 큐에 들어가지 않았다
    CHECK(sink.records == 1);
    CHECK(sink.added == ErrorCode::Busy && sink.removed == ErrorCode::Busy && !sink.flushed);
    CHECK(logs.Stats().reentered == 1);
}

TEST(LogSystemSinkRegistrationErrors) {
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Manual));
    LineSink sinks[MaxLogSinks + 1];
    for (usize index = 0; index < MaxLogSinks; ++index) {
        REQUIRE(logs.AddSink(sinks[index]).IsOk());
    }

    CHECK(logs.AddSink(sinks[0]).Error() == ErrorCode::AlreadyExists);
    CHECK(logs.AddSink(sinks[MaxLogSinks]).Error() == ErrorCode::CapacityExceeded);
    CHECK(logs.RemoveSink(sinks[MaxLogSinks]).Error() == ErrorCode::NotFound);
    LineSink invalid;
    REQUIRE(logs.RemoveSink(sinks[0]).IsOk());
    CHECK(logs.AddSink(invalid, {static_cast<LogLevel>(LogLevelCount), {}}).Error() ==
          ErrorCode::InvalidArgument);
}

// 레벨 우선순위: 카테고리 재정의(점 접두사 최장 일치) → 전역 최소. 재정의 이름은 ASCII 대소문자를
// 무시한다.
TEST(LogSystemLevelOverridesUseLongestPrefix) {
    SystemAllocator allocator;
    LogSystemConfig config = Config(LogMode::Manual);
    config.globalMinimum = LogLevel::Warning;
    LogSystem logs(allocator, config);
    CHECK(!logs.IsEnabled(LogLevel::Info, LogAudio));
    CHECK(logs.IsEnabled(LogLevel::Warning, LogAudio));
    CHECK(logs.IsEnabled(LogLevel::Fatal, LogAudio));

    REQUIRE(logs.SetLevel("Graphics", LogLevel::Debug).IsOk());
    REQUIRE(logs.SetLevel("Graphics.D3D12", LogLevel::Error).IsOk());
    CHECK(logs.IsEnabled(LogLevel::Debug, LogGraphicsVulkan));
    CHECK(logs.IsEnabled(LogLevel::Debug, LogGraphics));
    CHECK(!logs.IsEnabled(LogLevel::Warning, LogGraphicsD3D12));
    CHECK(logs.IsEnabled(LogLevel::Error, LogGraphicsD3D12));
    CHECK(!logs.IsEnabled(LogLevel::Info, LogAudio));

    // 설정은 대소문자를 무시한다. 같은 칸을 바꾸며 이름 충돌로 보지 않는다.
    REQUIRE(logs.SetLevel("graphics", LogLevel::Trace).IsOk());
    CHECK(logs.IsEnabled(LogLevel::Trace, LogGraphicsVulkan));
    REQUIRE(logs.ClearLevel("GRAPHICS").IsOk());
    CHECK(!logs.IsEnabled(LogLevel::Info, LogGraphicsVulkan));
    CHECK(logs.ClearLevel("Graphics").Error() == ErrorCode::NotFound);
    CHECK(logs.SetLevel("Bad..Name", LogLevel::Info).Error() == ErrorCode::InvalidArgument);

    logs.SetGlobalMinimum(LogLevel::Info);
    CHECK(logs.IsEnabled(LogLevel::Info, LogAudio));

    // 표 용량을 넘기면 CapacityExceeded다.
    REQUIRE(logs.ClearLevel("Graphics.D3D12").IsOk());
    char name[8]{};
    usize filled = 0;
    for (usize index = 0; index < LogLevelOverrideCapacity; ++index) {
        name[0] = 'C';
        name[1] = static_cast<char>('a' + index / 26);
        name[2] = static_cast<char>('a' + index % 26);
        filled += logs.SetLevel(StringView(name, 3), LogLevel::Error).IsOk() ? 1 : 0;
    }

    CHECK(filled == LogLevelOverrideCapacity);
    CHECK(logs.SetLevel("Overflow", LogLevel::Error).Error() == ErrorCode::CapacityExceeded);
}

// Lazy: 꺼진 레벨이면 람다를 부르지 않고, 켜져 있으면 호출 중 정확히 한 번 부른다.
TEST(LogLazyRunsOnlyWhenEnabled) {
    if constexpr (LogLevel::Debug >= LogCompiledMinimum) {
        ResetUninstalledDrops();
        SystemAllocator allocator;
        LogSystemConfig config = Config(LogMode::Manual);
        config.globalMinimum = LogLevel::Info;
        LogSystem logs(allocator, config);
        LineSink sink;
        REQUIRE(logs.AddSink(sink).IsOk());
        REQUIRE(logs.Start().IsOk());
        LogInstallScope installed(logs);
        uint32 calls = 0;
        const auto build = [&](LogLine& line) noexcept {
            ++calls;
            line.Write("value {V}", 5);
            line.Write(" and {W}", "more");
        };

        Log::DebugLazy(LogSystemTests, build);
        CHECK(calls == 0);
        REQUIRE(logs.SetLevel("Tests", LogLevel::Debug).IsOk());
        Log::DebugLazy(LogSystemTests, build);
        CHECK(calls == 1);
        CHECK(logs.Pump() == 1);
        CHECK(sink.Text() == "[Debug][Tests.System] value 5 and more\n");
    }
}

// 설치는 중첩되고 끝나면 이전 대상으로 돌아간다. 설치 전에 버린 낮은 레벨 기록 수는 다음 설치 때 한
// 번 알린다.
TEST(LogInstallNestsAndReportsDropsBeforeInstall) {
    SystemAllocator allocator;
    {
        // 앞선 시험이 남긴 설치 전 버림 수를 비운다.
        LogSystem reset(allocator, Config(LogMode::Manual));
        REQUIRE(reset.Start().IsOk());
        LogInstallScope installed(reset);
    }

    Log::Info(LogSystemTests, "dropped {Index}", 1);
    Log::Info(LogSystemTests, "dropped {Index}", 2);
    Log::Debug(LogSystemTests, "dropped {Index}", 3);
    const uint32 expectedDrops = LogLevel::Debug >= LogCompiledMinimum ? 3 : 2;

    LogSystem outer(allocator, Config(LogMode::Manual));
    LogSystem inner(allocator, Config(LogMode::Manual));
    LineSink outerSink;
    LineSink innerSink;
    REQUIRE(outer.AddSink(outerSink).IsOk());
    REQUIRE(inner.AddSink(innerSink).IsOk());
    REQUIRE(outer.Start().IsOk());
    REQUIRE(inner.Start().IsOk());
    {
        LogInstallScope installedOuter(outer);
        CHECK(outer.Stats().droppedBeforeInstall == expectedDrops);
        {
            LogInstallScope installedInner(inner);
            Log::Info(LogSystemTests, "to inner");
        }

        Log::Info(LogSystemTests, "to outer");
    }

    (void)outer.Pump();
    (void)inner.Pump();
    CHECK(innerSink.Text() == "[Info][Tests.System] to inner\n");
    CHECK(outerSink.records == 2);
    CHECK(outerSink.Text().Contains("log records below Warning were dropped while no log system "
                                    "was installed",
                                    StringComparison::CaseSensitive));
    CHECK(outerSink.Text().EndsWith("[Info][Tests.System] to outer\n",
                                    StringComparison::CaseSensitive));
}

namespace {

constexpr uint32 LoggingThreads = 4;

struct ContinuousContext {
    uint32 stop = 0;
    platform::ManualResetEvent started[LoggingThreads];
    uint64 written[LoggingThreads]{};
};

struct ContinuousArgument {
    ContinuousContext* context = nullptr;
    uint32 index = 0;
};

void LogContinuously(void* raw) noexcept {
    auto& argument = *static_cast<ContinuousArgument*>(raw);
    ContinuousContext& context = *argument.context;
    while (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&context.stop) == 0) {
        Log::Info(LogSystemTests, "busy {Thread}", argument.index);
        if (++context.written[argument.index] == 1) {
            context.started[argument.index].Signal();
        }
    }
}

} // namespace

// 계속 로그가 들어와도 해제가 끝난다(두 세대 카운터: 새 호출은 새 세대로 가므로 옛 세대만
// 기다린다).
TEST(LogUninstallDoesNotStarveUnderContinuousLogging) {
    SystemAllocator allocator;
    LogSystemConfig config = Config(LogMode::Manual);
    config.queueSlots = 64;
    LogSystem logs(allocator, config);
    REQUIRE(logs.Start().IsOk());
    ContinuousContext context;
    ContinuousArgument arguments[LoggingThreads];
    platform::Thread threads[LoggingThreads];
    {
        LogInstallScope installed(logs);
        for (uint32 index = 0; index < LoggingThreads; ++index) {
            arguments[index] = {&context, index};
            if (threads[index].Start(LogContinuously, &arguments[index]) !=
                platform::ThreadStatus::Success) {
                platform::Fatal("logging thread start failed");
            }
        }

        for (platform::ManualResetEvent& started : context.started) {
            if (started.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
                platform::Fatal("logging thread did not start");
            }
        }
        // 범위가 끝나며 해제한다. 생산자들은 계속 로그를 남기는 중이다.
    }

    platform::AtomicStore<platform::MemoryOrder::Release>(&context.stop, 1u);
    for (platform::Thread& thread : threads) {
        Join(thread);
    }

    CHECK(logs.Stats().written == 0); // Manual이라 Pump 전에는 sink에 가지 않는다
    for (uint64 written : context.written) {
        CHECK(written != 0);
    }
}

namespace {

// Write 안에서 게이트를 기다리는 sink다(RemoveSink가 진행 중 콜백을 기다리는지 확인).
class GateSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent&) noexcept override {
        entered.Signal();
        if (release.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
            platform::Fatal("gate sink was never released");
        }

        platform::AtomicStore<platform::MemoryOrder::Release>(&finished, 1u);
        return Result<void>::Success();
    }

    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
    uint32 finished = 0;
};

struct RemovalContext {
    LogSystem* logs = nullptr;
    GateSink* sink = nullptr;
    platform::ManualResetEvent started;
    uint32 finishedWhenRemoved = 0;
    bool removed = false;
};

void RemoveGateSink(void* raw) noexcept {
    auto& context = *static_cast<RemovalContext*>(raw);
    context.started.Signal();
    context.removed = context.logs->RemoveSink(*context.sink).IsOk();
    context.finishedWhenRemoved =
        platform::AtomicLoad<platform::MemoryOrder::Acquire>(&context.sink->finished);
}

} // namespace

// RemoveSink는 진행 중인 콜백이 끝난 뒤 반환한다. 시간 대신 순서로 확인한다: RemoveSink가 반환했을
// 때 콜백의 마지막 쓰기(finished)가 이미 보여야 한다.
TEST(LogSystemRemoveSinkWaitsForCallback) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Async));
    GateSink sink;
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    LogInstallScope installed(logs);
    Log::Info(LogSystemTests, "gate");
    if (sink.entered.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
        platform::Fatal("gate sink was not entered");
    }

    RemovalContext context;
    context.logs = &logs;
    context.sink = &sink;
    platform::Thread remover;
    if (remover.Start(RemoveGateSink, &context) != platform::ThreadStatus::Success ||
        context.started.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
        platform::Fatal("remover did not start");
    }

    sink.release.Signal();
    Join(remover);
    CHECK(context.removed && context.finishedWhenRemoved == 1);
}

namespace {

class FailingSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent&) noexcept override {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
};

// 각 모드에서 Start 뒤의 기록 경로를 지난다: 내장 타입, Formatter, 큰 값(블록), 런타임 템플릿,
// 실패하는 sink, (Manual) 포화.
void ExerciseLogPaths(LogSystem& logs) noexcept {
    char longText[1500];
    for (usize index = 0; index < sizeof(longText) - 1; ++index) {
        longText[index] = 'L';
    }

    longText[sizeof(longText) - 1] = '\0';
    for (uint32 index = 0; index < 40; ++index) {
        Log::Info(LogSystemTests, "plain {Index} {Ratio:.2f}", index, 0.25);
        Log::Warning(LogSystemTests, "wide {Value}", Wide{600});
        Log::Error(LogSystemTests, "long {Text}", StringView(longText));
        Log::Write(LogLevel::Info, LogSystemTests, RuntimeTemplate{StringView(longText)});
    }

    (void)logs.Flush(SafetyTimeoutMilliseconds);
}

} // namespace

// Start 뒤에는 생산자·출력 쪽 모두 할당하지 않는다(세는 allocator와 이 스레드의 operator new).
TEST(LogSystemDoesNotAllocateAfterStart) {
    ResetUninstalledDrops();
    const LogMode modes[] = {LogMode::Async, LogMode::Sync, LogMode::Manual};
    for (LogMode mode : modes) {
        CountingAllocator allocator;
        LogSystemConfig config = Config(mode);
        config.queueSlots = 32; // Manual에서 포화 경로도 지난다
        config.blockCount = 4;
        LogSystem logs(allocator, config);
        LineSink sink;
        FailingSink failing;
        REQUIRE(logs.AddSink(sink).IsOk());
        REQUIRE(logs.AddSink(failing).IsOk());
        REQUIRE(logs.Start().IsOk());
        const uint64 afterStart = allocator.Allocations();
        const uint64 news = test::OperatorNewCallsOnThisThread();
        {
            LogInstallScope installed(logs);
            ExerciseLogPaths(logs);
        }

        CHECK(allocator.Allocations() == afterStart);
        CHECK(test::OperatorNewCallsOnThisThread() == news);
        CHECK(sink.records != 0);
        logs.Stop();
    }
}

// 출력 스레드를 만들지 못하면 Start는 성공하고 동기 비상 모드로 낮춘다: Warning 이상만 호출
// 스레드에서 비상 출력하고, 그 아래는 버린다(장애 주입: 스택 크기 상한을 넘겨 스레드 생성을
// 실패시킨다).
TEST(LogSystemWriterStartFailureFallsBackToEmergencyOutput) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystemConfig config = Config(LogMode::Async);
    config.writerStackBytes = platform::MaxThreadStackBytes + 1;
    LogSystem logs(allocator, config);
    LineSink sink;
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    CHECK(logs.Stats().emergencyMode);
    LogInstallScope installed(logs);
    Log::Info(LogSystemTests, "dropped in emergency mode");
    Log::Warning(LogSystemTests, "emergency output (expected in test log)");
    CHECK(logs.Flush(0));
    CHECK(sink.records == 0);
    CHECK(logs.Stats().dropped[static_cast<usize>(LogLevel::Info)] == 1);
}

// 비활성 레벨 판정은 컴파일 타임 하한과 설치된 LogSystem을 함께 본다.
TEST(LogIsEnabledCombinesCompiledAndRuntimeLevels) {
    SystemAllocator allocator;
    LogSystemConfig config = Config(LogMode::Manual);
    config.globalMinimum = LogLevel::Warning;
    LogSystem logs(allocator, config);
    REQUIRE(logs.Start().IsOk());
    LogInstallScope installed(logs);
    CHECK(!Log::IsEnabled(LogLevel::Info, LogAudio));
    CHECK(Log::IsEnabled(LogLevel::Warning, LogAudio));
    CHECK(Log::IsEnabled(LogLevel::Fatal, LogAudio));
    REQUIRE(logs.SetLevel("Audio", LogLevel::Trace).IsOk());
    CHECK(Log::IsEnabled(LogLevel::Trace, LogAudio) == (LogLevel::Trace >= LogCompiledMinimum));
}

#include <TestSupport/Test.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Time/DateTime.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

using namespace rocks;

namespace rocks::test {
uint64 OperatorNewCalls() noexcept;
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
    LineSink sink;
    LogSystem logs(allocator, Config(LogMode::Manual));
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
    LineSink sink;
    LogSystem logs(allocator, Config(LogMode::Async));
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
    LineSink sink;
    LogSystem logs(allocator, Config(LogMode::Async));
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
    LineSink sink;
    LogSystem logs(allocator, config);
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
    CHECK(sink.Text().EndsWith("[Warning][Log] 7 log records dropped since the last report (0 "
                               "Trace, 0 Debug, 6 Info, 1 Warning, 0 Error)\n",
                               StringComparison::CaseSensitive));
    CHECK(sink.Text().Contains("[Warning][Tests.System] warning 2\n",
                               StringComparison::CaseSensitive));

    // 두 번째 포화의 알림은 그 사이 늘어난 수만 싣는다(누계가 아니다).
    sink.Clear();
    for (uint32 index = 0; index < 16; ++index) {
        Log::Info(LogSystemTests, "again {Index}", index);
    }

    CHECK(logs.Pump() == 14);
    CHECK(sink.Text().EndsWith("[Warning][Log] 2 log records dropped since the last report (0 "
                               "Trace, 0 Debug, 2 Info, 0 Warning, 0 Error)\n",
                               StringComparison::CaseSensitive));
}

// sink가 연속 LogSinkFailureLimit번 실패하면 막고, LogSinkRetryBatches 묶음마다 다시 시도한다. 다른
// sink는 계속 받는다.
TEST(LogSystemBlocksFailingSinkAndRetries) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LineSink failing;
    LineSink healthy;
    LogSystem logs(allocator, Config(LogMode::Manual));
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
    // 빈 Pump는 다시 시도까지의 묶음 수에 들지 않는다. 기록이 있는 묶음만 센다.
    for (uint32 batch = 0; batch < LogSinkRetryBatches; ++batch) {
        CHECK(logs.Pump() == 0);
    }

    bool stillBlocked = true;
    for (uint32 batch = 0; batch + 1 < LogSinkRetryBatches; ++batch) {
        Log::Info(LogSystemTests, "while blocked {Batch}", batch);
        stillBlocked = stillBlocked && logs.Pump() == 1 && failing.records == LogSinkFailureLimit;
    }

    CHECK(stillBlocked);
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
        globalSet = logs.SetGlobalMinimum(LogLevel::Info).Error();
        levelSet = logs.SetLevel("Audio", LogLevel::Error).Error();
        levelCleared = logs.ClearLevel("Audio").Error();
        return Result<void>::Success();
    }

    LogSystem& logs;
    LineSink other;
    uint32 records = 0;
    ErrorCode added = ErrorCode::None;
    ErrorCode removed = ErrorCode::None;
    ErrorCode globalSet = ErrorCode::None;
    ErrorCode levelSet = ErrorCode::None;
    ErrorCode levelCleared = ErrorCode::None;
    bool flushed = true;
};

} // namespace

// sink 콜백 안의 로그는 큐에 들어가지 않고 비상 경로로만 간다(재귀 없음). 콜백 안의 등록·해제·레벨
// 변경은 Busy, Flush는 false다.
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
    // 레벨 변경도 콜백 안에서는 Busy다(설치·해제와 lock 순서가 엇갈리지 않게).
    CHECK(sink.globalSet == ErrorCode::Busy && sink.levelSet == ErrorCode::Busy &&
          sink.levelCleared == ErrorCode::Busy);
    CHECK(logs.Stats().reentered == 1);
    // sink가 logs를 참조하므로 logs보다 나중에 선언했다. 파괴 전에 등록을 푼다(RuntimeLogSink
    // 수명).
    REQUIRE(logs.RemoveSink(sink).IsOk());
}

// 복사할 수 있는 최소 sink다(RuntimeLogSink 복사가 등록 상태를 옮기지 않는지 본다).
namespace {

class CopyableSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent&) noexcept override {
        return Result<void>::Success();
    }
};

} // namespace

// sink 수명(설계 L28): 등록은 RemoveSink가 성공하거나 그 LogSystem이 파괴될 때 풀린다. Stop은
// 등록을 풀지 않는다(소멸자가 다시 flush한다). 여러 LogSystem에 등록하면 모두 풀려야 하고, 복사한
// sink는 등록되지 않은 상태로 시작한다.
TEST(LogSystemSinkRegistrationLastsUntilRemoveOrDestruction) {
    SystemAllocator allocator;
    LineSink shared;
    LineSink stopped;
    CHECK(!shared.IsAdded());
    {
        LogSystem first(allocator, Config(LogMode::Manual));
        LogSystem second(allocator, Config(LogMode::Manual));
        REQUIRE(first.AddSink(shared).IsOk());
        CHECK(shared.IsAdded());
        REQUIRE(second.AddSink(shared).IsOk());
        REQUIRE(first.RemoveSink(shared).IsOk());
        CHECK(shared.IsAdded()); // second에 남아 있다
        CHECK(first.RemoveSink(shared).Error() == ErrorCode::NotFound);
        CHECK(shared.IsAdded());

        REQUIRE(first.AddSink(stopped).IsOk());
        CHECK(first.AddSink(stopped).Error() == ErrorCode::AlreadyExists);
        REQUIRE(first.Start().IsOk());
        first.Stop();
        CHECK(stopped.IsAdded()); // Stop은 등록을 풀지 않는다

        // 복사·대입은 등록 상태를 옮기지 않는다.
        CopyableSink original;
        REQUIRE(first.AddSink(original).IsOk());
        CopyableSink copy(original);
        CHECK(!copy.IsAdded());
        copy = original;
        CHECK(!copy.IsAdded());
        REQUIRE(first.RemoveSink(original).IsOk());
    }

    // 두 LogSystem의 소멸자가 남은 등록을 풀었다. 이제 sink를 파괴해도 된다.
    CHECK(!shared.IsAdded());
    CHECK(!stopped.IsAdded());
}

TEST(LogSystemSinkRegistrationErrors) {
    SystemAllocator allocator;
    LineSink sinks[MaxLogSinks + 1];
    LineSink invalid;
    LogSystem logs(allocator, Config(LogMode::Manual));
    for (usize index = 0; index < MaxLogSinks; ++index) {
        REQUIRE(logs.AddSink(sinks[index]).IsOk());
    }

    CHECK(logs.AddSink(sinks[0]).Error() == ErrorCode::AlreadyExists);
    CHECK(logs.AddSink(sinks[MaxLogSinks]).Error() == ErrorCode::CapacityExceeded);
    CHECK(logs.RemoveSink(sinks[MaxLogSinks]).Error() == ErrorCode::NotFound);
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

    REQUIRE(logs.SetGlobalMinimum(LogLevel::Info).IsOk());
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
        LineSink sink;
        LogSystem logs(allocator, config);
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

    LineSink outerSink;
    LogSystem outer(allocator, Config(LogMode::Manual));
    LineSink innerSink;
    LogSystem inner(allocator, Config(LogMode::Manual));
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

// 게이트에서 멈추는 Formatter 값이다. 로그 호출을 읽기 범위(설치 세대 카운터) 안에 붙잡아 둔다.
struct GateState {
    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
};

struct GatedValue {
    GateState* gate = nullptr;
};

} // namespace

namespace rocks {

template <>
struct Formatter<GatedValue> {
    static Result<void> Format(StringBuilderBase& out, const GatedValue& value,
                               StringView) noexcept {
        value.gate->entered.Signal();
        if (value.gate->release.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
            platform::Fatal("gated log value was never released");
        }

        return out.TryAppend("gated");
    }
};

} // namespace rocks

namespace {

struct UninstallContext {
    LogSystem* logs = nullptr;
    platform::ManualResetEvent installed;
    platform::ManualResetEvent uninstall;
    platform::ManualResetEvent uninstalled;
};

// 설치하고, 신호를 받으면 범위를 끝내 해제한다(해제가 이 스레드에서 기다린다).
void InstallThenUninstall(void* raw) noexcept {
    auto& context = *static_cast<UninstallContext*>(raw);
    {
        LogInstallScope installed(*context.logs);
        context.installed.Signal();
        if (context.uninstall.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
            platform::Fatal("uninstall was never requested");
        }
    }

    context.uninstalled.Signal();
}

void LogOldGeneration(void* raw) noexcept {
    Log::Info(LogSystemTests, "old {Value}", GatedValue{static_cast<GateState*>(raw)});
}

void LogNewGeneration(void* raw) noexcept {
    Log::Warning(LogSystemTests, "new reader (expected in test log) {Value}",
                 GatedValue{static_cast<GateState*>(raw)});
}

void Start(platform::Thread& thread, platform::Thread::Entry entry, void* context) noexcept {
    if (thread.Start(entry, context) != platform::ThreadStatus::Success) {
        platform::Fatal("log system test worker did not start");
    }
}

void Await(platform::ManualResetEvent& event, const char* what) noexcept {
    if (event.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
        platform::Fatal(what);
    }
}

} // namespace

// 해제는 뒤집기 전 세대의 호출만 기다린다. 옛 세대 호출 하나를 게이트로 붙잡은 채 해제를 시작하고,
// 뒤집기가 끝난 뒤 시작한 호출(새 세대)도 게이트로 붙잡는다. 옛 호출만 놓아도 해제가 끝나야 한다 —
// 새 호출이 여전히 읽기 범위 안에 있는 동안. 순서는 모두 이벤트와 세대 값으로 정한다.
TEST(LogUninstallWaitsOnlyForReadersFromBeforeTheFlip) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Manual));
    REQUIRE(logs.Start().IsOk());
    UninstallContext context;
    context.logs = &logs;
    GateState oldGate;
    GateState newGate;
    platform::Thread installer;
    platform::Thread oldReader;
    platform::Thread newReader;
    Start(installer, InstallThenUninstall, &context);
    Await(context.installed, "install did not finish");
    Start(oldReader, LogOldGeneration, &oldGate);
    Await(oldGate.entered, "old-generation reader did not enter");

    // 해제를 시작하고 세대가 뒤집히기를 기다린다(옛 호출이 붙잡혀 있어 해제는 끝나지 않는다).
    const uint64 before = detail::LogInstallGeneration();
    context.uninstall.Signal();
    const uint64 start = platform::MonotonicNanoseconds();
    while (detail::LogInstallGeneration() == before) {
        if (platform::MonotonicNanoseconds() - start >
            uint64{SafetyTimeoutMilliseconds} * 1000000u) {
            platform::Fatal("uninstall did not flip the generation");
        }

        platform::YieldThread();
    }

    CHECK(context.uninstalled.Wait(0) == platform::WaitResult::TimedOut);
    Start(newReader, LogNewGeneration, &newGate);
    Await(newGate.entered, "new-generation reader did not enter");

    // 옛 호출만 놓는다. 새 호출이 읽기 범위 안에 있어도 해제는 끝난다.
    oldGate.release.Signal();
    CHECK(context.uninstalled.Wait(SafetyTimeoutMilliseconds) == platform::WaitResult::Signaled);
    newGate.release.Signal();
    Join(oldReader);
    Join(newReader);
    Join(installer);
    CHECK(logs.Pump() == 1); // 옛 호출은 해제 전에 설치된 시스템의 큐에 들어갔다
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
    uint32 returned = 0;
    bool removed = false;
};

void RemoveGateSink(void* raw) noexcept {
    auto& context = *static_cast<RemovalContext*>(raw);
    context.started.Signal();
    context.removed = context.logs->RemoveSink(*context.sink).IsOk();
    context.finishedWhenRemoved =
        platform::AtomicLoad<platform::MemoryOrder::Acquire>(&context.sink->finished);
    platform::AtomicStore<platform::MemoryOrder::Release>(&context.returned, 1u);
}

} // namespace

// RemoveSink는 진행 중인 콜백이 끝난 뒤 반환한다. 콜백은 시작을 알리고(entered) 게이트에서 멈춘다.
// 그동안 RemoveSink를 부른 스레드는 반환하지 않아야 하고(게이트를 열기 전 returned가 0), 반환했을
// 때는 콜백의 마지막 쓰기(finished)가 이미 보여야 한다. 시간이 아니라 순서로 확인한다.
TEST(LogSystemRemoveSinkWaitsForCallback) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    GateSink sink;
    LogSystem logs(allocator, Config(LogMode::Async));
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

    CHECK(platform::AtomicLoad<platform::MemoryOrder::Acquire>(&context.returned) == 0);
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
        LineSink sink;
        FailingSink failing;
        LogSystem logs(allocator, config);
        REQUIRE(logs.AddSink(sink).IsOk());
        REQUIRE(logs.AddSink(failing).IsOk());
        REQUIRE(logs.Start().IsOk());
        const uint64 afterStart = allocator.Allocations();
        const uint64 news = test::OperatorNewCalls();
        {
            LogInstallScope installed(logs);
            ExerciseLogPaths(logs);
        }

        CHECK(allocator.Allocations() == afterStart);
        CHECK(test::OperatorNewCalls() == news);
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
    LineSink sink;
    LogSystem logs(allocator, config);
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
namespace {

// 시험이 정하는 단조 tick 시계다(초당 TestClockFrequency tick). rate limit 창을 결정적으로 정한다.
constexpr uint64 TestClockFrequency = 1000;
uint64 testClockTicks = 0;

uint64 TestClock() noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&testClockTicks);
}

void SetTestClockSeconds(uint64 seconds) noexcept {
    platform::AtomicStore<platform::MemoryOrder::Relaxed>(&testClockTicks,
                                                          seconds * TestClockFrequency);
}

LogSystemConfig RateLimitConfig() noexcept {
    LogSystemConfig config = Config(LogMode::Manual);
    config.clock = &TestClock;
    config.clockFrequency = TestClockFrequency;
    return config;
}

usize CountText(StringView text, StringView needle) noexcept {
    usize count = 0;
    for (usize index = 0; index + needle.ByteCount() <= text.ByteCount(); ++index) {
        if (StringView(text.Data() + index, needle.ByteCount()) == needle) {
            ++count;
        }
    }

    return count;
}

constexpr uint32 RateLimitThreads = 4;
constexpr uint32 RecordsPerThread = 50;
constexpr uint32 SharedRateLimit = 20;

void EmitLimitedRecords(void*) noexcept {
    for (uint32 index = 0; index < RecordsPerThread; ++index) {
        Log::Info(LogSystemTests, "shared {Index}", index);
    }
}

} // namespace

// rate limit은 창(1초)마다 정확히 상한만 통과시키고, Error는 세지도 버리지도 않는다. 버린 수는
// 출력 쪽이 접두사마다 1초에 한 번까지 알린다.
TEST(LogSystemRateLimitPassesExactlyTheLimitPerWindow) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    SetTestClockSeconds(5);
    LineSink sink;
    LogSystem logs(allocator, RateLimitConfig());
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    REQUIRE(logs.SetRateLimit("Tests", 3).IsOk());
    CHECK(logs.SetRateLimit("Tests", 0).Error() == ErrorCode::InvalidArgument);
    {
        LogInstallScope installed(logs);
        for (uint32 index = 0; index < 10; ++index) {
            Log::Info(LogSystemTests, "limited {Index}", index);
        }

        Log::Error(LogSystemTests, "error is never limited");
        CHECK(logs.Pump() == 4);
        CHECK(CountText(sink.Text(), "limited ") == 3);
        CHECK(CountText(sink.Text(), "Rate limit dropped 7 records for Tests (limit 3/s)") == 1);

        // 같은 창: 모두 버리고, 같은 초에는 다시 알리지 않는다.
        for (uint32 index = 0; index < 10; ++index) {
            Log::Info(LogSystemTests, "limited {Index}", index);
        }

        Log::Error(LogSystemTests, "error is never limited");
        CHECK(logs.Pump() == 1);
        CHECK(CountText(sink.Text(), "Rate limit dropped") == 1);

        // 다음 창: 다시 3건이 통과하고, 지난 알림 뒤로 버린 수를 한 번 알린다.
        SetTestClockSeconds(6);
        for (uint32 index = 0; index < 10; ++index) {
            Log::Info(LogSystemTests, "limited {Index}", index);
        }

        CHECK(logs.Pump() == 3);
        CHECK(CountText(sink.Text(), "limited ") == 6);
        CHECK(CountText(sink.Text(), "Rate limit dropped 17 records for Tests (limit 3/s)") == 1);
        CHECK(logs.Stats().rateLimited == 24);

        // 지우면 모두 통과한다.
        REQUIRE(logs.ClearRateLimits().IsOk());
        for (uint32 index = 0; index < 10; ++index) {
            Log::Info(LogSystemTests, "limited {Index}", index);
        }

        CHECK(logs.Pump() == 10);
    }

    CHECK(CountText(sink.Text(), "error is never limited") == 2);
}

// 여러 생산자가 같은 칸을 동시에 써도 창마다 통과 수는 정확히 상한이다(창·수를 원자 값 하나로
// 바꾸므로).
TEST(LogSystemRateLimitIsExactUnderConcurrentProducers) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    SetTestClockSeconds(9);
    LineSink sink;
    LogSystem logs(allocator, RateLimitConfig());
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    REQUIRE(logs.SetRateLimit("Tests.System", SharedRateLimit).IsOk());
    {
        LogInstallScope installed(logs);
        platform::Thread threads[RateLimitThreads];
        for (platform::Thread& thread : threads) {
            REQUIRE(thread.Start(&EmitLimitedRecords, nullptr) == platform::ThreadStatus::Success);
        }

        for (platform::Thread& thread : threads) {
            Join(thread);
        }

        CHECK(logs.Pump() == SharedRateLimit);
    }

    CHECK(logs.Stats().rateLimited == RateLimitThreads * RecordsPerThread - SharedRateLimit);
}

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

namespace {

// 문이 열릴 때까지 Write 안에서 기다리는 sink다. 처음에는 닫혀 있다.
class HoldingSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent&) noexcept override {
        (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&records, 1u);
        entered.Signal();
        if (release.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
            platform::Fatal("holding sink was never released");
        }

        return Result<void>::Success();
    }

    uint32 Records() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&records);
    }

    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
    uint32 records = 0;
};

} // namespace

// Manual의 Stop은 Pump하지 않은 기록도 sink에 넘긴다.
TEST(LogSystemManualStopDeliversQueuedRecords) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LineSink sink;
    LogSystem logs(allocator, Config(LogMode::Manual));
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    {
        LogInstallScope installed(logs);
        Log::Info(LogSystemTests, "queued {Index}", 1);
        Log::Info(LogSystemTests, "queued {Index}", 2);
    }

    CHECK(sink.records == 0);
    logs.Stop();
    CHECK(sink.Text() == "[Info][Tests.System] queued 1\n"
                         "[Info][Tests.System] queued 2\n");
}

// Stop → Start 뒤에도 Flush는 그 전에 넣은 기록이 sink에 닿을 때까지 기다린다(앞 실행의 완료 위치가
// 남아 바로 끝나지 않는다).
TEST(LogSystemFlushWaitsAfterRestart) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    HoldingSink sink;
    LogSystem logs(allocator, Config(LogMode::Async));
    sink.release.Signal();
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    {
        LogInstallScope installed(logs);
        for (uint32 index = 0; index < 5; ++index) {
            Log::Info(LogSystemTests, "first run {Index}", index);
        }

        CHECK(logs.Flush(SafetyTimeoutMilliseconds));
    }

    logs.Stop();
    REQUIRE(logs.Start().IsOk());
    sink.release.Reset();
    sink.entered.Reset();
    {
        LogInstallScope installed(logs);
        Log::Info(LogSystemTests, "second run");
        if (sink.entered.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
            platform::Fatal("holding sink was not entered");
        }

        // 기록이 sink 안에 붙잡혀 있으므로 끝나지 않았다.
        CHECK(!logs.Flush(0));
        sink.release.Signal();
        CHECK(logs.Flush(SafetyTimeoutMilliseconds));
    }

    CHECK(sink.Records() == 6);
}

// Async Stop은 sink가 멈춰도 끝없이 기다리지 않는다. drain을 포기하고(남은 기록은 버린다) 출력
// 스레드와 메모리를 남긴 채 반환한다. 그 스레드가 남은 동안 Start는 Busy이고, 소멸자가 sink에서
// 돌아온 스레드를 회수한다.
TEST(LogSystemStopAbandonsWriterStuckInSink) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystemConfig config = Config(LogMode::Async);
    config.drainTimeoutMilliseconds = 10;
    HoldingSink sink;
    LineSink other;
    LineSink late;
    {
        LogSystem logs(allocator, config);
        REQUIRE(logs.AddSink(sink).IsOk());
        REQUIRE(logs.AddSink(other).IsOk());
        REQUIRE(logs.Start().IsOk());
        {
            LogInstallScope installed(logs);
            for (uint32 index = 0; index < 3; ++index) {
                Log::Info(LogSystemTests, "stuck {Index}", index);
            }

            if (sink.entered.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
                platform::Fatal("holding sink was not entered");
            }
        }

        // sink는 Stop이 반환한 뒤에야 열린다. 그러므로 Stop은 sink를 기다리지 않고 반환했다.
        logs.Stop();
        CHECK(logs.Stats().writerAbandoned);
        CHECK(logs.Start().Error() == ErrorCode::Busy);
        // 남은 스레드가 sink lock을 쥐고 있으므로 등록·해제는 기다리지 않고 Busy다.
        CHECK(logs.AddSink(late).Error() == ErrorCode::Busy);
        CHECK(logs.RemoveSink(other).Error() == ErrorCode::Busy);
        sink.release.Signal();
    }

    // 멈췄던 sink에서 돌아온 출력 스레드는 다음 sink(other)를 부르지 않았다.
    CHECK(sink.Records() == 1);
    CHECK(other.records == 0 && late.records == 0);
}

namespace {

// 콜백 안에서 Stop을 부른다(아무것도 하지 않고 돌아와야 한다).
class StoppingSink final : public RuntimeLogSink {
public:
    explicit StoppingSink(LogSystem& owner) noexcept : logs(owner) {
    }

    Result<void> Write(const LogEvent&) noexcept override {
        logs.Stop();
        ++records;
        return Result<void>::Success();
    }

    LogSystem& logs;
    uint32 records = 0;
};

} // namespace

// sink 콜백 안의 Stop은 자기 자신(Pump 중인 pumping_)을 기다리지 않고 돌아온다. Stop 뒤의 Pump는
// 0이다.
TEST(LogSystemStopInsideSinkReturnsAndPumpAfterStopIsEmpty) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Manual));
    StoppingSink sink(logs);
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    {
        LogInstallScope installed(logs);
        Log::Info(LogSystemTests, "first");
        CHECK(logs.Pump() == 1);
        // 콜백 안의 Stop은 무시됐으므로 계속 받는다.
        Log::Info(LogSystemTests, "second");
        CHECK(logs.Pump() == 1);
        Log::Info(LogSystemTests, "third");
    }

    logs.Stop();
    CHECK(sink.records == 3);
    CHECK(logs.Pump() == 0);
    // sink가 logs를 참조하므로 logs보다 나중에 선언했다. 파괴 전에 등록을 푼다(RuntimeLogSink
    // 수명).
    REQUIRE(logs.RemoveSink(sink).IsOk());
}

// Sync도 큰 값을 블록에 담아 Manual과 같은 출력을 낸다(준비 버퍼 크기에서 잘리지 않는다).
TEST(LogSystemSyncKeepsLargeValuesLikeManual) {
    ResetUninstalledDrops();
    char large[2600];
    for (usize index = 0; index < sizeof(large) - 1; ++index) {
        large[index] = static_cast<char>('a' + index % 26);
    }

    large[sizeof(large) - 1] = '\0';
    SystemAllocator allocator;
    LineSink syncSink;
    LineSink manualSink;
    {
        LogSystem logs(allocator, Config(LogMode::Sync));
        REQUIRE(logs.AddSink(syncSink).IsOk());
        REQUIRE(logs.Start().IsOk());
        LogInstallScope installed(logs);
        Log::Info(LogSystemTests, "large {Text}", StringView(large));
        CHECK(logs.Stats().truncated == 0);
    }

    {
        LogSystem logs(allocator, Config(LogMode::Manual));
        REQUIRE(logs.AddSink(manualSink).IsOk());
        REQUIRE(logs.Start().IsOk());
        LogInstallScope installed(logs);
        Log::Info(LogSystemTests, "large {Text}", StringView(large));
        CHECK(logs.Pump() == 1);
        CHECK(logs.Stats().truncated == 0);
    }

    CHECK(syncSink.Text() == manualSink.Text());
    CHECK(syncSink.Text().ByteCount() ==
          sizeof("[Info][Tests.System] large \n") - 1 + sizeof(large) - 1);
}

// Log::Write(RuntimeTemplate)도 사용자 Formatter의 IsValidSpec으로 spec을 검사한다(DateTime은
// "ms"만).
TEST(LogWriteRuntimeTemplateChecksFormatterSpec) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LineSink sink;
    LogSystem logs(allocator, Config(LogMode::Manual));
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    const DateTime time = DateTime::TryFromDate(2026, 11, 18, 1, 2, 3).Value();
    {
        LogInstallScope installed(logs);
        Log::Write(LogLevel::Info, LogSystemTests, RuntimeTemplate{StringView("at {Time:zz}")},
                   time);
        Log::Write(LogLevel::Info, LogSystemTests, RuntimeTemplate{StringView("at {Time:ms}")},
                   time);
        CHECK(logs.Pump() == 2);
    }

    CHECK(sink.Text() ==
          "[Info][Tests.System] at {Time:zz} [template error: InvalidFormatterSpec]\n"
          "[Info][Tests.System] at 2026-11-18T01:02:03.000Z\n");
}

// 창은 앞으로만 옮긴다: 늦게 온 생산자의 과거 시각(6초)이 지금 창(7초)을 되돌려 상한을 다시 열지
// 않는다. 버린 수는 통과한 기록이 없어도 Pump에서 알린다.
TEST(LogSystemRateLimitWindowNeverMovesBackward) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LineSink sink;
    LogSystem logs(allocator, RateLimitConfig());
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    REQUIRE(logs.SetRateLimit("Tests", 3).IsOk());
    {
        LogInstallScope installed(logs);
        SetTestClockSeconds(7);
        for (uint32 index = 0; index < 3; ++index) {
            Log::Info(LogSystemTests, "window record {Index}", index);
        }

        SetTestClockSeconds(6);
        Log::Info(LogSystemTests, "late producer {Index}", 0);
        SetTestClockSeconds(7);
        Log::Info(LogSystemTests, "after late producer {Index}", 1);
        CHECK(logs.Pump() == 3);
        CHECK(logs.Pump() == 0);
    }

    CHECK(CountText(sink.Text(), "window record") == 3);
    CHECK(CountText(sink.Text(), "late producer") == 0);
    CHECK(CountText(sink.Text(), "Rate limit dropped 2 records for Tests (limit 3/s)") == 1);
    CHECK(logs.Stats().rateLimited == 2);
}

namespace {

uint32 previousHookCalls = 0;

void CountingTerminationHook(const char*, usize) noexcept {
    ++previousHookCalls;
}

} // namespace

// LogInstallScope는 종료 hook을 걸고 해제 때 앞 hook으로 되돌린다. 로그 hook은 flush 뒤 앞 hook에
// 연쇄한다.
TEST(LogInstallScopeChainsAndRestoresTerminationHook) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Manual));
    REQUIRE(logs.Start().IsOk());
    previousHookCalls = 0;
    const platform::TerminationHook original =
        platform::SetTerminationHook(&CountingTerminationHook);
    {
        LogInstallScope installed(logs);
        const platform::TerminationHook current = platform::SetTerminationHook(nullptr);
        CHECK(current == &detail::LogTerminationHook);
        (void)platform::SetTerminationHook(current);
        current("probe", 5);
        CHECK(previousHookCalls == 1);
    }

    CHECK(platform::SetTerminationHook(original) == &CountingTerminationHook);
}

#include "LogInternal.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

using platform::MemoryOrder;

namespace {

// 이 파일이 남기는 진단의 카테고리다.
constexpr LogCategory LogLogging{"Log"};

// 한 묶음에서 꺼내는 최대 기록 수다(그 뒤 sink flush·Flush 대기자 알림).
constexpr usize WriterBatchLimit = 256;

// 출력 스레드 이름이다.
constexpr const char* WriterThreadName = "LogWriter";

// 스레드별 상태다. 정적 TLS 고정 배열이며 힙을 쓰지 않는다.
// - staging: 준비 버퍼 두 개(깊이 1까지의 중첩: Formatter·Lazy 안의 로그)
// - inSink: 이 스레드가 sink 콜백 안에 있다(그 안의 로그는 비상 경로로)
alignas(8) thread_local uint8 staging[2][LogSlotBytes];
thread_local uint32 stagingDepth = 0;
thread_local bool inSink = false;

class SinkCallbackScope {
public:
    SinkCallbackScope() noexcept {
        inSink = true;
    }

    ~SinkCallbackScope() noexcept {
        inSink = false;
    }

    SinkCallbackScope(const SinkCallbackScope&) = delete;
    SinkCallbackScope& operator=(const SinkCallbackScope&) = delete;
};

class StagingScope {
public:
    StagingScope() noexcept : depth_(stagingDepth) {
        ++stagingDepth;
    }

    ~StagingScope() noexcept {
        --stagingDepth;
    }

    StagingScope(const StagingScope&) = delete;
    StagingScope& operator=(const StagingScope&) = delete;

    bool Available() const noexcept {
        return depth_ < 2;
    }

    Span<uint8> Buffer() const noexcept {
        return Span<uint8>(staging[depth_], LogSlotBytes);
    }

private:
    uint32 depth_;
};

uint64 Load(const uint64& value) noexcept {
    return platform::AtomicLoad<MemoryOrder::Relaxed>(&value);
}

void Increment(uint64& value) noexcept {
    (void)platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&value, uint64{1});
}

// ---- 설치(두 세대 카운터, SRCU 방식) ----
// 호출은 현재 세대를 읽어 그 세대 카운터를 올리고, seq_cst로 포인터를 다시 읽고, 끝나면 내린다.
// 설치·해제는 포인터를 바꾼 뒤 세대를 뒤집고 옛 세대 카운터만 0이 되기를 기다린다. 새 호출은 새
// 세대로 가므로 로그가 계속 들어와도 해제가 굶지 않는다. 카운터가 LogSystem 밖에 있으므로 파괴된
// 객체를 건드리지 않는다.
constinit LogSystem* installedSystem = nullptr;
constinit uint64 installGeneration = 0;
constinit uint64 installReaders[2] = {0, 0};
constinit uint64 droppedWhileUninstalled = 0;

platform::Mutex& InstallMutex() noexcept {
    static platform::Mutex mutex;
    return mutex;
}

// 옛 세대의 진행 중 호출이 모두 끝날 때까지 기다린다(installMutex 안에서 부른다).
void SynchronizeReaders() noexcept {
    const uint64 previous =
        platform::AtomicFetchAdd<MemoryOrder::SequentiallyConsistent>(&installGeneration, 1u);
    uint64& readers = installReaders[previous & 1u];
    while (platform::AtomicLoad<MemoryOrder::Acquire>(&readers) != 0) {
        platform::YieldThread();
    }
}

} // namespace

namespace detail {

constinit uint8 logFastMinimum = 0;

LogReadScope::LogReadScope() noexcept {
    generation_ =
        platform::AtomicLoad<MemoryOrder::SequentiallyConsistent>(&installGeneration) & 1u;
    (void)platform::AtomicFetchAdd<MemoryOrder::SequentiallyConsistent>(
        &installReaders[generation_], uint64{1});
    system_ = platform::AtomicLoad<MemoryOrder::SequentiallyConsistent>(&installedSystem);
}

LogReadScope::~LogReadScope() noexcept {
    (void)platform::AtomicFetchSub<MemoryOrder::Release>(&installReaders[generation_], uint64{1});
}

void CountUninstalledDrop() noexcept {
    Increment(droppedWhileUninstalled);
}

struct LogSystemAccess {
    static void Submit(LogSystem& system, LogLevel level, const LogCategory& category,
                       const LogCaptureTemplate& text, const FormatArg* args,
                       usize count) noexcept {
        system.Submit(level, category, text, args, count);
    }

    static void DeliverFatal(LogSystem& system, Span<const uint8> capture) noexcept {
        system.DeliverFatal(capture);
    }
};

void SubmitToLogSystem(LogSystem& system, LogLevel level, const LogCategory& category,
                       const LogCaptureTemplate& text, const FormatArg* args,
                       usize count) noexcept {
    LogSystemAccess::Submit(system, level, category, text, args, count);
}

void DeliverFatalToLogSystem(LogSystem& system, Span<const uint8> capture) noexcept {
    LogSystemAccess::DeliverFatal(system, capture);
}

void RefreshLogFastMinimum(LogSystem& system, uint8 minimum) noexcept {
    platform::LockGuard lock(InstallMutex());
    if (platform::AtomicLoad<MemoryOrder::SequentiallyConsistent>(&installedSystem) == &system) {
        platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&logFastMinimum, minimum);
    }
}

} // namespace detail

// ---- LogInstallScope ----

LogInstallScope::LogInstallScope(LogSystem& system) noexcept {
    uint64 dropped = 0;
    {
        platform::LockGuard lock(InstallMutex());
        previous_ = platform::AtomicExchange<MemoryOrder::SequentiallyConsistent>(&installedSystem,
                                                                                  &system);
        platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&detail::logFastMinimum,
                                                                   system.EffectiveMinimum());
        SynchronizeReaders();
        dropped =
            platform::AtomicExchange<MemoryOrder::Relaxed>(&droppedWhileUninstalled, uint64{0});
    }

    // 설치 전에 버린 낮은 레벨 기록 수를 한 번 알린다(sink가 레벨을 바꿀 수 있으므로 lock 밖에서).
    if (dropped != 0) {
        (void)platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&system.droppedBeforeInstall_,
                                                             dropped);
        detail::LogTo(system, LogLevel::Warning, LogLogging,
                      "{Dropped} log records below Warning were dropped while no log system was "
                      "installed",
                      dropped);
    }
}

LogInstallScope::~LogInstallScope() noexcept {
    platform::LockGuard lock(InstallMutex());
    (void)platform::AtomicExchange<MemoryOrder::SequentiallyConsistent>(&installedSystem,
                                                                        previous_);
    platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(
        &detail::logFastMinimum, previous_ != nullptr ? previous_->EffectiveMinimum() : uint8{0});
    SynchronizeReaders();
}

// ---- LogSystem ----

LogSystem::LogSystem(Allocator& allocator, const LogSystemConfig& config) noexcept
    : allocator_(&allocator), config_(config),
      globalMinimum_(static_cast<uint8>(config.globalMinimum)) {
}

LogSystem::~LogSystem() noexcept {
    Stop();
}

Result<void> LogSystem::Start() noexcept {
    if (platform::AtomicLoad<MemoryOrder::Acquire>(&started_) != 0 ||
        !IsValidLogLevel(config_.globalMinimum) ||
        config_.renderBytes < 2 * MinimumLogRenderBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool queued = config_.mode != LogMode::Sync;
    if (queued) {
        auto queue = queue_.Initialize(*allocator_, config_.queueSlots);
        if (!queue) {
            return queue;
        }

        auto pool = blocks_.Initialize(*allocator_, config_.blockCount, config_.blockBytes);
        if (!pool) {
            queue_.Shutdown();
            return pool;
        }
    }

    auto render = allocator_->Allocate(config_.renderBytes, alignof(uint64));
    if (!render) {
        queue_.Shutdown();
        blocks_.Shutdown();
        return Result<void>::Failure(render.Error());
    }

    render_ = static_cast<char*>(render.Value());
    platform::AtomicStore<MemoryOrder::Relaxed>(&stopping_, 0u);
    platform::AtomicStore<MemoryOrder::Relaxed>(&emergency_, 0u);
    platform::AtomicStore<MemoryOrder::Release>(&started_, 1u);
    platform::AtomicStore<MemoryOrder::Release>(&accepting_, 1u);
    if (config_.mode == LogMode::Async) {
        platform::ThreadOptions options;
        options.name = WriterThreadName;
        options.stackBytes = config_.writerStackBytes;
        if (writer_.Start(WriterEntry, this, options) != platform::ThreadStatus::Success) {
            // 출력 스레드 없이도 엔진은 계속 돈다. Warning 이상만 호출 스레드에서 비상 출력한다.
            platform::AtomicStore<MemoryOrder::Release>(&emergency_, 1u);
            detail::WriteLogEmergencyText(LogLevel::Warning, LogLogging.Name(),
                                          "Log writer thread could not start; falling back to "
                                          "synchronous emergency output for Warning and above");
        }
    }

    return Result<void>::Success();
}

void LogSystem::Stop() noexcept {
    if (platform::AtomicExchange<MemoryOrder::AcquireRelease>(&started_, 0u) == 0) {
        return;
    }

    platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&accepting_, 0u);
    if (config_.mode == LogMode::Async &&
        platform::AtomicLoad<MemoryOrder::Acquire>(&emergency_) == 0) {
        platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&stopping_, 1u);
        Wake();
        // drain은 제한 시간 안의 best effort다. 넘기면 알리고, sink가 멈춘 경우에도 회수는 끝까지
        // 기다린다(스레드를 강제로 끝내지 않는다).
        if (writer_.WaitForCompletion(config_.drainTimeoutMilliseconds) !=
            platform::WaitResult::Signaled) {
            detail::WriteLogEmergencyText(LogLevel::Warning, LogLogging.Name(),
                                          "Log writer did not drain within the stop timeout");
        }

        (void)writer_.Join();
    } else if (config_.mode == LogMode::Manual) {
        (void)Pump();
    }

    {
        platform::LockGuard lock(sinkMutex_);
        FlushSinks();
    }

    queue_.Shutdown();
    blocks_.Shutdown();
    if (render_ != nullptr && !allocator_->Deallocate(render_)) {
        platform::Fatal("LogSystem render buffer deallocation was refused");
    }

    render_ = nullptr;
}

Result<void> LogSystem::AddSink(RuntimeLogSink& sink, LogFilter filter) noexcept {
    if (inSink) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (!IsValidLogLevel(filter.minimum)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(sinkMutex_);
    for (usize index = 0; index < sinkCount_; ++index) {
        if (sinks_[index].sink == &sink) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    if (sinkCount_ == MaxLogSinks) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    sinks_[sinkCount_] = SinkSlot{};
    sinks_[sinkCount_].sink = &sink;
    sinks_[sinkCount_].filter = filter;
    ++sinkCount_;
    return Result<void>::Success();
}

Result<void> LogSystem::RemoveSink(RuntimeLogSink& sink) noexcept {
    if (inSink) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 출력 쪽은 묶음 동안 sinkMutex_를 쥔다. lock을 얻었으면 진행 중인 콜백이 없다.
    platform::LockGuard lock(sinkMutex_);
    for (usize index = 0; index < sinkCount_; ++index) {
        if (sinks_[index].sink == &sink) {
            for (usize next = index + 1; next < sinkCount_; ++next) {
                sinks_[next - 1] = sinks_[next];
            }

            sinks_[--sinkCount_] = SinkSlot{};
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

void LogSystem::Wake() noexcept {
    // Dekker 짝(설계 8.4절): 생산자는 공개 → fence → sleeping 교환. 출력 스레드는 sleeping 표시 →
    // fence → 큐 재확인. 한쪽은 반드시 상대의 기록을 본다. exchange가 1을 돌려준 하나만 깨운다.
    platform::AtomicThreadFence<MemoryOrder::SequentiallyConsistent>();
    if (platform::AtomicExchange<MemoryOrder::SequentiallyConsistent>(&sleeping_, 0u) != 0) {
        wake_.Signal();
    }
}

void LogSystem::WriterEntry(void* context) noexcept {
    static_cast<LogSystem*>(context)->WriterLoop();
}

void LogSystem::WriterLoop() noexcept {
    platform::AtomicStore<MemoryOrder::Release>(&writerThread_, platform::CurrentThreadId());
    detail::LogWriterScope writerScope;
    for (;;) {
        if (DrainBatch(WriterBatchLimit) != 0) {
            continue;
        }

        if (!queue_.IsEmpty()) {
            // 예약했지만 아직 공개하지 않은 칸이 맨 앞이다. 예약과 공개 사이는 memcpy뿐이다.
            platform::YieldThread();
            continue;
        }

        if (platform::AtomicLoad<MemoryOrder::Acquire>(&stopping_) != 0) {
            break;
        }

        wake_.Reset();
        platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&sleeping_, 1u);
        platform::AtomicThreadFence<MemoryOrder::SequentiallyConsistent>();
        if (!queue_.IsEmpty() || platform::AtomicLoad<MemoryOrder::Acquire>(&stopping_) != 0) {
            platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&sleeping_, 0u);
            continue;
        }

        (void)wake_.Wait();
        platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&sleeping_, 0u);
    }
}

usize LogSystem::Pump(usize maxRecords) noexcept {
    if (config_.mode != LogMode::Manual ||
        platform::AtomicLoad<MemoryOrder::Acquire>(&started_) == 0 || inSink ||
        platform::AtomicExchange<MemoryOrder::Acquire>(&pumping_, 1u) != 0) {
        return 0;
    }

    usize total = 0;
    {
        detail::LogWriterScope writerScope;
        while (total < maxRecords) {
            const usize limit =
                maxRecords - total < WriterBatchLimit ? maxRecords - total : WriterBatchLimit;
            const usize count = DrainBatch(limit);
            if (count == 0) {
                break;
            }

            total += count;
        }
    }

    platform::AtomicStore<MemoryOrder::Release>(&pumping_, 0u);
    return total;
}

usize LogSystem::DrainBatch(usize limit) noexcept {
    usize count = 0;
    {
        platform::LockGuard lock(sinkMutex_);
        // UTC 기준점을 묶음마다 다시 맞춘다. 실패하면 이 묶음의 UTC를 비운다.
        int64 utc = 0;
        anchorValid_ = platform::TryUtcTicks(utc);
        anchorMonotonic_ = platform::MonotonicNanoseconds();
        anchorUtc_ = utc;
        for (usize index = 0; index < sinkCount_; ++index) {
            SinkSlot& slot = sinks_[index];
            if (slot.blocked && ++slot.batchesSinceBlocked >= LogSinkRetryBatches) {
                // 한 번 다시 시도한다. 또 실패하면 바로 다시 막는다.
                slot.blocked = false;
                slot.batchesSinceBlocked = 0;
                slot.consecutiveFailures = LogSinkFailureLimit - 1;
            }
        }

        while (count < limit && queue_.TryPopWith([&](QueueSlot& slot) noexcept {
            const Span<const uint8> capture(slot.data, slot.bytes);
            Dispatch(capture, &blocks_);
            ReleaseLogCaptureBlocks(capture, blocks_);
        })) {
            ++count;
        }

        if (count != 0) {
            ReportDrops();
            FlushSinks();
        }
    }

    if (count != 0) {
        platform::LockGuard lock(flushMutex_);
        completed_ = queue_.PopPosition();
        flushed_.NotifyAll();
    }

    return count;
}

void LogSystem::Dispatch(Span<const uint8> capture, const LogBlockProvider* blocks) noexcept {
    const usize messageBytes = config_.renderBytes / 4 * 3;
    LogRenderTarget target{Span<char>(render_, messageBytes),
                           Span<char>(render_ + messageBytes, config_.renderBytes - messageBytes),
                           Span<LogProperty>(properties_), Span<LogProperty>(context_)};
    LogEvent event;
    if (!RenderLogCapture(capture, blocks, target, event)) {
        // 손상된 기록은 버린다(이 경로에서 손상은 결함이지만 Fatal로 가지 않는다).
        Increment(formatErrors_);
        return;
    }

    if (anchorValid_) {
        const auto delta = static_cast<int64>(event.monotonicNanoseconds - anchorMonotonic_);
        event.utcTicks = anchorUtc_ + delta / 100;
    }

    event.threadName = FindThreadName(event.threadId);
    if (HasAnyLogEventFlags(event.flags,
                            LogEventFlags::ValueTruncated | LogEventFlags::MessageTruncated)) {
        Increment(truncated_);
    }

    if (HasAnyLogEventFlags(event.flags, LogEventFlags::TemplateError)) {
        Increment(templateErrors_);
    }

    if (HasAnyLogEventFlags(event.flags, LogEventFlags::FormatError)) {
        Increment(formatErrors_);
    }

    Increment(written_);
    for (usize index = 0; index < sinkCount_; ++index) {
        SinkSlot& slot = sinks_[index];
        if (slot.blocked || !slot.filter.Accepts(event)) {
            continue;
        }

        bool written = false;
        {
            SinkCallbackScope callback;
            written = slot.sink->Write(event).IsOk();
        }

        if (written) {
            slot.consecutiveFailures = 0;
            continue;
        }

        Increment(sinkFailures_);
        if (++slot.consecutiveFailures >= LogSinkFailureLimit) {
            slot.blocked = true;
            slot.batchesSinceBlocked = 0;
            Increment(sinkBlocks_);
            detail::WriteLogEmergencyText(LogLevel::Error, LogLogging.Name(),
                                          "A log sink failed repeatedly and is blocked; it will be "
                                          "retried later");
        }
    }
}

void LogSystem::FlushSinks() noexcept {
    for (usize index = 0; index < sinkCount_; ++index) {
        SinkSlot& slot = sinks_[index];
        if (slot.blocked) {
            continue;
        }

        SinkCallbackScope callback;
        if (!slot.sink->Flush()) {
            Increment(sinkFailures_);
        }
    }
}

void LogSystem::ReportDrops() noexcept {
    uint64 levels[LogLevelCount]{};
    uint64 total = 0;
    for (usize index = 0; index < LogLevelCount; ++index) {
        levels[index] = Load(dropped_[index]);
        total += levels[index];
    }

    const usize threshold = queue_.Capacity() * LogSystemConfig::SaturationPercent / 100;
    if (total == reportedDrops_ || queue_.ApproximateSize() >= threshold) {
        return;
    }

    reportedDrops_ = total;
    // 출력 쪽에서 직접 렌더해 넘긴다(큐를 거치지 않는다).
    uint8 capture[LogSlotBytes];
    const auto captured = CaptureLog(
        {Span<uint8>(capture), nullptr, platform::MonotonicNanoseconds(),
         platform::CurrentThreadId(), Load(frame_)},
        LogLevel::Warning, LogLogging,
        "{Dropped} log records dropped ({Trace} Trace, {Debug} Debug, {Info} Info, {Warning} "
        "Warning, {Error} Error)",
        total, levels[0], levels[1], levels[2], levels[3], levels[4]);
    if (captured) {
        Dispatch(Span<const uint8>(capture, captured.Value()), nullptr);
    }
}

bool LogSystem::Flush(uint32 timeoutMilliseconds) noexcept {
    if (inSink || detail::OnLogWriterThread() ||
        platform::AtomicLoad<MemoryOrder::Acquire>(&started_) == 0) {
        return false;
    }

    if (config_.mode == LogMode::Sync) {
        platform::LockGuard lock(sinkMutex_);
        FlushSinks();
        return true;
    }

    if (config_.mode == LogMode::Manual) {
        (void)Pump();
        return queue_.IsEmpty();
    }

    if (platform::AtomicLoad<MemoryOrder::Acquire>(&emergency_) != 0) {
        return true;
    }

    // 이 호출 전에 예약된 모든 칸이 출력되면 완료다(출력 스레드가 묶음 끝에 위치를 알린다).
    const uint64 target = queue_.PushPosition();
    Wake();
    const uint64 start = platform::MonotonicNanoseconds();
    const uint64 limit = static_cast<uint64>(timeoutMilliseconds) * 1000000u;
    platform::LockGuard lock(flushMutex_);
    while (completed_ < target) {
        uint32 wait = platform::InfiniteTimeout;
        if (timeoutMilliseconds != platform::InfiniteTimeout) {
            const uint64 elapsed = platform::MonotonicNanoseconds() - start;
            if (elapsed >= limit) {
                return false;
            }

            wait = static_cast<uint32>((limit - elapsed) / 1000000u) + 1u;
        }

        (void)flushed_.Wait(flushMutex_, wait);
    }

    return true;
}

void LogSystem::CountDrop(LogLevel level) noexcept {
    Increment(dropped_[static_cast<usize>(level)]);
}

void LogSystem::Submit(LogLevel level, const LogCategory& category,
                       const detail::LogCaptureTemplate& text, const detail::FormatArg* args,
                       usize count) noexcept {
    if (platform::AtomicLoad<MemoryOrder::Acquire>(&accepting_) == 0) {
        CountDrop(level);
        return;
    }

    // 출력 스레드·sink 콜백 안의 로그는 큐에 넣지 않고 비상 경로로만 보낸다(재귀 없음, 집계).
    if (inSink || detail::OnLogWriterThread()) {
        Increment(reentered_);
        detail::WriteLogEmergency(level, category, text, args, count);
        return;
    }

    // 출력 스레드를 만들지 못한 동기 비상 모드: Warning 이상만 호출 스레드에서 비상 출력한다.
    if (platform::AtomicLoad<MemoryOrder::Acquire>(&emergency_) != 0) {
        if (level >= LogLevel::Warning) {
            detail::WriteLogEmergency(level, category, text, args, count);
        } else {
            CountDrop(level);
        }

        return;
    }

    StagingScope staging;
    if (!staging.Available()) {
        // Formatter·Lazy 안에서 다시 들어온 로그가 깊이 1을 넘었다.
        Increment(reentered_);
        detail::WriteLogEmergency(level, category, text, args, count);
        return;
    }

    const bool sync = config_.mode == LogMode::Sync;
    const LogCaptureTarget target{staging.Buffer(), sync ? nullptr : &blocks_,
                                  platform::MonotonicNanoseconds(), platform::CurrentThreadId(),
                                  Load(frame_)};
    const auto captured = detail::WriteLogCapture(target, level, category, text, args, count);
    if (!captured) {
        CountDrop(level);
        return;
    }

    const Span<const uint8> capture(target.buffer.Data(), captured.Value());
    if (sync) {
        platform::LockGuard lock(sinkMutex_);
        platform::AtomicStore<MemoryOrder::Relaxed>(&syncDispatching_, 1u);
        int64 utc = 0;
        anchorValid_ = platform::TryUtcTicks(utc);
        anchorMonotonic_ = platform::MonotonicNanoseconds();
        anchorUtc_ = utc;
        Dispatch(capture, nullptr);
        FlushSinks();
        platform::AtomicStore<MemoryOrder::Relaxed>(&syncDispatching_, 0u);
        return;
    }

    // 포화: 사용량이 SaturationPercent 이상이면 Info 이하를 버린다(남은 칸은 Warning 이상 전용).
    const usize threshold = queue_.Capacity() * LogSystemConfig::SaturationPercent / 100;
    const bool lowLevel = level <= LogLevel::Info;
    if (lowLevel && queue_.ApproximateSize() >= threshold) {
        CountDrop(level);
        ReleaseLogCaptureBlocks(capture, blocks_);
        return;
    }

    const auto fill = [&](QueueSlot& slot) noexcept {
        slot.bytes = static_cast<uint32>(capture.Size());
        platform::MemoryCopy(slot.data, capture.Data(), capture.Size());
    };

    bool pushed = queue_.TryPushWith(fill);
    if (!pushed && config_.overflow == LogOverflow::WaitForSpace &&
        config_.mode == LogMode::Async) {
        // 손실이 더 나쁜 도구 호스트: 출력 스레드를 깨우며 공간을 기다린다.
        while (!pushed && platform::AtomicLoad<MemoryOrder::Acquire>(&stopping_) == 0) {
            Wake();
            platform::YieldThread();
            pushed = queue_.TryPushWith(fill);
        }
    }

    if (!pushed) {
        CountDrop(level);
        ReleaseLogCaptureBlocks(capture, blocks_);
        return;
    }

    if (config_.mode == LogMode::Async) {
        Wake();
    }
}

void LogSystem::DeliverFatal(Span<const uint8> capture) noexcept {
    if (platform::AtomicLoad<MemoryOrder::Acquire>(&started_) == 0 || inSink ||
        detail::OnLogWriterThread()) {
        return;
    }

    if (config_.mode == LogMode::Sync) {
        // 다른 스레드가 sink 안에 있으면(멈췄을 수 있다) 건너뛴다. 확인과 lock 사이의 좁은 틈은
        // best effort로 받아들인다.
        if (platform::AtomicLoad<MemoryOrder::Relaxed>(&syncDispatching_) != 0) {
            return;
        }

        platform::LockGuard lock(sinkMutex_);
        Dispatch(capture, nullptr);
        FlushSinks();
        return;
    }

    // Manual은 기다릴 출력 스레드가 없고, 비상 모드는 출력 스레드가 없다.
    if (config_.mode != LogMode::Async ||
        platform::AtomicLoad<MemoryOrder::Acquire>(&emergency_) != 0) {
        return;
    }

    const bool pushed = queue_.TryPushWith([&](QueueSlot& slot) noexcept {
        slot.bytes = static_cast<uint32>(capture.Size());
        platform::MemoryCopy(slot.data, capture.Data(), capture.Size());
    });
    if (pushed) {
        (void)Flush(config_.fatalFlushTimeoutMilliseconds);
    }
}

uint8 LogSystem::EffectiveMinimum() const noexcept {
    // 빠른 거부 하한: 전역 최소와 모든 재정의 레벨 중 가장 낮은 값이다.
    uint8 minimum = platform::AtomicLoad<MemoryOrder::Relaxed>(&globalMinimum_);
    const uint32 high = platform::AtomicLoad<MemoryOrder::Acquire>(&overrideHighWater_);
    for (uint32 index = 0; index < high; ++index) {
        if (platform::AtomicLoad<MemoryOrder::Acquire>(&overrides_[index].hash) !=
            InvalidLogCategoryHash) {
            const uint8 level =
                platform::AtomicLoad<MemoryOrder::Relaxed>(&overrides_[index].level);
            minimum = level < minimum ? level : minimum;
        }
    }

    return minimum;
}

bool LogSystem::IsEnabled(LogLevel level, const LogCategory& category) const noexcept {
    if (level == LogLevel::Fatal) {
        return true;
    }

    const auto value = static_cast<uint8>(level);
    if (platform::AtomicLoad<MemoryOrder::Relaxed>(&overrideCount_) != 0) {
        const uint32 high = platform::AtomicLoad<MemoryOrder::Acquire>(&overrideHighWater_);
        // 긴 접두사부터 표와 대조한다(최장 일치).
        for (usize prefix = category.PrefixCount(); prefix-- > 0;) {
            const uint64 hash = category.PrefixHash(prefix);
            for (uint32 index = 0; index < high; ++index) {
                // 해시를 acquire로 읽으면 그 칸에 해시보다 먼저 쓴 레벨이 보인다.
                if (platform::AtomicLoad<MemoryOrder::Acquire>(&overrides_[index].hash) == hash) {
                    return value >=
                           platform::AtomicLoad<MemoryOrder::Relaxed>(&overrides_[index].level);
                }
            }
        }
    }

    return value >= platform::AtomicLoad<MemoryOrder::Relaxed>(&globalMinimum_);
}

void LogSystem::WarnOnce(uint32& flag, StringView message) noexcept {
    if (platform::AtomicExchange<MemoryOrder::Relaxed>(&flag, 1u) == 0) {
        detail::LogTo(*this, LogLevel::Warning, LogLogging, "{Message}", message);
    }
}

void LogSystem::SetGlobalMinimum(LogLevel level) noexcept {
    if (!IsValidLogLevel(level)) {
        return;
    }

    {
        platform::LockGuard lock(levelMutex_);
        platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&globalMinimum_,
                                                                   static_cast<uint8>(level));
        detail::RefreshLogFastMinimum(*this, EffectiveMinimum());
    }

    if (level < LogCompiledMinimum) {
        WarnOnce(warnedCompiledMinimum_,
                 "A log level below the compiled minimum was requested; those records are not "
                 "compiled in");
    }
}

Result<void> LogSystem::SetLevel(StringView prefix, LogLevel level) noexcept {
    if (!IsValidLogLevel(level) || ValidateLogCategoryName(prefix) != LogCategoryNameError::None) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 hash = HashLogCategoryName(prefix);
    {
        platform::LockGuard lock(levelMutex_);
        usize empty = LogLevelOverrideCapacity;
        for (usize index = 0; index < LogLevelOverrideCapacity; ++index) {
            const uint64 current =
                platform::AtomicLoad<MemoryOrder::Relaxed>(&overrides_[index].hash);
            if (current == InvalidLogCategoryHash) {
                empty = empty == LogLevelOverrideCapacity ? index : empty;
                continue;
            }

            if (current != hash) {
                continue;
            }

            // 해시가 같다. 이름까지 같으면 레벨만 바꾸고, 다르면 서로의 재정의가 섞이므로 거부한다.
            if (!EqualsIgnoringAsciiCase(StringView(overrideNames_[index]), prefix)) {
                return Result<void>::Failure(ErrorCode::AlreadyExists);
            }

            platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&overrides_[index].level,
                                                                       static_cast<uint8>(level));
            detail::RefreshLogFastMinimum(*this, EffectiveMinimum());
            return Result<void>::Success();
        }

        if (empty == LogLevelOverrideCapacity) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        platform::MemoryCopy(overrideNames_[empty], prefix.Data(), prefix.ByteCount());
        overrideNames_[empty][prefix.ByteCount()] = '\0';
        // 레벨을 먼저 쓰고 해시를 release로 공개한다(해시를 본 생산자는 레벨도 본다).
        platform::AtomicStore<MemoryOrder::Relaxed>(&overrides_[empty].level,
                                                    static_cast<uint8>(level));
        platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&overrides_[empty].hash, hash);
        if (empty + 1 > platform::AtomicLoad<MemoryOrder::Relaxed>(&overrideHighWater_)) {
            platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(
                &overrideHighWater_, static_cast<uint32>(empty + 1));
        }

        (void)platform::AtomicFetchAdd<MemoryOrder::SequentiallyConsistent>(&overrideCount_, 1u);
        detail::RefreshLogFastMinimum(*this, EffectiveMinimum());
    }

    if (level < LogCompiledMinimum) {
        WarnOnce(warnedCompiledMinimum_,
                 "A log level below the compiled minimum was requested; those records are not "
                 "compiled in");
    }

    return Result<void>::Success();
}

Result<void> LogSystem::ClearLevel(StringView prefix) noexcept {
    if (ValidateLogCategoryName(prefix) != LogCategoryNameError::None) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 hash = HashLogCategoryName(prefix);
    platform::LockGuard lock(levelMutex_);
    for (usize index = 0; index < LogLevelOverrideCapacity; ++index) {
        if (platform::AtomicLoad<MemoryOrder::Relaxed>(&overrides_[index].hash) == hash &&
            EqualsIgnoringAsciiCase(StringView(overrideNames_[index]), prefix)) {
            // 해시를 먼저 비운다(그 뒤 시작한 호출은 이 칸을 보지 않는다).
            platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&overrides_[index].hash,
                                                                       InvalidLogCategoryHash);
            (void)platform::AtomicFetchSub<MemoryOrder::SequentiallyConsistent>(&overrideCount_,
                                                                                1u);
            overrideNames_[index][0] = '\0';
            detail::RefreshLogFastMinimum(*this, EffectiveMinimum());
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

void LogSystem::SetFrame(uint64 frame) noexcept {
    platform::AtomicStore<MemoryOrder::Relaxed>(&frame_, frame);
}

Result<void> LogSystem::SetThreadName(StringView name) noexcept {
    if (name.ByteCount() > MaxLogThreadNameBytes || !name.IsValidUtf8()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 thread = platform::CurrentThreadId();
    platform::LockGuard lock(nameMutex_);
    ThreadName* free = nullptr;
    for (ThreadName& entry : threadNames_) {
        if (entry.bytes != 0 && entry.threadId == thread) {
            free = &entry;
            break;
        }

        if (entry.bytes == 0 && free == nullptr) {
            free = &entry;
        }
    }

    if (free == nullptr) {
        return name.IsEmpty() ? Result<void>::Success()
                              : Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    free->threadId = thread;
    free->bytes = static_cast<uint8>(name.ByteCount());
    platform::MemoryCopy(free->name, name.Data(), name.ByteCount());
    free->name[name.ByteCount()] = '\0';
    return Result<void>::Success();
}

StringView LogSystem::FindThreadName(uint64 threadId) noexcept {
    // 이름은 출력 스레드가 sink lock 안에서 읽어 event에 싣는다. 콜백 동안 이름 표가 바뀌면 낡은
    // 이름이 보일 수 있으나 표 항목은 덮어쓰기만 하므로 범위를 벗어나지 않는다.
    platform::LockGuard lock(nameMutex_);
    for (const ThreadName& entry : threadNames_) {
        if (entry.bytes != 0 && entry.threadId == threadId) {
            return StringView(entry.name, entry.bytes);
        }
    }

    return StringView();
}

LogSystemStats LogSystem::Stats() const noexcept {
    LogSystemStats stats;
    for (usize index = 0; index < LogLevelCount; ++index) {
        stats.dropped[index] = Load(dropped_[index]);
    }

    stats.written = Load(written_);
    stats.truncated = Load(truncated_);
    stats.templateErrors = Load(templateErrors_);
    stats.formatErrors = Load(formatErrors_);
    stats.sinkFailures = Load(sinkFailures_);
    stats.sinkBlocks = Load(sinkBlocks_);
    stats.reentered = Load(reentered_);
    stats.droppedBeforeInstall = Load(droppedBeforeInstall_);
    stats.emergencyMode = platform::AtomicLoad<MemoryOrder::Acquire>(&emergency_) != 0;
    return stats;
}

} // namespace rocks

#include "ProgressInternal.h"

#include <Core/Move.h>
#include <Localization/Catalog.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <Progress/ProgressHub.h>

namespace rocks {

namespace detail {

constinit thread_local ProgressThreadState ProgressThread;
constinit ProgressHubState ProgressHubGlobal;

namespace {

// UI 펌프 간격(나노초)이다(ProgressUiPumpInterval = 0.2 s).
constexpr uint64 HubUiPumpIntervalNanoseconds = 200'000'000;
static_assert(static_cast<double>(HubUiPumpIntervalNanoseconds) ==
              ProgressUiPumpInterval * 1'000'000'000.0);

bool HubIsOnceWarning(ProgressWarning kind) noexcept {
    return kind != ProgressWarning::Abandoned && kind != ProgressWarning::ShutdownAbandoned;
}

} // namespace

void RetainRecord(ProgressTaskRecord* record) noexcept {
    if (record != nullptr) {
        (void)record->references.FetchAdd<platform::MemoryOrder::Relaxed>(1u);
    }
}

void ReleaseRecord(ProgressTaskRecord* record) noexcept {
    if (record == nullptr) {
        return;
    }

    // 마지막 계수를 놓는 쪽이 지운다(앞선 쓰기가 보이도록 acquire-release).
    if (record->references.FetchSub<platform::MemoryOrder::AcquireRelease>(1u) != 1u) {
        return;
    }

    Allocator* allocator = record->allocator;
    record->~ProgressTaskRecord();
    (void)allocator->Deallocate(record);
}

uint64 ProgressNow() noexcept {
    ProgressHubState& hub = ProgressHubGlobal;
    return hub.clock != nullptr ? hub.clock(hub.clockContext) : platform::MonotonicNanoseconds();
}

void ProgressWarn(ProgressWarning kind, const char* message) noexcept {
    ProgressHubState& hub = ProgressHubGlobal;
    const uint32 index = static_cast<uint32>(kind);
    if (HubIsOnceWarning(kind)) {
        const uint32 bit = 1u << index;
        if ((hub.warnedOnce.FetchOr<platform::MemoryOrder::Relaxed>(bit) & bit) != 0) {
            return;
        }
    }

    (void)hub.warningCounts[index].FetchAdd<platform::MemoryOrder::Relaxed>(1u);
    Log::Warning(LogProgress, "{Message}", StringView(message));
}

void PumpUiSlow() noexcept {
    ProgressThreadState& thread = ProgressThread;
    ProgressHubState& hub = ProgressHubGlobal;
    if (thread.inPump) {
        return;
    }

    // 주입 시계로 마지막 펌프 뒤 간격이 지났을 때만 부른다(정확히 간격이면 부른다). 간격 판정은
    // 잠금 없이, 펌프 함수는 잠금 안에서 등록이 그대로인지 보고 복사한다(잠금을 놓고 부른다 —
    // 펌프가 Snapshot을 당긴다).
    const uint64 now = ProgressNow();
    if (now - hub.lastPumpNanoseconds.Load<platform::MemoryOrder::Relaxed>() <
        HubUiPumpIntervalNanoseconds) {
        return;
    }

    ProgressUiPump pump;
    {
        platform::LockGuard lock(hub.mutex);
        const uint64 generation = hub.uiGeneration.Load<platform::MemoryOrder::Relaxed>();
        if (generation == 0 || generation != thread.uiGeneration || hub.pump.pump == nullptr) {
            return;
        }

        pump = hub.pump;
        hub.lastPumpNanoseconds.Store<platform::MemoryOrder::Relaxed>(now);
    }

    thread.inPump = true;
    pump.pump(pump.context);
    thread.inPump = false;
}

void PublishSelfProgress(ProgressTaskRecord& record, uint64 serial, double base,
                         double width) noexcept {
    // seqlock 쓰기: 홀수로 올리고 칸을 쓴 뒤 짝수로 올린다(쓰는 쪽은 Primary 스레드 하나).
    const uint32 sequence = record.sequence.Load<platform::MemoryOrder::Relaxed>();
    record.sequence.Store<platform::MemoryOrder::Relaxed>(sequence + 1);
    platform::AtomicThreadFence<platform::MemoryOrder::Release>();
    record.stepSerial.Store<platform::MemoryOrder::Relaxed>(serial);
    record.baseBits.Store<platform::MemoryOrder::Relaxed>(platform::BitCast<uint64>(base));
    record.widthBits.Store<platform::MemoryOrder::Relaxed>(platform::BitCast<uint64>(width));
    record.sequence.Store<platform::MemoryOrder::Release>(sequence + 2);
    record.scopePublished.Store<platform::MemoryOrder::Relaxed>(1u);
    record.selfReported.Store<platform::MemoryOrder::Relaxed>(1u);
}

ProgressSelfStep ReadSelfProgress(const ProgressTaskRecord& record) noexcept {
    ProgressSelfStep step;
    platform::SpinBackoff backoff;
    while (true) {
        const uint32 before = record.sequence.Load<platform::MemoryOrder::Acquire>();
        if ((before & 1u) != 0) {
            backoff.Pause();
            continue;
        }

        step.serial = record.stepSerial.Load<platform::MemoryOrder::Relaxed>();
        step.base =
            platform::BitCast<double>(record.baseBits.Load<platform::MemoryOrder::Relaxed>());
        step.width =
            platform::BitCast<double>(record.widthBits.Load<platform::MemoryOrder::Relaxed>());
        platform::AtomicThreadFence<platform::MemoryOrder::Acquire>();
        if (record.sequence.Load<platform::MemoryOrder::Relaxed>() == before) {
            return step;
        }

        backoff.Pause();
    }
}

Result<ResolvedText> ProgressSourceText(Allocator& allocator, const Text& text) noexcept {
    return ResolveText(allocator, text, CatalogSnapshot());
}

} // namespace detail

namespace {

using detail::ProgressHubGlobal;
using detail::ProgressHubPhase;
using detail::ProgressHubState;
using detail::ProgressTaskRecord;

bool HubIsFinite(double value) noexcept {
    return platform::PlatformMath::IsFinite(value);
}

ProgressHubPhase HubPhase() noexcept {
    return static_cast<ProgressHubPhase>(
        ProgressHubGlobal.phase.Load<platform::MemoryOrder::Acquire>());
}

ErrorCode HubStateError() noexcept {
    switch (HubPhase()) {
    case ProgressHubPhase::Uninitialized:
        return ErrorCode::NotReady;
    case ProgressHubPhase::ShutDown:
        return ErrorCode::ShuttingDown;
    case ProgressHubPhase::Running:
        break;
    }

    return ErrorCode::None;
}

// 살아 있는 목록에서 id를 찾는다(hub 잠금). 없으면 liveCount.
uint32 HubFindLive(ProgressHubState& hub, ProgressTaskId id) noexcept {
    for (uint32 index = 0; index < hub.liveCount; ++index) {
        if (hub.live[index]->id == id) {
            return index;
        }
    }

    return hub.liveCount;
}

uint32 HubIndexOf(ProgressHubState& hub, const ProgressTaskRecord* record) noexcept {
    for (uint32 index = 0; index < hub.liveCount; ++index) {
        if (hub.live[index] == record) {
            return index;
        }
    }

    return hub.liveCount;
}

// 살아 있지 않은 id의 오류다: 발급된 적 없으면 NotFound, 끝났으면 StaleHandle.
ErrorCode HubMissingIdError(ProgressTaskId id) noexcept {
    if (id == NoProgressTask ||
        id >= ProgressHubGlobal.nextTaskId.Load<platform::MemoryOrder::Relaxed>()) {
        return ErrorCode::NotFound;
    }

    return ErrorCode::StaleHandle;
}

bool HubIsDescendant(const ProgressTaskRecord* record,
                     const ProgressTaskRecord* ancestor) noexcept {
    for (const ProgressTaskRecord* walk = record->parent; walk != nullptr; walk = walk->parent) {
        if (walk == ancestor) {
            return true;
        }
    }

    return false;
}

// 사건 하나를 고리에 쌓는다(hub 잠금). 덮어쓰는 칸의 문구는 여기서 놓인다.
ProgressEvent& HubPushEvent(ProgressHubState& hub, ProgressEventKind kind,
                            const ProgressTaskRecord& record) noexcept {
    const uint64 serial = hub.nextEventSerial++;
    ProgressEvent& event = hub.ring[serial % hub.ringCapacity];
    event.serial = serial;
    event.kind = kind;
    event.id = record.id;
    event.parent = record.parentId;
    event.title = Text();
    if (record.title.IsValid()) {
        if (auto title = record.title.Clone()) {
            event.title = Move(title.Value());
        }
    }

    event.presentation = record.presentation;
    event.cancel = record.cancel;
    event.outcome = ProgressOutcome::Succeeded;
    event.detail = Text();
    event.fraction = 0.0;
    return event;
}

// 자기 진행 S와 계수 값이다(5.5절 1).
double HubSelfFraction(const ProgressTaskRecord& record, uint64& done, uint64& total) noexcept {
    done = 0;
    total = 0;
    const bool hasTaskCounter = record.hasTaskCounter.Load<platform::MemoryOrder::Acquire>() != 0;
    if (hasTaskCounter) {
        total = record.taskTotal.Load<platform::MemoryOrder::Relaxed>();
        done = record.taskDone.Load<platform::MemoryOrder::Relaxed>();
        done = done < total ? done : total;
    }

    // 작업 수준 계수가 있으면 모양이 Count이므로 막대도 완료/총량이다(5.4·5.5절).
    if (hasTaskCounter) {
        return total != 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
    }

    if (record.scopePublished.Load<platform::MemoryOrder::Relaxed>() != 0) {
        const detail::ProgressSelfStep step = detail::ReadSelfProgress(record);
        double fraction = step.base;
        if (record.scopeCounterSerial.Load<platform::MemoryOrder::Acquire>() == step.serial) {
            const uint64 stepTotal = record.scopeTotal.Load<platform::MemoryOrder::Relaxed>();
            uint64 stepDone = record.scopeDone.Load<platform::MemoryOrder::Relaxed>();
            stepDone = stepDone < stepTotal ? stepDone : stepTotal;
            if (stepTotal != 0) {
                fraction +=
                    step.width * static_cast<double>(stepDone) / static_cast<double>(stepTotal);
            }
        }

        return fraction;
    }

    return 0.0;
}

double HubClamp01(double value) noexcept {
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

// 기록 하나를 끝낸다(hub 잠금). 살아 있는 목록에서 빼고 hub 참조를 놓는다.
void HubFinishOne(ProgressHubState& hub, uint32 index, ProgressOutcome outcome,
                  Text&& detail) noexcept {
    ProgressTaskRecord* record = hub.live[index];
    record->outcome = outcome;
    if (outcome == ProgressOutcome::Succeeded) {
        record->visible = 1.0;
    } else if (record->childCount == 0) {
        // 마지막 스냅샷 뒤의 보고도 끝 사건 비율에 담는다(하위 작업 합성은 스냅샷 때만 한다).
        uint64 done = 0;
        uint64 total = 0;
        const double self = HubClamp01(HubSelfFraction(*record, done, total));
        record->visible = self > record->visible ? self : record->visible;
    }

    record->finished.Store<platform::MemoryOrder::Release>(1u);
    record->live = false;
    ProgressEvent& event = HubPushEvent(hub, ProgressEventKind::End, *record);
    event.outcome = outcome;
    event.detail = Move(detail);
    event.fraction = record->visible;
    if (record->parent != nullptr && record->parent->live) {
        record->parent->finishedChildWeight += record->weightInParent;
    }

    for (uint32 next = index + 1; next < hub.liveCount; ++next) {
        hub.live[next - 1] = hub.live[next];
    }

    --hub.liveCount;
    record->parent = nullptr;
    detail::ReleaseRecord(record);
}

// 작업과 살아 있는 하위 작업을 끝낸다(hub 잠금). 하위 작업이 먼저 Abandoned로 닫힌다(7.3절).
void HubFinishTree(ProgressHubState& hub, uint32 index, ProgressOutcome outcome,
                   Text&& detail) noexcept {
    ProgressTaskRecord* record = hub.live[index];
    for (uint32 walk = hub.liveCount; walk > index + 1; --walk) {
        if (HubIsDescendant(hub.live[walk - 1], record)) {
            HubFinishOne(hub, walk - 1, ProgressOutcome::Abandoned, Text());
        }
    }

    HubFinishOne(hub, index, outcome, Move(detail));
}

Result<void> HubCloneInto(Text& target, const Text& source) noexcept {
    if (!source.IsValid()) {
        target = Text();
        return Result<void>::Success();
    }

    auto clone = source.Clone();
    if (!clone) {
        return Result<void>::Failure(clone.Error());
    }

    target = Move(clone.Value());
    return Result<void>::Success();
}

// 작업 하나의 보기를 채운다(hub 잠금).
Result<void> HubFillView(ProgressTaskView& view, ProgressTaskRecord& record, uint64 done,
                         uint64 total) noexcept {
    view.id = record.id;
    view.parent = record.parentId;
    if (auto cloned = HubCloneInto(view.title, record.title); !cloned) {
        return cloned;
    }

    view.presentation = record.presentation;
    const bool hasTaskCounter = record.hasTaskCounter.Load<platform::MemoryOrder::Acquire>() != 0;
    if (hasTaskCounter) {
        view.shape = ProgressShape::Count;
    } else if (record.scopePublished.Load<platform::MemoryOrder::Relaxed>() != 0 ||
               record.childCount != 0) {
        view.shape = ProgressShape::Fraction;
    } else {
        view.shape = ProgressShape::Indeterminate;
    }

    view.fraction = record.visible;
    view.done = done;
    view.total = total;
    view.startNanoseconds = record.startNanoseconds;
    view.lastProgressNanoseconds = record.lastProgressNanoseconds;
    view.modalDelaySeconds = record.modalDelaySeconds;
    view.cancellable = record.cancel == ProgressCancel::UserCancellable;
    view.cancelRequested = record.token.IsCancellationRequested();

    platform::LockGuard lock(record.messageMutex);
    if (auto cloned = HubCloneInto(view.taskMessage, record.taskMessage); !cloned) {
        return cloned;
    }

    uint32 depth = record.publishedDepth.Load<platform::MemoryOrder::Relaxed>();
    depth = depth < MaxReportedProgressDepth ? depth : MaxReportedProgressDepth;
    view.messageCount = depth;
    for (uint32 index = 0; index < MaxReportedProgressDepth; ++index) {
        view.messages[index] = Text();
        if (index >= depth) {
            continue;
        }

        const detail::ProgressDepthMessage& message = record.depthMessages[index];
        const uint64 current = record.depthStepSerial[index].Load<platform::MemoryOrder::Relaxed>();
        const Text& chosen = message.stepText.IsValid() && message.stepTextSerial == current
                                 ? message.stepText
                                 : message.defaultText;
        if (auto cloned = HubCloneInto(view.messages[index], chosen); !cloned) {
            return cloned;
        }
    }

    return Result<void>::Success();
}

Result<void> HubCopyEvent(ProgressEvent& target, const ProgressEvent& source) noexcept {
    target.serial = source.serial;
    target.kind = source.kind;
    target.id = source.id;
    target.parent = source.parent;
    target.presentation = source.presentation;
    target.cancel = source.cancel;
    target.outcome = source.outcome;
    target.fraction = source.fraction;
    if (auto cloned = HubCloneInto(target.title, source.title); !cloned) {
        return cloned;
    }

    return HubCloneInto(target.detail, source.detail);
}

void HubClearSnapshot(ProgressSnapshot& out) noexcept {
    out.tasks.Clear();
    out.events.Clear();
    out.lostEvents = 0;
}

Result<void> HubSnapshotLocked(ProgressHubState& hub, ProgressSnapshot& out,
                               uint64& cursor) noexcept {
    const uint64 now = detail::ProgressNow();
    out.takenNanoseconds = now;

    // 비율 합성(5.5절): 아래부터 위로. 목록은 시작 순서라 자식이 부모 뒤에 있다.
    for (uint32 index = 0; index < hub.liveCount; ++index) {
        hub.live[index]->scratchSum = 0.0;
        hub.live[index]->scratchWeight = 0.0;
    }

    if (auto reserved = out.tasks.TryReserve(hub.liveCount); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < hub.liveCount; ++index) {
        if (auto added = out.tasks.TryEmplaceBack(); !added) {
            return Result<void>::Failure(added.Error());
        }
    }

    for (uint32 index = hub.liveCount; index != 0; --index) {
        ProgressTaskRecord& record = *hub.live[index - 1];
        uint64 done = 0;
        uint64 total = 0;
        const double self = HubSelfFraction(record, done, total);
        double fraction = self;
        if (record.childCount != 0) {
            const double selfWeight =
                record.selfReported.Load<platform::MemoryOrder::Relaxed>() != 0 ? record.selfWeight
                                                                                : 0.0;
            const double weight = selfWeight + record.scratchWeight + record.finishedChildWeight;
            fraction =
                weight > 0.0
                    ? (selfWeight * self + record.scratchSum + record.finishedChildWeight) / weight
                    : 0.0;
        }

        fraction = HubClamp01(fraction);
        if (record.parent != nullptr) {
            record.parent->scratchSum += record.weightInParent * fraction;
            record.parent->scratchWeight += record.weightInParent;
        }

        if (fraction > record.visible) {
            record.visible = fraction;
        }

        if (fraction > record.lastFraction || done != record.lastDone) {
            record.lastProgressNanoseconds = now;
        }

        record.lastFraction = fraction > record.lastFraction ? fraction : record.lastFraction;
        record.lastDone = done;
        if (auto filled = HubFillView(out.tasks[index - 1], record, done, total); !filled) {
            return filled;
        }

        out.tasks[index - 1].selfFraction = HubClamp01(self);
    }

    // 사건(7.3절). cursor는 다음에 읽을 번호이고 0은 "지금부터"다.
    const uint64 next = hub.nextEventSerial;
    if (cursor == 0 || cursor > next) {
        cursor = next;
        return Result<void>::Success();
    }

    uint64 oldest = next > hub.ringCapacity ? next - hub.ringCapacity : 1;
    oldest = oldest > hub.ringFirstSerial ? oldest : hub.ringFirstSerial;
    if (cursor < oldest) {
        out.lostEvents = oldest - cursor;
        cursor = oldest;
    }

    if (auto reserved = out.events.TryReserve(static_cast<usize>(next - cursor)); !reserved) {
        return reserved;
    }

    for (uint64 serial = cursor; serial < next; ++serial) {
        auto added = out.events.TryEmplaceBack();
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        if (auto copied = HubCopyEvent(*added.Value(), hub.ring[serial % hub.ringCapacity]);
            !copied) {
            return copied;
        }
    }

    cursor = next;
    return Result<void>::Success();
}

void HubWarnAbandoned(ProgressTaskId id, const Text& title, Allocator& allocator) noexcept {
    (void)ProgressHubGlobal.warningCounts[static_cast<uint32>(detail::ProgressWarning::Abandoned)]
        .FetchAdd<platform::MemoryOrder::Relaxed>(1u);
    auto source = detail::ProgressSourceText(allocator, title);
    const StringView name = source ? source.Value().value.View() : StringView("?");
    Log::Warning(detail::LogProgress,
                 "Progress task {Id} '{Title}' was destroyed without Finish; it ends as Abandoned",
                 id, name);
}

} // namespace

// ---- ProgressHub ----

ProgressHub& ProgressHub::Get() noexcept {
    static constinit ProgressHub hub;
    return hub;
}

Result<void> ProgressHub::Initialize(Allocator& allocator,
                                     const ProgressHubSettings& settings) noexcept {
    if (settings.eventLogCapacity == 0 || settings.eventLogCapacity > MaxProgressEventLogCapacity) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ProgressHubState& hub = ProgressHubGlobal;
    platform::LockGuard lock(hub.mutex);
    if (HubPhase() == ProgressHubPhase::Running) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto live = allocator.Allocate(sizeof(ProgressTaskRecord*) * MaxLiveProgressTasks,
                                   alignof(ProgressTaskRecord*));
    if (!live) {
        return Result<void>::Failure(live.Error());
    }

    auto ring = allocator.Allocate(sizeof(ProgressEvent) * settings.eventLogCapacity,
                                   alignof(ProgressEvent));
    if (!ring) {
        (void)allocator.Deallocate(live.Value());
        return Result<void>::Failure(ring.Error());
    }

    hub.allocator = &allocator;
    hub.clock = settings.clock;
    hub.clockContext = settings.clockContext;
    hub.live = static_cast<ProgressTaskRecord**>(live.Value());
    hub.liveCount = 0;
    hub.ring = static_cast<ProgressEvent*>(ring.Value());
    hub.ringCapacity = settings.eventLogCapacity;
    for (uint32 index = 0; index < hub.ringCapacity; ++index) {
        (void)InitializeAt<ProgressEvent>(hub.ring + index);
    }

    hub.ringFirstSerial = hub.nextEventSerial;
    hub.warnedOnce.Store<platform::MemoryOrder::Relaxed>(0u);
    for (auto& count : hub.warningCounts) {
        count.Store<platform::MemoryOrder::Relaxed>(0u);
    }

    hub.pump = {};
    hub.uiGeneration.Store<platform::MemoryOrder::Relaxed>(0);
    (void)hub.epoch.FetchAdd<platform::MemoryOrder::Relaxed>(1);
    hub.phase.Store<platform::MemoryOrder::Release>(static_cast<uint32>(ProgressHubPhase::Running));
    return Result<void>::Success();
}

void ProgressHub::Shutdown() noexcept {
    ProgressHubState& hub = ProgressHubGlobal;
    uint32 abandoned = 0;
    {
        platform::LockGuard lock(hub.mutex);
        if (HubPhase() != ProgressHubPhase::Running) {
            return;
        }

        abandoned = hub.liveCount;
        ShutdownLocked();
    }

    // 로그는 잠금 밖에서 남긴다(sink가 hub를 다시 불러도 교착하지 않게).
    if (abandoned != 0) {
        (void)hub.warningCounts[static_cast<uint32>(detail::ProgressWarning::ShutdownAbandoned)]
            .FetchAdd<platform::MemoryOrder::Relaxed>(1u);
        Log::Warning(detail::LogProgress,
                     "Progress hub shut down with {Count} live tasks; they end as Abandoned",
                     abandoned);
    }
}

void ProgressHub::ShutdownLocked() noexcept {
    ProgressHubState& hub = ProgressHubGlobal;
    while (hub.liveCount != 0) {
        HubFinishTree(hub, 0, ProgressOutcome::Abandoned, Text());
    }

    hub.phase.Store<platform::MemoryOrder::Release>(
        static_cast<uint32>(ProgressHubPhase::ShutDown));
    hub.uiGeneration.Store<platform::MemoryOrder::Relaxed>(0);
    hub.pump = {};
    for (uint32 index = 0; index < hub.ringCapacity; ++index) {
        hub.ring[index].~ProgressEvent();
    }

    (void)hub.allocator->Deallocate(hub.ring);
    (void)hub.allocator->Deallocate(hub.live);
    hub.ring = nullptr;
    hub.ringCapacity = 0;
    hub.live = nullptr;
}

Result<ProgressTaskHandle> ProgressHub::Begin(const ProgressTaskDesc& desc) noexcept {
    if (desc.title == nullptr || !desc.title->IsValid() || !HubIsFinite(desc.weightInParent) ||
        desc.weightInParent <= 0.0 || !HubIsFinite(desc.selfWeight) || desc.selfWeight < 0.0 ||
        !HubIsFinite(desc.modalDelaySeconds) || desc.modalDelaySeconds < 0.0 ||
        (desc.parent != NoProgressTask && desc.externalCancel.CanBeCancelled())) {
        return Result<ProgressTaskHandle>::Failure(ErrorCode::InvalidArgument);
    }

    ProgressHubState& hub = ProgressHubGlobal;
    platform::LockGuard lock(hub.mutex);
    if (const ErrorCode error = HubStateError(); error != ErrorCode::None) {
        return Result<ProgressTaskHandle>::Failure(error);
    }

    Allocator& allocator = *hub.allocator;
    {
        auto source = detail::ProgressSourceText(allocator, *desc.title);
        if (!source) {
            return Result<ProgressTaskHandle>::Failure(source.Error());
        }

        if (source.Value().value.IsEmpty()) {
            return Result<ProgressTaskHandle>::Failure(ErrorCode::InvalidArgument);
        }
    }

    ProgressTaskRecord* parent = nullptr;
    if (desc.parent != NoProgressTask) {
        const uint32 index = HubFindLive(hub, desc.parent);
        if (index == hub.liveCount) {
            return Result<ProgressTaskHandle>::Failure(HubMissingIdError(desc.parent));
        }

        parent = hub.live[index];
    }

    if (hub.liveCount >= MaxLiveProgressTasks) {
        return Result<ProgressTaskHandle>::Failure(ErrorCode::CapacityExceeded);
    }

    auto title = desc.title->Clone();
    if (!title) {
        return Result<ProgressTaskHandle>::Failure(title.Error());
    }

    // 취소(7.4절): 하위 작업은 부모 token, 최상위 작업은 외부 token을 바탕으로 한다.
    const CancelToken base = parent != nullptr ? parent->token : desc.externalCancel;
    CancelSource source;
    if (desc.cancel == ProgressCancel::UserCancellable) {
        auto created = CancelSource::CreateLinked(allocator, base);
        if (!created) {
            return Result<ProgressTaskHandle>::Failure(created.Error());
        }

        source = Move(created.Value());
    }

    auto memory = allocator.Allocate(sizeof(ProgressTaskRecord), alignof(ProgressTaskRecord));
    if (!memory) {
        return Result<ProgressTaskHandle>::Failure(memory.Error());
    }

    auto* record = InitializeAt<ProgressTaskRecord>(memory.Value());
    record->allocator = &allocator;
    record->id = hub.nextTaskId.FetchAdd<platform::MemoryOrder::Relaxed>(1);
    record->parentId = parent != nullptr ? parent->id : NoProgressTask;
    record->epoch = hub.epoch.Load<platform::MemoryOrder::Relaxed>();
    record->title = Move(title.Value());
    record->presentation = desc.presentation;
    record->cancel = desc.cancel;
    record->weightInParent = desc.weightInParent;
    record->selfWeight = desc.selfWeight;
    record->modalDelaySeconds = desc.modalDelaySeconds;
    record->startNanoseconds = detail::ProgressNow();
    record->lastProgressNanoseconds = record->startNanoseconds;
    record->token = source.IsValid() ? source.Token() : base;
    record->source = Move(source);
    record->parent = parent;
    if (parent != nullptr) {
        ++parent->childCount;
    }

    // 살아 있는 목록이 하나, handle이 하나를 쥔다.
    hub.live[hub.liveCount++] = record;
    detail::RetainRecord(record);
    (void)HubPushEvent(hub, ProgressEventKind::Begin, *record);
    return Result<ProgressTaskHandle>::Success(ProgressTaskHandle(record));
}

Result<void> ProgressHub::RequestCancel(ProgressTaskId id) noexcept {
    ProgressHubState& hub = ProgressHubGlobal;
    platform::LockGuard lock(hub.mutex);
    if (const ErrorCode error = HubStateError(); error != ErrorCode::None) {
        return Result<void>::Failure(error);
    }

    const uint32 index = HubFindLive(hub, id);
    if (index == hub.liveCount) {
        return Result<void>::Failure(HubMissingIdError(id));
    }

    ProgressTaskRecord& record = *hub.live[index];
    if (record.cancel != ProgressCancel::UserCancellable) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    record.source.Cancel();
    return Result<void>::Success();
}

void ProgressHub::RequestCancelAllTopLevel() noexcept {
    ProgressHubState& hub = ProgressHubGlobal;
    platform::LockGuard lock(hub.mutex);
    if (HubPhase() != ProgressHubPhase::Running) {
        return;
    }

    for (uint32 index = 0; index < hub.liveCount; ++index) {
        ProgressTaskRecord& record = *hub.live[index];
        if (record.parent == nullptr && record.cancel == ProgressCancel::UserCancellable) {
            record.source.Cancel();
        }
    }
}

Result<void> ProgressHub::Snapshot(ProgressSnapshot& out, uint64& cursor) noexcept {
    HubClearSnapshot(out);
    ProgressHubState& hub = ProgressHubGlobal;
    platform::LockGuard lock(hub.mutex);
    if (const ErrorCode error = HubStateError(); error != ErrorCode::None) {
        return Result<void>::Failure(error);
    }

    auto taken = HubSnapshotLocked(hub, out, cursor);
    if (!taken) {
        HubClearSnapshot(out);
    }

    return taken;
}

Result<void> ProgressHub::SetUiPump(const ProgressUiPump& pump) noexcept {
    if (pump.pump == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ProgressHubState& hub = ProgressHubGlobal;
    detail::ProgressThreadState& thread = detail::ProgressThread;
    platform::LockGuard lock(hub.mutex);
    if (const ErrorCode error = HubStateError(); error != ErrorCode::None) {
        return Result<void>::Failure(error);
    }

    const uint64 current = hub.uiGeneration.Load<platform::MemoryOrder::Relaxed>();
    if (current != 0 && thread.uiGeneration != current) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (current == 0) {
        thread.uiGeneration = ++hub.uiGenerationSource;
        hub.lastPumpNanoseconds.Store<platform::MemoryOrder::Relaxed>(detail::ProgressNow());
    }

    hub.pump = pump;
    hub.uiGeneration.Store<platform::MemoryOrder::Relaxed>(thread.uiGeneration);
    return Result<void>::Success();
}

void ProgressHub::ClearUiPump() noexcept {
    ProgressHubState& hub = ProgressHubGlobal;
    detail::ProgressThreadState& thread = detail::ProgressThread;
    platform::LockGuard lock(hub.mutex);
    const uint64 current = hub.uiGeneration.Load<platform::MemoryOrder::Relaxed>();
    if (current == 0 || thread.uiGeneration != current) {
        return;
    }

    hub.uiGeneration.Store<platform::MemoryOrder::Relaxed>(0);
    hub.pump = {};
    thread.uiGeneration = 0;
}

ProgressHubStats ProgressHub::Stats() const noexcept {
    const auto count = [](detail::ProgressWarning kind) noexcept {
        return ProgressHubGlobal.warningCounts[static_cast<uint32>(kind)]
            .Load<platform::MemoryOrder::Relaxed>();
    };
    ProgressHubStats stats;
    stats.invalidWorkWarnings = count(detail::ProgressWarning::InvalidWork);
    stats.stepOverflowWarnings = count(detail::ProgressWarning::StepOverflow);
    stats.scopeDepthWarnings = count(detail::ProgressWarning::ScopeDepth);
    stats.messageCloneWarnings = count(detail::ProgressWarning::MessageClone);
    stats.abandonedTasks = count(detail::ProgressWarning::Abandoned);
    stats.shutdownAbandoned = count(detail::ProgressWarning::ShutdownAbandoned);
    return stats;
}

void ProgressHub::ReturnToUninitialized() noexcept {
    ProgressHubState& hub = ProgressHubGlobal;
    platform::LockGuard lock(hub.mutex);
    if (HubPhase() == ProgressHubPhase::ShutDown) {
        hub.phase.Store<platform::MemoryOrder::Release>(
            static_cast<uint32>(ProgressHubPhase::Uninitialized));
    }
}

// ---- ProgressTaskHandle ----

ProgressTaskHandle::ProgressTaskHandle(ProgressTaskHandle&& other) noexcept
    : record_(other.record_) {
    other.record_ = nullptr;
}

ProgressTaskHandle& ProgressTaskHandle::operator=(ProgressTaskHandle&& other) noexcept {
    if (this != &other) {
        Release();
        record_ = other.record_;
        other.record_ = nullptr;
    }

    return *this;
}

ProgressTaskHandle::~ProgressTaskHandle() noexcept {
    Release();
}

void ProgressTaskHandle::Release() noexcept {
    ProgressTaskRecord* record = record_;
    record_ = nullptr;
    if (record == nullptr) {
        return;
    }

    if (record->finished.Load<platform::MemoryOrder::Acquire>() == 0) {
        bool abandoned = false;
        {
            ProgressHubState& hub = ProgressHubGlobal;
            platform::LockGuard lock(hub.mutex);
            if (record->live) {
                const uint32 index = HubIndexOf(hub, record);
                if (index != hub.liveCount) {
                    HubFinishTree(hub, index, ProgressOutcome::Abandoned, Text());
                    abandoned = true;
                }
            }
        }

        if (abandoned) {
            HubWarnAbandoned(record->id, record->title, *record->allocator);
        }
    }

    detail::ReleaseRecord(record);
}

ProgressTaskId ProgressTaskHandle::Id() const noexcept {
    return record_ != nullptr ? record_->id : NoProgressTask;
}

CancelToken ProgressTaskHandle::Token() const noexcept {
    return record_ != nullptr ? record_->token : CancelToken();
}

Result<void> ProgressTaskHandle::SetMessage(const Text& message) noexcept {
    if (record_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!detail::IsCurrentEpoch(*record_)) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (record_->finished.Load<platform::MemoryOrder::Acquire>() != 0) {
        return Result<void>::Failure(ErrorCode::ScopeClosed);
    }

    Text replaced;
    if (auto cloned = HubCloneInto(replaced, message); !cloned) {
        return cloned;
    }

    {
        platform::LockGuard lock(record_->messageMutex);
        Text previous = Move(record_->taskMessage);
        record_->taskMessage = Move(replaced);
        replaced = Move(previous);
    }

    detail::MaybePumpUi();
    return Result<void>::Success();
}

Result<void> ProgressTaskHandle::Finish(ProgressOutcome outcome) noexcept {
    return FinishWith(outcome, nullptr);
}

Result<void> ProgressTaskHandle::Finish(ProgressOutcome outcome, const Text& detail) noexcept {
    return FinishWith(outcome, &detail);
}

Result<void> ProgressTaskHandle::FinishWith(ProgressOutcome outcome, const Text* detail) noexcept {
    if (record_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Text reason;
    ErrorCode cloneError = ErrorCode::None;
    if (detail != nullptr) {
        if (auto cloned = HubCloneInto(reason, *detail); !cloned) {
            cloneError = cloned.Error();
        }
    }

    ProgressHubState& hub = ProgressHubGlobal;
    platform::LockGuard lock(hub.mutex);
    if (!detail::IsCurrentEpoch(*record_)) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (!record_->live) {
        return Result<void>::Failure(ErrorCode::ScopeClosed);
    }

    const uint32 index = HubIndexOf(hub, record_);
    HubFinishTree(hub, index, outcome, Move(reason));
    return cloneError == ErrorCode::None ? Result<void>::Success()
                                         : Result<void>::Failure(cloneError);
}

} // namespace rocks

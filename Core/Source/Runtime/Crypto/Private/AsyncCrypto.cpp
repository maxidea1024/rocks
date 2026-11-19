#include <Crypto/AsyncCrypto.h>

#include "AsyncCryptoInternal.h"
#include "KeyAccess.h"
#include "SegmentStreamInternal.h"

#include <Core/Assert.h>
#include <Core/Function.h>
#include <Core/Move.h>
#include <Crypto/Bytes.h>
#include <Crypto/Random.h>
#include <Crypto/Secret.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>

namespace rocks::crypto {
namespace detail {

// SegmentCryptoTask의 결과 slot과 handle 배열을 제출 경로와 마지막 작업이 다룬다. slot은 마지막
// 작업의 capture 정리(terminal 게시 전) 또는 제출 스레드가 쓰므로, 모든 handle의 terminal을 본
// 스레드는 안전하게 읽는다.
struct SegmentCryptoAccess {
    struct Slot {
        SegmentCryptoTask::Outcome outcome;
        ErrorCode error;
        usize bytes;
    };

    static Slot Save(const SegmentCryptoTask& task) noexcept {
        return Slot{task.outcome_, task.error_, task.bytes_};
    }

    static void Restore(SegmentCryptoTask& task, const Slot& slot) noexcept {
        task.outcome_ = slot.outcome;
        task.error_ = slot.error;
        task.bytes_ = slot.bytes;
    }

    static void Reset(SegmentCryptoTask& task) noexcept {
        Restore(task, Slot{SegmentCryptoTask::Outcome::NotRun, ErrorCode::None, 0});
    }

    static void Complete(SegmentCryptoTask& task, usize bytes) noexcept {
        Restore(task, Slot{SegmentCryptoTask::Outcome::Completed, ErrorCode::None, bytes});
    }

    static void Fail(SegmentCryptoTask& task, ErrorCode error) noexcept {
        Restore(task, Slot{SegmentCryptoTask::Outcome::Failed, error, 0});
    }

    static void Cancel(SegmentCryptoTask& task) noexcept {
        Restore(task, Slot{SegmentCryptoTask::Outcome::Cancelled, ErrorCode::Cancelled, 0});
    }

    // 지난 제출의 handle을 놓고 새 handle로 바꾼다(지난 작업은 모두 terminal이다).
    static void ReplaceHandles(SegmentCryptoTask& task, WorkerTaskHandle* handles,
                               uint32 count) noexcept {
        for (uint32 index = 0; index < task.handleCount_; ++index) {
            task.handles_[index].Reset();
        }

        for (uint32 index = 0; index < count; ++index) {
            task.handles_[index] = Move(handles[index]);
        }

        task.handleCount_ = count;
    }
};

} // namespace detail

namespace {

using Access = detail::SegmentCryptoAccess;

// 세그먼트 크기의 최솟값(4 KiB)이다. 복호는 제출 때 헤더를 읽지 않으므로 이 값으로 세그먼트 수의
// 상한을 잡아 작업 수를 정한다.
constexpr uint64 MinSegmentBytes = uint64{1} << SegmentSizeLog2Min;

bool IsTerminal(WorkerTaskState state) noexcept {
    return state == WorkerTaskState::Succeeded || state == WorkerTaskState::Failed ||
           state == WorkerTaskState::Cancelled;
}

enum class JobKind : uint8 {
    Encrypt,
    Decrypt
};

// 복호 작업의 헤더 처리(commitment·마지막 세그먼트 검증) 단계다. 처음 시작한 작업 하나가 열고,
// 다른 작업은 opened 이벤트를 기다린다(여는 작업이 실행 중이므로 기다림은 그 처리 시간으로 묶인다).
enum OpenState : uint32 {
    NotOpened = 0,
    Opening = 1,
    Opened = 2,
    OpenFailed = 3
};

// 한 제출의 공유 상태다. 작업 capture마다 표(ticket) 하나와 제출 스레드의 표 하나가 참조하고,
// 마지막 표가 놓일 때 결과를 쓰고(Finalize) 해제한다. 모든 Span은 호출자가 결과까지 유지한다.
struct SegmentJob {
    SegmentCryptoTask* task = nullptr;
    JobKind kind = JobKind::Encrypt;
    Span<const uint8> input;
    Span<uint8> output;
    detail::SegmentCryptoProbe probe;
    SegmentLayout layout; // 암호화는 제출 때, 복호는 여는 작업이 정한다

    // 암호화: 제출 때 만든 writer와 헤더(성공하면 Finalize가 출력 앞에 쓴다).
    SegmentStreamParallelWriter writer;
    uint8 header[SegmentHeaderBytes] = {};

    // 복호: 마스터 키 사본과 열린 임의 접근 객체.
    SegmentStreamKey key;
    Span<const uint8> associatedData;
    SegmentStreamRandomAccess reader;
    platform::ManualResetEvent opened;
    uint32 openState = NotOpened;

    // 진행(원자 변수).
    uint64 nextSegment = 0;
    uint64 doneSegments = 0;
    uint32 stopReason = 0; // ErrorCode, 처음 기록한 값이 남는다
    uint32 tickets = 1;    // 제출 스레드의 표
    // 제출이 통째로 거부됐다. 마지막 표는 task·출력을 건드리지 않고 해제만 한다.
    bool aborted = false;
};

Result<SegmentJob*> TryCreateJob() noexcept {
    auto memory = GetSecretAllocator().Allocate(sizeof(SegmentJob), alignof(SegmentJob));
    if (!memory) {
        return Result<SegmentJob*>::Failure(memory.Error());
    }

    return Result<SegmentJob*>::Success(InitializeAt<SegmentJob>(memory.Value()));
}

void DestroyJob(SegmentJob* job) noexcept {
    DestroyAt(job);
    (void)GetSecretAllocator().Deallocate(job);
}

ErrorCode StopReason(const SegmentJob& job) noexcept {
    return static_cast<ErrorCode>(platform::AtomicLoad(&job.stopReason));
}

// 처음 기록한 이유만 남긴다(뒤의 이유는 앞 이유의 결과다).
void RecordStop(SegmentJob& job, ErrorCode reason) noexcept {
    uint32 expected = 0;
    (void)platform::AtomicCompareExchangeStrong(&job.stopReason, expected,
                                                static_cast<uint32>(reason));
}

// 결과를 정한다. 성공이 아니면 출력 전체를 지운다(2절). 마지막 표를 놓는 스레드(작업 capture 정리
// 또는 제출 스레드)에서 terminal 게시 전에 한 번 실행된다. 모든 세그먼트를 처리했으면 그 뒤에
// 기록된 멈춤 이유(늦게 걸린 취소 등)와 관계없이 완료다. 세그먼트 오류는 doneSegments를 올리지
// 않고, 복호 열기에 실패하면 segmentCount가 0이므로 이 조건만으로 성공을 가린다.
void Finalize(SegmentJob& job) noexcept {
    const ErrorCode reason = StopReason(job);
    const uint64 done = platform::AtomicLoad(&job.doneSegments);
    const bool complete = job.layout.segmentCount != 0 && done == job.layout.segmentCount;
    if (complete) {
        if (job.kind == JobKind::Encrypt) {
            platform::MemoryCopy(job.output.Data(), job.header, SegmentHeaderBytes);
            Access::Complete(*job.task, static_cast<usize>(job.layout.lastSegmentOffset +
                                                           job.layout.lastSegmentBytes));
        } else {
            Access::Complete(*job.task, static_cast<usize>(job.layout.plaintextBytes));
        }

        return;
    }

    SecureZero(job.output);
    if (reason == ErrorCode::Cancelled) {
        Access::Cancel(*job.task);
    } else if (reason != ErrorCode::None) {
        Access::Fail(*job.task, reason);
    } else {
        // 실행되지 않은 작업 때문에 끝나지 못했다(실행 전 취소·의존 실패). TryGetResult가 scheduler
        // 상태를 옮긴다.
        Access::Reset(*job.task);
    }
}

void ReleaseJob(SegmentJob* job) noexcept {
    if (platform::AtomicFetchSub(&job->tickets, uint32{1}) != 1) {
        return;
    }

    if (!job->aborted) {
        Finalize(*job);
    }

    DestroyJob(job);
}

// 작업 capture가 드는 표다. body가 실행되지 않고 capture만 정리돼도(취소·의존 실패·Shutdown) 표가
// 놓이므로 마지막 표가 늘 결과를 쓴다.
class JobTicket {
public:
    explicit JobTicket(SegmentJob* job) noexcept : job_(job) {
        platform::AtomicFetchAdd(&job_->tickets, uint32{1});
    }

    JobTicket(JobTicket&& other) noexcept : job_(other.job_) {
        other.job_ = nullptr;
    }

    JobTicket(const JobTicket&) = delete;
    JobTicket& operator=(const JobTicket&) = delete;
    JobTicket& operator=(JobTicket&&) = delete;

    ~JobTicket() noexcept {
        if (job_ != nullptr) {
            ReleaseJob(job_);
        }
    }

    SegmentJob& Job() const noexcept {
        return *job_;
    }

private:
    SegmentJob* job_;
};

// 세그먼트 경계의 취소 확인이다. 먼저 멈춘 작업이 있으면 바로 멈춘다.
bool ShouldStop(SegmentJob& job, WorkerTaskContext& context) noexcept {
    if (StopReason(job) != ErrorCode::None) {
        return true;
    }

    const bool probed = job.probe.check != nullptr && job.probe.check(job.probe.context);
    if (probed || context.IsCancellationRequested()) {
        RecordStop(job, ErrorCode::Cancelled);
        return true;
    }

    return false;
}

// 동기 사용자와 같은 순서로 연다: 헤더 사본 → Peek → 크기 배치 → TryOpen(commitment·마지막
// 세그먼트) → 출력 크기.
ErrorCode OpenReader(SegmentJob& job) noexcept {
    // 헤더는 지역 사본 하나만 본다(입력이 호출 중 바뀌어도 Peek와 TryOpen이 같은 byte를 본다).
    uint8 header[SegmentHeaderBytes];
    const usize headerBytes =
        job.input.Size() < SegmentHeaderBytes ? job.input.Size() : SegmentHeaderBytes;
    if (headerBytes != 0) {
        platform::MemoryCopy(header, job.input.Data(), headerBytes);
    }

    const Span<const uint8> headerView(header, headerBytes);
    auto info = TryPeekSegmentHeader(headerView);
    if (!info) {
        return info.Error();
    }

    auto layout = TryGetSegmentLayout(info.Value().Params(), job.input.Size());
    if (!layout) {
        return layout.Error();
    }

    const Span<const uint8> lastSegment =
        job.input.Mid(static_cast<usize>(layout.Value().lastSegmentOffset),
                      static_cast<usize>(layout.Value().lastSegmentBytes));
    auto reader = SegmentStreamRandomAccess::TryOpen(job.key, job.associatedData, headerView,
                                                     lastSegment, job.input.Size());
    if (!reader) {
        return reader.Error();
    }

    if (job.output.Size() < reader.Value().PlaintextBytes()) {
        return ErrorCode::CapacityExceeded;
    }

    job.reader = Move(reader.Value());
    job.layout = job.reader.Layout();
    return ErrorCode::None;
}

bool EnsureOpened(SegmentJob& job, WorkerTaskContext& context) noexcept {
    uint32 expected = NotOpened;
    if (platform::AtomicCompareExchangeStrong(&job.openState, expected, uint32{Opening})) {
        bool ok = false;
        if (!ShouldStop(job, context)) {
            const ErrorCode error = OpenReader(job);
            if (error == ErrorCode::None) {
                ok = true;
            } else {
                RecordStop(job, error);
            }
        }

        platform::AtomicStore(&job.openState, uint32{ok ? Opened : OpenFailed});
        job.opened.Signal();
        return ok;
    }

    if (job.opened.Wait(platform::InfiniteTimeout) != platform::WaitResult::Signaled) {
        platform::Fatal("SegmentCryptoTask: open event wait failed");
    }

    return platform::AtomicLoad(&job.openState) == Opened;
}

ErrorCode SealOne(SegmentJob& job, uint64 index) noexcept {
    const SegmentLayout& layout = job.layout;
    const uint64 plainPerSegment = layout.segmentBytes - SegmentTagBytes;
    const bool last = index + 1 == layout.segmentCount;
    const uint64 plainBytes = last ? layout.lastSegmentBytes - SegmentTagBytes : plainPerSegment;
    const Span<const uint8> plaintext =
        job.input.Mid(static_cast<usize>(index * plainPerSegment), static_cast<usize>(plainBytes));
    const Span<uint8> sealed =
        job.output.Mid(static_cast<usize>(SegmentHeaderBytes + index * layout.segmentBytes),
                       static_cast<usize>(plainBytes + SegmentTagBytes));
    auto written = job.writer.TrySealSegment(index, plaintext, sealed);
    return written ? ErrorCode::None : written.Error();
}

ErrorCode OpenOne(SegmentJob& job, uint64 index) noexcept {
    const SegmentLayout& layout = job.layout;
    const uint64 plainPerSegment = layout.segmentBytes - SegmentTagBytes;
    const bool last = index + 1 == layout.segmentCount;
    const uint64 sealedBytes = last ? layout.lastSegmentBytes : layout.segmentBytes;
    const Span<const uint8> sealed =
        job.input.Mid(static_cast<usize>(SegmentHeaderBytes + index * layout.segmentBytes),
                      static_cast<usize>(sealedBytes));
    const Span<uint8> plaintext = job.output.Mid(static_cast<usize>(index * plainPerSegment),
                                                 static_cast<usize>(sealedBytes - SegmentTagBytes));
    auto written = job.reader.TryOpenSegment(index, sealed, plaintext);
    return written ? ErrorCode::None : written.Error();
}

// 멈춘 이유를 scheduler 상태로 옮긴다. 그래야 이 작업에 이어 붙인 작업이 실행되지 않는다. 자기
// 취소 요청으로 멈췄으면 Fail하지 않아 Cancelled가 된다.
void ReportStop(SegmentJob& job, WorkerTaskContext& context) noexcept {
    const ErrorCode reason = StopReason(job);
    if (reason == ErrorCode::None) {
        return;
    }

    if (reason == ErrorCode::Cancelled && context.IsCancellationRequested()) {
        return;
    }

    context.Fail(reason);
}

void RunWorker(SegmentJob& job, WorkerTaskContext& context) noexcept {
    if (job.kind == JobKind::Decrypt && !EnsureOpened(job, context)) {
        ReportStop(job, context);
        return;
    }

    while (!ShouldStop(job, context)) {
        const uint64 index = platform::AtomicFetchAdd(&job.nextSegment, uint64{1});
        if (index >= job.layout.segmentCount) {
            break;
        }

        const ErrorCode error =
            job.kind == JobKind::Encrypt ? SealOne(job, index) : OpenOne(job, index);
        if (error != ErrorCode::None) {
            RecordStop(job, error);
            break;
        }

        platform::AtomicFetchAdd(&job.doneSegments, uint64{1});
    }

    ReportStop(job, context);
}

Result<void> CheckIdle(const SegmentCryptoTask& task) noexcept {
    for (const WorkerTaskHandle& handle : task.Handles()) {
        auto status = handle.Status();
        if (!status) {
            return Result<void>::Failure(status.Error());
        }

        if (!IsTerminal(status.Value().state)) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
    }

    return Result<void>::Success();
}

uint32 TaskCount(const WorkerScheduler& scheduler, const SegmentCryptoOptions& options,
                 uint64 segmentLimit) noexcept {
    uint32 count = options.maxTasks != 0 ? options.maxTasks : scheduler.Stats().workerCount;
    if (count > SegmentCryptoMaxTasks) {
        count = SegmentCryptoMaxTasks;
    }

    if (segmentLimit < count) {
        count = static_cast<uint32>(segmentLimit);
    }

    return count == 0 ? 1 : count;
}

// 작업을 제출한다. job은 제출 스레드의 표 하나를 가진 채로 넘어오고, 여기서 그 표를 놓는다.
Result<void> SubmitJob(WorkerScheduler& scheduler, SegmentCryptoTask& task, SegmentJob* job,
                       uint32 taskCount, Span<const WorkerTaskHandle> dependencies) noexcept {
    using Body = Function<void(WorkerTaskContext&)>;
    WorkerTaskHandle submitted[SegmentCryptoMaxTasks];
    uint32 accepted = 0;
    ErrorCode rejected = ErrorCode::None;

    // worker가 Submit 반환 전에 body를 실행할 수 있지만, 제출 스레드의 표가 남아 있는 동안 결과는
    // 쓰이지 않는다. 그래도 slot은 제출 전에 초기화하고 통째로 거부되면 되돌린다.
    const auto saved = Access::Save(task);
    Access::Reset(task);
    job->task = &task;
    for (uint32 index = 0; index < taskCount; ++index) {
        auto body = Body::TryCreate(GetSecretAllocator(),
                                    [ticket = JobTicket(job)](WorkerTaskContext& context) noexcept {
                                        RunWorker(ticket.Job(), context);
                                    });
        if (!body) {
            rejected = body.Error();
            break;
        }

        auto handle = scheduler.Submit(Move(body.Value()), dependencies);
        if (!handle) {
            rejected = handle.Error();
            break;
        }

        submitted[accepted] = Move(handle.Value());
        ++accepted;
    }

    if (accepted == 0) {
        // 받아들인 작업이 없으므로 이 표가 마지막이다. task·출력을 건드리지 않는다.
        job->aborted = true;
        Access::Restore(task, saved);
        ReleaseJob(job);
        return Result<void>::Failure(rejected);
    }

    Access::ReplaceHandles(task, submitted, accepted);
    ReleaseJob(job);
    return Result<void>::Success();
}

Result<void> SubmitEncrypt(WorkerScheduler& scheduler, SegmentCryptoTask& task,
                           const SegmentStreamKey& key, const SegmentStreamParams& params,
                           Span<const uint8> associatedData, Span<const uint8> plaintext,
                           Span<uint8> ciphertext, const SegmentCryptoOptions& options,
                           Span<const WorkerTaskHandle> dependencies, Span<const uint8> salt,
                           detail::SegmentCryptoProbe probe) noexcept {
    if (auto idle = CheckIdle(task); !idle) {
        return idle;
    }

    if (key.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (!detail::IsValidSegmentParams(params) || Overlaps(plaintext, ciphertext)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    uint8 randomSalt[detail::SegmentSaltBytes];
    if (salt.IsEmpty()) {
        if (auto filled = TryFillRandom(Span<uint8>(randomSalt)); !filled) {
            return Result<void>::Failure(filled.Error());
        }

        salt = Span<const uint8>(randomSalt);
    }

    auto created = TryCreateJob();
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    SegmentJob* job = created.Value();
    auto writer = detail::SegmentStreamFactory::TryCreateParallelWriter(
        key, params, associatedData, plaintext.Size(), salt, Span<uint8>(job->header),
        GetSecretAllocator());
    if (!writer) {
        DestroyJob(job);
        return Result<void>::Failure(writer.Error());
    }

    const SegmentLayout layout = writer.Value().Layout();
    if (ciphertext.Size() < layout.lastSegmentOffset + layout.lastSegmentBytes) {
        DestroyJob(job);
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    job->kind = JobKind::Encrypt;
    job->input = plaintext;
    job->output = ciphertext;
    job->probe = probe;
    job->layout = layout;
    job->writer = Move(writer.Value());
    return SubmitJob(scheduler, task, job, TaskCount(scheduler, options, layout.segmentCount),
                     dependencies);
}

Result<void> SubmitDecrypt(WorkerScheduler& scheduler, SegmentCryptoTask& task,
                           const SegmentStreamKey& key, Span<const uint8> associatedData,
                           Span<const uint8> ciphertext, Span<uint8> plaintext,
                           const SegmentCryptoOptions& options,
                           Span<const WorkerTaskHandle> dependencies,
                           detail::SegmentCryptoProbe probe) noexcept {
    if (auto idle = CheckIdle(task); !idle) {
        return idle;
    }

    if (key.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (Overlaps(ciphertext, plaintext)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto created = TryCreateJob();
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    // 마스터 키를 복사해 둔다(헤더는 작업이 시작할 때 읽는다). 호출자는 제출 뒤 키를 버려도 된다.
    SegmentJob* job = created.Value();
    auto copy = SegmentStreamKey::TryImport(key.Id(), detail::KeyAccess::Bytes(key));
    if (!copy) {
        DestroyJob(job);
        return Result<void>::Failure(copy.Error());
    }

    job->kind = JobKind::Decrypt;
    job->input = ciphertext;
    job->output = plaintext;
    job->probe = probe;
    job->key = Move(copy.Value());
    job->associatedData = associatedData;
    // 세그먼트는 4 KiB 이상이므로 본문 4 KiB마다 작업 하나를 넘지 않는다.
    const uint64 body =
        ciphertext.Size() > SegmentHeaderBytes ? ciphertext.Size() - SegmentHeaderBytes : 0;
    const uint64 segmentLimit = (body + MinSegmentBytes - 1) / MinSegmentBytes;
    return SubmitJob(scheduler, task, job, TaskCount(scheduler, options, segmentLimit),
                     dependencies);
}

} // namespace

SegmentCryptoTask::~SegmentCryptoTask() noexcept {
    bool pending = false;
    for (const WorkerTaskHandle& handle : Handles()) {
        auto status = handle.Status();
        if (!status || !IsTerminal(status.Value().state)) {
            pending = true;
        }
    }

    if (!pending) {
        return;
    }

    // 진행 중이면 취소를 요청하고 모두 끝날 때까지 기다린다(마지막 작업이 이 객체의 slot에 쓴다).
    // 같은 scheduler의 worker에서는 기다릴 수 없어 WrongThread가 나오며, 이는 수명 오용이므로
    // Fatal이다.
    (void)RequestCancel();
    for (const WorkerTaskHandle& handle : Handles()) {
        auto waited = handle.Wait(platform::InfiniteTimeout);
        ROCKS_ASSERT(waited.IsOk() && waited.Value() == platform::WaitResult::Signaled);
    }
}

Result<void> SegmentCryptoTask::RequestCancel() const noexcept {
    if (handleCount_ == 0) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    for (const WorkerTaskHandle& handle : Handles()) {
        if (auto requested = handle.RequestCancel(); !requested) {
            return requested;
        }
    }

    return Result<void>::Success();
}

Result<usize> SegmentCryptoTask::TryGetResult() const noexcept {
    if (handleCount_ == 0) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    bool cancelled = false;
    ErrorCode failure = ErrorCode::None;
    for (const WorkerTaskHandle& handle : Handles()) {
        auto status = handle.Status();
        if (!status) {
            return Result<usize>::Failure(status.Error());
        }

        const WorkerTaskStatus& value = status.Value();
        if (!IsTerminal(value.state)) {
            return Result<usize>::Failure(ErrorCode::Busy);
        }

        if (value.state == WorkerTaskState::Cancelled) {
            cancelled = true;
        } else if (value.state == WorkerTaskState::Failed && failure == ErrorCode::None) {
            failure = value.error;
        }
    }

    switch (outcome_) {
    case Outcome::Completed:
        return Result<usize>::Success(bytes_);
    case Outcome::Failed:
        return Result<usize>::Failure(error_);
    case Outcome::Cancelled:
        return Result<usize>::Failure(ErrorCode::Cancelled);
    case Outcome::NotRun:
        break;
    }

    // 실행되지 않은 작업이 있어 끝나지 못했다: 실행 전 취소·Shutdown(Cancel)·의존 실패.
    if (cancelled) {
        return Result<usize>::Failure(ErrorCode::Cancelled);
    }

    return Result<usize>::Failure(failure != ErrorCode::None ? failure
                                                             : ErrorCode::DependencyFailed);
}

Result<usize> SegmentCryptoTask::TryWaitResult(uint32 timeoutMs) const noexcept {
    if (handleCount_ == 0) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    constexpr uint64 NanosecondsPerMillisecond = 1000000;
    const uint64 start = platform::MonotonicNanoseconds();
    for (const WorkerTaskHandle& handle : Handles()) {
        uint32 remaining = timeoutMs;
        if (timeoutMs != platform::InfiniteTimeout) {
            const uint64 elapsed =
                (platform::MonotonicNanoseconds() - start) / NanosecondsPerMillisecond;
            remaining = elapsed >= timeoutMs ? 0 : static_cast<uint32>(timeoutMs - elapsed);
        }

        auto waited = handle.Wait(remaining);
        if (!waited) {
            return Result<usize>::Failure(waited.Error());
        }

        if (waited.Value() == platform::WaitResult::TimedOut) {
            return Result<usize>::Failure(ErrorCode::Busy);
        }

        if (waited.Value() != platform::WaitResult::Signaled) {
            return Result<usize>::Failure(ErrorCode::DependencyFailed);
        }
    }

    return TryGetResult();
}

Result<void> TrySubmitSegmentEncrypt(WorkerScheduler& scheduler, SegmentCryptoTask& task,
                                     const SegmentStreamKey& key, const SegmentStreamParams& params,
                                     Span<const uint8> associatedData, Span<const uint8> plaintext,
                                     Span<uint8> ciphertext, const SegmentCryptoOptions& options,
                                     Span<const WorkerTaskHandle> dependencies) noexcept {
    return SubmitEncrypt(scheduler, task, key, params, associatedData, plaintext, ciphertext,
                         options, dependencies, Span<const uint8>(), detail::SegmentCryptoProbe{});
}

Result<void> TrySubmitSegmentDecrypt(WorkerScheduler& scheduler, SegmentCryptoTask& task,
                                     const SegmentStreamKey& key, Span<const uint8> associatedData,
                                     Span<const uint8> ciphertext, Span<uint8> plaintext,
                                     const SegmentCryptoOptions& options,
                                     Span<const WorkerTaskHandle> dependencies) noexcept {
    return SubmitDecrypt(scheduler, task, key, associatedData, ciphertext, plaintext, options,
                         dependencies, detail::SegmentCryptoProbe{});
}

namespace detail {

Result<void>
TrySubmitSegmentEncryptWithSalt(WorkerScheduler& scheduler, SegmentCryptoTask& task,
                                const SegmentStreamKey& key, const SegmentStreamParams& params,
                                Span<const uint8> associatedData, Span<const uint8> plaintext,
                                Span<uint8> ciphertext, const SegmentCryptoOptions& options,
                                Span<const WorkerTaskHandle> dependencies, Span<const uint8> salt,
                                SegmentCryptoProbe probe) noexcept {
    if (salt.Size() != SegmentSaltBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return SubmitEncrypt(scheduler, task, key, params, associatedData, plaintext, ciphertext,
                         options, dependencies, salt, probe);
}

Result<void> TrySubmitSegmentDecryptWithProbe(WorkerScheduler& scheduler, SegmentCryptoTask& task,
                                              const SegmentStreamKey& key,
                                              Span<const uint8> associatedData,
                                              Span<const uint8> ciphertext, Span<uint8> plaintext,
                                              const SegmentCryptoOptions& options,
                                              Span<const WorkerTaskHandle> dependencies,
                                              SegmentCryptoProbe probe) noexcept {
    return SubmitDecrypt(scheduler, task, key, associatedData, ciphertext, plaintext, options,
                         dependencies, probe);
}

} // namespace detail

} // namespace rocks::crypto

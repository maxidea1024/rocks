#include "CompressionInternal.h"

#include <Compression/AsyncCompression.h>
#include <Core/Assert.h>
#include <Core/Function.h>
#include <Core/Move.h>

namespace rocks {

// CompressionTask의 결과 slot을 worker body와 제출 경로에서 다룬다. slot은 body가 쓴 뒤 scheduler가
// terminal을 게시하므로(terminal 관측은 body·capture 정리 이후) terminal을 본 스레드는 안전하게
// 읽는다.
struct CompressionTaskAccess {
    struct Slot {
        CompressionTask::Outcome outcome;
        ErrorCode error;
        usize bytes;
    };

    static Slot Save(const CompressionTask& task) noexcept {
        return Slot{task.outcome_, task.error_, task.bytes_};
    }

    static void Restore(CompressionTask& task, const Slot& slot) noexcept {
        task.outcome_ = slot.outcome;
        task.error_ = slot.error;
        task.bytes_ = slot.bytes;
    }

    static void Reset(CompressionTask& task) noexcept {
        Restore(task, Slot{CompressionTask::Outcome::NotRun, ErrorCode::None, 0});
    }

    static void Complete(CompressionTask& task, usize bytes) noexcept {
        Restore(task, Slot{CompressionTask::Outcome::Completed, ErrorCode::None, bytes});
    }

    static void Fail(CompressionTask& task, ErrorCode error) noexcept {
        Restore(task, Slot{CompressionTask::Outcome::Failed, error, 0});
    }

    static void Cancel(CompressionTask& task) noexcept {
        Restore(task, Slot{CompressionTask::Outcome::Cancelled, ErrorCode::Cancelled, 0});
    }

    static WorkerTaskHandle& Handle(CompressionTask& task) noexcept {
        return task.handle_;
    }
};

namespace {

bool IsTerminal(WorkerTaskState state) noexcept {
    return state == WorkerTaskState::Succeeded || state == WorkerTaskState::Failed ||
           state == WorkerTaskState::Cancelled;
}

bool IsCancellationRequested(void* context) noexcept {
    return static_cast<WorkerTaskContext*>(context)->IsCancellationRequested();
}

enum class Work : uint8 {
    CompressAppend,
    DecompressAppend,
    DecompressInto
};

// body가 capture하는 작업 설명이다. 모든 포인터·Span은 caller가 terminal까지 유지한다.
struct Job {
    CompressionTask* task = nullptr;
    Work work = Work::CompressAppend;
    CompressionFormat format = CompressionFormat::Zstd;
    int32 level = 0;
    Span<const uint8> source;
    Array<uint8>* output = nullptr;
    usize maxOutputBytes = 0;
    Span<uint8> destination;
    Allocator* scratch = nullptr;
    // 사전 작업이 가진 사전 handle 복사본이다(capture가 정리될 때 놓는다).
    SharedPtr<detail::DictionaryState> dictionary;
};

Result<usize> Execute(const Job& job, const detail::CompressionProbe& probe) noexcept {
    switch (job.work) {
    case Work::CompressAppend: {
        // 제출 전에 검증한 설정이다.
        auto settings = CompressionSettings::TryMake(job.format, job.level);
        ROCKS_ASSERT(settings.IsOk());
        return detail::CompressAppendOnce(settings.Value(), job.source, *job.output, *job.scratch,
                                          probe, job.dictionary);
    }
    case Work::DecompressAppend:
        return detail::DecompressAppendOnce(job.format, job.source, job.maxOutputBytes, *job.output,
                                            *job.scratch, probe, job.dictionary);
    case Work::DecompressInto:
        return detail::DecompressIntoOnce(job.format, job.source, job.destination, *job.scratch,
                                          probe, job.dictionary);
    }
    return Result<usize>::Failure(ErrorCode::InvalidArgument);
}

void Run(const Job& job, WorkerTaskContext& context) noexcept {
    detail::CompressionProbe probe{&IsCancellationRequested, &context};
    if (probe.IsCancelled()) {
        CompressionTaskAccess::Cancel(*job.task);
        return;
    }

    auto result = Execute(job, probe);
    if (result) {
        CompressionTaskAccess::Complete(*job.task, result.Value());
        return;
    }
    if (result.Error() == ErrorCode::Cancelled && context.IsCancellationRequested()) {
        // 출력은 코덱 구현이 이미 되돌렸다. Fail하지 않아 scheduler 상태가 Cancelled가 된다. 취소
        // 요청 없이 Cancelled가 나오면(예: 할당기가 돌려준 오류) 실패로 보고해 후속 작업이 실행되지
        // 않게 한다.
        CompressionTaskAccess::Cancel(*job.task);
        return;
    }

    // Fail하지 않으면 Succeeded가 되어 이 작업에 의존한 후속 작업이 잘못된 데이터로 실행된다.
    CompressionTaskAccess::Fail(*job.task, result.Error());
    context.Fail(result.Error());
}

Result<void> Submit(WorkerScheduler& scheduler, CompressionTask& task, const Job& job,
                    Allocator& allocator, Span<const WorkerTaskHandle> dependencies) noexcept {
    WorkerTaskHandle& handle = CompressionTaskAccess::Handle(task);
    if (handle.IsValid()) {
        auto status = handle.Status();
        if (!status) {
            return Result<void>::Failure(status.Error());
        }
        if (!IsTerminal(status.Value().state)) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
    }

    // body 할당이 실패하면 아무것도 바뀌지 않는다.
    auto body = Function<void(WorkerTaskContext&)>::TryCreate(
        allocator, [job](WorkerTaskContext& context) noexcept { Run(job, context); });
    if (!body) {
        return Result<void>::Failure(body.Error());
    }

    // worker가 Submit 반환 전에 body를 실행할 수 있으므로 slot은 Submit 전에 초기화한다. 거부되면
    // 되돌린다.
    const auto saved = CompressionTaskAccess::Save(task);
    CompressionTaskAccess::Reset(task);
    auto submitted = scheduler.Submit(Move(body.Value()), dependencies);
    if (!submitted) {
        CompressionTaskAccess::Restore(task, saved);
        return Result<void>::Failure(submitted.Error());
    }

    handle = Move(submitted.Value());
    return Result<void>::Success();
}

} // namespace

CompressionTask::~CompressionTask() noexcept {
    if (!handle_.IsValid()) {
        return;
    }

    auto status = handle_.Status();
    if (status && IsTerminal(status.Value().state)) {
        return;
    }

    // 진행 중이면 취소를 요청하고 끝날 때까지 기다린다(body가 이 객체의 slot에 쓰기 때문이다). 같은
    // scheduler의 worker에서는 기다릴 수 없어 WrongThread가 나오며, 이는 수명 오용이므로 Fatal이다.
    (void)handle_.RequestCancel();
    auto waited = handle_.Wait(platform::InfiniteTimeout);
    ROCKS_ASSERT(waited.IsOk() && waited.Value() == platform::WaitResult::Signaled);
}

Result<usize> CompressionTask::TryGetResult() const noexcept {
    if (!handle_.IsValid()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    auto status = handle_.Status();
    if (!status) {
        return Result<usize>::Failure(status.Error());
    }
    const WorkerTaskStatus& value = status.Value();
    if (!IsTerminal(value.state)) {
        return Result<usize>::Failure(ErrorCode::Busy);
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

    // body가 실행되지 않았다: 실행 전 취소·Shutdown(Cancel)·의존 실패. body는 항상 결과를
    // 기록하므로 실행 없이 Succeeded일 수 없다(순서 오류를 다른 오류로 가리지 않는다).
    ROCKS_ASSERT(value.state != WorkerTaskState::Succeeded);
    if (value.state == WorkerTaskState::Cancelled) {
        return Result<usize>::Failure(ErrorCode::Cancelled);
    }
    return Result<usize>::Failure(value.error != ErrorCode::None ? value.error
                                                                 : ErrorCode::DependencyFailed);
}

Result<usize> CompressionTask::TryWaitResult(uint32 timeoutMs) const noexcept {
    if (!handle_.IsValid()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    auto waited = handle_.Wait(timeoutMs);
    if (!waited) {
        return Result<usize>::Failure(waited.Error());
    }
    if (waited.Value() == platform::WaitResult::TimedOut) {
        return Result<usize>::Failure(ErrorCode::Busy);
    }
    if (waited.Value() != platform::WaitResult::Signaled) {
        return Result<usize>::Failure(ErrorCode::DependencyFailed);
    }

    return TryGetResult();
}

Result<void> TrySubmitCompress(WorkerScheduler& scheduler, CompressionTask& task,
                               const CompressionSettings& settings, Span<const uint8> source,
                               Array<uint8>& output,
                               Span<const WorkerTaskHandle> dependencies) noexcept {
    // output은 의존 작업이 worker에서 키우는 중일 수 있으므로 제출 시점에는 읽지 않는다. output과의
    // 겹침은 body가 실행 시점에 검사한다(TryCompressAppend와 같은 InvalidArgument).
    if (settings.Format() == CompressionFormat::Lz4 && source.Size() > Lz4MaxInputBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Job job;
    job.task = &task;
    job.work = Work::CompressAppend;
    job.format = settings.Format();
    job.level = settings.Level();
    job.source = source;
    job.output = &output;
    job.scratch = &output.GetAllocator();
    return Submit(scheduler, task, job, output.GetAllocator(), dependencies);
}

Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task,
                                 CompressionFormat format, Span<const uint8> source,
                                 usize maxOutputBytes, Array<uint8>& output,
                                 Span<const WorkerTaskHandle> dependencies) noexcept {
    // output은 제출 시점에 읽지 않는다(위와 같은 이유). 겹침은 body가 검사한다.
    if (source.Size() == 0 || !TryGetCompressionLevelRange(format)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Job job;
    job.task = &task;
    job.work = Work::DecompressAppend;
    job.format = format;
    job.source = source;
    job.output = &output;
    job.maxOutputBytes = maxOutputBytes;
    job.scratch = &output.GetAllocator();
    return Submit(scheduler, task, job, output.GetAllocator(), dependencies);
}

Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task,
                                 CompressionFormat format, Span<const uint8> source,
                                 Span<uint8> destination, Allocator& scratch,
                                 Span<const WorkerTaskHandle> dependencies) noexcept {
    if (source.Size() == 0 ||
        detail::Overlaps(source.Data(), source.Size(), destination.Data(), destination.Size()) ||
        !TryGetCompressionLevelRange(format)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Job job;
    job.task = &task;
    job.work = Work::DecompressInto;
    job.format = format;
    job.source = source;
    job.destination = destination;
    job.scratch = &scratch;
    return Submit(scheduler, task, job, scratch, dependencies);
}

Result<void> TrySubmitCompress(WorkerScheduler& scheduler, CompressionTask& task,
                               const CompressDictionary& dictionary, Span<const uint8> source,
                               Array<uint8>& output,
                               Span<const WorkerTaskHandle> dependencies) noexcept {
    const auto& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    Job job;
    job.task = &task;
    job.work = Work::CompressAppend;
    job.format = state->format;
    job.level = state->level;
    job.source = source;
    job.output = &output;
    job.scratch = &output.GetAllocator();
    job.dictionary = state;
    return Submit(scheduler, task, job, output.GetAllocator(), dependencies);
}

Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task,
                                 const DecompressDictionary& dictionary, Span<const uint8> source,
                                 usize maxOutputBytes, Array<uint8>& output,
                                 Span<const WorkerTaskHandle> dependencies) noexcept {
    const auto& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (source.Size() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Job job;
    job.task = &task;
    job.work = Work::DecompressAppend;
    job.format = state->format;
    job.source = source;
    job.output = &output;
    job.maxOutputBytes = maxOutputBytes;
    job.scratch = &output.GetAllocator();
    job.dictionary = state;
    return Submit(scheduler, task, job, output.GetAllocator(), dependencies);
}

Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task,
                                 const DecompressDictionary& dictionary, Span<const uint8> source,
                                 Span<uint8> destination, Allocator& scratch,
                                 Span<const WorkerTaskHandle> dependencies) noexcept {
    const auto& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (source.Size() == 0 ||
        detail::Overlaps(source.Data(), source.Size(), destination.Data(), destination.Size())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Job job;
    job.task = &task;
    job.work = Work::DecompressInto;
    job.format = state->format;
    job.source = source;
    job.destination = destination;
    job.scratch = &scratch;
    job.dictionary = state;
    return Submit(scheduler, task, job, scratch, dependencies);
}

} // namespace rocks

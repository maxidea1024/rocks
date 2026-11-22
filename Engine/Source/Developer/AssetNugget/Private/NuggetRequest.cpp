#include <AssetNugget/NuggetRequest.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks {

// 항목 작업 capture의 ticket이다. body가 실행되면 Run이 항목을 끝내고, scheduler가 body 없이
// capture를 정리하면(Shutdown(Cancel)·의존 실패) 소멸자가 그 항목을 Cancelled로 끝낸다 — 단, 제출이
// 성공해 무장(armed)된 항목만(제출 실패로 body가 버려질 때는 Start가 항목을 끝낸다).
struct NuggetItemTicket {
    NuggetItemTicket(NuggetRequest* owner, usize item) noexcept : request(owner), index(item) {
    }

    NuggetItemTicket(NuggetItemTicket&& other) noexcept
        : request(other.request), index(other.index) {
        other.request = nullptr;
    }

    NuggetItemTicket(const NuggetItemTicket&) = delete;
    NuggetItemTicket& operator=(const NuggetItemTicket&) = delete;

    ~NuggetItemTicket() noexcept {
        if (request != nullptr &&
            platform::AtomicLoad<platform::MemoryOrder::Acquire>(&request->armed_[index]) != 0) {
            request->Execute(index, true);
        }
    }

    void Run(bool cancelled) noexcept {
        NuggetRequest* owner = request;
        request = nullptr;
        owner->Execute(index, cancelled);
    }

    NuggetRequest* request;
    usize index;
};

NuggetRequest::~NuggetRequest() noexcept = default;

void NuggetRequest::RequestCancel() noexcept {
    platform::AtomicStore<platform::MemoryOrder::Release>(&cancelled_, uint32{1});
    if (hooks_.onCancel != nullptr) {
        hooks_.onCancel(hooks_.context);
    }
}

bool NuggetRequest::IsBusy() const noexcept {
    return IsRunning() || platform::AtomicLoad<platform::MemoryOrder::Acquire>(&registered_) != 0;
}

bool NuggetRequest::IsRunning() const noexcept {
    // 완료 handle은 외부 이벤트다 — Trigger(또는 scheduler 취소) 전에는 Pending이다. 유효한
    // handle의 Status는 실패하지 않는다.
    return done_.IsValid() && done_.Handle().Status().Value().state == WorkerTaskState::Pending;
}

bool NuggetRequest::IsCancelRequested() const noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Acquire>(&cancelled_) != 0;
}

Result<void> NuggetRequest::CheckDone() const noexcept {
    if (!done_.IsValid()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    return IsRunning() ? Result<void>::Failure(ErrorCode::Busy) : Result<void>::Success();
}

void NuggetRequest::WaitForCompletion() noexcept {
    if (!IsRunning()) {
        return;
    }

    RequestCancel();
    if (scheduler_->IsCurrentWorker()) {
        // 같은 scheduler의 worker는 남은 항목을 기다릴 수 없다(교착) — 수명 오용이다.
        platform::Fatal("NuggetRequest destroyed on a worker of its scheduler while running");
    }

    (void)done_.Handle().Wait(platform::InfiniteTimeout);
}

Result<void> NuggetRequest::Begin(NuggetCache& cache, WorkerScheduler& scheduler, usize count,
                                  bool put, bool& registered) noexcept {
    registered = false;
    Array<uint32> armed(armed_.GetAllocator());
    if (auto sized = armed.TryResize(count); !sized) {
        return sized;
    }

    auto event = scheduler.TryCreateEvent();
    if (!event) {
        return Result<void>::Failure(event.Error());
    }

    armed_ = Move(armed);
    done_ = Move(event.Value());
    cache_ = &cache;
    scheduler_ = &scheduler;
    put_ = put;
    cancelled_ = 0;
    remaining_ = static_cast<uint32>(count + 1);
    registered = cache.Register(*this);
    if (!registered) {
        // 종료한 캐시다 — 항목은 Cancelled(결과 slot 초기값), 제출 표지만 남긴다.
        remaining_ = 1;
    }

    return Result<void>::Success();
}

Result<void> NuggetRequest::Submit(usize index) noexcept {
    auto body = Function<void(WorkerTaskContext&)>::TryCreate(
        armed_.GetAllocator(),
        [ticket = NuggetItemTicket(this, index)](WorkerTaskContext& context) mutable noexcept {
            ticket.Run(context.IsCancellationRequested());
        });
    if (!body) {
        return Result<void>::Failure(body.Error());
    }

    // 무장한 뒤 낸다. 거부되면 무장을 풀어 버려지는 capture가 항목을 끝내지 않게 한다.
    platform::AtomicStore<platform::MemoryOrder::Release>(&armed_[index], uint32{1});
    auto submitted = scheduler_->Submit(Move(body.Value()));
    if (!submitted) {
        platform::AtomicStore<platform::MemoryOrder::Release>(&armed_[index], uint32{0});
        return Result<void>::Failure(submitted.Error());
    }

    return Result<void>::Success();
}

void NuggetRequest::Execute(usize index, bool cancelled) noexcept {
    if (cancelled || platform::AtomicLoad<platform::MemoryOrder::Acquire>(&cancelled_) != 0) {
        CancelItem(index);
    } else {
        RunItem(index);
    }

    FinishOne();
}

void NuggetRequest::FinishOne() noexcept {
    if (platform::AtomicFetchSub<platform::MemoryOrder::AcquireRelease>(&remaining_, 1) != 1) {
        return;
    }

    // 마지막이다: 목록에서 빼고 완료한다(완료 뒤에는 이 객체를 건드리지 않는다 — 소유자가 소멸할 수
    // 있다).
    if (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&registered_) != 0) {
        cache_->Unregister(*this);
    }

    (void)done_.TryTrigger();
}

NuggetCacheGetResult NuggetRequest::CacheGet(const NuggetCacheGet& request) noexcept {
    return cache_->Fetch(request, true);
}

NuggetCachePutResult NuggetRequest::CachePut(const NuggetCachePut& request) noexcept {
    return cache_->Store(request, true);
}

bool NuggetRequest::ReserveBytes(uint64 bytes) noexcept {
    return cache_->ReservePutBytes(bytes);
}

void NuggetRequest::ReleaseBytes(uint64 bytes) noexcept {
    cache_->ReleasePutBytes(bytes);
}

namespace {

Result<void> CopyInputs(Span<const NuggetInput> source, Array<NuggetInput>& out) noexcept {
    if (auto reserved = out.TryReserve(source.Size()); !reserved) {
        return reserved;
    }

    for (const NuggetInput& input : source) {
        NuggetInput copy(out.GetAllocator());
        if (auto assigned = copy.bucket.TryAssign(input.bucket.View()); !assigned) {
            return assigned;
        }

        copy.key = input.key;
        copy.digest = input.digest;
        (void)out.TryPushBack(Move(copy));
    }

    return Result<void>::Success();
}

} // namespace

NuggetGetRequest::NuggetGetRequest(Allocator& allocator) noexcept
    : NuggetRequest(allocator), items_(allocator), results_(allocator) {
}

NuggetGetRequest::~NuggetGetRequest() noexcept {
    WaitForCompletion();
}

Result<void> NuggetGetRequest::Start(NuggetCache& cache, Span<const NuggetGetItem> items,
                                     WorkerScheduler& scheduler) noexcept {
    if (IsBusy()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    Allocator& allocator = items_.GetAllocator();
    items_.Clear();
    results_.Clear();
    auto fail = [&](ErrorCode error) noexcept {
        items_.Clear();
        results_.Clear();
        return Result<void>::Failure(error);
    };
    if (auto reserved = items_.TryReserve(items.Size()); !reserved) {
        return fail(reserved.Error());
    }

    if (auto reserved = results_.TryReserve(items.Size()); !reserved) {
        return fail(reserved.Error());
    }

    for (const NuggetGetItem& item : items) {
        Stored stored(allocator);
        stored.key = item.key;
        stored.policy = item.policy;
        if (auto assigned = stored.bucket.TryAssign(item.bucket); !assigned) {
            return fail(assigned.Error());
        }

        if (auto appended = stored.values.TryAppend(item.values); !appended) {
            return fail(appended.Error());
        }

        (void)items_.TryPushBack(Move(stored));
        NuggetCacheGetResult result(allocator);
        result.status = NuggetStatus::Cancelled;
        (void)results_.TryPushBack(Move(result));
    }

    bool registered = false;
    if (auto begun = Begin(cache, scheduler, items.Size(), false, registered); !begun) {
        return fail(begun.Error());
    }

    for (usize index = 0; registered && index < items.Size(); ++index) {
        if (auto submitted = Submit(index); !submitted) {
            // 이 항목과 뒤 항목은 내지 못했다(Dropped — 그 오류, 실패 아님).
            for (usize rest = index; rest < items.Size(); ++rest) {
                results_[rest].status = NuggetStatus::Dropped;
                results_[rest].error = submitted.Error();
                FinishOne();
            }

            break;
        }
    }

    FinishOne();
    return Result<void>::Success();
}

void NuggetGetRequest::RunItem(usize index) noexcept {
    const Stored& item = items_[index];
    results_[index] =
        CacheGet(NuggetCacheGet{item.bucket.View(), item.key, item.values.View(), item.policy});
}

void NuggetGetRequest::CancelItem(usize index) noexcept {
    results_[index].status = NuggetStatus::Cancelled;
}

Result<Span<const NuggetCacheGetResult>> NuggetGetRequest::Results() const noexcept {
    if (auto done = CheckDone(); !done) {
        return Result<Span<const NuggetCacheGetResult>>::Failure(done.Error());
    }

    return Result<Span<const NuggetCacheGetResult>>::Success(results_.View());
}

Result<void> NuggetGetRequest::Outcome() const noexcept {
    if (auto done = CheckDone(); !done) {
        return done;
    }

    for (const NuggetCacheGetResult& result : results_) {
        if (result.failed && cache_->IsRequired()) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }

    return Result<void>::Success();
}

NuggetPutRequest::NuggetPutRequest(Allocator& allocator) noexcept
    : NuggetRequest(allocator), items_(allocator), results_(allocator) {
}

NuggetPutRequest::~NuggetPutRequest() noexcept {
    WaitForCompletion();
}

Result<void> NuggetPutRequest::Start(NuggetCache& cache, Span<NuggetPutItem> items,
                                     WorkerScheduler& scheduler) noexcept {
    if (IsBusy()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    Allocator& allocator = items_.GetAllocator();
    items_.Clear();
    results_.Clear();
    // 값 버퍼를 옮긴 뒤 실패하면 항목에 되돌린다.
    auto fail = [&](ErrorCode error) noexcept {
        for (usize index = 0; index < items_.Size(); ++index) {
            if (!items_[index].values.IsEmpty()) {
                items[index].values = Move(items_[index].values);
            }
        }

        items_.Clear();
        results_.Clear();
        return Result<void>::Failure(error);
    };
    if (auto reserved = items_.TryReserve(items.Size()); !reserved) {
        return fail(reserved.Error());
    }

    if (auto reserved = results_.TryReserve(items.Size()); !reserved) {
        return fail(reserved.Error());
    }

    for (const NuggetPutItem& item : items) {
        Stored stored(allocator);
        stored.key = item.key;
        stored.policy = item.policy;
        Result<void> step = stored.bucket.TryAssign(item.bucket);
        if (step) {
            step = CopyInputs(item.inputs, stored.inputs);
        }

        if (step) {
            step = stored.meta.TryAppend(item.meta);
        }

        if (!step) {
            return fail(step.Error());
        }

        (void)items_.TryPushBack(Move(stored));
        (void)results_.TryPushBack(NuggetCachePutResult{NuggetStatus::Cancelled});
    }

    // 복사가 다 됐다 — 값 버퍼를 옮긴다(할당 없음).
    for (usize index = 0; index < items.Size(); ++index) {
        items_[index].values = Move(items[index].values);
        for (const NuggetValue& value : items_[index].values) {
            items_[index].reserved += value.bytes.Size();
        }
    }

    bool registered = false;
    if (auto begun = Begin(cache, scheduler, items.Size(), true, registered); !begun) {
        return fail(begun.Error());
    }

    bool rejected = false;
    ErrorCode rejection = ErrorCode::None;
    for (usize index = 0; registered && index < items.Size(); ++index) {
        Stored& item = items_[index];
        auto drop = [&](ErrorCode error) noexcept {
            results_[index].status = NuggetStatus::Dropped;
            results_[index].error = error;
            FinishOne();
        };
        if (rejected) {
            drop(rejection);
            continue;
        }

        if (!ReserveBytes(item.reserved)) {
            // 대기 Put byte 상한 — 메모리 보호(실패 아님).
            item.reserved = 0;
            drop(ErrorCode::CapacityExceeded);
            continue;
        }

        if (auto submitted = Submit(index); !submitted) {
            ReleaseBytes(item.reserved);
            item.reserved = 0;
            rejected = true;
            rejection = submitted.Error();
            drop(rejection);
        }
    }

    if (!registered) {
        for (Stored& item : items_) {
            item.reserved = 0;
        }
    }

    FinishOne();
    return Result<void>::Success();
}

void NuggetPutRequest::RunItem(usize index) noexcept {
    Stored& item = items_[index];
    results_[index] = CachePut(NuggetCachePut{item.bucket.View(), item.key, item.values.View(),
                                              item.inputs.View(), item.meta.View(), item.policy});
    ReleaseBytes(item.reserved);
    item.reserved = 0;
}

void NuggetPutRequest::CancelItem(usize index) noexcept {
    Stored& item = items_[index];
    results_[index] = NuggetCachePutResult{NuggetStatus::Cancelled};
    ReleaseBytes(item.reserved);
    item.reserved = 0;
}

Result<Span<const NuggetCachePutResult>> NuggetPutRequest::Results() const noexcept {
    if (auto done = CheckDone(); !done) {
        return Result<Span<const NuggetCachePutResult>>::Failure(done.Error());
    }

    return Result<Span<const NuggetCachePutResult>>::Success(results_.View());
}

Result<void> NuggetPutRequest::Outcome() const noexcept {
    if (auto done = CheckDone(); !done) {
        return done;
    }

    for (const NuggetCachePutResult& result : results_) {
        if (result.failed && cache_->IsRequired()) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks

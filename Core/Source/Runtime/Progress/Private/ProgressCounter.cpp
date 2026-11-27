#include "ProgressInternal.h"

#include <Progress/ProgressTaskHandle.h>

namespace rocks {

ProgressCounter::ProgressCounter(ProgressCounter&& other) noexcept
    : record_(other.record_), kind_(other.kind_) {
    other.record_ = nullptr;
}

ProgressCounter& ProgressCounter::operator=(ProgressCounter&& other) noexcept {
    if (this != &other) {
        Release();
        record_ = other.record_;
        kind_ = other.kind_;
        other.record_ = nullptr;
    }

    return *this;
}

ProgressCounter::~ProgressCounter() noexcept {
    Release();
}

void ProgressCounter::Release() noexcept {
    if (record_ == nullptr) {
        return;
    }

    if (kind_ == Kind::Scope) {
        record_->scopeCounterLive.Store<platform::MemoryOrder::Release>(0u);
    }

    detail::ReleaseRecord(record_);
    record_ = nullptr;
}

Result<ProgressCounter> ProgressCounter::ForTask(const ProgressTaskHandle& task,
                                                 uint64 total) noexcept {
    detail::ProgressTaskRecord* record = task.record_;
    if (record == nullptr) {
        return Result<ProgressCounter>::Failure(ErrorCode::InvalidArgument);
    }

    if (!detail::IsCurrentEpoch(*record)) {
        return Result<ProgressCounter>::Failure(ErrorCode::ShuttingDown);
    }

    if (record->finished.Load<platform::MemoryOrder::Acquire>() != 0) {
        return Result<ProgressCounter>::Failure(ErrorCode::ScopeClosed);
    }

    uint32 expected = 0;
    if (!record->taskCounterClaimed.CompareExchangeStrong<platform::MemoryOrder::AcquireRelease,
                                                          platform::MemoryOrder::Acquire>(expected,
                                                                                          1u)) {
        return Result<ProgressCounter>::Failure(ErrorCode::AlreadyExists);
    }

    record->taskDone.Store<platform::MemoryOrder::Relaxed>(0);
    record->taskTotal.Store<platform::MemoryOrder::Relaxed>(total);
    record->hasTaskCounter.Store<platform::MemoryOrder::Release>(1u);
    record->selfReported.Store<platform::MemoryOrder::Relaxed>(1u);
    detail::RetainRecord(record);
    return Result<ProgressCounter>::Success(ProgressCounter(record, Kind::Task));
}

void ProgressCounter::Add(uint64 done) noexcept {
    if (record_ == nullptr || record_->finished.Load<platform::MemoryOrder::Relaxed>() != 0) {
        return;
    }

    auto& target = kind_ == Kind::Task ? record_->taskDone : record_->scopeDone;
    (void)target.FetchAdd<platform::MemoryOrder::Relaxed>(done);
    detail::MaybePumpUi();
}

void ProgressCounter::AddTotal(uint64 more) noexcept {
    if (record_ == nullptr || record_->finished.Load<platform::MemoryOrder::Relaxed>() != 0) {
        return;
    }

    auto& target = kind_ == Kind::Task ? record_->taskTotal : record_->scopeTotal;
    (void)target.FetchAdd<platform::MemoryOrder::Relaxed>(more);
    detail::MaybePumpUi();
}

} // namespace rocks

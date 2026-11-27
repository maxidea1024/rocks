#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Engine/CountedPendingWork.h>
#include <Engine/EngineLog.h>
#include <Progress/ProgressCounter.h>
#include <Progress/ProgressScope.h>

// 수와 일 구간으로 추적하는 대기 작업 종류다(Docs/Design/ProgressReporting 8장 PG5 연결). 규칙은
// 공개 헤더 주석을 따른다.
namespace rocks {

using platform::MemoryOrder;

CountedPendingWork::WorkScope::WorkScope(WorkScope&& other) noexcept : owner_(other.owner_) {
    other.owner_ = nullptr;
}

CountedPendingWork::WorkScope&
CountedPendingWork::WorkScope::operator=(WorkScope&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = other.owner_;
        other.owner_ = nullptr;
    }

    return *this;
}

CountedPendingWork::WorkScope::~WorkScope() noexcept {
    Release();
}

void CountedPendingWork::WorkScope::Release() noexcept {
    if (owner_ != nullptr) {
        owner_->EndWork();
        owner_ = nullptr;
    }
}

CountedPendingWork::CountedPendingWork() noexcept = default;

CountedPendingWork::~CountedPendingWork() noexcept {
    if (active_.Load<MemoryOrder::Acquire>() != 0) {
        Log::Error(LogEngine, "Pending work {Name} destroyed with {Count} open work scope(s)",
                   name_, static_cast<uint64>(active_.Load<MemoryOrder::Acquire>()));
    }
}

Result<void> CountedPendingWork::Initialize(Allocator& allocator, StringView name,
                                            const Text& displayName,
                                            Span<const StringView> runBefore) noexcept {
    if (name.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (initialized_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto text = displayName.Clone();
    if (!text) {
        return Result<void>::Failure(text.Error());
    }

    auto source = CancelSource::Create(allocator);
    if (!source) {
        return Result<void>::Failure(source.Error());
    }

    name_ = name;
    runBefore_ = runBefore;
    displayName_ = Move(text.Value());
    cancel_ = Move(source.Value());
    initialized_ = true;
    return Result<void>::Success();
}

void CountedPendingWork::AddWork(uint64 count) noexcept {
    if (count == 0) {
        return;
    }

    (void)added_.FetchAdd<MemoryOrder::AcquireRelease>(count);
    if (waiters_.Load<MemoryOrder::Acquire>() != 0) {
        platform::LockGuard lock(gate_);
        changed_.NotifyAll();
    }
}

void CountedPendingWork::FinishWork(uint64 count) noexcept {
    if (count == 0) {
        return;
    }

    (void)finished_.FetchAdd<MemoryOrder::AcquireRelease>(count);
    // 기다리는 FinishAll이 있을 때만 깨운다(진행 보고용 — 끝남 판정은 EndWork가 깨운다).
    if (waiters_.Load<MemoryOrder::Acquire>() != 0) {
        platform::LockGuard lock(gate_);
        changed_.NotifyAll();
    }
}

CountedPendingWork::WorkScope CountedPendingWork::BeginWork() noexcept {
    platform::LockGuard lock(gate_);
    (void)active_.FetchAdd<MemoryOrder::AcquireRelease>(1u);
    return WorkScope(this);
}

void CountedPendingWork::EndWork() noexcept {
    platform::LockGuard lock(gate_);
    (void)active_.FetchSub<MemoryOrder::AcquireRelease>(1u);
    changed_.NotifyAll();
}

CancelToken CountedPendingWork::Token() const noexcept {
    return cancel_.Token();
}

uint32 CountedPendingWork::ActiveWork() const noexcept {
    return active_.Load<MemoryOrder::Acquire>();
}

StringView CountedPendingWork::Name() const noexcept {
    return name_;
}

const Text& CountedPendingWork::DisplayName() const noexcept {
    return displayName_;
}

Span<const StringView> CountedPendingWork::RunBefore() const noexcept {
    return runBefore_;
}

uint64 CountedPendingWork::RemainingCount() const noexcept {
    // 끝난 수를 먼저 읽는다(더한 수가 뒤에 읽혀 더 커도 남은 수가 음수로 보이지 않는다).
    const uint64 finished = finished_.Load<MemoryOrder::Acquire>();
    const uint64 added = added_.Load<MemoryOrder::Acquire>();
    const uint64 remaining = added > finished ? added - finished : 0;
    if (remaining == 0 && active_.Load<MemoryOrder::Acquire>() != 0) {
        return 1;
    }

    return remaining;
}

Result<void> CountedPendingWork::FinishFor(Span<Object* const> targets) noexcept {
    (void)targets;
    return Result<void>::Success();
}

void CountedPendingWork::CancelFor(Span<Object* const> targets) noexcept {
    (void)targets;
}

Result<void> CountedPendingWork::FinishAll() noexcept {
    // 등록소가 바인딩한 하위 작업(없으면 아무 일도 하지 않는 구간)의 한 구간을 계수로 채운다.
    ProgressScope scope(1.0);
    scope.EnterStep(1.0);
    uint64 seenAdded = added_.Load<MemoryOrder::Acquire>();
    uint64 seenFinished = finished_.Load<MemoryOrder::Acquire>();
    auto counter = scope.MakeCounter(seenAdded > seenFinished ? seenAdded - seenFinished : 0);
    (void)waiters_.FetchAdd<MemoryOrder::AcquireRelease>(1u);
    while (true) {
        // 계수는 잠금 밖에서 올린다(Add가 UI 펌프 hook — 외부 코드 — 를 부를 수 있다).
        const uint64 added = added_.Load<MemoryOrder::Acquire>();
        const uint64 finished = finished_.Load<MemoryOrder::Acquire>();
        if (counter) {
            if (added > seenAdded) {
                counter.Value().AddTotal(added - seenAdded);
            }

            if (finished > seenFinished) {
                counter.Value().Add(finished - seenFinished);
            }
        }

        seenAdded = added > seenAdded ? added : seenAdded;
        seenFinished = finished > seenFinished ? finished : seenFinished;
        platform::LockGuard lock(gate_);
        if (active_.Load<MemoryOrder::Acquire>() == 0) {
            break;
        }

        // 읽은 뒤 바뀌었으면 기다리지 않고 다시 보고한다(깨움을 놓치지 않게 — 바꾸는 쪽은 gate_를
        // 잡고 깨운다).
        if (added_.Load<MemoryOrder::Acquire>() != added ||
            finished_.Load<MemoryOrder::Acquire>() != finished) {
            continue;
        }

        (void)changed_.Wait(gate_);
    }

    (void)waiters_.FetchSub<MemoryOrder::AcquireRelease>(1u);
    return Result<void>::Success();
}

void CountedPendingWork::Shutdown() noexcept {
    cancel_.Cancel();
    platform::LockGuard lock(gate_);
    while (active_.Load<MemoryOrder::Acquire>() != 0) {
        (void)changed_.Wait(gate_);
    }
}

void CountedPendingWork::ApplyCompleted(const PendingWorkPumpParams& params) noexcept {
    (void)params;
}

} // namespace rocks

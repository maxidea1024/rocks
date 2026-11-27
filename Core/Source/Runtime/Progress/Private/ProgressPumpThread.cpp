#include "ProgressInternal.h"

#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressPumpThread.h>

namespace rocks {

namespace {

constexpr const char* PumpThreadName = "ProgressPump";

} // namespace

// 스레드와 멈춤 표지다(따로 할당 — Thread는 이동할 수 없고 크다).
struct ProgressPumpThread::State {
    platform::Thread thread;
    platform::ManualResetEvent stop;
    platform::Atomic<uint32> stopping{0};
};

ProgressPumpThread::ProgressPumpThread() noexcept = default;

ProgressPumpThread::~ProgressPumpThread() noexcept {
    Stop();
}

Result<void> ProgressPumpThread::Start(Allocator& allocator,
                                       const ProgressPumpThreadDesc& desc) noexcept {
    if (desc.pump == nullptr || desc.intervalMilliseconds == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (state_ != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    desc_ = desc;
    allocator_ = &allocator;
    auto memory = allocator.Allocate(sizeof(State), alignof(State));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    state_ = InitializeAt<State>(memory.Value());
    platform::ThreadOptions options;
    options.name = PumpThreadName;
    if (state_->thread.Start(&Run, this, options) != platform::ThreadStatus::Success) {
        state_->~State();
        (void)allocator.Deallocate(state_);
        state_ = nullptr;
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

void ProgressPumpThread::Stop() noexcept {
    if (state_ == nullptr) {
        return;
    }

    state_->stopping.Store<platform::MemoryOrder::Release>(1u);
    state_->stop.Signal();
    (void)state_->thread.Join();
    state_->~State();
    (void)allocator_->Deallocate(state_);
    state_ = nullptr;
}

void ProgressPumpThread::Tick(uint64 nowNanoseconds) noexcept {
    if (desc_.pump != nullptr) {
        desc_.pump(desc_.context, nowNanoseconds);
    }

    if (cancelRequests_ == 0 && desc_.terminationRequested != nullptr &&
        desc_.terminationRequested(desc_.terminationContext)) {
        cancelRequests_ = 1;
        ProgressHub::Get().RequestCancelAllTopLevel();
    }
}

void ProgressPumpThread::Run(void* context) noexcept {
    auto& self = *static_cast<ProgressPumpThread*>(context);
    State& state = *self.state_;
    while (state.stopping.Load<platform::MemoryOrder::Acquire>() == 0) {
        (void)state.stop.Wait(self.desc_.intervalMilliseconds);
        if (state.stopping.Load<platform::MemoryOrder::Acquire>() != 0) {
            break;
        }

        self.Tick(platform::MonotonicNanoseconds());
    }

    // 멈출 때 마지막 Tick 한 번(끝 사건 줄이 빠지지 않게).
    self.Tick(platform::MonotonicNanoseconds());
}

} // namespace rocks

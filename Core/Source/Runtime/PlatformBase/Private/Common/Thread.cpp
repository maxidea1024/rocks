#include "ThreadInternal.h"
#include "WaitDeadline.h"
#include <Platform/Fatal.h>
#include <new>

namespace rocks::platform::detail {
namespace {

// OS thread id 재사용과 무관하게 자기 실행을 식별한다. pointer는 TLS 종료 정리까지 유지한다.
thread_local ThreadState* executingThread = nullptr;

} // namespace

bool IsExecutingThread(const ThreadState& state) noexcept {
    return executingThread == &state;
}

void RunThread(ThreadState& state) noexcept {
    executingThread = &state;
    Thread::Entry entry;
    void* context;
    {
        // 생성자가 native handle/소유권을 게시하기 전에 사용자 함수가 실행되지 않도록 한다.
        LockGuard lock(state.gate);
        entry = state.entry;
        context = state.context;
    }
    entry(context);
    {
        LockGuard lock(state.gate);
        state.completed = true;
    }
    // 여기서 알리는 것은 entry의 반환이다. 이후 CRT/TLS/native 종료는 Join이 별도로 회수한다.
    state.completion.NotifyAll();
}

} // namespace rocks::platform::detail

namespace rocks::platform {

Thread::Thread() noexcept {
    static_assert(sizeof(detail::ThreadState) <= sizeof(storage_));
    static_assert(alignof(detail::ThreadState) <= alignof(Thread));
    ::new (storage_) detail::ThreadState{};
}

Thread::~Thread() noexcept {
    auto& state = *reinterpret_cast<detail::ThreadState*>(storage_);
    if (IsJoinable()) {
        Fatal("Thread destroyed without a successful Join");
    }
    state.~ThreadState();
}

ThreadStatus Thread::Start(Entry entry, void* context) noexcept {
    if (entry == nullptr) {
        return ThreadStatus::InvalidArgument;
    }
    auto& state = *reinterpret_cast<detail::ThreadState*>(storage_);
    LockGuard lock(state.gate);
    if (state.joinable) {
        return ThreadStatus::AlreadyJoinable;
    }
    if (state.waiters != 0) {
        // 이전 실행을 기다린 reader가 복귀하기 전에 새로운 완료 predicate를 게시하지 않는다.
        return ThreadStatus::Busy;
    }
    state.entry = entry;
    state.context = context;
    state.completed = false;
    const ThreadStatus started = detail::StartNativeThread(state);
    if (started == ThreadStatus::Success) {
        state.joinable = true;
    } else {
        state.entry = nullptr;
        state.context = nullptr;
    }
    return started;
}

WaitResult Thread::WaitForCompletion(uint32 timeoutMs) noexcept {
    detail::WaitDeadline deadline(timeoutMs);
    auto& state = *reinterpret_cast<detail::ThreadState*>(storage_);
    LockGuard lock(state.gate);
    if (!state.joinable || detail::IsExecutingThread(state) ||
        state.waiters == static_cast<uint64>(-1)) {
        return WaitResult::Failed;
    }
    ++state.waiters;
    WaitResult result = WaitResult::Signaled;
    while (!state.completed) {
        const uint32 remaining = deadline.RemainingMilliseconds();
        if (remaining == 0) {
            result = WaitResult::TimedOut;
            break;
        }
        const WaitResult wait = state.completion.Wait(state.gate, remaining);
        if (wait == WaitResult::Failed) {
            result = wait;
            break;
        }
    }
    --state.waiters;
    return result;
}

ThreadStatus Thread::Join() noexcept {
    auto& state = *reinterpret_cast<detail::ThreadState*>(storage_);
    {
        LockGuard lock(state.gate);
        if (!state.joinable) {
            return ThreadStatus::NotJoinable;
        }
        if (detail::IsExecutingThread(state)) {
            return ThreadStatus::SelfJoin;
        }
        if (state.joining) {
            return ThreadStatus::Busy;
        }
        state.joining = true;
    }
    const ThreadStatus joined = detail::JoinNativeThread(state);
    LockGuard lock(state.gate);
    state.joining = false;
    if (joined == ThreadStatus::Success) {
        state.joinable = false;
        state.entry = nullptr;
        state.context = nullptr;
    }
    return joined;
}

bool Thread::IsJoinable() const noexcept {
    const auto& state = *reinterpret_cast<const detail::ThreadState*>(storage_);
    LockGuard lock(state.gate);
    return state.joinable;
}

} // namespace rocks::platform

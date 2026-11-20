#include "CrashReportingInternal.h"
#include "ThreadInternal.h"
#include "WaitDeadline.h"
#include <Base/Compiler.h>
#include <Base/PlacementNew.h>
#include <Platform/CrashReporting.h>
#include <Platform/Fatal.h>

namespace rocks::platform::detail {
namespace {

// OS thread id 재사용과 무관하게 자기 실행을 식별한다. pointer는 TLS 종료 정리까지 유지한다.
thread_local ThreadState* executingThread = nullptr;

// name이 MaxThreadNameBytes 이하의 올바른 UTF-8이면 길이를 length에 두고 true다. 과대 표현·대리
// 영역·U+10FFFF 초과는 거부한다(Windows UTF-16 변환이 실패하지 않게 한다).
bool MeasureThreadName(const char* name, usize& length) noexcept {
    usize index = 0;
    while (name[index] != '\0') {
        if (index >= MaxThreadNameBytes) {
            return false;
        }

        const auto lead = static_cast<uint8>(name[index]);
        usize extra = 0;
        uint32 codePoint = 0;
        if (lead < 0x80) {
            ++index;
            continue;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1;
            codePoint = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            extra = 2;
            codePoint = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            extra = 3;
            codePoint = lead & 0x07u;
        } else {
            return false;
        }

        for (usize offset = 1; offset <= extra; ++offset) {
            const auto next = static_cast<uint8>(name[index + offset]);
            if ((next & 0xC0u) != 0x80u) {
                return false;
            }

            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }

        const bool overlong =
            (extra == 2 && codePoint < 0x800u) || (extra == 3 && codePoint < 0x10000u);
        const bool surrogate = codePoint >= 0xD800u && codePoint <= 0xDFFFu;
        if (overlong || surrogate || codePoint > 0x10FFFFu) {
            return false;
        }

        index += extra + 1;
    }

    if (index > MaxThreadNameBytes) {
        return false;
    }

    length = index;
    return true;
}

} // namespace

bool IsExecutingThread(const ThreadState& state) noexcept {
    return executingThread == &state;
}

const char* CurrentPlatformThreadName() noexcept {
    // RunThread가 끝날 때까지 state는 살아 있고 이름은 실행 중 바뀌지 않는다.
    return executingThread != nullptr ? executingThread->name : nullptr;
}

namespace {

struct GuardedThreadEntry {
    Thread::Entry entry;
    void* context;
};

// 보호 프레임이 부르는 별도 함수다(보호 블록 안에 인라인되지 않는다).
ROCKS_NOINLINE int RunGuardedThreadEntry(void* raw) noexcept {
    const auto* guarded = static_cast<const GuardedThreadEntry*>(raw);
    guarded->entry(guarded->context);
    return 0;
}

} // namespace

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
    // 이름·우선순위는 entry가 보기 전에 적용한다. Start가 joinable 동안 옵션을 바꾸지 않으므로 gate
    // 밖에서 읽어도 된다.
    ApplyCurrentThreadOptions(state);
    // 진입 함수를 크래시 보호 프레임 안에서 부른다(Windows SEH 경계, 설계 8.6절).
    GuardedThreadEntry guarded{entry, context};
    (void)RunWithCrashGuard(&RunGuardedThreadEntry, &guarded);
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

ThreadStatus Thread::Start(Entry entry, void* context, const ThreadOptions& options) noexcept {
    if (entry == nullptr) {
        return ThreadStatus::InvalidArgument;
    }
    usize nameLength = 0;
    if (options.name != nullptr && !detail::MeasureThreadName(options.name, nameLength)) {
        return ThreadStatus::InvalidArgument;
    }

    if (options.stackBytes > MaxThreadStackBytes ||
        static_cast<uint32>(options.priority) > static_cast<uint32>(ThreadPriority::Highest)) {
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
    state.priority = options.priority;
    state.stackBytes = options.stackBytes;
    for (usize index = 0; index < nameLength; ++index) {
        state.name[index] = options.name[index];
    }

    state.name[nameLength] = '\0';
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
        state.waiters == NumericLimits<uint64>::Max()) {
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

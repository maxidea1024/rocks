#pragma once

#include <Platform/Thread.h>

namespace rocks::platform::detail {

// 공개 Thread의 opaque storage에 placement 생성한다. native slot만 선택된 backend가 해석한다.
struct ThreadState {
    mutable Mutex gate;
    ConditionVariable completion;
    Thread::Entry entry = nullptr;
    void* context = nullptr;
    uint64 waiters = 0;
    bool joinable = false;
    bool joining = false;
    bool completed = false;
    // Start가 검사·복사한 생성 옵션이다. 실행 중에는 바꾸지 않는다(Start는 joinable 동안 거부된다).
    ThreadPriority priority = ThreadPriority::Normal;
    usize stackBytes = 0;
    char name[MaxThreadNameBytes + 1]{};
    alignas(16) unsigned char native[16]{};
};

// Start는 state gate 안, Join은 joining reservation을 확보한 뒤 gate 밖에서 호출한다.
ThreadStatus StartNativeThread(ThreadState&) noexcept;
ThreadStatus JoinNativeThread(ThreadState&) noexcept;
void RunThread(ThreadState&) noexcept;
// 새 스레드 안에서 entry보다 먼저 부른다. 이름·우선순위를 현재 스레드에 적용하며 OS 거부는 무시한다
// (ThreadOptions 계약: OS 힌트). 이름이 비어 있으면 붙이지 않고, Normal이면 우선순위를 바꾸지
// 않는다.
void ApplyCurrentThreadOptions(const ThreadState&) noexcept;

} // namespace rocks::platform::detail

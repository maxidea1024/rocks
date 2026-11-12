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
    alignas(16) unsigned char native[16]{};
};

// Start는 state gate 안, Join은 joining reservation을 확보한 뒤 gate 밖에서 호출한다.
ThreadStatus StartNativeThread(ThreadState&) noexcept;
ThreadStatus JoinNativeThread(ThreadState&) noexcept;
void RunThread(ThreadState&) noexcept;

} // namespace rocks::platform::detail

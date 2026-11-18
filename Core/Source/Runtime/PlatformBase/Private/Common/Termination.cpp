#include "TerminationInternal.h"

#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks::platform {

namespace {

// 종료 hook(원자 포인터)과, 프로세스에서 한 번만 부르기 위한 표시다.
constinit TerminationHook terminationHook = nullptr;
constinit uint32 terminationRan = 0;

} // namespace

TerminationHook SetTerminationHook(TerminationHook hook) noexcept {
    return AtomicExchange<MemoryOrder::AcquireRelease>(&terminationHook, hook);
}

namespace detail {

void RunTerminationHook(const char* message, usize bytes) noexcept {
    // Fatal·OS 예외·신호가 겹쳐도(abort가 SIGABRT를 내는 등) 한 번만 부른다.
    if (AtomicExchange<MemoryOrder::AcquireRelease>(&terminationRan, 1u) != 0) {
        return;
    }

    if (const TerminationHook hook = AtomicLoad<MemoryOrder::Acquire>(&terminationHook)) {
        hook(message, bytes);
    }
}

} // namespace detail

} // namespace rocks::platform

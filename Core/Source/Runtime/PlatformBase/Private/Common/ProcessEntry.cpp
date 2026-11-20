#include "ProcessEntryInternal.h"

#include <Platform/Atomics.h>

namespace rocks::platform::detail {

namespace {

constinit TerminationRequestHandler requestHandler = nullptr;
constinit uint32 requestCount = 0;

} // namespace

bool AcquireTerminationRequestHandler(TerminationRequestHandler handler) noexcept {
    if (handler == nullptr) {
        return false;
    }

    // 요청 수는 칸이 빌 때 늘 0으로 돌아간다(ReleaseTerminationRequestHandler).
    TerminationRequestHandler expected = nullptr;
    return AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Relaxed>(
        &requestHandler, expected, handler);
}

void ReleaseTerminationRequestHandler() noexcept {
    AtomicStore<MemoryOrder::Release>(&requestHandler,
                                      static_cast<TerminationRequestHandler>(nullptr));
    AtomicStore<MemoryOrder::Relaxed>(&requestCount, 0u);
}

TerminationRequestOutcome DispatchTerminationRequest() noexcept {
    const TerminationRequestHandler handler = AtomicLoad<MemoryOrder::Acquire>(&requestHandler);
    if (handler == nullptr) {
        return TerminationRequestOutcome::PassToSystem;
    }

    // 두 번째 요청부터는 사용자가 기다리지 않겠다는 뜻이다. OS 기본 동작(즉시 종료)으로 넘긴다.
    if (AtomicFetchAdd<MemoryOrder::AcquireRelease>(&requestCount, 1u) != 0) {
        return TerminationRequestOutcome::PassToSystem;
    }

    handler();
    return TerminationRequestOutcome::Handled;
}

} // namespace rocks::platform::detail

#include <Platform/Atomics.h>
#include <Platform/SystemDialog.h>

namespace rocks::platform {

namespace {

// 걸린 hook이다. null이면 없다. 크래시 경로에서도 읽으므로 원자 포인터 하나만 둔다.
Atomic<SystemDialogHook> SystemDialogHookSlot{nullptr};

} // namespace

bool SetSystemDialogHook(SystemDialogHook hook) noexcept {
    if (hook == nullptr) {
        return false;
    }

    SystemDialogHook expected = nullptr;
    if (SystemDialogHookSlot
            .CompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(expected,
                                                                                      hook)) {
        return true;
    }

    return expected == hook;
}

void ClearSystemDialogHook(SystemDialogHook hook) noexcept {
    if (hook == nullptr) {
        return;
    }

    SystemDialogHook expected = hook;
    (void)SystemDialogHookSlot
        .CompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(expected,
                                                                                  nullptr);
}

void NotifySystemDialogOpening() noexcept {
    if (const SystemDialogHook hook = SystemDialogHookSlot.Load<MemoryOrder::Acquire>()) {
        hook();
    }
}

} // namespace rocks::platform

#include <TestSupport/HostFailure.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

namespace rocks::test {

FILE* OpenReportFile(const char* path) noexcept {
    if (path == nullptr) {
        return nullptr;
    }
    return fopen(path, "wb");
}

void PrepareFatalTestProcess() noexcept {
    // macOS·Android는 abort·assert가 대화상자를 띄우지 않는다. crash reporter는 시스템 설정을
    // 따른다.
}

void LockStandardErrorForTest() noexcept {
    flockfile(stderr);
}

void UnlockStandardErrorForTest() noexcept {
    funlockfile(stderr);
}

} // namespace rocks::test

namespace rocks::test {

namespace {

struct UnmanagedStart {
    void (*entry)(void*) noexcept;
    void* context;
};

void* UnmanagedThreadMain(void* raw) noexcept {
    const UnmanagedStart start = *static_cast<UnmanagedStart*>(raw);
    free(raw);
    start.entry(start.context);
    return nullptr;
}

} // namespace

bool StartUnmanagedThread(void (*entry)(void* context) noexcept, void* context) noexcept {
    auto* start = static_cast<UnmanagedStart*>(malloc(sizeof(UnmanagedStart)));
    if (start == nullptr) {
        return false;
    }

    start->entry = entry;
    start->context = context;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, &UnmanagedThreadMain, start) != 0) {
        free(start);
        return false;
    }

    (void)pthread_detach(thread);
    return true;
}

} // namespace rocks::test

namespace rocks::test {

bool PassInvalidParameter() noexcept {
    return false;
}

bool CallPureVirtual() noexcept {
    return false;
}

bool CorruptHeap() noexcept {
    return false;
}

bool RaiseUserException() noexcept {
    return false;
}

bool CorruptProcessHeap() noexcept {
    return false;
}

bool FailFast() noexcept {
    return false;
}

bool ExitWithControlCStatus() noexcept {
    return false;
}

bool InstallCrashHelperTestMode() noexcept {
    return false;
}

bool ReplaceUnhandledFilterForTest() noexcept {
    return false;
}

bool TestUnhandledFilterIsCurrent() noexcept {
    return false;
}

void RestoreUnhandledFilterForTest() noexcept {
}

bool EnterKillOnCloseJob(bool) noexcept {
    return false;
}

bool EnterBreakawayBlockingJob() noexcept {
    return false;
}

bool HoldProcessHeapDuringDump() noexcept {
    return false;
}

} // namespace rocks::test

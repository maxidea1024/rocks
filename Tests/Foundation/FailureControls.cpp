#include "Tests/Support/Test.h"
#include <Core/Assert.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace rocks::test {
namespace {

void CheckWorker() noexcept {
    for (int i = 0; i < 50; ++i) {
        RecordFailure("parallel-negative-control", __FILE__, __LINE__);
    }
}

#if defined(_WIN32)
DWORD WINAPI ParallelEntry(void*) {
    CheckWorker();
    return 0;
}

DWORD WINAPI AssertEntry(void*) {
    ROCKS_ASSERT(false);
    return 0;
}
#else
void* ParallelEntry(void*) {
    CheckWorker();
    return nullptr;
}

void* AssertEntry(void*) {
    ROCKS_ASSERT(false);
    return nullptr;
}
#endif
} // namespace

void RunParallelNegativeControl() noexcept {
#if defined(_WIN32)
    HANDLE workers[4]{};
    for (auto& worker : workers) {
        worker = CreateThread(nullptr, 0, ParallelEntry, nullptr, 0, nullptr);
        ROCKS_ASSERT(worker != nullptr);
    }
    for (auto worker : workers) {
        ROCKS_ASSERT(WaitForSingleObject(worker, INFINITE) == WAIT_OBJECT_0);
        CloseHandle(worker);
    }
#else
    pthread_t workers[4]{};
    for (auto& worker : workers) {
        ROCKS_ASSERT(pthread_create(&worker, nullptr, ParallelEntry, nullptr) == 0);
    }
    for (auto worker : workers) {
        ROCKS_ASSERT(pthread_join(worker, nullptr) == 0);
    }
#endif
}

int RunAssertWithLockedStderr() noexcept {
#if defined(_WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _lock_file(stderr);
    HANDLE worker = CreateThread(nullptr, 0, AssertEntry, nullptr, 0, nullptr);
    if (worker == nullptr) {
        _unlock_file(stderr);
        return 2;
    }
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    _unlock_file(stderr);
#else
    flockfile(stderr);
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, AssertEntry, nullptr) != 0) {
        funlockfile(stderr);
        return 2;
    }
    pthread_join(worker, nullptr);
    funlockfile(stderr);
#endif
    return 0;
}
} // namespace rocks::test

namespace rocks::test {
int RunAsanNegativeControl() noexcept {
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    auto* memory = static_cast<unsigned char*>(malloc(32));
    if (memory == nullptr) {
        return 2;
    }
    memory[0] = 17;
    volatile unsigned char* dangling = memory;
    free(memory);
    // Deliberate child-process negative control, never part of normal tests.
    const volatile unsigned char read = *dangling;
    (void)read;
    return 0;
#endif
#endif
    return 2;
}
} // namespace rocks::test

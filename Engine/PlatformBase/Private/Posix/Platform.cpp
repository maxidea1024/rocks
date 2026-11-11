#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>
#include <Base/Checked.h>
#include "PosixPlatform.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

namespace rocks::platform {

void* AlignedAllocate(usize bytes, usize alignment) noexcept {
    if (bytes == 0 || !IsPowerOfTwo(alignment) || bytes > MaxSize - (alignment - 1)) {
        return nullptr;
    }

    // posix_memalign의 포인터 크기 정렬 조건을 충족하면서 더 작은 요청도 수용한다.
    if (alignment < sizeof(void*)) {
        alignment = sizeof(void*);
    }

    void* result = nullptr;
    return posix_memalign(&result, alignment, bytes) == 0 ? result : nullptr;
}

void AlignedFree(void* address) noexcept {
    free(address);
}

Mutex::Mutex() noexcept {
    // SDK 타입은 private에서만 해석하며 공개 opaque storage의 ABI 전제를 검사한다.
    static_assert(sizeof(pthread_mutex_t) <= sizeof(storage_));
    static_assert(alignof(pthread_mutex_t) <= alignof(Mutex));
    if (pthread_mutex_init(reinterpret_cast<pthread_mutex_t*>(storage_), nullptr) != 0) {
        Fatal("pthread_mutex_init failed");
    }
}

Mutex::~Mutex() noexcept {
    if (pthread_mutex_destroy(reinterpret_cast<pthread_mutex_t*>(storage_)) != 0) {
        Fatal("Destroying a busy mutex");
    }
}

void Mutex::Lock() noexcept {
    if (pthread_mutex_lock(reinterpret_cast<pthread_mutex_t*>(storage_)) != 0) {
        Fatal("pthread_mutex_lock failed");
    }
}

void Mutex::Unlock() noexcept {
    if (pthread_mutex_unlock(reinterpret_cast<pthread_mutex_t*>(storage_)) != 0) {
        Fatal("pthread_mutex_unlock failed");
    }
}

uint64 MonotonicNanoseconds() noexcept {
    timespec time{};
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0 || time.tv_sec < 0) {
        Fatal("clock_gettime failed");
    }

    // 초를 ns로 바꾸기 전에 상한을 검사한다. 실패 시 wrap된 deadline을 반환하지 않는다.
    const uint64 seconds = static_cast<uint64>(time.tv_sec);
    if (seconds > (static_cast<uint64>(-1) - 999999999) / 1000000000) {
        Fatal("Monotonic clock overflow");
    }

    return seconds * 1000000000 + static_cast<uint64>(time.tv_nsec);
}

uint64 CurrentThreadId() noexcept {
    return detail::PosixThreadId();
}

[[noreturn]] void Fatal(const char* message) noexcept {
    const char* text = message != nullptr ? message : "Rocks fatal error";
    usize length = 0;
    while (length < 4096 && text[length] != '\0') {
        ++length;
    }

    // CRT FILE lock을 거치지 않는다. 출력 오류가 발생해도 무제한 재시도나 heap formatting은 하지
    // 않는다.
    const auto ignoredText = write(STDERR_FILENO, text, length);
    const auto ignoredNewline = write(STDERR_FILENO, "\n", 1);
    (void)ignoredText;
    (void)ignoredNewline;
    abort();
}

} // namespace rocks::platform

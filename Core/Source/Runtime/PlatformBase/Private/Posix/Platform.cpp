#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <Base/Checked.h>
#include "../Common/TerminationInternal.h"
#include "PosixPlatform.h"
#include <pthread.h>
#include <errno.h>
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

bool Mutex::TryLock() noexcept {
    const int result = pthread_mutex_trylock(reinterpret_cast<pthread_mutex_t*>(storage_));
    if (result == 0) {
        return true;
    }

    if (result != EBUSY) {
        Fatal("pthread_mutex_trylock failed");
    }

    return false;
}

void Mutex::Unlock() noexcept {
    if (pthread_mutex_unlock(reinterpret_cast<pthread_mutex_t*>(storage_)) != 0) {
        Fatal("pthread_mutex_unlock failed");
    }
}

// 원천은 OS hook이 고른다(Apple mach_absolute_time, Android CLOCK_MONOTONIC). ns 변환
// (MonotonicNanoseconds)은 플랫폼 공통 Private/Common/Clock.cpp가 이 두 값으로 한다.
uint64 MonotonicTicks() noexcept {
    return detail::PosixMonotonicTicks();
}

uint64 MonotonicTickFrequency() noexcept {
    return detail::PosixMonotonicTickFrequency();
}

bool TryUtcTicks(int64& ticks) noexcept {
    timespec time{};
    if (clock_gettime(CLOCK_REALTIME, &time) != 0) {
        return false;
    }

    // 0001-01-01부터 1970-01-01까지는 719162일이다.
    constexpr int64 Offset = 719162ll * 864000000000ll;
    constexpr int64 MaxTicks = 3155378975999999999ll;
    const int64 seconds = static_cast<int64>(time.tv_sec);
    if (seconds < -Offset / 10000000ll || seconds > (MaxTicks - Offset) / 10000000ll) {
        return false;
    }

    ticks = seconds * 10000000ll + static_cast<int64>(time.tv_nsec) / 100 + Offset;
    return true;
}

int64 UtcTicks() noexcept {
    int64 ticks = 0;
    if (!TryUtcTicks(ticks)) {
        Fatal("UTC clock query failed or is outside the DateTime range");
    }

    return ticks;
}

uint64 CurrentThreadId() noexcept {
    return detail::PosixThreadId();
}

void WriteDiagnostic(const char* bytes, usize count) noexcept {
    if (bytes == nullptr) {
        return;
    }

    // 부분 쓰기는 이어 쓰고, 오류·진행 없음이면 멈춘다(EINTR만 다시 시도한다).
    while (count != 0) {
        const auto written = write(STDERR_FILENO, bytes, count);
        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written <= 0) {
            return;
        }

        bytes += written;
        count -= static_cast<usize>(written);
    }
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
    detail::RecordTerminatingThread(CurrentThreadId());
    detail::RunTerminationHook(text, length);
    abort();
}

} // namespace rocks::platform

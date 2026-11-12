#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>
#include <Base/Checked.h>

#include <Windows.h>
#include <malloc.h>
#include <stdlib.h>

namespace rocks::platform {

void* AlignedAllocate(usize bytes, usize alignment) noexcept {
    if (bytes == 0 || !IsPowerOfTwo(alignment) || bytes > MaxSize - (alignment - 1)) {
        return nullptr;
    }

    // 1/2/4-byte 요청도 지원하되 CRT가 요구하는 최소 정렬 이상으로 올린다.
    if (alignment < sizeof(void*)) {
        alignment = sizeof(void*);
    }

    return _aligned_malloc(bytes, alignment);
}

void AlignedFree(void* address) noexcept {
    _aligned_free(address);
}

Mutex::Mutex() noexcept {
    // SDK 타입은 private에서만 해석하며 공개 opaque storage의 ABI 전제를 검사한다.
    static_assert(sizeof(SRWLOCK) <= sizeof(storage_));
    static_assert(alignof(SRWLOCK) <= alignof(Mutex));
    InitializeSRWLock(reinterpret_cast<SRWLOCK*>(storage_));
}

Mutex::~Mutex() noexcept = default;

void Mutex::Lock() noexcept {
    AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(storage_));
}

void Mutex::Unlock() noexcept {
    ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(storage_));
}

uint64 MonotonicNanoseconds() noexcept {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) ||
        counter.QuadPart < 0 || frequency.QuadPart <= 0) {
        Fatal("QueryPerformanceCounter failed");
    }

    // 정수 tick×10^9 중간 곱의 overflow를 피한다. 단위 변환은 해상도 향상을 뜻하지 않는다.
    const long double nanoseconds = static_cast<long double>(counter.QuadPart) * 1000000000.0L /
                                    static_cast<long double>(frequency.QuadPart);
    if (nanoseconds >= static_cast<long double>(static_cast<uint64>(-1))) {
        Fatal("Monotonic clock overflow");
    }

    return static_cast<uint64>(nanoseconds);
}

uint64 CurrentThreadId() noexcept {
    return static_cast<uint64>(GetCurrentThreadId());
}

[[noreturn]] void Fatal(const char* message) noexcept {
    const char* text = message != nullptr ? message : "Rocks fatal error";
    DWORD length = 0;
    while (length < 4096 && text[length] != '\0') {
        ++length;
    }

    // CRT FILE lock을 거치지 않는다. 오류 진단 자체의 실패는 추가 logger나 재할당으로 처리하지
    // 않는다.
    DWORD written = 0;
    const HANDLE errorStream = GetStdHandle(STD_ERROR_HANDLE);
    WriteFile(errorStream, text, length, &written, nullptr);
    WriteFile(errorStream, "\n", 1, &written, nullptr);
    abort();
}

} // namespace rocks::platform

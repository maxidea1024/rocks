#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <Base/Checked.h>
#include "../Common/TerminationInternal.h"

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

bool Mutex::TryLock() noexcept {
    return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(storage_)) != 0;
}

void Mutex::Unlock() noexcept {
    ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(storage_));
}

// ns 변환(MonotonicNanoseconds)은 플랫폼 공통 Private/Common/Clock.cpp가 이 두 값으로 한다.
uint64 MonotonicTicks() noexcept {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter) || counter.QuadPart < 0) {
        Fatal("QueryPerformanceCounter failed");
    }

    return static_cast<uint64>(counter.QuadPart);
}

// 주파수는 부팅 때 정해져 바뀌지 않는다. 호출마다 읽어도 비용이 작아 따로 캐시하지 않는다.
uint64 MonotonicTickFrequency() noexcept {
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        Fatal("QueryPerformanceFrequency failed");
    }

    return static_cast<uint64>(frequency.QuadPart);
}

bool TryUtcTicks(int64& ticks) noexcept {
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    // FILETIME은 1601-01-01부터의 100ns다. 0001-01-01부터 1601-01-01까지는 584388일이다.
    constexpr int64 Offset = 584388ll * 864000000000ll;
    constexpr uint64 MaxTicks = 3155378975999999999ull;
    const uint64 value = (static_cast<uint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    if (value > MaxTicks - static_cast<uint64>(Offset)) {
        return false;
    }

    ticks = static_cast<int64>(value) + Offset;
    return true;
}

int64 UtcTicks() noexcept {
    int64 ticks = 0;
    if (!TryUtcTicks(ticks)) {
        Fatal("UTC clock is outside the DateTime range");
    }

    return ticks;
}

uint64 CurrentThreadId() noexcept {
    return static_cast<uint64>(GetCurrentThreadId());
}

void WriteDiagnostic(const char* bytes, usize count) noexcept {
    const HANDLE errorStream = GetStdHandle(STD_ERROR_HANDLE);
    if (bytes == nullptr || errorStream == nullptr || errorStream == INVALID_HANDLE_VALUE) {
        return;
    }

    // WriteFile은 DWORD 길이를 받으므로 나눠 쓴다. 진행이 없으면(실패·0 byte) 멈춘다.
    constexpr usize MaxChunk = 1u << 30;
    while (count != 0) {
        const DWORD chunk = static_cast<DWORD>(count < MaxChunk ? count : MaxChunk);
        DWORD written = 0;
        if (!WriteFile(errorStream, bytes, chunk, &written, nullptr) || written == 0) {
            return;
        }

        bytes += written;
        count -= written;
    }
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
    detail::RecordTerminatingThread(CurrentThreadId());
    detail::RunTerminationHook(text, length);
    abort();
}

} // namespace rocks::platform

#pragma once

#include <Base/Types.h>

namespace rocks::platform {

// 대기 시간 단위는 millisecond다. 0은 즉시 검사이며 아래 값만 무한 대기를 뜻한다.
inline constexpr uint32 InfiniteTimeout = static_cast<uint32>(-1);

enum class WaitResult {
    Signaled,
    TimedOut,
    Failed
};

// 할당 없는 native mutex wrapper이며 같은 thread의 재귀 Lock은 지원하지 않는다.
// 잠근 thread가 Unlock해야 하며, 모든 사용자와 대기자가 끝난 뒤에만 파괴한다.
// 잘못된 사용의 검출 여부나 대기 순서의 공정성은 플랫폼 공통 보장이 아니다.
class Mutex {
public:
    Mutex() noexcept;
    ~Mutex() noexcept;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void Lock() noexcept;
    void Unlock() noexcept;

private:
    friend class ConditionVariable;
    // OS 타입을 공개 ABI에 노출하지 않는다. 각 backend가 크기·정렬을 static_assert한다.
    alignas(16) unsigned char storage_[64]{};
};

// Mutex 자체는 소유하지 않는다. guard보다 Mutex가 오래 살아야 하며 같은 thread에서
// guard를 파괴해야 한다. 보호 구간에서 사용자 callback을 호출할지는 상위 코드가 결정한다.
class LockGuard {
public:
    explicit LockGuard(Mutex& mutex) noexcept : mutex_(mutex) {
        mutex_.Lock();
    }

    ~LockGuard() noexcept {
        mutex_.Unlock();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& mutex_;
};

// Wait는 caller가 소유한 Mutex를 원자적으로 풀고 대기한 뒤 다시 잠근 상태로 반환한다.
// Signaled도 spurious wake일 수 있으므로 보호하는 predicate를 반드시 반복 검사한다.
// 유한 반복 대기는 최초 monotonic deadline의 남은 시간만 넘겨 timeout을 연장하지 않는다.
// 같은 대기 묶음은 같은 Mutex를 사용한다. 모든 waiter 종료 뒤 파괴하며 native 초기화 실패는
// Fatal다.
class ConditionVariable {
public:
    ConditionVariable() noexcept;
    ~ConditionVariable() noexcept;
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    WaitResult Wait(Mutex& mutex, uint32 timeoutMs = InfiniteTimeout) noexcept;
    void NotifyOne() noexcept;
    void NotifyAll() noexcept;

private:
    alignas(16) unsigned char storage_[64]{};
};

// Signal 이후 Reset까지 모든 waiter와 이후 Wait가 통과한다. thread affinity는 없다.
// 동시 Signal/Reset/Wait를 지원하나 파괴 전에는 모든 caller를 종료해야 한다. Close/detach는 없다.
class ManualResetEvent {
public:
    explicit ManualResetEvent(bool signaled = false) noexcept;
    ManualResetEvent(const ManualResetEvent&) = delete;
    ManualResetEvent& operator=(const ManualResetEvent&) = delete;

    void Signal() noexcept;
    void Reset() noexcept;
    WaitResult Wait(uint32 timeoutMs = InfiniteTimeout) noexcept;

private:
    Mutex gate_;
    ConditionVariable changed_;
    bool signaled_;
};

// Wait 성공은 permit 하나를 소비한다. Release는 count 전체를 더하거나 실패 시 전혀 바꾸지 않는다.
// initial<=maximum, maximum>0이 생성 전제다(위반은 Fatal). Release(0)/한도 초과는 false다.
// 동시 Wait/Release를 지원한다. 공정성은 보장하지 않으며 모든 caller 종료 뒤 파괴한다.
class Semaphore {
public:
    Semaphore(uint32 initialCount, uint32 maximumCount) noexcept;
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    bool Release(uint32 count = 1) noexcept;
    WaitResult Wait(uint32 timeoutMs = InfiniteTimeout) noexcept;

private:
    Mutex gate_;
    ConditionVariable changed_;
    uint32 count_;
    uint32 maximum_;
};
} // namespace rocks::platform

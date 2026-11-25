#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Platform/Atomics.h>
#include <Platform/Sync/ParkingLot.h>
#include <Platform/Sync/WaitTypes.h>

namespace rocks::platform {

// 잠긴 채 만드는 표지다(Mutex(InitiallyLocked)). 잠긴 잠금을 만들어 다른 스레드에 넘기는 용도다
// (UE UE::AcquireLock 대응). 그 잠금의 첫 해제는 다른 스레드가 해도
// 된다(Docs/Design/Synchronization 8.1절).
struct InitiallyLockedTag {
    explicit constexpr InitiallyLockedTag() noexcept = default;
};

inline constexpr InitiallyLockedTag InitiallyLocked{};

// 호출자의 Atomic<StateType> 안 2 bit로 만드는 잠금이다(UE UE::TIntrusiveMutex 대응, 8.2절). 슬롯
// 상태 word·해시 칸처럼 이미 원자 정수가 있는 자료에 잠금을 따로 두지 않고 넣는다. Mutex의 구현도
// 이것이다.
//
// Params가 정하는 것:
// - StateType: 상태 word의 정수 형식. LockedBit·WaitingBit: 서로 다른 1 bit.
// - 선택: LockedMask(잠김으로 보는 bit 묶음, 기본 LockedBit — 읽기쓰기 잠금이 쓴다),
// SpinLimit(대기자가
//   없을 때 도는 횟수, 기본 SpinBackoff::PauseLimit), WaitAddress(State) → 대기 주소(기본 &State).
// 상태 word의 나머지 bit를 다른 CAS로 바꾸는 코드는 잠김·대기 bit를 보존해야 한다.
//
// 사용 예:
//   struct SlotBits {
//       using StateType = uint32;
//       static constexpr uint32 LockedBit = 1u << 30;
//       static constexpr uint32 WaitingBit = 1u << 31;
//   };
//   IntrusiveMutex<SlotBits>::Lock(slot.state);
//   ...
//   IntrusiveMutex<SlotBits>::Unlock(slot.state);
template <class Params>
class IntrusiveMutex {
public:
    using StateType = typename Params::StateType;

private:
    static constexpr StateType LockedBit = Params::LockedBit;
    static constexpr StateType WaitingBit = Params::WaitingBit;

    static constexpr StateType LockedMaskValue() noexcept {
        if constexpr (requires { Params::LockedMask; }) {
            return Params::LockedMask;
        } else {
            return LockedBit;
        }
    }

    static constexpr uint32 SpinLimitValue() noexcept {
        if constexpr (requires { Params::SpinLimit; }) {
            return Params::SpinLimit;
        } else {
            return SpinBackoff::PauseLimit;
        }
    }

    static constexpr StateType LockedMask = LockedMaskValue();
    static constexpr uint32 SpinLimit = SpinLimitValue();

    static_assert(LockedBit != 0 && (LockedBit & (LockedBit - 1)) == 0,
                  "LockedBit must be one bit");
    static_assert(WaitingBit != 0 && (WaitingBit & (WaitingBit - 1)) == 0,
                  "WaitingBit must be one bit");
    static_assert(LockedBit != WaitingBit, "LockedBit and WaitingBit must be different bits");
    static_assert((LockedMask & LockedBit) == LockedBit, "LockedMask must contain LockedBit");
    static_assert((LockedMask & WaitingBit) == 0, "LockedMask must not contain WaitingBit");

    static const void* WaitAddress(const Atomic<StateType>& state) noexcept {
        if constexpr (requires { Params::WaitAddress(state); }) {
            return Params::WaitAddress(state);
        } else {
            return &state;
        }
    }

public:
    // 진단용이다. 다른 스레드에서 읽은 값은 곧 낡는다.
    static bool IsLocked(const Atomic<StateType>& state) noexcept {
        return (state.template Load<MemoryOrder::Relaxed>() & LockedBit) != 0;
    }

    static bool TryLock(Atomic<StateType>& state) noexcept {
        StateType expected = state.template Load<MemoryOrder::Relaxed>();
        return (expected & LockedMask) == 0 &&
               state.template CompareExchangeStrong<MemoryOrder::Acquire, MemoryOrder::Relaxed>(
                   expected, static_cast<StateType>(expected | LockedBit));
    }

    // 빠른 경로는 CAS 하나다. 잠겼으면 대기자가 없을 때만 SpinLimit번 돌고 대기 bit를 세운 뒤
    // ParkingLot에서 잔다. 깨면 처음부터 다시 잡는다(공정하지 않다).
    static void Lock(Atomic<StateType>& state) noexcept {
        StateType expected = state.template Load<MemoryOrder::Relaxed>() &
                             static_cast<StateType>(~(LockedMask | WaitingBit));
        if (state.template CompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Relaxed>(
                expected, static_cast<StateType>(expected | LockedBit))) {
            return;
        }

        LockSlow(state);
    }

    // 빠른 경로는 fetch_sub 하나다. 대기 bit가 있었을 때만 하나를 깨운다.
    static void Unlock(Atomic<StateType>& state) noexcept {
        const StateType last = state.template FetchSub<MemoryOrder::Release>(LockedBit);
        if ((last & WaitingBit) != 0) {
            WakeWaitingThread(state);
        }
    }

    // timeoutMs 안에 잡으면 true다(0은 TryLock, InfiniteTimeout은 Lock과 같다). 시한은 처음 부른
    // 때의 단조 시계 기준이다. deadline은 호출자가 남은 시간을 주는 함수다(remaining() → uint32).
    template <class Remaining>
    static bool TryLockUntil(Atomic<StateType>& state, Remaining&& remaining) noexcept {
        uint32 spins = 0;
        for (StateType current = state.template Load<MemoryOrder::Relaxed>();;) {
            if ((current & LockedMask) == 0) {
                if (state.template CompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Relaxed>(
                        current, static_cast<StateType>(current | LockedBit))) {
                    return true;
                }

                continue;
            }

            if ((current & WaitingBit) == 0 && spins < SpinLimit) {
                ++spins;
                CpuPause();
                current = state.template Load<MemoryOrder::Relaxed>();
                continue;
            }

            if ((current & WaitingBit) == 0) {
                if (!state.template CompareExchangeWeak<MemoryOrder::Relaxed, MemoryOrder::Relaxed>(
                        current, static_cast<StateType>(current | WaitingBit))) {
                    continue;
                }

                current |= WaitingBit;
            }

            const uint32 timeoutMs = remaining();
            if (timeoutMs == 0) {
                return false;
            }

            // 대기 표시 규칙(10.3절): 밖에서 세운 대기 bit와 잠김을 bucket 잠금 아래에서 다시 본다.
            ParkingLot::Wait(
                WaitAddress(state),
                [&state]() noexcept {
                    const StateType now = state.template Load<MemoryOrder::Relaxed>();
                    return (now & LockedMask) != 0 && (now & WaitingBit) != 0;
                },
                []() noexcept {}, timeoutMs);
            current = state.template Load<MemoryOrder::Relaxed>();
        }
    }

private:
    static void LockSlow(Atomic<StateType>& state) noexcept {
        TryLockUntil(state, []() noexcept { return InfiniteTimeout; });
    }

    // 깨운 스레드가 없을 때만 대기 bit를 지운다(UE와 같다). 깨운 스레드가 있으면 bit를 남겨 그
    // 스레드의 Unlock이 남은 대기자를 다시 확인하게 한다.
    static void WakeWaitingThread(Atomic<StateType>& state) noexcept {
        ParkingLot::WakeOne(WaitAddress(state),
                            [&state](ParkingLotWakeOutcome outcome) noexcept -> uint64 {
                                if (!outcome.wokeThread) {
                                    state.template FetchAnd<MemoryOrder::Relaxed>(
                                        static_cast<StateType>(~WaitingBit));
                                }

                                return 0;
                            });
    }
};

// 1 byte 비재진입 잠금이다(UE UE::FMutex 대응, 8.1절). 상수 초기화되고 파괴자가 없어 constinit
// 전역으로 둘 수 있다. 공정하지 않다: 풀린 순간 먼저 CAS한 스레드가 잡는다(기다리던 스레드가 밀릴
// 수 있다). 빠른 경로는 CAS 하나(잠금)·fetch_sub 하나(풀기)이고, 경합하면 대기자가 없을 때만 40번
// 돈 뒤 ParkingLot에서 잔다.
//
// 계약: 잠근 스레드가 푼다(예외: InitiallyLocked로 만든 잠금의 첫 해제). 같은 스레드의 재잠금은
// 교착이다. 잠긴 채 파괴하는 것은 계약 위반이다.
//
// 사용 예:
//   constinit Mutex registryMutex;
//   { LockGuard lock(registryMutex); Insert(entry); }
//   if (registryMutex.TryLockFor(1000)) { Work(); registryMutex.Unlock(); }
class ROCKS_CAPABILITY("mutex") Mutex {
public:
    constexpr Mutex() noexcept = default;

    explicit constexpr Mutex(InitiallyLockedTag) noexcept : state_(Bits::LockedBit) {
    }

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void Lock() noexcept ROCKS_ACQUIRE() {
        Lockable::Lock(state_);
    }

    // 기다리지 않고 잠가 본다. 잠갔으면 true이고 Unlock해야 한다. 멈췄을 수 있는 쪽을 기다리면 안
    // 되는 경로(예: Fatal 기록 전달)가 쓴다.
    bool TryLock() noexcept ROCKS_TRY_ACQUIRE(true) {
        return Lockable::TryLock(state_);
    }

    // timeoutMs 안에 잡으면 true다. 0은 TryLock과 같고 InfiniteTimeout은 Lock과 같다.
    ROCKS_PLATFORMBASE_API bool TryLockFor(uint32 timeoutMs) noexcept ROCKS_TRY_ACQUIRE(true);

    void Unlock() noexcept ROCKS_RELEASE() {
        Lockable::Unlock(state_);
    }

    // 진단·assert용이다. 판단 근거로 쓰지 않는다.
    bool IsLocked() const noexcept {
        return Lockable::IsLocked(state_);
    }

private:
    friend class ConditionVariable;
    friend struct MutexTestAccess;

    struct Bits {
        using StateType = uint8;
        static constexpr uint8 LockedBit = 1;
        static constexpr uint8 WaitingBit = 2;
    };

    using Lockable = IntrusiveMutex<Bits>;

    Atomic<uint8> state_;
};

static_assert(sizeof(Mutex) == 1);

// LockOwner를 만드는 방식이다.
enum class LockStart : uint8 {
    // 생성 때 잡는다.
    Now,
    // 잡지 않고 만든다(나중에 Lock).
    Deferred,
    // TryLock으로 잡아 본다(OwnsLock으로 결과를 본다).
    Try
};

// 생성 때 Lock, 소멸 때 Unlock하는 가드다(UE UE::TUniqueLock 대응). CTAD로 LockGuard
// lock(mutex);라고 쓴다. 잠금을 소유하지 않는다: 잠금이 가드보다 오래 살아야 하고 같은 스레드에서
// 소멸한다. 보호 구간에서 사용자 callback을 부를지는 상위 코드가 정한다.
template <class Lockable>
class ROCKS_SCOPED_CAPABILITY LockGuard {
public:
    explicit LockGuard(Lockable& lock) noexcept ROCKS_ACQUIRE(lock) : lock_(lock) {
        lock_.Lock();
    }

    ~LockGuard() noexcept ROCKS_RELEASE() {
        lock_.Unlock();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lockable& lock_;
};

// 미룸·시도·이른 해제·이동이 되는 가드다(UE UE::TDynamicUniqueLock 대응). OwnsLock으로 쥐었는지
// 본다. 이동할 수 있고 복사할 수 없다. 조건부 잠금이라 정적 분석이 따라가지 못한다(보호 필드를
// 읽는 함수는 LockGuard를 쓴다).
//
// 사용 예:
//   LockOwner owner(mutex, LockStart::Try);
//   if (!owner.OwnsLock()) { return Busy; }
//   ...
//   owner.Unlock();   // 이른 해제
template <class Lockable>
class LockOwner {
public:
    explicit LockOwner(Lockable& lock,
                       LockStart start = LockStart::Now) noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS
        : lock_(&lock) {
        if (start == LockStart::Now) {
            lock_->Lock();
            owns_ = true;
        } else if (start == LockStart::Try) {
            owns_ = lock_->TryLock();
        }
    }

    LockOwner(LockOwner&& other) noexcept : lock_(other.lock_), owns_(other.owns_) {
        other.lock_ = nullptr;
        other.owns_ = false;
    }

    LockOwner& operator=(LockOwner&& other) noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
        if (this != &other) {
            if (owns_) {
                lock_->Unlock();
            }

            lock_ = other.lock_;
            owns_ = other.owns_;
            other.lock_ = nullptr;
            other.owns_ = false;
        }

        return *this;
    }

    ~LockOwner() noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
        if (owns_) {
            lock_->Unlock();
        }
    }

    LockOwner(const LockOwner&) = delete;
    LockOwner& operator=(const LockOwner&) = delete;

    bool OwnsLock() const noexcept {
        return owns_;
    }

    // 쥐지 않았을 때만 부른다(쥔 채 부르면 교착이다).
    void Lock() noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
        lock_->Lock();
        owns_ = true;
    }

    bool TryLock() noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
        owns_ = lock_->TryLock();
        return owns_;
    }

    // 쥐었을 때만 부른다.
    void Unlock() noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
        lock_->Unlock();
        owns_ = false;
    }

private:
    Lockable* lock_;
    bool owns_ = false;
};

// 범위 동안 쥔 잠금을 풀어 두는 가드다(UE UE::TScopeUnlock 대응). 보호 구간 안에서 사용자
// callback을 잠금 없이 부를 때 쓴다. 생성 때 Unlock, 소멸 때 Lock한다(쥔 잠금에만 쓴다).
template <class Lockable>
class UnlockGuard {
public:
    explicit UnlockGuard(Lockable& lock) noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS : lock_(lock) {
        lock_.Unlock();
    }

    ~UnlockGuard() noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
        lock_.Lock();
    }

    UnlockGuard(const UnlockGuard&) = delete;
    UnlockGuard& operator=(const UnlockGuard&) = delete;

private:
    Lockable& lock_;
};

// 1 byte 조건 변수다(8.6절, D9). UE에는 없고 Rocks의 "잠금 아래 상태 + 기다림" 사용처를 위해 둔다.
// Mutex와만 짝짓는다. 상수 초기화되고 파괴자가 없다.
//
// Wait는 짝 Mutex를 원자적으로 풀고 기다린 뒤 다시 잠근 채 돌아온다. Signaled도 spurious wake일 수
// 있으므로 predicate를 반복 검사한다. 유한 반복 대기는 최초 deadline의 남은 시간만 넘긴다.
// predicate는 짝 Mutex를 쥔 채 바꾸고, Notify는 Mutex 밖에서 불러도 된다.
//
// 사용 예:
//   LockGuard lock(mutex);
//   while (!ready) { changed.Wait(mutex); }
//   // 깨우는 쪽: { LockGuard lock(mutex); ready = true; } changed.NotifyAll();
class ConditionVariable {
public:
    constexpr ConditionVariable() noexcept = default;
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    ROCKS_PLATFORMBASE_API WaitResult Wait(Mutex& mutex,
                                           uint32 timeoutMs = InfiniteTimeout) noexcept
        ROCKS_REQUIRES(mutex);

    void NotifyOne() noexcept {
        if ((state_.Load<MemoryOrder::Relaxed>() & WaitingBit) != 0) {
            NotifySlow(1);
        }
    }

    void NotifyAll() noexcept {
        if ((state_.Load<MemoryOrder::Relaxed>() & WaitingBit) != 0) {
            NotifySlow(NumericLimits<uint32>::Max());
        }
    }

private:
    static constexpr uint8 WaitingBit = 1;

    ROCKS_PLATFORMBASE_API void NotifySlow(uint32 count) noexcept;

    Atomic<uint8> state_;
};

static_assert(sizeof(ConditionVariable) == 1);

// Signal 이후 Reset까지 모든 waiter와 이후 Wait가 통과한다. thread affinity는 없다.
// 동시 Signal/Reset/Wait를 지원하나 파괴 전에는 모든 caller를 종료해야 한다. Close/detach는 없다.
class ROCKS_PLATFORMBASE_API ManualResetEvent {
public:
    explicit ManualResetEvent(bool signaled = false) noexcept;
    ManualResetEvent(const ManualResetEvent&) = delete;
    ManualResetEvent& operator=(const ManualResetEvent&) = delete;

    void Signal() noexcept;
    // 막히지 않는 Signal이다. 내부 lock을 TryLock으로 attempts번까지 시도하고, 잡으면 Signal과 같고
    // true다. 못 잡으면 false다(종료 경로처럼 막히면 안 되는 곳에서 쓴다).
    bool TrySignal(uint32 attempts = 1) noexcept;
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
class ROCKS_PLATFORMBASE_API Semaphore {
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

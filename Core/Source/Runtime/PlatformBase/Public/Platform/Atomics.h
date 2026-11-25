#pragma once

#include <Base/Compiler.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Platform/Fatal.h>

namespace rocks::platform {

// ---- 원자 연산(UE FPlatformAtomics 대응) ----
// 컴파일러 원자 builtin(__atomic_*)을 직접 부르지 않고 이 함수를 쓴다. 다른 스레드와 공유하는
// 정수·포인터·열거형 한 칸을 잠금 없이 읽고 쓴다. 여러 필드를 함께 바꾸거나 기다려야 하면
// Platform/Sync.h의 Mutex·ConditionVariable을 쓴다.
//
// 메모리 순서는 템플릿 인자로 받는다. 컴파일 시점 상수이므로 잘못된 조합(예: Acquire 저장)은
// static_assert로 막고, 생성 코드는 그 순서의 명령 하나로 내려간다. 생략하면 가장 강한
// SequentiallyConsistent다.
//
//   uint64 count = 0;
//   AtomicFetchAdd<MemoryOrder::Relaxed>(&count, 1);                // 통계 증가
//   if (AtomicFetchSub<MemoryOrder::AcquireRelease>(&refs, 1) == 1)   // 마지막 해제
//       Destroy();
//   while (AtomicLoad<MemoryOrder::Acquire>(&ready) == 0) { ... }     // 발행 관찰
//
// 대상 타입 T는 크기 1·2·4·8 byte의 정수·열거형·포인터이며, 그 크기에서 대상 CPU가 잠금 없이
// 처리해야 한다(외부 원자 라이브러리를 부르지 않는다). 주소는 T의 자연 정렬을 지켜야 한다.
// bool은 산술 연산(Fetch*) 대상이 아니다. 포인터에 Fetch 산술은 원소가 아닌 byte 단위로 더해져
// 혼동되므로 막는다(정수 인덱스를 원자로 두고 포인터를 계산한다).

// C++ 메모리 모델의 순서다(표준 라이브러리 memory_order 대응). 값은 컴파일러 builtin 상수와 같다.
enum class MemoryOrder : int {
    // 원자성만 보장한다. 다른 메모리 접근과의 순서는 보장하지 않는다(통계·고유 번호).
    Relaxed = __ATOMIC_RELAXED,
    // 읽기 전용. 이 읽기 뒤의 접근이 앞으로 당겨지지 않는다(Release 쓰기의 발행을 관찰).
    Acquire = __ATOMIC_ACQUIRE,
    // 쓰기 전용. 이 쓰기 앞의 접근이 뒤로 밀리지 않는다(데이터를 채운 뒤 발행).
    Release = __ATOMIC_RELEASE,
    // 읽고-쓰기 연산 전용. Acquire와 Release를 함께 건다(참조 계수 감소).
    AcquireRelease = __ATOMIC_ACQ_REL,
    // AcquireRelease에 더해 모든 SequentiallyConsistent 연산이 하나의 전역 순서를 이룬다.
    SequentiallyConsistent = __ATOMIC_SEQ_CST,
};

namespace detail {

// 값 인자를 타입 추론에서 빼서 대상 주소의 타입만으로 T를 정한다(AtomicFetchAdd(&count64, 1) 허용).
template <class T>
struct AtomicOperand {
    using Type = T;
};

template <class T>
inline constexpr bool IsAtomicValue =
    (IsIntegralV<T> || IsEnumV<T> || IsPointerV<T>) &&
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8) &&
    __atomic_always_lock_free(sizeof(T), 0);

template <class T>
inline constexpr bool IsAtomicArithmetic = IsIntegralV<T> && !IsSameV<T, bool> && IsAtomicValue<T>;

template <MemoryOrder Order>
inline constexpr bool IsLoadOrder =
    Order == MemoryOrder::Relaxed || Order == MemoryOrder::Acquire ||
    Order == MemoryOrder::SequentiallyConsistent;

template <MemoryOrder Order>
inline constexpr bool IsStoreOrder =
    Order == MemoryOrder::Relaxed || Order == MemoryOrder::Release ||
    Order == MemoryOrder::SequentiallyConsistent;

} // namespace detail

// address의 현재 값을 읽는다. Order는 Relaxed·Acquire·SequentiallyConsistent 중 하나다.
template <MemoryOrder Order = MemoryOrder::SequentiallyConsistent, class T>
inline T AtomicLoad(const T* address) noexcept {
    static_assert(detail::IsAtomicValue<T>,
                  "AtomicLoad requires a lock-free integer, enum or pointer");
    static_assert(detail::IsLoadOrder<Order>,
                  "AtomicLoad order must be Relaxed, Acquire or SequentiallyConsistent");
    T result;
    __atomic_load(address, &result, static_cast<int>(Order));
    return result;
}

// address에 value를 쓴다. Order는 Relaxed·Release·SequentiallyConsistent 중 하나다.
template <MemoryOrder Order = MemoryOrder::SequentiallyConsistent, class T>
inline void AtomicStore(T* address, typename detail::AtomicOperand<T>::Type value) noexcept {
    static_assert(detail::IsAtomicValue<T>,
                  "AtomicStore requires a lock-free integer, enum or pointer");
    static_assert(detail::IsStoreOrder<Order>,
                  "AtomicStore order must be Relaxed, Release or SequentiallyConsistent");
    __atomic_store(address, &value, static_cast<int>(Order));
}

// address를 value로 바꾸고 이전 값을 반환한다.
template <MemoryOrder Order = MemoryOrder::SequentiallyConsistent, class T>
inline T AtomicExchange(T* address, typename detail::AtomicOperand<T>::Type value) noexcept {
    static_assert(detail::IsAtomicValue<T>,
                  "AtomicExchange requires a lock-free integer, enum or pointer");
    T previous;
    __atomic_exchange(address, &value, &previous, static_cast<int>(Order));
    return previous;
}

// address가 expected와 같으면 desired로 바꾸고 true를 반환한다(Success 순서). 다르면 바꾸지 않고
// 현재 값을 expected에 쓴 뒤 false를 반환한다(Failure 순서). Failure는 쓰기를 하지 않으므로
// Relaxed·Acquire·SequentiallyConsistent 중 하나다.
// Weak는 값이 같아도 드물게 실패할 수 있어 재시도 루프 안에서만 쓴다. 대신 LL/SC CPU(ARM)에서
// 더 싸다. 한 번의 판정이 필요하면 Strong을 쓴다.
//
//   uint64 expected = AtomicLoad<MemoryOrder::Relaxed>(&strong);
//   while (expected != 0 && !AtomicCompareExchangeWeak<MemoryOrder::AcquireRelease,
//                                                      MemoryOrder::Relaxed>(&strong, expected,
//                                                                            expected + 1)) {
//   }
template <MemoryOrder Success = MemoryOrder::SequentiallyConsistent,
          MemoryOrder Failure = MemoryOrder::SequentiallyConsistent, class T>
inline bool AtomicCompareExchangeWeak(T* address, T& expected,
                                      typename detail::AtomicOperand<T>::Type desired) noexcept {
    static_assert(detail::IsAtomicValue<T>,
                  "AtomicCompareExchangeWeak requires a lock-free integer, enum or pointer");
    static_assert(
        detail::IsLoadOrder<Failure>,
        "compare-exchange failure order must be Relaxed, Acquire or SequentiallyConsistent");
    return __atomic_compare_exchange(address, &expected, &desired, true, static_cast<int>(Success),
                                     static_cast<int>(Failure));
}

template <MemoryOrder Success = MemoryOrder::SequentiallyConsistent,
          MemoryOrder Failure = MemoryOrder::SequentiallyConsistent, class T>
inline bool AtomicCompareExchangeStrong(T* address, T& expected,
                                        typename detail::AtomicOperand<T>::Type desired) noexcept {
    static_assert(detail::IsAtomicValue<T>,
                  "AtomicCompareExchangeStrong requires a lock-free integer, enum or pointer");
    static_assert(
        detail::IsLoadOrder<Failure>,
        "compare-exchange failure order must be Relaxed, Acquire or SequentiallyConsistent");
    return __atomic_compare_exchange(address, &expected, &desired, false, static_cast<int>(Success),
                                     static_cast<int>(Failure));
}

// address에 산술·비트 연산을 적용하고 연산 전 값을 반환한다. 정수(bool 제외)만 받는다.
// 부호 없는 정수는 2^N으로 감싸고, 부호 있는 정수도 2의 보수로 감싼다(정의된 동작).
template <MemoryOrder Order = MemoryOrder::SequentiallyConsistent, class T>
inline T AtomicFetchAdd(T* address, typename detail::AtomicOperand<T>::Type value) noexcept {
    static_assert(detail::IsAtomicArithmetic<T>, "AtomicFetchAdd requires a lock-free integer");
    return __atomic_fetch_add(address, value, static_cast<int>(Order));
}

template <MemoryOrder Order = MemoryOrder::SequentiallyConsistent, class T>
inline T AtomicFetchSub(T* address, typename detail::AtomicOperand<T>::Type value) noexcept {
    static_assert(detail::IsAtomicArithmetic<T>, "AtomicFetchSub requires a lock-free integer");
    return __atomic_fetch_sub(address, value, static_cast<int>(Order));
}

template <MemoryOrder Order = MemoryOrder::SequentiallyConsistent, class T>
inline T AtomicFetchAnd(T* address, typename detail::AtomicOperand<T>::Type value) noexcept {
    static_assert(detail::IsAtomicArithmetic<T>, "AtomicFetchAnd requires a lock-free integer");
    return __atomic_fetch_and(address, value, static_cast<int>(Order));
}

template <MemoryOrder Order = MemoryOrder::SequentiallyConsistent, class T>
inline T AtomicFetchOr(T* address, typename detail::AtomicOperand<T>::Type value) noexcept {
    static_assert(detail::IsAtomicArithmetic<T>, "AtomicFetchOr requires a lock-free integer");
    return __atomic_fetch_or(address, value, static_cast<int>(Order));
}

template <MemoryOrder Order = MemoryOrder::SequentiallyConsistent, class T>
inline T AtomicFetchXor(T* address, typename detail::AtomicOperand<T>::Type value) noexcept {
    static_assert(detail::IsAtomicArithmetic<T>, "AtomicFetchXor requires a lock-free integer");
    return __atomic_fetch_xor(address, value, static_cast<int>(Order));
}

// 원자 변수 없이 메모리 순서만 건다(표준 라이브러리 atomic_thread_fence 대응). Relaxed는 아무
// 일도 하지 않는다. 짝이 되는 원자 읽기·쓰기가 Relaxed일 때 순서를 보강하는 용도다.
template <MemoryOrder Order>
inline void AtomicThreadFence() noexcept {
    __atomic_thread_fence(static_cast<int>(Order));
}

// ---- 원자 값 타입(Docs/Design/Synchronization 11절) ----

namespace detail {

// 읽고-쓰기 순서 Order로 시작한 CAS 반복이 실패했을 때의 순서다(쓰기가 없으므로 읽기 부분만).
template <MemoryOrder Order>
inline constexpr MemoryOrder FailureOrderFor =
    Order == MemoryOrder::AcquireRelease ? MemoryOrder::Acquire
    : Order == MemoryOrder::Release      ? MemoryOrder::Relaxed
                                         : Order;

// Atomic<T>·AtomicRef<T>의 공용 멤버다. Owner는 MutableAddress()·ConstAddress()를 준다.
template <class T, class Owner>
class AtomicOperations {
    static_assert(IsAtomicValue<T>, "Atomic requires a lock-free integer, enum, pointer or bool");

public:
    // 현재 값을 읽는다. Order는 Relaxed·Acquire·SequentiallyConsistent 중 하나다.
    template <MemoryOrder Order>
    T Load() const noexcept {
        return AtomicLoad<Order>(Self().ConstAddress());
    }

    // value를 쓴다. Order는 Relaxed·Release·SequentiallyConsistent 중 하나다.
    template <MemoryOrder Order>
    void Store(T value) noexcept {
        AtomicStore<Order>(Self().MutableAddress(), value);
    }

    // value로 바꾸고 이전 값을 돌려준다.
    template <MemoryOrder Order>
    T Exchange(T value) noexcept {
        return AtomicExchange<Order>(Self().MutableAddress(), value);
    }

    // 값이 expected와 같으면 desired로 바꾸고 true다. 다르면 현재 값을 expected에 쓰고 false다.
    // Weak는 같아도 드물게 실패하므로 반복 안에서만 쓴다.
    template <MemoryOrder Success, MemoryOrder Failure>
    bool CompareExchangeWeak(T& expected, T desired) noexcept {
        return AtomicCompareExchangeWeak<Success, Failure>(Self().MutableAddress(), expected,
                                                           desired);
    }

    template <MemoryOrder Success, MemoryOrder Failure>
    bool CompareExchangeStrong(T& expected, T desired) noexcept {
        return AtomicCompareExchangeStrong<Success, Failure>(Self().MutableAddress(), expected,
                                                             desired);
    }

    // 산술·비트 연산을 적용하고 연산 전 값을 돌려준다. 정수(bool 제외)만 받는다.
    template <MemoryOrder Order>
    T FetchAdd(T value) noexcept
        requires IsAtomicArithmetic<T>
    {
        return AtomicFetchAdd<Order>(Self().MutableAddress(), value);
    }

    template <MemoryOrder Order>
    T FetchSub(T value) noexcept
        requires IsAtomicArithmetic<T>
    {
        return AtomicFetchSub<Order>(Self().MutableAddress(), value);
    }

    template <MemoryOrder Order>
    T FetchAnd(T value) noexcept
        requires IsAtomicArithmetic<T>
    {
        return AtomicFetchAnd<Order>(Self().MutableAddress(), value);
    }

    template <MemoryOrder Order>
    T FetchOr(T value) noexcept
        requires IsAtomicArithmetic<T>
    {
        return AtomicFetchOr<Order>(Self().MutableAddress(), value);
    }

    template <MemoryOrder Order>
    T FetchXor(T value) noexcept
        requires IsAtomicArithmetic<T>
    {
        return AtomicFetchXor<Order>(Self().MutableAddress(), value);
    }

    // ---- 보조 연산(11.3절). 모두 CAS 반복이고 성공 순서는 Order, 실패 순서는 그 읽기 부분이다.
    // ----

    // 현재 값보다 value가 크면 value로 바꾼다. 이전 값을 돌려준다(바꾸지 않았으면 현재 값).
    template <MemoryOrder Order>
    T FetchMax(T value) noexcept
        requires IsAtomicArithmetic<T>
    {
        T current = Load<MemoryOrder::Relaxed>();
        while (current < value &&
               !CompareExchangeWeak<Order, FailureOrderFor<Order>>(current, value)) {
        }

        return current;
    }

    // 더한 결과가 limit 이하일 때만 value를 더한다. 더했으면 true이고 previous에 이전 값을 쓴다.
    // 넘으면 바꾸지 않고 false다(previous는 그대로). 넘침(감김)도 넘는 것으로 본다.
    //
    // 사용 예(한도 안 예약):
    //   uint64 start = 0;
    //   if (!reserved.TryFetchAddBounded<MemoryOrder::Relaxed>(bytes, capacity, start)) return
    //   false;
    template <MemoryOrder Order>
    bool TryFetchAddBounded(T value, T limit, T& previous) noexcept
        requires IsAtomicArithmetic<T>
    {
        T current = Load<MemoryOrder::Relaxed>();
        while (true) {
            if (current > limit || value > static_cast<T>(limit - current)) {
                return false;
            }

            if (CompareExchangeWeak<Order, FailureOrderFor<Order>>(
                    current, static_cast<T>(current + value))) {
                previous = current;
                return true;
            }
        }
    }

    // value를 더하되 결과가 limit를 넘으면 limit에 멈춘다. 이전 값을 돌려준다. 이미 limit 이상이면
    // 바꾸지 않는다.
    template <MemoryOrder Order>
    T FetchAddSaturating(T value, T limit) noexcept
        requires IsAtomicArithmetic<T>
    {
        T current = Load<MemoryOrder::Relaxed>();
        while (current < limit) {
            const T next =
                value > static_cast<T>(limit - current) ? limit : static_cast<T>(current + value);
            if (CompareExchangeWeak<Order, FailureOrderFor<Order>>(current, next)) {
                break;
            }
        }

        return current;
    }

    // 0이 아닐 때만 1 올린다(약한 참조의 강한 참조 승격). 올렸으면 true다. 최댓값에서는 넘치지
    // 않게 false다.
    template <MemoryOrder Order>
    bool TryIncrementIfNonZero() noexcept
        requires IsAtomicArithmetic<T>
    {
        T current = Load<MemoryOrder::Relaxed>();
        while (current != 0 && current != NumericLimits<T>::Max()) {
            if (CompareExchangeWeak<Order, FailureOrderFor<Order>>(current,
                                                                   static_cast<T>(current + 1))) {
                return true;
            }
        }

        return false;
    }

    // 0을 쓰지 않는 단조 번호 하나를 받는다(값은 1부터 오르고 최댓값까지 쓴다). 받았으면 true이고
    // number에 쓴다. 다 썼으면 false다(호출자가 Fatal하거나 오류로 돌려준다). 상태 값은 마지막으로
    // 준 번호다.
    template <MemoryOrder Order>
    bool TakeUniqueNumber(T& number) noexcept
        requires IsAtomicArithmetic<T>
    {
        T current = Load<MemoryOrder::Relaxed>();
        while (current != NumericLimits<T>::Max()) {
            const T next = static_cast<T>(current + 1);
            if (CompareExchangeWeak<Order, FailureOrderFor<Order>>(current, next)) {
                number = next;
                return true;
            }
        }

        return false;
    }

    // 현재 값에 function(T → T, noexcept)을 적용한 결과를 쓴다. 이전 값을 돌려준다. function은 경쟁
    // 때 여러 번 불리므로 부작용이 없어야 한다.
    //
    // 사용 예(double을 uint64 bit 표현으로 더하기):
    //   bits.FetchUpdate<MemoryOrder::Relaxed>([&](uint64 old) noexcept {
    //       return BitCast<uint64>(BitCast<double>(old) + delta); });
    template <MemoryOrder Order, class Function>
    T FetchUpdate(Function&& function) noexcept {
        static_assert(noexcept(function(T{})), "FetchUpdate function must be noexcept");
        T current = Load<MemoryOrder::Relaxed>();
        while (!CompareExchangeWeak<Order, FailureOrderFor<Order>>(current, function(current))) {
        }

        return current;
    }

private:
    Owner& Self() noexcept {
        return static_cast<Owner&>(*this);
    }

    const Owner& Self() const noexcept {
        return static_cast<const Owner&>(*this);
    }
};

} // namespace detail

// 다른 스레드와 공유하는 값 하나다. 모든 접근은 원자적이고 메모리 순서를 늘 템플릿 인자로 적는다
// (기본값이 없다). 연산자 overload(=, ++, 암시적 변환)는 두지 않는다: 순서가 코드에 드러나야
// 하고, 실수로 일반 읽기·쓰기를 섞지 않게 한다. 상수 초기화되고 파괴자가 없어 constinit 전역으로
// 둘 수 있다. 크기와 정렬은 T와 같다(8 byte 값이 구조체 안에서 4 byte 정렬로 떨어지지 않는다).
//
// T는 1·2·4·8 byte의 정수·열거형·포인터·bool이고 대상 CPU가 잠금 없이 처리해야 한다. 산술(Fetch*)은
// bool이 아닌 정수만 받는다.
//
// 사용 예:
//   Atomic<uint64> dropped;
//   dropped.FetchAdd<MemoryOrder::Relaxed>(1);
//   if (refs.FetchSub<MemoryOrder::AcquireRelease>(1) == 1) Destroy();
//   while (ready.Load<MemoryOrder::Acquire>() == 0) { backoff.Pause(); }
template <class T>
class Atomic : public detail::AtomicOperations<T, Atomic<T>> {
public:
    constexpr Atomic() noexcept = default;

    constexpr explicit Atomic(T initial) noexcept : value_(initial) {
    }

    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

private:
    friend class detail::AtomicOperations<T, Atomic<T>>;

    T* MutableAddress() noexcept {
        return &value_;
    }

    const T* ConstAddress() const noexcept {
        return &value_;
    }

    alignas(sizeof(T)) T value_{};
};

// 이미 있는 T 객체 하나를 원자로 본다. 객체는 AtomicRef보다 오래 살고 T의 자연 정렬을 지켜야
// 한다(Debug·Development는 정렬을 검사한다). 같은 객체를 AtomicRef로 보는 동안 다른 스레드가 일반
// 접근을 하면 안 된다. 멤버와 순서 규칙은 Atomic<T>와 같다. trivially copyable이어야 하는 칸,
// 외부·공유 메모리, 시기별로 일반 접근과 원자 접근이 나뉘는 값(초기화 때 일반 쓰기, 게시 뒤 원자
// 읽기)에 쓴다.
//
// 사용 예:
//   AtomicRef<uint64>(cell.sequence).Store<MemoryOrder::Release>(next);
template <class T>
class AtomicRef : public detail::AtomicOperations<T, AtomicRef<T>> {
public:
    explicit AtomicRef(T& object) noexcept : object_(&object) {
#if !ROCKS_BUILD_RELEASE
        if (reinterpret_cast<uptr>(object_) % sizeof(T) != 0) {
            Fatal("AtomicRef object is not naturally aligned");
        }
#endif
    }

    AtomicRef(const AtomicRef&) noexcept = default;
    AtomicRef& operator=(const AtomicRef&) = delete;

private:
    friend class detail::AtomicOperations<T, AtomicRef<T>>;

    T* MutableAddress() noexcept {
        return object_;
    }

    const T* ConstAddress() const noexcept {
        return object_;
    }

    T* object_;
};

// ---- spin 힌트(11.2절) ----

// 같은 값이 바뀔 때까지 짧게 도는 반복 안에서 CPU에 "기다리는 중"을 알린다(x86-64 pause, ARM64
// isb — UE와 같다). OS 양보(YieldThread)와 다르다: 스레드를 내놓지 않는다.
inline void CpuPause() noexcept {
#if ROCKS_ARCH_X64
    __builtin_ia32_pause();
#else
    __builtin_arm_isb(15);
#endif
}

namespace detail {

// SpinBackoff가 PauseLimit를 넘긴 뒤 부르는 OS 양보다(Platform/Thread.h의 YieldThread와 같다).
// Atomics.h가 Thread.h를 include하지 않도록 따로 둔다.
ROCKS_PLATFORMBASE_API void YieldAfterSpin() noexcept;

} // namespace detail

// 값이 바뀔 때까지 도는 반복 하나의 상태다. 처음 PauseLimit번은 CpuPause, 그 뒤로는 반복마다
// OS 양보다. 시간이 아니라 횟수로 넘어간다. 상대가 같은 코어에서 선점되어 멈췄을 때 CPU를 태우며
// 상대를 막지 않게 한다. 오래 걸릴 수 있는 기다림은 반복이 아니라 대기(EventCount 등)로 한다.
//
// 사용 예(소비자가 미공개 칸을 기다린다):
//   SpinBackoff backoff;
//   while (!cell.IsPublished()) { backoff.Pause(); }
class SpinBackoff {
public:
    // 잠금 spin과 같은 횟수다(UE FMutex의 40과 같다).
    static constexpr uint32 PauseLimit = 40;

    void Pause() noexcept {
        if (count_ < PauseLimit) {
            ++count_;
            CpuPause();
            return;
        }

        detail::YieldAfterSpin();
    }

    // 지금까지 CpuPause로 넘어간 횟수다(PauseLimit에서 멈춘다). 시험·통계용이다.
    uint32 PauseCount() const noexcept {
        return count_;
    }

private:
    uint32 count_ = 0;
};

} // namespace rocks::platform

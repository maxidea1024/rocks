#pragma once

#include <Base/Types.h>

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
    (__is_integral(T) || __is_enum(T) || __is_pointer(T)) &&
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8) &&
    __atomic_always_lock_free(sizeof(T), 0);

template <class T>
inline constexpr bool IsAtomicArithmetic =
    __is_integral(T) && !__is_same(T, bool) && IsAtomicValue<T>;

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

} // namespace rocks::platform

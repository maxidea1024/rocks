#pragma once

#include <Core/Containers/Span.h>
#include <Core/Move.h>
#include <Platform/Memory.h>

namespace rocks::algo {

// BinarySearch가 찾지 못한 결과다. LowerBound/UpperBound는 대신 삽입 위치 Size()를 반환한다.
inline constexpr usize NotFound = MaxSize;

struct Less {
    template <typename A, typename B>
    constexpr bool operator()(const A& left, const B& right) const
        noexcept(noexcept(static_cast<bool>(left < right))) {
        return static_cast<bool>(left < right);
    }
};

namespace detail {

template <typename T>
void Swap(T& left, T& right) noexcept {
    T* leftAddress = platform::AddressOf(left);
    T* rightAddress = platform::AddressOf(right);
    if (leftAddress == rightAddress) {
        return;
    }

    T temporary(Move(left));
    DestroyAt(leftAddress);
    ConstructAt<T>(leftAddress, Move(right));
    DestroyAt(rightAddress);
    ConstructAt<T>(rightAddress, Move(temporary));
}

template <typename T, typename Compare>
void SiftDown(Span<T> values, usize root, usize count, Compare& compare) noexcept {
    // root < count / 2일 때에만 자식 인덱스를 계산하여 2 * root + 1 overflow를 방지한다.
    while (root < count / 2) {
        usize child = root * 2 + 1;
        if (child + 1 < count && static_cast<bool>(compare(values[child], values[child + 1]))) {
            ++child;
        }
        if (!static_cast<bool>(compare(values[root], values[child]))) {
            return;
        }

        Swap(values[root], values[child]);
        root = child;
    }
}

} // namespace detail

// compare(a, b)는 a가 b보다 앞서는지 판단하는 noexcept 엄격 약순서여야 한다.
// 추가 할당 없이 최악 O(n log n)으로 정렬한다. 동등 원소의 순서는 보장하지 않는다.
// 원소의 주소는 유지되지만 이동·파괴·재생성되므로 원소의 논리적 정체성과 참조에 주의한다.
// 객체 이동 중 원소 주소를 저장하는 타입의 내부 참조는 자동 보정하지 않는다.
// UE TArray::Sort와 달리 포인터 원소를 자동 역참조하지 않는다. 필요하면 비교기를 지정한다.
// 기본 포인터 비교는 서로 무관한 객체의 주소에 이식 가능한 전체 순서를 제공하지 않는다.
// 비교기는 정렬 중 범위·원본 컨테이너를 변경하거나 정렬에 재진입해서는 안 된다.
// 호출 중 다른 스레드에서 해당 범위를 읽거나 변경하려면 외부 동기화가 필요하다.
// 예: algo::Sort(Span<int>(nativeArray)); algo::Sort(engineArray.View());
template <typename T, typename Compare>
void Sort(Span<T> values, Compare&& compare) noexcept {
    static_assert(!IsSameV<T, const T>, "Sort requires mutable elements");
    static_assert(IsNothrowMoveConstructibleV<T> && IsNothrowDestructibleV<T>);
    static_assert(noexcept(static_cast<bool>(compare(values[0], values[0]))),
                  "Sort comparator must be noexcept");

    const usize count = values.Size();
    for (usize parent = count / 2; parent != 0; --parent) {
        detail::SiftDown(values, parent - 1, count, compare);
    }
    for (usize end = count; end > 1; --end) {
        detail::Swap(values[0], values[end - 1]);
        detail::SiftDown(values, 0, end - 1, compare);
    }
}

template <typename T>
void Sort(Span<T> values) noexcept {
    Less compare;
    Sort(values, compare);
}

// 검색 범위는 동일한 비교기로 이미 정렬되어 있어야 한다. 찾지 못하면 삽입 위치를 반환한다.
// compare(element, key)와 compare(key, element)는 각각 noexcept여야 한다.
// 입력과 비교기가 빌리는 메모리는 호출이 끝날 때까지 살아 있어야 하며 다른 스레드에서 변경하지
// 않는다.
template <typename T, typename Key, typename Compare>
usize LowerBound(Span<T> values, const Key& key, Compare&& compare) noexcept {
    static_assert(noexcept(static_cast<bool>(compare(static_cast<const T&>(values[0]), key))),
                  "LowerBound comparator must be noexcept");

    usize first = 0;
    usize count = values.Size();
    while (count != 0) {
        const usize half = count / 2;
        const usize middle = first + half;
        if (static_cast<bool>(compare(static_cast<const T&>(values[middle]), key))) {
            first = middle + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    return first;
}

template <typename T, typename Key>
usize LowerBound(Span<T> values, const Key& key) noexcept {
    Less compare;
    return LowerBound(values, key, compare);
}

template <typename T, typename Key, typename Compare>
usize UpperBound(Span<T> values, const Key& key, Compare&& compare) noexcept {
    static_assert(noexcept(static_cast<bool>(compare(key, static_cast<const T&>(values[0])))),
                  "UpperBound comparator must be noexcept");

    usize first = 0;
    usize count = values.Size();
    while (count != 0) {
        const usize half = count / 2;
        const usize middle = first + half;
        if (!static_cast<bool>(compare(key, static_cast<const T&>(values[middle])))) {
            first = middle + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    return first;
}

template <typename T, typename Key>
usize UpperBound(Span<T> values, const Key& key) noexcept {
    Less compare;
    return UpperBound(values, key, compare);
}

// 동등한 원소가 여러 개이면 첫 위치를 반환하고 없으면 NotFound를 반환한다.
template <typename T, typename Key, typename Compare>
usize BinarySearch(Span<T> values, const Key& key, Compare&& compare) noexcept {
    static_assert(noexcept(static_cast<bool>(compare(key, static_cast<const T&>(values[0])))),
                  "BinarySearch comparator must be noexcept");
    const usize index = LowerBound(values, key, compare);
    if (index == values.Size() ||
        static_cast<bool>(compare(key, static_cast<const T&>(values[index])))) {
        return NotFound;
    }
    return index;
}

template <typename T, typename Key>
usize BinarySearch(Span<T> values, const Key& key) noexcept {
    Less compare;
    return BinarySearch(values, key, compare);
}

} // namespace rocks::algo

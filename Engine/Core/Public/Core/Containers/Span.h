#pragma once

#include <Base/Types.h>
#include <Platform/Fatal.h>

namespace rocks {

// 연속된 원소를 빌리는 view다. 메모리·객체 수명·동기화를 소유하지 않는다.
// 원본 파괴·재할당 뒤에는 사용할 수 없고, const Span<T>도 T가 mutable이면 원소 변경을 허용한다.
// 읽기 전용 원소가 필요하면 Span<const T>를 사용한다.
template <typename T>
class Span {
public:
    constexpr Span() noexcept = default;

    // count는 byte 수가 아닌 원소 수다. 실제 범위·정렬·수명은 caller가 보장한다.
    // 배열 간 호환성을 검사하여 Derived 배열을 Base 간격으로 잘못 순회하는 변환을 거부한다.
    template <typename U>
        requires(__is_convertible_to(U (*)[], T (*)[]))
    constexpr Span(U* data, usize count) noexcept : data_(data), count_(count) {
        if (data == nullptr && count != 0) {
            platform::Fatal("A nonempty Span requires storage");
        }
    }

    constexpr Span(decltype(nullptr), usize count) noexcept : count_(count) {
        if (count != 0) {
            platform::Fatal("A nonempty Span requires storage");
        }
    }

    template <usize Count>
    constexpr Span(T (&values)[Count]) noexcept : data_(values), count_(Count) {
    }

    // 동일 원소 타입의 const 추가 같은 안전한 변환만 허용하며 buffer를 복사하지 않는다.
    template <typename U>
        requires(__is_convertible_to(U (*)[], T (*)[]))
    constexpr Span(Span<U> other) noexcept : data_(other.Data()), count_(other.Size()) {
    }

    constexpr T* Data() const noexcept {
        return data_;
    }

    constexpr usize Size() const noexcept {
        return count_;
    }

    // 저장된 count 밖의 접근은 Fatal이다. 원본 storage가 여전히 살아 있는지는 검사할 수 없다.
    constexpr T& operator[](usize index) const noexcept {
        if (index >= count_) {
            platform::Fatal("Span index out of bounds");
        }

        return data_[index];
    }

private:
    T* data_ = nullptr;
    usize count_ = 0;
};

} // namespace rocks

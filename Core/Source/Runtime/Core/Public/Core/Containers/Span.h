#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Core/Traits.h>
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

    // 원소가 차지하는 byte 수(Size() * sizeof(T))다. 실제로 존재하는 저장소를 가리키므로 넘치지
    // 않는다.
    constexpr usize ByteCount() const noexcept {
        return count_ * sizeof(T);
    }

    constexpr bool IsEmpty() const noexcept {
        return count_ == 0;
    }

    // 저장된 count 밖의 접근은 Fatal이다. 원본 storage가 여전히 살아 있는지는 검사할 수 없다.
    constexpr T& operator[](usize index) const noexcept {
        if (index >= count_) {
            platform::Fatal("Span index out of bounds");
        }

        return data_[index];
    }

    // ---- 하위 범위 ----
    // 결과는 같은 저장소를 빌리는 view이며 원본과 수명이 같다. 원소를 복사하지 않는다.
    //
    // 사용 예(길이를 이미 검사한 header ‖ body 분리):
    //   const Span<const uint8> header = packet.Left(HeaderBytes);
    //   const Span<const uint8> body = packet.RightChop(HeaderBytes);
    //
    // Slice는 [index, index + count)가 범위 밖이면 Fatal이다(operator[]와 같은 규칙). 길이를 이미
    // 검사한 경로에서 쓴다. 외부 입력을 자를 때는 범위 밖을 오류로 돌려주는 TrySlice를 쓴다.
    constexpr Span Slice(usize index, usize count) const noexcept {
        if (index > count_ || count > count_ - index) {
            platform::Fatal("Span slice out of bounds");
        }

        return Span(data_ + index, count);
    }

    // [index, index + count)가 범위 밖이면 InvalidArgument다(index + count의 overflow 포함). 상태를
    // 바꾸지 않는다.
    Result<Span> TrySlice(usize index, usize count) const noexcept {
        if (index > count_ || count > count_ - index) {
            return Result<Span>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Span>::Success(Span(data_ + index, count));
    }

    // 아래 넷과 Mid는 범위를 넘는 count를 Size()로 잘라 낸다(StringView::TryLeftBytes 등과 같은
    // 의미). 앞 count개다.
    constexpr Span Left(usize count) const noexcept {
        return Span(data_, count < count_ ? count : count_);
    }

    // 뒤 count개를 뺀 나머지다.
    constexpr Span LeftChop(usize count) const noexcept {
        return Span(data_, count < count_ ? count_ - count : 0);
    }

    // 뒤 count개다.
    constexpr Span Right(usize count) const noexcept {
        const usize kept = count < count_ ? count : count_;
        return Span(data_ + (count_ - kept), kept);
    }

    // 앞 count개를 뺀 나머지다.
    constexpr Span RightChop(usize count) const noexcept {
        const usize dropped = count < count_ ? count : count_;
        return Span(data_ + dropped, count_ - dropped);
    }

    // index부터 count개다. index가 Size()를 넘으면 빈 view, count는 남은 길이로 잘라 낸다.
    constexpr Span Mid(usize index, usize count = MaxSize) const noexcept {
        const usize start = index < count_ ? index : count_;
        const usize remaining = count_ - start;
        return Span(data_ + start, count < remaining ? count : remaining);
    }

    // ---- STL 호환 별칭과 반복자 ----
    // 반복자는 원시 포인터이므로 range-for, STL 알고리즘(sort 등)과 ranges 라이브러리에서
    // contiguous iterator로 동작한다. Span은 원소를 소유하지 않으므로 원본의 수명·재할당 규약을
    // 그대로 따른다. const Span<T>도 T가 mutable이면 begin()으로 원소를 바꿀 수 있다.
    using element_type = T;
    using value_type = RemoveConstT<T>;
    using size_type = usize;
    using difference_type = isize;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using const_iterator = const T*;

    constexpr T* begin() const noexcept {
        return data_;
    }

    constexpr T* end() const noexcept {
        return data_ + count_;
    }

    constexpr const T* cbegin() const noexcept {
        return data_;
    }

    constexpr const T* cend() const noexcept {
        return data_ + count_;
    }

private:
    T* data_ = nullptr;
    usize count_ = 0;
};

// ---- byte view ----
// 원소 배열을 같은 저장소의 byte view로 본다(UE MakeMemoryView 대응). T는 trivially copyable이어야
// 하며(표현이 byte로 정해지는 타입), 결과의 수명은 원본과 같다. 해시·암호 입력처럼 byte Span을 받는
// API에 구조체 배열을 복사 없이 넘길 때 쓴다.
//
// 사용 예:
//   const uint32 words[4] = {...};
//   hasher.TryUpdate(AsBytes(Span<const uint32>(words)));   // 16 byte
template <typename T>
Span<const uint8> AsBytes(Span<T> values) noexcept {
    static_assert(__is_trivially_copyable(RemoveConstT<T>),
                  "AsBytes requires a trivially copyable element type");
    return Span<const uint8>(reinterpret_cast<const uint8*>(values.Data()), values.ByteCount());
}

// 쓰기 가능한 byte view다. 원소가 const면 컴파일 오류다. 원소의 byte 표현을 직접 채우는 경우(난수로
// 구조체를 채우기 등)에 쓰며, 채운 byte가 T의 올바른 값이 되는지는 호출자가 보장한다.
template <typename T>
Span<uint8> AsWritableBytes(Span<T> values) noexcept {
    static_assert(!__is_const(T), "AsWritableBytes requires a mutable element type");
    static_assert(__is_trivially_copyable(T),
                  "AsWritableBytes requires a trivially copyable element type");
    return Span<uint8>(reinterpret_cast<uint8*>(values.Data()), values.ByteCount());
}

// 두 view의 byte 범위가 한 byte라도 겹치면 true다. 빈 view는 어떤 view와도 겹치지 않는다. 입력과
// 출력이 같은 버퍼를 쓰는지 판정할 때 쓴다(정확히 같은 시작 주소만 허용하는 제자리 규칙은 Data()를
// 비교해 함께 검사한다). 주소는 정수로 비교하므로 서로 다른 할당의 view도 안전하게 판정한다.
template <typename T, typename U>
bool Overlaps(Span<T> a, Span<U> b) noexcept {
    if (a.IsEmpty() || b.IsEmpty()) {
        return false;
    }

    const uptr aBegin = reinterpret_cast<uptr>(a.Data());
    const uptr bBegin = reinterpret_cast<uptr>(b.Data());
    return aBegin < bBegin + b.ByteCount() && bBegin < aBegin + a.ByteCount();
}

} // namespace rocks

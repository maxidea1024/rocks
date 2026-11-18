#pragma once

#include <Base/TypeTraits.h>
#include <Core/Containers/Span.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Platform/Fatal.h>

namespace rocks {

// 원소 저장소를 객체 안에 고정 용량으로 갖는 가변 길이 배열이다(UE TArray<T, TFixedAllocator<N>>
// 대응). heap을 쓰지 않고 Allocator도 받지 않는다. 용량을 넘는 추가는 넘치는 heap 저장으로 바꾸지
// 않고 실패(TryPushBack 계열은 CapacityExceeded, PushBack 계열은 Fatal)한다.
//
// 쓰임: 크기 상한이 증명된 작업 목록(공간 인덱스 검색의 명시적 스택처럼 최대 깊이에서 유도한 용량),
// 힙 할당이 금지된 경로의 소규모 임시 목록. 용량이 크면 객체 자체가 커지므로 스택에 둘 때는 스레드
// 스택 한도를 고려한다(예: 40 byte × 218개 = 약 9 KB).
//
// 계약
// - 원소 [0, Size())만 살아 있다. 나머지 자리는 생성되지 않은 저장소다.
// - 원소 주소는 그 원소를 지우거나(PopBack·Clear·RemoveAtSwap) 배열을 이동·파괴할 때까지 유효하다.
//   재할당이 없으므로 추가로는 무효화되지 않는다. 배열을 이동하면 원소도 새 객체로 옮겨 가므로 옛
//   주소는 쓰지 않는다.
// - 이동·복사는 원소를 하나씩 이동·복사한다(O(Size())). 이동된 원본은 비어 있다.
// - 동시 접근의 동기화는 caller 책임이다. 원소 생성·소멸 중 같은 배열을 다시 바꾸는 재진입은
//   지원하지 않는다.
//
// 사용 예:
//   InlineArray<uint32, 16> pending;
//   if (!pending.TryPushBack(node)) {
//       return Result<void>::Failure(ErrorCode::CapacityExceeded);
//   }
//   while (!pending.IsEmpty()) {
//       const uint32 next = pending.PopBack();
//       ...
//   }
template <typename T, usize CapacityValue>
class InlineArray {
    static_assert(CapacityValue > 0, "InlineArray capacity must be nonzero");
    static_assert(IsNothrowMoveConstructibleV<T>, "InlineArray elements must be nothrow movable");
    static_assert(IsNothrowDestructibleV<T>, "InlineArray elements must be nothrow destructible");

public:
    // 최대 원소 수다. 컴파일 시점 상수이며 바뀌지 않는다.
    static constexpr usize MaxCapacity = CapacityValue;

    constexpr InlineArray() noexcept = default;

    InlineArray(const InlineArray& other) noexcept
        requires IsNothrowCopyConstructibleV<T>
    {
        for (usize index = 0; index < other.size_; ++index) {
            InitializeAt<T>(Slot(index), other[index]);
        }
        size_ = other.size_;
    }

    // 원소를 하나씩 옮긴다. 원본은 비어 있다.
    InlineArray(InlineArray&& other) noexcept {
        for (usize index = 0; index < other.size_; ++index) {
            InitializeAt<T>(Slot(index), Move(other[index]));
        }
        size_ = other.size_;
        other.Clear();
    }

    InlineArray& operator=(const InlineArray& other) noexcept
        requires IsNothrowCopyConstructibleV<T>
    {
        if (this != &other) {
            Clear();
            for (usize index = 0; index < other.size_; ++index) {
                InitializeAt<T>(Slot(index), other[index]);
            }
            size_ = other.size_;
        }
        return *this;
    }

    InlineArray& operator=(InlineArray&& other) noexcept {
        if (this != &other) {
            Clear();
            for (usize index = 0; index < other.size_; ++index) {
                InitializeAt<T>(Slot(index), Move(other[index]));
            }
            size_ = other.size_;
            other.Clear();
        }
        return *this;
    }

    ~InlineArray() noexcept {
        Clear();
    }

    static constexpr usize Capacity() noexcept {
        return CapacityValue;
    }

    usize Size() const noexcept {
        return size_;
    }

    bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    bool IsFull() const noexcept {
        return size_ == CapacityValue;
    }

    // 끝에 원소를 만든다. 가득 차 있으면 CapacityExceeded이며 아무것도 바꾸지 않는다. 반환 포인터는
    // 그 원소를 지우거나 배열을 이동·파괴할 때까지 유효하다.
    template <typename... Args>
    Result<T*> TryEmplaceBack(Args&&... args) noexcept {
        static_assert(IsNothrowConstructibleV<T, Args&&...>);
        if (size_ == CapacityValue) {
            return Result<T*>::Failure(ErrorCode::CapacityExceeded);
        }
        T* value = InitializeAt<T>(Slot(size_), Forward<Args>(args)...);
        ++size_;
        return Result<T*>::Success(value);
    }

    Result<void> TryPushBack(const T& value) noexcept {
        auto pushed = TryEmplaceBack(value);
        return pushed ? Result<void>::Success() : Result<void>::Failure(pushed.Error());
    }

    Result<void> TryPushBack(T&& value) noexcept {
        auto pushed = TryEmplaceBack(Move(value));
        return pushed ? Result<void>::Success() : Result<void>::Failure(pushed.Error());
    }

    // 용량이 충분함이 증명된 경로(최대 깊이에서 유도한 검색 스택 등)의 추가다. 가득 차 있으면 계약
    // 위반이므로 Fatal이다. 실패를 처리해야 하면 TryPushBack을 쓴다.
    template <typename... Args>
    T& EmplaceBack(Args&&... args) noexcept {
        static_assert(IsNothrowConstructibleV<T, Args&&...>);
        if (size_ == CapacityValue) {
            platform::Fatal("InlineArray capacity exceeded");
        }
        T* value = InitializeAt<T>(Slot(size_), Forward<Args>(args)...);
        ++size_;
        return *value;
    }

    void PushBack(const T& value) noexcept {
        EmplaceBack(value);
    }

    void PushBack(T&& value) noexcept {
        EmplaceBack(Move(value));
    }

    // 마지막 원소를 값으로 꺼내고 지운다. 비어 있으면 Fatal이다.
    T PopBack() noexcept {
        if (size_ == 0) {
            platform::Fatal("InlineArray::PopBack on an empty array");
        }
        T* last = Slot(size_ - 1);
        T value(Move(*last));
        if constexpr (!IsTriviallyDestructibleV<T>) {
            DestroyAt(last);
        }
        --size_;
        return value;
    }

    // index 원소를 마지막 원소로 덮고 끝을 줄인다(순서 비보존, O(1)). 범위 밖이면 Fatal이다. 옮겨진
    // 마지막 원소의 주소는 index 자리로 바뀐다.
    void RemoveAtSwap(usize index) noexcept {
        CheckIndex(index);
        T* last = Slot(size_ - 1);
        if (index != size_ - 1) {
            T* target = Slot(index);
            if constexpr (!IsTriviallyDestructibleV<T>) {
                DestroyAt(target);
            }
            InitializeAt<T>(target, Move(*last));
        }
        if constexpr (!IsTriviallyDestructibleV<T>) {
            DestroyAt(last);
        }
        --size_;
    }

    // 원소를 역순으로 소멸시킨다. 저장소는 객체 안에 있으므로 반환할 것이 없다.
    void Clear() noexcept {
        if constexpr (!IsTriviallyDestructibleV<T>) {
            for (usize index = size_; index != 0; --index) {
                DestroyAt(Slot(index - 1));
            }
        }
        size_ = 0;
    }

    // 범위 밖이면 Fatal이다.
    T& operator[](usize index) noexcept {
        CheckIndex(index);
        return *Slot(index);
    }

    const T& operator[](usize index) const noexcept {
        CheckIndex(index);
        return *Slot(index);
    }

    // 마지막 원소다. 비어 있으면 Fatal이다.
    T& Back() noexcept {
        return (*this)[size_ - (size_ == 0 ? 0 : 1)];
    }

    const T& Back() const noexcept {
        return (*this)[size_ - (size_ == 0 ? 0 : 1)];
    }

    T* Data() noexcept {
        return Slot(0);
    }

    const T* Data() const noexcept {
        return Slot(0);
    }

    // 살아 있는 원소 view다. 원소를 지우거나 배열을 이동·파괴하면 무효다.
    Span<T> View() noexcept {
        return Span<T>(Data(), size_);
    }

    Span<const T> View() const noexcept {
        return Span<const T>(Data(), size_);
    }

    // range-for용 반복자(원시 포인터)다. 추가로는 무효화되지 않지만 지우기·이동은 무효화한다.
    T* begin() noexcept {
        return Data();
    }

    T* end() noexcept {
        return Data() + size_;
    }

    const T* begin() const noexcept {
        return Data();
    }

    const T* end() const noexcept {
        return Data() + size_;
    }

private:
    T* Slot(usize index) noexcept {
        return reinterpret_cast<T*>(storage_) + index;
    }

    const T* Slot(usize index) const noexcept {
        return reinterpret_cast<const T*>(storage_) + index;
    }

    void CheckIndex(usize index) const noexcept {
        if (index >= size_) {
            platform::Fatal("InlineArray index out of range");
        }
    }

    alignas(T) unsigned char storage_[sizeof(T) * CapacityValue];
    usize size_ = 0;
};

} // namespace rocks

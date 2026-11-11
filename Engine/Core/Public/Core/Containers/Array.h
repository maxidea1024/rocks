#pragma once

#include <Base/Checked.h>
#include <Core/Algorithms.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>

namespace rocks {

// Allocator를 빌리고 원소·연속 buffer를 소유하는 이동 전용 컨테이너다.
// Allocator는 buffer의 마지막 사용자보다 오래 살아야 하며 동시 접근의 동기화는 caller 책임이다.
// 원소 생성·이동·소멸 중 같은 Array를 다시 변경하는 재진입은 지원하지 않는다.
// StackAllocator처럼 엄격히 역순 반환하는 backing에서는 버퍼 재할당 시 새 버퍼보다
// 오래된 기존 버퍼를 먼저 반환할 수 없어 Fatal이 될 수 있다. 필요한 용량을 미리 예약하고
// 그 용량 안에서 사용한다. 여러 컨테이너가 공유하면 각 버퍼도 생성 역순으로 반환한다.
template <typename T>
class Array {
    static_assert(IsNothrowMoveConstructibleV<T>);
    static_assert(IsNothrowDestructibleV<T>);

public:
    explicit Array(Allocator& allocator) noexcept : allocator_(&allocator) {
    }

    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    // allocator와 buffer 소유권을 함께 이전한다. 원본은 비어 있으며 기존 allocator로 재사용
    // 가능하다.
    Array(Array&& other) noexcept
        : allocator_(other.allocator_), data_(other.data_), size_(other.size_),
          capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    Array& operator=(Array&& other) noexcept {
        if (this != &other) {
            ReleaseStorage();
            allocator_ = other.allocator_;
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }

        return *this;
    }

    ~Array() noexcept {
        ReleaseStorage();
    }

    // 실패 시 데이터·size·capacity·기존 view를 보존한다. 실제 확장에 성공하면 기존 원소 주소와
    // view가 모두 무효화된다. 이미 충분한 capacity이면 원소를 이동하거나 buffer를 교체하지 않는다.
    Result<void> TryReserve(usize capacity) noexcept {
        if (capacity <= capacity_) {
            return Result<void>::Success();
        }

        auto allocation = AllocateElements(capacity);
        if (!allocation) {
            return Result<void>::Failure(allocation.Error());
        }

        T* replacement = allocation.Value();
        RelocateTo(replacement);
        data_ = replacement;
        capacity_ = capacity;
        return Result<void>::Success();
    }

    // 자기 원소를 인자로 넘길 수 있다. 확장 실패는 원본도 보존하며, 성공한 rvalue 추가는
    // 지정 원소를 T의 이동 후 상태로 남길 수 있다.
    // T가 이전 buffer의 주소를 자기 상태에 저장한 경우 그 주소를 자동 보정하지는 않는다.
    Result<void> TryPushBack(const T& value) noexcept {
        auto result = TryEmplaceBack(value);
        return result ? Result<void>::Success() : Result<void>::Failure(result.Error());
    }

    Result<void> TryPushBack(T&& value) noexcept {
        auto result = TryEmplaceBack(Move(value));
        return result ? Result<void>::Success() : Result<void>::Failure(result.Error());
    }

    // 원소 생성도 noexcept여야 한다. 실패 가능한 업무 초기화는 T 생성자 밖에서 처리한다.
    // 반환 pointer의 수명은 해당 원소와 같으며 후속 재할당·Clear·파괴로 무효화된다.
    template <typename... Args>
    Result<T*> TryEmplaceBack(Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...));
        if (size_ == MaxSize / sizeof(T)) {
            return Result<T*>::Failure(ErrorCode::Overflow);
        }

        if (size_ < capacity_) {
            T* value = ConstructAt<T>(data_ + size_, Forward<Args>(args)...);
            ++size_;
            return Result<T*>::Success(value);
        }

        const usize capacity = GrowthCapacity(size_ + 1);
        auto allocation = AllocateElements(capacity);
        if (!allocation) {
            return Result<T*>::Failure(allocation.Error());
        }

        T* replacement = allocation.Value();
        // 새 tail을 기존 buffer가 유효할 때 먼저 생성하여 자기 원소 인자를 안전하게 읽는다.
        // 할당 실패는 이 지점 전에 반환하며, 이후 이동·소멸은 noexcept이므로 rollback이 필요 없다.
        T* value = ConstructAt<T>(replacement + size_, Forward<Args>(args)...);
        RelocateTo(replacement);
        data_ = replacement;
        capacity_ = capacity;
        ++size_;
        return Result<T*>::Success(value);
    }

    // 지정 위치 앞에 하나를 생성한다. size 위치는 끝에 추가한다. 원본 원소를
    // 인자로 전달해도 되며, 확장 실패 시 원본과 모든 원소·주소는 유지된다.
    // 성공한 rvalue 별칭은 원본을 이동 후 상태로 남긴다. 삽입 위치 이후의 참조는
    // 재할당이 없어도 원소 이동으로 무효다.
    template <typename... Args>
    Result<T*> TryEmplaceAt(usize index, Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...));
        if (index > size_) {
            return Result<T*>::Failure(ErrorCode::InvalidArgument);
        }
        if (index == size_) {
            return TryEmplaceBack(Forward<Args>(args)...);
        }
        if (size_ == MaxSize / sizeof(T)) {
            return Result<T*>::Failure(ErrorCode::Overflow);
        }

        if (size_ < capacity_) {
            // 이동으로 입력 별칭이 바뀌기 전에 새 원소를 확보한다.
            T staged(Forward<Args>(args)...);
            for (usize i = size_; i > index; --i) {
                ConstructAt<T>(data_ + i, Move(data_[i - 1]));
                DestroyAt(data_ + i - 1);
            }
            T* inserted = ConstructAt<T>(data_ + index, Move(staged));
            ++size_;
            return Result<T*>::Success(inserted);
        }

        const usize capacity = GrowthCapacity(size_ + 1);
        auto allocation = AllocateElements(capacity);
        if (!allocation) {
            return Result<T*>::Failure(allocation.Error());
        }
        T* replacement = allocation.Value();
        // 원본이 살아 있을 때 새 원소부터 만들어 자기 원소 인자를 보호한다.
        T* inserted = ConstructAt<T>(replacement + index, Forward<Args>(args)...);
        for (usize i = 0; i < index; ++i) {
            ConstructAt<T>(replacement + i, Move(data_[i]));
        }
        for (usize i = index; i < size_; ++i) {
            ConstructAt<T>(replacement + i + 1, Move(data_[i]));
        }
        for (usize i = size_; i != 0; --i) {
            DestroyAt(data_ + i - 1);
        }
        FreeStorage(data_);
        data_ = replacement;
        capacity_ = capacity;
        ++size_;
        return Result<T*>::Success(inserted);
    }

    Result<void> TryInsert(const T& value, usize index) noexcept {
        auto inserted = TryEmplaceAt(index, value);
        return inserted ? Result<void>::Success() : Result<void>::Failure(inserted.Error());
    }

    Result<void> TryInsert(T&& value, usize index) noexcept {
        auto inserted = TryEmplaceAt(index, Move(value));
        return inserted ? Result<void>::Success() : Result<void>::Failure(inserted.Error());
    }

    // 범위의 원소를 복사해 끝에 붙인다. 자기 배열 전체/부분 view도 허용한다.
    // 성장 실패 시 원본·size·capacity·주소를 보존한다. 원소 복사 생성은 noexcept여야 한다.
    Result<void> TryAppend(Span<const T> values) noexcept {
        static_assert(__is_nothrow_constructible(T, const T&));
        if (values.Size() == 0) {
            return Result<void>::Success();
        }
        const usize maximum = MaxSize / sizeof(T);
        if (values.Size() > maximum - size_) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        const usize newSize = size_ + values.Size();
        if (newSize <= capacity_) {
            for (usize i = 0; i < values.Size(); ++i) {
                ConstructAt<T>(data_ + size_ + i, values[i]);
            }
            size_ = newSize;
            return Result<void>::Success();
        }

        const usize capacity = GrowthCapacity(newSize);
        auto allocation = AllocateElements(capacity);
        if (!allocation) {
            return Result<void>::Failure(allocation.Error());
        }
        T* replacement = allocation.Value();
        // source가 자기 배열일 때도 relocation 전에 새 tail을 복사한다.
        for (usize i = 0; i < values.Size(); ++i) {
            ConstructAt<T>(replacement + size_ + i, values[i]);
        }
        RelocateTo(replacement);
        data_ = replacement;
        capacity_ = capacity;
        size_ = newSize;
        return Result<void>::Success();
    }

    Result<void> TryAppend(const Array& values) noexcept {
        return TryAppend(values.View());
    }

    // 다른 배열에서 원소를 이동한다. 성공하면 source는 비지만 capacity는 유지한다.
    // 자기 자신을 rvalue로 전달하면 InvalidArgument이며 실패 시 양쪽을 보존한다.
    Result<void> TryAppend(Array&& values) noexcept {
        if (this == &values) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (values.size_ == 0) {
            return Result<void>::Success();
        }
        const usize maximum = MaxSize / sizeof(T);
        if (values.size_ > maximum - size_) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        const usize newSize = size_ + values.size_;
        if (newSize <= capacity_) {
            for (usize i = 0; i < values.size_; ++i) {
                ConstructAt<T>(data_ + size_ + i, Move(values.data_[i]));
            }
            values.Clear();
            size_ = newSize;
            return Result<void>::Success();
        }

        const usize capacity = GrowthCapacity(newSize);
        auto allocation = AllocateElements(capacity);
        if (!allocation) {
            return Result<void>::Failure(allocation.Error());
        }
        T* replacement = allocation.Value();
        for (usize i = 0; i < values.size_; ++i) {
            ConstructAt<T>(replacement + size_ + i, Move(values.data_[i]));
        }
        RelocateTo(replacement);
        values.Clear();
        data_ = replacement;
        capacity_ = capacity;
        size_ = newSize;
        return Result<void>::Success();
    }

    // 원소 수를 변경한다. 증가한 원소는 기본 생성하며 T의 기본 생성은 noexcept여야 한다.
    // 축소는 뒤에서부터 소멸하고 저장소를 유지한다. 확장 실패는 기존 원소와 주소를 보존한다.
    Result<void> TryResize(usize newSize) noexcept {
        static_assert(__is_nothrow_constructible(T));
        if (newSize > MaxSize / sizeof(T)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        if (newSize <= size_) {
            while (size_ > newSize) {
                --size_;
                DestroyAt(data_ + size_);
            }
            return Result<void>::Success();
        }
        if (newSize <= capacity_) {
            for (usize i = size_; i < newSize; ++i) {
                ConstructAt<T>(data_ + i);
            }
            size_ = newSize;
            return Result<void>::Success();
        }

        const usize capacity = GrowthCapacity(newSize);
        auto allocation = AllocateElements(capacity);
        if (!allocation) {
            return Result<void>::Failure(allocation.Error());
        }
        T* replacement = allocation.Value();
        for (usize i = size_; i < newSize; ++i) {
            ConstructAt<T>(replacement + i);
        }
        RelocateTo(replacement);
        data_ = replacement;
        capacity_ = capacity;
        size_ = newSize;
        return Result<void>::Success();
    }

    // 지정 범위를 제거한다. 범위 밖이면 InvalidArgument이며 변경하지 않는다.
    // count가 0이면 index == Size()도 허용한다. 순서 보존 또는 끝 원소 교체를 선택한다.
    // 살아남은 원소의 참조 중 변경된 위치 이후는 무효다. capacity는 유지한다.
    Result<void> TryRemoveAt(usize index, usize count = 1) noexcept {
        if (index > size_ || count > size_ - index) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (count == 0) {
            return Result<void>::Success();
        }
        for (usize i = index + count; i != index; --i) {
            DestroyAt(data_ + i - 1);
        }
        for (usize i = index + count; i < size_; ++i) {
            ConstructAt<T>(data_ + i - count, Move(data_[i]));
            DestroyAt(data_ + i);
        }
        size_ -= count;
        return Result<void>::Success();
    }

    Result<void> TryRemoveAtSwap(usize index, usize count = 1) noexcept {
        if (index > size_ || count > size_ - index) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (count == 0) {
            return Result<void>::Success();
        }
        const usize trailing = size_ - index - count;
        const usize moved = trailing < count ? trailing : count;
        for (usize i = index + count; i != index; --i) {
            DestroyAt(data_ + i - 1);
        }
        for (usize i = 0; i < moved; ++i) {
            const usize source = size_ - moved + i;
            ConstructAt<T>(data_ + index + i, Move(data_[source]));
            DestroyAt(data_ + source);
        }
        size_ -= count;
        return Result<void>::Success();
    }

    // 마지막 원소의 소유권을 값으로 넘기고 제거한다. 빈 배열은 NotFound다.
    Result<T> TryPopBack() noexcept {
        if (size_ == 0) {
            return Result<T>::Failure(ErrorCode::NotFound);
        }
        T value(Move(data_[size_ - 1]));
        --size_;
        DestroyAt(data_ + size_);
        return Result<T>::Success(Move(value));
    }

    // 일치하는 첫/마지막 위치 또는 NotFound를 반환한다. 비교는 noexcept여야 한다.
    static constexpr usize NotFound = MaxSize;

    // 추가 할당 없이 제자리에서 정렬한다. 동등한 원소의 기존 순서는 보장하지 않는다.
    // 원소의 논리적 정체성은 바뀌며 보관한 참조의 사용에 주의한다. 포인터 원소는 자동으로
    // 역참조하지 않으므로 대상의 값으로 정렬하려면 비교기를 전달한다.
    void Sort() noexcept {
        algo::Sort(View());
    }

    // 비교기는 noexcept 엄격 약순서여야 한다. 정렬 중 배열을 수정하거나 재진입하지 않는다.
    template <typename Compare>
    void Sort(Compare&& compare) noexcept {
        algo::Sort(View(), Forward<Compare>(compare));
    }

    usize Find(const T& value) const noexcept {
        static_assert(noexcept(static_cast<const T&>(data_[0]) == value));
        for (usize i = 0; i < size_; ++i) {
            if (static_cast<const T&>(data_[i]) == value) {
                return i;
            }
        }
        return NotFound;
    }

    usize FindLast(const T& value) const noexcept {
        static_assert(noexcept(static_cast<const T&>(data_[0]) == value));
        for (usize i = size_; i != 0; --i) {
            if (static_cast<const T&>(data_[i - 1]) == value) {
                return i - 1;
            }
        }
        return NotFound;
    }

    bool Contains(const T& value) const noexcept {
        return Find(value) != NotFound;
    }

    bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    usize Size() const noexcept {
        return size_;
    }

    usize Capacity() const noexcept {
        return capacity_;
    }

    T* Data() noexcept {
        return data_;
    }

    const T* Data() const noexcept {
        return data_;
    }

    T& operator[](usize index) noexcept {
        CheckIndex(index);
        return data_[index];
    }

    const T& operator[](usize index) const noexcept {
        CheckIndex(index);
        return data_[index];
    }

    // view는 buffer를 보존하지 않는다. 사용 중 Array를 재할당하거나 원소를 소멸시키면 안 된다.
    Span<T> View() noexcept {
        return Span<T>(data_, size_);
    }

    Span<const T> View() const noexcept {
        return Span<const T>(data_, size_);
    }

    // 원소를 역순 소멸시키고 capacity는 유지한다. 원소 참조는 무효지만 다음 추가에 buffer를
    // 재사용한다.
    void Clear() noexcept {
        for (usize index = size_; index != 0; --index) {
            DestroyAt(data_ + index - 1);
        }

        size_ = 0;
    }

    // Reset은 보관 capacity를 재사용하며 Empty는 저장소까지 반환한다.
    void Reset() noexcept {
        Clear();
    }

    void Empty() noexcept {
        ReleaseStorage();
    }

    // 현재 원소 수에 맞춰 저장소를 줄인다. 실패 시 상태와 주소를 보존한다.
    // 성공한 재할당은 모든 원소 주소·view를 무효화한다.
    Result<void> TryShrink() noexcept {
        if (size_ == capacity_) {
            return Result<void>::Success();
        }
        if (size_ == 0) {
            FreeStorage(data_);
            data_ = nullptr;
            capacity_ = 0;
            return Result<void>::Success();
        }
        auto allocation = AllocateElements(size_);
        if (!allocation) {
            return Result<void>::Failure(allocation.Error());
        }
        T* replacement = allocation.Value();
        RelocateTo(replacement);
        data_ = replacement;
        capacity_ = size_;
        return Result<void>::Success();
    }

private:
    Result<T*> AllocateElements(usize capacity) noexcept {
        usize bytes = 0;
        if (!TryMultiply(capacity, sizeof(T), bytes)) {
            return Result<T*>::Failure(ErrorCode::Overflow);
        }

        auto allocation = allocator_->Allocate(bytes, alignof(T));
        if (!allocation) {
            return Result<T*>::Failure(allocation.Error());
        }

        if (allocation.Value() == nullptr) {
            platform::Fatal("Allocator returned null for a nonzero allocation");
        }

        return Result<T*>::Success(static_cast<T*>(allocation.Value()));
    }

    // 기하급수적으로 확장하되 원소 수×sizeof(T)가 표현 범위를 넘지 않게 상한을 적용한다.
    usize GrowthCapacity(usize required) const noexcept {
        const usize maximum = MaxSize / sizeof(T);
        if (capacity_ == 0) {
            return maximum < 4 || required > 4 ? required : 4;
        }

        const usize growth = capacity_ / 2 + 1;
        const usize candidate = growth > maximum - capacity_ ? maximum : capacity_ + growth;
        return candidate < required ? required : candidate;
    }

    void FreeStorage(T* storage) noexcept {
        if (storage != nullptr && !allocator_->Deallocate(storage)) {
            platform::Fatal("Array storage was not owned by its allocator");
        }
    }

    // byte 복사로 수명을 흉내 내지 않고 각 T를 이동 생성한 뒤 이전 객체를 소멸시킨다.
    // replacement의 충분한 공간과 비중첩은 이 함수를 호출하는 할당 경로가 보장한다.
    void RelocateTo(T* replacement) noexcept {
        for (usize index = 0; index < size_; ++index) {
            ConstructAt<T>(replacement + index, Move(data_[index]));
        }

        for (usize index = size_; index != 0; --index) {
            DestroyAt(data_ + index - 1);
        }

        FreeStorage(data_);
    }

    void ReleaseStorage() noexcept {
        Clear();
        FreeStorage(data_);
        data_ = nullptr;
        capacity_ = 0;
    }

    void CheckIndex(usize index) const noexcept {
        if (index >= size_) {
            platform::Fatal("Array index out of bounds");
        }
    }

    Allocator* allocator_;
    T* data_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
};

} // namespace rocks

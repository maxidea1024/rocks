#pragma once

#include <Base/Checked.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>

namespace rocks {

// Allocator를 빌리고 원소·연속 buffer를 소유하는 이동 전용 컨테이너다.
// Allocator는 buffer의 마지막 사용자보다 오래 살아야 하며 동시 접근의 동기화는 caller 책임이다.
// 원소 생성·이동·소멸 중 같은 Array를 다시 변경하는 재진입은 지원하지 않는다.
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
            return maximum < 4 ? required : 4;
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

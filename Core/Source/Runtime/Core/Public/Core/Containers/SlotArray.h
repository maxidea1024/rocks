#pragma once

#include <Base/Checked.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

// SlotArray 원소를 가리키는 소유권 없는 값 식별자다. generation 0은 비어 있는 handle이다.
// 다른 SlotArray의 handle과 구별하지 않으므로 handle은 발급한 배열에만 사용한다.
struct SlotHandle {
    uint32 index = 0;
    uint32 generation = 0;

    bool IsNull() const noexcept {
        return generation == 0;
    }

    bool operator==(const SlotHandle&) const noexcept = default;
};

// 안정적인 slot과 generation handle을 제공하는 배열이다. 원소를 지우면 slot의 generation이 올라가
// 이전 handle은 stale이 되며, slot은 다음 추가에 재사용된다(가장 최근에 지운 slot부터).
// - generation은 1부터 시작한다. maxGeneration에 도달한 slot은 다시 0이나 과거 값으로 순환하지 않고
//   폐기(retired)되어 재사용하지 않는다(ObjectRegistry와 같은 규칙). 오래된 handle이 새 원소를
//   가리키는 일은 없다.
// - Clear는 원소만 지우고 slot과 generation을 유지한다. 저장소는 소멸 시에만 반환한다(generation
//   기록을 버리면 이전 handle이 다시 유효해질 수 있다).
// - handle은 발급한 배열 객체의 generation 기록 안에서만 의미가 있다. 이동하면 기록이 대상으로
//   옮겨 가고, 이동된 원본이나 새로 만든 배열(이동 대입 대상 포함)은 generation 1부터 다시
//   발급한다. 그 뒤 이전 기록의 handle을 쓰면 새 원소를 가리킬 수 있으며 이를 검출하지
//   않는다(caller 책임). 저장소를 줄이려고 새 배열로 교체할 때는 이전 handle을 모두 버린다.
// - 원소 주소는 용량 확장에서 바뀔 수 있다. 오래 보관할 때는 handle을 보관한다.
// - 반복은 사용 중인 slot의 오름차순이다. 반복 중 원소를 지워도 되며, 추가한 원소는 현재보다 큰
//   slot일 때만 방문한다. 반복자는 방문한 원소의 generation을 기억하므로 그 원소가 지워지면(같은
//   slot이 다른 원소로 재사용돼도) Handle()은 빈 handle이고 역참조는 Fatal이다.
// - 원소 생성·이동·소멸 중 같은 SlotArray를 다시 변경하는 재진입은 지원하지 않는다. 동시 접근의
//   동기화는 caller 책임이다.
template <typename T>
class SlotArray {
    static_assert(IsNothrowMoveConstructibleV<T>);
    static_assert(IsNothrowDestructibleV<T>);

    static constexpr uint32 EndOfList = 0xffffffffu;
    static constexpr uint32 Occupied = 0xfffffffeu;
    static constexpr uint32 Retired = 0xfffffffdu;

    // next가 Occupied면 value가 살아 있다. 그 밖에는 free list의 다음 slot(EndOfList 포함) 또는
    // Retired다. generation은 살아 있으면 현재 세대, 비어 있으면 다음 원소가 받을 세대다.
    struct Slot {
        union {
            T value;
        };
        uint32 generation;
        uint32 next;

        Slot() noexcept {
        }
        ~Slot() noexcept {
        }
    };

public:
    static constexpr usize MaxSlots = Retired;
    static constexpr uint32 DefaultMaxGeneration = 0xffffffffu;

    // maxGeneration은 slot 하나가 받을 수 있는 최대 generation이며 0이면 Fatal이다. 낮추면 slot
    // 폐기 경로를 시험할 수 있다.
    explicit SlotArray(Allocator& allocator, uint32 maxGeneration = DefaultMaxGeneration) noexcept
        : allocator_(&allocator), maxGeneration_(maxGeneration) {
        if (maxGeneration == 0) {
            platform::Fatal("SlotArray maxGeneration must be nonzero");
        }
    }

    SlotArray(const SlotArray&) = delete;
    SlotArray& operator=(const SlotArray&) = delete;

    // 저장소와 generation 기록을 넘긴다. 원본은 비며 같은 allocator로 다시 쓸 수 있다.
    SlotArray(SlotArray&& other) noexcept
        : allocator_(other.allocator_), slots_(other.slots_), slotCount_(other.slotCount_),
          capacity_(other.capacity_), size_(other.size_), retired_(other.retired_),
          firstFree_(other.firstFree_), maxGeneration_(other.maxGeneration_) {
        other.ForgetStorage();
    }

    SlotArray& operator=(SlotArray&& other) noexcept {
        if (this != &other) {
            Release();
            allocator_ = other.allocator_;
            slots_ = other.slots_;
            slotCount_ = other.slotCount_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            retired_ = other.retired_;
            firstFree_ = other.firstFree_;
            maxGeneration_ = other.maxGeneration_;
            other.ForgetStorage();
        }
        return *this;
    }

    ~SlotArray() noexcept {
        Release();
    }

    // 살아 있는 원소 수다.
    usize Size() const noexcept {
        return size_;
    }

    // 지금까지 만든 slot 수(사용 중·빈·폐기 포함)다.
    usize SlotCount() const noexcept {
        return slotCount_;
    }

    // generation 한도에 도달해 다시 쓰지 않는 slot 수다.
    usize RetiredCount() const noexcept {
        return retired_;
    }

    usize Capacity() const noexcept {
        return capacity_;
    }

    bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    uint32 MaxGeneration() const noexcept {
        return maxGeneration_;
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    // handle이 지금 살아 있는 원소를 가리키는지다. 비어 있는·범위 밖·stale handle은 false다.
    bool IsValid(SlotHandle handle) const noexcept {
        return handle.generation != 0 && handle.index < slotCount_ &&
               slots_[handle.index].next == Occupied &&
               slots_[handle.index].generation == handle.generation;
    }

    // 살아 있으면 원소 주소, 아니면 nullptr다. 주소는 용량 확장이나 원소 삭제로 무효가 된다.
    T* Find(SlotHandle handle) noexcept {
        return IsValid(handle) ? &slots_[handle.index].value : nullptr;
    }

    const T* Find(SlotHandle handle) const noexcept {
        return IsValid(handle) ? &slots_[handle.index].value : nullptr;
    }

    // 살아 있지 않은 handle은 Fatal이다. 오류 복구가 필요하면 Find·IsValid를 쓴다.
    T& operator[](SlotHandle handle) noexcept {
        CheckHandle(handle);
        return slots_[handle.index].value;
    }

    const T& operator[](SlotHandle handle) const noexcept {
        CheckHandle(handle);
        return slots_[handle.index].value;
    }

    // 원소를 만들고 handle을 반환한다. 인자가 이 배열의 원소를 가리켜도 된다. 모든 slot이 폐기되어
    // 더 만들 수 없으면 CapacityExceeded다. 실패하면 상태를 보존한다.
    Result<SlotHandle> TryAdd(const T& value) noexcept {
        return TryEmplace(value);
    }

    Result<SlotHandle> TryAdd(T&& value) noexcept {
        return TryEmplace(Move(value));
    }

    template <typename... Args>
    Result<SlotHandle> TryEmplace(Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...));
        if (firstFree_ != EndOfList) {
            const uint32 index = firstFree_;
            Slot& slot = slots_[index];
            const uint32 next = slot.next;
            ConstructAt<T>(&slot.value, Forward<Args>(args)...);
            slot.next = Occupied;
            firstFree_ = next;
            ++size_;
            return Result<SlotHandle>::Success(SlotHandle{index, slot.generation});
        }
        if (slotCount_ == MaxSlots) {
            return Result<SlotHandle>::Failure(ErrorCode::CapacityExceeded);
        }
        Slot* target = slots_;
        Slot* replacement = nullptr;
        usize capacity = capacity_;
        if (slotCount_ == capacity_) {
            capacity = GrowthCapacity(slotCount_ + 1);
            auto allocation = AllocateSlots(capacity);
            if (!allocation) {
                return Result<SlotHandle>::Failure(allocation.Error());
            }
            replacement = allocation.Value();
            target = replacement;
        }
        // 기존 저장소가 아직 유효할 때 먼저 생성하므로 인자가 기존 원소를 가리켜도 된다.
        const uint32 index = static_cast<uint32>(slotCount_);
        Slot& slot = target[index];
        ConstructAt<T>(&slot.value, Forward<Args>(args)...);
        slot.generation = 1;
        slot.next = Occupied;
        if (replacement != nullptr) {
            RelocateTo(replacement);
            slots_ = replacement;
            capacity_ = capacity;
        }
        ++slotCount_;
        ++size_;
        return Result<SlotHandle>::Success(SlotHandle{index, 1});
    }

    // 원소를 소멸하고 slot을 비운다. 비어 있는 handle은 InvalidArgument, 그 밖에 살아 있지 않은
    // handle은 StaleHandle이며 상태를 바꾸지 않는다.
    Result<void> TryRemove(SlotHandle handle) noexcept {
        auto checked = CheckRemovable(handle);
        if (!checked) {
            return checked;
        }
        DestroyAt(&slots_[handle.index].value);
        FreeSlot(handle.index);
        return Result<void>::Success();
    }

    // 원소를 꺼내 값으로 돌려주고 slot을 비운다. 오류는 TryRemove와 같다.
    Result<T> TryTake(SlotHandle handle) noexcept {
        auto checked = CheckRemovable(handle);
        if (!checked) {
            return Result<T>::Failure(checked.Error());
        }
        Slot& slot = slots_[handle.index];
        auto taken = Result<T>::Success(Move(slot.value));
        DestroyAt(&slot.value);
        FreeSlot(handle.index);
        return taken;
    }

    // slot 용량을 확보한다. 실패하면 상태를 보존하고, 실제로 늘면 원소 주소가 바뀐다.
    Result<void> TryReserve(usize capacity) noexcept {
        if (capacity <= capacity_) {
            return Result<void>::Success();
        }
        if (capacity > Limit()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        auto allocation = AllocateSlots(capacity);
        if (!allocation) {
            return Result<void>::Failure(allocation.Error());
        }
        RelocateTo(allocation.Value());
        slots_ = allocation.Value();
        capacity_ = capacity;
        return Result<void>::Success();
    }

    // 살아 있는 원소를 slot 역순으로 지운다. 지운 slot은 TryRemove와 같이 generation이 올라가거나
    // 폐기되므로 이전 handle은 모두 stale이 된다. 저장소는 유지한다.
    void Clear() noexcept {
        for (usize index = slotCount_; index != 0; --index) {
            if (slots_[index - 1].next == Occupied) {
                DestroyAt(&slots_[index - 1].value);
                FreeSlot(static_cast<uint32>(index - 1));
            }
        }
    }

    // ---- 반복자 ----
    // 살아 있는 원소를 slot 오름차순으로 방문한다. Handle()로 현재 원소의 handle을 얻는다.
    template <bool Constant>
    class IteratorBase {
        using Owner = ConditionalT<Constant, const SlotArray, SlotArray>;
        using Reference = ConditionalT<Constant, const T&, T&>;

    public:
        // STL 호환 중첩 타입이다(std 태그 없이 forward iterator로 쓰인다).
        using value_type = T;
        using difference_type = isize;
        using reference = Reference;
        using pointer = ConditionalT<Constant, const T*, T*>;

        IteratorBase() noexcept = default;
        IteratorBase(Owner* owner, usize index) noexcept : owner_(owner), index_(index) {
            Capture();
        }

        Reference operator*() const noexcept {
            return (*owner_)[Handle()];
        }

        pointer operator->() const noexcept {
            return &(*owner_)[Handle()];
        }

        // 방문한 원소의 handle이다. 그 원소가 지워졌으면(같은 slot이 다른 원소로 재사용된 경우
        // 포함) 비어 있는 handle이다.
        SlotHandle Handle() const noexcept {
            const SlotHandle visited{static_cast<uint32>(index_), generation_};
            return owner_ != nullptr && index_ < owner_->slotCount_ && owner_->IsValid(visited)
                       ? visited
                       : SlotHandle{};
        }

        IteratorBase& operator++() noexcept {
            index_ = owner_->NextOccupied(index_ + 1);
            Capture();
            return *this;
        }

        IteratorBase operator++(int) noexcept {
            IteratorBase previous = *this;
            ++*this;
            return previous;
        }

        friend bool operator==(const IteratorBase& left, const IteratorBase& right) noexcept {
            return left.index_ == right.index_;
        }

    private:
        // 방문한 slot의 generation을 기억해 재사용된 slot을 구별한다.
        void Capture() noexcept {
            generation_ = owner_ != nullptr && index_ < owner_->slotCount_
                              ? owner_->slots_[index_].generation
                              : 0;
        }

        Owner* owner_ = nullptr;
        usize index_ = MaxSize;
        uint32 generation_ = 0;
    };

    using Iterator = IteratorBase<false>;
    using ConstIterator = IteratorBase<true>;
    using value_type = T;
    using size_type = usize;
    using difference_type = isize;
    using iterator = Iterator;
    using const_iterator = ConstIterator;

    Iterator begin() noexcept {
        return Iterator(this, NextOccupied(0));
    }
    Iterator end() noexcept {
        return Iterator(this, MaxSize);
    }
    ConstIterator begin() const noexcept {
        return ConstIterator(this, NextOccupied(0));
    }
    ConstIterator end() const noexcept {
        return ConstIterator(this, MaxSize);
    }
    ConstIterator cbegin() const noexcept {
        return begin();
    }
    ConstIterator cend() const noexcept {
        return end();
    }

private:
    Result<void> CheckRemovable(SlotHandle handle) const noexcept {
        if (handle.IsNull()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (!IsValid(handle)) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        return Result<void>::Success();
    }

    // 값이 소멸된 slot의 generation을 올려 free list 앞에 넣거나, 한도에 도달했으면 폐기한다.
    void FreeSlot(uint32 index) noexcept {
        Slot& slot = slots_[index];
        --size_;
        if (slot.generation >= maxGeneration_) {
            slot.next = Retired;
            ++retired_;
            return;
        }
        ++slot.generation;
        slot.next = firstFree_;
        firstFree_ = index;
    }

    Result<Slot*> AllocateSlots(usize capacity) noexcept {
        usize bytes = 0;
        if (!TryMultiply(capacity, sizeof(Slot), bytes)) {
            return Result<Slot*>::Failure(ErrorCode::Overflow);
        }
        auto allocation = allocator_->Allocate(bytes, alignof(Slot));
        if (!allocation) {
            return Result<Slot*>::Failure(allocation.Error());
        }
        if (allocation.Value() == nullptr) {
            platform::Fatal("Allocator returned null for a nonzero allocation");
        }
        // Slot은 사용자 정의 생성자가 있어 암묵적으로 생기지 않으므로 수명을 명시적으로 시작한다.
        // value는 생성하지 않는다.
        auto* slots = static_cast<Slot*>(allocation.Value());
        for (usize index = 0; index < capacity; ++index) {
            ConstructAt<Slot>(slots + index);
        }
        return Result<Slot*>::Success(slots);
    }

    void FreeStorage(Slot* slots) noexcept {
        if (slots != nullptr && !allocator_->Deallocate(slots)) {
            platform::Fatal("SlotArray storage was not owned by its allocator");
        }
    }

    // 살아 있는 원소는 이동 생성 후 소멸하고 generation·link는 복사한다. 이전 저장소를 반환한다.
    void RelocateTo(Slot* replacement) noexcept {
        for (usize index = 0; index < slotCount_; ++index) {
            Slot& source = slots_[index];
            Slot& target = replacement[index];
            if (source.next == Occupied) {
                ConstructAt<T>(&target.value, Move(source.value));
                DestroyAt(&source.value);
            }
            target.generation = source.generation;
            target.next = source.next;
        }
        FreeStorage(slots_);
    }

    usize NextOccupied(usize start) const noexcept {
        for (usize index = start; index < slotCount_; ++index) {
            if (slots_[index].next == Occupied) {
                return index;
            }
        }
        return MaxSize;
    }

    static constexpr usize Limit() noexcept {
        return MaxSize / sizeof(Slot) < MaxSlots ? MaxSize / sizeof(Slot) : MaxSlots;
    }

    usize GrowthCapacity(usize required) const noexcept {
        const usize maximum = Limit();
        if (capacity_ == 0) {
            return maximum < 4 || required > 4 ? required : 4;
        }
        const usize growth = capacity_ / 2 + 1;
        const usize candidate = growth > maximum - capacity_ ? maximum : capacity_ + growth;
        return candidate < required ? required : candidate;
    }

    void CheckHandle(SlotHandle handle) const noexcept {
        if (!IsValid(handle)) {
            platform::Fatal("SlotArray handle is stale or invalid");
        }
    }

    void Release() noexcept {
        for (usize index = slotCount_; index != 0; --index) {
            if (slots_[index - 1].next == Occupied) {
                DestroyAt(&slots_[index - 1].value);
            }
        }
        FreeStorage(slots_);
        ForgetStorage();
    }

    void ForgetStorage() noexcept {
        slots_ = nullptr;
        slotCount_ = 0;
        capacity_ = 0;
        size_ = 0;
        retired_ = 0;
        firstFree_ = EndOfList;
    }

    Allocator* allocator_;
    Slot* slots_ = nullptr;
    usize slotCount_ = 0;
    usize capacity_ = 0;
    usize size_ = 0;
    usize retired_ = 0;
    uint32 firstFree_ = EndOfList;
    uint32 maxGeneration_;
};

} // namespace rocks

#pragma once

#include <Base/Checked.h>
#include <Core/Containers/BitArray.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

// 원소를 지워도 다른 원소의 index가 바뀌지 않는 배열이다(UE5.6 TSparseArray 대응). 지운 자리는
// free list에 들어가 다음 추가에 재사용되며, 사용 여부는 BitArray로 표시한다.
// - index는 Compact·CompactStable 전까지 안정적이다. 원소 주소는 용량 확장(Add·Emplace·Reserve·
//   Shrink)에서 바뀔 수 있다.
// - 추가는 가장 최근에 지운 자리부터 재사용한다(LIFO). 가장 낮은 빈 index가 필요하면
//   TryEmplaceAtLowestFreeIndex를 쓴다. 빈 자리가 없으면 MaxIndex() 위치에 붙인다.
// - 반복은 사용 중인 index의 오름차순이다. 반복 중 원소를 지워도 되며(다음 원소로 진행한다), 반복
//   중 추가한 원소는 현재 위치보다 큰 index일 때만 방문한다. 반복자는 index만 기억하므로 가리키는
//   index가 빈 slot이면 역참조는 Fatal이지만, 그 index가 다른 원소로 재사용되면 새 원소를 가리킨다
//   (원소 identity는 index다).
// - 원소 생성·이동·소멸 중 같은 SparseArray를 다시 변경하는 재진입은 지원하지 않는다. 동시
//   접근의 동기화는 caller 책임이다.
// - index는 uint32 free list에 들어가므로 MaxIndex()는 MaxSlots를 넘지 않는다(Overflow).
// - 원소 저장소와 사용 표시 bit는 같은 allocator에서 따로 할당한다. StackAllocator처럼 역순
//   반환이 필요한 allocator는 TryReserve로 필요한 용량을 먼저 확보한다.
template <typename T>
class SparseArray {
    static_assert(IsNothrowMoveConstructibleV<T>);
    static_assert(IsNothrowDestructibleV<T>);

    static constexpr uint32 NoIndex = 0xffffffffu;

    struct FreeLink {
        uint32 previous;
        uint32 next;
    };

    // 사용 중이면 value, 비어 있으면 free list link가 살아 있다.
    union Node {
        T value;
        FreeLink link;

        Node() noexcept {
        }
        ~Node() noexcept {
        }
    };

public:
    static constexpr usize MaxSlots = 0xfffffffeu;
    static constexpr usize NotFound = MaxSize;

    explicit SparseArray(Allocator& allocator) noexcept
        : allocator_(&allocator), allocated_(allocator) {
    }

    SparseArray(const SparseArray&) = delete;
    SparseArray& operator=(const SparseArray&) = delete;

    // 저장소 소유권을 넘긴다. 원본은 비며 같은 allocator로 다시 쓸 수 있다.
    SparseArray(SparseArray&& other) noexcept
        : allocator_(other.allocator_), nodes_(other.nodes_), allocated_(Move(other.allocated_)),
          maxIndex_(other.maxIndex_), capacity_(other.capacity_), freeCount_(other.freeCount_),
          firstFree_(other.firstFree_) {
        other.ForgetStorage();
    }

    SparseArray& operator=(SparseArray&& other) noexcept {
        if (this != &other) {
            Release();
            allocator_ = other.allocator_;
            nodes_ = other.nodes_;
            allocated_ = Move(other.allocated_);
            maxIndex_ = other.maxIndex_;
            capacity_ = other.capacity_;
            freeCount_ = other.freeCount_;
            firstFree_ = other.firstFree_;
            other.ForgetStorage();
        }
        return *this;
    }

    ~SparseArray() noexcept {
        Release();
    }

    // 사용 중인 원소 수다(UE Num).
    usize Size() const noexcept {
        return maxIndex_ - freeCount_;
    }

    // 사용 중이거나 비어 있는 slot을 합한 index 상한이다(UE GetMaxIndex). 유효 index는 이보다 작다.
    usize MaxIndex() const noexcept {
        return maxIndex_;
    }

    usize Capacity() const noexcept {
        return capacity_;
    }

    bool IsEmpty() const noexcept {
        return Size() == 0;
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    // 범위 안이며 사용 중인 index인지다. 범위 밖도 false다.
    bool IsValidIndex(usize index) const noexcept {
        return index < maxIndex_ && allocated_[index];
    }

    // 빈 slot이나 범위 밖 index는 Fatal이다. 오류 복구가 필요하면 IsValidIndex로 먼저 확인한다.
    T& operator[](usize index) noexcept {
        CheckIndex(index);
        return nodes_[index].value;
    }

    const T& operator[](usize index) const noexcept {
        CheckIndex(index);
        return nodes_[index].value;
    }

    // 추가한 원소의 index를 반환한다. 인자가 이 배열의 원소를 가리켜도 된다. 실패하면 원소·index·
    // free list·용량을 보존한다.
    Result<usize> TryAdd(const T& value) noexcept {
        return TryEmplace(value);
    }

    Result<usize> TryAdd(T&& value) noexcept {
        return TryEmplace(Move(value));
    }

    template <typename... Args>
    Result<usize> TryEmplace(Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...));
        if (firstFree_ != NoIndex) {
            const usize index = firstFree_;
            Unlink(index);
            ConstructAt<T>(&nodes_[index].value, Forward<Args>(args)...);
            allocated_[index] = true;
            return Result<usize>::Success(index);
        }
        return EmplaceBeyond(maxIndex_, Forward<Args>(args)...);
    }

    // 가장 낮은 빈 index에 생성한다(UE EmplaceAtLowestFreeIndex). 빈 자리가 없으면 끝에 붙인다.
    // 매번 사용 표시 bit를 처음부터 찾으므로 O(MaxIndex()/64)다(UE의 검색 시작 hint는 없다).
    template <typename... Args>
    Result<usize> TryEmplaceAtLowestFreeIndex(Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...));
        if (freeCount_ == 0) {
            return EmplaceBeyond(maxIndex_, Forward<Args>(args)...);
        }
        const usize index = allocated_.Find(false);
        Unlink(index);
        ConstructAt<T>(&nodes_[index].value, Forward<Args>(args)...);
        allocated_[index] = true;
        return Result<usize>::Success(index);
    }

    // 지정 index에 생성한다(UE EmplaceAt). 사용 중이면 AlreadyExists, MaxIndex() 이상이면 그 사이를
    // 빈 slot으로 늘린다. 늘어난 빈 slot은 낮은 index부터 재사용된다(UE 구현과 순서가 다를 수
    // 있으므로 순서에 의존하지 않는다). 실패하면 상태를 보존한다.
    template <typename... Args>
    Result<usize> TryEmplaceAt(usize index, Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...));
        if (index >= maxIndex_) {
            return EmplaceBeyond(index, Forward<Args>(args)...);
        }
        if (allocated_[index]) {
            return Result<usize>::Failure(ErrorCode::AlreadyExists);
        }
        Unlink(index);
        ConstructAt<T>(&nodes_[index].value, Forward<Args>(args)...);
        allocated_[index] = true;
        return Result<usize>::Success(index);
    }

    // 원소를 소멸하고 slot을 free list 앞에 넣는다. MaxIndex()와 다른 원소의 index는 바뀌지 않는다.
    // 범위 밖은 InvalidArgument, 빈 slot은 NotFound이며 상태를 바꾸지 않는다.
    Result<void> TryRemoveAt(usize index) noexcept {
        if (index >= maxIndex_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (!allocated_[index]) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }
        DestroyAt(&nodes_[index].value);
        allocated_[index] = false;
        PushFree(index);
        return Result<void>::Success();
    }

    // slot 용량을 확보한다. MaxIndex()는 바뀌지 않는다. 실패하면 상태를 보존하고, 실제로 늘면 원소
    // 주소가 바뀐다.
    Result<void> TryReserve(usize capacity) noexcept {
        if (capacity <= capacity_) {
            return Result<void>::Success();
        }
        if (capacity > Limit()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        auto replacement = PrepareStorage(capacity);
        if (!replacement) {
            return Result<void>::Failure(replacement.Error());
        }
        AdoptStorage(replacement.Value(), capacity);
        return Result<void>::Success();
    }

    // 끝의 빈 slot을 없애고 저장소를 MaxIndex()에 맞춘다(UE Shrink). 사이의 빈 slot은 남는다.
    // 재할당이 실패하면 상태를 보존한다. 성공한 재할당은 원소 주소를 바꾼다.
    Result<void> TryShrink() noexcept {
        usize end = maxIndex_;
        while (end != 0 && !allocated_[end - 1]) {
            --end;
        }
        if (end < capacity_) {
            Node* replacement = nullptr;
            if (end != 0) {
                auto allocation = AllocateNodes(end);
                if (!allocation) {
                    return Result<void>::Failure(allocation.Error());
                }
                replacement = allocation.Value();
            }
            TrimTail(end);
            if (replacement != nullptr) {
                RelocateTo(replacement);
            } else {
                FreeNodes(nodes_);
            }
            nodes_ = replacement;
            capacity_ = end;
        } else {
            TrimTail(end);
        }
        return Result<void>::Success();
    }

    // 끝 쪽 원소를 앞의 빈 자리로 옮겨 빈 slot을 없앤다(UE Compact). 순서는 보존하지 않으며 옮긴
    // 원소의 index가 바뀐다. 할당하지 않는다. 원소를 하나라도 옮겼으면 true다.
    bool Compact() noexcept {
        const usize size = Size();
        bool moved = false;
        usize last = maxIndex_;
        for (usize hole = allocated_.Find(false); hole != BitArray::NotFound && hole < size;
             hole = allocated_.Find(false, hole + 1)) {
            do {
                --last;
            } while (!allocated_[last]);
            ConstructAt<T>(&nodes_[hole].value, Move(nodes_[last].value));
            DestroyAt(&nodes_[last].value);
            allocated_[hole] = true;
            allocated_[last] = false;
            moved = true;
        }
        FinishCompaction(size);
        return moved;
    }

    // 원소의 상대 순서를 유지하며 앞으로 당긴다(UE CompactStable). 옮긴 원소의 index가 바뀐다.
    // 할당하지 않는다. 원소를 하나라도 옮겼으면 true다.
    bool CompactStable() noexcept {
        const usize size = Size();
        bool moved = false;
        usize write = 0;
        for (usize read = 0; read < maxIndex_; ++read) {
            if (!allocated_[read]) {
                continue;
            }
            if (read != write) {
                ConstructAt<T>(&nodes_[write].value, Move(nodes_[read].value));
                DestroyAt(&nodes_[read].value);
                allocated_[write] = true;
                allocated_[read] = false;
                moved = true;
            }
            ++write;
        }
        FinishCompaction(size);
        return moved;
    }

    // 원소를 index 역순으로 소멸하고 용량은 유지한다. 모든 index가 무효가 된다.
    void Clear() noexcept {
        for (usize index = maxIndex_; index != 0; --index) {
            if (allocated_[index - 1]) {
                DestroyAt(&nodes_[index - 1].value);
            }
        }
        allocated_.Clear();
        maxIndex_ = 0;
        freeCount_ = 0;
        firstFree_ = NoIndex;
    }

    // Reset은 용량을 유지하고 Empty는 저장소까지 반환한다.
    void Reset() noexcept {
        Clear();
    }

    void Empty() noexcept {
        Release();
        ForgetStorage();
    }

    // ---- 반복자 ----
    // 사용 중인 index를 오름차순으로 방문한다. Index()로 현재 index를 얻는다. 반복자는 index만
    // 기억하므로 용량 확장 뒤에도 쓸 수 있다. 가리키는 index가 빈 slot이면 역참조는 Fatal이다.
    template <bool Constant>
    class IteratorBase {
        using Owner = ConditionalT<Constant, const SparseArray, SparseArray>;
        using Reference = ConditionalT<Constant, const T&, T&>;

    public:
        // STL 호환 중첩 타입이다(std 태그 없이 forward iterator로 쓰인다).
        using value_type = T;
        using difference_type = isize;
        using reference = Reference;
        using pointer = ConditionalT<Constant, const T*, T*>;

        IteratorBase() noexcept = default;
        IteratorBase(Owner* owner, usize index) noexcept : owner_(owner), index_(index) {
        }

        Reference operator*() const noexcept {
            return (*owner_)[index_];
        }

        pointer operator->() const noexcept {
            return &(*owner_)[index_];
        }

        usize Index() const noexcept {
            return index_;
        }

        IteratorBase& operator++() noexcept {
            index_ = owner_->NextAllocated(index_ + 1);
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
        Owner* owner_ = nullptr;
        usize index_ = NotFound;
    };

    using Iterator = IteratorBase<false>;
    using ConstIterator = IteratorBase<true>;
    using value_type = T;
    using size_type = usize;
    using difference_type = isize;
    using iterator = Iterator;
    using const_iterator = ConstIterator;

    Iterator begin() noexcept {
        return Iterator(this, NextAllocated(0));
    }
    Iterator end() noexcept {
        return Iterator(this, NotFound);
    }
    ConstIterator begin() const noexcept {
        return ConstIterator(this, NextAllocated(0));
    }
    ConstIterator end() const noexcept {
        return ConstIterator(this, NotFound);
    }
    ConstIterator cbegin() const noexcept {
        return begin();
    }
    ConstIterator cend() const noexcept {
        return end();
    }

private:
    // index 위치(>= MaxIndex())에 생성하고 사이를 빈 slot으로 채운다.
    template <typename... Args>
    Result<usize> EmplaceBeyond(usize index, Args&&... args) noexcept {
        if (index >= Limit()) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }
        const usize required = index + 1;
        Node* target = nodes_;
        Node* replacement = nullptr;
        usize capacity = capacity_;
        if (required > capacity_) {
            capacity = GrowthCapacity(required);
            auto prepared = PrepareStorage(capacity);
            if (!prepared) {
                return Result<usize>::Failure(prepared.Error());
            }
            replacement = prepared.Value();
            target = replacement;
        }
        // 기존 저장소가 아직 유효할 때 먼저 생성하므로 인자가 기존 원소를 가리켜도 된다. 이후
        // 단계는 실패하지 않는다.
        ConstructAt<T>(&target[index].value, Forward<Args>(args)...);
        if (replacement != nullptr) {
            AdoptStorage(replacement, capacity);
        }
        // 높은 index부터 넣어 낮은 index가 먼저 재사용되게 한다.
        for (usize slot = index; slot > maxIndex_; --slot) {
            PushFree(slot - 1);
        }
        AppendFlags(false, index - maxIndex_);
        AppendFlags(true, 1);
        maxIndex_ = required;
        return Result<usize>::Success(index);
    }

    // 사용 표시 bit의 용량을 먼저 늘린 뒤 새 원소 저장소를 할당한다. 둘째가 실패해도 bit 용량만
    // 커지므로 상태는 보존된다. 불변식: allocated_.Capacity() >= capacity_.
    Result<Node*> PrepareStorage(usize capacity) noexcept {
        auto reserved = allocated_.TryReserve(capacity);
        if (!reserved) {
            return Result<Node*>::Failure(reserved.Error());
        }
        return AllocateNodes(capacity);
    }

    void AdoptStorage(Node* replacement, usize capacity) noexcept {
        RelocateTo(replacement);
        nodes_ = replacement;
        capacity_ = capacity;
    }

    Result<Node*> AllocateNodes(usize capacity) noexcept {
        usize bytes = 0;
        if (!TryMultiply(capacity, sizeof(Node), bytes)) {
            return Result<Node*>::Failure(ErrorCode::Overflow);
        }
        auto allocation = allocator_->Allocate(bytes, alignof(Node));
        if (!allocation) {
            return Result<Node*>::Failure(allocation.Error());
        }
        if (allocation.Value() == nullptr) {
            platform::Fatal("Allocator returned null for a nonzero allocation");
        }
        // Node는 사용자 정의 생성자가 있어 암묵적으로 생기지 않으므로 수명을 명시적으로 시작한다.
        // 활성 멤버(value·link)는 사용할 때 정한다.
        auto* nodes = static_cast<Node*>(allocation.Value());
        for (usize index = 0; index < capacity; ++index) {
            ConstructAt<Node>(nodes + index);
        }
        return Result<Node*>::Success(nodes);
    }

    void FreeNodes(Node* nodes) noexcept {
        if (nodes != nullptr && !allocator_->Deallocate(nodes)) {
            platform::Fatal("SparseArray storage was not owned by its allocator");
        }
    }

    // 사용 중 원소는 이동 생성 후 소멸하고 빈 slot은 link를 복사한다. 이전 저장소를 반환한다.
    void RelocateTo(Node* replacement) noexcept {
        for (usize index = 0; index < maxIndex_; ++index) {
            if (allocated_[index]) {
                ConstructAt<T>(&replacement[index].value, Move(nodes_[index].value));
                DestroyAt(&nodes_[index].value);
            } else {
                replacement[index].link = nodes_[index].link;
            }
        }
        FreeNodes(nodes_);
    }

    // 끝의 빈 slot [end, maxIndex_)를 free list에서 빼고 MaxIndex()를 줄인다.
    void TrimTail(usize end) noexcept {
        for (usize index = maxIndex_; index > end; --index) {
            Unlink(index - 1);
        }
        if (end != maxIndex_ && !allocated_.TryResize(end)) {
            platform::Fatal("SparseArray failed to shrink its allocation flags");
        }
        maxIndex_ = end;
    }

    void FinishCompaction(usize size) noexcept {
        if (!allocated_.TryResize(size)) {
            platform::Fatal("SparseArray failed to shrink its allocation flags");
        }
        maxIndex_ = size;
        freeCount_ = 0;
        firstFree_ = NoIndex;
    }

    // 용량 확보 뒤에만 호출하므로 할당 없이 성공해야 한다.
    void AppendFlags(bool value, usize count) noexcept {
        if (count != 0 && !allocated_.TryAdd(value, count)) {
            platform::Fatal("SparseArray allocation flags exceeded their reserved capacity");
        }
    }

    void PushFree(usize index) noexcept {
        nodes_[index].link = FreeLink{NoIndex, firstFree_};
        if (firstFree_ != NoIndex) {
            nodes_[firstFree_].link.previous = static_cast<uint32>(index);
        }
        firstFree_ = static_cast<uint32>(index);
        ++freeCount_;
    }

    void Unlink(usize index) noexcept {
        const FreeLink link = nodes_[index].link;
        if (link.previous != NoIndex) {
            nodes_[link.previous].link.next = link.next;
        } else {
            firstFree_ = link.next;
        }
        if (link.next != NoIndex) {
            nodes_[link.next].link.previous = link.previous;
        }
        --freeCount_;
    }

    usize NextAllocated(usize start) const noexcept {
        if (start >= maxIndex_) {
            return NotFound;
        }
        const usize found = allocated_.Find(true, start);
        return found == BitArray::NotFound ? NotFound : found;
    }

    static constexpr usize Limit() noexcept {
        return MaxSize / sizeof(Node) < MaxSlots ? MaxSize / sizeof(Node) : MaxSlots;
    }

    // 기하급수적으로 늘리되 Limit()을 넘지 않는다. required <= Limit()은 caller가 보장한다.
    usize GrowthCapacity(usize required) const noexcept {
        const usize maximum = Limit();
        if (capacity_ == 0) {
            return maximum < 4 || required > 4 ? required : 4;
        }
        const usize growth = capacity_ / 2 + 1;
        const usize candidate = growth > maximum - capacity_ ? maximum : capacity_ + growth;
        return candidate < required ? required : candidate;
    }

    void CheckIndex(usize index) const noexcept {
        if (!IsValidIndex(index)) {
            platform::Fatal("SparseArray index is not allocated");
        }
    }

    void Release() noexcept {
        Clear();
        FreeNodes(nodes_);
        nodes_ = nullptr;
        capacity_ = 0;
        if (!allocated_.Trim()) {
            platform::Fatal("SparseArray failed to release its allocation flags");
        }
    }

    void ForgetStorage() noexcept {
        nodes_ = nullptr;
        maxIndex_ = 0;
        capacity_ = 0;
        freeCount_ = 0;
        firstFree_ = NoIndex;
    }

    Allocator* allocator_;
    Node* nodes_ = nullptr;
    BitArray allocated_;
    usize maxIndex_ = 0;
    usize capacity_ = 0;
    usize freeCount_ = 0;
    uint32 firstFree_ = NoIndex;
};

} // namespace rocks

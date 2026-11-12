#pragma once

#include <Base/Checked.h>
#include <Core/Hash.h>
#include <Core/Memory/Allocator.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

enum class InsertOutcome : uint8 {
    Inserted,
    AlreadyPresent
};

// value는 컨테이너가 소유한 빌린 포인터다. 재해시·삭제 등의 무효화 규약을 지킨다.
template <typename V>
struct FindOrAddResult {
    V* value;
    InsertOutcome outcome;
};

// 같은 컨테이너의 동시 접근은 caller가 동기화한다. const 순회도 내부 상태를 변경한다.
// allocator는 모든 컨테이너보다 오래 살아야 한다.
// 키/값 생성·복사·이동·소멸, 해시·동등 비교 및 allocator 콜백에서는 같은 Map에 대한
// 읽기를 포함한 모든 재진입 접근이 금지된다. 순회 predicate/factory의 중첩 조회와는 다르다.
// 해시와 동등 비교는 noexcept이며 같은 키에 안정적인 값을 반환하고 동등 키는 해시가 같아야 한다.
// K의 복사·이동 생성 후 키는 원본과 동등하고 해시가 같아야 한다. 이동한 정책 역시 동일한
// 키에 대해 이동 전과 같은 결과를 반환해야 한다.
// StringView 등 비소유 키의 바이트는 키가 존재하는 동안 유효하고 변경되지 않아야 한다.
// rehash 성공, Reset, 대입, 파괴는 모든 포인터를 무효화한다. Erase는 해당 원소만,
// Clear는 모든 원소를, InsertOrAssign의 기존 값 교체는 그 값 포인터를 무효화한다.
// 실패한 Reserve/Insert는 기존 내용과 포인터를 보존한다. 신규 삽입과 값 교체는 저장소의
// 직접 별칭을 거부한다. key/value 인자끼리 겹칠 때는 양쪽 생성이 trivial이어야 한다.
// 중복 Insert는 변경 없이 성공한다. 간접 별칭은 탐지할 수 없으므로 caller가 수명을 지킨다.
template <typename K, typename V, typename Hasher = Hash<K>, typename KeyEqual = Equal<K>>
class HashMap {
    static_assert(IsNothrowMoveConstructibleV<K> && IsNothrowDestructibleV<K>);
    static_assert(IsNothrowMoveConstructibleV<V> && IsNothrowDestructibleV<V>);
    static_assert(IsNothrowMoveConstructibleV<Hasher> && IsNothrowDestructibleV<Hasher>);
    static_assert(IsNothrowMoveConstructibleV<KeyEqual> && IsNothrowDestructibleV<KeyEqual>);
    static_assert(__is_nothrow_assignable(Hasher&, Hasher&&));
    static_assert(__is_nothrow_assignable(KeyEqual&, KeyEqual&&));

    enum class State : uint8 {
        Empty,
        Occupied,
        Deleted
    };
    struct Slot {
        Slot() noexcept : hash(0), state(State::Empty) {
        }
        alignas(K) unsigned char key[sizeof(K)];
        alignas(V) unsigned char value[sizeof(V)];
        uint64 hash;
        State state;
        K* Key() noexcept {
            return platform::Launder(reinterpret_cast<K*>(key));
        }
        const K* Key() const noexcept {
            return platform::Launder(reinterpret_cast<const K*>(key));
        }
        V* Value() noexcept {
            return platform::Launder(reinterpret_cast<V*>(value));
        }
        const V* Value() const noexcept {
            return platform::Launder(reinterpret_cast<const V*>(value));
        }
    };

public:
    explicit HashMap(Allocator& allocator, Hasher hasher = {}, KeyEqual equal = {}) noexcept
        : allocator_(&allocator), hasher_(Move(hasher)), equal_(Move(equal)) {
    }

    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    // allocator와 정책·저장소를 함께 이전한다. 이동한 정책은 기존 키에 같은 해시·동등성을
    // 제공해야 하며, 원본 정책은 각 정책의 이동 후 계약을 따른다.
    HashMap(HashMap&& other) noexcept
        : allocator_(other.allocator_), hasher_(Move(other.hasher_)), equal_(Move(other.equal_)) {
        other.CheckNotIterating();
        slots_ = other.slots_;
        slotCount_ = other.slotCount_;
        size_ = other.size_;
        deleted_ = other.deleted_;
        other.slots_ = nullptr;
        other.slotCount_ = 0;
        other.size_ = 0;
        other.deleted_ = 0;
        ++other.revision_;
    }

    HashMap& operator=(HashMap&& other) noexcept {
        CheckNotIterating();
        other.CheckNotIterating();
        if (this != &other) {
            Reset();
            allocator_ = other.allocator_;
            hasher_ = Move(other.hasher_);
            equal_ = Move(other.equal_);
            slots_ = other.slots_;
            slotCount_ = other.slotCount_;
            size_ = other.size_;
            deleted_ = other.deleted_;
            other.slots_ = nullptr;
            other.slotCount_ = 0;
            other.size_ = 0;
            other.deleted_ = 0;
            ++other.revision_;
            ++revision_;
        }
        return *this;
    }

    ~HashMap() noexcept {
        Reset();
    }

    usize Count() const noexcept {
        return size_;
    }

    // 슬롯을 늘리지 않고 보관 가능한 원소 수다. 삭제 누적 정리는 같은 크기로 재할당할 수 있다.
    usize Capacity() const noexcept {
        return slotCount_ / 2;
    }

    bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    // 값의 빌린 포인터를 반환하며 없으면 nullptr다. 호출 예: if (auto* p = map.Find(key)).
    // 조회 키 Q가 정책의 hash(Q), equal(K,Q)와 호환된다면 이종 조회도 허용한다.
    // 동등한 서로 다른 타입의 키는 같은 해시를 반환해야 한다. 반환 포인터는 재해시나
    // 해당 항목의 교체·삭제, Clear, Reset, 대입, 파괴 시 무효화된다.
    template <typename Q>
    V* Find(const Q& key) noexcept {
        const usize index = FindIndex(key, HashKey(key));
        return index == slotCount_ ? nullptr : slots_[index].Value();
    }

    template <typename Q>
    const V* Find(const Q& key) const noexcept {
        const usize index = FindIndex(key, HashKey(key));
        return index == slotCount_ ? nullptr : slots_[index].Value();
    }

    template <typename Q>
    bool Contains(const Q& key) const noexcept {
        return Find(key) != nullptr;
    }

    // 저장된 키를 읽기 전용으로 빌린다. 값 포인터와 같은 재해시/삭제 수명 규약을 따른다.
    template <typename Q>
    const K* FindStoredKey(const Q& key) const noexcept {
        const usize index = FindIndex(key, HashKey(key));
        return index == slotCount_ ? nullptr : slots_[index].Key();
    }

    // KeyArg는 정확한 K여야 한다. 이종 키는 조회에서만 지원한다. 중복 키는 변경하지 않고
    // AlreadyPresent다. 실패나 중복 시 입력을 이동하지 않는다.
    template <typename KeyArg, typename ValueArg>
    Result<InsertOutcome> Insert(KeyArg&& key, ValueArg&& value) noexcept {
        return InsertImpl<false>(Forward<KeyArg>(key), Forward<ValueArg>(value));
    }

    // 기존 값을 새 값으로 재구성한다. 키는 변경하지 않고 기존 항목 교체 시 AlreadyPresent다.
    // 성공한 교체는 입력 값을 소비하고 해당 값 포인터를 무효화한다. 실패 시 입력을 소비하지 않는다.
    template <typename KeyArg, typename ValueArg>
    Result<InsertOutcome> InsertOrAssign(KeyArg&& key, ValueArg&& value) noexcept {
        return InsertImpl<true>(Forward<KeyArg>(key), Forward<ValueArg>(value));
    }

    // 기존 값이 있으면 입력을 소비하지 않고 그 포인터를 반환한다. 새 값을 삽입하는 경우에도
    // 실패하면 입력과 기존 원소·주소는 유지된다. value 포인터의 수명은 Find와 같다.
    template <typename KeyArg, typename ValueArg>
    Result<FindOrAddResult<V>> FindOrAdd(KeyArg&& key, ValueArg&& value) noexcept {
        V* found = nullptr;
        auto result = InsertImpl<false>(Forward<KeyArg>(key), Forward<ValueArg>(value), &found);
        if (!result) {
            return Result<FindOrAddResult<V>>::Failure(result.Error());
        }
        return Result<FindOrAddResult<V>>::Success({found, result.Value()});
    }

    // 키가 없을 때만 noexcept 기본 생성자를 호출한다. 할당기 등이 필요한 값은 With를 쓴다.
    template <typename KeyArg>
    Result<FindOrAddResult<V>> FindOrAdd(KeyArg&& key) noexcept {
        static_assert(__is_nothrow_constructible(V));
        return FindOrAddWith(Forward<KeyArg>(key),
                             []() noexcept { return Result<V>::Success(V{}); });
    }

    // factory는 키가 없을 때만 한 번 호출하며 noexcept Result<V>를 반환해야 한다.
    // factory 실패는 기존 포인터와 키를 보존한다. factory의 외부 부작용은 되돌리지 않으며,
    // 기존 원소·키나 입력 key를 직접 수정하지 않아야 한다. 간접 별칭의 수명은 caller 책임이다.
    // factory 중 구조 변경은 차단되며, 재해시가 일어나면 기존 포인터는 무효화된다.
    template <typename KeyArg, typename Factory>
    Result<FindOrAddResult<V>> FindOrAddWith(KeyArg&& key, Factory&& factory) noexcept {
        static_assert(
            IsSameV<RemoveConstT<RemoveReferenceT<KeyArg>>, K>,
            "HashMap insertion key must be exactly K; heterogeneous keys are lookup only");
        static_assert(__is_nothrow_constructible(K, KeyArg&&));
        static_assert(noexcept(factory()));
        static_assert(IsSameV<decltype(factory()), Result<V>>,
                      "HashMap factory must return Result<V>");

        if (iterationDepth_ != 0) {
            return Result<FindOrAddResult<V>>::Failure(ErrorCode::Busy);
        }
        const usize existing = FindIndex(key, HashKey(key));
        if (existing != slotCount_) {
            return Result<FindOrAddResult<V>>::Success(
                {slots_[existing].Value(), InsertOutcome::AlreadyPresent});
        }
        if (AliasesSlots(key)) {
            return Result<FindOrAddResult<V>>::Failure(ErrorCode::InvalidArgument);
        }

        // 생성된 값의 소멸까지 가드를 유지한다. 실제 삽입 동안에만 일시 해제한다.
        IterationGuard guard(*this);
        auto created = factory();
        if (!created) {
            return Result<FindOrAddResult<V>>::Failure(created.Error());
        }

        V* found = nullptr;
        guard.Suspend();
        auto result = InsertImpl<false>(Forward<KeyArg>(key), Move(created.Value()), &found);
        guard.Resume();
        if (!result) {
            return Result<FindOrAddResult<V>>::Failure(result.Error());
        }
        return Result<FindOrAddResult<V>>::Success({found, result.Value()});
    }

    // capacity는 슬롯 수가 아닌 원소 수다. 할당 실패와 overflow에서 기존 주소도 보존한다.
    Result<void> Reserve(usize capacity) noexcept {
        if (iterationDepth_ != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        if (capacity <= Capacity()) {
            return Result<void>::Success();
        }
        constexpr usize maximum = MaxSize / sizeof(Slot);
        if (maximum < 8 || capacity > maximum / 2) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        usize target = 8;
        while (capacity > target / 2) {
            if (target > maximum / 2) {
                return Result<void>::Failure(ErrorCode::Overflow);
            }
            target *= 2;
        }
        return Rehash(target);
    }

    // tombstone을 제거하고 현재 원소 수에 필요한 최소 용량으로 축소한다. 할당 실패는
    // 데이터와 기존 포인터를 보존한다. 빈 Map은 즉시 저장소를 반환한다.
    Result<void> Compact() noexcept {
        if (iterationDepth_ != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        if (size_ == 0) {
            Reset();
            return Result<void>::Success();
        }
        usize target = 8;
        while (size_ > target / 2) {
            target *= 2; // 이미 할당된 slotCount_ 이하이므로 overflow할 수 없다.
        }
        if (target == slotCount_ && deleted_ == 0) {
            return Result<void>::Success();
        }
        return Rehash(target);
    }

    // 키가 존재하면 삭제 후 true, 없으면 false다. 삭제한 원소의 빌린 포인터는 무효다.
    // 동일 컨테이너 순회 중 구조 변경은 Fatal이다. 다른 스레드 접근도 외부 동기화가 필요하다.
    template <typename Q>
    bool Erase(const Q& key) noexcept {
        CheckNotIterating();
        const usize index = FindIndex(key, HashKey(key));
        if (index == slotCount_) {
            return false;
        }
        Slot& slot = slots_[index];
        DestroyAt(slot.Value());
        DestroyAt(slot.Key());
        slot.state = State::Deleted;
        --size_;
        ++revision_;
        ++deleted_;
        if (size_ == 0) {
            ClearSlots();
        }
        return true;
    }

    // predicate가 true를 반환한 현재 원소만 삭제하고 삭제 수를 반환한다.
    // 순회 중 다른 구조 변경은 금지하며 중첩 조회는 가능하다. predicate는 noexcept이고
    // 정확히 bool을 반환해야 한다. 삭제된 원소의 포인터는 사용할 수 없다.
    template <typename Predicate>
    Result<usize> EraseIf(Predicate&& predicate) noexcept {
        static_assert(
            noexcept(predicate(*static_cast<const K*>(nullptr), *static_cast<const V*>(nullptr))));
        static_assert(IsSameV<decltype(predicate(*static_cast<const K*>(nullptr),
                                                 *static_cast<const V*>(nullptr))),
                              bool>,
                      "HashMap EraseIf predicate must return bool");
        if (iterationDepth_ != 0) {
            return Result<usize>::Failure(ErrorCode::Busy);
        }

        usize removed = 0;
        EnterIteration();
        for (usize index = 0; index < slotCount_; ++index) {
            Slot& slot = slots_[index];
            if (slot.state != State::Occupied || !predicate(*static_cast<const K*>(slot.Key()),
                                                            *static_cast<const V*>(slot.Value()))) {
                continue;
            }
            // 소멸 중에는 이 항목을 조회할 수 없게 먼저 논리적으로 제거한다.
            slot.state = State::Deleted;
            --size_;
            ++revision_;
            ++deleted_;
            ++removed;
            DestroyAt(slot.Value());
            DestroyAt(slot.Key());
        }
        --iterationDepth_;
        if (removed != 0 && size_ == 0) {
            ClearSlots();
        }
        return Result<usize>::Success(removed);
    }

    // 모든 원소를 제거하지만 할당된 용량은 유지한다. 기존 원소 포인터는 무효다.
    void Clear() noexcept {
        CheckNotIterating();
        ClearSlots();
    }

    // 모든 원소와 할당된 저장소를 반환한다. 같은 allocator로 다시 사용할 수 있다.
    void Reset() noexcept {
        CheckNotIterating();
        ClearSlots();
        FreeSlots(slots_, slotCount_);
        slots_ = nullptr;
        slotCount_ = 0;
    }

    // 순서와 순회 간 순서 안정성은 보장하지 않는다. 순회 중 중첩 읽기·순회 및 값 수정은
    // 가능하지만 구조 변경은 금지한다. 콜백은 noexcept여야 한다. 포인터를 보존하지 않는다.
    template <typename Visitor>
    void ForEach(Visitor&& visitor) noexcept {
        static_assert(
            noexcept(visitor(*static_cast<const K*>(nullptr), *static_cast<V*>(nullptr))));
        EnterIteration();
        for (usize index = 0; index < slotCount_; ++index) {
            Slot& slot = slots_[index];
            if (slot.state == State::Occupied) {
                visitor(*static_cast<const K*>(slot.Key()), *slot.Value());
            }
        }
        --iterationDepth_;
    }

    template <typename Visitor>
    void ForEach(Visitor&& visitor) const noexcept {
        static_assert(
            noexcept(visitor(*static_cast<const K*>(nullptr), *static_cast<const V*>(nullptr))));
        EnterIteration();
        for (usize index = 0; index < slotCount_; ++index) {
            const Slot& slot = slots_[index];
            if (slot.state == State::Occupied) {
                visitor(*slot.Key(), *slot.Value());
            }
        }
        --iterationDepth_;
    }

    // ---- STL 호환 반복자 ----
    // 원소를 {key, value} 참조 쌍으로 방문하는 전방 반복자다. 순서는 정해지지 않는다.
    //   for (auto [key, value] : map) { value += 1; }
    //   사용자 코드의 STL find_if(map.begin(), map.end(), predicate)도 그대로 쓸 수 있다.
    // 역참조 결과는 값으로 반환되는 참조 쌍(KeyValue)이므로 auto 또는 const auto&로 받는다
    // (auto&는 컴파일되지 않는다). key는 바꿀 수 없고 value는 비 const 순회에서 바꿀 수 있다.
    // 삽입·삭제·Clear·Reserve·Compact·Reset·이동처럼 원소 집합이나 저장소가 바뀌면 기존 반복자의
    // 역참조·증가가 Fatal이다(실패한 삽입도 재해시가 일어났을 수 있어 같게 취급한다).
    // 같은 키의 값 교체(InsertOrAssign)는 반복자를 무효화하지 않지만 이전 값 참조는 무효다.
    // ForEach와 달리 순회 중 구조 변경을 막지 않고 사후에 탐지한다. 컨테이너 소멸 후 사용은
    // 탐지하지 못한다. 동시 사용 규약은 컨테이너와 같다.
    template <typename ValueType>
    struct KeyValue {
        const K& key;
        ValueType& value;
    };

    template <typename MapType, typename ValueType>
    class BasicIterator {
    public:
        using value_type = KeyValue<ValueType>;
        using reference = KeyValue<ValueType>;
        using difference_type = isize;

        // -> 연산을 위해 값으로 반환한 참조 쌍을 보관한다.
        struct Arrow {
            KeyValue<ValueType> pair;
            const KeyValue<ValueType>* operator->() const noexcept {
                return &pair;
            }
        };

        BasicIterator() noexcept = default;

        KeyValue<ValueType> operator*() const noexcept {
            auto& slot = CheckedSlot();
            return KeyValue<ValueType>{*slot.Key(), *slot.Value()};
        }

        Arrow operator->() const noexcept {
            return Arrow{**this};
        }

        BasicIterator& operator++() noexcept {
            CheckedSlot();
            index_ = map_->NextOccupied(index_ + 1);
            return *this;
        }

        BasicIterator operator++(int) noexcept {
            BasicIterator previous = *this;
            ++*this;
            return previous;
        }

        friend bool operator==(const BasicIterator& left, const BasicIterator& right) noexcept {
            return left.map_ == right.map_ && left.index_ == right.index_;
        }

    private:
        friend class HashMap;

        BasicIterator(MapType* map, usize index) noexcept
            : map_(map), index_(index), revision_(map->revision_) {
        }

        decltype(auto) CheckedSlot() const noexcept {
            if (map_ == nullptr || map_->revision_ != revision_) {
                platform::Fatal("HashMap iterator used after structural modification");
            }
            if (index_ >= map_->slotCount_ || map_->slots_[index_].state != State::Occupied) {
                platform::Fatal("HashMap iterator is not dereferenceable");
            }
            return (map_->slots_[index_]);
        }

        MapType* map_ = nullptr;
        usize index_ = 0;
        uint64 revision_ = 0;
    };

    using key_type = K;
    using mapped_type = V;
    using size_type = usize;
    using difference_type = isize;
    using iterator = BasicIterator<HashMap, V>;
    using const_iterator = BasicIterator<const HashMap, const V>;
    using value_type = KeyValue<V>;

    iterator begin() noexcept {
        return iterator(this, NextOccupied(0));
    }

    iterator end() noexcept {
        return iterator(this, slotCount_);
    }

    const_iterator begin() const noexcept {
        return const_iterator(this, NextOccupied(0));
    }

    const_iterator end() const noexcept {
        return const_iterator(this, slotCount_);
    }

    const_iterator cbegin() const noexcept {
        return begin();
    }

    const_iterator cend() const noexcept {
        return end();
    }

private:
    usize NextOccupied(usize index) const noexcept {
        while (index < slotCount_ && slots_[index].state != State::Occupied) {
            ++index;
        }
        return index;
    }

    struct IterationGuard {
        explicit IterationGuard(const HashMap& map) noexcept : map_(map) {
            map_.EnterIteration();
        }

        ~IterationGuard() noexcept {
            if (active_) {
                --map_.iterationDepth_;
            }
        }

        IterationGuard(const IterationGuard&) = delete;
        IterationGuard& operator=(const IterationGuard&) = delete;

        void Suspend() noexcept {
            --map_.iterationDepth_;
            active_ = false;
        }

        void Resume() noexcept {
            map_.EnterIteration();
            active_ = true;
        }

        const HashMap& map_;
        bool active_ = true;
    };

    // HashSet은 임시 합집합 생성 중 양 입력의 구조 변경을 차단한다.
    template <typename, typename, typename>
    friend class HashSet;

    template <typename Q>
    uint64 HashKey(const Q& key) const noexcept {
        static_assert(noexcept(static_cast<uint64>(hasher_(key))));
        return static_cast<uint64>(hasher_(key));
    }

    template <typename Q>
    bool KeysEqual(const K& left, const Q& right) const noexcept {
        static_assert(noexcept(static_cast<bool>(equal_(left, right))));
        return static_cast<bool>(equal_(left, right));
    }

    template <typename Q>
    usize FindIndex(const Q& key, uint64 hash) const noexcept {
        if (slotCount_ == 0) {
            return 0;
        }
        usize index = static_cast<usize>(hash) & (slotCount_ - 1);
        // 삭제 슬롯은 탐사를 끊지 않는다. 슬롯 전체를 살펴도 종료하도록 상한을 둔다.
        for (usize visited = 0; visited < slotCount_; ++visited) {
            const Slot& slot = slots_[index];
            if (slot.state == State::Empty) {
                return slotCount_;
            }
            if (slot.state == State::Occupied && slot.hash == hash && KeysEqual(*slot.Key(), key)) {
                return index;
            }
            index = (index + 1) & (slotCount_ - 1);
        }
        return slotCount_;
    }

    static bool Overlap(uptr a, usize aSize, uptr b, usize bSize) noexcept {
        // 서로 다른 객체의 포인터를 직접 순서 비교하거나 끝 주소를 더하지 않는다.
        return a <= b ? b - a < aSize : a - b < bSize;
    }

    template <typename A>
    bool AliasesSlots(A& arg) const noexcept {
        if (!slots_) {
            return false;
        }
        const uptr address = reinterpret_cast<uptr>(platform::AddressOf(arg));
        const uptr base = reinterpret_cast<uptr>(slots_);
        return Overlap(address, sizeof(A), base, slotCount_ * sizeof(Slot));
    }

    template <typename A, typename B>
    static bool ArgumentsOverlap(A& a, B& b) noexcept {
        return Overlap(reinterpret_cast<uptr>(platform::AddressOf(a)), sizeof(A),
                       reinterpret_cast<uptr>(platform::AddressOf(b)), sizeof(B));
    }

    template <bool Assign, typename KeyArg, typename ValueArg>
    Result<InsertOutcome> InsertImpl(KeyArg&& key, ValueArg&& value,
                                     V** resultValue = nullptr) noexcept {
        static_assert(
            IsSameV<RemoveConstT<RemoveReferenceT<KeyArg>>, K>,
            "HashMap insertion key must be exactly K; heterogeneous keys are lookup only");
        static_assert(__is_nothrow_constructible(K, KeyArg&&));
        static_assert(__is_nothrow_constructible(V, ValueArg&&));
        if (iterationDepth_ != 0) {
            return Result<InsertOutcome>::Failure(ErrorCode::Busy);
        }
        // 조회와 중복 확인은 입력을 이동하지 않는다. 기존 키의 값 교체만 별도로 처리한다.
        const K& lookup = key;
        const uint64 hash = HashKey(lookup);
        const usize existing = FindIndex(lookup, hash);
        constexpr bool safeSharedInput = __is_trivially_constructible(K, KeyArg&&) &&
                                         __is_trivially_constructible(V, ValueArg&&);
        if (existing != slotCount_) {
            if constexpr (Assign) {
                if (AliasesSlots(key) || AliasesSlots(value) ||
                    (!safeSharedInput && ArgumentsOverlap(key, value))) {
                    return Result<InsertOutcome>::Failure(ErrorCode::InvalidArgument);
                }
                DestroyAt(slots_[existing].Value());
                ConstructAt<V>(slots_[existing].value, Forward<ValueArg>(value));
            }
            if (resultValue != nullptr) {
                *resultValue = slots_[existing].Value();
            }
            return Result<InsertOutcome>::Success(InsertOutcome::AlreadyPresent);
        }
        if (AliasesSlots(key) || AliasesSlots(value) ||
            (!safeSharedInput && ArgumentsOverlap(key, value))) {
            return Result<InsertOutcome>::Failure(ErrorCode::InvalidArgument);
        }

        // 모든 실패 가능한 할당과 재구성이 끝나기 전에는 입력 객체를 이동하지 않는다.
        if (size_ == MaxSize) {
            return Result<InsertOutcome>::Failure(ErrorCode::Overflow);
        }
        auto reserve = Reserve(size_ + 1);
        if (!reserve) {
            return Result<InsertOutcome>::Failure(reserve.Error());
        }
        // tombstone 누적으로 탐색이 길어지는 것을 완화한다. 실패 시 입력은 소비하지 않는다.
        if (deleted_ > slotCount_ / 4 && deleted_ > size_) {
            auto cleanup = Rehash(slotCount_);
            if (!cleanup) {
                return Result<InsertOutcome>::Failure(cleanup.Error());
            }
        }
        usize index = static_cast<usize>(hash) & (slotCount_ - 1);
        usize deleted = slotCount_;
        // 충돌 사슬을 유지하면서 최초 삭제 슬롯을 재사용한다.
        for (usize visited = 0; visited < slotCount_; ++visited) {
            Slot& slot = slots_[index];
            if (slot.state == State::Deleted && deleted == slotCount_) {
                deleted = index;
            }
            if (slot.state == State::Empty) {
                if (deleted != slotCount_) {
                    index = deleted;
                }
                break;
            }
            index = (index + 1) & (slotCount_ - 1);
        }
        // 전체 슬롯이 tombstone일 수 있다. 항상 한 개 이상의 비점유 슬롯이 존재한다.
        if (slots_[index].state == State::Occupied && deleted != slotCount_) {
            index = deleted;
        }
        Slot& target = slots_[index];
        if (target.state == State::Occupied) {
            platform::Fatal("HashMap insertion found no free slot");
        }
        if (target.state == State::Deleted) {
            --deleted_;
        }
        ConstructAt<K>(target.key, Forward<KeyArg>(key));
        ConstructAt<V>(target.value, Forward<ValueArg>(value));
        target.hash = hash;
        target.state = State::Occupied;
        ++size_;
        ++revision_;
        if (resultValue != nullptr) {
            *resultValue = target.Value();
        }
        return Result<InsertOutcome>::Success(InsertOutcome::Inserted);
    }

    Result<void> Rehash(usize count) noexcept {
        usize bytes = 0;
        if (!TryMultiply(count, sizeof(Slot), bytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        auto allocation = allocator_->Allocate(bytes, alignof(Slot));
        if (!allocation) {
            return Result<void>::Failure(allocation.Error());
        }
        Slot* replacement = static_cast<Slot*>(allocation.Value());
        if (!replacement || reinterpret_cast<uptr>(replacement) % alignof(Slot) != 0) {
            platform::Fatal("HashMap allocator returned invalid storage");
        }

        // 할당에 성공한 뒤에만 이전 객체를 noexcept 이동하고 수명을 끝낸다.
        for (usize i = 0; i < count; ++i) {
            ConstructAt<Slot>(replacement + i);
        }
        for (usize i = 0; i < slotCount_; ++i) {
            Slot& old = slots_[i];
            if (old.state != State::Occupied) {
                continue;
            }
            usize index = static_cast<usize>(old.hash) & (count - 1);
            while (replacement[index].state == State::Occupied) {
                index = (index + 1) & (count - 1);
            }
            Slot& dest = replacement[index];
            ConstructAt<K>(dest.key, Move(*old.Key()));
            ConstructAt<V>(dest.value, Move(*old.Value()));
            dest.hash = old.hash;
            dest.state = State::Occupied;
            DestroyAt(old.Value());
            DestroyAt(old.Key());
            old.state = State::Empty;
        }
        FreeSlots(slots_, slotCount_);
        slots_ = replacement;
        slotCount_ = count;
        deleted_ = 0;
        ++revision_;
        return Result<void>::Success();
    }

    void ClearSlots() noexcept {
        for (usize i = 0; i < slotCount_; ++i) {
            Slot& slot = slots_[i];
            if (slot.state == State::Occupied) {
                DestroyAt(slot.Value());
                DestroyAt(slot.Key());
            }
            slot.state = State::Empty;
        }
        size_ = 0;
        deleted_ = 0;
        ++revision_;
    }

    void FreeSlots(Slot* slots, usize count) noexcept {
        if (!slots) {
            return;
        }
        for (usize i = 0; i < count; ++i) {
            DestroyAt(slots + i);
        }
        if (!allocator_->Deallocate(slots)) {
            platform::Fatal("HashMap storage was not owned by its allocator");
        }
    }

    void CheckNotIterating() const noexcept {
        if (iterationDepth_ != 0) {
            platform::Fatal("HashMap structural mutation during iteration");
        }
    }

    void EnterIteration() const noexcept {
        if (iterationDepth_ == MaxSize) {
            platform::Fatal("HashMap iteration depth overflow");
        }
        ++iterationDepth_;
    }

    Allocator* allocator_;
    Hasher hasher_;
    KeyEqual equal_;
    Slot* slots_ = nullptr;
    usize slotCount_ = 0;
    usize size_ = 0;
    usize deleted_ = 0;
    mutable usize iterationDepth_ = 0;
    // 원소 집합·저장소 배치가 바뀔 때마다 증가한다. STL식 반복자가 구조 변경 후 사용을 탐지한다.
    uint64 revision_ = 0;
};

} // namespace rocks

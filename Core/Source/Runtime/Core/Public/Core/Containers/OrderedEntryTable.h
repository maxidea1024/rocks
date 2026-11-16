#pragma once

// OrderedMap·OrderedSet의 저장소다(공개 API가 아니다 — rocks::detail).
// 삽입 순서의 조밀 항목 배열과 open addressing 색인(u32 항목 위치)을 할당 한 번으로 잡는다.
// 순서·압축·무효화 규칙과 사용법은 Core/Containers/OrderedMap.h 머리 주석을 본다.
//
// 불변식:
//   - 산 항목마다 색인 칸이 정확히 하나 있고, 그 칸은 항목 위치를 담는다.
//   - 지운 항목의 칸은 DeletedSlot이고, 항목은 죽은 것으로 남는다(자리를 다시 쓰지 않는다).
//   - 위치 [0, used_)의 항목만 쓴 자리다. 산 항목의 상대 순서가 삽입 순서다.
//   - 용량은 0 또는 2의 거듭제곱이고, 색인 칸 수는 용량의 두 배다.
//   - 삽입 직전 (산 칸 + 삭제 칸) < 칸 수라 탐사는 늘 빈 칸에서 끝난다.
//
// 반환 순서(리플렉션 B9f3 리뷰 — 경로 쓰기의 B6 규칙 "옛 버퍼는 게시 뒤 반환"): 옛 저장소는 새
// 저장소와 멤버(항목·색인·용량·개수)를 게시한 뒤에 반환한다(Reallocate·Release·CommitReserve).
// 원소의 소멸은 그 항목을 논리적으로 지운 뒤(산 표시·개수·색인 칸)에 일어난다. 그래서 allocator의
// 해제 콜백이 같은 표를 읽으면 늘 일관된 상태(옛 저장소가 아니라 게시된 표)를 본다.

#include <Base/Checked.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/ContainerDetail.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Core/Traits.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks::detail {

template <typename K, typename V>
class OrderedEntryTable {
public:
    // 삽입 순서의 항목이다. alive가 거짓이면 키·값의 수명이 끝난 죽은 항목이다.
    struct Entry {
        alignas(K) unsigned char key[sizeof(K)];
        alignas(V) unsigned char value[sizeof(V)];
        uint64 hash;
        bool alive;

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

    static_assert(alignof(Entry) >= alignof(uint32),
                  "OrderedMap index must be aligned when it follows the entries");

    // 색인 칸의 표지다. 항목 위치는 늘 DeletedSlot보다 작다.
    static constexpr uint32 EmptySlot = NumericLimits<uint32>::Max();
    static constexpr uint32 DeletedSlot = EmptySlot - 1;
    // 첫 할당의 항목 수다.
    static constexpr usize MinimumCapacity = 4;
    // 위치가 DeletedSlot 아래에 머무는 가장 큰 2의 거듭제곱(2^31)이다.
    static constexpr usize MaximumCapacity =
        static_cast<usize>(NumericLimits<uint32>::Max() / 2) + 1;

    explicit OrderedEntryTable(Allocator& allocator) noexcept : allocator_(&allocator) {
    }

    // 용량 capacity의 저장소 byte다(항목 capacity개 + 색인 칸 capacity × 2개의 u32 — Reallocate가
    // 할당 한 번으로 잡는 크기). usize를 넘으면 false다.
    static constexpr bool StorageBytes(usize capacity, usize& bytes) noexcept {
        usize entryBytes = 0;
        usize indexBytes = 0;
        return TryMultiply(capacity, sizeof(Entry), entryBytes) &&
               TryMultiply(capacity, 2 * sizeof(uint32), indexBytes) &&
               TryAdd(entryBytes, indexBytes, bytes);
    }

    // 빈 표(또는 Clear한 표)의 Reserve(count)가 새로 잡는 저장소 byte의 상한이다: count가 0이면 0,
    // 아니면 MinimumCapacity부터 두 배씩 count 이상으로 올린 용량의 StorageBytes다(빈 표에서는
    // 정확히 그 할당 한 번 — 이미 더 큰 용량이 있으면 할당하지 않는다). count가 MaximumCapacity를
    // 넘거나 byte가 usize를 넘으면 false다(Reserve의 Overflow와 같다).
    static constexpr bool ReservedBytes(usize count, usize& bytes) noexcept {
        if (count == 0) {
            bytes = 0;
            return true;
        }

        if (count > MaximumCapacity) {
            return false;
        }

        usize capacity = MinimumCapacity;
        while (capacity < count) {
            capacity *= 2; // 2의 거듭제곱이 MaximumCapacity 이하에서 멈춘다.
        }

        return StorageBytes(capacity, bytes);
    }

    OrderedEntryTable(const OrderedEntryTable&) = delete;
    OrderedEntryTable& operator=(const OrderedEntryTable&) = delete;

    ~OrderedEntryTable() noexcept {
        Release();
    }

    // 지금 저장소를 반환하고 other의 allocator·저장소를 넘겨받는다. other는 같은 allocator를 쓰는
    // 빈 표가 된다. 자기 자신은 넘기지 않는다.
    void TakeFrom(OrderedEntryTable& other) noexcept {
        Release();
        allocator_ = other.allocator_;
        entries_ = other.entries_;
        index_ = other.index_;
        capacity_ = other.capacity_;
        used_ = other.used_;
        count_ = other.count_;
        tombstones_ = other.tombstones_;
        other.entries_ = nullptr;
        other.index_ = nullptr;
        other.capacity_ = 0;
        other.used_ = 0;
        other.count_ = 0;
        other.tombstones_ = 0;
        ++other.revision_;
        ++revision_;
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    usize Capacity() const noexcept {
        return capacity_;
    }

    // 쓴 항목 수(죽은 항목 포함)이자 다음 삽입 위치다.
    usize Used() const noexcept {
        return used_;
    }

    // 산 항목 수다.
    usize Count() const noexcept {
        return count_;
    }

    // 원소 집합·저장소 배치가 바뀔 때마다 증가한다. 반복자가 구조 변경 뒤 사용을 탐지한다.
    uint64 Revision() const noexcept {
        return revision_;
    }

    Entry& At(usize position) noexcept {
        return entries_[position];
    }

    const Entry& At(usize position) const noexcept {
        return entries_[position];
    }

    // position부터 첫 산 항목의 위치이며 없으면 Used()다.
    usize NextAlive(usize position) const noexcept {
        while (position < used_ && !entries_[position].alive) {
            ++position;
        }

        return position;
    }

    // hash가 같고 matches(const K&)가 참인 산 항목의 색인 칸이며 없으면 IndexNone이다. 삭제 칸과
    // 대량 삭제 중 아직 정리하지 않은 죽은 항목의 칸(MarkDeadKeepingSlot)은 탐사를 끊지 않는다.
    template <typename Matches>
    usize FindSlot(uint64 hash, Matches&& matches) const noexcept {
        if (capacity_ == 0) {
            return IndexNone;
        }

        const usize slotCount = SlotCount();
        const usize mask = slotCount - 1;
        usize slot = static_cast<usize>(hash) & mask;
        for (usize visited = 0; visited < slotCount; ++visited) {
            const uint32 position = index_[slot];
            if (position == EmptySlot) {
                return IndexNone;
            }

            if (position != DeletedSlot) {
                const Entry& entry = entries_[position];
                if (entry.alive && entry.hash == hash && matches(*entry.Key())) {
                    return slot;
                }
            }

            slot = (slot + 1) & mask;
        }

        return IndexNone;
    }

    usize PositionOfSlot(usize slot) const noexcept {
        return static_cast<usize>(index_[slot]);
    }

    // [address, address + size)가 항목 배열과 겹치는지 본다(삽입 인자의 직접 별칭 검사).
    bool Aliases(uptr address, usize size) const noexcept {
        if (entries_ == nullptr) {
            return false;
        }

        return AddressRangesOverlap(address, size, reinterpret_cast<uptr>(entries_),
                                    capacity_ * sizeof(Entry));
    }

    // 끝에 항목 하나를 붙일 자리를 만든다. 가득 찼을 때 죽은 항목이 용량의 1/8 이상이면 제자리
    // 압축(할당 없음 — 그 죽은 항목을 만든 삭제들에 상환), 아니면 두 배로 성장한다. 실패하면
    // 아무것도 바꾸지 않는다.
    Result<void> EnsureAppendRoom() noexcept {
        if (used_ < capacity_) {
            return Result<void>::Success();
        }

        const usize dead = used_ - count_;
        if (dead != 0 && (dead >= capacity_ / 8 || capacity_ == MaximumCapacity)) {
            CompactEntries();
            return Result<void>::Success();
        }

        if (capacity_ == MaximumCapacity) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        return Reallocate(capacity_ == 0 ? MinimumCapacity : capacity_ * 2);
    }

    // EnsureAppendRoom이 성공한 직후에만 부른다. 실패하지 않는다. 같은 키가 없음은 caller가
    // 확인했다.
    template <typename KeyArg, typename ValueArg>
    Entry& Append(uint64 hash, KeyArg&& key, ValueArg&& value) noexcept {
        // 삭제 칸이 쌓여 탐사가 길어지면 제자리에서 색인만 다시 짓는다(항목은 움직이지 않는다).
        if (tombstones_ > SlotCount() / 4) {
            RebuildIndex();
        }

        const usize position = used_;
        Entry* entry = ConstructAt<Entry>(entries_ + position);
        ConstructAt<K>(entry->key, Forward<KeyArg>(key));
        ConstructAt<V>(entry->value, Forward<ValueArg>(value));
        entry->hash = hash;
        entry->alive = true;
        if (PlaceInIndex(index_, SlotCount(), hash, position)) {
            --tombstones_;
        }

        ++used_;
        ++count_;
        ++revision_;
        return *entry;
    }

    // 2단계 예약(PrepareReserve → CommitReserve)의 준비 결과다(이동 전용). 새 저장소가 필요한
    // 예약이면 그 저장소를 소유하고(아직 표에 넣지 않았다), 제자리 압축이면 할당이 없다. 쓰지 않고
    // 소멸하면 잡은 저장소를 반환한다(표는 그대로). 준비한 표에만, 그 뒤 구조 변경 없이 한 번
    // CommitReserve한다(어기면 Fatal).
    class Reservation {
    public:
        Reservation(Reservation&& other) noexcept
            : table_(other.table_), allocator_(other.allocator_), storage_(other.storage_),
              capacity_(other.capacity_), revision_(other.revision_), action_(other.action_) {
            other.table_ = nullptr;
            other.storage_ = nullptr;
            other.action_ = Action::None;
        }

        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation& operator=(Reservation&&) = delete;

        ~Reservation() noexcept {
            if (storage_ != nullptr && !allocator_->Deallocate(storage_)) {
                platform::Fatal("OrderedMap reservation storage was not owned by its allocator");
            }
        }

    private:
        friend class OrderedEntryTable;

        enum class Action : uint8 {
            None,
            Compact,
            Grow
        };

        Reservation(const OrderedEntryTable* table, Action action, Entry* storage,
                    usize capacity) noexcept
            : table_(table), allocator_(table->allocator_), storage_(storage), capacity_(capacity),
              revision_(table->revision_), action_(action) {
        }

        const OrderedEntryTable* table_ = nullptr;
        Allocator* allocator_ = nullptr;
        Entry* storage_ = nullptr;
        usize capacity_ = 0;
        uint64 revision_ = 0;
        Action action_ = Action::None;
    };

    // Reserve(capacity)와 같은 판정으로 예약을 준비한다: 할당이 필요하면 새 저장소만 잡고 표는
    // 바꾸지 않는다. 오류: Overflow, 할당 오류(표는 그대로).
    Result<Reservation> PrepareReserve(usize capacity) noexcept {
        using Action = typename Reservation::Action;
        if (capacity <= count_) {
            return Result<Reservation>::Success(Reservation(this, Action::None, nullptr, 0));
        }

        usize target = 0;
        if (capacity <= capacity_) {
            if (capacity - count_ <= capacity_ - used_) {
                return Result<Reservation>::Success(Reservation(this, Action::None, nullptr, 0));
            }

            const usize dead = used_ - count_;
            if (dead >= capacity_ / 8 || count_ < capacity_ / 2 || capacity_ == MaximumCapacity) {
                return Result<Reservation>::Success(Reservation(this, Action::Compact, nullptr, 0));
            }

            target = capacity_ * 2;
        } else {
            if (capacity > MaximumCapacity) {
                return Result<Reservation>::Failure(ErrorCode::Overflow);
            }

            target = capacity_ == 0 ? MinimumCapacity : capacity_;
            while (target < capacity) {
                target *= 2; // 2의 거듭제곱이 MaximumCapacity(2의 거듭제곱) 이하에서 멈춘다.
            }
        }

        auto storage = AllocateStorage(target);
        if (!storage) {
            return Result<Reservation>::Failure(storage.Error());
        }

        return Result<Reservation>::Success(
            Reservation(this, Action::Grow, storage.Value(), target));
    }

    // 준비한 예약을 적용한다(할당 없음, 실패 없음): 제자리 압축 또는 새 저장소로 옮겨 게시한 뒤 옛
    // 저장소를 반환한다. 다른 표의 예약이거나 준비 뒤 구조가 바뀌었으면 Fatal이다.
    void CommitReserve(Reservation& reservation) noexcept {
        using Action = typename Reservation::Action;
        if (reservation.table_ != this ||
            (reservation.action_ != Action::None && reservation.revision_ != revision_)) {
            platform::Fatal("OrderedMap reservation committed to a changed or different map");
        }

        if (reservation.action_ == Action::Compact) {
            CompactEntries();
        } else if (reservation.action_ == Action::Grow) {
            Entry* storage = reservation.storage_;
            reservation.storage_ = nullptr;
            AdoptStorage(storage, reservation.capacity_);
        }

        reservation.action_ = Action::None;
    }

    // capacity개 원소를 성장 없이 담게 한다. 용량이 모자라면 새 저장소다. 용량은 되는데 죽은 항목
    // 때문에 끝자리가 모자라면, 죽은 항목이 용량의 1/8 이상이거나 산 항목이 용량의 절반 미만일 때
    // 제자리 압축(할당 없음)을, 아니면 두 배로 성장한다. 그래서 "가득 찬 상태에서 삭제 하나 +
    // Reserve(Count() + 1) + 삽입" 반복도 상환 O(1)이다. Reserve(Capacity()) 반복처럼 매번 남은
    // 용량 전부를 요구하면 보장상 매번 O(용량)이다. 오류: Overflow, 할당 오류. 실패하면 그대로다.
    Result<void> Reserve(usize capacity) noexcept {
        auto prepared = PrepareReserve(capacity);
        if (!prepared) {
            return Result<void>::Failure(prepared.Error());
        }

        CommitReserve(prepared.Value());
        return Result<void>::Success();
    }

    // 죽은 항목과 삭제 칸을 정리하고 최소 용량으로 줄인다. 비었으면 저장소를 반환한다. 실패하면
    // 그대로다.
    Result<void> Compact() noexcept {
        if (count_ == 0) {
            Release();
            return Result<void>::Success();
        }

        usize target = MinimumCapacity;
        while (target < count_) {
            target *= 2; // count_ <= capacity_이므로 넘치지 않는다.
        }

        if (target == capacity_) {
            if (used_ != count_) {
                CompactEntries();
            }

            if (tombstones_ != 0) {
                RebuildIndex();
            }

            return Result<void>::Success();
        }

        return Reallocate(target);
    }

    // 소멸 중 이 항목을 찾을 수 없게 먼저 논리적으로 지운 뒤 키·값의 수명을 끝낸다. 압축은
    // SettleAfterErase가 한다.
    void EraseSlot(usize slot) noexcept {
        Entry& entry = entries_[index_[slot]];
        index_[slot] = DeletedSlot;
        ++tombstones_;
        entry.alive = false;
        --count_;
        ++revision_;
        DestroyAt(entry.Value());
        DestroyAt(entry.Key());
    }

    void EraseAt(usize position) noexcept {
        EraseSlot(SlotOfPosition(entries_[position].hash, position));
    }

    // 대량 삭제에서 칸을 하나씩 찾아 지우는 수가 EagerEraseLimit()을 넘으면 쓰는 길이다. 항목만
    // 죽은 것으로 표시하고 색인 칸은 남긴다(FindSlot은 죽은 항목의 칸을 건너뛴다). 다른 변경 전에
    // 반드시 RebuildIndex로 정리한다.
    void MarkDeadKeepingSlot(usize position) noexcept {
        Entry& entry = entries_[position];
        entry.alive = false;
        --count_;
        ++revision_;
        DestroyAt(entry.Value());
        DestroyAt(entry.Key());
    }

    // 한 번의 대량 삭제에서 칸을 하나씩 찾아 지우는 최대 수(칸 수/8 — 용량에 비례)다. 넘는 삭제는
    // MarkDeadKeepingSlot과 RebuildIndex 한 번으로 처리한다. 해시가 칸에 고르게 퍼지면 칸 하나
    // 찾기와 RebuildIndex의 원소당 배치가 기대 O(1)이라, 재구성의 O(칸 수)를 이 수만큼의 삭제에
    // 나눠 대량 삭제가 기대 O(n)이다(기본 SeededHash가 그 전제를 지킨다). 해시가 모두 충돌하면
    // 칸 찾기(EraseAt -> SlotOfPosition)와 재구성의 배치가 각각 사슬 길이만큼 들어 O(n^2)이며, 이
    // 한도는 그 경우를 막지 않는다.
    usize EagerEraseLimit() const noexcept {
        return SlotCount() / 8;
    }

    // 삭제 칸과 죽은 항목의 칸을 없애고 산 항목의 위치로 색인을 다시 채운다(할당 없음, 항목 불변).
    void RebuildIndex() noexcept {
        const usize slotCount = SlotCount();
        for (usize slot = 0; slot < slotCount; ++slot) {
            index_[slot] = EmptySlot;
        }

        for (usize position = 0; position < used_; ++position) {
            if (entries_[position].alive) {
                PlaceInIndex(index_, slotCount, entries_[position].hash, position);
            }
        }

        tombstones_ = 0;
    }

    // 끝의 죽은 항목은 바로 떼고, 죽은 항목이 산 항목의 절반을 넘으면 제자리 압축한다.
    void SettleAfterErase() noexcept {
        while (used_ != 0 && !entries_[used_ - 1].alive) {
            --used_;
        }

        if (used_ - count_ > count_ / 2) {
            CompactEntries();
        }
    }

    // 모든 원소를 지우고 용량은 유지한다.
    void Clear() noexcept {
        DestroyEntries();
        const usize slotCount = SlotCount();
        for (usize slot = 0; slot < slotCount; ++slot) {
            index_[slot] = EmptySlot;
        }

        tombstones_ = 0;
        ++revision_;
    }

    // 모든 원소와 저장소를 반환한다. 빈 표를 게시한 뒤 옛 저장소를 반환한다(해제 콜백은 빈 표를
    // 본다).
    void Release() noexcept {
        DestroyEntries();
        Entry* old = entries_;
        entries_ = nullptr;
        index_ = nullptr;
        capacity_ = 0;
        tombstones_ = 0;
        ++revision_;
        if (old != nullptr && !allocator_->Deallocate(old)) {
            platform::Fatal("OrderedMap storage was not owned by its allocator");
        }
    }

private:
    usize SlotCount() const noexcept {
        return capacity_ * 2;
    }

    // 산 항목 position을 가리키는 색인 칸이다.
    usize SlotOfPosition(uint64 hash, usize position) const noexcept {
        const usize slotCount = SlotCount();
        const usize mask = slotCount - 1;
        usize slot = static_cast<usize>(hash) & mask;
        for (usize visited = 0; visited < slotCount; ++visited) {
            const uint32 stored = index_[slot];
            if (static_cast<usize>(stored) == position) {
                return slot;
            }

            if (stored == EmptySlot) {
                break;
            }

            slot = (slot + 1) & mask;
        }

        platform::Fatal("OrderedMap index lost a live entry");
    }

    // 첫 빈 칸이나 삭제 칸에 위치를 적는다. 삭제 칸을 다시 썼으면 true다.
    static bool PlaceInIndex(uint32* index, usize slotCount, uint64 hash, usize position) noexcept {
        const usize mask = slotCount - 1;
        usize slot = static_cast<usize>(hash) & mask;
        for (usize visited = 0; visited < slotCount; ++visited) {
            const uint32 stored = index[slot];
            if (stored == EmptySlot || stored == DeletedSlot) {
                index[slot] = static_cast<uint32>(position);
                return stored == DeletedSlot;
            }

            slot = (slot + 1) & mask;
        }

        platform::Fatal("OrderedMap index has no free slot");
    }

    // 새 저장소(항목 + 색인, 할당 한 번)를 잡은 뒤에만 산 항목을 순서대로 옮긴다. 실패하면
    // 그대로다.
    Result<void> Reallocate(usize newCapacity) noexcept {
        auto storage = AllocateStorage(newCapacity);
        if (!storage) {
            return Result<void>::Failure(storage.Error());
        }

        AdoptStorage(storage.Value(), newCapacity);
        return Result<void>::Success();
    }

    // 용량 newCapacity의 저장소를 잡는다(표는 바꾸지 않는다). 오류: Overflow, 할당 오류.
    Result<Entry*> AllocateStorage(usize newCapacity) noexcept {
        usize bytes = 0;
        if (newCapacity > MaximumCapacity || !StorageBytes(newCapacity, bytes)) {
            return Result<Entry*>::Failure(ErrorCode::Overflow);
        }

        auto allocation = allocator_->Allocate(bytes, alignof(Entry));
        if (!allocation) {
            return Result<Entry*>::Failure(allocation.Error());
        }

        Entry* entries = static_cast<Entry*>(allocation.Value());
        if (entries == nullptr || reinterpret_cast<uptr>(entries) % alignof(Entry) != 0) {
            platform::Fatal("OrderedMap allocator returned invalid storage");
        }

        return Result<Entry*>::Success(entries);
    }

    // 잡아 둔 저장소 entries(용량 newCapacity)로 산 항목을 순서대로 옮기고 새 표를 게시한 뒤 옛
    // 저장소를 반환한다(할당 없음, 실패 없음 — 이동된 원소의 소멸자는 allocator를 부르지 않는다).
    void AdoptStorage(Entry* entries, usize newCapacity) noexcept {

        // StorageBytes가 넘침 없이 계산했으므로 아래 곱은 넘치지 않는다.
        unsigned char* indexStorage =
            reinterpret_cast<unsigned char*>(entries) + newCapacity * sizeof(Entry);
        uint32* index = reinterpret_cast<uint32*>(indexStorage);
        const usize slotCount = newCapacity * 2;
        for (usize slot = 0; slot < slotCount; ++slot) {
            ConstructAt<uint32>(index + slot, EmptySlot);
        }

        usize target = 0;
        for (usize position = 0; position < used_; ++position) {
            Entry& old = entries_[position];
            if (!old.alive) {
                continue;
            }

            Entry* moved = ConstructAt<Entry>(entries + target);
            ConstructAt<K>(moved->key, Move(*old.Key()));
            ConstructAt<V>(moved->value, Move(*old.Value()));
            moved->hash = old.hash;
            moved->alive = true;
            old.alive = false;
            DestroyAt(old.Value());
            DestroyAt(old.Key());
            PlaceInIndex(index, slotCount, moved->hash, target);
            ++target;
        }

        Entry* old = entries_;
        entries_ = entries;
        index_ = index;
        capacity_ = newCapacity;
        used_ = target;
        tombstones_ = 0;
        ++revision_;
        if (old != nullptr && !allocator_->Deallocate(old)) {
            platform::Fatal("OrderedMap storage was not owned by its allocator");
        }
    }

    // 산 항목을 상대 순서 그대로 앞으로 당긴다(할당 없음). 옮긴 항목의 색인 칸만 고친다.
    void CompactEntries() noexcept {
        usize target = 0;
        for (usize position = 0; position < used_; ++position) {
            Entry& entry = entries_[position];
            if (!entry.alive) {
                continue;
            }

            if (position != target) {
                const usize slot = SlotOfPosition(entry.hash, position);
                Entry& destination = entries_[target];
                ConstructAt<K>(destination.key, Move(*entry.Key()));
                ConstructAt<V>(destination.value, Move(*entry.Value()));
                destination.hash = entry.hash;
                destination.alive = true;
                entry.alive = false;
                DestroyAt(entry.Value());
                DestroyAt(entry.Key());
                index_[slot] = static_cast<uint32>(target);
            }

            ++target;
        }

        used_ = target;
        ++revision_;
    }

    // 산 항목을 하나씩 논리적으로 지운 뒤(산 표시·개수) 소멸시킨다 — 원소 소멸의 해제 콜백은 아직
    // 남은 항목만 산 것으로 본다.
    void DestroyEntries() noexcept {
        for (usize position = 0; position < used_; ++position) {
            Entry& entry = entries_[position];
            if (entry.alive) {
                entry.alive = false;
                --count_;
                DestroyAt(entry.Value());
                DestroyAt(entry.Key());
            }
        }

        used_ = 0;
        count_ = 0;
    }

    Allocator* allocator_;
    // 항목 배열(capacity_개)이고, 같은 할당 안 그 뒤에 색인(capacity_ * 2칸)이 있다.
    Entry* entries_ = nullptr;
    uint32* index_ = nullptr;
    usize capacity_ = 0;
    usize used_ = 0;
    usize count_ = 0;
    // 색인의 삭제 칸 수다.
    usize tombstones_ = 0;
    uint64 revision_ = 0;
};

} // namespace rocks::detail

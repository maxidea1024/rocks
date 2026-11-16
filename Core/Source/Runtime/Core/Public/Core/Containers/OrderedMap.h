#pragma once

// OrderedMap: 삽입 순서를 보존한다 — 키 정렬이 아니다.
//
// 키·값 쌍을 처음 넣은 순서대로 순회하는 해시 Map이다. 조회·삽입·삭제는 평균 O(1)이고
// (압축은 상환 O(1)), 순회 순서는 해시 값·seed·플랫폼과 무관하게 연산 순서만으로 정해진다.
// 그래서 순회 결과를 그대로 직렬화해도 결정적이다. 리플렉션 Map 필드와 Sparks Map의
// C++ 대응이다(Docs/Design/ReflectionExtensions.ko.md 7.1절).
//
// 순서 규칙:
//   - 새 키는 끝에 붙는다.
//   - 있는 키에 값을 대입하면(InsertOrAssign) 자리를 지킨다.
//   - 지운 뒤 다시 넣은 키는 끝에 붙는다.
//   - 같음(operator==)은 순서까지 비교한다. 같은 쌍이라도 순서가 다르면 다르다.
//
// 구조(Core/Containers/OrderedEntryTable.h): 삽입 순서의 조밀 항목 배열 + open addressing
// 색인(u32 항목 위치)을 할당 한 번으로 잡는다. 삭제는 항목을 죽은 것으로 표시하고 색인 칸을
// 삭제 칸으로 바꾼다. 빈 항목 자리는 다시 쓰지 않는다. 끝의 죽은 항목은 바로 떼고, 죽은
// 항목이 산 항목의 절반을 넘으면 그 Erase가 제자리에서 앞으로 당긴다(할당 없음). 가득 찬
// 저장소에 넣을 때는 제자리 압축이나 두 배 성장을 한다. 모든 압축은 상대 순서를 지킨다.
//
// 사용 예:
//   OrderedMap<int32, String> names(allocator);
//   auto inserted = names.Insert(int32{7}, Move(name));
//   if (!inserted) {
//       return inserted.Error();
//   }
//   for (auto [key, value] : names) { ... }  // 넣은 순서대로
//
// 계약(HashMap과 같은 규약 — Docs/Implementation/HashCollections.ko.md):
//   - 같은 컨테이너의 동시 접근은 caller가 동기화한다. const 순회도 내부 상태를 바꾼다.
//   - allocator는 컨테이너와 모든 사용보다 오래 살아야 한다.
//   - 해시·동등 비교는 noexcept이고 같은 키에 안정적이며, 동등 키는 해시가 같아야 한다.
//     키를 복사·이동한 뒤에도 원본과 동등하고 해시가 같아야 한다.
//   - 키·값 생성·이동·소멸, 해시·동등 비교, allocator 콜백에서 같은 Map에 재진입(읽기
//     포함)하지 않는다.
//   - 실패한 Insert·InsertOrAssign·FindOrAdd*·Reserve·Compact는 내용·순서·용량·포인터·
//     반복자를 보존하고 입력 인자를 소비하지 않는다.
//   - 신규 삽입과 값 교체는 저장소 원소의 직접 별칭을 거부한다(InvalidArgument).
//     key/value 인자끼리 겹칠 때는 양쪽 생성이 trivial이어야 한다.
//
// 포인터 무효화(Find·FindOrAdd가 돌려준 포인터, 반복자가 가리키는 원소):
//   - 성장(삽입·Reserve), 압축(Erase·EraseIf·가득 찬 저장소에 삽입·Reserve·Compact),
//     Clear, Reset, 이동, 파괴는 모든 포인터를 무효화한다. 안전하게는 Erase 뒤 모든
//     포인터를 무효로 본다.
//   - Reserve(n)이 성공한 뒤 Erase·EraseIf·Compact·Reset 없이 Count()가 n이 될 때까지의
//     삽입은 할당·이동하지 않으므로 기존 포인터가 유효하다.
//   - InsertOrAssign의 기존 값 교체는 그 값 포인터만 무효화한다(자리는 그대로).
//
// 반복자 무효화: 원소 집합이나 저장소가 바뀌는 연산(삽입·삭제·Clear·Reserve·Compact·
// Reset·이동) 뒤 기존 반복자의 역참조·증가는 Fatal이다. 값 교체는 반복자를 무효화하지
// 않는다. ForEach·EraseIf predicate·FindOrAddWith factory 안의 구조 변경은 삽입·예약·
// 압축·EraseIf가 Busy, 삭제·Clear·Reset·이동이 Fatal이다(HashMap과 같다).
//
// 해시(hash flooding 방어): 기본 정책은 SeededHash<K>(Core/Hash.h — seed를 넣은 SipHash-2-4,
// 기본 seed는 프로세스마다 OS 난수)다. 순서가 해시와 무관하므로 seed가 바뀌어도 순회·직렬화·
// 같음이 그대로이고, 파일에서 온 키로도 상대가 색인 칸 충돌을 미리 고를 수 없다. 시험·재현은
// SeededHash<K>(HashSeed{...})를 넘긴다. 다른 해시 정책을 넘기면 이 보장은 그 정책의 몫이다.

#include <Core/Containers/InsertOutcome.h>
#include <Core/Containers/OrderedEntryTable.h>
#include <Core/Hash.h>

namespace rocks {

template <typename K, typename V, typename Hasher = SeededHash<K>, typename KeyEqual = Equal<K>>
class OrderedMap {
    static_assert(IsNothrowMoveConstructibleV<K> && IsNothrowDestructibleV<K>,
                  "OrderedMap key must be nothrow move constructible and nothrow destructible");
    static_assert(IsNothrowMoveConstructibleV<V> && IsNothrowDestructibleV<V>,
                  "OrderedMap value must be nothrow move constructible and nothrow destructible");
    static_assert(IsNothrowMoveConstructibleV<Hasher> && IsNothrowDestructibleV<Hasher>,
                  "OrderedMap hasher must be nothrow move constructible and nothrow destructible");
    static_assert(
        IsNothrowMoveConstructibleV<KeyEqual> && IsNothrowDestructibleV<KeyEqual>,
        "OrderedMap key equality must be nothrow move constructible and nothrow destructible");
    static_assert(__is_nothrow_assignable(Hasher&, Hasher&&),
                  "OrderedMap hasher must be nothrow move assignable");
    static_assert(__is_nothrow_assignable(KeyEqual&, KeyEqual&&),
                  "OrderedMap key equality must be nothrow move assignable");

    using Table = detail::OrderedEntryTable<K, V>;
    using Entry = typename Table::Entry;

public:
    // 담을 수 있는 최대 원소 수(2^31)다. 넘는 Reserve·삽입은 Overflow다.
    static constexpr usize MaximumCapacity = Table::MaximumCapacity;

    // 빈(또는 Clear한) Map의 Reserve(count)가 새로 잡는 저장소 byte의 상한을 bytes에 쓴다: count가
    // 0이면 0, 아니면 용량을 2의 거듭제곱(최소 4)으로 올린 항목 배열 + 색인(용량 × 2 × u32)이다 —
    // 빈 Map에서는 그 할당 한 번과 정확히 같다. count가 MaximumCapacity를 넘거나 byte가 넘치면
    // false다. 원소 자체가 따로 할당하는 메모리(String 버퍼 등)는 들지 않는다. 미리 작업량을
    // 청구하는 곳(리플렉션 Map 복원의 2단계 몫)이 쓴다.
    //
    // 사용 예:
    //   usize bytes = 0;
    //   if (!OrderedMap<uint32, int32>::ReservedBytes(count, bytes)) { return Overflow; }
    static constexpr bool ReservedBytes(usize count, usize& bytes) noexcept {
        return Table::ReservedBytes(count, bytes);
    }

    explicit OrderedMap(Allocator& allocator, Hasher hasher = {}, KeyEqual equal = {}) noexcept
        : hasher_(Move(hasher)), equal_(Move(equal)), table_(allocator) {
    }

    OrderedMap(const OrderedMap&) = delete;
    OrderedMap& operator=(const OrderedMap&) = delete;

    // allocator·정책·저장소를 함께 옮긴다. 원본은 같은 allocator를 쓰는 빈 Map이 되어
    // 다시 쓸 수 있다. 원본을 순회하는 중이면 Fatal이다.
    OrderedMap(OrderedMap&& other) noexcept
        : hasher_(Move(other.hasher_)), equal_(Move(other.equal_)),
          table_(other.table_.GetAllocator()) {
        other.CheckNotIterating();
        table_.TakeFrom(other.table_);
    }

    OrderedMap& operator=(OrderedMap&& other) noexcept {
        CheckNotIterating();
        other.CheckNotIterating();
        if (this != &other) {
            hasher_ = Move(other.hasher_);
            equal_ = Move(other.equal_);
            table_.TakeFrom(other.table_);
        }

        return *this;
    }

    ~OrderedMap() noexcept {
        CheckNotIterating();
    }

    // 산 원소 수다.
    usize Count() const noexcept {
        return table_.Count();
    }

    // 지금 저장소의 항목 수다. 삭제가 없으면 Count()가 이 값에 이를 때까지 성장하지 않는다.
    // 삭제가 섞이면 죽은 항목이 끝자리를 차지해, 죽은 항목이 용량의 1/8 미만일 때는
    // Count() < Capacity()여도 삽입이 두 배로 성장할 수 있다. 성장 없음이 필요하면 Reserve로
    // 보장받는다.
    usize Capacity() const noexcept {
        return table_.Capacity();
    }

    bool IsEmpty() const noexcept {
        return table_.Count() == 0;
    }

    // 저장소를 할당하는 allocator다(생성자에 준 것 — 이동하면 함께 옮겨 간다). 원소가 allocator를
    // 받는 값(String·리플렉션 struct)이면 같은 allocator로 만들어 넣는다(리플렉션 Map 복원 —
    // Array::GetAllocator와 같은 용도).
    Allocator& GetAllocator() const noexcept {
        return table_.GetAllocator();
    }

    // 값의 빌린 포인터를 돌려주며 없으면 nullptr다. 호출 예: if (auto* p = map.Find(key)).
    // 조회 키 Q가 정책의 hash(Q)·equal(K, Q)와 호환되면 이종 조회도 된다(String 키를
    // StringView로). 포인터 수명은 머리 주석의 무효화 규칙을 따른다.
    template <typename Q>
    V* Find(const Q& key) noexcept {
        const usize position = FindPosition(key, HashKey(key));
        return position == IndexNone ? nullptr : table_.At(position).Value();
    }

    template <typename Q>
    const V* Find(const Q& key) const noexcept {
        const usize position = FindPosition(key, HashKey(key));
        return position == IndexNone ? nullptr : table_.At(position).Value();
    }

    template <typename Q>
    bool Contains(const Q& key) const noexcept {
        return FindPosition(key, HashKey(key)) != IndexNone;
    }

    // 저장된 키를 읽기 전용으로 빌린다. 값 포인터와 같은 수명 규칙이다.
    template <typename Q>
    const K* FindStoredKey(const Q& key) const noexcept {
        const usize position = FindPosition(key, HashKey(key));
        return position == IndexNone ? nullptr : table_.At(position).Key();
    }

    // 새 키를 끝에 붙인다. KeyArg는 정확히 K여야 한다(이종 키는 조회 전용). 같은 키가
    // 있으면 값도 순서도 바꾸지 않고 AlreadyPresent다. 실패(OutOfMemory·Overflow·Busy·
    // InvalidArgument)나 중복이면 입력을 이동하지 않고 Map도 그대로다.
    template <typename KeyArg, typename ValueArg>
    Result<InsertOutcome> Insert(KeyArg&& key, ValueArg&& value) noexcept {
        return InsertImpl<false>(Forward<KeyArg>(key), Forward<ValueArg>(value));
    }

    // 새 키는 끝에 붙이고, 있는 키는 자리를 지킨 채 값을 새 값으로 다시 만든다
    // (AlreadyPresent). Sparks `m[k] = v`의 의미다. 성공한 교체는 입력 값을 소비하고 그 값
    // 포인터를 무효화한다.
    template <typename KeyArg, typename ValueArg>
    Result<InsertOutcome> InsertOrAssign(KeyArg&& key, ValueArg&& value) noexcept {
        return InsertImpl<true>(Forward<KeyArg>(key), Forward<ValueArg>(value));
    }

    // 있는 값이면 입력을 소비하지 않고 그 포인터를, 없으면 끝에 붙인 새 값의 포인터를
    // 돌려준다. 실패하면 입력과 Map이 그대로다.
    template <typename KeyArg, typename ValueArg>
    Result<FindOrAddResult<V>> FindOrAdd(KeyArg&& key, ValueArg&& value) noexcept {
        V* found = nullptr;
        auto result = InsertImpl<false>(Forward<KeyArg>(key), Forward<ValueArg>(value), &found);
        if (!result) {
            return Result<FindOrAddResult<V>>::Failure(result.Error());
        }

        return Result<FindOrAddResult<V>>::Success({found, result.Value()});
    }

    // 키가 없을 때만 noexcept 기본 생성자로 값을 만든다. allocator가 필요한 값은
    // FindOrAddWith를 쓴다.
    template <typename KeyArg>
    Result<FindOrAddResult<V>> FindOrAdd(KeyArg&& key) noexcept {
        static_assert(__is_nothrow_constructible(V),
                      "OrderedMap::FindOrAdd(key) requires a nothrow default constructible value");
        return FindOrAddWith(Forward<KeyArg>(key),
                             []() noexcept { return Result<V>::Success(V{}); });
    }

    // factory는 키가 없을 때만 한 번 호출하며 noexcept Result<V>를 돌려줘야 한다. factory
    // 실패나 그 뒤 저장소 할당 실패는 Map·키 인자를 보존한다(factory의 외부 부작용은 되돌리지
    // 않는다). factory 실행과 만든 임시 값의 소멸 동안 같은 Map의 구조 변경은 막힌다.
    // 중첩 조회는 된다.
    //
    // 사용 예:
    //   auto name = names.FindOrAddWith(int32{42}, [&]() noexcept -> Result<String> { ... });
    template <typename KeyArg, typename Factory>
    Result<FindOrAddResult<V>> FindOrAddWith(KeyArg&& key, Factory&& factory) noexcept {
        static_assert(
            IsSameV<RemoveConstT<RemoveReferenceT<KeyArg>>, K>,
            "OrderedMap insertion key must be exactly K; heterogeneous keys are lookup only");
        static_assert(__is_nothrow_constructible(K, KeyArg&&),
                      "OrderedMap key must be nothrow constructible from the argument");
        static_assert(noexcept(factory()), "OrderedMap factory must be noexcept");
        static_assert(IsSameV<decltype(factory()), Result<V>>,
                      "OrderedMap factory must return Result<V>");

        if (iterationDepth_ != 0) {
            return Result<FindOrAddResult<V>>::Failure(ErrorCode::Busy);
        }

        const usize existing = FindPosition(key, HashKey(key));
        if (existing != IndexNone) {
            return Result<FindOrAddResult<V>>::Success(
                {table_.At(existing).Value(), InsertOutcome::AlreadyPresent});
        }

        if (AliasesStorage(key)) {
            return Result<FindOrAddResult<V>>::Failure(ErrorCode::InvalidArgument);
        }

        // 만든 값의 소멸까지 가드를 유지한다. 실제 삽입 동안에만 잠시 푼다.
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

    // capacity개 원소를 성장 없이 담게 한다(단위는 원소 수). 성공하면 Erase·EraseIf·Compact·
    // Reset 없이 Count()가 capacity가 될 때까지의 삽입은 할당·이동하지 않는다. 용량이 모자라면
    // 새 저장소를 잡는다. 용량은 되는데 죽은 항목 때문에 끝자리가 모자라면, 죽은 항목이 용량의
    // 1/8 이상이거나 원소가 용량의 절반 미만일 때 제자리 압축(할당 없음)을, 아니면 두 배로
    // 성장한다(상환 O(1) — "삭제 + Reserve(Count() + 1) + 삽입" 반복도). 매번 남은 용량 전부를
    // 요구하는 Reserve(Capacity()) 반복은 매번 O(용량)이다. 성장·압축 모두 포인터를 무효화한다.
    // 오류: Busy(순회 중), Overflow(MaximumCapacity 초과), 할당 오류. 실패하면 그대로다.
    Result<void> Reserve(usize capacity) noexcept {
        if (iterationDepth_ != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return table_.Reserve(capacity);
    }

    // 죽은 항목과 삭제 칸을 정리하고 현재 원소 수에 맞는 최소 용량으로 줄인다. 빈 Map은
    // 저장소를 반환한다. 할당 실패면 내용·순서·용량·포인터를 보존한다. 순회 중이면 Busy다.
    Result<void> Compact() noexcept {
        if (iterationDepth_ != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return table_.Compact();
    }

    // 키가 있으면 지우고 true, 없으면 false다. 남은 원소의 순서는 그대로다. 죽은 항목이 산
    // 항목의 절반을 넘으면 제자리 압축이 일어나 모든 포인터가 무효다. 순회 중 호출은
    // Fatal이다.
    template <typename Q>
    bool Erase(const Q& key) noexcept {
        CheckNotIterating();
        const usize slot = FindSlot(key, HashKey(key));
        if (slot == IndexNone) {
            return false;
        }

        table_.EraseSlot(slot);
        table_.SettleAfterErase();
        return true;
    }

    // predicate가 true인 원소를 지우고 지운 수를 돌려준다. 남은 원소의 순서는 그대로다.
    // predicate는 noexcept이고 정확히 bool을 돌려줘야 하며, 삽입 순서로 불린다. 그 안에서
    // 중첩 조회는 되지만 구조 변경은 안 된다. 다른 순회 중 호출은 Busy다. Erase처럼 압축이
    // 일어날 수 있다.
    template <typename Predicate>
    Result<usize> EraseIf(Predicate&& predicate) noexcept {
        static_assert(
            noexcept(predicate(*static_cast<const K*>(nullptr), *static_cast<const V*>(nullptr))),
            "OrderedMap EraseIf predicate must be noexcept");
        static_assert(IsSameV<decltype(predicate(*static_cast<const K*>(nullptr),
                                                 *static_cast<const V*>(nullptr))),
                              bool>,
                      "OrderedMap EraseIf predicate must return bool");
        if (iterationDepth_ != 0) {
            return Result<usize>::Failure(ErrorCode::Busy);
        }

        // 처음 EagerEraseLimit()개는 색인 칸을 하나씩 지우고, 그 뒤는 항목만 죽은 것으로 표시한 뒤
        // 끝에 색인을 한 번 다시 짓는다. 모두 충돌하는 해시에서도 대량 삭제가 O(n^2)이 되지 않는다.
        usize removed = 0;
        const usize eagerLimit = table_.EagerEraseLimit();
        bool rebuild = false;
        EnterIteration();
        for (usize position = 0; position < table_.Used(); ++position) {
            Entry& entry = table_.At(position);
            if (!entry.alive || !predicate(*static_cast<const K*>(entry.Key()),
                                           *static_cast<const V*>(entry.Value()))) {
                continue;
            }

            if (removed < eagerLimit) {
                table_.EraseAt(position);
            } else {
                table_.MarkDeadKeepingSlot(position);
                rebuild = true;
            }

            ++removed;
        }

        --iterationDepth_;
        if (rebuild) {
            table_.RebuildIndex();
        }

        if (removed != 0) {
            table_.SettleAfterErase();
        }

        return Result<usize>::Success(removed);
    }

    // 모든 원소를 지우고 용량은 유지한다. 순회 중 호출은 Fatal이다.
    void Clear() noexcept {
        CheckNotIterating();
        table_.Clear();
    }

    // 모든 원소와 저장소를 반환한다. 같은 allocator로 다시 쓸 수 있다. 순회 중 호출은
    // Fatal이다.
    void Reset() noexcept {
        CheckNotIterating();
        table_.Release();
    }

    // 원소를 삽입 순서로 방문한다. 콜백은 noexcept다. 순회 중 중첩 조회·순회와 값 수정은
    // 되지만 구조 변경은 안 된다(삽입·예약·압축은 Busy, 삭제·Clear·Reset은 Fatal).
    template <typename Visitor>
    void ForEach(Visitor&& visitor) noexcept {
        static_assert(noexcept(visitor(*static_cast<const K*>(nullptr), *static_cast<V*>(nullptr))),
                      "OrderedMap ForEach visitor must be noexcept");
        EnterIteration();
        for (usize position = 0; position < table_.Used(); ++position) {
            Entry& entry = table_.At(position);
            if (entry.alive) {
                visitor(*static_cast<const K*>(entry.Key()), *entry.Value());
            }
        }

        --iterationDepth_;
    }

    template <typename Visitor>
    void ForEach(Visitor&& visitor) const noexcept {
        static_assert(
            noexcept(visitor(*static_cast<const K*>(nullptr), *static_cast<const V*>(nullptr))),
            "OrderedMap ForEach visitor must be noexcept");
        EnterIteration();
        for (usize position = 0; position < table_.Used(); ++position) {
            const Entry& entry = table_.At(position);
            if (entry.alive) {
                visitor(*entry.Key(), *entry.Value());
            }
        }

        --iterationDepth_;
    }

    // 두 Map이 같은 키·값 쌍을 같은 순서로 담았는지 본다. 키는 이 Map의 KeyEqual로, 값은
    // valueEqual(noexcept bool(const V&, const V&))로 비교한다. 비교 중 두 Map의 구조 변경은
    // 막힌다.
    template <typename ValueEqual>
    bool SequenceEquals(const OrderedMap& other, ValueEqual&& valueEqual) const noexcept {
        static_assert(
            noexcept(valueEqual(*static_cast<const V*>(nullptr), *static_cast<const V*>(nullptr))),
            "OrderedMap value equality must be noexcept");
        static_assert(IsSameV<decltype(valueEqual(*static_cast<const V*>(nullptr),
                                                  *static_cast<const V*>(nullptr))),
                              bool>,
                      "OrderedMap value equality must return bool");
        if (this == &other) {
            return true;
        }

        if (Count() != other.Count()) {
            return false;
        }

        IterationGuard leftGuard(*this);
        IterationGuard rightGuard(other);
        usize right = other.table_.NextAlive(0);
        for (usize left = table_.NextAlive(0); left < table_.Used();
             left = table_.NextAlive(left + 1)) {
            const Entry& leftEntry = table_.At(left);
            const Entry& rightEntry = other.table_.At(right);
            if (!KeysEqual(*leftEntry.Key(), *rightEntry.Key()) ||
                !valueEqual(*leftEntry.Value(), *rightEntry.Value())) {
                return false;
            }

            right = other.table_.NextAlive(right + 1);
        }

        return true;
    }

    // 순서를 포함한 같음이다(left의 KeyEqual, 값은 Equal<V>). 같은 쌍이라도 순서가 다르면
    // 거짓이다.
    friend bool operator==(const OrderedMap& left, const OrderedMap& right) noexcept {
        return left.SequenceEquals(right, Equal<V>{});
    }

    // ---- STL 호환 반복자 ----
    // 원소를 {key, value} 참조 쌍으로 삽입 순서대로 방문하는 전방 반복자다.
    //   for (auto [key, value] : map) { value += 1; }
    // 역참조 결과는 값으로 반환되는 참조 쌍(KeyValue)이므로 auto 또는 const auto&로 받는다.
    // key는 바꿀 수 없고 value는 비 const 순회에서 바꿀 수 있다. 원소 집합·저장소가 바뀐 뒤
    // 기존 반복자의 역참조·증가는 Fatal이다. 같은 키의 값 교체는 반복자를 무효화하지 않는다.
    // ForEach와 달리 구조 변경을 막지 않고 사후에 탐지한다. 컨테이너 소멸 후 사용은 탐지하지
    // 못한다.
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
            auto& entry = CheckedEntry();
            return KeyValue<ValueType>{*entry.Key(), *entry.Value()};
        }

        Arrow operator->() const noexcept {
            return Arrow{**this};
        }

        BasicIterator& operator++() noexcept {
            CheckedEntry();
            position_ = map_->table_.NextAlive(position_ + 1);
            return *this;
        }

        BasicIterator operator++(int) noexcept {
            BasicIterator previous = *this;
            ++*this;
            return previous;
        }

        friend bool operator==(const BasicIterator& left, const BasicIterator& right) noexcept {
            return left.map_ == right.map_ && left.position_ == right.position_;
        }

    private:
        friend class OrderedMap;

        BasicIterator(MapType* map, usize position) noexcept
            : map_(map), position_(position), revision_(map->table_.Revision()) {
        }

        decltype(auto) CheckedEntry() const noexcept {
            if (map_ == nullptr || map_->table_.Revision() != revision_) {
                platform::Fatal("OrderedMap iterator used after structural modification");
            }

            if (position_ >= map_->table_.Used() || !map_->table_.At(position_).alive) {
                platform::Fatal("OrderedMap iterator is not dereferenceable");
            }

            return (map_->table_.At(position_));
        }

        MapType* map_ = nullptr;
        usize position_ = 0;
        uint64 revision_ = 0;
    };

    using key_type = K;
    using mapped_type = V;
    using size_type = usize;
    using difference_type = isize;
    using iterator = BasicIterator<OrderedMap, V>;
    using const_iterator = BasicIterator<const OrderedMap, const V>;
    using value_type = KeyValue<V>;

    iterator begin() noexcept {
        return iterator(this, table_.NextAlive(0));
    }

    iterator end() noexcept {
        return iterator(this, table_.Used());
    }

    const_iterator begin() const noexcept {
        return const_iterator(this, table_.NextAlive(0));
    }

    const_iterator end() const noexcept {
        return const_iterator(this, table_.Used());
    }

    const_iterator cbegin() const noexcept {
        return begin();
    }

    const_iterator cend() const noexcept {
        return end();
    }

private:
    struct IterationGuard {
        explicit IterationGuard(const OrderedMap& map) noexcept : map_(map) {
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

        const OrderedMap& map_;
        bool active_ = true;
    };

    template <typename Q>
    uint64 HashKey(const Q& key) const noexcept {
        static_assert(noexcept(static_cast<uint64>(hasher_(key))),
                      "OrderedMap hasher must be noexcept");
        return static_cast<uint64>(hasher_(key));
    }

    template <typename Q>
    bool KeysEqual(const K& left, const Q& right) const noexcept {
        static_assert(noexcept(static_cast<bool>(equal_(left, right))),
                      "OrderedMap key equality must be noexcept");
        return static_cast<bool>(equal_(left, right));
    }

    template <typename Q>
    usize FindSlot(const Q& key, uint64 hash) const noexcept {
        return table_.FindSlot(hash,
                               [&](const K& stored) noexcept { return KeysEqual(stored, key); });
    }

    template <typename Q>
    usize FindPosition(const Q& key, uint64 hash) const noexcept {
        const usize slot = FindSlot(key, hash);
        return slot == IndexNone ? IndexNone : table_.PositionOfSlot(slot);
    }

    template <typename A>
    bool AliasesStorage(A& arg) const noexcept {
        return table_.Aliases(reinterpret_cast<uptr>(platform::AddressOf(arg)), sizeof(A));
    }

    template <typename A, typename B>
    static bool ArgumentsOverlap(A& a, B& b) noexcept {
        return detail::AddressRangesOverlap(
            reinterpret_cast<uptr>(platform::AddressOf(a)), sizeof(A),
            reinterpret_cast<uptr>(platform::AddressOf(b)), sizeof(B));
    }

    template <bool Assign, typename KeyArg, typename ValueArg>
    Result<InsertOutcome> InsertImpl(KeyArg&& key, ValueArg&& value,
                                     V** resultValue = nullptr) noexcept {
        static_assert(
            IsSameV<RemoveConstT<RemoveReferenceT<KeyArg>>, K>,
            "OrderedMap insertion key must be exactly K; heterogeneous keys are lookup only");
        static_assert(__is_nothrow_constructible(K, KeyArg&&),
                      "OrderedMap key must be nothrow constructible from the argument");
        static_assert(__is_nothrow_constructible(V, ValueArg&&),
                      "OrderedMap value must be nothrow constructible from the argument");
        if (iterationDepth_ != 0) {
            return Result<InsertOutcome>::Failure(ErrorCode::Busy);
        }

        // 조회와 중복 확인은 입력을 이동하지 않는다.
        const K& lookup = key;
        const uint64 hash = HashKey(lookup);
        const usize existing = FindPosition(lookup, hash);
        constexpr bool safeSharedInput = __is_trivially_constructible(K, KeyArg&&) &&
                                         __is_trivially_constructible(V, ValueArg&&);
        const bool aliased = AliasesStorage(key) || AliasesStorage(value) ||
                             (!safeSharedInput && ArgumentsOverlap(key, value));
        if (existing != IndexNone) {
            Entry& entry = table_.At(existing);
            if constexpr (Assign) {
                if (aliased) {
                    return Result<InsertOutcome>::Failure(ErrorCode::InvalidArgument);
                }

                DestroyAt(entry.Value());
                ConstructAt<V>(entry.value, Forward<ValueArg>(value));
            }

            if (resultValue != nullptr) {
                *resultValue = entry.Value();
            }

            return Result<InsertOutcome>::Success(InsertOutcome::AlreadyPresent);
        }

        if (aliased) {
            return Result<InsertOutcome>::Failure(ErrorCode::InvalidArgument);
        }

        // 실패할 수 있는 단계(성장 할당)는 이 한 곳이고, 실패하면 아무것도 바꾸지 않았다.
        auto room = table_.EnsureAppendRoom();
        if (!room) {
            return Result<InsertOutcome>::Failure(room.Error());
        }

        Entry& entry = table_.Append(hash, Forward<KeyArg>(key), Forward<ValueArg>(value));
        if (resultValue != nullptr) {
            *resultValue = entry.Value();
        }

        return Result<InsertOutcome>::Success(InsertOutcome::Inserted);
    }

    void CheckNotIterating() const noexcept {
        if (iterationDepth_ != 0) {
            platform::Fatal("OrderedMap structural mutation during iteration");
        }
    }

    void EnterIteration() const noexcept {
        if (iterationDepth_ == MaxSize) {
            platform::Fatal("OrderedMap iteration depth overflow");
        }

        ++iterationDepth_;
    }

    Hasher hasher_;
    KeyEqual equal_;
    Table table_;
    mutable usize iterationDepth_ = 0;
};

} // namespace rocks

#include <Base/Types.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Optional.h>
#include <Core/Text/String.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

// OrderedMap·OrderedSet의 저장소 반환 순서와 2단계 예약이다(리플렉션 B9f3 리뷰 반영 — Docs/Design/
// ReflectionExtensions.ko.md 7.3절): 옛 저장소는 새 저장소를 게시한 뒤 반환하고(그 해제 콜백은
// 일관된 Map을 본다), PrepareReserve는 할당만 하고 Map을 바꾸지 않으며(버리면 잡은 저장소만 반환),
// CommitReserve는 할당·해제·실패 없이 적용한다(옛 저장소는 예약이 소멸할 때 반환 — 리플렉션 B9f4).
// IsIterating은 ForEach 중에만 참이다. EraseAndTake는 지운 항목의 키·값을 소멸 전에 넘긴다.
using namespace rocks;

namespace {

// 지켜보는 주소의 반환 직전에 hook을 한 번 실행하고, 할당·반환 수를 세고, 지정한 번째 할당을
// 실패시킨다.
class WatchAllocator final : public Allocator {
public:
    explicit WatchAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        ++allocations;
        if (failAt != 0 && allocations == failAt) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        ++deallocations;
        if (hook != nullptr && address == watched) {
            auto run = hook;
            hook = nullptr;
            run(context);
        }

        return backing_.Deallocate(address);
    }

    void Watch(const void* address, void (*run)(void*) noexcept, void* argument) noexcept {
        watched = address;
        hook = run;
        context = argument;
    }

    uint64 allocations = 0;
    uint64 deallocations = 0;
    uint64 failAt = 0;

private:
    Allocator& backing_;
    const void* watched = nullptr;
    void (*hook)(void*) noexcept = nullptr;
    void* context = nullptr;
};

using Map = OrderedMap<int32, int32>;

// 해제 콜백이 본 Map의 상태다.
struct Seen {
    const Map* map = nullptr;
    usize count = NumericLimits<usize>::Max();
    bool allFound = false;
    bool valuesMatch = false;
    bool called = false;
};

void Observe(void* argument) noexcept {
    Seen& seen = *static_cast<Seen*>(argument);
    seen.called = true;
    seen.count = seen.map->Count();
    seen.allFound = true;
    seen.valuesMatch = true;
    for (int32 key = 0; key < static_cast<int32>(seen.count); ++key) {
        const int32* value = seen.map->Find(key);
        seen.allFound = seen.allFound && value != nullptr && seen.map->Contains(key);
        seen.valuesMatch = seen.valuesMatch && value != nullptr && *value == key * 10;
    }
}

// 저장소의 시작 주소다(항목 배열 — 첫 항목의 키가 그 맨 앞이다).
const void* StorageOf(const Map& map) noexcept {
    for (auto entry : map) {
        return &entry.key;
    }

    return nullptr;
}

} // namespace

// 성장(Reallocate)·Reset·Compact와 CommitReserve로 적용한 예약의 소멸이 옛 저장소를 반환하는 순간
// 그 해제 콜백은 게시된 Map을 본다: 성장이면 옛 원소가 모두 새 저장소에 있고(Count·Find·Contains
// 일관), Reset이면 빈 Map이다. CommitReserve 자체는 반환하지 않는다(리플렉션 B9f4).
TEST(OrderedMap_ReleaseCallbacksSeeThePublishedTable) {
    SystemAllocator system;
    WatchAllocator allocator(system);
    Map map(allocator);
    for (int32 key = 0; key < 4; ++key) {
        REQUIRE(map.Insert(key, key * 10));
    }

    REQUIRE(map.Capacity() == 4);
    Seen grown;
    grown.map = &map;
    allocator.Watch(StorageOf(map), &Observe, &grown);
    REQUIRE(map.Insert(int32{4}, int32{40}));
    CHECK(grown.called && grown.count == 4 && grown.allFound && grown.valuesMatch);
    CHECK(map.Capacity() == 8 && map.Count() == 5);

    // CommitReserve는 옛 저장소를 예약에 넘긴다: 적용 뒤 넣은 원소까지 게시된 Map을 예약 소멸 때의
    // 해제 콜백이 본다.
    Seen reserved;
    reserved.map = &map;
    allocator.Watch(StorageOf(map), &Observe, &reserved);
    {
        auto prepared = map.PrepareReserve(9);
        REQUIRE(prepared);
        map.CommitReserve(prepared.Value());
        CHECK(!reserved.called);
        REQUIRE(map.Insert(int32{5}, int32{50}));
        CHECK(!reserved.called);
    }

    CHECK(reserved.called && reserved.count == 6 && reserved.allFound && reserved.valuesMatch);
    REQUIRE(map.Erase(int32{5}));

    Seen compacted;
    compacted.map = &map;
    for (int32 key = 1; key < 5; ++key) {
        REQUIRE(map.Erase(key));
    }

    allocator.Watch(StorageOf(map), &Observe, &compacted);
    REQUIRE(map.Compact());
    CHECK(compacted.called && compacted.count == 1 && compacted.allFound);

    Seen released;
    released.map = &map;
    allocator.Watch(StorageOf(map), &Observe, &released);
    map.Reset();
    CHECK(released.called && released.count == 0 && map.Capacity() == 0);
}

// PrepareReserve는 새 저장소만 잡고 Map(내용·용량·포인터)을 바꾸지 않는다. 버리면 그 저장소만
// 반환하고, CommitReserve는 할당 없이 적용해 그 뒤 Count()가 예약 수에 닿을 때까지의 삽입이
// 할당하지 않는다. 압축으로 끝자리가 생기면 할당이 없다. 준비 실패는 OutOfMemory이고 Map은
// 그대로다. 순회 중에는 Busy이고 IsIterating이 참이다. OrderedSet도 같다.
TEST(OrderedMap_PrepareReserveSplitsAllocationFromCommit) {
    SystemAllocator system;
    WatchAllocator allocator(system);
    Map map(allocator);
    for (int32 key = 0; key < 4; ++key) {
        REQUIRE(map.Insert(key, key * 10));
    }

    const void* storage = StorageOf(map);
    const uint64 allocations = allocator.allocations;
    const uint64 deallocations = allocator.deallocations;
    {
        auto prepared = map.PrepareReserve(5);
        REQUIRE(prepared);
        CHECK(allocator.allocations == allocations + 1);
        CHECK(map.Capacity() == 4 && map.Count() == 4 && StorageOf(map) == storage);
    }

    CHECK(allocator.deallocations == deallocations + 1);
    CHECK(map.Capacity() == 4 && StorageOf(map) == storage && *map.Find(int32{3}) == 30);

    allocator.failAt = allocator.allocations + 1;
    auto failed = map.PrepareReserve(5);
    CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
    CHECK(map.Capacity() == 4 && StorageOf(map) == storage);
    allocator.failAt = 0;

    auto prepared = map.PrepareReserve(8);
    REQUIRE(prepared);
    map.CommitReserve(prepared.Value());
    CHECK(map.Capacity() == 8 && map.Count() == 4 && *map.Find(int32{2}) == 20);
    const uint64 committed = allocator.allocations;
    for (int32 key = 4; key < 8; ++key) {
        REQUIRE(map.Insert(key, key * 10));
    }

    CHECK(allocator.allocations == committed);

    // 가득 찬 저장소의 죽은 항목이 용량의 1/8 이상이면 할당 없는 압축으로 끝자리를 만든다.
    REQUIRE(map.Erase(int32{2}));
    REQUIRE(map.Erase(int32{3}));
    REQUIRE(map.Capacity() == 8 && map.Count() == 6);
    const uint64 beforeCompact = allocator.allocations;
    auto compact = map.PrepareReserve(map.Count() + 1);
    REQUIRE(compact);
    CHECK(allocator.allocations == beforeCompact);
    map.CommitReserve(compact.Value());
    REQUIRE(map.Insert(int32{8}, int32{80}));
    CHECK(allocator.allocations == beforeCompact && map.Capacity() == 8);
    const int32 order[] = {0, 1, 4, 5, 6, 7, 8};
    usize position = 0;
    for (auto entry : map) {
        REQUIRE(position < 7);
        CHECK(entry.key == order[position++]);
    }

    bool iterating = false;
    ErrorCode busy = ErrorCode::None;
    CHECK(!map.IsIterating());
    map.ForEach([&](const int32&, int32&) noexcept {
        iterating = map.IsIterating();
        auto inside = map.PrepareReserve(64);
        busy = inside ? ErrorCode::None : inside.Error();
    });
    CHECK(iterating && busy == ErrorCode::Busy && !map.IsIterating());

    OrderedSet<int32> set(allocator);
    REQUIRE(set.Add(int32{1}));
    auto setPrepared = set.PrepareReserve(16);
    REQUIRE(setPrepared);
    set.CommitReserve(setPrepared.Value());
    CHECK(set.Capacity() == 16 && set.Contains(int32{1}) && !set.IsIterating());
}

// EraseAndTake(리플렉션 B9f4)는 항목을 논리적으로 지운 뒤 키·값을 taker로 넘긴다: taker 안에서
// Map은 이미 그 키가 없고(Count·Contains), 키 버퍼는 taker가 가져간 사본이 소멸할 때 반환된다(Map
// 안의 껍데기 소멸은 반환하지 않는다). 없는 키는 taker를 부르지 않고 false다. 압축이 일어나도 남은
// 순서는 그대로다.
TEST(OrderedMap_EraseAndTakeHandsOverTheErasedEntry) {
    SystemAllocator system;
    WatchAllocator allocator(system);
    OrderedMap<String, int32> map(allocator);
    for (int32 index = 0; index < 6; ++index) {
        String key(allocator);
        const char text[] = {'k', 'e', 'y', '-', 'l',
                             'o', 'n', 'g', '-', static_cast<char>('a' + index)};
        REQUIRE(key.TryAssign(StringView(text, sizeof(text))));
        REQUIRE(map.Insert(Move(key), index));
    }

    Optional<String> taken;
    int32 value = -1;
    usize seenCount = NumericLimits<usize>::Max();
    bool seenContains = true;
    const uint64 before = allocator.deallocations;
    const bool erased =
        map.EraseAndTake(StringView("key-long-c"), [&](String&& key, int32&& stored) noexcept {
            seenCount = map.Count();
            seenContains = map.Contains(StringView("key-long-c"));
            taken.Emplace(Move(key));
            value = stored;
        });
    CHECK(erased && value == 2 && seenCount == 5 && !seenContains);
    CHECK(taken && taken->View() == StringView("key-long-c"));
    CHECK(allocator.deallocations == before);
    CHECK(
        !map.EraseAndTake(StringView("missing"), [&](String&&, int32&&) noexcept { value = 99; }));
    CHECK(value == 2);

    // 절반 넘게 지워 제자리 압축이 일어나도 남은 순서는 삽입 순서다.
    const char* const erasedKeys[] = {"key-long-a", "key-long-d", "key-long-e"};
    for (const char* key : erasedKeys) {
        String sink(allocator);
        REQUIRE(map.EraseAndTake(StringView(key),
                                 [&](String&& stored, int32&&) noexcept { sink = Move(stored); }));
    }

    const char* const order[] = {"key-long-b", "key-long-f"};
    usize position = 0;
    for (auto entry : map) {
        REQUIRE(position < 2);
        CHECK(entry.key.View() == StringView(order[position++]));
    }

    CHECK(position == 2 && map.Count() == 2);
    taken.Reset();
    CHECK(allocator.deallocations == before + 4);
}

#include <Base/Types.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

// OrderedMap·OrderedSet의 저장소 반환 순서와 2단계 예약이다(리플렉션 B9f3 리뷰 반영 — Docs/Design/
// ReflectionExtensions.ko.md 7.3절): 옛 저장소는 새 저장소를 게시한 뒤 반환하고(그 해제 콜백은
// 일관된 Map을 본다), PrepareReserve는 할당만 하고 Map을 바꾸지 않으며(버리면 잡은 저장소만 반환),
// CommitReserve는 할당·실패 없이 적용한다. IsIterating은 ForEach 중에만 참이다.
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

// 성장(Reallocate)·Reset·Compact·CommitReserve가 옛 저장소를 반환하는 순간 그 해제 콜백은 게시된
// Map을 본다: 성장이면 옛 원소가 모두 새 저장소에 있고(Count·Find·Contains 일관), Reset이면 빈
// Map이다.
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

    Seen reserved;
    reserved.map = &map;
    allocator.Watch(StorageOf(map), &Observe, &reserved);
    auto prepared = map.PrepareReserve(9);
    REQUIRE(prepared);
    map.CommitReserve(prepared.Value());
    CHECK(reserved.called && reserved.count == 5 && reserved.allFound && reserved.valuesMatch);

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

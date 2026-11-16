#include "MapTestSupport.h"

// 리플렉션 B9f3 key 경로 연산의 재진입·소유자 파괴 시험이다(Docs/Design/ReflectionExtensions.ko.md
// 7.3절, RF 결정 20 — 할당은 게시 전, 게시는 무할당, 해제 콜백은 게시된 상태만 본다). 객체
// allocator의 명시 hook(할당 직전·지켜보는 주소의 반환 직전)으로 순서를 만들고 결과를 결정적으로
// 단언한다: 지우기 키의 해제 콜백의 재진입 쓰기는 Busy이고 읽기는 지운 뒤의 Map을 보며, 넣기의 할당
// 콜백 재진입 쓰기도 Busy, 성장의 옛 저장소 해제 콜백은 게시된 Map을 보고, 저장소 준비 중 owner가
// 파괴되면 Destroyed이고 컨테이너(용량·첫 항목 주소 포함)는 그대로다. 비우기의 해제 콜백은 빈
// 컨테이너를 본다.
using namespace rocks;
using namespace rocks::test::map;

namespace {

// 할당 직전과 지켜보는 주소의 반환 직전에 hook을 한 번 실행한다.
class HookAllocator final : public Allocator {
public:
    explicit HookAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        ++allocations;
        if (hook != nullptr && allocations == hookAt) {
            auto run = hook;
            hook = nullptr;
            run(hookContext);
        }

        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (releaseHook != nullptr && address == watched) {
            auto run = releaseHook;
            releaseHook = nullptr;
            run(releaseContext);
        }

        return backing_.Deallocate(address);
    }

    // 지금부터 after번째 할당 직전에 run을 한 번 실행한다.
    void ArmAllocation(void (*run)(void*) noexcept, void* context, uint64 after) noexcept {
        hook = run;
        hookContext = context;
        hookAt = allocations + after;
    }

    // address의 반환 직전에 run을 한 번 실행한다.
    void ArmRelease(const void* address, void (*run)(void*) noexcept, void* context) noexcept {
        watched = address;
        releaseHook = run;
        releaseContext = context;
    }

    uint64 allocations = 0;

private:
    Allocator& backing_;
    uint64 hookAt = 0;
    void (*hook)(void*) noexcept = nullptr;
    void* hookContext = nullptr;
    const void* watched = nullptr;
    void (*releaseHook)(void*) noexcept = nullptr;
    void* releaseContext = nullptr;
};

// 힙 버퍼를 쓰는 긴 키다(첫 글자만 다르다).
#define LONG_KEY(c) c "_key_long_enough_to_need_a_heap_buffer_of_its_own_xxxxxxxx"

// 시험 하나의 Registry 묶음과 탐침 결과다.
struct World {
    explicit World(Allocator& system) noexcept
        : hooked(system), types(system), objects(hooked, types, {0x4d5052, 1}), values(system) {
    }

    HookAllocator hooked;
    TypeRegistry types;
    ObjectRegistry objects;
    Allocator& values;
    Result<TypeLease> lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
    ObjectHandle owner;
    ErrorCode written = ErrorCode::DependencyFailed;
    int contains[3] = {-1, -1, -1};
    usize count = NumericLimits<usize>::Max();
    const char* probes[3] = {nullptr, nullptr, nullptr};
    const char* writePath = nullptr;
    ErrorCode destroyed = ErrorCode::DependencyFailed;
};

ErrorCode Write(World& world, const char* path, const FieldValue& value) noexcept {
    const PropertyPathBuffer parsed = PathText(world.lease.Value(), path);
    return ErrorOf(
        PathWrite(world.objects, world.objects.OwnerAccess(), world.owner, parsed.View(), value));
}

int ContainsOf(World& world, const char* path) noexcept {
    const PropertyPathBuffer parsed = PathText(world.lease.Value(), path);
    auto found =
        PathContains(world.objects, world.objects.OwnerAccess(), world.owner, parsed.View());
    return found ? (found.Value() ? 1 : 0) : -1;
}

usize CountOf(World& world, const char* path) noexcept {
    const PropertyPathBuffer parsed = PathText(world.lease.Value(), path);
    auto count = PathCount(world.objects, world.objects.OwnerAccess(), world.owner, parsed.View());
    return count ? count.Value() : NumericLimits<usize>::Max();
}

// 콜백: writePath에 쓰고(결과 기록) probes를 조회하고 원소 수(probes[0]의 필드)를 센다.
void Probe(void* argument) noexcept {
    World& world = *static_cast<World*>(argument);
    if (world.writePath) {
        world.written = Write(world, world.writePath, Scalar(world.values, ValueKind::Float32, 9));
    }

    for (usize index = 0; index < 3; ++index) {
        if (world.probes[index]) {
            world.contains[index] = ContainsOf(world, world.probes[index]);
        }
    }

    world.count = CountOf(world, "ratios");
}

void Destroy(void* argument) noexcept {
    World& world = *static_cast<World*>(argument);
    world.destroyed =
        ErrorOf(world.objects.RequestDestroy(world.objects.OwnerAccess(), world.owner));
}

// ratios에 a·b·c·d 긴 키를 넣는다(용량 4 — 가득 찬다).
void FillRatios(World& world) noexcept {
    const char* keys[] = {"ratios[" LONG_KEY("a") "]", "ratios[" LONG_KEY("b") "]",
                          "ratios[" LONG_KEY("c") "]", "ratios[" LONG_KEY("d") "]"};
    for (const char* key : keys) {
        REQUIRE(Write(world, key, Scalar(world.values, ValueKind::Float32, 1)) == ErrorCode::None);
    }
}

// MapHolder ratios의 저장소 시작(첫 항목 키 주소)과 키 텍스트가 prefix로 시작하는 키의 버퍼다.
const void* Storage(World& world) noexcept {
    const void* storage = nullptr;
    With(world.objects, world.owner, [&](MapHolder& value) noexcept {
        for (auto entry : value.ratios) {
            storage = &entry.key;
            break;
        }
    });

    return storage;
}

const void* KeyBuffer(World& world, char first) noexcept {
    const void* buffer = nullptr;
    With(world.objects, world.owner, [&](MapHolder& value) noexcept {
        for (auto entry : value.ratios) {
            if (entry.key.View().Data()[0] == first) {
                buffer = entry.key.View().Data();
            }
        }
    });

    return buffer;
}

void Begin(World& world, RootRef<MapHolder>& holder) noexcept {
    REQUIRE(RegisterFixture(world.types));
    world.lease = world.types.Acquire(TypeIdOf<MapHolder>());
    REQUIRE(world.lease);
    holder = New(world.objects);
    world.owner = holder.Handle();
}

void End(World& world, RootRef<MapHolder>& holder) noexcept {
    holder.Reset();
    world.lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
    CHECK(world.objects.Collect(world.objects.OwnerAccess()));
    CHECK(world.objects.Shutdown(world.objects.OwnerAccess()));
    CHECK(world.types.Shutdown());
}

} // namespace

// 지우기: 키 b 버퍼의 해제 콜백(Erase 안)에서 같은 Map에 쓰면 고정 때문에 Busy이고, 조회는 b가 없고
// c·d가 있는 지운 뒤의 Map(원소 3)을 본다. 끝난 뒤 Map은 a·c·d 순서이고 쓰려던 키는 없다. removed를
// 준 지우기·Set 원소·값 struct 항목 지우기도 같다.
TEST(MapPathReentry_EraseKeyReleaseSeesErasedMapAndWritesAreBusy) {
    SystemAllocator system;
    World world(system);
    RootRef<MapHolder> holder;
    Begin(world, holder);
    FillRatios(world);
    for (int round = 0; round < 2; ++round) {
        const char* erased = round == 0 ? "ratios[" LONG_KEY("b") "]" : "ratios[" LONG_KEY("c") "]";
        world.writePath = "ratios[" LONG_KEY("z") "]";
        world.probes[0] = erased;
        world.probes[1] = "ratios[" LONG_KEY("d") "]";
        world.probes[2] = "ratios[" LONG_KEY("a") "]";
        world.hooked.ArmRelease(KeyBuffer(world, round == 0 ? 'b' : 'c'), &Probe, &world);
        const PropertyPathBuffer path = PathText(world.lease.Value(), erased);
        FieldValue removed(world.values);
        REQUIRE(PathEraseKey(world.objects, world.objects.OwnerAccess(), world.owner, path.View(),
                             round == 0 ? nullptr : &removed));
        CHECK(world.written == ErrorCode::Busy);
        CHECK(world.contains[0] == 0 && world.contains[1] == 1 && world.contains[2] == 1);
        CHECK(world.count == static_cast<usize>(3 - round));
        CHECK(ContainsOf(world, "ratios[" LONG_KEY("z") "]") == 0);
        world.written = ErrorCode::DependencyFailed;
    }

    With(world.objects, world.owner, [](MapHolder& value) noexcept {
        REQUIRE(value.ratios.Count() == 2);
        auto first = value.ratios.begin();
        CHECK((*first).key.View().Data()[0] == 'a');
    });

    // Set 원소와 값 struct 항목.
    REQUIRE(PathInsertKey(world.objects, world.objects.OwnerAccess(), world.owner,
                          PathText(world.lease.Value(), "words[" LONG_KEY("w") "]").View()));
    REQUIRE(PathInsertKey(world.objects, world.objects.OwnerAccess(), world.owner,
                          PathText(world.lease.Value(), "points[" LONG_KEY("p") "]").View()));
    const char* sets[] = {"words[" LONG_KEY("w") "]", "points[" LONG_KEY("p") "]"};
    for (const char* target : sets) {
        const void* buffer = nullptr;
        With(world.objects, world.owner, [&](MapHolder& value) noexcept {
            if (target[0] == 'w') {
                for (const String& word : value.words) {
                    buffer = word.View().Data();
                }
            } else {
                for (auto entry : value.points) {
                    buffer = entry.key.View().Data();
                }
            }
        });

        world.writePath = "ratios[" LONG_KEY("y") "]";
        world.probes[0] = target;
        world.probes[1] = nullptr;
        world.probes[2] = nullptr;
        world.hooked.ArmRelease(buffer, &Probe, &world);
        REQUIRE(PathEraseKey(world.objects, world.objects.OwnerAccess(), world.owner,
                             PathText(world.lease.Value(), target).View()));
        CHECK(world.written == ErrorCode::Busy && world.contains[0] == 0);
        world.written = ErrorCode::DependencyFailed;
    }

    End(world, holder);
}

// 성장: 가득 찬 Map에 새 키를 넣으면 옛 저장소는 새 저장소를 게시한 뒤 반환되고, 그 해제 콜백의
// 조회는 옛 원소 넷이 모두 있는 Map을 본다(새 키는 아직 없다).
TEST(MapPathReentry_GrowthReleaseSeesPublishedMap) {
    SystemAllocator system;
    World world(system);
    RootRef<MapHolder> holder;
    Begin(world, holder);
    FillRatios(world);
    world.probes[0] = "ratios[" LONG_KEY("a") "]";
    world.probes[1] = "ratios[" LONG_KEY("d") "]";
    world.probes[2] = "ratios[" LONG_KEY("e") "]";
    world.hooked.ArmRelease(Storage(world), &Probe, &world);
    REQUIRE(Write(world, "ratios[" LONG_KEY("e") "]",
                  Scalar(world.values, ValueKind::Float32, 5)) == ErrorCode::None);
    CHECK(world.contains[0] == 1 && world.contains[1] == 1 && world.contains[2] == 0);
    CHECK(world.count == 4 && CountOf(world, "ratios") == 5);
    End(world, holder);
}

// 넣기: 새 키 버퍼를 만드는 할당 콜백에서 같은 Map에 쓰면 고정 때문에 Busy이고 바깥 넣기는
// 성공한다(Map·Set 모두).
TEST(MapPathReentry_InsertAllocationWritesAreBusy) {
    SystemAllocator system;
    World world(system);
    RootRef<MapHolder> holder;
    Begin(world, holder);
    world.writePath = "ratios[" LONG_KEY("x") "]";
    world.hooked.ArmAllocation(&Probe, &world, 1);
    REQUIRE(Write(world, "ratios[" LONG_KEY("a") "]",
                  Scalar(world.values, ValueKind::Float32, 1)) == ErrorCode::None);
    CHECK(world.written == ErrorCode::Busy);
    CHECK(ContainsOf(world, "ratios[" LONG_KEY("x") "]") == 0 && CountOf(world, "ratios") == 1);

    world.written = ErrorCode::DependencyFailed;
    world.hooked.ArmAllocation(&Probe, &world, 1);
    auto inserted = PathInsertKey(world.objects, world.objects.OwnerAccess(), world.owner,
                                  PathText(world.lease.Value(), "words[" LONG_KEY("w") "]").View());
    CHECK(inserted && inserted.Value() && world.written == ErrorCode::Busy);
    CHECK(ContainsOf(world, "ratios[" LONG_KEY("x") "]") == 0);
    End(world, holder);
}

// 소유자 파괴: 저장소를 준비하는 할당(넣기의 둘째 할당 — 첫째는 키 버퍼) 직전에 owner를 파괴하면
// 결과는 Destroyed이고 컨테이너는 내용·용량·첫 항목 주소가 그대로다(준비한 저장소만 반환한다).
// 단일 값 Map·값 struct Map·Set이 같다.
TEST(MapPathReentry_OwnerDestroyedDuringReserveLeavesContainerUntouched) {
    SystemAllocator system;
    World world(system);
    RootRef<MapHolder> holder;
    Begin(world, holder);
    FillRatios(world);
    const char* names[] = {LONG_KEY("a"), LONG_KEY("b"), LONG_KEY("c"), LONG_KEY("d")};
    for (const char* name : names) {
        String word = Text(world.objects.ObjectAllocator(), name);
        With(world.objects, world.owner, [&](MapHolder& value) noexcept {
            REQUIRE(value.words.Add(Move(word)));
            MapPoint point(world.objects.ObjectAllocator());
            Put(value.points, Text(world.objects.ObjectAllocator(), name), Move(point));
        });
    }

    struct Snapshot {
        usize count = 0;
        usize capacity = 0;
        const void* first = nullptr;
    };

    // 파괴 요청 뒤에는 경로·Resolve로 닿지 않으므로 객체 주소를 미리 쥔다(물리 수명은 Collect
    // 전까지다).
    MapHolder* raw = nullptr;
    With(world.objects, world.owner, [&](MapHolder& value) noexcept { raw = &value; });
    REQUIRE(raw);
    const auto snapshot = [&](int which) noexcept {
        Snapshot result;
        if (which == 0) {
            result = {raw->ratios.Count(), raw->ratios.Capacity(), &(*raw->ratios.begin()).key};
        } else if (which == 1) {
            result = {raw->points.Count(), raw->points.Capacity(), &(*raw->points.begin()).key};
        } else {
            result = {raw->words.Count(), raw->words.Capacity(), &*raw->words.begin()};
        }

        return result;
    };

    const Snapshot before[] = {snapshot(0), snapshot(1), snapshot(2)};
    world.hooked.ArmAllocation(&Destroy, &world, 2);
    CHECK(Write(world, "ratios[" LONG_KEY("e") "]", Scalar(world.values, ValueKind::Float32, 5)) ==
          ErrorCode::Destroyed);
    CHECK(world.destroyed == ErrorCode::None);
    const char* paths[] = {"points[" LONG_KEY("e") "]", "words[" LONG_KEY("e") "]"};
    for (int which = 0; which < 3; ++which) {
        const Snapshot after = snapshot(which);
        CHECK(after.count == before[which].count && after.capacity == before[which].capacity &&
              after.first == before[which].first);
    }

    for (const char* path : paths) {
        auto inserted = PathInsertKey(world.objects, world.objects.OwnerAccess(), world.owner,
                                      PathText(world.lease.Value(), path).View());
        CHECK(!inserted);
    }

    End(world, holder);
}

// 비우기: 컨테이너를 지역으로 옮겨 빈 Map을 게시한 뒤 옛 원소·저장소를 반환하므로 그 해제 콜백은 빈
// Map을 본다.
TEST(MapPathReentry_ClearReleaseSeesEmptyMap) {
    SystemAllocator system;
    World world(system);
    RootRef<MapHolder> holder;
    Begin(world, holder);
    FillRatios(world);
    world.probes[0] = "ratios[" LONG_KEY("a") "]";
    world.hooked.ArmRelease(Storage(world), &Probe, &world);
    REQUIRE(PathClear(world.objects, world.objects.OwnerAccess(), world.owner,
                      PathText(world.lease.Value(), "ratios").View()));
    CHECK(world.count == 0 && world.contains[0] == 0);
    End(world, holder);
}

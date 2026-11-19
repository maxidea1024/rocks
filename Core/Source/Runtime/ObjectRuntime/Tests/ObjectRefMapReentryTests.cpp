#include "ObjectRefMapTestSupport.h"

#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// ObjectRefMap(리플렉션 B9f4 — Docs/Design/ReflectionExtensions.ko.md 7.4절)의 재진입·실패
// 주입·관찰자 시험이다. 잠금 밖 준비(키 사본·저장소) 중 allocator 콜백이 같은 컨테이너의 구조를
// 바꾸면 Busy, owner를 파괴 요청하면 StaleHandle이고 컨테이너의 내용·용량·저장소 주소가 그대로다.
// 해제 콜백(지운 키·옛 저장소·떼어 낸 항목)은 게시를 마친 컨테이너를 보고 재진입 편집도 된다. 모든
// 할당 실패 지점은 컨테이너를 바꾸지 않는다. 관찰자 계수는 편집은 Registry API, Commit·Discard는
// Trace로 센다.
using namespace rocks;
namespace object_ref_map_reentry_tests_file {

using namespace refmap_fixture;

namespace {

const StringView Alpha("alpha-key");
const StringView Beta("beta-key");
const StringView Gamma("gamma-key");
const StringView Delta("delta-key");

// allocator 콜백에서 같은 컨테이너를 바꾸는 탐침이다.
struct Probe {
    ObjectRegistry* objects = nullptr;
    ObjectHandle owner;
    Holder* holder = nullptr;
    ObjectHandle target;
    ErrorCode result = ErrorCode::DependencyFailed;
    usize count = NumericLimits<usize>::Max();
    bool contains = false;
    bool valueMatches = false;
};

void InsertNumberFifty(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.result =
        ErrorOf(probe.objects->SetMapReference(probe.objects->OwnerAccess(), probe.owner,
                                               probe.holder->numbered, uint32{50}, probe.target));
}

void EraseNumberZero(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.result = ErrorOf(probe.objects->EraseMapReference(
        probe.objects->OwnerAccess(), probe.owner, probe.holder->numbered, uint32{0}));
}

void ReplaceNumberOne(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.result =
        ErrorOf(probe.objects->SetMapReference(probe.objects->OwnerAccess(), probe.owner,
                                               probe.holder->numbered, uint32{1}, probe.target));
}

void DestroyOwner(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.result =
        ErrorOf(probe.objects->RequestDestroy(probe.objects->OwnerAccess(), probe.owner));
}

void InsertGammaName(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.result = ErrorOf(probe.objects->SetMapReference(
        probe.objects->OwnerAccess(), probe.owner, probe.holder->named, Gamma, probe.target));
}

// 해제 콜백에서 Map을 읽고(삭제·게시를 마친 상태여야 한다) 다른 키를 넣는다.
void ObserveErasedAndInsert(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.count = probe.holder->named.Count();
    probe.contains = probe.holder->named.Contains(Alpha);
    probe.result = ErrorOf(probe.objects->SetMapReference(
        probe.objects->OwnerAccess(), probe.owner, probe.holder->named, Delta, probe.target));
}

// 옛 저장소 해제 콜백에서 성장을 마친 Map(새 키 포함)을 본다.
void ObserveGrown(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.count = probe.holder->numbered.Count();
    const ObjectHandle* found = probe.holder->numbered.Find(uint32{9});
    probe.contains = found != nullptr;
    probe.valueMatches = found != nullptr && *found == probe.target;
}

// 비우기의 옛 저장소 해제 콜백에서 빈 Map을 보고 새 키를 넣는다.
void ObserveClearedAndInsert(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.count = probe.holder->numbered.Count();
    probe.result =
        ErrorOf(probe.objects->SetMapReference(probe.objects->OwnerAccess(), probe.owner,
                                               probe.holder->numbered, uint32{77}, probe.target));
}

// numbered를 키 0..3(값 target)으로 채워 용량 4를 꽉 채운다.
bool FillNumbered(ObjectRegistry& objects, ObjectHandle owner, Holder& holder,
                  ObjectHandle target) noexcept {
    for (uint32 key = 0; key < 4; ++key) {
        if (!objects.SetMapReference(objects.OwnerAccess(), owner, holder.numbered, key, target)) {
            return false;
        }
    }

    return holder.numbered.View().Capacity() == 4 && holder.numbered.Count() == 4;
}

} // namespace

// 넣기 준비(저장소 할당) 중 콜백이 같은 Map에 다른 키를 넣거나 지우면 다시 잠근 뒤 Busy이고 콜백의
// 변경만 남는다. 콜백이 owner를 파괴 요청하면 StaleHandle이고 내용·용량·저장소 주소가 그대로다.
// 구조를 바꾸지 않는 값 교체는 넣기를 막지 않는다. String 키 사본 할당 중 콜백이 같은 키를 넣으면
// Busy이고 콜백의 값이 남는다.
TEST(ObjectRefMap_InsertionReentryRejudgesAfterRelock) {
    ScriptedAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 21});
        const auto access = objects.OwnerAccess();
        auto first = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto second = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(first && second);
        const ObjectHandle one = first.Value().Handle();
        const ObjectHandle two = second.Value().Handle();
        struct Case {
            void (*hook)(void*) noexcept;
            ErrorCode expected;
            usize count;
            bool outerInserted;
        };

        const Case cases[] = {
            {&InsertNumberFifty, ErrorCode::Busy, 5, false},
            {&EraseNumberZero, ErrorCode::Busy, 3, false},
            {&ReplaceNumberOne, ErrorCode::None, 5, true},
            {&DestroyOwner, ErrorCode::StaleHandle, 4, false},
        };
        for (const Case& current : cases) {
            auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
            REQUIRE(holder);
            const ObjectHandle owner = holder.Value().Handle();
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            REQUIRE(FillNumbered(objects, owner, *pointer, one));
            const void* storage = StorageOf(pointer->numbered);
            Probe probe;
            probe.objects = &objects;
            probe.owner = owner;
            probe.holder = pointer;
            probe.target = two;
            // uint32 키는 할당하지 않으므로 첫 할당이 새 저장소다.
            allocator.OnAllocation(1, current.hook, &probe);
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->numbered, uint32{60},
                                                  one)) == current.expected);
            allocator.Disarm();
            CHECK(probe.result == ErrorCode::None);
            CHECK(pointer->numbered.Count() == current.count);
            CHECK(pointer->numbered.Contains(uint32{60}) == current.outerInserted);
            if (current.expected == ErrorCode::StaleHandle) {
                CHECK(pointer->numbered.View().Capacity() == 4 &&
                      StorageOf(pointer->numbered) == storage);
            }

            if (current.hook == &ReplaceNumberOne) {
                CHECK(*pointer->numbered.Find(uint32{1}) == two &&
                      *pointer->numbered.Find(uint32{60}) == one);
            }

            borrowed.Value().Reset();
            holder.Value().Reset();
        }

        // String 키: 첫 할당(키 사본) 중 같은 키를 넣으면 Busy이고 콜백의 값이 남는다.
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        REQUIRE(holder);
        const ObjectHandle owner = holder.Value().Handle();
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            Probe probe;
            probe.objects = &objects;
            probe.owner = owner;
            probe.holder = pointer;
            probe.target = two;
            allocator.OnAllocation(1, &InsertGammaName, &probe);
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Gamma, one)) ==
                  ErrorCode::Busy);
            allocator.Disarm();
            CHECK(probe.result == ErrorCode::None && pointer->named.Count() == 1 &&
                  *pointer->named.Find(Gamma) == two);
            REQUIRE(objects.ClearMapReferences(access, owner, pointer->named));
        }

        holder.Value().Reset();
        first.Value().Reset();
        second.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

// 해제 콜백은 게시를 마친 Map을 본다: 지운 String 키의 해제 콜백은 그 키가 없는 Map을 보고 재진입한
// 넣기도 된다(잠금을 푼 뒤라 Busy가 아니다). 성장으로 버린 옛 저장소의 해제 콜백은 새 키까지 담은
// Map을, 비우기로 떼어 낸 옛 저장소의 해제 콜백은 빈 Map을 본다.
TEST(ObjectRefMap_ReleaseCallbacksSeeThePublishedMap) {
    ScriptedAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 22});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto item = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && item);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle target = item.Value().Handle();
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            REQUIRE(objects.SetMapReference(access, owner, pointer->named, Alpha, target));
            REQUIRE(objects.SetMapReference(access, owner, pointer->named, Beta, target));
            Probe erased;
            erased.objects = &objects;
            erased.owner = owner;
            erased.holder = pointer;
            erased.target = target;
            const String* stored = pointer->named.View().FindStoredKey(Alpha);
            REQUIRE(stored != nullptr);
            allocator.OnRelease(stored->Data(), &ObserveErasedAndInsert, &erased);
            REQUIRE(objects.EraseMapReference(access, owner, pointer->named, Alpha));
            CHECK(allocator.releaseHook == nullptr);
            CHECK(erased.count == 1 && !erased.contains && erased.result == ErrorCode::None);
            const StringView order[] = {Beta, Delta};
            CHECK(NamedOrderIs(pointer->named, order, 2));

            Probe grown;
            grown.objects = &objects;
            grown.owner = owner;
            grown.holder = pointer;
            grown.target = target;
            REQUIRE(FillNumbered(objects, owner, *pointer, ObjectHandle{}));
            allocator.OnRelease(StorageOf(pointer->numbered), &ObserveGrown, &grown);
            REQUIRE(objects.SetMapReference(access, owner, pointer->numbered, uint32{9}, target));
            CHECK(allocator.releaseHook == nullptr);
            CHECK(grown.count == 5 && grown.contains && grown.valueMatches);

            Probe cleared;
            cleared.objects = &objects;
            cleared.owner = owner;
            cleared.holder = pointer;
            cleared.target = target;
            allocator.OnRelease(StorageOf(pointer->numbered), &ObserveClearedAndInsert, &cleared);
            REQUIRE(objects.ClearMapReferences(access, owner, pointer->numbered));
            CHECK(allocator.releaseHook == nullptr);
            CHECK(cleared.count == 0 && cleared.result == ErrorCode::None);
            CHECK(pointer->numbered.Count() == 1 && *pointer->numbered.Find(uint32{77}) == target);
            REQUIRE(objects.ClearMapReferences(access, owner, pointer->numbered));
            REQUIRE(objects.ClearMapReferences(access, owner, pointer->named));
        }

        holder.Value().Reset();
        item.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

// 모든 할당 실패 지점이 컨테이너를 바꾸지 않는다: 가득 찬 String 키 Map의 새 키(키 사본·새
// 저장소)는 지점마다 OutOfMemory이고 개수·용량·저장소 주소·순서가 그대로다. 있는 키의 교체는
// 할당하지 않는다. staged 통째 지정(새 저장소·키 사본)도 지점마다 OutOfMemory이고 옛 내용이
// 그대로다.
TEST(ObjectRefMap_EveryAllocationFailureLeavesMapUnchanged) {
    ScriptedAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 23});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto item = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && item);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle target = item.Value().Handle();
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            const StringView keys[] = {Alpha, Beta, Gamma, Delta};
            for (StringView key : keys) {
                REQUIRE(objects.SetMapReference(access, owner, pointer->named, key, target));
            }

            REQUIRE(pointer->named.View().Capacity() == 4);
            const void* storage = StorageOf(pointer->named);
            for (uint64 attempt = 1; attempt <= 2; ++attempt) {
                allocator.FailAt(attempt);
                CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named,
                                                      StringView("epsilon-key"), target)) ==
                      ErrorCode::OutOfMemory);
                allocator.Disarm();
                CHECK(NamedOrderIs(pointer->named, keys, 4));
                CHECK(pointer->named.View().Capacity() == 4 &&
                      StorageOf(pointer->named) == storage);
            }

            const uint64 before = allocator.allocations;
            REQUIRE(objects.SetMapReference(access, owner, pointer->named, Beta, ObjectHandle{}));
            CHECK(allocator.allocations == before);
            REQUIRE(objects.SetMapReference(access, owner, pointer->named,
                                            StringView("epsilon-key"), target));
            CHECK(allocator.allocations == before + 2 && pointer->named.Count() == 5);
            REQUIRE(objects.ClearMapReferences(access, owner, pointer->named));
        }

        {
            ConstructionContext transaction(objects, access);
            auto staged = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(staged);
            auto pointer = transaction.Get<Holder>(staged.Value());
            REQUIRE(pointer);
            String first[1] = {String(allocator)};
            REQUIRE(first[0].TryAssign(Alpha));
            const ObjectHandle one[] = {target};
            REQUIRE(transaction.AssignMapReferences(staged.Value(), pointer.Value()->named,
                                                    Span<const String>(first, 1),
                                                    Span<const ObjectHandle>(one)));
            String keys[2] = {String(allocator), String(allocator)};
            REQUIRE(keys[0].TryAssign(Beta) && keys[1].TryAssign(Gamma));
            const ObjectHandle targets[] = {target, ObjectHandle{}};
            // 새 저장소 1번, 키 사본 2·3번.
            for (uint64 attempt = 1; attempt <= 3; ++attempt) {
                allocator.FailAt(attempt);
                CHECK(ErrorOf(transaction.AssignMapReferences(
                          staged.Value(), pointer.Value()->named, Span<const String>(keys, 2),
                          Span<const ObjectHandle>(targets))) == ErrorCode::OutOfMemory);
                allocator.Disarm();
                CHECK(NamedOrderIs(pointer.Value()->named, &Alpha, 1));
            }

            REQUIRE(transaction.AssignMapReferences(staged.Value(), pointer.Value()->named,
                                                    Span<const String>(keys, 2),
                                                    Span<const ObjectHandle>(targets)));
            const StringView order[] = {Beta, Gamma};
            CHECK(NamedOrderIs(pointer.Value()->named, order, 2));
        }

        holder.Value().Reset();
        item.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

namespace {

// staged 통째 지정의 잠금 밖 할당 중 재진입은 처리 표시 때문에 Busy다.
struct StagedProbe {
    ObjectRegistry* objects = nullptr;
    ObjectHandle owner;
    Holder* holder = nullptr;
    ErrorCode result = ErrorCode::None;
};

void ReenterLiveEdit(void* raw) noexcept {
    auto& probe = *static_cast<StagedProbe*>(raw);
    probe.result =
        ErrorOf(probe.objects->SetMapReference(probe.objects->OwnerAccess(), probe.owner,
                                               probe.holder->numbered, uint32{1}, ObjectHandle{}));
}

} // namespace

// staged AssignMapReferences의 잠금 밖 준비 중 allocator 콜백의 Registry 변경은 Busy이고(처리
// 표시), 지정은 그 뒤 성공한다.
TEST(ConstructionContext_AssignMapReferencesReentryIsBusy) {
    ScriptedAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 24});
        const auto access = objects.OwnerAccess();
        auto live = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        REQUIRE(live);
        auto borrowed = objects.Resolve<Holder>(access, live.Value().Handle());
        REQUIRE(borrowed);
        StagedProbe probe;
        probe.objects = &objects;
        probe.owner = live.Value().Handle();
        probe.holder = Get(borrowed.Value());
        {
            ConstructionContext transaction(objects, access);
            auto staged = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(staged);
            auto pointer = transaction.Get<Holder>(staged.Value());
            REQUIRE(pointer);
            const uint32 keys[] = {1, 2};
            const ObjectHandle targets[] = {ObjectHandle{}, ObjectHandle{}};
            allocator.OnAllocation(1, &ReenterLiveEdit, &probe);
            REQUIRE(transaction.AssignMapReferences(staged.Value(), pointer.Value()->numbered,
                                                    Span<const uint32>(keys),
                                                    Span<const ObjectHandle>(targets)));
            allocator.Disarm();
            CHECK(probe.result == ErrorCode::Busy && probe.holder->numbered.IsEmpty());
            CHECK(pointer.Value()->numbered.Count() == 2);
        }

        borrowed.Value().Reset();
        live.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}
} // namespace object_ref_map_reentry_tests_file

namespace refmap_observer {

using namespace rocks;

struct MapPackage final : Object {};

struct MapNode final : Object {
    explicit MapNode(Allocator& allocator) noexcept : slots(allocator), named(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        slots.Trace(visitor);
        named.Trace(visitor);
    }

    ObjectRefMap<uint32, Object> slots;
    ObjectRefMap<String, Object> named;
};

constexpr TypeId PackageType{0x52464d504b470001ULL, 1};
constexpr TypeId NodeType{0x52464d4e4f440001ULL, 1};

template <class T>
RuntimeTypeOps CompleteOps() noexcept {
    RuntimeTypeOps ops = MakeRuntimeTypeOps<T>();
    ops.completeFieldLayout = true;
    return ops;
}

struct Fixture {
    Fixture(Allocator& allocator, uint64 id, Allocator& observerAllocator) noexcept
        : allocator(allocator), observerAllocator(observerAllocator), types(allocator),
          observer(observerAllocator, ReferenceObserver::DefaultMaxKeys),
          registry(allocator, types, {0x5247, id}, Options(observer)), roots(allocator) {
        if (!types.Register({PackageType, 1, {}, {}}, CompleteOps<MapPackage>()) ||
            !types.Register({NodeType, 1, {}, {}}, CompleteOps<MapNode>())) {
            platform::Fatal("ObjectRefMap observer fixture registration failed");
        }
    }

    ~Fixture() {
        for (RootRef<Object>& root : roots) {
            if (root.IsValid()) {
                (void)registry.Discard(registry.OwnerAccess(), root.Handle());
            }

            root.Reset();
        }

        if (!registry.Shutdown(registry.OwnerAccess())) {
            platform::Fatal("ObjectRefMap observer fixture registry did not shut down");
        }
    }

    static RegistryOptions Options(ReferenceObserver& observer) noexcept {
        RegistryOptions options;
        options.policy.packageNativeType = NativeTypeKey<MapPackage>();
        options.referenceObserver = &observer;
        return options;
    }

    ObjectHandle Package() noexcept {
        auto made = registry.NewObject<Object>(registry.OwnerAccess(), PackageType);
        if (!made || !roots.TryPushBack(Move(made.Value()))) {
            platform::Fatal("ObjectRefMap observer fixture package failed");
        }

        return roots[roots.Size() - 1].Handle();
    }

    ObjectHandle Node(ObjectHandle outer, StringView name = {}) noexcept {
        auto made = registry.NewObject<MapNode>(registry.OwnerAccess(), NodeType, outer, name);
        if (!made) {
            platform::Fatal("ObjectRefMap observer fixture node failed");
        }

        return made.Value().Handle();
    }

    Array<ReferenceEdgeDelta> Take() noexcept {
        Array<ReferenceEdgeDelta> changes(observerAllocator);
        if (!observer.TakeChanges(changes)) {
            platform::Fatal("ObjectRefMap observer fixture take failed");
        }

        return changes;
    }

    Allocator& allocator;
    Allocator& observerAllocator;
    TypeRegistry types;
    ReferenceObserver observer;
    ObjectRegistry registry;
    Array<RootRef<Object>> roots;
};

bool Is(const ReferenceEdgeDelta& change, ObjectHandle owner, ObjectHandle target, int64 delta,
        ReferenceChangeCause cause = ReferenceChangeCause::Edit, uint64 transaction = 0) noexcept {
    return change.ownerPackage == owner && change.targetPackage == target &&
           change.delta == delta && change.cause == cause && change.transaction == transaction;
}

} // namespace refmap_observer

namespace object_ref_map_reentry_tests_file {

// 관찰자 계수(RuntimeTypeOps.h의 규칙 — 편집은 Registry API, Commit·Discard는 Trace): 새 키·교체·
// 지우기는 단일 간선(옛 대상 키 먼저, 같은 패키지·null은 기록 없음), 비우기는 쌍별로 먼저 합친다.
// 관찰자 예약 실패는 OutOfMemory이고 컨테이너·기록이 그대로다. staged 지정은 기록하지 않고 Commit이
// Trace로 한 번 센다. Discard는 Trace로 나가는 다중도를 반납하고 그 뒤의 지우기·비우기는 기록하지
// 않는다.
TEST(ReferenceObserver_ObjectRefMapEditsFollowArrayRules) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        refmap_observer::Fixture fixture(system, 1, failing);
        using refmap_observer::Is;
        using refmap_observer::MapNode;
        const auto access = fixture.registry.OwnerAccess();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle same = fixture.Node(a, "Same");
        const ObjectHandle b1 = fixture.Node(q);
        const ObjectHandle b2 = fixture.Node(b1, "Two");
        const ObjectHandle c = fixture.Node(r);
        auto borrowed = fixture.registry.Resolve<MapNode>(access, a);
        REQUIRE(borrowed);
        MapNode* node = Get(borrowed.Value());
        ObjectRefMap<uint32, Object>& slots = node->slots;

        // 관찰자 예약 실패: 새 키 넣기는 OutOfMemory이고 항목·기록이 없다.
        failing.FailOnAttempt(1);
        CHECK(ErrorOf(fixture.registry.SetMapReference(access, a, slots, uint32{1}, b1)) ==
              ErrorCode::OutOfMemory);
        failing.DisableFailure();
        CHECK(slots.IsEmpty() && fixture.observer.PendingCount() == 0);

        REQUIRE(fixture.registry.SetMapReference(access, a, slots, uint32{1}, b1));
        REQUIRE(fixture.registry.SetMapReference(access, a, slots, uint32{2}, b2));
        REQUIRE(fixture.registry.SetMapReference(access, a, slots, uint32{3}, c));
        REQUIRE(fixture.registry.SetMapReference(access, a, slots, uint32{4}, same));
        REQUIRE(fixture.registry.SetMapReference(access, a, slots, uint32{5}, ObjectHandle{}));
        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, q, 2));
        CHECK(Is(changes[1], p, r, 1));

        // 교체(옛 대상 키 먼저 — (P, R)은 +1 −1로 빠진다)·같은 패키지 안 교체·지우기.
        REQUIRE(fixture.registry.SetMapReference(access, a, slots, uint32{3}, b1));
        REQUIRE(fixture.registry.SetMapReference(access, a, slots, uint32{3}, b2));
        REQUIRE(fixture.registry.EraseMapReference(access, a, slots, uint32{4}));
        REQUIRE(fixture.registry.EraseMapReference(access, a, slots, uint32{5}));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, r, -1));
        CHECK(Is(changes[1], p, q, 1));
        REQUIRE(fixture.registry.EraseMapReference(access, a, slots, uint32{1}));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, q, -1));

        // 비우기는 쌍별 합(삽입 순서로 처음 만난 쌍이 먼저).
        REQUIRE(fixture.registry.SetMapReference(access, a, slots, uint32{6}, c));
        (void)fixture.Take();
        REQUIRE(fixture.registry.ClearMapReferences(access, a, slots));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, q, -2));
        CHECK(Is(changes[1], p, r, -1));
        CHECK(slots.IsEmpty());

        // staged 지정은 기록하지 않고 Commit이 Trace로 센다.
        uint64 transactionId = 0;
        ObjectHandle inner;
        {
            ConstructionContext transaction(fixture.registry, access);
            auto made = transaction.Create<MapNode>(refmap_observer::NodeType, a, "Inner");
            REQUIRE(made);
            inner = made.Value();
            auto pointer = transaction.Get<MapNode>(inner);
            REQUIRE(pointer);
            String keys[3] = {String(system), String(system), String(system)};
            REQUIRE(keys[0].TryAssign(Alpha) && keys[1].TryAssign(Beta) &&
                    keys[2].TryAssign(Gamma));
            const ObjectHandle targets[] = {b1, c, b2};
            REQUIRE(transaction.AssignMapReferences(inner, pointer.Value()->named,
                                                    Span<const String>(keys, 3),
                                                    Span<const ObjectHandle>(targets)));
            CHECK(fixture.observer.PendingCount() == 0);
            transactionId = transaction.TransactionId();
            REQUIRE(transaction.Commit(Span<const ObjectHandle>(), Span<RootRef<Object>>()));
        }

        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, q, 2, ReferenceChangeCause::Commit, transactionId));
        CHECK(Is(changes[1], p, r, 1, ReferenceChangeCause::Commit, transactionId));

        // Discard는 Trace로 나가는 다중도를 반납한다. 그 뒤의 지우기·비우기는 기록하지 않는다.
        {
            auto innerBorrow = fixture.registry.Resolve<MapNode>(access, inner);
            REQUIRE(innerBorrow);
            MapNode* innerNode = Get(innerBorrow.Value());
            REQUIRE(fixture.registry.Discard(access, inner));
            changes = fixture.Take();
            REQUIRE(changes.Size() == 2);
            CHECK(Is(changes[0], p, q, -2, ReferenceChangeCause::Discard));
            CHECK(Is(changes[1], p, r, -1, ReferenceChangeCause::Discard));
            REQUIRE(fixture.registry.EraseMapReference(access, inner, innerNode->named, Beta));
            REQUIRE(fixture.registry.ClearMapReferences(access, inner, innerNode->named));
            CHECK(fixture.observer.PendingCount() == 0);
        }

        borrowed.Value().Reset();
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

// 관찰자 예약 콜백에서 같은 키의 값을 같은 패키지 안 다른 대상으로 바꾸는 탐침이다(새 관찰자 키가
// 필요 없어 바로 게시된다 — 구조 변경 계수는 그대로다).
struct ReplaceProbe {
    ObjectRegistry* objects = nullptr;
    ObjectHandle owner;
    ObjectRefMap<uint32, Object>* slots = nullptr;
    ObjectHandle target;
    ErrorCode result = ErrorCode::DependencyFailed;
};

void ReplaceSlotOne(void* raw) noexcept {
    auto& probe = *static_cast<ReplaceProbe*>(raw);
    probe.result = ErrorOf(probe.objects->SetMapReference(probe.objects->OwnerAccess(), probe.owner,
                                                          *probe.slots, uint32{1}, probe.target));
}

} // namespace

// 옛 값은 Registry가 게시 잠금 안에서 실제로 덮어쓴(지운) 값이다(리플렉션 B9f4 리뷰 L1):
// 교체·지우기가 관찰자 예약으로 잠금을 푼 사이 콜백이 같은 키의 값을 바꾸면,
// MapReferenceWrite::previous와 EraseMapReference의 removed는 호출 전 값이 아니라 콜백이 넣은
// 값이다. 새 키면 replaced가 거짓이고 previous는 null이다.
TEST(ObjectRefMap_WriteReportsTheValueActuallyReplaced) {
    SystemAllocator system;
    ScriptedAllocator observing;
    {
        refmap_observer::Fixture fixture(system, 2, observing);
        using refmap_observer::MapNode;
        const auto access = fixture.registry.OwnerAccess();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle b1 = fixture.Node(q);
        const ObjectHandle b2 = fixture.Node(b1, "Two");
        const ObjectHandle c = fixture.Node(r);
        auto borrowed = fixture.registry.Resolve<MapNode>(access, a);
        REQUIRE(borrowed);
        MapNode* node = Get(borrowed.Value());
        MapReferenceWrite inserted;
        REQUIRE(fixture.registry.SetMapReference(access, a, node->slots, uint32{1}, b1, &inserted));
        CHECK(!inserted.replaced && inserted.previous == ObjectHandle{});

        // 교체 (P,Q) -1·(P,R) +1은 새 관찰자 키 둘이 필요해 잠금 밖에서 예약한다. 그 할당 콜백이
        // 값을 b2(같은 패키지 Q — 기록 없음)로 바꾸면 실제로 덮어쓴 값은 b2다.
        (void)fixture.Take();
        ReplaceProbe probe;
        probe.objects = &fixture.registry;
        probe.owner = a;
        probe.slots = &node->slots;
        probe.target = b2;
        observing.OnAllocation(1, &ReplaceSlotOne, &probe);
        MapReferenceWrite write;
        REQUIRE(fixture.registry.SetMapReference(access, a, node->slots, uint32{1}, c, &write));
        observing.Disarm();
        CHECK(probe.result == ErrorCode::None);
        CHECK(write.replaced && write.previous == b2);
        CHECK(*node->slots.Find(uint32{1}) == c);

        // 지우기도 같다: 값을 b1(Q)로 되돌려 기록을 꺼낸 뒤, 지우기 (P,Q) -1의 예약 콜백이 값을
        // b2(같은 패키지)로 바꾸면 실제로 지운 값은 b2다.
        REQUIRE(fixture.registry.SetMapReference(access, a, node->slots, uint32{1}, b1));
        (void)fixture.Take();
        observing.OnAllocation(1, &ReplaceSlotOne, &probe);
        ObjectHandle removed;
        REQUIRE(fixture.registry.EraseMapReference(access, a, node->slots, uint32{1}, &removed));
        observing.Disarm();
        CHECK(probe.result == ErrorCode::None && removed == b2);
        CHECK(node->slots.IsEmpty());
        borrowed.Value().Reset();
    }

    CHECK(system.Stats().liveAllocations == 0);
    CHECK(observing.backing.Stats().liveAllocations == 0);
}

// 새 키의 항목 수 상한(MapReferenceWrite::maxCount — 리플렉션 B9f4 리뷰 L2): 상한에 닿은 Map의 새
// 키는 할당 없이 CapacityExceeded이고 그대로다. 있는 키의 교체는 상한과 무관하다. 상한 아래면
// 넣는다. 잠금 밖 준비 뒤 게시 직전에도 같은 판정을 한다(그 사이 길이를 바꾸는 재진입은 구조 변경
// 계수로 먼저 Busy다 — ObjectRefMap_InsertionReentryRejudgesAfterRelock).
TEST(ObjectRefMap_WriteLimitRejectsNewKeysAtTheLimit) {
    ScriptedAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 25});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto item = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && item);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle target = item.Value().Handle();
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            MapReferenceWrite limited;
            limited.maxCount = 2;
            REQUIRE(
                objects.SetMapReference(access, owner, pointer->named, Alpha, target, &limited));
            REQUIRE(objects.SetMapReference(access, owner, pointer->named, Beta, {}, &limited));
            const uint64 before = allocator.allocations;
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Gamma, target,
                                                  &limited)) == ErrorCode::CapacityExceeded);
            CHECK(allocator.allocations == before);
            const StringView order[] = {Alpha, Beta};
            CHECK(NamedOrderIs(pointer->named, order, 2));
            REQUIRE(objects.SetMapReference(access, owner, pointer->named, Beta, target, &limited));
            CHECK(limited.replaced && limited.previous == ObjectHandle{});
            limited.maxCount = 3;
            REQUIRE(
                objects.SetMapReference(access, owner, pointer->named, Gamma, target, &limited));
            CHECK(!limited.replaced && pointer->named.Count() == 3);
            REQUIRE(objects.ClearMapReferences(access, owner, pointer->named));
        }

        holder.Value().Reset();
        item.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}
} // namespace object_ref_map_reentry_tests_file

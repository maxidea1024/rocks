#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// A0-Ob ReferenceObserver 시험이다(Docs/Design/ObjectFoundations.ko.md 3절, 9절 Ob "관찰자"). 편집
// (단일·다중 간선 지점)·Commit·Discard 기록, 합산·순서, 예약 규칙(실패 원자성·재진입 재검증·중첩
// Busy), TakeChanges 계약, maxKeys, AcquireStructByNative를 본다. 경로 쓰기 기록은
// Reflection/PropertyPathTests.cpp, 무작위 대조는 ReferenceObserverModelTests.cpp다.
namespace observer_fixture {

using namespace rocks;

// StructRefArray 원소다(등록 struct — 관찰자 Registry의 원소 제거·비우기가 이것만 trace한다).
struct EdgeSlot {
    explicit EdgeSlot(Allocator&) noexcept {
    }

    ObjectRef<Object> first;
    ObjectRef<Object> second;

    static inline uint32 Traced = 0;
};

// 등록하지 않는 원소 struct다(AcquireStructByNative 실패 → Unsupported).
struct LooseSlot {
    ObjectRef<Object> target;
};

struct ObservedNode final : Object {
    explicit ObservedNode(Allocator& allocator) noexcept
        : items(allocator), slots(allocator), loose(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    ObjectRef<Object> link;
    ObjectRefArray<Object> items;
    StructRefArray<EdgeSlot> slots;
    StructRefArray<LooseSlot> loose;

    static inline uint32 Traced = 0;
};

struct ObservedPackage final : Object {
    ObjectRef<Object> link;

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(link);
    }
};

} // namespace observer_fixture

namespace rocks {

template <>
struct GeneratedType<observer_fixture::EdgeSlot> {
    static constexpr bool ContainsStrongReference = true;

    static void Trace(const observer_fixture::EdgeSlot& value, TraceVisitor& visitor) noexcept {
        ++observer_fixture::EdgeSlot::Traced;
        visitor.Visit(value.first);
        visitor.Visit(value.second);
    }
};

template <>
struct GeneratedType<observer_fixture::LooseSlot> {
    static constexpr bool ContainsStrongReference = true;

    static void Trace(const observer_fixture::LooseSlot& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }
};

} // namespace rocks

void observer_fixture::ObservedNode::Trace(TraceVisitor& visitor) const noexcept {
    ++Traced;
    visitor.Visit(link);
    items.Trace(visitor);
    slots.Trace(visitor);
    loose.Trace(visitor);
}

using namespace rocks;
namespace reference_observer_tests_file {

namespace {

using namespace observer_fixture;

constexpr TypeId PackageType{0x4f4252504b474531ULL, 1};
constexpr TypeId NodeType{0x4f42524e4f444531ULL, 1};
constexpr TypeId SlotType{0x4f425253534c5431ULL, 1};
constexpr Guid FirstId{0x4f425246ULL, 1};
constexpr Guid SecondId{0x4f425246ULL, 2};

FieldSchema TransientReference(Guid id, const char* name, usize bytes) noexcept {
    FieldSchema field;
    field.id = id;
    field.kind = FieldKind::ObjectReference;
    field.flags = static_cast<uint32>(FieldFlags::Transient);
    field.name = Span<const char>(name, bytes);
    field.value = ValueType{ValueKind::ObjectReference, {}};
    return field;
}

Result<TypeRegistrationToken> RegisterSlot(TypeRegistry& types) noexcept {
    static const FieldSchema fields[] = {TransientReference(FirstId, "first", 5),
                                         TransientReference(SecondId, "second", 6)};
    static const RuntimeStructFieldOps fieldOps[] = {{FirstId}, {SecondId}};
    RuntimeStructOps ops = MakeRuntimeStructOps<EdgeSlot>();
    ops.trace = [](const void* value, TraceVisitor& visitor) noexcept {
        GeneratedType<EdgeSlot>::Trace(*static_cast<const EdgeSlot*>(value), visitor);
    };
    ops.fields = Span<const RuntimeStructFieldOps>(fieldOps, 2);
    return types.RegisterStruct({SlotType, 1, Span<const FieldSchema>(fields, 2), {"EdgeSlot", 8}},
                                Move(ops));
}

// 완전한 필드 배치를 선언한 함수표다(Trace가 모든 참조 멤버를 방문한다 — 패키지 정책 Registry의
// 생성 조건).
template <class T>
RuntimeTypeOps CompleteOps() noexcept {
    RuntimeTypeOps ops = MakeRuntimeTypeOps<T>();
    ops.completeFieldLayout = true;
    return ops;
}

bool RegisterTypes(TypeRegistry& types) noexcept {
    return RegisterSlot(types).IsOk() &&
           types.Register({PackageType, 1, {}, {}}, CompleteOps<ObservedPackage>()).IsOk() &&
           types.Register({NodeType, 1, {}, {}}, CompleteOps<ObservedNode>()).IsOk();
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 한 시험의 Registry 묶음이다. observerAllocator·registryAllocator를 따로 줄 수 있다(실패 주입).
struct Fixture {
    Fixture(Allocator& allocator, uint64 id, usize maxKeys = ReferenceObserver::DefaultMaxKeys,
            Allocator* observerAllocator = nullptr, Allocator* registryAllocator = nullptr) noexcept
        : allocator(allocator),
          observerAllocator(observerAllocator != nullptr ? *observerAllocator : allocator),
          types(allocator), observer(this->observerAllocator, maxKeys),
          registry(registryAllocator != nullptr ? *registryAllocator : allocator, types,
                   {0x4f6230, id}, Options(observer)),
          roots(allocator) {
        if (!RegisterTypes(types)) {
            platform::Fatal("observer fixture type registration failed");
        }
    }

    ~Fixture() {
        // 남은 패키지를 Discard한 뒤 root를 놓는다(retiredWithoutDiscard 없이).
        for (RootRef<Object>& root : roots) {
            if (root.IsValid()) {
                (void)registry.Discard(registry.OwnerAccess(), root.Handle());
            }

            root.Reset();
        }

        if (!registry.Shutdown(registry.OwnerAccess())) {
            platform::Fatal("observer fixture registry did not shut down");
        }
    }

    static RegistryOptions Options(ReferenceObserver& observer) noexcept {
        RegistryOptions options;
        options.policy.packageNativeType = NativeTypeKey<ObservedPackage>();
        options.referenceObserver = &observer;
        return options;
    }

    ObjectAccessContext Access() const noexcept {
        return registry.OwnerAccess();
    }

    ObjectHandle Package() noexcept {
        auto made = registry.NewObject<Object>(Access(), PackageType);
        if (!made || !roots.TryPushBack(Move(made.Value()))) {
            platform::Fatal("observer fixture package failed");
        }

        return roots[roots.Size() - 1].Handle();
    }

    ObjectHandle Node(ObjectHandle outer, StringView name = {}) noexcept {
        auto made = registry.NewObject<ObservedNode>(Access(), NodeType, outer, name);
        if (!made) {
            platform::Fatal("observer fixture node failed");
        }

        return made.Value().Handle();
    }

    // handle의 Borrow를 쥔 채 객체를 돌려준다(borrow가 사는 동안 유효).
    template <class T>
    T* Pin(ObjectHandle handle, Borrow<T>& borrow) noexcept {
        auto resolved = registry.Resolve<T>(Access(), handle);
        if (!resolved) {
            platform::Fatal("observer fixture resolve failed");
        }

        borrow = Move(resolved.Value());
        auto pointer = borrow.Get();
        if (!pointer) {
            platform::Fatal("observer fixture borrow failed");
        }

        return pointer.Value();
    }

    Result<void> Link(ObjectHandle owner, ObjectHandle target) noexcept {
        Borrow<ObservedNode> borrow;
        ObservedNode* node = Pin(owner, borrow);
        return registry.SetReference(Access(), owner, node->link, target);
    }

    // 쌓인 변화를 새 빈 배열로 꺼낸다(관찰자는 용량 0 배열을 넘겨받아 다음 기록이 다시 예약한다).
    Array<ReferenceEdgeDelta> Take() noexcept {
        Array<ReferenceEdgeDelta> changes(observerAllocator);
        if (!observer.TakeChanges(changes)) {
            platform::Fatal("observer fixture take failed");
        }

        return changes;
    }

    // 용량 capacity의 빈 배열과 맞바꿔 꺼낸다(관찰자에 남는 새 키 공간이 정확히 capacity다).
    Array<ReferenceEdgeDelta> TakeLeaving(usize capacity) noexcept {
        Array<ReferenceEdgeDelta> spare(observerAllocator);
        if (!spare.TryReserve(capacity) || !observer.TakeChanges(spare)) {
            platform::Fatal("observer fixture take failed");
        }

        return spare;
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

} // namespace

// SetReference: null→A(+1), A→B(옛 대상 키 먼저 −1·+1), B→null(−1). 같은 패키지 대상·같은 패키지
// 안 교체는 기록·예약하지 않는다. 패키지 객체가 소유한 간선은 기록하지 않는다.
TEST(ReferenceObserver_SetReferenceRecordsPackagePairs) {
    SystemAllocator system;
    test::NFailAllocator counting(system);
    {
        Fixture fixture(system, 1, ReferenceObserver::DefaultMaxKeys, &counting);
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle same = fixture.Node(a, "Same");
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle b2 = fixture.Node(b, "Two");
        const ObjectHandle c = fixture.Node(r);

        REQUIRE(fixture.Link(a, b));
        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, q, 1));

        REQUIRE(fixture.Link(a, c));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, q, -1));
        CHECK(Is(changes[1], p, r, 1));

        REQUIRE(fixture.Link(a, {}));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, r, -1));

        // 같은 패키지 대상·같은 패키지 안 교체(Q 안 b → b2)는 기록·예약하지 않는다(꺼낸 뒤라 관찰자
        // 공간이 없으므로 예약하면 할당이 보인다).
        const uint64 before = counting.Attempts();
        REQUIRE(fixture.Link(a, same));
        REQUIRE(fixture.Link(a, {}));
        CHECK(fixture.observer.PendingCount() == 0);
        CHECK(counting.Attempts() == before);
        REQUIRE(fixture.Link(a, b));
        (void)fixture.Take();
        const uint64 reserved = counting.Attempts();
        REQUIRE(fixture.Link(a, b2));
        CHECK(fixture.observer.PendingCount() == 0);
        CHECK(counting.Attempts() == reserved);

        // 패키지 객체가 소유한 간선(AS 6.1의 하드 의존 사본)은 구조 규칙으로 빠진다.
        {
            Borrow<ObservedPackage> borrow;
            ObservedPackage* package = fixture.Pin(p, borrow);
            REQUIRE(fixture.registry.SetReference(fixture.Access(), p, package->link, c));
            REQUIRE(fixture.registry.SetReference(fixture.Access(), p, package->link, {}));
        }

        CHECK(fixture.observer.PendingCount() == 0);
        REQUIRE(fixture.Link(a, {}));
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// ObjectRefArray: 삽입·교체·삭제는 단일 간선, 비우기는 쌍별로 먼저 합친 뒤 기록한다.
// DestroyRequested owner의 제거·비우기는 기록하지 않는다(Discard가 이미 반납했다).
TEST(ReferenceObserver_ArrayEditsMergePerPackagePair) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 2);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle same = fixture.Node(a, "Same");
        const ObjectHandle b1 = fixture.Node(q);
        const ObjectHandle b2 = fixture.Node(b1, "Two");
        const ObjectHandle c = fixture.Node(r);
        Borrow<ObservedNode> borrow;
        ObservedNode* node = fixture.Pin(a, borrow);
        ObjectRefArray<Object>& items = node->items;
        REQUIRE(fixture.registry.AddReference(access, a, items, b1));
        REQUIRE(fixture.registry.AddReference(access, a, items, b2));
        REQUIRE(fixture.registry.InsertReferenceAt(access, a, items, 0, c));
        REQUIRE(fixture.registry.AddReference(access, a, items, same));
        REQUIRE(fixture.registry.AddReference(access, a, items, {}));
        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, q, 2));
        CHECK(Is(changes[1], p, r, 1));

        // 비우기는 쌍별 합(원소 순서로 처음 만난 쌍이 먼저 — c가 0번).
        REQUIRE(fixture.registry.ClearReferences(access, a, items));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, r, -1));
        CHECK(Is(changes[1], p, q, -2));

        // 교체(옛 대상 먼저)·삭제.
        REQUIRE(fixture.registry.AddReference(access, a, items, c));
        REQUIRE(fixture.registry.SetReferenceAt(access, a, items, 0, b1));
        REQUIRE(fixture.registry.SetReferenceAt(access, a, items, 0, b2)); // 같은 패키지 안
        changes = fixture.Take();
        REQUIRE(changes.Size() == 1); // (P, R)은 +1 −1로 0이 되어 빠진다
        CHECK(Is(changes[0], p, q, 1));
        REQUIRE(fixture.registry.AddReference(access, a, items, same));
        REQUIRE(fixture.registry.RemoveReference(access, a, items, same));
        CHECK(fixture.observer.PendingCount() == 0);
        REQUIRE(fixture.registry.RemoveReferenceAt(access, a, items, 0));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, q, -1));

        // DestroyRequested owner: Discard가 남은 원소를 반납하고, 이후 제거·비우기는 기록하지
        // 않는다.
        REQUIRE(fixture.registry.AddReference(access, a, items, b1));
        REQUIRE(fixture.registry.AddReference(access, a, items, c));
        (void)fixture.Take();
        REQUIRE(fixture.registry.Discard(access, a));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, q, -1, ReferenceChangeCause::Discard));
        CHECK(Is(changes[1], p, r, -1, ReferenceChangeCause::Discard));
        REQUIRE(fixture.registry.RemoveReferenceAt(access, a, items, 0));
        REQUIRE(fixture.registry.ClearReferences(access, a, items));
        CHECK(fixture.observer.PendingCount() == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// RemoveReferenceAtSwap은 지운 간선 하나만 기록한다(옮긴 끝 원소는 같은 소유자의 간선이라 패키지
// 쌍 계수가 바뀌지 않는다).
TEST(ReferenceObserver_SwapRemoveRecordsOnlyTheRemovedEdge) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 3);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle c = fixture.Node(r);
        Borrow<ObservedNode> borrow;
        ObservedNode* node = fixture.Pin(a, borrow);
        ObjectRefArray<Object>& items = node->items;
        REQUIRE(fixture.registry.AddReference(access, a, items, c));
        REQUIRE(fixture.registry.AddReference(access, a, items, b));
        REQUIRE(fixture.registry.AddReference(access, a, items, b));
        (void)fixture.Take();
        REQUIRE(fixture.registry.RemoveReferenceAtSwap(access, a, items, 0));
        REQUIRE(items.Size() == 2);
        CHECK(items[0] == b && items[1] == b);
        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, r, -1));
        REQUIRE(fixture.registry.RemoveReferenceAtSwap(access, a, items, 0));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, q, -1));
        REQUIRE(fixture.registry.ClearReferences(access, a, items));
        (void)fixture.Take();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// StructRefArray: 원소 추가·삽입은 기록이 없고(참조가 null), 제거·비우기는 떼어 낸 원소만 등록
// struct trace로 훑는다(소유자 Trace 호출 0). 원소 안 참조 쓰기는 SetReference와 같다.
TEST(ReferenceObserver_StructElementRemovalTracesOnlyDetachedElements) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 3);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle same = fixture.Node(a, "Same");
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle b2 = fixture.Node(b, "Two");
        const ObjectHandle c = fixture.Node(r);
        Borrow<ObservedNode> borrow;
        ObservedNode* node = fixture.Pin(a, borrow);
        StructRefArray<EdgeSlot>& slots = node->slots;
        for (uint32 index = 0; index < 3; ++index) {
            REQUIRE(fixture.registry.AddElement(access, a, slots));
        }

        REQUIRE(fixture.registry.InsertElementAt(access, a, slots, 3));
        CHECK(fixture.observer.PendingCount() == 0);
        REQUIRE(fixture.registry.SetElementReference(access, a, slots, 0, slots[0].first, b));
        REQUIRE(fixture.registry.SetElementReference(access, a, slots, 1, slots[1].first, c));
        REQUIRE(fixture.registry.SetElementReference(access, a, slots, 1, slots[1].second, b2));
        REQUIRE(fixture.registry.SetElementReference(access, a, slots, 2, slots[2].first, same));
        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, q, 2));
        CHECK(Is(changes[1], p, r, 1));

        ObservedNode::Traced = 0;
        EdgeSlot::Traced = 0;
        REQUIRE(fixture.registry.RemoveElementAt(access, a, slots, 1));
        CHECK(ObservedNode::Traced == 0);
        CHECK(EdgeSlot::Traced == 1);
        REQUIRE(fixture.registry.ClearElements(access, a, slots));
        CHECK(ObservedNode::Traced == 0);
        CHECK(EdgeSlot::Traced == 4);
        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, r, -1));
        CHECK(Is(changes[1], p, q, -2));
        CHECK(slots.Size() == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Commit: staged 쓰기는 기록하지 않고 공개 때 Outer 있는 소유자의 패키지 사이 간선을 쌍별로 합쳐
// transaction = TransactionId()로 기록한다. 트랜잭션마다 키가 따로다(N6).
TEST(ReferenceObserver_CommitRecordsTransactionAndStagedWritesAreSilent) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 4);
        const auto access = fixture.Access();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle b2 = fixture.Node(b, "Two");
        uint64 first = 0;
        ObjectHandle package;
        ObjectHandle assetHandle;
        {
            ConstructionContext transaction(fixture.registry, access);
            auto made = transaction.Create<ObservedPackage>(PackageType);
            REQUIRE(made);
            package = made.Value();
            auto asset = transaction.Create<ObservedNode>(NodeType, package);
            REQUIRE(asset);
            assetHandle = asset.Value();
            auto inner = transaction.Create<ObservedNode>(NodeType, asset.Value(), "Inner");
            REQUIRE(inner);
            auto packagePointer = transaction.Get<ObservedPackage>(package);
            auto assetPointer = transaction.Get<ObservedNode>(asset.Value());
            auto innerPointer = transaction.Get<ObservedNode>(inner.Value());
            REQUIRE(packagePointer && assetPointer && innerPointer);
            REQUIRE(transaction.SetReference(asset.Value(), assetPointer.Value()->link, b));
            REQUIRE(
                transaction.SetReference(inner.Value(), innerPointer.Value()->link, asset.Value()));
            const ObjectHandle targets[] = {b2, {}, b, inner.Value()};
            REQUIRE(
                transaction.AssignReferences(inner.Value(), innerPointer.Value()->items, targets));
            // 패키지 객체 소유 간선은 Commit에서도 빠진다.
            REQUIRE(transaction.SetReference(package, packagePointer.Value()->link, b));
            CHECK(fixture.observer.PendingCount() == 0);
            first = transaction.TransactionId();
            CHECK(first != 0);
            const ObjectHandle roots[] = {package};
            RootRef<Object> held[1];
            REQUIRE(transaction.Commit(roots, held));
            CHECK(transaction.TransactionId() == 0);
            REQUIRE(fixture.roots.TryPushBack(Move(held[0])));
        }

        // 두 번째 트랜잭션: 살아 있는 패키지 아래 새 객체.
        uint64 second = 0;
        const ObjectHandle host = fixture.Node(assetHandle, "Host");
        {
            ConstructionContext transaction(fixture.registry, access);
            auto asset = transaction.Create<ObservedNode>(NodeType, host, "Late");
            REQUIRE(asset);
            auto pointer = transaction.Get<ObservedNode>(asset.Value());
            REQUIRE(pointer);
            REQUIRE(transaction.SetReference(asset.Value(), pointer.Value()->link, b));
            second = transaction.TransactionId();
            REQUIRE(transaction.Commit(Span<const ObjectHandle>(), Span<RootRef<Object>>()));
        }

        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], package, q, 3, ReferenceChangeCause::Commit, first));
        CHECK(Is(changes[1], package, q, 1, ReferenceChangeCause::Commit, second));
        CHECK(first != second);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Discard: 부분 트리에서 나가는 강한 간선 다중도를 Discard 원인의 음수로 한 번 기록한다(목록 순 ×
// trace 순, transaction 0). 다른 패키지에서 들어오는 간선은 반납하지 않는다.
TEST(ReferenceObserver_DiscardReturnsOutgoingMultiplicityOnce) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 5);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle a1 = fixture.Node(a, "Child");
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle c = fixture.Node(r);
        {
            Borrow<ObservedNode> borrow;
            ObservedNode* node = fixture.Pin(a, borrow);
            REQUIRE(fixture.registry.SetReference(access, a, node->link, b));
            REQUIRE(fixture.registry.AddReference(access, a, node->items, b));
            REQUIRE(fixture.registry.AddReference(access, a, node->items, c));
            REQUIRE(fixture.registry.AddElement(access, a, node->slots));
            REQUIRE(fixture.registry.SetElementReference(access, a, node->slots, 0,
                                                         node->slots[0].second, c));
        }

        REQUIRE(fixture.Link(a1, c));
        REQUIRE(fixture.Link(b, a1)); // Q → P(들어오는 간선)
        (void)fixture.Take();
        REQUIRE(fixture.registry.Discard(access, p));
        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, q, -2, ReferenceChangeCause::Discard));
        CHECK(Is(changes[1], p, r, -3, ReferenceChangeCause::Discard));
        // 이미 Discard된 패키지는 무동작이다(다시 반납하지 않는다).
        REQUIRE(fixture.registry.Discard(access, p));
        CHECK(fixture.observer.PendingCount() == 0);
        // Q의 간선은 그대로다: 끊을 때 기록한다.
        REQUIRE(fixture.Link(b, {}));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], q, p, -1));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 합산: +1·−1로 0이 된 키는 꺼낼 때 빠지고(그 전까지는 키로 센다), 순서는 키가 처음 생긴 순서다.
TEST(ReferenceObserver_NetZeroKeysDropOutInDiscoveryOrder) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 6);
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle s = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle a2 = fixture.Node(a, "Second");
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle c = fixture.Node(r);
        const ObjectHandle d = fixture.Node(s);
        REQUIRE(fixture.Link(a, b));
        REQUIRE(fixture.Link(a2, d));
        REQUIRE(fixture.Link(a, c));
        REQUIRE(fixture.Link(a, {}));
        CHECK(fixture.observer.PendingCount() == 3);
        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, s, 1));
        CHECK(fixture.observer.PendingCount() == 0);
        REQUIRE(fixture.Link(a2, {}));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 예약 실패: 단일 간선(쓰기·삽입)·Commit의 관찰자 할당이 실패하면 OutOfMemory이고 아무것도 바뀌지
// 않는다(Commit은 비공개로 남는다).
TEST(ReferenceObserver_ReservationFailureLeavesTheChangeUnapplied) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        Fixture fixture(system, 7, ReferenceObserver::DefaultMaxKeys, &failing);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle b = fixture.Node(q);
        Borrow<ObservedNode> borrow;
        ObservedNode* node = fixture.Pin(a, borrow);
        for (uint64 attempt = 1; attempt <= 2; ++attempt) {
            failing.FailOnAttempt(attempt);
            CHECK(ErrorOf(fixture.registry.SetReference(access, a, node->link, b)) ==
                  ErrorCode::OutOfMemory);
            failing.DisableFailure();
            CHECK(node->link.Handle() == ObjectHandle{});
            CHECK(fixture.observer.PendingCount() == 0);
            failing.FailOnAttempt(attempt);
            CHECK(ErrorOf(fixture.registry.AddReference(access, a, node->items, b)) ==
                  ErrorCode::OutOfMemory);
            failing.DisableFailure();
            CHECK(node->items.Size() == 0);
        }

        REQUIRE(fixture.registry.SetReference(access, a, node->link, b));
        (void)fixture.Take(); // 관찰자 배열을 비운다(다음 Commit이 다시 예약해야 한다)
        {
            ConstructionContext transaction(fixture.registry, access);
            auto inner = transaction.Create<ObservedNode>(NodeType, a, "Inner");
            REQUIRE(inner);
            auto pointer = transaction.Get<ObservedNode>(inner.Value());
            REQUIRE(pointer);
            REQUIRE(transaction.SetReference(inner.Value(), pointer.Value()->link, b));
            failing.FailOnAttempt(1);
            CHECK(ErrorOf(transaction.Commit(Span<const ObjectHandle>(),
                                             Span<RootRef<Object>>())) == ErrorCode::OutOfMemory);
            failing.DisableFailure();
            CHECK(fixture.registry.Stats().reservedObjects == 1);
            CHECK(fixture.registry.FindChild(access, a, "Inner").Error() == ErrorCode::NotFound);
            CHECK(fixture.observer.PendingCount() == 0);
        }

        CHECK(fixture.registry.Stats().reservedObjects == 0);
        REQUIRE(fixture.registry.SetReference(access, a, node->link, {}));
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

// 관찰자 allocator: 다음 할당 한 번에 hook을 부른다(예약 중 재진입).
class HookAllocator final : public Allocator {
public:
    explicit HookAllocator(Allocator& base) noexcept : base_(base) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (hook != nullptr && (hookTag == 0 || tag == hookTag)) {
            auto run = hook;
            hook = nullptr;
            run(context);
        }

        return base_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return base_.Deallocate(address);
    }

    void (*hook)(void*) noexcept = nullptr;
    void* context = nullptr;
    uint32 hookTag = 0;

private:
    Allocator& base_;
};

struct Reentry {
    Fixture* fixture;
    ObjectHandle owner;
    ObjectHandle target;
    ErrorCode error = ErrorCode::None;
    ErrorCode takeError = ErrorCode::None;
};

void LinkFromCallback(void* raw) noexcept {
    auto& reentry = *static_cast<Reentry*>(raw);
    reentry.error = ErrorOf(reentry.fixture->Link(reentry.owner, reentry.target));
    Array<ReferenceEdgeDelta> out(reentry.fixture->observerAllocator);
    reentry.takeError = ErrorOf(reentry.fixture->observer.TakeChanges(out));
}

} // namespace

// 예약 중 재진입: 관찰자 할당 콜백의 다른 SetReference가 새 키가 필요하면 중첩 예약이라 Busy,
// 있는 키면 옛 표에 기록되고 그 기록도 새 표로 옮겨진다. 같은 field를 바꾸면 재잠금 뒤 다시 검증해
// 그때의 옛 대상으로 기록한다. 콜백의 TakeChanges는 Busy다.
TEST(ReferenceObserver_ReentrantRecordDuringReservationIsRevalidated) {
    SystemAllocator system;
    HookAllocator hooked(system);
    {
        Fixture fixture(system, 8, ReferenceObserver::DefaultMaxKeys, &hooked);
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle a2 = fixture.Node(a, "Second");
        const ObjectHandle a3 = fixture.Node(a, "Third");
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle c = fixture.Node(r);

        // 1) 첫 예약 중 콜백이 새 키가 필요한 기록을 하면 중첩 예약이라 Busy(그 field 불변), 바깥
        //    쓰기는 성공한다. 콜백의 TakeChanges도 Busy다.
        Reentry nested{&fixture, a2, c};
        hooked.hook = &LinkFromCallback;
        hooked.context = &nested;
        REQUIRE(fixture.Link(a, b));
        CHECK(nested.error == ErrorCode::Busy);
        CHECK(nested.takeError == ErrorCode::Busy);
        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, q, 1));
        {
            Borrow<ObservedNode> borrow;
            CHECK(fixture.Pin(a2, borrow)->link.Handle() == ObjectHandle{});
        }

        // 2) 새 키 공간을 하나만 남기고 (P, R)로 채운 뒤, 새 키(P, Q −1)를 위한 예약 중 콜백이 있는
        //    키 (P, R)에 +1한다 — 옛 표에 들어간 기록도 새 표로 옮겨진다.
        (void)fixture.TakeLeaving(1);
        REQUIRE(fixture.Link(a2, c));
        Reentry existing{&fixture, a3, c};
        hooked.hook = &LinkFromCallback;
        hooked.context = &existing;
        REQUIRE(fixture.Link(a, {}));
        CHECK(hooked.hook == nullptr);
        CHECK(existing.error == ErrorCode::None);
        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, r, 2));
        CHECK(Is(changes[1], p, q, -1));

        // 3) 콜백이 같은 field를 바꾸면 다시 검증한다: 예약 중 a.link가 c(있는 키 (P, R))가 되면
        //    바깥 쓰기(→ b)는 그때의 옛 대상 c 기준으로 (P, R) −1·(P, Q) +1을 기록한다.
        (void)fixture.TakeLeaving(1);
        REQUIRE(fixture.Link(a3, {})); // (P, R) −1 — 공간을 다 쓴다
        Reentry same{&fixture, a, c};
        hooked.hook = &LinkFromCallback;
        hooked.context = &same;
        REQUIRE(fixture.Link(a, b));
        CHECK(same.error == ErrorCode::None);
        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, r, -1)); // −1(a3) +1(콜백) −1(바깥 재검증)
        CHECK(Is(changes[1], p, q, 1));
        {
            Borrow<ObservedNode> borrow;
            CHECK(fixture.Pin(a, borrow)->link.Handle() == b);
        }

        REQUIRE(fixture.Link(a, {}));
        REQUIRE(fixture.Link(a2, {}));
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

struct ForeignTake {
    ReferenceObserver* observer;
    Allocator* allocator;
    ErrorCode error = ErrorCode::None;
    usize taken = 0;
    ReferenceEdgeDelta first{};
};

void TakeFromForeignThread(void* raw) noexcept {
    auto& call = *static_cast<ForeignTake*>(raw);
    Array<ReferenceEdgeDelta> out(*call.allocator);
    call.error = ErrorOf(call.observer->TakeChanges(out));
    call.taken = out.Size();
    if (out.Size() != 0) {
        call.first = out[0];
    }
}

} // namespace

// TakeChanges: out과 맞바꾸고(할당 없음), 비어 있지 않은 out·다른 allocator는 InvalidArgument, 다른
// 스레드는 WrongThread다. Registry가 사라진 뒤에도 남은 변화를 꺼낼 수 있다.
TEST(ReferenceObserver_TakeChangesContract) {
    SystemAllocator allocator;
    SystemAllocator other;
    {
        ReferenceObserver observer(allocator);
        ObjectHandle p;
        ObjectHandle q;
        {
            TypeRegistry types(allocator);
            REQUIRE(RegisterTypes(types));
            RegistryOptions options = Fixture::Options(observer);
            ObjectRegistry registry(allocator, types, {0x4f6239, 1}, options);
            const auto access = registry.OwnerAccess();
            auto package = registry.NewObject<ObservedPackage>(access, PackageType);
            auto target = registry.NewObject<ObservedPackage>(access, PackageType);
            REQUIRE(package && target);
            p = package.Value().Handle();
            q = target.Value().Handle();
            auto node = registry.NewObject<ObservedNode>(access, NodeType, p, {});
            REQUIRE(node);
            {
                auto borrowed = registry.Resolve<ObservedNode>(access, node.Value().Handle());
                REQUIRE(borrowed);
                auto pointer = borrowed.Value().Get();
                REQUIRE(pointer);
                REQUIRE(
                    registry.SetReference(access, node.Value().Handle(), pointer.Value()->link, q));
            }

            Array<ReferenceEdgeDelta> filled(allocator);
            REQUIRE(filled.TryPushBack(ReferenceEdgeDelta{}));
            CHECK(ErrorOf(observer.TakeChanges(filled)) == ErrorCode::InvalidArgument);
            CHECK(filled.Size() == 1);
            Array<ReferenceEdgeDelta> foreign(other);
            CHECK(ErrorOf(observer.TakeChanges(foreign)) == ErrorCode::InvalidArgument);
            ForeignTake call{&observer, &allocator};
            platform::Thread thread;
            if (thread.Start(TakeFromForeignThread, &call) != platform::ThreadStatus::Success ||
                thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
                thread.Join() != platform::ThreadStatus::Success) {
                platform::Fatal("TakeChanges test worker did not finish");
            }

            CHECK(call.error == ErrorCode::WrongThread);
            CHECK(observer.PendingCount() == 1);

            Array<ReferenceEdgeDelta> taken(allocator);
            REQUIRE(observer.TakeChanges(taken));
            REQUIRE(taken.Size() == 1);
            CHECK(Is(taken[0], p, q, 1));
            CHECK(observer.PendingCount() == 0);
            REQUIRE(registry.Discard(access, p));
            REQUIRE(registry.Discard(access, q));
            package.Value().Reset();
            target.Value().Reset();
            node.Value().Reset();
            REQUIRE(registry.Shutdown(access));
            REQUIRE(types.Shutdown());
        }

        // Registry가 떨어진 뒤에도 남은 변화(Discard의 반납)를 꺼낸다 — 떨어진 관찰자는 스레드를
        // 검사하지 않는다(다른 스레드도 꺼낸다).
        CHECK(observer.PendingCount() == 1);
        ForeignTake detached{&observer, &allocator};
        platform::Thread thread;
        if (thread.Start(TakeFromForeignThread, &detached) != platform::ThreadStatus::Success ||
            thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            thread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("TakeChanges test worker did not finish");
        }

        CHECK(detached.error == ErrorCode::None);
        CHECK(detached.taken == 1);
        CHECK(Is(detached.first, p, q, -1, ReferenceChangeCause::Discard));
        CHECK(observer.PendingCount() == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(other.Stats().liveAllocations == 0);
}

// maxKeys: 새 키가 상한을 넘는 기록 지점(단일 간선·다중 간선·Commit·Discard)은 CapacityExceeded이고
// 아무것도 바뀌지 않는다. 0이 된 키도 TakeChanges 전까지 센다.
TEST(ReferenceObserver_MaxKeysOverflowLeavesEverythingUnchanged) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 10, 2);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle s = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle a2 = fixture.Node(a, "Second");
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle c = fixture.Node(r);
        const ObjectHandle d = fixture.Node(s);
        REQUIRE(fixture.Link(a, b));
        REQUIRE(fixture.Link(a, {})); // (P, Q) 0 — 여전히 키다
        REQUIRE(fixture.Link(a2, c));
        CHECK(fixture.observer.PendingCount() == 2);
        CHECK(ErrorOf(fixture.Link(a, d)) == ErrorCode::CapacityExceeded);
        {
            Borrow<ObservedNode> borrow;
            CHECK(fixture.Pin(a, borrow)->link.Handle() == ObjectHandle{});
        }

        CHECK(fixture.observer.PendingCount() == 2);
        // 키 수가 상한에 닿아도 있는 키만 쓰는 변경은 된다(새 키 수를 정확히 센다): a2의 c → null →
        // c는 (P, R) Edit 키만 쓴다.
        REQUIRE(fixture.Link(a2, {}));
        REQUIRE(fixture.Link(a2, c));
        CHECK(fixture.observer.PendingCount() == 2);
        // Commit의 키(원인 Commit·이 트랜잭션)는 언제나 새 키라 넘친다.
        {
            ConstructionContext transaction(fixture.registry, access);
            auto inner = transaction.Create<ObservedNode>(NodeType, a, "Inner");
            REQUIRE(inner);
            auto pointer = transaction.Get<ObservedNode>(inner.Value());
            REQUIRE(pointer);
            REQUIRE(transaction.SetReference(inner.Value(), pointer.Value()->link, d));
            CHECK(
                ErrorOf(transaction.Commit(Span<const ObjectHandle>(), Span<RootRef<Object>>())) ==
                ErrorCode::CapacityExceeded);
            CHECK(fixture.registry.Stats().reservedObjects == 1);
        }

        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, r, 1));

        // 다중 간선: items {b, c}를 비우려면 (P, Q)·(P, R) Edit 키가 필요하다. (P, R)·(P, S)로
        // 상한을 채우면 (P, Q)가 넘친다(배열 그대로).
        Borrow<ObservedNode> borrow;
        ObservedNode* node = fixture.Pin(a, borrow);
        REQUIRE(fixture.registry.AddReference(access, a, node->items, b));
        REQUIRE(fixture.registry.AddReference(access, a, node->items, c));
        (void)fixture.Take();
        REQUIRE(fixture.Link(a2, d)); // a2: c → d — (P, R) −1, (P, S) +1
        CHECK(fixture.observer.PendingCount() == 2);
        CHECK(ErrorOf(fixture.registry.ClearReferences(access, a, node->items)) ==
              ErrorCode::CapacityExceeded);
        CHECK(node->items.Size() == 2);
        // Discard의 키(원인 Discard)는 모두 새 키다.
        CHECK(ErrorOf(fixture.registry.Discard(access, a)) == ErrorCode::CapacityExceeded);
        auto state = fixture.registry.InspectLifecycle(access, a);
        CHECK(state && state.Value() == ObjectLifecycle::Alive);
        CHECK(fixture.observer.PendingCount() == 2);
        changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, r, -1));
        CHECK(Is(changes[1], p, s, 1));
        REQUIRE(fixture.registry.ClearReferences(access, a, node->items));
        CHECK(node->items.Size() == 0);
        (void)fixture.Take();
        REQUIRE(fixture.Link(a2, {}));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

struct ProcessingReentry {
    Fixture* fixture;
    ObjectHandle owner;
    ObservedNode* node; // owner의 Borrow는 시험이 쥐고 있다
    ObjectHandle target;
    ErrorCode error = ErrorCode::None;
};

void LinkDuringTally(void* raw) noexcept {
    auto& reentry = *static_cast<ProcessingReentry*>(raw);
    reentry.error = ErrorOf(reentry.fixture->registry.SetReference(
        reentry.fixture->Access(), reentry.owner, reentry.node->link, reentry.target));
}

} // namespace

// 다중 간선 지점: 집계 할당 실패는 OutOfMemory이고 배열·관찰자 불변이다. 집계 할당 콜백(처리
// 표시 아래)의 Registry 변경 재진입은 Busy다.
TEST(ReferenceObserver_MultiEdgeSitesFailAtomicallyAndRejectReentry) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    HookAllocator hooked(failing);
    {
        Fixture fixture(system, 11, ReferenceObserver::DefaultMaxKeys, nullptr, &hooked);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle a2 = fixture.Node(a, "Second");
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle c = fixture.Node(r);
        Borrow<ObservedNode> borrow;
        ObservedNode* node = fixture.Pin(a, borrow);
        REQUIRE(fixture.registry.AddReference(access, a, node->items, b));
        REQUIRE(fixture.registry.AddReference(access, a, node->items, c));
        REQUIRE(fixture.registry.AddElement(access, a, node->slots));
        REQUIRE(fixture.registry.SetElementReference(access, a, node->slots, 0,
                                                     node->slots[0].first, b));
        (void)fixture.Take();
        for (uint64 attempt = 1; attempt <= 2; ++attempt) {
            failing.FailOnAttempt(attempt);
            const ErrorCode cleared =
                ErrorOf(fixture.registry.ClearReferences(access, a, node->items));
            failing.DisableFailure();
            CHECK(cleared == ErrorCode::OutOfMemory);
            CHECK(node->items.Size() == 2);
            failing.FailOnAttempt(attempt);
            const ErrorCode removed =
                ErrorOf(fixture.registry.RemoveElementAt(access, a, node->slots, 0));
            failing.DisableFailure();
            CHECK(removed == ErrorCode::OutOfMemory);
            failing.FailOnAttempt(attempt);
            const ErrorCode emptied =
                ErrorOf(fixture.registry.ClearElements(access, a, node->slots));
            failing.DisableFailure();
            CHECK(emptied == ErrorCode::OutOfMemory);
            CHECK(node->slots.Size() == 1);
            CHECK(fixture.observer.PendingCount() == 0);
        }

        Borrow<ObservedNode> secondBorrow;
        ObservedNode* second = fixture.Pin(a2, secondBorrow);
        ProcessingReentry reentry{&fixture, a2, second, c};
        // 다음 Registry 할당(비우기의 집계 표)에서 재진입한다.
        hooked.hook = &LinkDuringTally;
        hooked.context = &reentry;
        REQUIRE(fixture.registry.ClearReferences(access, a, node->items));
        CHECK(reentry.error == ErrorCode::Busy);
        CHECK(node->items.Size() == 0);
        auto changes = fixture.Take();
        REQUIRE(changes.Size() == 2);
        CHECK(Is(changes[0], p, q, -1));
        CHECK(Is(changes[1], p, r, -1));
        REQUIRE(fixture.registry.ClearElements(access, a, node->slots));
        changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(Is(changes[0], p, q, -1));
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 소비자 규칙: 묶음 안 순서는 키가 처음 생긴 순서라 Edit −1 키가 Commit +1 키보다 앞설 수 있다.
// 중간 합(0)으로 판정하지 않고 묶음 전체를 더해야 다중도가 맞다.
TEST(ReferenceObserver_BatchMustBeSummedBeforeJudging) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 12);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle b = fixture.Node(q);
        REQUIRE(fixture.Link(a, b));
        int64 multiplicity = 0;
        for (const ReferenceEdgeDelta& change : fixture.Take()) {
            multiplicity += change.delta;
        }

        CHECK(multiplicity == 1);
        REQUIRE(fixture.Link(a, {}));
        uint64 committed = 0;
        {
            ConstructionContext transaction(fixture.registry, access);
            auto inner = transaction.Create<ObservedNode>(NodeType, a, "Inner");
            REQUIRE(inner);
            auto pointer = transaction.Get<ObservedNode>(inner.Value());
            REQUIRE(pointer);
            REQUIRE(transaction.SetReference(inner.Value(), pointer.Value()->link, b));
            committed = transaction.TransactionId();
            REQUIRE(transaction.Commit(Span<const ObjectHandle>(), Span<RootRef<Object>>()));
        }

        auto batch = fixture.Take();
        REQUIRE(batch.Size() == 2);
        CHECK(Is(batch[0], p, q, -1));
        CHECK(Is(batch[1], p, q, 1, ReferenceChangeCause::Commit, committed));
        // 첫 항목만 더하면 0(잘못된 Closing 판정)이다. 묶음 전체 합은 1이다.
        CHECK(multiplicity + batch[0].delta == 0);
        for (const ReferenceEdgeDelta& change : batch) {
            multiplicity += change.delta;
        }

        CHECK(multiplicity == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 원소 struct가 등록되어 있지 않으면 관찰자 Registry의 원소 제거·비우기는 Unsupported다(배열 불변).
// 기록하지 않는 owner(패키지 객체)나 관찰자 없는 Registry는 조회하지 않으므로 된다.
TEST(ReferenceObserver_UnregisteredElementStructIsUnsupported) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 13);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        Borrow<ObservedNode> borrow;
        ObservedNode* node = fixture.Pin(a, borrow);
        REQUIRE(fixture.registry.AddElement(access, a, node->loose));
        REQUIRE(fixture.registry.AddElement(access, a, node->loose));
        CHECK(ErrorOf(fixture.registry.RemoveElementAt(access, a, node->loose, 0)) ==
              ErrorCode::Unsupported);
        CHECK(ErrorOf(fixture.registry.ClearElements(access, a, node->loose)) ==
              ErrorCode::Unsupported);
        CHECK(node->loose.Size() == 2);

        // 정책은 같고 관찰자 없는 Registry는 조회하지 않는다. 패키지 클러스터(GC-S7)를 켜면 소속
        // 소유자의 원소 제거도 원소 Trace가 필요하므로(13.4절) 이 비교는 클러스터를 끈다.
        RegistryOptions options;
        options.policy.packageNativeType = NativeTypeKey<ObservedPackage>();
        options.packageClusters = false;
        ObjectRegistry plain(allocator, fixture.types, {0x4f623d, 2}, options);
        const auto plainAccess = plain.OwnerAccess();
        auto package = plain.NewObject<ObservedPackage>(plainAccess, PackageType);
        REQUIRE(package);
        auto other =
            plain.NewObject<ObservedNode>(plainAccess, NodeType, package.Value().Handle(), {});
        REQUIRE(other);
        {
            auto resolved = plain.Resolve<ObservedNode>(plainAccess, other.Value().Handle());
            REQUIRE(resolved);
            auto pointer = resolved.Value().Get();
            REQUIRE(pointer);
            REQUIRE(plain.AddElement(plainAccess, other.Value().Handle(), pointer.Value()->loose));
            REQUIRE(plain.RemoveElementAt(plainAccess, other.Value().Handle(),
                                          pointer.Value()->loose, 0));
        }

        REQUIRE(plain.Discard(plainAccess, package.Value().Handle()));
        package.Value().Reset();
        other.Value().Reset();
        REQUIRE(plain.Shutdown(plainAccess));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// AcquireStructByNative: struct native 키로 등록 struct를 연다(클래스 키·미등록 NotFound, null
// InvalidArgument, 종료 뒤 ShuttingDown). 원소를 지우는 호출자가 넘기는 NativeTypeKey<S>()와 같은
// 키다.
TEST(TypeRegistry_AcquireStructByNativeFindsTheElementStruct) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        const void* key = NativeTypeKey<EdgeSlot>();
        {
            auto lease = types.AcquireStructByNative(key);
            REQUIRE(lease);
            CHECK(lease.Value().Kind() == TypeKind::Struct);
            CHECK(lease.Value().Id() == SlotType);
            CHECK(lease.Value().StructOps().trace != nullptr);
        }

        CHECK(types.AcquireStructByNative(nullptr).Error() == ErrorCode::InvalidArgument);
        CHECK(types.AcquireStructByNative(NativeTypeKey<LooseSlot>()).Error() ==
              ErrorCode::NotFound);
        CHECK(types.AcquireStructByNative(NativeTypeKey<ObservedNode>()).Error() ==
              ErrorCode::NotFound);
        REQUIRE(types.Shutdown());
        CHECK(types.AcquireStructByNative(key).Error() == ErrorCode::ShuttingDown);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 다중 간선 지점의 관찰자 예약 실패(관찰자 allocator)와 Commit 집계 실패(Registry allocator)는
// OutOfMemory이고 배열·관찰자·공개 상태가 그대로다.
TEST(ReferenceObserver_MultiEdgeReserveAndCommitTallyFailuresLeaveStateUnchanged) {
    SystemAllocator system;
    test::NFailAllocator observerFailing(system);
    test::NFailAllocator registryFailing(system);
    {
        Fixture fixture(system, 14, ReferenceObserver::DefaultMaxKeys, &observerFailing,
                        &registryFailing);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle b = fixture.Node(q);
        {
            Borrow<ObservedNode> borrow;
            ObservedNode* node = fixture.Pin(a, borrow);
            REQUIRE(fixture.registry.AddReference(access, a, node->items, b));
            REQUIRE(fixture.registry.AddReference(access, a, node->items, b));
            REQUIRE(fixture.registry.AddElement(access, a, node->slots));
            REQUIRE(fixture.registry.SetElementReference(access, a, node->slots, 0,
                                                         node->slots[0].first, b));
            for (uint64 attempt = 1; attempt <= 2; ++attempt) {
                (void)fixture.Take(); // 관찰자 공간을 비운다(다음 기록이 예약해야 한다)
                observerFailing.FailOnAttempt(attempt);
                const ErrorCode cleared =
                    ErrorOf(fixture.registry.ClearReferences(access, a, node->items));
                observerFailing.FailOnAttempt(attempt);
                const ErrorCode removed =
                    ErrorOf(fixture.registry.RemoveElementAt(access, a, node->slots, 0));
                observerFailing.DisableFailure();
                CHECK(cleared == ErrorCode::OutOfMemory);
                CHECK(removed == ErrorCode::OutOfMemory);
                CHECK(node->items.Size() == 2);
                CHECK(node->slots.Size() == 1);
                CHECK(fixture.observer.PendingCount() == 0);
            }
        }

        for (uint64 attempt = 1; attempt <= 2; ++attempt) {
            ConstructionContext transaction(fixture.registry, access);
            auto inner = transaction.Create<ObservedNode>(NodeType, a, "Inner");
            REQUIRE(inner);
            auto pointer = transaction.Get<ObservedNode>(inner.Value());
            REQUIRE(pointer);
            REQUIRE(transaction.SetReference(inner.Value(), pointer.Value()->link, b));
            registryFailing.FailOnAttempt(attempt);
            const ErrorCode committed =
                ErrorOf(transaction.Commit(Span<const ObjectHandle>(), Span<RootRef<Object>>()));
            registryFailing.DisableFailure();
            CHECK(committed == ErrorCode::OutOfMemory);
            CHECK(fixture.registry.Stats().reservedObjects == 1);
            CHECK(fixture.registry.FindChild(access, a, "Inner").Error() == ErrorCode::NotFound);
            CHECK(fixture.observer.PendingCount() == 0);
        }

        CHECK(fixture.registry.Stats().reservedObjects == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

struct ContainerReentry {
    Fixture* fixture;
    ObjectHandle owner;
    ObservedNode* node;
    ErrorCode first = ErrorCode::None;
    ErrorCode second = ErrorCode::None;
};

// 예약 콜백: 같은 배열의 0번 원소(같은 패키지 대상이라 기록이 없다)를 지운다.
void RemoveFirstItem(void* raw) noexcept {
    auto& reentry = *static_cast<ContainerReentry*>(raw);
    reentry.first = ErrorOf(reentry.fixture->registry.RemoveReferenceAt(
        reentry.fixture->Access(), reentry.owner, reentry.node->items, 0));
}

// 예약 콜백: 같은 StructRefArray의 0번 원소를 지우고 기본 원소를 다시 만든다(같은 블록 주소가
// 재사용될 수 있다).
void RecreateFirstSlot(void* raw) noexcept {
    auto& reentry = *static_cast<ContainerReentry*>(raw);
    const auto access = reentry.fixture->Access();
    reentry.first = ErrorOf(
        reentry.fixture->registry.RemoveElementAt(access, reentry.owner, reentry.node->slots, 0));
    auto added = reentry.fixture->registry.AddElement(access, reentry.owner, reentry.node->slots);
    reentry.second = added ? ErrorCode::None : added.Error();
}

} // namespace

// 단일 간선 지점의 예약 뒤 재시도: 예약 콜백이 위치를 없애면(index가 size 이상) Busy이고, 원소를
// 지우고 다시 만들면(같은 블록 주소 재사용 — ABA) 컨테이너 변경 계수로 가려 Busy다. 어느 경우도
// 기록·쓰기가 없다.
TEST(ReferenceObserver_RetryAfterReservationRejectsChangedContainers) {
    SystemAllocator system;
    HookAllocator hooked(system);
    {
        Fixture fixture(system, 15, ReferenceObserver::DefaultMaxKeys, &hooked);
        const auto access = fixture.Access();
        const ObjectHandle p = fixture.Package();
        const ObjectHandle q = fixture.Package();
        const ObjectHandle r = fixture.Package();
        const ObjectHandle a = fixture.Node(p);
        const ObjectHandle same = fixture.Node(a, "Same");
        const ObjectHandle b = fixture.Node(q);
        const ObjectHandle c = fixture.Node(r);
        Borrow<ObservedNode> borrow;
        ObservedNode* node = fixture.Pin(a, borrow);
        ContainerReentry reentry{&fixture, a, node};

        // SetReferenceAt(0, b): 새 키 (P, Q)를 예약하는 중 0번(same)이 지워져 위치가 사라진다.
        REQUIRE(fixture.registry.AddReference(access, a, node->items, same));
        (void)fixture.Take();
        hooked.hook = &RemoveFirstItem;
        hooked.context = &reentry;
        CHECK(ErrorOf(fixture.registry.SetReferenceAt(access, a, node->items, 0, b)) ==
              ErrorCode::Busy);
        CHECK(reentry.first == ErrorCode::None);
        CHECK(node->items.Size() == 0);
        CHECK(fixture.observer.PendingCount() == 0);

        // RemoveReferenceAt(1): (P, R) −1을 예약하는 중 0번이 지워져 1번 위치가 사라진다(c는
        // 남는다).
        REQUIRE(fixture.registry.AddReference(access, a, node->items, same));
        REQUIRE(fixture.registry.AddReference(access, a, node->items, c));
        (void)fixture.Take();
        hooked.hook = &RemoveFirstItem;
        CHECK(ErrorOf(fixture.registry.RemoveReferenceAt(access, a, node->items, 1)) ==
              ErrorCode::Busy);
        CHECK(node->items.Size() == 1 && node->items[0] == c);
        CHECK(fixture.observer.PendingCount() == 0);

        // SetElementReference(0): 예약 중 원소를 지우고 다시 만들면 Busy다(새 원소는 null 그대로).
        REQUIRE(fixture.registry.AddElement(access, a, node->slots));
        (void)fixture.Take();
        hooked.hook = &RecreateFirstSlot;
        CHECK(ErrorOf(fixture.registry.SetElementReference(
                  access, a, node->slots, 0, node->slots[0].first, b)) == ErrorCode::Busy);
        CHECK(reentry.first == ErrorCode::None && reentry.second == ErrorCode::None);
        CHECK(node->slots.Size() == 1);
        CHECK(node->slots[0].first.Handle() == ObjectHandle{});
        CHECK(fixture.observer.PendingCount() == 0);
        hooked.hook = nullptr;
        REQUIRE(fixture.registry.ClearReferences(access, a, node->items));
    }

    CHECK(system.Stats().liveAllocations == 0);
}
} // namespace reference_observer_tests_file

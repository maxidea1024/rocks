#include <Core/Memory/SystemAllocator.h>
#include <Object/ObjectBatchPublish.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 리플렉션 B9i2 묶음 게시 구간 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.2절, 17절 B9i의
// Publish 원자성): 변경 표지, BeginBatchPublish의 재확인(Discard·GC 회수·타입·다른
// Registry·Borrow)과 실패 원자성, 관찰자 묶음 예약(할당 실패·키 상한·예약 콜백 재진입), 구간 안
// Busy, 준비 단위 게시 (단일 참조 위치·StructRefArray 원소 멤버·ObjectRefArray 버퍼·ObjectRefMap
// 내용)와 관찰자 한 번 기록, Marking 중 삽입 장벽. 순서는 모두 명시 호출과 allocator 시도 번호로
// 정한다(시간에 기대지 않는다).
namespace batch_fixture {

using namespace rocks;

struct Target final : Object {
    void Trace(TraceVisitor&) const noexcept {
    }
};

struct OtherTarget final : Object {
    void Trace(TraceVisitor&) const noexcept {
    }
};

struct BatchSlot {
    explicit BatchSlot(Allocator&) noexcept {
    }

    ObjectRef<Target> first;
};

struct BatchNode final : Object {
    explicit BatchNode(Allocator& allocator) noexcept
        : items(allocator), slots(allocator), map(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    ObjectRef<Target> link;
    ObjectRefArray<Target> items;
    StructRefArray<BatchSlot> slots;
    ObjectRefMap<uint32, Target> map;
};

struct BatchPackage final : Object {
    void Trace(TraceVisitor&) const noexcept {
    }
};

} // namespace batch_fixture

namespace rocks {

template <>
struct GeneratedType<batch_fixture::BatchSlot> {
    static constexpr bool ContainsStrongReference = true;

    static void Trace(const batch_fixture::BatchSlot& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.first);
    }
};

} // namespace rocks

void batch_fixture::BatchNode::Trace(TraceVisitor& visitor) const noexcept {
    visitor.Visit(link);
    items.Trace(visitor);
    slots.Trace(visitor);
    map.Trace(visitor);
}

using namespace rocks;

namespace {

using namespace batch_fixture;

constexpr TypeId PackageType{0x4239495250414b31ULL, 1};
constexpr TypeId NodeType{0x423949524e4f4431ULL, 1};
constexpr TypeId TargetType{0x4239495254475431ULL, 1};
constexpr TypeId OtherType{0x423949524f544831ULL, 1};

template <class T>
RuntimeTypeOps CompleteOps() noexcept {
    RuntimeTypeOps ops = MakeRuntimeTypeOps<T>();
    ops.completeFieldLayout = true;
    return ops;
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 한 시험의 Registry 묶음이다. observed면 패키지 정책 + 관찰자 Registry(객체는 패키지 아래), 아니면
// 정책 없는 Registry(Outer 없는 객체 — GC 회수 시험용)다.
struct Fixture {
    Fixture(Allocator& allocator, uint64 id, bool observed,
            usize maxKeys = ReferenceObserver::DefaultMaxKeys,
            Allocator* observerAllocator = nullptr, Allocator* registryAllocator = nullptr) noexcept
        : allocator(allocator),
          observerAllocator(observerAllocator != nullptr ? *observerAllocator : allocator),
          observed(observed), types(allocator), observer(this->observerAllocator, maxKeys),
          registry(registryAllocator != nullptr ? *registryAllocator : allocator, types,
                   {0x423969, id}, Options(observer, observed)),
          roots(allocator) {
        const bool registered =
            types.Register({PackageType, 1, {}, {}}, CompleteOps<BatchPackage>()).IsOk() &&
            types.Register({NodeType, 1, {}, {}}, CompleteOps<BatchNode>()).IsOk() &&
            types.Register({TargetType, 1, {}, {}}, CompleteOps<Target>()).IsOk() &&
            types.Register({OtherType, 1, {}, {}}, CompleteOps<OtherTarget>()).IsOk();
        if (!registered) {
            platform::Fatal("batch fixture type registration failed");
        }
    }

    ~Fixture() {
        for (RootRef<Object>& root : roots) {
            if (observed && root.IsValid()) {
                (void)registry.Discard(registry.OwnerAccess(), root.Handle());
            }

            root.Reset();
        }

        if (!registry.Shutdown(registry.OwnerAccess())) {
            platform::Fatal("batch fixture registry did not shut down");
        }
    }

    static RegistryOptions Options(ReferenceObserver& observer, bool observed) noexcept {
        RegistryOptions options;
        if (observed) {
            options.policy.packageNativeType = NativeTypeKey<BatchPackage>();
            options.referenceObserver = &observer;
        }

        return options;
    }

    ObjectAccessContext Access() const noexcept {
        return registry.OwnerAccess();
    }

    ObjectHandle Keep(Result<RootRef<Object>> made) noexcept {
        if (!made || !roots.TryPushBack(Move(made.Value()))) {
            platform::Fatal("batch fixture object failed");
        }

        return roots[roots.Size() - 1].Handle();
    }

    ObjectHandle Package() noexcept {
        return Keep(registry.NewObject<Object>(Access(), PackageType));
    }

    // observed면 outer 아래(이름 name), 아니면 Outer 없는 객체다. rooted가 거짓이면 RootRef를
    // 놓는다.
    ObjectHandle Make(TypeId type, ObjectHandle outer, StringView name, bool rooted) noexcept {
        auto made = observed ? registry.NewObject<Object>(Access(), type, outer, name)
                             : registry.NewObject<Object>(Access(), type);
        if (!made) {
            platform::Fatal("batch fixture object failed");
        }

        const ObjectHandle handle = made.Value().Handle();
        if (rooted && !roots.TryPushBack(Move(made.Value()))) {
            platform::Fatal("batch fixture root failed");
        }

        return handle;
    }

    Result<uint64> Stamp(ObjectHandle owner) const noexcept {
        return registry.MutationStamp(Access(), owner);
    }

    Array<ReferenceEdgeDelta> Take() noexcept {
        Array<ReferenceEdgeDelta> changes(observerAllocator);
        if (!observer.TakeChanges(changes)) {
            platform::Fatal("batch fixture take failed");
        }

        return changes;
    }

    Allocator& allocator;
    Allocator& observerAllocator;
    bool observed;
    TypeRegistry types;
    ReferenceObserver observer;
    ObjectRegistry registry;
    Array<RootRef<Object>> roots;
};

BatchNode* Open(Fixture& fixture, ObjectHandle node, Borrow<BatchNode>& borrow) noexcept {
    auto resolved = fixture.registry.Resolve<BatchNode>(fixture.Access(), node);
    if (!resolved) {
        platform::Fatal("batch fixture resolve failed");
    }

    borrow = Move(resolved.Value());
    auto pointer = borrow.Get();
    if (!pointer) {
        platform::Fatal("batch fixture borrow failed");
    }

    return pointer.Value();
}

// 할당 시도 하나에서 hook을 한 번 실행한다(관찰자 예약 콜백 재진입 시험).
class HookAllocator final : public Allocator {
public:
    using Hook = void (*)(void*) noexcept;

    explicit HookAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        if (hook_ != nullptr) {
            Hook run = hook_;
            hook_ = nullptr;
            run(context_);
        }

        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    void OnNextAllocation(Hook run, void* context) noexcept {
        hook_ = run;
        context_ = context;
    }

private:
    Allocator& backing_;
    Hook hook_ = nullptr;
    void* context_ = nullptr;
};

} // namespace

// 준비 단위 넷을 한 구간에서 게시하고 관찰자에는 선언한 간선 변경을 한 번에 합쳐 기록한다(같은
// 패키지 쌍의 +·-가 상쇄되면 TakeChanges가 뺀다). 표지는 End에서 한 번 오른다.
TEST(BatchPublish_PublishesUnitsAndRecordsObserverOnce) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 1, true);
        const auto access = fixture.Access();
        const ObjectHandle a = fixture.Package();
        const ObjectHandle b = fixture.Package();
        const ObjectHandle c = fixture.Package();
        const ObjectHandle node = fixture.Make(NodeType, a, "Node", false);
        const ObjectHandle a1 = fixture.Make(TargetType, a, "A1", false);
        const ObjectHandle b1 = fixture.Make(TargetType, b, "B1", false);
        const ObjectHandle b2 = fixture.Make(TargetType, b, "B2", false);
        const ObjectHandle c1 = fixture.Make(TargetType, c, "C1", false);
        Borrow<BatchNode> borrow;
        BatchNode* pointer = Open(fixture, node, borrow);
        REQUIRE(fixture.registry.SetReference(access, node, pointer->link, a1));
        REQUIRE(fixture.registry.AddReference(access, node, pointer->items, b1));
        REQUIRE(fixture.registry.AddElement(access, node, pointer->slots));
        REQUIRE(fixture.registry.SetMapReference(access, node, pointer->map, uint32{7}, b1));
        (void)fixture.Take();

        // Prepare: 표지를 읽고 새 버퍼를 만든다.
        const auto stamp = fixture.Stamp(node);
        REQUIRE(stamp);
        PreparedReferenceArray prepared(fixture.registry.ObjectAllocator());
        const ObjectHandle elements[] = {b2, ObjectHandle{}, c1};
        REQUIRE(prepared.TryAssign(Span<const ObjectHandle>(elements, 3)));
        ObjectRefMap<uint32, Target>::MapType preparedMap(pointer->map.GetAllocator());
        REQUIRE(preparedMap.Insert(uint32{8}, c1));
        REQUIRE(preparedMap.Insert(uint32{7}, ObjectHandle{}));

        const BatchTargetCheck targets[] = {ExpectBatchTarget<Target>(b1),
                                            ExpectBatchTarget<Target>(b2),
                                            ExpectBatchTarget<Target>(c1)};
        const BatchEdgeChange edges[] = {{a1, b1},
                                         {b1, ObjectHandle{}},
                                         {ObjectHandle{}, b2},
                                         {ObjectHandle{}, c1},
                                         {ObjectHandle{}, c1},
                                         {b1, ObjectHandle{}},
                                         {ObjectHandle{}, c1}};
        auto batch = fixture.registry.BeginBatchPublish(access, node, stamp.Value(),
                                                        Span<const BatchTargetCheck>(targets, 3),
                                                        Span<const BatchEdgeChange>(edges, 7));
        REQUIRE(batch);
        CHECK(batch.Value().IsActive() && batch.Value().Owner() == node);
        batch.Value().SetReference(pointer->link, b1);
        batch.Value().ReplaceReferences(pointer->items, prepared);
        batch.Value().SetElementReference(pointer->slots, 0, pointer->slots[0].first, c1);
        batch.Value().ReplaceMapReferences(pointer->map, preparedMap);
        batch.Value().End();
        CHECK(!batch.Value().IsActive());

        CHECK(pointer->link.Handle() == b1);
        REQUIRE(pointer->items.Size() == 3);
        CHECK(pointer->items[0] == b2 && pointer->items[1] == ObjectHandle{} &&
              pointer->items[2] == c1);
        CHECK(pointer->slots[0].first.Handle() == c1);
        REQUIRE(pointer->map.Count() == 2);
        CHECK(*pointer->map.Find(uint32{8}) == c1 &&
              *pointer->map.Find(uint32{7}) == ObjectHandle{});
        // 옛 내용은 준비물로 넘어왔다(구간 뒤 호출자가 반환한다).
        CHECK(prepared.View().Size() == 1 && prepared.View()[0] == b1);
        CHECK(preparedMap.Count() == 1);

        // (a, b): +1 -1 +1 -1 = 0이라 빠지고, (a, c): +3만 남는다.
        Array<ReferenceEdgeDelta> changes = fixture.Take();
        REQUIRE(changes.Size() == 1);
        CHECK(changes[0].ownerPackage == a && changes[0].targetPackage == c &&
              changes[0].delta == 3 && changes[0].cause == ReferenceChangeCause::Edit);
        CHECK(fixture.Stamp(node).Value() == stamp.Value() + 1);

        // 옛 표지로는 다시 들어갈 수 없다.
        auto stale = fixture.registry.BeginBatchPublish(access, node, stamp.Value(), {}, {});
        CHECK(ErrorOf(stale) == ErrorCode::Busy);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 변경 표지: 쓰기 Borrow·live 참조·컨테이너 변경마다 오르고 읽기 Borrow·조회는 그대로다. 아무것도
// 게시하지 않고 닫은 구간은 표지를 올리지 않는다.
TEST(BatchPublish_MutationStampTracksWrites) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 2, false);
        const auto access = fixture.Access();
        const ObjectHandle node = fixture.Make(NodeType, {}, {}, true);
        const ObjectHandle target = fixture.Make(TargetType, {}, {}, true);
        const uint64 initial = fixture.Stamp(node).Value();
        {
            Borrow<BatchNode> borrow;
            (void)Open(fixture, node, borrow);
            CHECK(fixture.Stamp(node).Value() == initial);
        }

        {
            auto writable = fixture.registry.ResolveForWrite<BatchNode>(access, node);
            REQUIRE(writable);
        }

        const uint64 written = fixture.Stamp(node).Value();
        CHECK(written > initial);
        Borrow<BatchNode> borrow;
        BatchNode* pointer = Open(fixture, node, borrow);
        REQUIRE(fixture.registry.SetReference(access, node, pointer->link, target));
        const uint64 linked = fixture.Stamp(node).Value();
        CHECK(linked > written);
        REQUIRE(fixture.registry.AddReference(access, node, pointer->items, target));
        const uint64 grown = fixture.Stamp(node).Value();
        CHECK(grown > linked);
        CHECK(ErrorOf(fixture.registry.BeginBatchPublish(access, node, linked, {}, {})) ==
              ErrorCode::Busy);

        auto idle = fixture.registry.BeginBatchPublish(access, node, grown, {}, {});
        REQUIRE(idle);
        idle.Value().End();
        CHECK(fixture.Stamp(node).Value() == grown);
        // 대상 없는 표지 조회는 생존 규칙을 따른다.
        CHECK(ErrorOf(fixture.registry.MutationStamp(access, ObjectHandle{})) ==
              ErrorCode::StaleHandle);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 재확인 실패(Borrow 없음·null·지난 removed·Discard된 대상·타입·다른 Registry·GC 회수)는 모두
// BeginBatchPublish에서 나고 아무것도 바뀌지 않으며 구간도 남지 않는다. 고정(RootRef)한 대상은 GC를
// 넘어 살아 게시된다.
TEST(BatchPublish_RevalidationFailuresChangeNothing) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 3, false);
        Fixture other(allocator, 4, false);
        const auto access = fixture.Access();
        const ObjectHandle node = fixture.Make(NodeType, {}, {}, true);
        const ObjectHandle kept = fixture.Make(TargetType, {}, {}, true);
        const ObjectHandle wrong = fixture.Make(OtherType, {}, {}, true);
        const ObjectHandle foreign = other.Make(TargetType, {}, {}, true);
        const ObjectHandle discarded = fixture.Make(TargetType, {}, {}, false);
        auto pinnedDiscarded = fixture.registry.TryPin<Object>(access, discarded);
        REQUIRE(pinnedDiscarded);
        REQUIRE(fixture.registry.Discard(access, discarded));
        const ObjectHandle collected = fixture.Make(TargetType, {}, {}, false);
        const ObjectHandle pinned = fixture.Make(TargetType, {}, {}, false);
        auto pin = fixture.registry.TryPin<Target>(access, pinned);
        REQUIRE(pin);
        REQUIRE(fixture.registry.Collect(access));
        const uint64 stamp = fixture.Stamp(node).Value();

        // Borrow 없이는 InvalidArgument다.
        const BatchTargetCheck keptCheck[] = {ExpectBatchTarget<Target>(kept)};
        CHECK(ErrorOf(fixture.registry.BeginBatchPublish(access, node, stamp,
                                                         Span<const BatchTargetCheck>(keptCheck, 1),
                                                         {})) == ErrorCode::InvalidArgument);

        Borrow<BatchNode> borrow;
        BatchNode* pointer = Open(fixture, node, borrow);
        struct Case {
            BatchTargetCheck target;
            BatchEdgeChange edge;
            ErrorCode expected;
        };

        const Case cases[] = {
            {{ObjectHandle{}, nullptr}, {}, ErrorCode::InvalidArgument},
            {ExpectBatchTarget<Target>(kept), {collected, kept}, ErrorCode::InvalidArgument},
            {ExpectBatchTarget<Target>(discarded),
             {ObjectHandle{}, discarded},
             ErrorCode::StaleHandle},
            {ExpectBatchTarget<Target>(wrong), {ObjectHandle{}, wrong}, ErrorCode::TypeMismatch},
            {ExpectBatchTarget<Target>(foreign), {ObjectHandle{}, foreign}, ErrorCode::StaleHandle},
            {ExpectBatchTarget<Target>(collected),
             {ObjectHandle{}, collected},
             ErrorCode::StaleHandle},
            {ExpectBatchTarget<Target>(kept), {ObjectHandle{}, collected}, ErrorCode::StaleHandle},
        };
        for (const Case& item : cases) {
            auto failed = fixture.registry.BeginBatchPublish(
                access, node, stamp, Span<const BatchTargetCheck>(&item.target, 1),
                Span<const BatchEdgeChange>(&item.edge, 1));
            CHECK(ErrorOf(failed) == item.expected);
            // 구간이 남지 않았다(표지 조회가 Busy가 아니다)·필드 불변.
            CHECK(fixture.Stamp(node).Value() == stamp);
            CHECK(pointer->link.Handle() == ObjectHandle{});
        }

        // 고정한 대상은 수집을 넘어 살아 있고 게시된다.
        const BatchTargetCheck pinnedCheck[] = {ExpectBatchTarget<Target>(pinned)};
        const BatchEdgeChange pinnedEdge[] = {{ObjectHandle{}, pinned}};
        auto batch = fixture.registry.BeginBatchPublish(
            access, node, stamp, Span<const BatchTargetCheck>(pinnedCheck, 1),
            Span<const BatchEdgeChange>(pinnedEdge, 1));
        REQUIRE(batch);
        batch.Value().SetReference(pointer->link, pinned);
        batch.Value().End();
        CHECK(pointer->link.Handle() == pinned);

        // owner가 Discard되면 StaleHandle이다.
        const uint64 after = fixture.Stamp(node).Value();
        REQUIRE(fixture.registry.Discard(access, node));
        CHECK(ErrorOf(fixture.registry.BeginBatchPublish(access, node, after, {}, {})) ==
              ErrorCode::StaleHandle);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 구간이 열린 동안 그 Registry의 다른 변경·Borrow·조회·GC 단계·Discard·두 번째 구간은 Busy이고,
// 닫으면 다시 된다. 이동한 구간 객체는 한 번만 닫는다.
TEST(BatchPublish_OpenSectionMakesRegistryBusy) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 5, false);
        const auto access = fixture.Access();
        const ObjectHandle node = fixture.Make(NodeType, {}, {}, true);
        const ObjectHandle target = fixture.Make(TargetType, {}, {}, true);
        Borrow<BatchNode> borrow;
        BatchNode* pointer = Open(fixture, node, borrow);
        const uint64 stamp = fixture.Stamp(node).Value();
        auto opened = fixture.registry.BeginBatchPublish(access, node, stamp, {}, {});
        REQUIRE(opened);
        ObjectBatchPublish batch = Move(opened.Value());
        CHECK(!opened.Value().IsActive() && batch.IsActive());
        CHECK(ErrorOf(fixture.registry.SetReference(access, node, pointer->link, target)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(fixture.registry.Resolve<BatchNode>(access, node)) == ErrorCode::Busy);
        CHECK(ErrorOf(fixture.Stamp(node)) == ErrorCode::Busy);
        CHECK(ErrorOf(fixture.registry.Discard(access, target)) == ErrorCode::Busy);
        CHECK(ErrorOf(fixture.registry.BeginBatchPublish(access, node, stamp, {}, {})) ==
              ErrorCode::Busy);
        batch.End();
        batch.End();
        CHECK(fixture.Stamp(node).Value() == stamp);
        CHECK(fixture.registry.SetReference(access, node, pointer->link, target));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 관찰자 묶음 예약 실패: 관찰자 할당 실패(OutOfMemory)·키 상한(CapacityExceeded)·Registry 집계 할당
// 실패(OutOfMemory)·예약 콜백의 재진입(그 호출은 Busy)에서 아무것도 바뀌지 않고, 기록도 없다.
TEST(BatchPublish_ObserverReservationFailuresChangeNothing) {
    SystemAllocator system;
    test::NFailAllocator observerFailing(system);
    test::NFailAllocator registryFailing(system);
    {
        Fixture fixture(system, 6, true, ReferenceObserver::DefaultMaxKeys, &observerFailing,
                        &registryFailing);
        const auto access = fixture.Access();
        const ObjectHandle a = fixture.Package();
        const ObjectHandle b = fixture.Package();
        const ObjectHandle c = fixture.Package();
        const ObjectHandle node = fixture.Make(NodeType, a, "Node", false);
        const ObjectHandle b1 = fixture.Make(TargetType, b, "B1", false);
        const ObjectHandle c1 = fixture.Make(TargetType, c, "C1", false);
        Borrow<BatchNode> borrow;
        BatchNode* pointer = Open(fixture, node, borrow);
        const uint64 stamp = fixture.Stamp(node).Value();
        const BatchTargetCheck targets[] = {ExpectBatchTarget<Target>(b1),
                                            ExpectBatchTarget<Target>(c1)};
        const BatchEdgeChange edges[] = {{ObjectHandle{}, b1}, {ObjectHandle{}, c1}};
        const auto begin = [&]() noexcept {
            return fixture.registry.BeginBatchPublish(access, node, stamp,
                                                      Span<const BatchTargetCheck>(targets, 2),
                                                      Span<const BatchEdgeChange>(edges, 2));
        };
        const auto unchanged = [&]() noexcept {
            return fixture.Stamp(node) && fixture.Stamp(node).Value() == stamp &&
                   pointer->link.Handle() == ObjectHandle{} && pointer->items.Size() == 0 &&
                   fixture.observer.PendingCount() == 0;
        };

        // 관찰자 공간 할당 실패(먼저 본다 — 성공한 예약은 공간을 남긴다).
        for (uint64 attempt = 1; attempt <= 2; ++attempt) {
            observerFailing.FailOnAttempt(attempt);
            CHECK(ErrorOf(begin()) == ErrorCode::OutOfMemory);
            observerFailing.DisableFailure();
            CHECK(unchanged());
        }

        // Registry 집계 할당(집계 객체·쌍 배열·쌍 색인)의 시도마다 실패시킨다.
        bool reached = false;
        for (uint64 attempt = 1; attempt <= 16 && !reached; ++attempt) {
            registryFailing.FailOnAttempt(attempt);
            auto failed = begin();
            registryFailing.DisableFailure();
            if (failed) {
                reached = true;
                failed.Value().End();
            } else {
                CHECK(failed.Error() == ErrorCode::OutOfMemory);
            }

            CHECK(unchanged());
        }

        CHECK(reached);

        // 예약 콜백 재진입: 그 사이 Registry 변경은 Busy이고 구간은 그대로 열린다.
        HookAllocator hooked(system);
        {
            Fixture reentrant(system, 7, true, ReferenceObserver::DefaultMaxKeys, &hooked);
            const ObjectHandle p = reentrant.Package();
            const ObjectHandle q = reentrant.Package();
            const ObjectHandle owner = reentrant.Make(NodeType, p, "Node", false);
            const ObjectHandle far = reentrant.Make(TargetType, q, "Far", false);
            Borrow<BatchNode> held;
            BatchNode* holder = Open(reentrant, owner, held);
            struct Reentry {
                Fixture* fixture;
                ObjectHandle owner;
                ObjectHandle target;
                BatchNode* holder;
                ErrorCode result;
            } reentry{&reentrant, owner, far, holder, ErrorCode::None};
            hooked.OnNextAllocation(
                [](void* context) noexcept {
                    auto& state = *static_cast<Reentry*>(context);
                    state.result = ErrorOf(state.fixture->registry.SetReference(
                        state.fixture->Access(), state.owner, state.holder->link, state.target));
                },
                &reentry);
            const BatchTargetCheck farCheck[] = {ExpectBatchTarget<Target>(far)};
            const BatchEdgeChange farEdge[] = {{ObjectHandle{}, far}};
            auto batch = reentrant.registry.BeginBatchPublish(
                reentrant.Access(), owner, reentrant.Stamp(owner).Value(),
                Span<const BatchTargetCheck>(farCheck, 1), Span<const BatchEdgeChange>(farEdge, 1));
            REQUIRE(batch);
            CHECK(reentry.result == ErrorCode::Busy);
            batch.Value().SetReference(holder->link, far);
            batch.Value().End();
            Array<ReferenceEdgeDelta> changes = reentrant.Take();
            REQUIRE(changes.Size() == 1);
            CHECK(changes[0].ownerPackage == p && changes[0].targetPackage == q &&
                  changes[0].delta == 1);
        }

        // 키 상한: 새 키 둘이 필요한데 하나만 둘 수 있다.
        Fixture limited(system, 8, true, 1);
        const ObjectHandle p = limited.Package();
        const ObjectHandle q = limited.Package();
        const ObjectHandle r = limited.Package();
        const ObjectHandle owner = limited.Make(NodeType, p, "Node", false);
        const ObjectHandle q1 = limited.Make(TargetType, q, "Q1", false);
        const ObjectHandle r1 = limited.Make(TargetType, r, "R1", false);
        Borrow<BatchNode> held;
        BatchNode* holder = Open(limited, owner, held);
        const BatchTargetCheck twoTargets[] = {ExpectBatchTarget<Target>(q1),
                                               ExpectBatchTarget<Target>(r1)};
        const BatchEdgeChange twoEdges[] = {{ObjectHandle{}, q1}, {ObjectHandle{}, r1}};
        const uint64 limitedStamp = limited.Stamp(owner).Value();
        CHECK(
            ErrorOf(limited.registry.BeginBatchPublish(
                limited.Access(), owner, limitedStamp, Span<const BatchTargetCheck>(twoTargets, 2),
                Span<const BatchEdgeChange>(twoEdges, 2))) == ErrorCode::CapacityExceeded);
        CHECK(limited.Stamp(owner).Value() == limitedStamp &&
              holder->link.Handle() == ObjectHandle{});
        CHECK(limited.observer.PendingCount() == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// Marking 중(owner가 이미 검게 스캔된 뒤) 게시한 새 대상은 삽입 장벽으로 shade되어 이번 수집을
// 넘긴다(루트 없는 대상).
TEST(BatchPublish_ShadesNewTargetsDuringMarking) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator, 9, false);
        const auto access = fixture.Access();
        // owner가 slot 0이라 root snapshot 큐의 맨 앞이다(첫 slice에서 검게 된다). 패딩 루트가
        // Marking을 이어 가게 한다.
        const ObjectHandle node = fixture.Make(NodeType, {}, {}, true);
        (void)fixture.Make(TargetType, {}, {}, true);
        (void)fixture.Make(TargetType, {}, {}, true);
        const ObjectHandle linked = fixture.Make(TargetType, {}, {}, false);
        const ObjectHandle listed = fixture.Make(TargetType, {}, {}, false);
        const ObjectHandle mapped = fixture.Make(TargetType, {}, {}, false);
        const ObjectHandle garbage = fixture.Make(TargetType, {}, {}, false);
        REQUIRE(fixture.registry.BeginCollection(access));
        GcBudget budget;
        budget.maxObjects = 1;
        budget.maxNanoseconds = 0;
        REQUIRE(fixture.registry.StepCollection(access, budget));
        REQUIRE(fixture.registry.CollectionPhase() == GcPhase::Marking);
        {
            Borrow<BatchNode> borrow;
            BatchNode* pointer = Open(fixture, node, borrow);
            PreparedReferenceArray prepared(fixture.registry.ObjectAllocator());
            REQUIRE(prepared.TryAssign(Span<const ObjectHandle>(&listed, 1)));
            ObjectRefMap<uint32, Target>::MapType preparedMap(pointer->map.GetAllocator());
            REQUIRE(preparedMap.Insert(uint32{1}, mapped));
            const BatchTargetCheck targets[] = {ExpectBatchTarget<Target>(linked),
                                                ExpectBatchTarget<Target>(listed),
                                                ExpectBatchTarget<Target>(mapped)};
            const BatchEdgeChange edges[] = {
                {ObjectHandle{}, linked}, {ObjectHandle{}, listed}, {ObjectHandle{}, mapped}};
            auto batch = fixture.registry.BeginBatchPublish(
                access, node, fixture.Stamp(node).Value(), Span<const BatchTargetCheck>(targets, 3),
                Span<const BatchEdgeChange>(edges, 3));
            REQUIRE(batch);
            batch.Value().SetReference(pointer->link, linked);
            batch.Value().ReplaceReferences(pointer->items, prepared);
            batch.Value().ReplaceMapReferences(pointer->map, preparedMap);
            batch.Value().End();
        }

        for (uint32 guard = 0; guard < 4096 && fixture.registry.CollectionPhase() != GcPhase::Idle;
             ++guard) {
            REQUIRE(fixture.registry.StepCollection(access, budget));
        }

        const auto alive = [&](ObjectHandle handle) noexcept {
            auto lifecycle = fixture.registry.InspectLifecycle(access, handle);
            return lifecycle && lifecycle.Value() == ObjectLifecycle::Alive;
        };
        CHECK(alive(linked) && alive(listed) && alive(mapped));
        CHECK(!alive(garbage));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// ReferenceObserver 모델 대조 시험이다. 패키지 정책 Registry에서 임의의 편집·Commit·Discard·GC
// 뒤 관찰자 누계가 객체 내용을 다시 센 패키지 쌍 다중도와 같은지 본다
// (Docs/Design/ObjectFoundations.ko.md 9절 Ob). Registry 자체의 무작위 대조는
// ObjectRegistryModelTests.cpp다.
using namespace rocks;

namespace model_fixture {

// 관찰자 대조 모델의 StructRefArray 원소다(등록 struct — Transient 참조 하나).
struct ModelSlot {
    explicit ModelSlot(Allocator&) noexcept {
    }

    ObjectRef<Object> target;
};

struct EdgeNode final : Object {
    explicit EdgeNode(Allocator& allocator) noexcept : items(allocator), slots(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    ObjectRef<Object> link;
    ObjectRefArray<Object> items;
    StructRefArray<ModelSlot> slots;
};

struct ModelPackage final : Object {};

} // namespace model_fixture

namespace rocks {

template <>
struct GeneratedType<model_fixture::ModelSlot> {
    static constexpr bool ContainsStrongReference = true;

    static void Trace(const model_fixture::ModelSlot& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }
};

} // namespace rocks

void model_fixture::EdgeNode::Trace(TraceVisitor& visitor) const noexcept {
    visitor.Visit(link);
    items.Trace(visitor);
    slots.Trace(visitor);
}

namespace {

using model_fixture::EdgeNode;
using model_fixture::ModelPackage;
using model_fixture::ModelSlot;

constexpr TypeId EdgePackageType{0x52474d4f44454c41ULL, 1};
constexpr TypeId EdgeNodeType{0x52474d4f44454c42ULL, 1};
constexpr TypeId EdgeSlotType{0x52474d4f44454c43ULL, 1};
constexpr Guid EdgeSlotTargetId{0x52474d53ULL, 1};

// 완전한 필드 배치를 선언한 함수표다(Trace가 모든 참조 멤버를 방문한다 — 패키지 정책 Registry의
// 생성 조건).
template <class T>
RuntimeTypeOps CompleteOps() noexcept {
    RuntimeTypeOps ops = MakeRuntimeTypeOps<T>();
    ops.completeFieldLayout = true;
    return ops;
}

bool RegisterEdgeTypes(TypeRegistry& types) noexcept {
    FieldSchema field;
    field.id = EdgeSlotTargetId;
    field.kind = FieldKind::ObjectReference;
    field.flags = static_cast<uint32>(FieldFlags::Transient);
    field.name = Span<const char>("target", 6);
    field.value = ValueType{ValueKind::ObjectReference, {}};
    const RuntimeStructFieldOps fieldOps[] = {{EdgeSlotTargetId}};
    RuntimeStructOps slot = MakeRuntimeStructOps<ModelSlot>();
    slot.trace = [](const void* value, TraceVisitor& visitor) noexcept {
        GeneratedType<ModelSlot>::Trace(*static_cast<const ModelSlot*>(value), visitor);
    };
    slot.fields = Span<const RuntimeStructFieldOps>(fieldOps, 1);
    return types
               .RegisterStruct({EdgeSlotType, 1, Span<const FieldSchema>(&field, 1), {"Slot", 4}},
                               Move(slot))
               .IsOk() &&
           types.Register({EdgePackageType, 1, {}, {}}, CompleteOps<ModelPackage>()).IsOk() &&
           types.Register({EdgeNodeType, 1, {}, {}}, CompleteOps<EdgeNode>()).IsOk();
}

// (소유 패키지, 대상 패키지) 다중도 표다(선형 — 모델 크기가 작다).
struct PairCount {
    ObjectHandle owner;
    ObjectHandle target;
    int64 count = 0;
};

void AddPair(Array<PairCount>& pairs, ObjectHandle owner, ObjectHandle target,
             int64 delta) noexcept {
    for (PairCount& pair : pairs) {
        if (pair.owner == owner && pair.target == target) {
            pair.count += delta;
            return;
        }
    }

    PairCount pair;
    pair.owner = owner;
    pair.target = target;
    pair.count = delta;
    if (!pairs.TryPushBack(pair)) {
        platform::Fatal("observer model pair allocation failed");
    }
}

int64 CountOf(const Array<PairCount>& pairs, ObjectHandle owner, ObjectHandle target) noexcept {
    for (const PairCount& pair : pairs) {
        if (pair.owner == owner && pair.target == target) {
            return pair.count;
        }
    }

    return 0;
}

constexpr uint32 MaxEdgeObjects = 600;

// 관찰자 대조 모델이다. 패키지 정책 Registry에서 임의의 live 편집(단일·다중 간선 지점)·staged
// 쓰기와 Commit·Rollback·Discard·root 해제(Discard 뒤에만)·전체·증분 GC를 섞고, 관찰자에서 꺼낸
// 변화의 누계가 Alive 소유자(Outer 있음)의 실제 필드를 다시 센 패키지 쌍 다중도와 같은지 본다.
// 간선 자체는 모델링하지 않는다(객체에서 읽는다) — 모델은 객체의 소속 패키지·Discard·회수만 안다.
class ObserverModel {
public:
    ObserverModel(Allocator& allocator, ObjectRegistry& registry, ReferenceObserver& observer,
                  uint64 seed) noexcept
        : allocator_(allocator), registry_(registry), observer_(observer),
          access_(registry.OwnerAccess()), seed_(seed), rng_(seed * 0x2545f4914f6cdd1dULL + 7),
          objects_(allocator), roots_(allocator), totals_(allocator) {
    }

    bool Prepare() noexcept {
        return objects_.TryReserve(MaxEdgeObjects).IsOk() &&
               roots_.TryReserve(MaxEdgeObjects).IsOk();
    }

    void Run(uint32 operations) noexcept {
        for (operation_ = 0; operation_ < operations && !failed_; ++operation_) {
            Refresh();
            const uint32 kind = Below(100);
            if (kind < 8) {
                NewPackage();
            } else if (kind < 20) {
                NewMember();
            } else if (kind < 50) {
                Edit();
            } else if (kind < 60) {
                Transaction();
            } else if (kind < 67) {
                DiscardSome();
            } else if (kind < 72) {
                ReleaseDiscardedRoot();
            } else if (kind < 78) {
                if (registry_.CollectionPhase() == GcPhase::Idle) {
                    Expect(registry_.Collect(access_).IsOk(), "Collect", __LINE__);
                }
            } else if (kind < 90) {
                StepGc();
            } else {
                Take();
            }

            if (operation_ % 25 == 24) {
                Verify();
            }
        }
    }

    // 남은 cycle을 마치고 대조한 뒤 모든 패키지를 Discard하고 root를 놓는다(Shutdown 전).
    void Finish() noexcept {
        for (uint32 guard = 0; guard < 1000000 && registry_.CollectionPhase() != GcPhase::Idle;
             ++guard) {
            if (!registry_.StepCollection(access_, {0xffffffffu, 0})) {
                break;
            }
        }

        Verify();
        for (uint32 index = 0; index < Count(); ++index) {
            if (objects_[index].isPackage && Live(index)) {
                Expect(registry_.Discard(access_, objects_[index].handle).IsOk(), "final Discard",
                       __LINE__);
            }

            roots_[index].Reset();
        }

        Expect(registry_.Collect(access_).IsOk(), "final Collect", __LINE__);
        const ObjectStats stats = registry_.Stats();
        Expect(stats.liveObjects == 0 && stats.destroyRequested == 0, "all reclaimed", __LINE__,
               stats.liveObjects, stats.destroyRequested);
        Expect(stats.retiredWithoutDiscard == 0, "retiredWithoutDiscard", __LINE__,
               stats.retiredWithoutDiscard);
    }

    bool Failed() const noexcept {
        return failed_;
    }

    usize Verified() const noexcept {
        return verified_;
    }

private:
    struct Tracked {
        ObjectHandle handle;
        int32 package = -1; // 소속 패키지 위치(패키지는 자기)
        bool isPackage = false;
        bool discarded = false;
        bool gone = false;
    };

    bool Expect(bool ok, const char* what, int line, int64 first = -1, int64 second = -1) noexcept {
        if (ok || failed_) {
            return ok;
        }

        failed_ = true;
        snprintf(message_, sizeof(message_), "ObserverModel seed %llu op %u: %s [%lld, %lld]",
                 static_cast<unsigned long long>(seed_), operation_, what,
                 static_cast<long long>(first), static_cast<long long>(second));
        test::RecordFailure(message_, __FILE__, line);
        return false;
    }

    uint32 Below(uint32 bound) noexcept {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return static_cast<uint32>(rng_ >> 11) % bound;
    }

    uint32 Count() const noexcept {
        return static_cast<uint32>(objects_.Size());
    }

    bool Live(uint32 index) const noexcept {
        return !objects_[index].discarded && !objects_[index].gone;
    }

    // live 객체(members면 패키지 제외)를 임의로 고른다(없으면 -1).
    int32 Pick(bool members) noexcept {
        if (Count() == 0) {
            return -1;
        }

        const uint32 start = Below(Count());
        for (uint32 step = 0; step < Count(); ++step) {
            const uint32 index = (start + step) % Count();
            if (Live(index) && (!members || !objects_[index].isPackage)) {
                return static_cast<int32>(index);
            }
        }

        return -1;
    }

    // 연결 대상: null 또는 Discard되지 않은 객체(패키지 포함).
    ObjectHandle PickTarget() noexcept {
        if (Below(5) == 0) {
            return {};
        }

        const int32 picked = Pick(false);
        return picked < 0 ? ObjectHandle{} : objects_[picked].handle;
    }

    int32 Append(ObjectHandle handle, int32 package, bool isPackage) noexcept {
        Tracked tracked;
        tracked.handle = handle;
        tracked.isPackage = isPackage;
        tracked.package = isPackage ? static_cast<int32>(Count()) : package;
        if (!objects_.TryPushBack(tracked) || !roots_.TryEmplaceBack()) {
            Expect(false, "model allocation", __LINE__);
            return -1;
        }

        return static_cast<int32>(Count() - 1);
    }

    void Refresh() noexcept {
        for (uint32 index = 0; index < Count(); ++index) {
            Tracked& object = objects_[index];
            if (object.gone) {
                continue;
            }

            auto state = registry_.InspectLifecycle(access_, object.handle);
            if (!state && state.Error() == ErrorCode::StaleHandle) {
                object.gone = true;
                // 패키지 정책: Discard되지 않은 객체는 회수되지 않는다.
                Expect(object.discarded, "reclaimed without Discard", __LINE__, index);
            }
        }
    }

    void NewPackage() noexcept {
        if (Count() + 2 > MaxEdgeObjects) {
            return;
        }

        auto package = registry_.NewObject<Object>(access_, EdgePackageType);
        if (!Expect(package.IsOk(), "NewObject package", __LINE__)) {
            return;
        }

        const int32 index = Append(package.Value().Handle(), -1, true);
        if (index < 0) {
            return;
        }

        roots_[index] = Move(package.Value());
        auto asset = registry_.NewObject<Object>(access_, EdgeNodeType, objects_[index].handle, {});
        if (Expect(asset.IsOk(), "NewObject asset", __LINE__)) {
            Append(asset.Value().Handle(), index, false);
        }
    }

    // 대표 에셋이나 그 아래 객체 밑에 이름 있는 객체를 만든다(패키지 바로 아래는 대표 자식뿐).
    void NewMember() noexcept {
        if (Count() + 1 > MaxEdgeObjects) {
            return;
        }

        const int32 outer = Pick(true);
        if (outer < 0) {
            return;
        }

        char name[16];
        snprintf(name, sizeof(name), "n%u", nameCounter_++);
        auto made = registry_.NewObject<Object>(access_, EdgeNodeType, objects_[outer].handle,
                                                StringView(name));
        if (Expect(made.IsOk(), "NewObject member", __LINE__)) {
            Append(made.Value().Handle(), objects_[outer].package, false);
        }
    }

    void Edit() noexcept {
        const int32 owner = Pick(true);
        if (owner < 0) {
            return;
        }

        const ObjectHandle handle = objects_[owner].handle;
        auto borrowed = registry_.Resolve<EdgeNode>(access_, handle);
        if (!Expect(borrowed.IsOk(), "Resolve owner", __LINE__, owner)) {
            return;
        }

        auto pointer = borrowed.Value().Get();
        if (!Expect(pointer.IsOk(), "Borrow owner", __LINE__, owner)) {
            return;
        }

        EdgeNode& node = *pointer.Value();
        Result<void> result = Result<void>::Success();
        const uint32 action = Below(11);
        const ObjectHandle target = PickTarget();
        const auto items = static_cast<uint32>(node.items.Size());
        const auto slots = static_cast<uint32>(node.slots.Size());
        switch (action) {
        case 0:
        case 1:
            result = registry_.SetReference(access_, handle, node.link, target);
            break;
        case 2:
            result = registry_.AddReference(access_, handle, node.items, target);
            break;
        case 3:
            result =
                registry_.InsertReferenceAt(access_, handle, node.items, Below(items + 1), target);
            break;
        case 4:
            if (items != 0) {
                result =
                    registry_.SetReferenceAt(access_, handle, node.items, Below(items), target);
            }

            break;
        case 5:
            if (items != 0) {
                result = registry_.RemoveReferenceAt(access_, handle, node.items, Below(items));
            }

            break;
        case 6:
            if (Below(3) == 0) {
                result = registry_.ClearReferences(access_, handle, node.items);
            }

            break;
        case 7: {
            auto added = registry_.AddElement(access_, handle, node.slots);
            result = added ? Result<void>::Success() : Result<void>::Failure(added.Error());
            break;
        }
        case 8:
            if (slots != 0) {
                const usize at = Below(slots);
                result = registry_.SetElementReference(access_, handle, node.slots, at,
                                                       node.slots[at].target, target);
            }

            break;
        case 9:
            if (slots != 0) {
                result = registry_.RemoveElementAt(access_, handle, node.slots, Below(slots));
            }

            break;
        default:
            if (Below(3) == 0) {
                result = registry_.ClearElements(access_, handle, node.slots);
            }

            break;
        }

        Expect(result.IsOk(), "edit", __LINE__, action,
               result ? 0 : static_cast<int64>(result.Error()));
    }

    // staged 객체(새 패키지와 대표 에셋, 또는 live 객체 아래 이름 있는 객체 1–3개)와 staged 쓰기,
    // 그 뒤 Commit 또는 Rollback.
    void Transaction() noexcept {
        if (Count() + 5 > MaxEdgeObjects) {
            return;
        }

        ConstructionContext transaction(registry_, access_);
        ObjectHandle staged[5];
        bool newPackage[5] = {}; // 새 패키지 소속(아래에서 위치를 얻는다)
        bool stagedIsPackage[5] = {};
        uint32 count = 0;
        int32 hostPackage = -1;
        ObjectHandle host;
        if (Below(3) == 0) {
            auto package = transaction.Create<ModelPackage>(EdgePackageType);
            if (!Expect(package.IsOk(), "staged package", __LINE__)) {
                return;
            }

            stagedIsPackage[count] = true;
            newPackage[count] = true;
            staged[count++] = package.Value();
            auto asset = transaction.Create<EdgeNode>(EdgeNodeType, package.Value());
            if (!Expect(asset.IsOk(), "staged asset", __LINE__)) {
                return;
            }

            newPackage[count] = true;
            staged[count++] = asset.Value();
            host = asset.Value();
        } else {
            const int32 outer = Pick(true);
            if (outer < 0) {
                return;
            }

            hostPackage = objects_[outer].package;
            host = objects_[outer].handle;
        }

        const uint32 extra = 1 + Below(3);
        for (uint32 step = 0; step < extra; ++step) {
            char name[16];
            snprintf(name, sizeof(name), "t%u", nameCounter_++);
            auto made = transaction.Create<EdgeNode>(EdgeNodeType, host, StringView(name));
            if (!Expect(made.IsOk(), "staged member", __LINE__)) {
                return;
            }

            newPackage[count] = hostPackage < 0;
            staged[count++] = made.Value();
        }

        // staged 쓰기: 대상은 live 객체 또는 같은 트랜잭션 객체.
        auto stagedTarget = [&]() noexcept {
            return Below(3) == 0 ? staged[Below(count)] : PickTarget();
        };
        for (uint32 at = 0; at < count && !failed_; ++at) {
            if (stagedIsPackage[at]) {
                continue;
            }

            auto pointer = transaction.Get<EdgeNode>(staged[at]);
            if (!Expect(pointer.IsOk(), "staged Get", __LINE__)) {
                return;
            }

            EdgeNode& node = *pointer.Value();
            Expect(transaction.SetReference(staged[at], node.link, stagedTarget()).IsOk(),
                   "staged SetReference", __LINE__);
            const ObjectHandle targets[] = {stagedTarget(), stagedTarget(), stagedTarget()};
            Expect(transaction
                       .AssignReferences(staged[at], node.items,
                                         Span<const ObjectHandle>(targets, Below(4)))
                       .IsOk(),
                   "staged AssignReferences", __LINE__);
            if (Below(2) == 0) {
                Expect(transaction.AssignElements(staged[at], node.slots, 2).IsOk(),
                       "staged AssignElements", __LINE__);
                Expect(transaction
                           .SetElementReference(staged[at], node.slots, 1, node.slots[1].target,
                                                stagedTarget())
                           .IsOk(),
                       "staged SetElementReference", __LINE__);
            }
        }

        if (failed_ || Below(5) == 0) {
            transaction.Rollback();
            return;
        }

        // 새 패키지는 root로 붙잡는다(패키지 정책 — Discard 전에는 놓지 않는다).
        const ObjectHandle roots[] = {staged[0]};
        RootRef<Object> held[1];
        const usize rootCount = stagedIsPackage[0] ? 1 : 0;
        auto committed = transaction.Commit(Span<const ObjectHandle>(roots, rootCount),
                                            Span<RootRef<Object>>(held, rootCount));
        if (!Expect(committed.IsOk(), "Commit", __LINE__)) {
            return;
        }

        int32 packageIndex = hostPackage;
        for (uint32 at = 0; at < count; ++at) {
            const int32 index = Append(staged[at], newPackage[at] ? packageIndex : hostPackage,
                                       stagedIsPackage[at]);
            if (index < 0) {
                return;
            }

            if (stagedIsPackage[at]) {
                packageIndex = index;
                roots_[index] = Move(held[0]);
            }
        }
    }

    // live 객체(패키지 포함)를 Discard하고 부분 트리(Discard되지 않은 자손)를 모델에 표시한다.
    void DiscardSome() noexcept {
        const int32 picked = Pick(Below(3) != 0);
        if (picked < 0) {
            return;
        }

        // 표시는 Discard 전에 정한다(Outer 조회는 Discard 뒤에도 되지만 부분 트리는 지금 기준이다).
        Array<uint32> subtree(allocator_);
        for (uint32 index = 0; index < Count(); ++index) {
            if (Live(index) && Descends(index, picked) && !subtree.TryPushBack(index)) {
                Expect(false, "model allocation", __LINE__);
                return;
            }
        }

        if (!Expect(registry_.Discard(access_, objects_[picked].handle).IsOk(), "Discard", __LINE__,
                    picked)) {
            return;
        }

        for (const uint32 index : subtree) {
            objects_[index].discarded = true;
        }
    }

    // index의 Outer 사슬에 picked가 있는지다(Registry의 GetOuter로 — 모델은 Outer 위치를 두지
    // 않는다).
    bool Descends(uint32 index, int32 picked) noexcept {
        ObjectHandle current = objects_[index].handle;
        for (uint32 guard = 0; guard < 64 && !current.IsNull(); ++guard) {
            if (current == objects_[picked].handle) {
                return true;
            }

            auto outer = registry_.GetOuter(access_, current);
            if (!outer) {
                return false;
            }

            current = outer.Value();
        }

        return false;
    }

    void ReleaseDiscardedRoot() noexcept {
        for (uint32 index = 0; index < Count(); ++index) {
            if (objects_[index].isPackage && objects_[index].discarded && roots_[index].IsValid()) {
                roots_[index].Reset();
                return;
            }
        }
    }

    void StepGc() noexcept {
        if (registry_.CollectionPhase() == GcPhase::Idle) {
            Expect(registry_.BeginCollection(access_).IsOk(), "BeginCollection", __LINE__);
            return;
        }

        Expect(registry_.StepCollection(access_, {1 + Below(6), 0}).IsOk(), "StepCollection",
               __LINE__);
    }

    void Take() noexcept {
        Array<ReferenceEdgeDelta> changes(allocator_);
        if (!Expect(observer_.TakeChanges(changes).IsOk(), "TakeChanges", __LINE__)) {
            return;
        }

        for (const ReferenceEdgeDelta& change : changes) {
            Expect(change.delta != 0, "zero delta taken", __LINE__);
            Expect((change.cause == ReferenceChangeCause::Commit) == (change.transaction != 0),
                   "transaction only on Commit", __LINE__, static_cast<int64>(change.cause));
            Expect(!(change.ownerPackage == change.targetPackage), "same-package change", __LINE__);
            AddPair(totals_, change.ownerPackage, change.targetPackage, change.delta);
        }
    }

    int32 IndexOf(ObjectHandle handle) const noexcept {
        for (uint32 index = 0; index < Count(); ++index) {
            if (objects_[index].handle == handle) {
                return static_cast<int32>(index);
            }
        }

        return -1;
    }

    // Alive 소유자(패키지가 아닌, Discard되지 않은 객체)의 실제 간선을 다시 세어 관찰자 누계와
    // 비교한다.
    void Verify() noexcept {
        Refresh();
        Take();
        Array<PairCount> recount(allocator_);
        for (uint32 index = 0; index < Count() && !failed_; ++index) {
            const Tracked& owner = objects_[index];
            if (owner.isPackage || !Live(index)) {
                continue;
            }

            auto borrowed = registry_.Resolve<EdgeNode>(access_, owner.handle);
            if (!Expect(borrowed.IsOk(), "Resolve for recount", __LINE__, index)) {
                return;
            }

            auto pointer = borrowed.Value().Get();
            if (!Expect(pointer.IsOk(), "Borrow for recount", __LINE__, index)) {
                return;
            }

            const ObjectHandle ownerPackage = objects_[owner.package].handle;
            auto count = [&](ObjectHandle target) noexcept {
                if (target.IsNull()) {
                    return;
                }

                const int32 at = IndexOf(target);
                if (!Expect(at >= 0, "unknown target", __LINE__, index)) {
                    return;
                }

                const ObjectHandle targetPackage = objects_[objects_[at].package].handle;
                if (!(targetPackage == ownerPackage)) {
                    AddPair(recount, ownerPackage, targetPackage, 1);
                }
            };
            const EdgeNode& node = *pointer.Value();
            count(node.link.Handle());
            for (const ObjectHandle item : node.items.View()) {
                count(item);
            }

            for (usize at = 0; at < node.slots.Size(); ++at) {
                count(node.slots[at].target.Handle());
            }
        }

        for (const PairCount& pair : recount) {
            Expect(CountOf(totals_, pair.owner, pair.target) == pair.count, "recount mismatch",
                   __LINE__, pair.count, CountOf(totals_, pair.owner, pair.target));
        }

        for (const PairCount& pair : totals_) {
            Expect(CountOf(recount, pair.owner, pair.target) == pair.count, "observer total extra",
                   __LINE__, pair.count, CountOf(recount, pair.owner, pair.target));
        }

        verified_ += recount.Size();
    }

    Allocator& allocator_;
    ObjectRegistry& registry_;
    ReferenceObserver& observer_;
    ObjectAccessContext access_;
    uint64 seed_;
    uint64 rng_;
    Array<Tracked> objects_;
    Array<RootRef<Object>> roots_;
    Array<PairCount> totals_;
    usize verified_ = 0;
    uint32 operation_ = 0;
    uint32 nameCounter_ = 0;
    bool failed_ = false;
    char message_[256] = {};
};

} // namespace

// 패키지 정책 Registry의 무작위 편집·Commit·Discard·GC 뒤 관찰자 누계 = 객체 내용을 다시 센
// 패키지 쌍 다중도다(속성 시험, 고정 seed 4개 × 연산 600개). 소속 객체는 Discard 없이 회수되지
// 않는다(assertRetiredWithoutDiscard 켬).
TEST(ObjectRegistryModel_ObserverTotalsMatchRecountedEdges) {
    usize verified = 0;
    for (uint64 seed = 1; seed <= 4; ++seed) {
        SystemAllocator allocator;
        {
            TypeRegistry types(allocator);
            REQUIRE(RegisterEdgeTypes(types));
            ReferenceObserver observer(allocator);
            RegistryOptions options;
            options.policy.packageNativeType = NativeTypeKey<ModelPackage>();
            options.referenceObserver = &observer;
            ObjectRegistry registry(allocator, types, {0x52474d54, seed}, options);
            {
                ObserverModel model(allocator, registry, observer, seed);
                REQUIRE(model.Prepare());
                model.Run(600);
                model.Finish();
                verified += model.Verified();
                CHECK(!model.Failed());
            }

            REQUIRE(registry.Shutdown(registry.OwnerAccess()));
            REQUIRE(types.Shutdown());
        }

        CHECK(allocator.Stats().liveAllocations == 0);
    }

    // 대조가 실제 간선을 보았다(빈 표끼리의 비교가 아니다).
    CHECK(verified > 100);
}

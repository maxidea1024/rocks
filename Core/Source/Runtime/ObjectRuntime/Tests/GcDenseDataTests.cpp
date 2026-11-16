#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>

// GC 밀집 자료(GC-S3)와 잎 타입(M2) 시험이다(Docs/Design/GarbageCollectionScale.ko.md).
// - 잎 타입(RuntimeTypeOps::hasStrongReferences == false)은 표식만 하고 trace하지 않지만 그 Outer
//   사슬은 살린다(정지형·증분 모두).
// - 루트 목록은 루트 추가·반환(0 ↔ 1)을 따라가며, 표식 결과가 루트 수와 그래프로만 정해진다.
namespace {

using namespace rocks;

constexpr TypeId DenseNodeType{0x4744656e73653031ULL, 1};
constexpr TypeId DenseLeafType{0x4744656e73653032ULL, 1};

uint32 leafTraces = 0;

class DenseNode final : public Object {
public:
    DenseNode() noexcept = default;

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(next);
    }

    ObjectRef<Object> next;
};

// 강한 참조가 없는 타입이다. Trace는 불린 횟수만 센다(GC는 부르지 않아야 한다).
class DenseLeaf final : public Object {
public:
    DenseLeaf() noexcept = default;

    void Trace(TraceVisitor&) const noexcept {
        ++leafTraces;
    }
};

bool RegisterDenseTypes(TypeRegistry& types) noexcept {
    RuntimeTypeOps leaf = MakeRuntimeTypeOps<DenseLeaf>();
    leaf.hasStrongReferences = false;
    return types.Register(TypeSchema{DenseNodeType, 1, {}, {}}, MakeRuntimeTypeOps<DenseNode>()) &&
           types.Register(TypeSchema{DenseLeafType, 1, {}, {}}, Move(leaf));
}

bool Alive(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto lifecycle = objects.InspectLifecycle(objects.OwnerAccess(), handle);
    return lifecycle && lifecycle.Value() == ObjectLifecycle::Alive;
}

bool Reclaimed(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto lifecycle = objects.InspectLifecycle(objects.OwnerAccess(), handle);
    return !lifecycle && lifecycle.Error() == ErrorCode::StaleHandle;
}

// root(노드) → leaf. leaf의 Outer 사슬은 inner → outer(노드) → top(노드)이다. outer·top은 루트가
// 없고 leaf를 통해서만 산다. incremental이면 Begin·Step(객체 하나씩), 아니면 Collect다.
void RunLeafKeepsOuterChain(bool incremental) noexcept {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterDenseTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4744, incremental ? 1u : 2u});
        const auto access = objects.OwnerAccess();
        auto root = objects.NewObject<DenseNode>(access, DenseNodeType);
        auto top = objects.NewObject<Object>(access, DenseNodeType, {}, "Top");
        REQUIRE(root && top);
        auto inner =
            objects.NewObject<Object>(access, DenseNodeType, top.Value().Handle(), "Inner");
        REQUIRE(inner);
        auto leaf =
            objects.NewObject<Object>(access, DenseLeafType, inner.Value().Handle(), "Leaf");
        auto loose = objects.NewObject<Object>(access, DenseLeafType);
        REQUIRE(leaf && loose);
        const ObjectHandle topHandle = top.Value().Handle();
        const ObjectHandle innerHandle = inner.Value().Handle();
        const ObjectHandle leafHandle = leaf.Value().Handle();
        const ObjectHandle looseHandle = loose.Value().Handle();
        {
            auto borrowed = objects.Resolve<DenseNode>(access, root.Value().Handle());
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            REQUIRE(objects.SetReference(access, root.Value().Handle(), pointer.Value()->next,
                                         leafHandle));
        }

        top.Value().Reset();
        inner.Value().Reset();
        leaf.Value().Reset();
        loose.Value().Reset();
        leafTraces = 0;
        if (incremental) {
            REQUIRE(objects.BeginCollection(access));
            for (uint32 guard = 0; guard < 64 && objects.CollectionPhase() != GcPhase::Idle;
                 ++guard) {
                REQUIRE(objects.StepCollection(access, GcBudget{1, 0}));
            }

            CHECK(objects.CollectionPhase() == GcPhase::Idle);
        } else {
            REQUIRE(objects.Collect(access));
        }

        // 잎은 trace되지 않고, 표식 수에는 잎도 든다(root·leaf·inner·top).
        CHECK(leafTraces == 0);
        CHECK(objects.LastGcReport().marked == 4);
        CHECK(Alive(objects, leafHandle));
        CHECK(Alive(objects, innerHandle));
        CHECK(Alive(objects, topHandle));
        CHECK(Reclaimed(objects, looseHandle));
        root.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(Reclaimed(objects, leafHandle));
        CHECK(Reclaimed(objects, innerHandle));
        CHECK(Reclaimed(objects, topHandle));
        CHECK(objects.Stats().liveObjects == 0);
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace

TEST(ObjectGc_LeafTypeIsMarkedWithoutTraceAndKeepsOuterChain) {
    RunLeafKeepsOuterChain(false);
}

TEST(ObjectGc_IncrementalLeafTypeIsMarkedWithoutTraceAndKeepsOuterChain) {
    RunLeafKeepsOuterChain(true);
}

// 루트 목록이 루트 추가·복제·반환을 따라간다: 같은 객체의 루트가 여럿이어도 한 번만 표식하고,
// 마지막 루트를 놓은 객체는 다음 수집에서 회수된다. 수집 사이에 슬롯이 재사용되어도 목록이 옛
// 슬롯을 가리키지 않는다(Debug Stats가 목록과 슬롯을 대조한다).
TEST(ObjectGc_RootListFollowsRootTransitions) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterDenseTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4745, 1});
        const auto access = objects.OwnerAccess();
        constexpr uint32 Count = 200;
        RootRef<Object> roots[Count];
        RootRef<Object> extra[Count];
        for (uint32 index = 0; index < Count; ++index) {
            auto created = objects.NewObject<Object>(access, DenseNodeType);
            REQUIRE(created);
            roots[index] = Move(created.Value());
            if (index % 3 == 0) {
                auto clone = roots[index].Clone();
                REQUIRE(clone);
                extra[index] = Move(clone.Value());
            }
        }

        auto collected = objects.Collect(access);
        REQUIRE(collected);
        CHECK(collected.Value().marked == Count);
        // 짝수 번째의 루트를 모두 놓는다(복제가 있으면 복제도).
        for (uint32 index = 0; index < Count; index += 2) {
            roots[index].Reset();
            extra[index].Reset();
        }

        uint32 heldRoots = 0;
        for (uint32 index = 1; index < Count; index += 2) {
            heldRoots += index % 3 == 0 ? 2u : 1u;
        }

        CHECK(objects.Stats().roots == heldRoots);
        collected = objects.Collect(access);
        REQUIRE(collected);
        CHECK(collected.Value().marked == Count / 2);
        CHECK(collected.Value().reclaimed == Count / 2);
        // 회수된 슬롯을 재사용하는 새 루트 객체도 목록에 든다.
        for (uint32 index = 0; index < Count; index += 2) {
            auto created = objects.NewObject<Object>(access, DenseNodeType);
            REQUIRE(created);
            roots[index] = Move(created.Value());
        }

        REQUIRE(objects.BeginCollection(access));
        for (uint32 guard = 0; guard < 4 * Count && objects.CollectionPhase() != GcPhase::Idle;
             ++guard) {
            REQUIRE(objects.StepCollection(access, GcBudget{7, 0}));
        }

        CHECK(objects.LastGcReport().marked == Count);
        CHECK(objects.Stats().liveObjects == Count);
        for (uint32 index = 0; index < Count; ++index) {
            roots[index].Reset();
            extra[index].Reset();
        }

        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

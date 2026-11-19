#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRefMap.h>
#include <Object/ObjectRegistry.h>
#include <Object/StructRefArray.h>
#include <Object/TypeRegistry.h>
#include <Task/WorkerScheduler.h>

// 클러스터(GC-S7) 시험이다(Docs/Design/GarbageCollectionScale.ko.md 13절).
// - 루트가 살면 소속이 모두 살고(루트 → 소속 암묵 간선), 경계 소속의 밖 간선·detached·outer는 따로
//   산다. 루트의 RequestDestroy는 해체, 소속의 RequestDestroy는 이탈이다.
// - 무작위 모델 시험: 클러스터 생성·가입·이탈·해체·간선 편집을 정지형·증분(직렬·병렬) 수집과 섞고,
//   매 slice 뒤 회수된 객체가 oracle(소속 간선 포함)에서 닿지 않는지(안전성), 변경 없는 정지형
//   수집 뒤 생존 집합이 oracle과 같은지(정확성) 본다. Debug는 Begin·Collect마다 장부를 다시 센다
//   (RegistryOptions::verifyClusterLedger).
// - 13.8절 특수 경우: 장벽으로만 shade된 클러스터의 해체, 처리된 클러스터에 밖 outer를 가진 소속
//   가입, slice 예산 중간에 끊긴 클러스터.
// 단언은 결과 집합과 계수로만 하고 시간·경쟁 운에 기대지 않는다.
namespace {

using namespace rocks;

constexpr TypeId ClusterNodeType{0x47436c7573744e31ULL, 1};
constexpr TypeId ClusterManualType{0x47436c7573744d32ULL, 1};
constexpr TypeId PlainNodeType{0x47436c7573745033ULL, 1};

class ClusterNode final : public Object {
public:
    explicit ClusterNode(Allocator& allocator) noexcept : items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        items.Trace(visitor);
    }

    ObjectRefArray<Object> items;
};

bool RegisterClusterTypes(TypeRegistry& types) noexcept {
    // 시험 타입은 수동 ops로 완전한 필드 배치를 선언한다(Trace가 모든 강한 간선을 방문한다).
    RuntimeTypeOps node = MakeRuntimeTypeOps<ClusterNode>();
    node.parallelTraceSafe = true;
    node.completeFieldLayout = true;
    RuntimeTypeOps manual = MakeRuntimeTypeOps<ClusterNode>();
    manual.completeFieldLayout = true;
    RuntimeTypeOps plain = MakeRuntimeTypeOps<ClusterNode>();
    plain.parallelTraceSafe = true;
    return types.Register(TypeSchema{ClusterNodeType, 1, {}, {}}, Move(node)) &&
           types.Register(TypeSchema{ClusterManualType, 1, {}, {}}, Move(manual)) &&
           types.Register(TypeSchema{PlainNodeType, 1, {}, {}}, Move(plain));
}

RegistryOptions ClusterOptions() noexcept {
    RegistryOptions options;
    options.clusters = true;
    options.verifyClusterLedger = true;
    return options;
}

bool Exists(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    return static_cast<bool>(objects.InspectLifecycle(objects.OwnerAccess(), handle));
}

// handle의 items에 target을 붙인다(Borrow를 쥐고 편집한다).
bool Link(ObjectRegistry& objects, ObjectHandle owner, ObjectHandle target) noexcept {
    auto borrowed = objects.Resolve<ClusterNode>(objects.OwnerAccess(), owner);
    if (!borrowed) {
        return false;
    }

    auto pointer = borrowed.Value().Get();
    return pointer &&
           objects.AddReference(objects.OwnerAccess(), owner, pointer.Value()->items, target);
}

bool Unlink(ObjectRegistry& objects, ObjectHandle owner, ObjectHandle target) noexcept {
    auto borrowed = objects.Resolve<ClusterNode>(objects.OwnerAccess(), owner);
    if (!borrowed) {
        return false;
    }

    auto pointer = borrowed.Value().Get();
    return pointer &&
           objects.RemoveReference(objects.OwnerAccess(), owner, pointer.Value()->items, target);
}

// 루트 하나와 소속 members개를 한 트랜잭션으로 만든다. 루트만 RootRef로 쥔다.
struct SmallCluster {
    RootRef<Object> root;
    ObjectHandle members[4];
};

bool MakeCluster(ObjectRegistry& objects, uint32 count, SmallCluster& out) noexcept {
    ConstructionContext transaction(objects, objects.OwnerAccess());
    auto root = transaction.Create<ClusterNode>(ClusterNodeType);
    if (!root || !transaction.SetClusterRoot(root.Value())) {
        return false;
    }

    for (uint32 index = 0; index < count; ++index) {
        const char name[] = {'m', static_cast<char>('0' + index)};
        auto member =
            transaction.Create<ClusterNode>(ClusterNodeType, root.Value(), StringView(name, 2));
        if (!member || !transaction.JoinCluster(member.Value(), root.Value())) {
            return false;
        }

        out.members[index] = member.Value();
    }

    const ObjectHandle roots[] = {root.Value()};
    RootRef<Object> held[1];
    if (!transaction.Commit(roots, held)) {
        return false;
    }

    out.root = Move(held[0]);
    return true;
}

TEST(ObjectGcCluster_RootKeepsMembersAndDissolvesWhenUnreachable) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 1}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        CHECK(objects.Stats().clusters == 1);
        CHECK(objects.Stats().clusterMembers == 4);
        REQUIRE(objects.Collect(access));
        CHECK(objects.LastGcReport().marked == 4);
        for (ObjectHandle member : cluster.members) {
            if (!member.IsNull()) {
                CHECK(Exists(objects, member));
            }
        }

        // 루트를 놓으면 클러스터가 표식되지 않아 sweep 전에 해체되고 모두 회수된다.
        const ObjectHandle rootHandle = cluster.root.Handle();
        cluster.root.Reset();
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, rootHandle));
        CHECK(!Exists(objects, cluster.members[0]));
        CHECK(objects.Stats().clusters == 0);
        CHECK(objects.Stats().clusterMembers == 0);
        CHECK(objects.Stats().liveObjects == 0);
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectGcCluster_BoundaryMemberKeepsOutsideTargetAndLeaveDetaches) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 2}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(outside);
        const ObjectHandle outsideHandle = outside.Value().Handle();
        // 경계: m0 → 밖 객체. 같은 클러스터 안: m1 → m2.
        REQUIRE(Link(objects, cluster.members[0], outsideHandle));
        REQUIRE(Link(objects, cluster.members[1], cluster.members[2]));
        outside.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, outsideHandle));
        // m2의 파괴 요청은 이탈이다. m1이 아직 가리키므로 detached로 남아 산다.
        REQUIRE(objects.RequestDestroy(access, cluster.members[2]));
        CHECK(objects.Stats().clusterMembers == 3);
        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, cluster.members[2]));
        // 간선을 지우면 detached가 풀리고 회수된다. 경계 간선도 지우면 밖 객체가 회수된다.
        REQUIRE(Unlink(objects, cluster.members[1], cluster.members[2]));
        REQUIRE(Unlink(objects, cluster.members[0], outsideHandle));
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, cluster.members[2]));
        CHECK(!Exists(objects, outsideHandle));
        CHECK(Exists(objects, cluster.members[1]));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 간선 제거의 장부 감소는 미뤄 기록한다(GC 설계 13.4절). 이탈은 먼저 기록을 적용해야 한다:
// 경계 간선을 지운 소속이 이탈하면 그 소속의 outCount가 0이어야 한다(적용하지 않으면 "outgoing
// ledger edges" Fatal). 이탈한 소속과 밖 객체는 다음 Collect에서 회수된다.
TEST(ObjectGcCluster_DeferredRemovalAppliesBeforeLeave) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 21}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(outside);
        const ObjectHandle outsideHandle = outside.Value().Handle();
        REQUIRE(Link(objects, cluster.members[2], outsideHandle));
        REQUIRE(Link(objects, cluster.members[1], cluster.members[0]));
        outside.Value().Reset();
        REQUIRE(objects.Collect(access));
        // 경계 간선과 안쪽 간선을 지우고(미뤄진 감소) 바로 이탈한다.
        REQUIRE(Unlink(objects, cluster.members[2], outsideHandle));
        REQUIRE(Unlink(objects, cluster.members[1], cluster.members[0]));
        REQUIRE(objects.RequestDestroy(access, cluster.members[2]));
        // m0은 안쪽 간선이 모두 지워졌으므로 detached가 아니라 클러스터 없음이 된다.
        REQUIRE(objects.RequestDestroy(access, cluster.members[0]));
        CHECK(objects.Stats().clusterMembers == 2);
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, cluster.members[2]));
        CHECK(!Exists(objects, cluster.members[0]));
        CHECK(!Exists(objects, outsideHandle));
        CHECK(Exists(objects, cluster.members[1]));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 루트의 파괴 요청은 클러스터를 해체한다. 해체도 먼저 기록을 적용해야 한다: 해체가 소속의 장부
// 칸을 비운 뒤 남은 기록을 적용하면 소유자가 이미 클러스터 밖이라 "out-count underflow" Fatal이다.
TEST(ObjectGcCluster_DeferredRemovalAppliesBeforeDissolve) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 24}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(outside);
        const ObjectHandle outsideHandle = outside.Value().Handle();
        REQUIRE(Link(objects, cluster.members[2], outsideHandle));
        REQUIRE(Link(objects, cluster.members[1], cluster.members[0]));
        outside.Value().Reset();
        REQUIRE(objects.Collect(access));
        // 경계 간선과 안쪽 간선을 지우고(미뤄진 감소) 바로 해체한다.
        REQUIRE(Unlink(objects, cluster.members[2], outsideHandle));
        REQUIRE(Unlink(objects, cluster.members[1], cluster.members[0]));
        REQUIRE(objects.RequestDestroy(access, cluster.root.Handle()));
        CHECK(objects.Stats().clusters == 0);
        CHECK(objects.Stats().clusterMembers == 0);
        cluster.root.Reset();
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, cluster.members[0]));
        CHECK(!Exists(objects, outsideHandle));
        CHECK(objects.Stats().liveObjects == 0);
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Marking 중에 지운 경계 간선의 감소는 cutoff에서 적용된다(대상이 회수되기 전). 그 대상이 이
// cycle에 회수되고 슬롯을 같은 클러스터의 새 소속이 다시 써도 장부가 맞다(Debug 장부 검증 — 적용이
// 늦으면 회수된 대상의 기록이 남아 세대 검사 Fatal).
TEST(ObjectGcCluster_DeferredRemovalDuringMarkingAppliesAtCutoff) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 22}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(outside);
        const ObjectHandle outsideHandle = outside.Value().Handle();
        REQUIRE(Link(objects, cluster.members[0], outsideHandle));
        outside.Value().Reset();
        REQUIRE(objects.Collect(access));
        REQUIRE(Exists(objects, outsideHandle));
        // cycle을 시작한 뒤(기록이 적용된 뒤) 경계 간선을 지운다. 밖 객체는 이 cycle에 회수된다.
        REQUIRE(objects.BeginCollection(access));
        REQUIRE(Unlink(objects, cluster.members[0], outsideHandle));
        GcBudget budget;
        budget.maxObjects = NumericLimits<uint32>::Max();
        budget.maxNanoseconds = 0;
        for (uint32 guard = 0; guard < 64 && objects.CollectionPhase() != GcPhase::Idle; ++guard) {
            REQUIRE(objects.StepCollection(access, budget));
        }

        REQUIRE(objects.CollectionPhase() == GcPhase::Idle);
        REQUIRE(objects.DrainRetired(access));
        CHECK(!Exists(objects, outsideHandle));
        // 같은 슬롯을 클러스터의 새 소속이 다시 쓸 때까지 소속을 만든다(빈 슬롯은 유한하다).
        ObjectHandle reused;
        {
            ConstructionContext transaction(objects, access);
            for (uint32 attempt = 0; attempt < 256 && reused.IsNull(); ++attempt) {
                auto created = transaction.Create<ClusterNode>(ClusterNodeType);
                REQUIRE(created);
                REQUIRE(transaction.JoinCluster(created.Value(), cluster.root.Handle()));
                if (created.Value().slot == outsideHandle.slot) {
                    reused = created.Value();
                }
            }

            REQUIRE(transaction.Commit());
        }

        REQUIRE(!reused.IsNull());
        REQUIRE(Link(objects, cluster.members[1], reused));
        // 적용(Collect 시작)과 Debug 장부 검증이 계수를 대조한다.
        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, reused));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 기록(4096칸)보다 많은 감소를 한 번에 미룬다(비우기 하나로 5,000개). 기록이 차면 먼저 적용하고
// 남은 것은 다음 적용 때 반영된다 — 장부 검증이 맞고, 계수가 0으로 돌아온 대상은 이탈 뒤 회수된다.
TEST(ObjectGcCluster_DeferredRemovalsOverflowTheLog) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 23}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        constexpr uint32 Edges = 5000;
        for (uint32 index = 0; index < Edges; ++index) {
            REQUIRE(Link(objects, cluster.members[0], cluster.members[1]));
        }

        REQUIRE(objects.Collect(access));
        {
            auto borrowed = objects.Resolve<ClusterNode>(access, cluster.members[0]);
            REQUIRE(borrowed);
            ClusterNode* node = borrowed.Value().Get().Value();
            REQUIRE(objects.ClearReferences(access, cluster.members[0], node->items));
        }

        REQUIRE(objects.RequestDestroy(access, cluster.members[1]));
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, cluster.members[1]));
        CHECK(Exists(objects, cluster.members[0]));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 끝까지 증분 cycle을 돈다(slice마다 객체 하나). reclaiming이 있으면 Reclaiming을 지났는지 적는다.
void StepToIdle(ObjectRegistry& objects, bool* reclaiming = nullptr) noexcept {
    GcBudget budget;
    budget.maxObjects = 1;
    budget.maxNanoseconds = 0;
    for (uint32 guard = 0; guard < 4096 && objects.CollectionPhase() != GcPhase::Idle; ++guard) {
        REQUIRE(objects.StepCollection(objects.OwnerAccess(), budget));
        if (reclaiming != nullptr && objects.CollectionPhase() == GcPhase::Reclaiming) {
            *reclaiming = true;
        }
    }

    REQUIRE(objects.CollectionPhase() == GcPhase::Idle);
}

// Sweeping 중에 지운 경계 간선의 감소는 Reclaiming·Idle을 지나 다음 Begin에서 적용된다. 대상은 그
// cycle에 표식됐으므로 그사이 회수되지 않는다(적용의 대상 검사가 통과한다). 다음 Collect가 기록을
// 적용하고 Debug 장부 검증이 계수를 대조한 뒤 밖 객체를 회수한다(기록을 잃으면 장부 검증 Fatal).
TEST(ObjectGcCluster_DeferredRemovalDuringSweepingAppliesAtNextBegin) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 25}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(outside);
        const ObjectHandle outsideHandle = outside.Value().Handle();
        REQUIRE(Link(objects, cluster.members[0], outsideHandle));
        outside.Value().Reset();
        REQUIRE(objects.Collect(access));
        // 이 cycle에 회수될 쓰레기(Reclaiming을 지나게 한다).
        for (uint32 index = 0; index < 4; ++index) {
            auto garbage = objects.NewObject<ClusterNode>(access, PlainNodeType);
            REQUIRE(garbage);
        }

        REQUIRE(objects.BeginCollection(access));
        GcBudget budget;
        budget.maxObjects = 1;
        budget.maxNanoseconds = 0;
        for (uint32 guard = 0; guard < 64 && objects.CollectionPhase() == GcPhase::Marking;
             ++guard) {
            REQUIRE(objects.StepCollection(access, budget));
        }

        REQUIRE(objects.CollectionPhase() == GcPhase::Sweeping);
        REQUIRE(Unlink(objects, cluster.members[0], outsideHandle));
        bool reclaiming = false;
        StepToIdle(objects, &reclaiming);
        CHECK(reclaiming);
        CHECK(Exists(objects, outsideHandle));
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, outsideHandle));
        CHECK(Exists(objects, cluster.members[0]));
        // 다음 검증이 적용 뒤의 계수를 대조한다(기록을 적용하지 않고 버리면 불일치 Fatal).
        REQUIRE(objects.Collect(access));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 취소(CancelCollection)는 기록을 적용하지 않는다. Marking 중에 지운 경계 간선의 감소는 다음 cycle
// 시작에 적용되고(Debug 장부 검증이 대조한다), 밖 객체는 그 cycle에 회수된다.
TEST(ObjectGcCluster_DeferredRemovalSurvivesCancel) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 26}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(outside);
        const ObjectHandle outsideHandle = outside.Value().Handle();
        REQUIRE(Link(objects, cluster.members[0], outsideHandle));
        outside.Value().Reset();
        REQUIRE(objects.Collect(access));
        REQUIRE(objects.BeginCollection(access));
        REQUIRE(Unlink(objects, cluster.members[0], outsideHandle));
        REQUIRE(objects.CancelCollection(access));
        REQUIRE(objects.CollectionPhase() == GcPhase::Idle);
        CHECK(Exists(objects, outsideHandle));
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, outsideHandle));
        CHECK(Exists(objects, cluster.members[0]));
        // 다음 검증이 적용 뒤의 계수를 대조한다(기록을 적용하지 않고 버리면 불일치 Fatal).
        REQUIRE(objects.Collect(access));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// cycle 밖(Idle)에서 지운 detached의 마지막 안쪽 간선은 다음 Begin(Collect)이 표식 전에 적용한다 —
// 정지형 Collect는 그 detached를 회수한다(적용이 cycle 시작에 없으면 처리가 detached를 shade해 한
// cycle 더 남는다).
TEST(ObjectGcCluster_IdleRemovalAppliesBeforeCollectMarks) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 29}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        REQUIRE(Link(objects, cluster.members[1], cluster.members[0]));
        REQUIRE(objects.Collect(access));
        REQUIRE(objects.RequestDestroy(access, cluster.members[0]));
        REQUIRE(objects.Collect(access));
        REQUIRE(Exists(objects, cluster.members[0]));
        REQUIRE(Unlink(objects, cluster.members[1], cluster.members[0]));
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, cluster.members[0]));
        CHECK(Exists(objects, cluster.members[1]));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 미룬 감소의 상한(GC 설계 13.4절 "늦는 쪽이 안전한 근거"): Marking 중 클러스터 처리 전에
// detached의 마지막 안쪽 간선을 지우면 감소가 미뤄져 처리가 detached를 shade한다 — 그 cycle은 산다.
// 감소는 그 cycle의 cutoff(경계 비율 판정 전)에 적용되어 클러스터 없음이 되고, 다음 cycle에
// 회수된다.
TEST(ObjectGcCluster_DetachedLivesOneMoreCycleAfterMarkingRemoval) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 27}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        REQUIRE(Link(objects, cluster.members[1], cluster.members[0]));
        REQUIRE(objects.Collect(access));
        // m0은 이탈해 detached가 된다(m1이 가리킨다).
        REQUIRE(objects.RequestDestroy(access, cluster.members[0]));
        CHECK(objects.Stats().clusterMembers == 3);
        REQUIRE(objects.Collect(access));
        REQUIRE(Exists(objects, cluster.members[0]));
        // cycle을 시작한 뒤 처리 전에(Step 전) 마지막 안쪽 간선을 지운다.
        REQUIRE(objects.BeginCollection(access));
        REQUIRE(Unlink(objects, cluster.members[1], cluster.members[0]));
        StepToIdle(objects);
        CHECK(Exists(objects, cluster.members[0]));
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, cluster.members[0]));
        CHECK(Exists(objects, cluster.members[1]));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 적용의 대상 검사는 세대에 더해 상태도 본다(GC 설계 13.4절): 세대가 다 찬 슬롯(maxGeneration)은
// 회수 뒤에도 세대가 그대로 Exhausted다. Marking 중 지운 경계 간선의 대상이 그 cycle에 회수되어
// Exhausted가 되는 정상 경로다(기록은 cutoff에서 적용된다). cutoff 적용이 빠지면 다음 Begin의
// 적용이 세대로는 회수를 보지 못하고 상태 검사로 Fatal이다.
TEST(ObjectGcCluster_DeferredRemovalTargetCheckSeesExhaustedSlot) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        RegistryOptions options = ClusterOptions();
        options.maxGeneration = 1;
        ObjectRegistry objects(allocator, types, Guid{0x4743, 28}, options);
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(outside);
        const ObjectHandle outsideHandle = outside.Value().Handle();
        REQUIRE(Link(objects, cluster.members[0], outsideHandle));
        outside.Value().Reset();
        REQUIRE(objects.Collect(access));
        REQUIRE(objects.BeginCollection(access));
        REQUIRE(Unlink(objects, cluster.members[0], outsideHandle));
        StepToIdle(objects);
        REQUIRE(objects.DrainRetired(access));
        CHECK(!Exists(objects, outsideHandle));
        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, cluster.members[0]));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 클러스터 소속 owner의 바꿔 떼기(RemoveReferenceAtSwap)는 지운 간선만 장부에서 뺀다. 옮긴 끝
// 원소가 이탈한(파괴 요청된) 소속이어도 그 간선의 계수는 그대로라 detached로 살고, 그 간선을 마저
// 지우면 회수된다. 매 Collect가 장부를 다시 세어 대조한다(verifyClusterLedger).
TEST(ObjectGcCluster_SwapRemoveKeepsLedgerForMovedDetachedMember) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 20}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 3, cluster));
        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(outside);
        const ObjectHandle outsideHandle = outside.Value().Handle();
        const ObjectHandle root = cluster.root.Handle();
        // 루트 items: [m0, outside, m1, m2]. m2(끝)가 이탈한 뒤 0번을 바꿔 떼면 m2가 0번으로 온다.
        REQUIRE(Link(objects, root, cluster.members[0]));
        REQUIRE(Link(objects, root, outsideHandle));
        REQUIRE(Link(objects, root, cluster.members[1]));
        REQUIRE(Link(objects, root, cluster.members[2]));
        outside.Value().Reset();
        REQUIRE(objects.RequestDestroy(access, cluster.members[2]));
        {
            auto borrowed = objects.Resolve<ClusterNode>(access, root);
            REQUIRE(borrowed);
            ClusterNode* node = borrowed.Value().Get().Value();
            REQUIRE(objects.RemoveReferenceAtSwap(access, root, node->items, 0));
            REQUIRE(node->items.Size() == 3);
            CHECK(node->items[0] == cluster.members[2]);
        }

        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, cluster.members[2]));
        CHECK(Exists(objects, outsideHandle));
        {
            auto borrowed = objects.Resolve<ClusterNode>(access, root);
            REQUIRE(borrowed);
            ClusterNode* node = borrowed.Value().Get().Value();
            // m2를 지우면(끝 원소 m1이 옮겨 온다) detached가 풀린다. 경계 간선도 바꿔 뗀다.
            REQUIRE(objects.RemoveReferenceAtSwap(access, root, node->items, 0));
            CHECK(node->items[0] == cluster.members[1]);
            REQUIRE(objects.RemoveReferenceAtSwap(access, root, node->items, 1));
            CHECK(node->items.Size() == 1);
        }

        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, cluster.members[2]));
        CHECK(!Exists(objects, outsideHandle));
        CHECK(Exists(objects, cluster.members[0]) && Exists(objects, cluster.members[1]));
        cluster.root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectGcCluster_RootDestroyDissolvesAndJoinRulesHold) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 3}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 2, cluster));
        // 소속이 아닌 객체(완전한 필드 배치가 아님)는 가입 지정이 성공해도 소속이 아니다.
        {
            ConstructionContext transaction(objects, access);
            auto plain = transaction.Create<ClusterNode>(PlainNodeType);
            REQUIRE(plain);
            CHECK(transaction.SetClusterRoot(plain.Value()).Error() == ErrorCode::Unsupported);
            REQUIRE(transaction.JoinCluster(plain.Value(), cluster.root.Handle()));
            // 루트가 아닌 소속에는 가입할 수 없다.
            auto other = transaction.Create<ClusterNode>(ClusterNodeType);
            REQUIRE(other);
            CHECK(transaction.JoinCluster(other.Value(), cluster.members[0]).Error() ==
                  ErrorCode::StaleHandle);
            REQUIRE(transaction.JoinCluster(other.Value(), cluster.root.Handle()));
            CHECK(transaction.JoinCluster(other.Value(), cluster.root.Handle()).Error() ==
                  ErrorCode::InvalidArgument);
            REQUIRE(transaction.Commit());
        }

        CHECK(objects.Stats().clusterMembers == 4);
        // 루트의 파괴 요청은 해체다. 소속은 실제 간선으로 닿지 않으면 회수된다.
        const ObjectHandle rootHandle = cluster.root.Handle();
        REQUIRE(objects.RequestDestroy(access, rootHandle));
        CHECK(objects.Stats().clusters == 0);
        CHECK(objects.Stats().clusterMembers == 0);
        {
            ConstructionContext transaction(objects, access);
            auto late = transaction.Create<ClusterNode>(ClusterNodeType);
            REQUIRE(late);
            CHECK(transaction.JoinCluster(late.Value(), rootHandle).Error() ==
                  ErrorCode::StaleHandle);
        }

        cluster.root.Reset();
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, cluster.members[0]));
        CHECK(objects.Stats().liveObjects == 0);
        REQUIRE(objects.Shutdown(access));
    }

    {
        // 클러스터를 끈 Registry는 지정을 받지 않는다.
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 4});
        ConstructionContext transaction(objects, objects.OwnerAccess());
        auto node = transaction.Create<ClusterNode>(ClusterNodeType);
        REQUIRE(node);
        CHECK(transaction.SetClusterRoot(node.Value()).Error() == ErrorCode::Unsupported);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 13.8절: 장벽으로만 shade된(표식됐지만 처리 전) 클러스터를 slice 사이에 해체해도 경계 소속의 밖
// 대상이 산다. 처리된 클러스터에 가입한 새 소속의 밖 outer도 산다.
TEST(ObjectGcCluster_IncrementalBarrierCasesKeepReachableObjects) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 5}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        SmallCluster cluster;
        REQUIRE(MakeCluster(objects, 2, cluster));
        auto keeper = objects.NewObject<ClusterNode>(access, PlainNodeType);
        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(keeper && outside);
        // 다른 루트 넷과, 첫 루트 아래 루트 없는 사슬이다. 첫 slice(루트 수만큼의 예산)가 루트를
        // 모두 trace하고(keeper 포함 — 루트 목록 순서와 무관) 사슬이 남아 표식이 끝나지 않게 한다.
        RootRef<ClusterNode> fillers[4];
        for (RootRef<ClusterNode>& filler : fillers) {
            auto made = objects.NewObject<ClusterNode>(access, PlainNodeType);
            REQUIRE(made);
            filler = Move(made.Value());
        }

        ObjectHandle chain = fillers[0].Handle();
        for (uint32 index = 0; index < 8; ++index) {
            auto link = objects.NewObject<ClusterNode>(access, PlainNodeType);
            REQUIRE(link);
            REQUIRE(Link(objects, chain, link.Value().Handle()));
            chain = link.Value().Handle();
        }

        const ObjectHandle outsideHandle = outside.Value().Handle();
        const ObjectHandle member = cluster.members[0];
        REQUIRE(Link(objects, member, outsideHandle));
        outside.Value().Reset();
        // 루트를 놓아 클러스터가 루트 목록으로 표식되지 않게 한다.
        const ObjectHandle rootHandle = cluster.root.Handle();
        cluster.root.Reset();
        REQUIRE(objects.BeginCollection(access));
        // 루트(keeper와 filler 넷)를 먼저 모두 trace한다. 그 뒤 keeper(이미 trace된 객체)가 소속
        // m0를 가리키게 한다: 이 간선은 삽입 장벽만 지킨다 — 장벽이 클러스터를 shade만 하고(처리
        // 전), 장벽이 없으면 m0·밖 대상이 keeper에서 닿는데도 회수된다.
        REQUIRE(objects.StepCollection(access, GcBudget{5, 0}));
        REQUIRE(objects.CollectionPhase() == GcPhase::Marking);
        REQUIRE(Link(objects, keeper.Value().Handle(), member));
        // 처리 전에 루트를 파괴해 해체한다. 경계 소속 m0의 밖 대상이 살아야 한다.
        REQUIRE(objects.RequestDestroy(access, rootHandle));
        while (objects.CollectionPhase() != GcPhase::Idle) {
            REQUIRE(objects.StepCollection(access, GcBudget{3, 0}));
        }

        CHECK(Exists(objects, member));
        CHECK(Exists(objects, outsideHandle));
        // 다음 정지형 수집: keeper → m0 → outside와 m0의 outer(파괴 요청된 옛 루트)만 산다. m1은
        // 해체 뒤 닿는 간선이 없어 회수된다.
        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, member));
        CHECK(Exists(objects, outsideHandle));
        CHECK(Exists(objects, rootHandle));
        CHECK(!Exists(objects, cluster.members[1]));

        // 처리된 클러스터에 가입: 새 소속의 outer(밖 객체, 루트 없음)는 새 소속을 통해서만 산다.
        SmallCluster second;
        REQUIRE(MakeCluster(objects, 1, second));
        auto host = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(host);
        const ObjectHandle hostHandle = host.Value().Handle();
        REQUIRE(objects.BeginCollection(access));
        while (objects.CollectionPhase() == GcPhase::Marking) {
            REQUIRE(objects.StepCollection(access, GcBudget{1, 0}));
            if (objects.CollectionPhase() != GcPhase::Marking) {
                break;
            }

            // 클러스터가 처리될 만큼 진행한 뒤(루트가 가장 먼저 표식된다) 가입한다.
            if (host.Value().Handle() == hostHandle) {
                ConstructionContext transaction(objects, access);
                auto joined = transaction.Create<ClusterNode>(ClusterNodeType, hostHandle, "J");
                REQUIRE(joined);
                REQUIRE(transaction.JoinCluster(joined.Value(), second.root.Handle()));
                REQUIRE(transaction.Commit(Span<const ObjectHandle>{}, Span<RootRef<Object>>{}));
                host.Value().Reset();
            }
        }

        while (objects.CollectionPhase() != GcPhase::Idle) {
            REQUIRE(objects.StepCollection(access, GcBudget{1, 0}));
        }

        CHECK(Exists(objects, hostHandle));
        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, hostHandle));
        second.root.Reset();
        keeper.Value().Reset();
        for (RootRef<ClusterNode>& filler : fillers) {
            filler.Reset();
        }

        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// ---- 무작위 모델 시험 ----

struct Rng {
    uint64 state;

    uint32 Below(uint32 bound) noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<uint32>((state * 0x2545f4914f6cdd1dULL) % bound);
    }
};

constexpr uint32 ModelCapacity = 900;
constexpr uint32 MaxDegree = 6;
constexpr uint32 NoIndex = NumericLimits<uint32>::Max();

// 시험 쪽 모델이다. 간선 다중집합·outer·소속(루트 번호)·파괴 요청·루트 보유를 따라간다. 회수된
// 객체는 dead다.
struct Model {
    uint32 count = 0;
    ObjectHandle handles[ModelCapacity];
    uint32 outer[ModelCapacity];
    uint32 cluster[ModelCapacity];
    bool clusterRoot[ModelCapacity];
    uint32 edges[ModelCapacity][MaxDegree];
    uint32 degree[ModelCapacity];
    bool destroyed[ModelCapacity];
    bool dead[ModelCapacity];
    bool reachable[ModelCapacity];
    RootRef<Object> held[ModelCapacity];

    // oracle: 루트에서 간선·inner → outer·루트 → 소속으로 닿는 객체.
    void Reach() noexcept {
        static uint32 stack[ModelCapacity];
        uint32 top = 0;
        for (uint32 index = 0; index < count; ++index) {
            reachable[index] = false;
        }

        auto visit = [&](uint32 next) noexcept {
            if (next != NoIndex && !reachable[next]) {
                reachable[next] = true;
                stack[top++] = next;
            }
        };
        for (uint32 index = 0; index < count; ++index) {
            if (!dead[index] && !held[index].Handle().IsNull()) {
                visit(index);
            }
        }

        while (top != 0) {
            const uint32 index = stack[--top];
            visit(outer[index]);
            for (uint32 edge = 0; edge < degree[index]; ++edge) {
                visit(edges[index][edge]);
            }

            if (clusterRoot[index]) {
                for (uint32 member = 0; member < count; ++member) {
                    if (cluster[member] == index) {
                        visit(member);
                    }
                }
            }
        }
    }
};

struct ModelRun {
    ObjectRegistry& objects;
    Model& model;
    Rng rng;
    WorkerScheduler* scheduler;
    uint32 names = 0;

    ObjectAccessContext Access() const noexcept {
        return objects.OwnerAccess();
    }

    bool Usable(uint32 index) const noexcept {
        return index < model.count && !model.dead[index] && !model.destroyed[index];
    }

    uint32 Pick() noexcept {
        return model.count == 0 ? NoIndex : rng.Below(model.count);
    }

    // 새 객체 하나(또는 새 클러스터 루트와 소속 둘)를 만든다.
    void Create() noexcept {
        if (model.count + 3 > ModelCapacity) {
            return;
        }

        ConstructionContext transaction(objects, Access());
        const uint32 roll = rng.Below(4);
        const uint32 first = model.count;
        uint32 made = 0;
        uint32 joinRoot = NoIndex;
        if (roll == 0) {
            // 기존 클러스터에 가입(outer는 아무 생존 객체 — 밖 outer 포함).
            const uint32 root = Pick();
            if (root == NoIndex || !Usable(root) || !model.clusterRoot[root]) {
                return;
            }

            joinRoot = root;
        }

        const uint32 outerPick = Pick();
        const uint32 outer =
            outerPick != NoIndex && Usable(outerPick) && rng.Below(2) == 0 ? outerPick : NoIndex;
        const ObjectHandle outerHandle = outer == NoIndex ? ObjectHandle{} : model.handles[outer];
        auto create = [&](uint32 at, ObjectHandle withOuter) noexcept -> ObjectHandle {
            char name[12] = {'c'};
            uint32 length = 1;
            for (uint32 value = ++names; value != 0; value /= 10) {
                name[length++] = static_cast<char>('0' + value % 10);
            }

            const TypeId type = rng.Below(5) == 0 ? ClusterManualType : ClusterNodeType;
            auto created = transaction.Create<ClusterNode>(
                type, withOuter, withOuter.IsNull() ? StringView{} : StringView(name, length));
            if (!created) {
                return {};
            }

            model.handles[at] = created.Value();
            model.outer[at] = NoIndex;
            model.cluster[at] = NoIndex;
            model.clusterRoot[at] = false;
            model.degree[at] = 0;
            model.destroyed[at] = false;
            model.dead[at] = false;
            return created.Value();
        };
        if (joinRoot != NoIndex) {
            const ObjectHandle created = create(first, outerHandle);
            REQUIRE(!created.IsNull());
            REQUIRE(transaction.JoinCluster(created, model.handles[joinRoot]));
            model.outer[first] = outer;
            model.cluster[first] = joinRoot;
            made = 1;
        } else if (roll == 1) {
            // 새 클러스터: 루트와 소속 둘(소속의 outer는 루트).
            const ObjectHandle root = create(first, {});
            REQUIRE(!root.IsNull());
            REQUIRE(transaction.SetClusterRoot(root));
            model.clusterRoot[first] = true;
            model.cluster[first] = first;
            for (uint32 index = 1; index < 3; ++index) {
                const ObjectHandle member = create(first + index, root);
                REQUIRE(!member.IsNull());
                REQUIRE(transaction.JoinCluster(member, root));
                model.outer[first + index] = first;
                model.cluster[first + index] = first;
            }

            made = 3;
        } else {
            const ObjectHandle created = create(first, outerHandle);
            REQUIRE(!created.IsNull());
            model.outer[first] = outer;
            made = 1;
        }

        // 첫 객체만 절반 확률로 루트로 쥔다.
        const bool hold = rng.Below(2) == 0;
        const ObjectHandle roots[] = {model.handles[first]};
        RootRef<Object> out[1];
        REQUIRE(transaction.Commit(Span<const ObjectHandle>(roots, hold ? 1u : 0u),
                                   Span<RootRef<Object>>(out, hold ? 1u : 0u)));
        if (hold) {
            model.held[first] = Move(out[0]);
        }

        model.count += made;
    }

    void Mutate() noexcept {
        const uint32 roll = rng.Below(16);
        const uint32 index = Pick();
        if (index == NoIndex) {
            Create();
            return;
        }

        if (roll < 3) {
            Create();
        } else if (roll < 8) {
            const uint32 target = Pick();
            if (Usable(index) && Usable(target) && model.degree[index] < MaxDegree &&
                Link(objects, model.handles[index], model.handles[target])) {
                model.edges[index][model.degree[index]++] = target;
            }
        } else if (roll < 11) {
            if (Usable(index) && model.degree[index] != 0) {
                const uint32 at = rng.Below(model.degree[index]);
                const uint32 target = model.edges[index][at];
                REQUIRE(Unlink(objects, model.handles[index], model.handles[target]));
                // 모델도 첫 같은 원소를 지운다(RemoveReference와 같은 규칙).
                for (uint32 edge = 0; edge < model.degree[index]; ++edge) {
                    if (model.edges[index][edge] == target) {
                        for (uint32 next = edge + 1; next < model.degree[index]; ++next) {
                            model.edges[index][next - 1] = model.edges[index][next];
                        }

                        --model.degree[index];
                        break;
                    }
                }
            }
        } else if (roll < 12) {
            if (Usable(index)) {
                auto borrowed = objects.Resolve<ClusterNode>(Access(), model.handles[index]);
                if (borrowed) {
                    auto pointer = borrowed.Value().Get();
                    REQUIRE(pointer);
                    REQUIRE(objects.ClearReferences(Access(), model.handles[index],
                                                    pointer.Value()->items));
                    model.degree[index] = 0;
                }
            }
        } else if (roll < 14) {
            if (Usable(index) && objects.RequestDestroy(Access(), model.handles[index])) {
                model.destroyed[index] = true;
                if (model.clusterRoot[index]) {
                    // 해체: 소속이 모두 클러스터 없음이 된다.
                    model.clusterRoot[index] = false;
                    for (uint32 member = 0; member < model.count; ++member) {
                        if (model.cluster[member] == index) {
                            model.cluster[member] = NoIndex;
                        }
                    }
                } else {
                    model.cluster[index] = NoIndex;
                }
            }
        } else {
            if (model.held[index].Handle().IsNull()) {
                if (Usable(index)) {
                    auto pinned = objects.TryPin<Object>(Access(), model.handles[index]);
                    if (pinned) {
                        model.held[index] = Move(pinned.Value());
                    }
                }
            } else {
                model.held[index].Reset();
            }
        }
    }

    // 회수된 객체를 dead로 옮기고 oracle에서 닿지 않았는지 본다(안전성).
    void CheckSafety() noexcept {
        model.Reach();
        for (uint32 index = 0; index < model.count; ++index) {
            if (model.dead[index] || Exists(objects, model.handles[index])) {
                continue;
            }

            CHECK(!model.reachable[index]);
            model.dead[index] = true;
            model.degree[index] = 0;
            if (model.clusterRoot[index]) {
                model.clusterRoot[index] = false;
                for (uint32 member = 0; member < model.count; ++member) {
                    if (model.cluster[member] == index) {
                        model.cluster[member] = NoIndex;
                    }
                }
            }
        }
    }

    // 정지형 수집 뒤 생존 집합이 oracle과 같다(정확성).
    void CheckExact() noexcept {
        REQUIRE(objects.Collect(Access(), scheduler));
        CheckSafety();
        model.Reach();
        uint32 clusters = 0;
        for (uint32 index = 0; index < model.count; ++index) {
            if (!model.dead[index]) {
                CHECK(model.reachable[index]);
                clusters += model.clusterRoot[index] ? 1u : 0u;
            }
        }

        CHECK(objects.Stats().clusters == clusters);
    }

    void Run(uint32 rounds) noexcept {
        for (uint32 count = 0; count < 40; ++count) {
            Mutate();
        }

        for (uint32 round = 0; round < rounds; ++round) {
            if (round % 5 == 4) {
                CheckExact();
                continue;
            }

            REQUIRE(objects.BeginCollection(Access()));
            uint32 slices = 0;
            while (objects.CollectionPhase() != GcPhase::Idle) {
                REQUIRE(
                    objects.StepCollection(Access(), GcBudget{7 + rng.Below(40), 0}, scheduler));
                CheckSafety();
                if (++slices % 2 == 0) {
                    for (uint32 edits = rng.Below(4); edits != 0; --edits) {
                        Mutate();
                    }
                }
            }

            CheckSafety();
        }

        CheckExact();
    }
};

void RunClusterModel(uint64 seed, uint32 workers) noexcept {
    static Model model;
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        if (workers != 0) {
            REQUIRE(scheduler.Start({workers, 256, 16}));
        }

        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        {
            ObjectRegistry objects(allocator, types, Guid{0x4744, seed}, ClusterOptions());
            model.count = 0;
            ModelRun run{objects, model, Rng{seed}, workers != 0 ? &scheduler : nullptr};
            run.Run(60);
            for (uint32 index = 0; index < ModelCapacity; ++index) {
                model.held[index].Reset();
            }

            REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        }

        if (workers != 0) {
            REQUIRE(scheduler.Shutdown());
        }

        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// ---- ObjectRefMap 편집(장부의 map 지점) ----

constexpr TypeId MapNodeType{0x47436c7573744d35ULL, 1};

class MapNode final : public Object {
public:
    explicit MapNode(Allocator& allocator) noexcept : values(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        values.Trace(visitor);
    }

    ObjectRefMap<uint32, Object> values;
};

// 소속 객체의 ObjectRefMap 넣기·교체·지우기·비우기가 장부를 맞춘다(Debug는 Collect마다 장부를 다시
// 센다). 밖 객체는 map 값으로만 살고, 비우면 회수된다.
TEST(ObjectGcCluster_MemberMapEditsKeepLedger) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        RuntimeTypeOps map = MakeRuntimeTypeOps<MapNode>();
        map.completeFieldLayout = true;
        REQUIRE(types.Register(TypeSchema{MapNodeType, 1, {}, {}}, Move(map)));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 9}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        RootRef<Object> root;
        ObjectHandle holder;
        ObjectHandle sibling;
        {
            ConstructionContext transaction(objects, access);
            auto made = transaction.Create<ClusterNode>(ClusterNodeType);
            REQUIRE(made && transaction.SetClusterRoot(made.Value()));
            auto member = transaction.Create<MapNode>(MapNodeType, made.Value(), "Map");
            auto other = transaction.Create<ClusterNode>(ClusterNodeType, made.Value(), "Other");
            REQUIRE(member && other);
            REQUIRE(transaction.JoinCluster(member.Value(), made.Value()));
            REQUIRE(transaction.JoinCluster(other.Value(), made.Value()));
            holder = member.Value();
            sibling = other.Value();
            auto committed = transaction.Commit();
            REQUIRE(committed);
            root = Move(committed.Value());
        }

        auto first = objects.NewObject<ClusterNode>(access, PlainNodeType);
        auto second = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(first && second);
        const ObjectHandle firstHandle = first.Value().Handle();
        const ObjectHandle secondHandle = second.Value().Handle();
        first.Value().Reset();
        second.Value().Reset();
        auto edit = [&](auto&& body) noexcept {
            auto borrowed = objects.Resolve<MapNode>(access, holder);
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            body(pointer.Value()->values);
        };
        edit([&](ObjectRefMap<uint32, Object>& values) noexcept {
            REQUIRE(objects.SetMapReference(access, holder, values, 1u, firstHandle));
            REQUIRE(objects.SetMapReference(access, holder, values, 2u, sibling));
            REQUIRE(objects.SetMapReference(access, holder, values, 3u, secondHandle));
            // 같은 키 교체: 밖(second) → 소속(sibling).
            REQUIRE(objects.SetMapReference(access, holder, values, 3u, sibling));
        });
        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, firstHandle));
        CHECK(!Exists(objects, secondHandle));
        edit([&](ObjectRefMap<uint32, Object>& values) noexcept {
            REQUIRE(objects.EraseMapReference(access, holder, values, 2u));
        });
        REQUIRE(objects.Collect(access));
        edit([&](ObjectRefMap<uint32, Object>& values) noexcept {
            REQUIRE(objects.ClearMapReferences(access, holder, values));
        });
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, firstHandle));
        CHECK(Exists(objects, sibling));
        root.Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// ---- 패키지 클러스터(P4c) ----

constexpr TypeId PackageNodeType{0x47436c7573745034ULL, 1};

class PackageNode final : public Object {
public:
    explicit PackageNode(Allocator& allocator) noexcept : items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        items.Trace(visitor);
    }

    ObjectRefArray<Object> items;
};

bool RegisterPackageTypes(TypeRegistry& types) noexcept {
    RuntimeTypeOps package = MakeRuntimeTypeOps<PackageNode>();
    package.parallelTraceSafe = true;
    package.completeFieldLayout = true;
    return RegisterClusterTypes(types) &&
           types.Register(TypeSchema{PackageNodeType, 1, {}, {}}, Move(package));
}

RegistryOptions PackageClusterOptions() noexcept {
    RegistryOptions options;
    options.policy.packageNativeType = NativeTypeKey<PackageNode>();
    options.verifyClusterLedger = true;
    options.assertRetiredWithoutDiscard = false;
    return options;
}

// mode 0: Collect, 1: 증분 직렬(객체 3개씩), 2: 증분 병렬.
void CollectBy(ObjectRegistry& objects, uint32 mode, WorkerScheduler& scheduler) noexcept {
    if (mode == 0) {
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        return;
    }

    REQUIRE(objects.BeginCollection(objects.OwnerAccess()));
    while (objects.CollectionPhase() != GcPhase::Idle) {
        REQUIRE(objects.StepCollection(objects.OwnerAccess(), GcBudget{3, 0},
                                       mode == 2 ? &scheduler : nullptr));
    }
}

void RunPackageClusters(uint32 mode) noexcept {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({2, 256, 16}));
        TypeRegistry types(allocator);
        REQUIRE(RegisterPackageTypes(types));
        {
            ObjectRegistry objects(allocator, types, Guid{0x4745, mode}, PackageClusterOptions());
            const auto access = objects.OwnerAccess();
            auto first = objects.NewObject<PackageNode>(access, PackageNodeType);
            auto second = objects.NewObject<PackageNode>(access, PackageNodeType);
            REQUIRE(first && second);
            const ObjectHandle p = first.Value().Handle();
            const ObjectHandle q = second.Value().Handle();
            auto a = objects.NewObject<ClusterNode>(access, ClusterNodeType, p, "A");
            REQUIRE(a);
            auto b =
                objects.NewObject<ClusterNode>(access, ClusterNodeType, a.Value().Handle(), "B");
            auto c = objects.NewObject<ClusterNode>(access, ClusterNodeType, p, "C");
            auto x = objects.NewObject<ClusterNode>(access, ClusterNodeType, q, "X");
            REQUIRE(b && c && x);
            const ObjectHandle ah = a.Value().Handle();
            const ObjectHandle bh = b.Value().Handle();
            const ObjectHandle ch = c.Value().Handle();
            const ObjectHandle xh = x.Value().Handle();
            a.Value().Reset();
            b.Value().Reset();
            c.Value().Reset();
            x.Value().Reset();
            CHECK(objects.Stats().clusters == 2);
            CHECK(objects.Stats().clusterMembers == 6);
            // 패키지 사이 간선(경계)과 같은 패키지 안 간선.
            REQUIRE(Link(objects, ah, xh));
            REQUIRE(Link(objects, ch, ah));
            CollectBy(objects, mode, scheduler);
            CHECK(Exists(objects, bh));
            CHECK(Exists(objects, xh));
            // 부분 트리 Discard(A·B): 이탈한다. C가 A를 가리키므로 A는 detached로 산다.
            REQUIRE(objects.Discard(access, ah));
            CHECK(objects.Stats().clusterMembers == 4);
            CollectBy(objects, mode, scheduler);
            CHECK(Exists(objects, ah));
            CHECK(!Exists(objects, bh));
            REQUIRE(Unlink(objects, ch, ah));
            CollectBy(objects, mode, scheduler);
            CHECK(!Exists(objects, ah));
            // 패키지 Discard는 해체다. 루트를 놓으면 회수된다.
            REQUIRE(objects.Discard(access, p));
            CHECK(objects.Stats().clusters == 1);
            first.Value().Reset();
            CollectBy(objects, mode, scheduler);
            CHECK(!Exists(objects, ch));
            CHECK(!Exists(objects, p));
            CHECK(objects.Stats().retiredWithoutDiscard == 0);
            // Discard 없이 루트를 놓은 버그 경로: 표식되지 않은 클러스터를 해체하고 센다.
            second.Value().Reset();
            CollectBy(objects, mode, scheduler);
            CHECK(!Exists(objects, xh));
            CHECK(objects.Stats().retiredWithoutDiscard == 2);
            CHECK(objects.Stats().clusters == 0);
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// ---- 영구 영역(P4d) ----

// 영구 창의 객체는 루트 없이 살고, 창을 닫은 뒤의 영구 → 일반 간선·파괴 요청·Discard는
// Unsupported다. mode는 CollectBy와 같다.
void RunPermanentRegion(uint32 mode) noexcept {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({2, 256, 16}));
        TypeRegistry types(allocator);
        REQUIRE(RegisterClusterTypes(types));
        {
            RegistryOptions options;
            options.permanentRegion = true;
            options.verifyClusterLedger = true;
            ObjectRegistry objects(allocator, types, Guid{0x4746, mode}, options);
            const auto access = objects.OwnerAccess();
            // 창 안: 완전한 필드 배치가 아닌 타입은 Commit이 거부한다.
            {
                ConstructionContext transaction(objects, access);
                REQUIRE(transaction.Create<ClusterNode>(PlainNodeType));
                CHECK(transaction.Commit().Error() == ErrorCode::Unsupported);
            }

            ObjectHandle permanent[3];
            for (ObjectHandle& handle : permanent) {
                auto made = objects.NewObject<ClusterNode>(access, ClusterNodeType);
                REQUIRE(made);
                handle = made.Value().Handle();
            }

            {
                auto inner =
                    objects.NewObject<ClusterNode>(access, ClusterNodeType, permanent[0], "Inner");
                REQUIRE(inner);
            }

            // 영구 → 영구 간선은 된다. 가입 지정은 창 안에서 받지 않는다.
            REQUIRE(Link(objects, permanent[0], permanent[1]));
            {
                ConstructionContext transaction(objects, access);
                auto node = transaction.Create<ClusterNode>(ClusterNodeType);
                REQUIRE(node);
                CHECK(transaction.SetClusterRoot(node.Value()).Error() == ErrorCode::Unsupported);
            }

            CHECK(objects.Stats().clusters == 1);
            CHECK(objects.Stats().clusterMembers == 4);
            REQUIRE(objects.ClosePermanentRegion(access));
            REQUIRE(objects.ClosePermanentRegion(access));
            auto normal = objects.NewObject<ClusterNode>(access, ClusterNodeType);
            REQUIRE(normal);
            const ObjectHandle normalHandle = normal.Value().Handle();
            CHECK(objects.Stats().clusterMembers == 4);
            // 영구 → 일반은 거부, 일반 → 영구는 된다.
            CHECK(!Link(objects, permanent[2], normalHandle));
            REQUIRE(Link(objects, normalHandle, permanent[2]));
            CHECK(objects.RequestDestroy(access, permanent[1]).Error() == ErrorCode::Unsupported);
            CHECK(objects.Discard(access, permanent[0]).Error() == ErrorCode::Unsupported);
            normal.Value().Reset();
            CollectBy(objects, mode, scheduler);
            CHECK(!Exists(objects, normalHandle));
            for (ObjectHandle handle : permanent) {
                CHECK(Exists(objects, handle));
            }

            CollectBy(objects, mode, scheduler);
            CHECK(objects.Stats().liveObjects == 4);
            // 종료가 영구 영역을 해체하고 회수한다.
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// ---- 경계 비율 안전장치 ----

// 경계 소속 비율이 ClusterFlattenBoundaryPercent(80%)를 넘는 패키지 클러스터는 cycle 끝에 해체되고,
// 그 패키지의 객체는 일반 객체로 계속 산다. 문턱 아래 패키지는 남는다. 해체된 패키지의 새 객체는
// 가입하지 않고, 패키지 Discard 뒤에는 모두 회수된다. mode는 CollectBy와 같다.
void RunPackageBoundaryValve(uint32 mode) noexcept {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({2, 256, 16}));
        TypeRegistry types(allocator);
        REQUIRE(RegisterPackageTypes(types));
        {
            ObjectRegistry objects(allocator, types, Guid{0x4747, mode}, PackageClusterOptions());
            const auto access = objects.OwnerAccess();
            auto wide = objects.NewObject<PackageNode>(access, PackageNodeType);
            auto narrow = objects.NewObject<PackageNode>(access, PackageNodeType);
            auto target = objects.NewObject<PackageNode>(access, PackageNodeType);
            REQUIRE(wide && narrow && target);
            const ObjectHandle w = wide.Value().Handle();
            const ObjectHandle n = narrow.Value().Handle();
            const ObjectHandle t = target.Value().Handle();
            auto outside = objects.NewObject<ClusterNode>(access, ClusterNodeType, t, "Y");
            REQUIRE(outside);
            const ObjectHandle y = outside.Value().Handle();
            outside.Value().Reset();
            // 패키지마다 소속 10개(패키지 객체 + 9): wide는 9개(90%), narrow는 1개(10%)가 경계다.
            constexpr uint32 Inner = 9;
            ObjectHandle wideNodes[Inner];
            ObjectHandle narrowNodes[Inner];
            for (uint32 index = 0; index < Inner; ++index) {
                const char name[] = {'n', static_cast<char>('0' + index)};
                auto first =
                    objects.NewObject<ClusterNode>(access, ClusterNodeType, w, StringView(name, 2));
                auto second =
                    objects.NewObject<ClusterNode>(access, ClusterNodeType, n, StringView(name, 2));
                REQUIRE(first && second);
                wideNodes[index] = first.Value().Handle();
                narrowNodes[index] = second.Value().Handle();
            }

            for (uint32 index = 0; index < Inner; ++index) {
                REQUIRE(Link(objects, wideNodes[index], y));
            }

            REQUIRE(Link(objects, narrowNodes[0], y));
            CHECK(objects.Stats().clusters == 3);
            CHECK(objects.Stats().clusterMembers == 22);
            CollectBy(objects, mode, scheduler);
            CHECK(objects.Stats().clusters == 2);
            CHECK(objects.Stats().clusterMembers == 12);
            CHECK(Exists(objects, w));
            CHECK(Exists(objects, y));
            for (uint32 index = 0; index < Inner; ++index) {
                CHECK(Exists(objects, wideNodes[index]));
                CHECK(Exists(objects, narrowNodes[index]));
            }

            // 해체된 패키지의 새 객체는 일반 객체다(가입하지 않고 Commit은 성공한다).
            auto late = objects.NewObject<ClusterNode>(access, ClusterNodeType, w, "Late");
            REQUIRE(late);
            const ObjectHandle lateHandle = late.Value().Handle();
            late.Value().Reset();
            REQUIRE(Link(objects, lateHandle, wideNodes[0]));
            CHECK(objects.Stats().clusterMembers == 12);
            CollectBy(objects, mode, scheduler);
            CHECK(Exists(objects, lateHandle));
            CHECK(objects.Stats().clusters == 2);
            // 해체된 패키지를 Discard하고 루트를 놓으면 모두 회수된다. 밖 대상은 다른 패키지라
            // 산다.
            REQUIRE(objects.Discard(access, w));
            wide.Value().Reset();
            CollectBy(objects, mode, scheduler);
            CHECK(!Exists(objects, w));
            CHECK(!Exists(objects, lateHandle));
            for (uint32 index = 0; index < Inner; ++index) {
                CHECK(!Exists(objects, wideNodes[index]));
                CHECK(Exists(objects, narrowNodes[index]));
            }

            CHECK(Exists(objects, y));
            CHECK(objects.Stats().retiredWithoutDiscard == 0);
            // 다시 읽은 패키지(새 패키지 객체)는 새 클러스터를 만든다 — 해체는 옛 패키지의
            // 수명에만 적용된다(GC 설계 13.5절).
            auto again = objects.NewObject<PackageNode>(access, PackageNodeType);
            REQUIRE(again);
            const ObjectHandle a = again.Value().Handle();
            auto reloaded = objects.NewObject<ClusterNode>(access, ClusterNodeType, a, "n0");
            REQUIRE(reloaded);
            reloaded.Value().Reset();
            CHECK(objects.Stats().clusters == 3);
            CHECK(objects.Stats().clusterMembers == 14);
            CollectBy(objects, mode, scheduler);
            CHECK(objects.Stats().clusters == 3);
            REQUIRE(objects.Discard(access, a));
            REQUIRE(objects.Discard(access, n));
            REQUIRE(objects.Discard(access, t));
            again.Value().Reset();
            narrow.Value().Reset();
            target.Value().Reset();
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace

TEST(ObjectGcCluster_PackageBoundaryValveCollect) {
    RunPackageBoundaryValve(0);
}

TEST(ObjectGcCluster_PackageBoundaryValveIncremental) {
    RunPackageBoundaryValve(1);
}

TEST(ObjectGcCluster_PackageBoundaryValveParallel) {
    RunPackageBoundaryValve(2);
}

// 경계 비율 안전장치는 미뤄진 장부 감소를 적용한 뒤 판정한다(GC 설계 13.4절). cycle 시작 때 경계가
// 90%인 패키지에서 Marking 중 경계 간선 5개를 지우면 실제 경계는 40%라 해체하지 않는다(적용하지
// 않으면 옛 계수 90%로 해체한다). 패키지는 모두 루트로 쥐어 표식되지 않은 클러스터 해체(먼저 적용
// 한다)가 끼지 않게 한다.
TEST(ObjectGcCluster_BoundaryValveSeesDeferredRemovals) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterPackageTypes(types));
        {
            ObjectRegistry objects(allocator, types, Guid{0x4747, 9}, PackageClusterOptions());
            const auto access = objects.OwnerAccess();
            auto wide = objects.NewObject<PackageNode>(access, PackageNodeType);
            auto target = objects.NewObject<PackageNode>(access, PackageNodeType);
            REQUIRE(wide && target);
            const ObjectHandle w = wide.Value().Handle();
            const ObjectHandle t = target.Value().Handle();
            auto outside = objects.NewObject<ClusterNode>(access, ClusterNodeType, t, "Y");
            REQUIRE(outside);
            const ObjectHandle y = outside.Value().Handle();
            outside.Value().Reset();
            constexpr uint32 Inner = 9;
            ObjectHandle nodes[Inner];
            for (uint32 index = 0; index < Inner; ++index) {
                const char name[] = {'n', static_cast<char>('0' + index)};
                auto node =
                    objects.NewObject<ClusterNode>(access, ClusterNodeType, w, StringView(name, 2));
                REQUIRE(node);
                nodes[index] = node.Value().Handle();
                REQUIRE(Link(objects, nodes[index], y));
            }

            REQUIRE(objects.Stats().clusters == 2);
            REQUIRE(objects.BeginCollection(access));
            for (uint32 index = 0; index < 5; ++index) {
                REQUIRE(Unlink(objects, nodes[index], y));
            }

            GcBudget budget;
            budget.maxObjects = NumericLimits<uint32>::Max();
            budget.maxNanoseconds = 0;
            for (uint32 guard = 0; guard < 64 && objects.CollectionPhase() != GcPhase::Idle;
                 ++guard) {
                REQUIRE(objects.StepCollection(access, budget));
            }

            REQUIRE(objects.CollectionPhase() == GcPhase::Idle);
            CHECK(objects.LastGcReport().flattenedClusters == 0);
            CHECK(objects.Stats().clusters == 2);
            REQUIRE(objects.Discard(access, w));
            REQUIRE(objects.Discard(access, t));
            wide.Value().Reset();
            target.Value().Reset();
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 패키지의 부분 트리 Discard(루트가 아닌 A와 자식 B)는 소속이 하나씩 이탈한다(이탈 루프). 이탈마다
// 미룬 감소를 먼저 적용하므로 직전에 지운 간선이 이탈 판정에 반영된다: B의 경계 간선(B → X)을 지운
// 뒤라 B의 outCount가 0이고(적용하지 않으면 "outgoing ledger edges" Fatal), 안쪽 간선(C → B)도
// 지웠으므로 B는 detached가 아니라 클러스터 없음이 되어 다음 cycle에 회수된다.
TEST(ObjectGcCluster_PackageSubtreeDiscardAppliesDeferredRemovals) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterPackageTypes(types));
        {
            ObjectRegistry objects(allocator, types, Guid{0x4747, 10}, PackageClusterOptions());
            const auto access = objects.OwnerAccess();
            auto first = objects.NewObject<PackageNode>(access, PackageNodeType);
            auto second = objects.NewObject<PackageNode>(access, PackageNodeType);
            REQUIRE(first && second);
            const ObjectHandle p = first.Value().Handle();
            const ObjectHandle q = second.Value().Handle();
            auto a = objects.NewObject<ClusterNode>(access, ClusterNodeType, p, "A");
            REQUIRE(a);
            auto b =
                objects.NewObject<ClusterNode>(access, ClusterNodeType, a.Value().Handle(), "B");
            auto c = objects.NewObject<ClusterNode>(access, ClusterNodeType, p, "C");
            auto d = objects.NewObject<ClusterNode>(access, ClusterNodeType, p, "D");
            auto x = objects.NewObject<ClusterNode>(access, ClusterNodeType, q, "X");
            REQUIRE(b && c && d && x);
            const ObjectHandle ah = a.Value().Handle();
            const ObjectHandle bh = b.Value().Handle();
            const ObjectHandle ch = c.Value().Handle();
            const ObjectHandle dh = d.Value().Handle();
            const ObjectHandle xh = x.Value().Handle();
            a.Value().Reset();
            b.Value().Reset();
            c.Value().Reset();
            d.Value().Reset();
            x.Value().Reset();
            REQUIRE(objects.Stats().clusterMembers == 7);
            REQUIRE(Link(objects, ch, bh));
            REQUIRE(Link(objects, bh, dh));
            REQUIRE(Link(objects, bh, xh));
            REQUIRE(objects.Collect(access));
            // 경계 간선과 안쪽 간선을 지우고(미뤄진 감소) 바로 부분 트리를 Discard한다.
            REQUIRE(Unlink(objects, bh, xh));
            REQUIRE(Unlink(objects, ch, bh));
            REQUIRE(objects.Discard(access, ah));
            CHECK(objects.Stats().clusterMembers == 5);
            REQUIRE(objects.Collect(access));
            CHECK(!Exists(objects, ah));
            CHECK(!Exists(objects, bh));
            CHECK(Exists(objects, ch));
            CHECK(Exists(objects, dh));
            CHECK(Exists(objects, xh));
            REQUIRE(objects.Discard(access, p));
            REQUIRE(objects.Discard(access, q));
            first.Value().Reset();
            second.Value().Reset();
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectGcCluster_PermanentRegionCollect) {
    RunPermanentRegion(0);
}

TEST(ObjectGcCluster_PermanentRegionIncremental) {
    RunPermanentRegion(1);
}

TEST(ObjectGcCluster_PermanentRegionParallel) {
    RunPermanentRegion(2);
}

TEST(ObjectGcCluster_PackageClustersCollect) {
    RunPackageClusters(0);
}

TEST(ObjectGcCluster_PackageClustersIncremental) {
    RunPackageClusters(1);
}

TEST(ObjectGcCluster_PackageClustersParallel) {
    RunPackageClusters(2);
}

TEST(ObjectGcCluster_ModelMatchesOracleSerial) {
    RunClusterModel(0x9e3779b97f4a7c15ULL, 0);
    RunClusterModel(0x51ed270b9a3c4d21ULL, 0);
}

TEST(ObjectGcCluster_ModelMatchesOracleParallel) {
    RunClusterModel(0x2545f4914f6cdd1dULL, 2);
    RunClusterModel(0x6a09e667f3bcc909ULL, 4);
}

// ---- 소속 소유자의 StructRefArray 원소 제거·비우기 ----

namespace cluster_struct_fixture {

using namespace rocks;

// 등록 struct 원소다(소속 소유자의 원소 제거·비우기가 원소 trace로 장부를 고친다).
struct ClusterSlot {
    explicit ClusterSlot(Allocator&) noexcept {
    }

    ObjectRef<Object> target;
};

class SlotNode final : public Object {
public:
    explicit SlotNode(Allocator& allocator) noexcept : slots(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    StructRefArray<ClusterSlot> slots;
};

} // namespace cluster_struct_fixture

namespace rocks {

template <>
struct GeneratedType<cluster_struct_fixture::ClusterSlot> {
    static constexpr bool ContainsStrongReference = true;

    static void Trace(const cluster_struct_fixture::ClusterSlot& value,
                      TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }
};

} // namespace rocks

void cluster_struct_fixture::SlotNode::Trace(TraceVisitor& visitor) const noexcept {
    slots.Trace(visitor);
}

namespace {

using namespace cluster_struct_fixture;

constexpr TypeId SlotNodeType{0x47436c7573745335ULL, 1};
constexpr TypeId ClusterSlotType{0x47436c7573745336ULL, 1};
constexpr Guid SlotTargetId{0x47435354ULL, 1};

bool RegisterSlotTypes(TypeRegistry& types) noexcept {
    static FieldSchema fields[1];
    fields[0].id = SlotTargetId;
    fields[0].kind = FieldKind::ObjectReference;
    fields[0].flags = static_cast<uint32>(FieldFlags::Transient);
    fields[0].name = Span<const char>("target", 6);
    fields[0].value = ValueType{ValueKind::ObjectReference, {}};
    static const RuntimeStructFieldOps fieldOps[] = {{SlotTargetId}};
    RuntimeStructOps slot = MakeRuntimeStructOps<ClusterSlot>();
    slot.trace = [](const void* value, TraceVisitor& visitor) noexcept {
        GeneratedType<ClusterSlot>::Trace(*static_cast<const ClusterSlot*>(value), visitor);
    };
    slot.fields = Span<const RuntimeStructFieldOps>(fieldOps, 1);
    RuntimeTypeOps node = MakeRuntimeTypeOps<SlotNode>();
    node.completeFieldLayout = true;
    return RegisterClusterTypes(types) &&
           types
               .RegisterStruct(
                   {ClusterSlotType, 1, Span<const FieldSchema>(fields, 1), {"ClusterSlot", 11}},
                   Move(slot))
               .IsOk() &&
           types.Register(TypeSchema{SlotNodeType, 1, {}, {}}, Move(node));
}

// owner의 원소 하나를 더하고 그 참조를 target으로 둔다.
bool AddSlot(ObjectRegistry& objects, ObjectHandle owner, ObjectHandle target) noexcept {
    auto borrowed = objects.Resolve<SlotNode>(objects.OwnerAccess(), owner);
    if (!borrowed) {
        return false;
    }

    auto pointer = borrowed.Value().Get();
    if (!pointer) {
        return false;
    }

    StructRefArray<ClusterSlot>& slots = pointer.Value()->slots;
    auto index = objects.AddElement(objects.OwnerAccess(), owner, slots);
    return index && objects.SetElementReference(objects.OwnerAccess(), owner, slots, index.Value(),
                                                slots[index.Value()].target, target);
}

template <class Edit>
bool EditSlots(ObjectRegistry& objects, ObjectHandle owner, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<SlotNode>(objects.OwnerAccess(), owner);
    if (!borrowed) {
        return false;
    }

    auto pointer = borrowed.Value().Get();
    return pointer && static_cast<bool>(edit(pointer.Value()->slots));
}

} // namespace

// 소속 소유자 M의 원소가 같은 클러스터 소속 N(두 번)과 밖 객체 O를 가리킨다. 원소 제거는 O의
// 밖 간선(outCount)을, 비우기는 N의 inCount를 장부에서 뺀다. Debug는 Collect마다 장부를 다시 센다
// (빠뜨리면 "Cluster ledger diverged from a recount"). 이탈한 N은 M의 원소가 가리키는 동안
// detached로 살고, 비운 뒤 회수된다.
TEST(ObjectGcCluster_MemberStructElementRemovalKeepsLedger) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterSlotTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, 9}, ClusterOptions());
        const auto access = objects.OwnerAccess();
        ObjectHandle root;
        ObjectHandle m;
        ObjectHandle n;
        RootRef<Object> held[1];
        {
            ConstructionContext transaction(objects, access);
            auto made = transaction.Create<SlotNode>(SlotNodeType);
            REQUIRE(made && transaction.SetClusterRoot(made.Value()));
            root = made.Value();
            auto owner = transaction.Create<SlotNode>(SlotNodeType, root, "M");
            auto inner = transaction.Create<ClusterNode>(ClusterNodeType, root, "N");
            REQUIRE(owner && inner);
            REQUIRE(transaction.JoinCluster(owner.Value(), root));
            REQUIRE(transaction.JoinCluster(inner.Value(), root));
            m = owner.Value();
            n = inner.Value();
            const ObjectHandle roots[] = {root};
            REQUIRE(transaction.Commit(roots, held));
        }

        auto outside = objects.NewObject<ClusterNode>(access, PlainNodeType);
        REQUIRE(outside);
        const ObjectHandle o = outside.Value().Handle();
        outside.Value().Reset();
        REQUIRE(AddSlot(objects, m, n));
        REQUIRE(AddSlot(objects, m, o));
        REQUIRE(AddSlot(objects, m, n));
        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, o));
        // 원소 제거: 밖 간선이 빠지고 O는 닿지 않는다.
        REQUIRE(EditSlots(objects, m, [&](StructRefArray<ClusterSlot>& slots) noexcept {
            return objects.RemoveElementAt(access, m, slots, 1);
        }));
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, o));
        // N 이탈: M의 원소 둘이 가리키므로 detached로 산다.
        REQUIRE(objects.RequestDestroy(access, n));
        REQUIRE(objects.Collect(access));
        CHECK(Exists(objects, n));
        // 비우기: N의 inCount가 0이 되어 클러스터 없음 — 회수된다.
        REQUIRE(EditSlots(objects, m, [&](StructRefArray<ClusterSlot>& slots) noexcept {
            return objects.ClearElements(access, m, slots);
        }));
        REQUIRE(objects.Collect(access));
        CHECK(!Exists(objects, n));
        CHECK(Exists(objects, m));
        held[0].Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

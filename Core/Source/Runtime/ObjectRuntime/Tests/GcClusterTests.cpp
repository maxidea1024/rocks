#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
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
        const ObjectHandle outsideHandle = outside.Value().Handle();
        const ObjectHandle member = cluster.members[0];
        REQUIRE(Link(objects, member, outsideHandle));
        outside.Value().Reset();
        // 루트를 놓아 클러스터가 루트 목록으로 표식되지 않게 한다.
        const ObjectHandle rootHandle = cluster.root.Handle();
        cluster.root.Reset();
        REQUIRE(objects.BeginCollection(access));
        // keeper(이미 표식된 루트)가 소속 m0를 가리키게 한다: 장벽이 클러스터를 shade만 한다.
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

} // namespace

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

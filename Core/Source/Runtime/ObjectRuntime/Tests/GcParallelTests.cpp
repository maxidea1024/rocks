#include <TestSupport/Test.h>

#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Task/WorkerScheduler.h>

// 병렬 표식(GC-S4) 시험이다(Docs/Design/GarbageCollectionScale.ko.md 6절 GC-S4, 9절).
// - worker 1·2·N개의 정지형·증분 병렬 표식이 seed 고정 무작위 그래프에서 독립 oracle(시험 안의
// BFS)과
//   같은 생존 집합을 내고 끝난다. 그래프에는 병렬 안전 타입, 수동 Trace(소유 스레드 전용) 타입, 잎
//   타입, Outer 사슬이 섞여 소유 스레드 큐와 worker 큐가 서로를 채운다.
// - 병렬 slice 사이의 삽입 장벽(장벽 큐)이 slice 뒤에 연결한 객체를 지킨다.
// - Debug는 강한 간선의 옛 handle(회수된 객체 세대)을 잡는다(Release는 세대를 보지 않는다).
// 단언은 결과 집합과 계수로만 하고 시간·경쟁 운에 기대지 않는다(표식 순서는 실행마다 달라도 된다).
namespace {

using namespace rocks;

constexpr TypeId ParallelNodeType{0x4750617261316e64ULL, 1};
constexpr TypeId ManualNodeType{0x475061726132616eULL, 1};
constexpr TypeId ParallelLeafType{0x4750617261336c66ULL, 1};
constexpr TypeId PoisonNodeType{0x4750617261347073ULL, 1};

class ParallelNode final : public Object {
public:
    explicit ParallelNode(Allocator& allocator) noexcept : items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        items.Trace(visitor);
    }

    ObjectRefArray<Object> items;
};

// parallelTraceSafe가 아닌 수동 Trace 타입이다(소유 스레드만 trace한다).
class ManualNode final : public Object {
public:
    explicit ManualNode(Allocator& allocator) noexcept : items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        items.Trace(visitor);
    }

    ObjectRefArray<Object> items;
};

class ParallelLeaf final : public Object {
public:
    ParallelLeaf() noexcept = default;
};

// 강한 간선 칸에 아무 handle이나 둘 수 있는 시험 타입이다.
class PoisonNode final : public Object {
public:
    PoisonNode() noexcept = default;

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(poison);
    }

    ObjectHandle poison;
};

bool RegisterParallelTypes(TypeRegistry& types) noexcept {
    RuntimeTypeOps node = MakeRuntimeTypeOps<ParallelNode>();
    node.parallelTraceSafe = true;
    RuntimeTypeOps manual = MakeRuntimeTypeOps<ManualNode>();
    RuntimeTypeOps leaf = MakeRuntimeTypeOps<ParallelLeaf>();
    leaf.parallelTraceSafe = true;
    leaf.hasStrongReferences = false;
    RuntimeTypeOps poison = MakeRuntimeTypeOps<PoisonNode>();
    poison.parallelTraceSafe = true;
    return types.Register(TypeSchema{ParallelNodeType, 1, {}, {}}, Move(node)) &&
           types.Register(TypeSchema{ManualNodeType, 1, {}, {}}, Move(manual)) &&
           types.Register(TypeSchema{ParallelLeafType, 1, {}, {}}, Move(leaf)) &&
           types.Register(TypeSchema{PoisonNodeType, 1, {}, {}}, Move(poison));
}

struct Rng {
    uint64 state;

    uint32 Below(uint32 bound) noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<uint32>((state * 0x2545f4914f6cdd1dULL) % bound);
    }
};

constexpr uint32 GraphSize = 6000;
constexpr uint32 GroupSize = 64;

enum class Kind : uint8 {
    Node,
    Manual,
    Leaf
};

// 시험 그래프와 그 oracle이다. 64개 묶음마다 머리(Node) 아래 inner(Outer 있음)를 두고, 노드·수동
// 노드마다 0–6개 무작위 간선을 넣는다. 루트는 무작위 100개다.
struct Graph {
    Kind kinds[GraphSize];
    uint32 outer[GraphSize];
    uint32 edges[GraphSize][6];
    uint32 degree[GraphSize];
    bool rooted[GraphSize];
    bool reachable[GraphSize];
    uint32 reachableCount = 0;

    void Build(uint64 seed) noexcept {
        Rng rng{seed};
        reachableCount = 0;
        for (uint32 index = 0; index < GraphSize; ++index) {
            const uint32 roll = rng.Below(10);
            kinds[index] = index % GroupSize == 0 ? Kind::Node
                           : roll < 2             ? Kind::Manual
                           : roll < 5             ? Kind::Leaf
                                                  : Kind::Node;
            outer[index] = index % GroupSize == 0 ? GraphSize : index - index % GroupSize;
            degree[index] = kinds[index] == Kind::Leaf ? 0 : rng.Below(7);
            for (uint32 edge = 0; edge < degree[index]; ++edge) {
                edges[index][edge] = rng.Below(GraphSize);
            }

            rooted[index] = false;
            reachable[index] = false;
        }

        for (uint32 count = 0; count < 100; ++count) {
            rooted[rng.Below(GraphSize)] = true;
        }

        // oracle: 루트에서 간선과 inner → outer로 닿는 객체.
        static uint32 stack[GraphSize];
        uint32 top = 0;
        for (uint32 index = 0; index < GraphSize; ++index) {
            if (rooted[index] && !reachable[index]) {
                reachable[index] = true;
                stack[top++] = index;
            }
        }

        while (top != 0) {
            const uint32 index = stack[--top];
            auto visit = [&](uint32 next) noexcept {
                if (next < GraphSize && !reachable[next]) {
                    reachable[next] = true;
                    stack[top++] = next;
                }
            };
            visit(outer[index]);
            for (uint32 edge = 0; edge < degree[index]; ++edge) {
                visit(edges[index][edge]);
            }
        }

        for (bool value : reachable) {
            reachableCount += value ? 1u : 0u;
        }
    }
};

// graph를 registry에 만든다. 모든 객체를 잠시 루트로 쥔 채 간선을 넣고, 그래프의 루트만 남긴다.
bool BuildGraph(ObjectRegistry& objects, const Graph& graph, ObjectHandle* handles,
                RootRef<Object>* roots) noexcept {
    const auto access = objects.OwnerAccess();
    char name[16] = {'n'};
    for (uint32 index = 0; index < GraphSize; ++index) {
        const TypeId type = graph.kinds[index] == Kind::Node     ? ParallelNodeType
                            : graph.kinds[index] == Kind::Manual ? ManualNodeType
                                                                 : ParallelLeafType;
        uint32 length = 1;
        for (uint32 value = index; value != 0 || length == 1; value /= 10) {
            name[length++] = static_cast<char>('0' + value % 10);
            if (value < 10) {
                break;
            }
        }

        const ObjectHandle outer =
            graph.outer[index] < GraphSize ? handles[graph.outer[index]] : ObjectHandle{};
        auto created = objects.NewObject<Object>(
            access, type, outer, outer.IsNull() ? StringView{} : StringView(name, length));
        if (!created) {
            return false;
        }

        handles[index] = created.Value().Handle();
        roots[index] = Move(created.Value());
    }

    for (uint32 index = 0; index < GraphSize; ++index) {
        if (graph.degree[index] == 0) {
            continue;
        }

        auto borrowed = objects.Resolve<Object>(access, handles[index]);
        if (!borrowed) {
            return false;
        }

        auto pointer = borrowed.Value().Get();
        if (!pointer) {
            return false;
        }

        ObjectRefArray<Object>& items = graph.kinds[index] == Kind::Node
                                            ? static_cast<ParallelNode*>(pointer.Value())->items
                                            : static_cast<ManualNode*>(pointer.Value())->items;
        for (uint32 edge = 0; edge < graph.degree[index]; ++edge) {
            if (!objects.AddReference(access, handles[index], items,
                                      handles[graph.edges[index][edge]])) {
                return false;
            }
        }
    }

    for (uint32 index = 0; index < GraphSize; ++index) {
        if (!graph.rooted[index]) {
            roots[index].Reset();
        }
    }

    return true;
}

bool MatchesOracle(ObjectRegistry& objects, const Graph& graph,
                   const ObjectHandle* handles) noexcept {
    for (uint32 index = 0; index < GraphSize; ++index) {
        auto lifecycle = objects.InspectLifecycle(objects.OwnerAccess(), handles[index]);
        const bool alive = lifecycle && lifecycle.Value() == ObjectLifecycle::Alive;
        if (alive != graph.reachable[index]) {
            return false;
        }
    }

    return true;
}

// workers개 worker로 정지형(incremental이 거짓) 또는 증분 병렬 표식을 하고 oracle과 대조한다.
void RunOracle(uint32 workers, bool incremental, uint64 seed) noexcept {
    static Graph graph;
    graph.Build(seed);
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({workers, 256, 16}));
        TypeRegistry types(allocator);
        REQUIRE(RegisterParallelTypes(types));
        static ObjectHandle handles[GraphSize];
        static RootRef<Object> roots[GraphSize];
        {
            ObjectRegistry objects(allocator, types, Guid{0x4750, seed * 16 + workers});
            const auto access = objects.OwnerAccess();
            REQUIRE(BuildGraph(objects, graph, handles, roots));
            if (incremental) {
                REQUIRE(objects.BeginCollection(access));
                uint32 slices = 0;
                // 작은 객체 예산으로 slice마다 남은 회색 객체를 다음 slice로 넘긴다.
                while (objects.CollectionPhase() != GcPhase::Idle && slices < 4 * GraphSize) {
                    REQUIRE(objects.StepCollection(access, GcBudget{97, 0}, &scheduler));
                    ++slices;
                }

                CHECK(objects.CollectionPhase() == GcPhase::Idle);
                CHECK(slices > 3);
            } else {
                REQUIRE(objects.Collect(access, &scheduler));
            }

            CHECK(objects.LastGcReport().marked == graph.reachableCount);
            CHECK(objects.Stats().liveObjects == graph.reachableCount);
            CHECK(MatchesOracle(objects, graph, handles));
            // 같은 그래프의 직렬 Collect(기준 구현)도 같은 결과다.
            REQUIRE(objects.Collect(access));
            CHECK(objects.LastGcReport().marked == graph.reachableCount);
            CHECK(MatchesOracle(objects, graph, handles));
            for (RootRef<Object>& root : roots) {
                root.Reset();
            }

            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace

TEST(ObjectGcParallel_CollectMatchesOracleWithOneWorker) {
    RunOracle(1, false, 11);
}

TEST(ObjectGcParallel_CollectMatchesOracleWithTwoWorkers) {
    RunOracle(2, false, 12);
}

TEST(ObjectGcParallel_CollectMatchesOracleWithEightWorkers) {
    RunOracle(8, false, 13);
}

TEST(ObjectGcParallel_IncrementalMatchesOracleWithOneWorker) {
    RunOracle(1, true, 14);
}

TEST(ObjectGcParallel_IncrementalMatchesOracleWithTwoWorkers) {
    RunOracle(2, true, 15);
}

TEST(ObjectGcParallel_IncrementalMatchesOracleWithEightWorkers) {
    RunOracle(8, true, 16);
}

// 병렬 slice 사이의 삽입 장벽: 이미 trace된 루트에 white 객체를 연결하면 장벽 큐가 그 객체를 다음
// slice의 씨앗으로 넘겨 이번 cycle에 살린다. 연결하지 않은 white는 회수된다.
TEST(ObjectGcParallel_BarrierBetweenParallelSlicesKeepsLinkedObjects) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({4, 256, 16}));
        TypeRegistry types(allocator);
        REQUIRE(RegisterParallelTypes(types));
        {
            ObjectRegistry objects(allocator, types, Guid{0x4751, 1});
            const auto access = objects.OwnerAccess();
            constexpr uint32 Chain = 600;
            // 루트 → 긴 사슬(slice 여러 개가 걸린다). linked·loose는 루트 없는 white다.
            auto root = objects.NewObject<ParallelNode>(access, ParallelNodeType);
            REQUIRE(root);
            ObjectHandle previous = root.Value().Handle();
            for (uint32 index = 0; index < Chain; ++index) {
                auto next = objects.NewObject<Object>(access, ParallelNodeType);
                REQUIRE(next);
                auto borrowed = objects.Resolve<ParallelNode>(access, previous);
                REQUIRE(borrowed);
                auto pointer = borrowed.Value().Get();
                REQUIRE(pointer);
                REQUIRE(objects.AddReference(access, previous, pointer.Value()->items,
                                             next.Value().Handle()));
                previous = next.Value().Handle();
            }

            auto linked = objects.NewObject<Object>(access, ParallelLeafType);
            auto loose = objects.NewObject<Object>(access, ParallelLeafType);
            REQUIRE(linked && loose);
            const ObjectHandle linkedHandle = linked.Value().Handle();
            const ObjectHandle looseHandle = loose.Value().Handle();
            linked.Value().Reset();
            loose.Value().Reset();
            REQUIRE(objects.BeginCollection(access));
            REQUIRE(objects.StepCollection(access, GcBudget{4, 0}, &scheduler));
            REQUIRE(objects.CollectionPhase() == GcPhase::Marking);
            {
                // 루트는 이미 trace되었다(black). 장벽이 linked를 장벽 큐로 shade한다.
                auto borrowed = objects.Resolve<ParallelNode>(access, root.Value().Handle());
                REQUIRE(borrowed);
                auto pointer = borrowed.Value().Get();
                REQUIRE(pointer);
                REQUIRE(objects.AddReference(access, root.Value().Handle(), pointer.Value()->items,
                                             linkedHandle));
            }

            for (uint32 guard = 0; guard < 4 * Chain && objects.CollectionPhase() != GcPhase::Idle;
                 ++guard) {
                REQUIRE(objects.StepCollection(access, GcBudget{4, 0}, &scheduler));
            }

            CHECK(objects.CollectionPhase() == GcPhase::Idle);
            auto linkedState = objects.InspectLifecycle(access, linkedHandle);
            CHECK(linkedState && linkedState.Value() == ObjectLifecycle::Alive);
            auto looseState = objects.InspectLifecycle(access, looseHandle);
            CHECK(!looseState && looseState.Error() == ErrorCode::StaleHandle);
            CHECK(objects.Stats().liveObjects == Chain + 2);
            root.Value().Reset();
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 강한 간선에 회수된 객체의 옛 handle(같은 슬롯을 새 객체가 재사용)이 남은 계약 위반을 Debug는
// StaleHandle로 잡는다(직렬·병렬). Release·Development는 세대를 보지 않아 새 객체를 표식한다.
TEST(ObjectGcParallel_DebugCatchesStaleHandleInStrongEdge) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({2, 256, 16}));
        TypeRegistry types(allocator);
        REQUIRE(RegisterParallelTypes(types));
        {
            ObjectRegistry objects(allocator, types, Guid{0x4752, 1});
            const auto access = objects.OwnerAccess();
            auto holder = objects.NewObject<PoisonNode>(access, PoisonNodeType);
            auto victim = objects.NewObject<Object>(access, ParallelLeafType);
            REQUIRE(holder && victim);
            const ObjectHandle stale = victim.Value().Handle();
            victim.Value().Reset();
            REQUIRE(objects.Collect(access));
            // 빈 슬롯 목록은 가장 최근에 회수한 슬롯부터 쓴다: 새 객체가 같은 슬롯·다음 세대다.
            auto reuse = objects.NewObject<Object>(access, ParallelLeafType);
            REQUIRE(reuse);
            REQUIRE(reuse.Value().Handle().slot == stale.slot);
            {
                auto borrowed = objects.Resolve<PoisonNode>(access, holder.Value().Handle());
                REQUIRE(borrowed);
                auto pointer = borrowed.Value().Get();
                REQUIRE(pointer);
                pointer.Value()->poison = stale;
            }

            auto serial = objects.Collect(access);
            auto parallel = objects.Collect(access, &scheduler);
            if constexpr (IsDebugBuild) {
                CHECK(!serial && serial.Error() == ErrorCode::StaleHandle);
                CHECK(!parallel && parallel.Error() == ErrorCode::StaleHandle);
            } else {
                CHECK(serial && parallel);
            }

            {
                auto borrowed = objects.Resolve<PoisonNode>(access, holder.Value().Handle());
                REQUIRE(borrowed);
                auto pointer = borrowed.Value().Get();
                REQUIRE(pointer);
                pointer.Value()->poison = {};
            }

            holder.Value().Reset();
            reuse.Value().Reset();
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

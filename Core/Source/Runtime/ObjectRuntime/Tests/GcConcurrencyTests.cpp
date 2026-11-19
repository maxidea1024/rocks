#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <Task/WorkerScheduler.h>

// GC-S1(잠금 없는 직렬 marker)의 전제 시험이다(Docs/Design/GarbageCollectionScale.ko.md 7.1·7.2절).
// - 표식 slice 안에서 다른 스레드가 RootRef·BackendLease·EncodePin을 반환해도 결과가 정의대로다.
//   순서는 탐침 객체의 Trace가 helper 스레드를 깨우고 끝날 때까지 기다리는 명시 hand-off로
//   만든다(시간· 경쟁 운에 기대지 않는다).
// - 배치 visitor가 방문 순서를 지키고 버퍼(256개)를 넘는 Trace에서도 간선을 빠뜨리지 않는다.
namespace {

using namespace rocks;

constexpr TypeId AuditNodeType{0x4743417564697431ULL, 1};
constexpr TypeId AuditProbeType{0x4743417564697432ULL, 1};
constexpr TypeId AuditWideType{0x4743417564697433ULL, 1};

class AuditNode final : public Object {
public:
    AuditNode() noexcept = default;

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(next);
    }

    ObjectRef<Object> next;
};

class AuditWide final : public Object {
public:
    explicit AuditWide(Allocator& allocator) noexcept : items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        items.Trace(visitor);
    }

    ObjectRefArray<Object> items;
};

// helper 스레드가 slice 안에서 할 일이다. 소유 스레드가 보유를 옮겨 둔다.
struct ReleaseWork {
    platform::Semaphore go{0, 1};
    platform::Semaphore done{0, 1};
    RootRef<Object> root;
    BackendLease lease;
    EncodePin pin;
    RootRef<Object>* cloneSource = nullptr;
    ErrorCode cloneResult = ErrorCode::None;
    uint32 traced = 0;
    GcPhase phaseInTrace = GcPhase::Idle;
    ObjectRegistry* registry = nullptr;

    static void Run(void* raw) noexcept {
        auto& work = *static_cast<ReleaseWork*>(raw);
        if (work.go.Wait() != platform::WaitResult::Signaled) {
            return;
        }

        // slice가 진행 중이다(소유 스레드는 Trace 안에서 done을 기다린다).
        auto cloned = work.cloneSource->Clone();
        work.cloneResult = cloned ? ErrorCode::None : cloned.Error();
        work.root.Reset();
        work.lease.Reset();
        work.pin.Reset();
        work.done.Release();
    }
};

ReleaseWork* activeWork = nullptr;

class AuditProbe final : public Object {
public:
    AuditProbe() noexcept = default;

    void Trace(TraceVisitor&) const noexcept {
        ReleaseWork* work = activeWork;
        if (work == nullptr) {
            return;
        }

        activeWork = nullptr;
        ++work->traced;
        work->phaseInTrace = work->registry->CollectionPhase();
        work->go.Release();
        (void)work->done.Wait();
    }
};

bool RegisterAuditTypes(TypeRegistry& types) noexcept {
    return types.Register(TypeSchema{AuditNodeType, 1, {}, {}}, MakeRuntimeTypeOps<AuditNode>()) &&
           types.Register(TypeSchema{AuditProbeType, 1, {}, {}},
                          MakeRuntimeTypeOps<AuditProbe>()) &&
           types.Register(TypeSchema{AuditWideType, 1, {}, {}}, MakeRuntimeTypeOps<AuditWide>());
}

bool IsReclaimed(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto lifecycle = objects.InspectLifecycle(objects.OwnerAccess(), handle);
    return !lifecycle && lifecycle.Error() == ErrorCode::StaleHandle;
}

bool IsAlive(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto lifecycle = objects.InspectLifecycle(objects.OwnerAccess(), handle);
    return lifecycle && lifecycle.Value() == ObjectLifecycle::Alive;
}

// incremental이 참이면 Begin·Step, 거짓이면 Collect로 수집한다. workers가 0이 아니면 그 수의
// worker로 병렬 표식한다(탐침·노드는 수동 Trace라 소유 스레드가 trace하고 그 안에서 hand-off한다).
void RunCrossThreadReleaseDuringSlice(bool incremental, uint32 workers) noexcept {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        WorkerScheduler* parallel = nullptr;
        if (workers != 0) {
            REQUIRE(scheduler.Start({workers, 256, 16}));
            parallel = &scheduler;
        }

        TypeRegistry types(allocator);
        REQUIRE(RegisterAuditTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4743, incremental ? 1u : 2u});
        const auto access = objects.OwnerAccess();
        // 탐침(루트), a → child(a의 루트는 slice 안에서 반환), garbage(루트 없음, backend lease를
        // slice 안에서 반환), pinned(루트 있음, encode 고정을 slice 안에서 반환), kept(루트 — slice
        // 안 복제 시도 대상).
        auto probe = objects.NewObject<AuditProbe>(access, AuditProbeType);
        auto a = objects.NewObject<Object>(access, AuditNodeType);
        auto child = objects.NewObject<AuditNode>(access, AuditNodeType);
        auto garbage = objects.NewObject<AuditNode>(access, AuditNodeType);
        auto pinned = objects.NewObject<AuditNode>(access, AuditNodeType);
        auto kept = objects.NewObject<Object>(access, AuditNodeType);
        REQUIRE(probe && a && child && garbage && pinned && kept);
        const ObjectHandle aHandle = a.Value().Handle();
        const ObjectHandle childHandle = child.Value().Handle();
        const ObjectHandle garbageHandle = garbage.Value().Handle();
        const ObjectHandle pinnedHandle = pinned.Value().Handle();
        {
            auto borrowed = objects.Resolve<AuditNode>(access, aHandle);
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            REQUIRE(objects.SetReference(access, aHandle, pointer.Value()->next, childHandle));
        }

        child.Value().Reset();
        auto lease = objects.AcquireBackendLease(access, garbageHandle);
        auto pin = objects.PinForEncode(access, pinnedHandle);
        REQUIRE(lease && pin);
        garbage.Value().Reset();

        ReleaseWork work;
        work.registry = &objects;
        work.root = Move(a.Value());
        work.lease = Move(lease.Value());
        work.pin = Move(pin.Value());
        work.cloneSource = &kept.Value();
        platform::Thread helper;
        REQUIRE(helper.Start(&ReleaseWork::Run, &work) == platform::ThreadStatus::Success);
        activeWork = &work;
        if (incremental) {
            REQUIRE(objects.BeginCollection(access));
            const GcBudget unlimited{NumericLimits<uint32>::Max(), 0};
            for (uint32 guard = 0; guard < 8 && objects.CollectionPhase() != GcPhase::Idle;
                 ++guard) {
                REQUIRE(objects.StepCollection(access, unlimited, parallel));
            }

            CHECK(objects.CollectionPhase() == GcPhase::Idle);
        } else {
            REQUIRE(objects.Collect(access, parallel));
        }

        activeWork = nullptr;
        REQUIRE(helper.Join() == platform::ThreadStatus::Success);
        // 탐침은 한 번 trace되었고 그 안에서 반환이 실행됐다. 복제는 처리 표시(slice) 중이라
        // Busy다.
        CHECK(work.traced == 1);
        CHECK(work.phaseInTrace == (incremental ? GcPhase::Marking : GcPhase::Idle));
        CHECK(work.cloneResult == ErrorCode::Busy);
        // a·child는 Begin 때 루트라 이번 cycle에 남고(floating) 다음 수집에서 회수된다. garbage는
        // backend lease가 slice 안에서 반환됐으므로 이번 cycle에서 회수된다.
        CHECK(IsAlive(objects, aHandle));
        CHECK(IsAlive(objects, childHandle));
        CHECK(IsReclaimed(objects, garbageHandle));
        CHECK(IsAlive(objects, pinnedHandle));
        const ObjectStats stats = objects.Stats();
        CHECK(stats.roots == 3);
        CHECK(stats.backendHolds == 0);
        CHECK(stats.encodePins == 0);
        REQUIRE(objects.Collect(access));
        CHECK(IsReclaimed(objects, aHandle));
        CHECK(IsReclaimed(objects, childHandle));
        CHECK(IsAlive(objects, pinnedHandle));
        probe.Value().Reset();
        pinned.Value().Reset();
        kept.Value().Reset();
        REQUIRE(objects.Shutdown(access));
        if (parallel != nullptr) {
            REQUIRE(scheduler.Shutdown());
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

struct OrderLog {
    ObjectHandle seen[600];
    uint32 count = 0;
    uint32 flushes = 0;

    static void Record(void* raw, const ObjectHandle* handles, uint32 count) noexcept {
        auto& log = *static_cast<OrderLog*>(raw);
        ++log.flushes;
        for (uint32 index = 0; index < count; ++index) {
            log.seen[log.count++] = handles[index];
        }
    }
};

} // namespace

TEST(ObjectGc_CrossThreadReleasesDuringIncrementalSliceAreSafe) {
    RunCrossThreadReleaseDuringSlice(true, 0);
}

TEST(ObjectGc_CrossThreadReleasesDuringCollectAreSafe) {
    RunCrossThreadReleaseDuringSlice(false, 0);
}

TEST(ObjectGc_CrossThreadReleasesDuringParallelSliceAreSafe) {
    RunCrossThreadReleaseDuringSlice(true, 3);
}

TEST(ObjectGc_CrossThreadReleasesDuringParallelCollectAreSafe) {
    RunCrossThreadReleaseDuringSlice(false, 3);
}

// 배치 visitor는 null을 빼고 방문 순서 그대로 넘기며, 버퍼가 차면 Trace 도중에도 넘긴다.
TEST(TraceVisitor_BatchModeKeepsVisitOrder) {
    OrderLog log;
    TraceVisitor visitor(&log, &OrderLog::Record);
    for (uint32 index = 0; index < 600; ++index) {
        visitor.Visit(index % 7 == 3 ? ObjectHandle{} : ObjectHandle{1, index, 1});
    }

    visitor.Flush();
    visitor.Flush();
    uint32 expected = 0;
    bool ordered = true;
    for (uint32 index = 0; index < 600; ++index) {
        if (index % 7 == 3) {
            continue;
        }

        ordered = ordered && log.seen[expected].slot == index;
        ++expected;
    }

    CHECK(log.count == expected);
    CHECK(ordered);
    CHECK(log.flushes ==
          (expected + TraceVisitor::BatchCapacity - 1) / TraceVisitor::BatchCapacity);
}

// 한 Trace가 배치 버퍼보다 많은 간선을 방문해도 정지형·증분 수집이 모두 표식한다.
TEST(ObjectGc_WideTraceLargerThanVisitorBatchMarksEveryEdge) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAuditTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x4744, 1});
        const auto access = objects.OwnerAccess();
        constexpr uint32 Count = 1000;
        auto wide = objects.NewObject<AuditWide>(access, AuditWideType);
        REQUIRE(wide);
        {
            auto borrowed = objects.Resolve<AuditWide>(access, wide.Value().Handle());
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            for (uint32 index = 0; index < Count; ++index) {
                auto item = objects.NewObject<AuditNode>(access, AuditNodeType);
                REQUIRE(item);
                REQUIRE(objects.AddReference(access, wide.Value().Handle(), pointer.Value()->items,
                                             item.Value().Handle()));
            }
        }

        auto collected = objects.Collect(access);
        REQUIRE(collected);
        CHECK(collected.Value().marked == Count + 1);
        CHECK(objects.Stats().liveObjects == Count + 1);
        REQUIRE(objects.BeginCollection(access));
        for (uint32 guard = 0; guard < 4 * Count && objects.CollectionPhase() != GcPhase::Idle;
             ++guard) {
            REQUIRE(objects.StepCollection(access, GcBudget{1, 0}));
        }

        CHECK(objects.CollectionPhase() == GcPhase::Idle);
        CHECK(objects.LastGcReport().marked == Count + 1);
        CHECK(objects.Stats().liveObjects == Count + 1);
        wide.Value().Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

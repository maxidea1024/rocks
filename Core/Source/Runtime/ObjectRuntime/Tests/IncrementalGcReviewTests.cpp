#include <TestSupport/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Task/WorkerScheduler.h>

using namespace rocks;
using namespace rocks::platform;

namespace {
constexpr Guid ReviewType{0x4743524556494557ULL, 1};

struct TraceGate {
    ManualResetEvent entered;
    ManualResetEvent release;
    ManualResetEvent finished;
};

struct ReviewNode final : Object {
    ObjectRef<ReviewNode> child;
    TraceGate* test = nullptr;

    void Trace(TraceVisitor& visitor) const noexcept {
        if (test != nullptr) {
            test->entered.Signal();
            if (test->release.Wait(10000) != WaitResult::Signaled) {
                Fatal("GC trace review barrier was not released");
            }
        }
        visitor.Visit(child);
        if (test != nullptr) {
            test->finished.Signal();
        }
    }
};

class FailSecondRecord final : public Allocator {
public:
    explicit FailSecondRecord(Allocator& source) noexcept : backing(source) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (armed && ++attempts == 2) {
            armed = false;
            rejected.Signal();
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }
        return backing.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* pointer) noexcept override {
        return backing.Deallocate(pointer);
    }

    Allocator& backing;
    bool armed = false;
    uint32 attempts = 0; // 이 fixture의 Allocate는 GC owner 한 스레드에서만 호출한다.
    ManualResetEvent rejected;
};

Result<TypeRegistrationToken> RegisterReviewType(TypeRegistry& types) noexcept {
    auto ops = MakeRuntimeTypeOps<ReviewNode>();
    ops.parallelTraceSafe = true;
    return types.Register({ReviewType, 1, {}, {}}, Move(ops));
}
} // namespace

// 병렬 표식(GC-S4)은 worker 수만큼 task를 한 번 제출한다. 두 번째 제출이 실패하면 이미 받아들여진
// task를 모두 회수한 뒤(그 task의 stack 표식 상태가 반환 뒤에 쓰이지 않는다) 오류를 돌려주고
// cycle을 버린다.
TEST(ObjectIncrementalGcDrainsAcceptedMarkerBeforeReturningLaterSubmissionFailure) {
    SystemAllocator allocator;
    {
        FailSecondRecord fault(allocator);
        WorkerScheduler scheduler(fault);
        REQUIRE(scheduler.Start({2, 8, 4}));
        TypeRegistry types(allocator);
        REQUIRE(RegisterReviewType(types));
        ObjectRegistry registry(allocator, types, {82, 1});
        const auto access = registry.OwnerAccess();
        auto first = registry.NewObject<ReviewNode>(access, ReviewType);
        auto second = registry.NewObject<ReviewNode>(access, ReviewType);
        REQUIRE(first && second);
        {
            auto borrow = registry.Resolve<ReviewNode>(access, first.Value().Handle());
            REQUIRE(borrow);
            auto pointer = borrow.Value().Get();
            REQUIRE(pointer);
            REQUIRE(registry.SetReference(access, first.Value().Handle(), pointer.Value()->child,
                                          second.Value().Handle()));
        }
        REQUIRE(registry.BeginCollection(access));
        fault.armed = true;
        auto failed = registry.StepCollection(access, {8, 0}, &scheduler);
        CHECK(fault.rejected.Wait(0) == WaitResult::Signaled);
        // 받아들여진 task는 반환 전에 끝났다.
        CHECK(scheduler.Stats().inFlight == 0);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(registry.CollectionPhase() == GcPhase::Idle);
        CHECK(registry.Stats().liveObjects == 2);
        CHECK(registry.Stats().retiredObjects == 0);
        // 버린 cycle 뒤의 새 병렬 수집은 끝까지 간다.
        REQUIRE(registry.BeginCollection(access));
        for (uint32 guard = 0; guard < 8 && registry.CollectionPhase() != GcPhase::Idle; ++guard) {
            REQUIRE(registry.StepCollection(access, {8, 0}, &scheduler));
        }

        CHECK(registry.CollectionPhase() == GcPhase::Idle);
        CHECK(registry.Stats().liveObjects == 2);
        REQUIRE(scheduler.Shutdown());
        first.Value().Reset();
        second.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIncrementalGcRejectsOwnSchedulerBeforeSubmittingStackBackedMarkers) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({1, 8, 4}));
        bool rejected = false;
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            allocator, [&](WorkerTaskContext&) noexcept {
                TypeRegistry types(allocator);
                REQUIRE(RegisterReviewType(types));
                // Registry owner 자체를 worker로 만든다. CheckAccess만으로는 이 deadlock을 막지
                // 못한다.
                ObjectRegistry registry(allocator, types, {82, 2});
                const auto access = registry.OwnerAccess();
                auto root = registry.NewObject<ReviewNode>(access, ReviewType);
                REQUIRE(root);
                REQUIRE(registry.BeginCollection(access));
                auto step = registry.StepCollection(access, {8, 0}, &scheduler);
                rejected = !step && step.Error() == ErrorCode::WrongThread;
                CHECK(registry.CollectionPhase() == GcPhase::Marking);
                REQUIRE(registry.CancelCollection(access));
                root.Value().Reset();
                REQUIRE(registry.Shutdown(access));
                REQUIRE(types.Shutdown());
            });
        REQUIRE(body);
        auto task = scheduler.Submit(Move(body.Value()));
        REQUIRE(task);
        auto waited = task.Value().Wait(10000);
        REQUIRE(waited && waited.Value() == WaitResult::Signaled);
        CHECK(rejected);
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

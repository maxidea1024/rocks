#include <Tests/Support/Test.h>
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

struct ReleaseContext {
    FailSecondRecord* fault;
    TraceGate* trace;
};

void ReleaseAcceptedTraceAfterNextSubmissionFails(void* raw) noexcept {
    auto& context = *static_cast<ReleaseContext*>(raw);
    if (context.fault->rejected.Wait(10000) != WaitResult::Signaled ||
        context.trace->entered.Wait(10000) != WaitResult::Signaled) {
        Fatal("GC partial submission failure was not reached");
    }
    context.trace->release.Signal();
}

Result<TypeRegistrationToken> RegisterReviewType(TypeRegistry& types) noexcept {
    auto ops = MakeRuntimeTypeOps<ReviewNode>();
    ops.parallelTraceSafe = true;
    return types.Register({ReviewType, 1, {}, {}}, Move(ops));
}
} // namespace

TEST(ObjectIncrementalGcDrainsAcceptedMarkerBeforeReturningLaterSubmissionFailure) {
    SystemAllocator allocator;
    {
        TraceGate trace;
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
            pointer.Value()->test = &trace;
        }
        REQUIRE(registry.BeginCollection(access));
        ReleaseContext context{&fault, &trace};
        Thread releaser;
        REQUIRE(releaser.Start(ReleaseAcceptedTraceAfterNextSubmissionFails, &context) ==
                ThreadStatus::Success);
        fault.armed = true;
        auto failed = registry.StepCollection(access, {8, 0}, &scheduler);
        // 두 번째 Submit 실패를 관측해도 첫 Trace의 visitor/stack 문맥은 완료까지 유효해야 한다.
        CHECK(trace.finished.Wait(0) == WaitResult::Signaled);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(registry.CollectionPhase() == GcPhase::Idle);
        CHECK(registry.Stats().liveObjects == 2);
        CHECK(registry.Stats().retiredObjects == 0);
        if (releaser.WaitForCompletion(10000) != WaitResult::Signaled ||
            releaser.Join() != ThreadStatus::Success) {
            Fatal("GC partial submission release fixture did not finish");
        }
        REQUIRE(scheduler.Shutdown());
        {
            auto borrow = registry.Resolve<ReviewNode>(access, first.Value().Handle());
            REQUIRE(borrow);
            auto pointer = borrow.Value().Get();
            REQUIRE(pointer);
            pointer.Value()->test = nullptr;
        }
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

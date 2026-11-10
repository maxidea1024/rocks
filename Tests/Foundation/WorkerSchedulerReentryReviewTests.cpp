#include <Tests/Support/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Task/WorkerScheduler.h>

using namespace rocks;

namespace {
using Body = Function<void(WorkerTaskContext&)>;

class NestedSubmissionAllocator final : public Allocator {
public:
    using Hook = void (*)(void*) noexcept;

    explicit NestedSubmissionAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    void Arm(Hook hook, void* context, bool failAfterHook = false) noexcept {
        platform::LockGuard lock(gate_);
        hook_ = hook;
        context_ = context;
        failAfterHook_ = failAfterHook;
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        Hook hook;
        void* context;
        bool fail;
        {
            platform::LockGuard lock(gate_);
            hook = hook_;
            context = context_;
            fail = failAfterHook_;
            hook_ = nullptr;
            context_ = nullptr;
            failAfterHook_ = false;
        }
        // 사용자 callback은 잠금 밖에서 실행한다. 중첩 Submit 한 번만 주입한다.
        if (hook != nullptr) {
            hook(context);
        }
        if (fail) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
    platform::Mutex gate_;
    Hook hook_ = nullptr;
    void* context_ = nullptr;
    bool failAfterHook_ = false;
};

struct NestedContext {
    WorkerScheduler* first;
    WorkerScheduler* second;
    WorkerScheduler* unrelated;
    Body* innerBody;
    WorkerTaskHandle inner{};
    ErrorCode firstShutdown = ErrorCode::DependencyFailed;
    ErrorCode secondShutdown = ErrorCode::DependencyFailed;
    ErrorCode unrelatedShutdown = ErrorCode::DependencyFailed;
    ErrorCode innerSubmission = ErrorCode::DependencyFailed;
    uint32 outerCalls = 0;
    uint32 innerCalls = 0;
};

void ShutdownFromInnerAllocation(void* pointer) noexcept {
    auto& context = *static_cast<NestedContext*>(pointer);
    ++context.innerCalls;
    // 이전 구현은 최상단 B만 기억하여 A를 Join하고, 아직 반환할 수 없는 A의 제출 예약을 기다렸다.
    context.firstShutdown = context.first->Shutdown().Error();
    context.secondShutdown = context.second->Shutdown().Error();
    // 활성 제출이 없는 별도 scheduler까지 거부해서는 안 된다.
    context.unrelatedShutdown = context.unrelated->Shutdown().Error();
}

void SubmitFromOuterAllocation(void* pointer) noexcept {
    auto& context = *static_cast<NestedContext*>(pointer);
    ++context.outerCalls;
    auto accepted = context.second->Submit(Move(*context.innerBody));
    context.innerSubmission = accepted.Error();
    if (accepted) {
        context.inner = Move(accepted.Value());
    }
}

void CheckSucceeded(const WorkerTaskHandle& handle) noexcept {
    auto waited = handle.Wait(10000);
    if (!waited || waited.Value() != platform::WaitResult::Signaled) {
        platform::Fatal("Nested submission regression did not complete");
    }
    auto status = handle.Status();
    REQUIRE(status);
    CHECK(status.Value().state == WorkerTaskState::Succeeded);
}

void RunNestedSubmission(bool failInner) noexcept {
    SystemAllocator backing;
    {
        NestedSubmissionAllocator firstAllocator(backing);
        NestedSubmissionAllocator secondAllocator(backing);
        WorkerScheduler first(firstAllocator);
        WorkerScheduler second(secondAllocator);
        WorkerScheduler unrelated(backing);
        REQUIRE(first.Start({1, 4, 0}));
        REQUIRE(second.Start({1, 4, 0}));
        REQUIRE(unrelated.Start({1, 4, 0}));
        uint32 firstRuns = 0;
        uint32 secondRuns = 0;
        auto outerBody =
            Body::TryCreate(backing, [&](WorkerTaskContext&) noexcept { ++firstRuns; });
        auto innerBody =
            Body::TryCreate(backing, [&](WorkerTaskContext&) noexcept { ++secondRuns; });
        REQUIRE(outerBody && innerBody);
        NestedContext context{&first, &second, &unrelated, &innerBody.Value()};
        firstAllocator.Arm(SubmitFromOuterAllocation, &context);
        secondAllocator.Arm(ShutdownFromInnerAllocation, &context, failInner);
        auto outer = first.Submit(Move(outerBody.Value()));
        REQUIRE(outer);
        CHECK(context.outerCalls == 1 && context.innerCalls == 1);
        CHECK(context.firstShutdown == ErrorCode::Busy);
        CHECK(context.secondShutdown == ErrorCode::Busy);
        CHECK(context.unrelatedShutdown == ErrorCode::None);
        CHECK(first.Stats().reservedSubmissions == 0);
        CHECK(second.Stats().reservedSubmissions == 0);
        CheckSucceeded(outer.Value());
        CHECK(firstRuns == 1);
        if (failInner) {
            CHECK(context.innerSubmission == ErrorCode::OutOfMemory);
            CHECK(!context.inner.IsValid());
            CHECK(static_cast<bool>(innerBody.Value()));
            CHECK(secondRuns == 0);
        } else {
            REQUIRE(context.innerSubmission == ErrorCode::None);
            CHECK(!innerBody.Value());
            CheckSucceeded(context.inner);
            CHECK(secondRuns == 1);
        }
        // 정상·OOM 경로 모두 TLS 스택을 복구하여 최상위 종료는 다시 성공해야 한다.
        REQUIRE(first.Shutdown());
        REQUIRE(second.Shutdown());
        REQUIRE(unrelated.Shutdown());
    }
    CHECK(backing.Stats().liveAllocations == 0);
}
} // namespace

TEST(WorkerSchedulerRejectsOuterShutdownAcrossNestedSchedulerSubmission) {
    RunNestedSubmission(false);
}

TEST(WorkerSchedulerRestoresNestedSubmissionFramesAfterInnerAllocationFailure) {
    RunNestedSubmission(true);
}

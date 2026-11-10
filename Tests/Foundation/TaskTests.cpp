#include "Tests/Support/Test.h"
#include <Core/Memory/SystemAllocator.h>
#include <Task/TaskScheduler.h>

using namespace rocks;

namespace {
class RejectAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize, usize, uint32 = 0) noexcept override {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    Result<void> Deallocate(void*) noexcept override {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

struct Capture {
    int* live;
    int* calls;

    Capture(int& liveCount, int& callCount) noexcept : live(&liveCount), calls(&callCount) {
        ++*live;
    }

    Capture(Capture&& other) noexcept : live(other.live), calls(other.calls) {
        other.live = nullptr;
    }

    ~Capture() noexcept {
        if (live != nullptr) {
            --*live;
        }
    }

    void operator()(TaskContext&) noexcept {
        ++*calls;
    }
};
} // namespace

TEST(Task_CompletionHandleDoesNotRetainCapture) {
    SystemAllocator allocator;
    InlineExecutor executor(allocator);
    int live = 0;
    int calls = 0;
    auto function = Function<void(TaskContext&)>::TryCreate(allocator, Capture(live, calls));
    REQUIRE(function.IsOk());
    CHECK(live == 1);
    TaskDesc description{Move(function.Value())};
    auto handle = executor.Submit(Move(description));
    REQUIRE(handle.IsOk());
    CHECK(!description.body);
    CHECK(calls == 1);
    CHECK(live == 0);
    REQUIRE(handle.Value().State().IsOk());
    CHECK(handle.Value().State().Value() == TaskState::Succeeded);
    CHECK(allocator.Stats().liveAllocations == 1);
    TaskHandle second(handle.Value());
    handle.Value().Reset();
    CHECK(second.State().IsOk());
    second.Reset();
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Task_RejectedSubmissionPreservesCallerOwnership) {
    SystemAllocator captureAllocator;
    RejectAllocator reject;
    InlineExecutor executor(reject);
    int live = 0;
    int calls = 0;
    auto function = Function<void(TaskContext&)>::TryCreate(captureAllocator, Capture(live, calls));
    REQUIRE(function.IsOk());
    TaskDesc description{Move(function.Value())};
    auto handle = executor.Submit(Move(description));
    CHECK(!handle.IsOk());
    CHECK(handle.Error() == ErrorCode::OutOfMemory);
    CHECK(static_cast<bool>(description.body));
    CHECK(live == 1);
    CHECK(calls == 0);
    description.body.Reset();
    CHECK(live == 0);
    CHECK(captureAllocator.Stats().liveAllocations == 0);
}

TEST(Task_CloseAndFailureAreExplicit) {
    SystemAllocator allocator;
    InlineExecutor executor(allocator);
    auto function =
        Function<void(TaskContext&)>::TryCreate(allocator, [](TaskContext& context) noexcept {
            context.Fail(ErrorCode::DependencyFailed);
        });
    REQUIRE(function.IsOk());
    auto result = executor.Submit(TaskDesc{Move(function.Value())});
    REQUIRE(result.IsOk());
    CHECK(result.Value().State().Value() == TaskState::Failed);
    CHECK(result.Value().Failure() == ErrorCode::DependencyFailed);
    result.Value().Reset();
    executor.Close();
    auto rejectedBody =
        Function<void(TaskContext&)>::TryCreate(allocator, [](TaskContext&) noexcept {});
    REQUIRE(rejectedBody.IsOk());
    TaskDesc rejected{Move(rejectedBody.Value())};
    auto closed = executor.Submit(Move(rejected));
    CHECK(!closed.IsOk());
    CHECK(closed.Error() == ErrorCode::ShuttingDown);
    CHECK(static_cast<bool>(rejected.body));
    rejected.body.Reset();
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Function_MoveOnlyAllocationFailureAndReset) {
    RejectAllocator reject;
    int value = 0;
    auto failed = Function<void()>::TryCreate(reject, [&value]() noexcept { ++value; });
    CHECK(!failed.IsOk());
    SystemAllocator allocator;
    auto created = Function<void()>::TryCreate(allocator, [&value]() noexcept { ++value; });
    REQUIRE(created.IsOk());
    Function<void()> function(Move(created.Value()));
    CHECK(!created.Value());
    function();
    CHECK(value == 1);
    function.Reset();
    function.Reset();
    CHECK(allocator.Stats().liveAllocations == 0);
}

#include <TestSupport/Test.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>

using namespace rocks;
using namespace rocks::platform;

namespace {

void CompleteAndJoin(Thread& thread) noexcept {
    if (!thread.IsJoinable()) {
        return;
    }
    if (thread.WaitForCompletion(10000) != WaitResult::Signaled ||
        thread.Join() != ThreadStatus::Success) {
        Fatal("Concurrent thread control fixture failed to terminate");
    }
}

class ControlFixture;

struct ControlAction {
    ControlFixture* fixture = nullptr;
    bool joining = false;
    ThreadStatus joinResult = ThreadStatus::SystemError;
    WaitResult waitResult = WaitResult::Failed;
};

// 부분적인 thread 생성 실패나 REQUIRE 조기 반환에서도 context보다 worker를 먼저 정리한다.
// 경합 시작은 ready semaphore와 launch event로 제어하고 시간 추측이나 Sleep에 의존하지 않는다.
class ControlFixture {
public:
    ~ControlFixture() noexcept {
        ReleaseAndJoin();
        CompleteAndJoin(target);
    }

    bool StartTarget() noexcept {
        if (target.Start(BlockTarget, this) != ThreadStatus::Success) {
            return false;
        }
        return entered_.Wait(10000) == WaitResult::Signaled;
    }

    bool AddController(bool joining) noexcept {
        if (count_ == 6) {
            return false;
        }
        actions[count_].fixture = this;
        actions[count_].joining = joining;
        if (controllers_[count_].Start(RunController, &actions[count_]) != ThreadStatus::Success) {
            return false;
        }
        ++count_;
        return true;
    }

    bool WaitUntilReady() noexcept {
        for (usize index = 0; index < count_; ++index) {
            if (ready_.Wait(10000) != WaitResult::Signaled) {
                return false;
            }
        }
        return true;
    }

    void ReleaseAndJoin() noexcept {
        launch_.Signal();
        releaseTarget_.Signal();
        for (usize index = 0; index < count_; ++index) {
            CompleteAndJoin(controllers_[index]);
        }
    }

    Thread target;
    ControlAction actions[6];
    uint32 targetCalls = 0;

private:
    static void BlockTarget(void* raw) noexcept {
        auto& fixture = *static_cast<ControlFixture*>(raw);
        fixture.entered_.Signal();
        if (fixture.releaseTarget_.Wait(10000) != WaitResult::Signaled) {
            Fatal("Concurrent thread control target was not released");
        }
        ++fixture.targetCalls;
    }

    static void RunController(void* raw) noexcept {
        auto& action = *static_cast<ControlAction*>(raw);
        auto& fixture = *action.fixture;
        if (!fixture.ready_.Release() || fixture.launch_.Wait(10000) != WaitResult::Signaled) {
            Fatal("Concurrent thread controller did not reach its start gate");
        }
        if (action.joining) {
            action.joinResult = fixture.target.Join();
        } else {
            action.waitResult = fixture.target.WaitForCompletion(10000);
        }
    }

    Thread controllers_[6];
    usize count_ = 0;
    ManualResetEvent entered_;
    ManualResetEvent releaseTarget_;
    ManualResetEvent launch_;
    Semaphore ready_{0, 6};
};

void EmptyEntry(void*) noexcept {
}

} // namespace

TEST(PlatformConcurrentJoinersTransferNativeOwnershipExactlyOnce) {
    ControlFixture fixture;
    REQUIRE(fixture.StartTarget());
    REQUIRE(fixture.AddController(true));
    REQUIRE(fixture.AddController(true));
    REQUIRE(fixture.WaitUntilReady());
    fixture.ReleaseAndJoin();

    usize succeeded = 0;
    for (usize index = 0; index < 2; ++index) {
        const ThreadStatus status = fixture.actions[index].joinResult;
        if (status == ThreadStatus::Success) {
            ++succeeded;
        } else {
            // reservation이 겹치면 Busy, 승자의 회수가 먼저 끝났다면 NotJoinable이다.
            CHECK(status == ThreadStatus::Busy || status == ThreadStatus::NotJoinable);
        }
    }
    CHECK(succeeded == 1);
    CHECK(fixture.targetCalls == 1);
    CHECK(!fixture.target.IsJoinable());
    CHECK(fixture.target.Join() == ThreadStatus::NotJoinable);
}

TEST(PlatformConcurrentCompletionReadersAndJoinLeaveNoPreviousRunOwnership) {
    ControlFixture fixture;
    REQUIRE(fixture.StartTarget());
    REQUIRE(fixture.AddController(true));
    for (usize index = 0; index < 5; ++index) {
        REQUIRE(fixture.AddController(false));
    }
    REQUIRE(fixture.WaitUntilReady());
    fixture.ReleaseAndJoin();

    CHECK(fixture.actions[0].joinResult == ThreadStatus::Success);
    for (usize index = 1; index < 6; ++index) {
        // Join 전에 admission한 reader는 Signaled다. Join이 먼저 완료되면 비joinable이므로
        // Failed도 정상이다. 특정 OS 스케줄이나 admission 순서를 테스트가 가정하지 않는다.
        CHECK(fixture.actions[index].waitResult == WaitResult::Signaled ||
              fixture.actions[index].waitResult == WaitResult::Failed);
    }
    CHECK(fixture.targetCalls == 1);
    CHECK(!fixture.target.IsJoinable());
    CHECK(fixture.target.WaitForCompletion(0) == WaitResult::Failed);
    // 모든 제어 호출이 반환된 뒤 이전 waiters 계수나 joining reservation이 남지 않아야 한다.
    REQUIRE(fixture.target.Start(EmptyEntry) == ThreadStatus::Success);
    CompleteAndJoin(fixture.target);
    CHECK(!fixture.target.IsJoinable());
}

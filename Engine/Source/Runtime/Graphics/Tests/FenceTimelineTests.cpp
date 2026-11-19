#include <Core/Memory/SystemAllocator.h>
#include <Graphics/FenceTimeline.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace fence_timeline_tests_file {

using namespace rocks::graphics;

namespace {

void JoinTestThread(platform::Thread& thread) noexcept {
    if (thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("FenceTimeline test thread failed to join");
    }
}

// 다른 스레드의 무한 대기다. 시작을 알린 뒤 기다린다.
struct BlockedWaiter {
    FenceTimeline* timeline = nullptr;
    GpuFencePoint point;
    platform::ManualResetEvent started;
    ErrorCode error = ErrorCode::None;
    bool signaled = false;

    static void Run(void* raw) noexcept {
        auto* self = static_cast<BlockedWaiter*>(raw);
        self->started.Signal();
        auto waited = self->timeline->Wait(self->point);
        if (waited) {
            self->signaled = waited.Value() == platform::WaitResult::Signaled;
        } else {
            self->error = waited.Error();
        }
    }
};

// 통지 결과 기록이다.
struct NotifyLog {
    ErrorCode results[8] = {};
    uint32 count = 0;

    static void Record(ErrorCode result, void* context) noexcept {
        auto* self = static_cast<NotifyLog*>(context);
        if (self->count < 8) {
            self->results[self->count++] = result;
        }
    }
};

// 시험이 제출 하나씩 완료를 허락하는 관찰 source다. Allow(value)까지만 끝난 것으로 본다.
class GatedSource final : public FenceWaitSource {
public:
    bool WaitSubmission(uint64 value, void* token) noexcept override {
        (void)token;
        platform::LockGuard lock(gate_);
        while (allowed_ < value && !lost_) {
            changed_.Wait(gate_);
        }

        return !lost_;
    }

    void ReleaseToken(void* token) noexcept override {
        if (token != nullptr) {
            platform::AtomicFetchAdd(&released, uint32{1});
        }
    }

    void Allow(uint64 value) noexcept {
        platform::LockGuard lock(gate_);
        allowed_ = value;
        changed_.NotifyAll();
    }

    void Lose() noexcept {
        platform::LockGuard lock(gate_);
        lost_ = true;
        changed_.NotifyAll();
    }

    uint32 released = 0;

private:
    platform::Mutex gate_;
    platform::ConditionVariable changed_;
    uint64 allowed_ = 0;
    bool lost_ = false;
};

} // namespace

TEST(FenceTimeline_PointsCompletionAndContext) {
    SystemAllocator allocator;
    FenceTimeline timeline(allocator, 7, QueueKind::Graphics);
    CHECK(timeline.LastSubmitted().IsNull());
    CHECK(timeline.Point(0).IsNull());

    timeline.Submitted(1, nullptr);
    timeline.Submitted(2, nullptr);
    const GpuFencePoint first = timeline.Point(1);
    const GpuFencePoint second = timeline.Point(2);
    CHECK(first.deviceEpoch == 7 && first.value == 1);
    CHECK(timeline.LastSubmitted() == second);

    auto pending = timeline.IsComplete(second);
    REQUIRE(pending);
    CHECK(!pending.Value());
    CHECK(timeline.IsComplete(GpuFencePoint{}).Value());

    GpuFencePoint foreign = second;
    foreign.deviceEpoch = 8;
    CHECK(timeline.IsComplete(foreign).Error() == ErrorCode::WrongContext);
    CHECK(timeline.IsComplete(timeline.Point(3)).Error() == ErrorCode::InvalidArgument);

    // 뒤 제출의 완료는 앞 제출의 완료다. 작은 값 게시는 무시한다.
    timeline.Publish(2);
    timeline.Publish(1);
    CHECK(timeline.Completed() == 2);
    CHECK(timeline.IsComplete(first).Value());
    auto immediate = timeline.Wait(second, 0);
    CHECK(immediate && immediate.Value() == platform::WaitResult::Signaled);

    timeline.Submitted(3, nullptr);
    auto timedOut = timeline.Wait(timeline.Point(3), 0);
    CHECK(timedOut && timedOut.Value() == platform::WaitResult::TimedOut);
}

TEST(FenceTimeline_WaitFromAnotherThreadIsReleasedByPublish) {
    SystemAllocator allocator;
    FenceTimeline timeline(allocator, 1, QueueKind::Graphics);
    timeline.Submitted(1, nullptr);
    BlockedWaiter waiter;
    waiter.timeline = &timeline;
    waiter.point = timeline.Point(1);
    platform::Thread thread;
    REQUIRE(thread.Start(&BlockedWaiter::Run, &waiter) == platform::ThreadStatus::Success);
    REQUIRE(waiter.started.Wait() == platform::WaitResult::Signaled);
    timeline.Publish(1);
    JoinTestThread(thread);
    CHECK(waiter.signaled);
}

TEST(FenceTimeline_DeviceLossWakesWaitersWithDestroyed) {
    SystemAllocator allocator;
    FenceTimeline timeline(allocator, 1, QueueKind::Graphics);
    timeline.Submitted(1, nullptr);
    timeline.Submitted(2, nullptr);
    timeline.Publish(1);
    BlockedWaiter waiter;
    waiter.timeline = &timeline;
    waiter.point = timeline.Point(2);
    NotifyLog log;
    REQUIRE(timeline.Notify(timeline.Point(2), &NotifyLog::Record, &log));
    platform::Thread thread;
    REQUIRE(thread.Start(&BlockedWaiter::Run, &waiter) == platform::ThreadStatus::Success);
    REQUIRE(waiter.started.Wait() == platform::WaitResult::Signaled);
    timeline.MarkLost();
    JoinTestThread(thread);
    CHECK(!waiter.signaled && waiter.error == ErrorCode::Destroyed);
    CHECK(log.count == 1 && log.results[0] == ErrorCode::Destroyed);
    CHECK(timeline.IsLost());
    // 손실 전에 끝난 지점은 여전히 완료다. 이후 통지는 바로 Destroyed다.
    CHECK(timeline.IsComplete(timeline.Point(1)).Value());
    CHECK(timeline.IsComplete(timeline.Point(2)).Error() == ErrorCode::Destroyed);
    REQUIRE(timeline.Notify(timeline.Point(2), &NotifyLog::Record, &log));
    CHECK(log.count == 2 && log.results[1] == ErrorCode::Destroyed);
}

TEST(FenceTimeline_NotifyRunsOnceWhenDecided) {
    SystemAllocator allocator;
    FenceTimeline timeline(allocator, 1, QueueKind::Graphics);
    timeline.Submitted(1, nullptr);
    timeline.Submitted(2, nullptr);
    NotifyLog log;
    REQUIRE(timeline.Notify(timeline.Point(2), &NotifyLog::Record, &log));
    REQUIRE(timeline.Notify(timeline.Point(1), &NotifyLog::Record, &log));
    CHECK(timeline.PendingNotifications() == 2);
    CHECK(timeline.Notify(timeline.Point(1), nullptr, &log).Error() == ErrorCode::InvalidArgument);

    timeline.Publish(1);
    CHECK(log.count == 1 && log.results[0] == ErrorCode::None);
    timeline.Publish(2);
    CHECK(log.count == 2 && log.results[1] == ErrorCode::None);
    CHECK(timeline.PendingNotifications() == 0);

    // 이미 끝난 지점은 부른 스레드에서 바로 통지한다.
    REQUIRE(timeline.Notify(timeline.Point(1), &NotifyLog::Record, &log));
    CHECK(log.count == 3);

    // 닫히면 남은 통지는 ShuttingDown이고 새 등록은 거부한다.
    timeline.Submitted(3, nullptr);
    REQUIRE(timeline.Notify(timeline.Point(3), &NotifyLog::Record, &log));
    timeline.Close();
    CHECK(log.count == 4 && log.results[3] == ErrorCode::ShuttingDown);
    CHECK(timeline.Notify(timeline.Point(3), &NotifyLog::Record, &log).Error() ==
          ErrorCode::ShuttingDown);
}

TEST(FenceTimeline_WatcherPublishesSubmissionsInOrder) {
    SystemAllocator allocator;
    GatedSource source;
    FenceTimeline timeline(allocator, 1, QueueKind::Graphics);
    REQUIRE(timeline.StartWatcher(source, "FenceTimelineTest"));
    CHECK(timeline.StartWatcher(source, "FenceTimelineTest").Error() == ErrorCode::Busy);
    int tokens[3] = {};
    timeline.Submitted(1, &tokens[0]);
    timeline.Submitted(2, &tokens[1]);
    timeline.Submitted(3, &tokens[2]);

    // 관찰 스레드는 source가 허락한 제출까지만 게시한다.
    source.Allow(2);
    auto second = timeline.Wait(timeline.Point(2));
    CHECK(second && second.Value() == platform::WaitResult::Signaled);
    CHECK(!timeline.IsComplete(timeline.Point(3)).Value());

    source.Allow(3);
    auto third = timeline.Wait(timeline.Point(3));
    CHECK(third && third.Value() == platform::WaitResult::Signaled);
    timeline.StopWatcher();
    CHECK(source.released == 3);
}

TEST(FenceTimeline_WatcherCoalescesWhenTheQueueIsFull) {
    SystemAllocator allocator;
    GatedSource source;
    FenceTimeline timeline(allocator, 1, QueueKind::Graphics);
    REQUIRE(timeline.StartWatcher(source, "FenceTimelineTest"));
    // 관찰 목록(64칸)을 넘게 넣는다. 넘친 제출은 가장 최근 항목을 대신하고 그 token을 놓는다.
    constexpr uint32 Submissions = 80;
    int tokens[Submissions] = {};
    for (uint32 index = 0; index < Submissions; ++index) {
        timeline.Submitted(index + 1, &tokens[index]);
    }

    source.Allow(Submissions);
    auto last = timeline.Wait(timeline.Point(Submissions));
    CHECK(last && last.Value() == platform::WaitResult::Signaled);
    timeline.StopWatcher();
    CHECK(source.released == Submissions);
}

TEST(FenceTimeline_WatcherReportsLoss) {
    SystemAllocator allocator;
    GatedSource source;
    FenceTimeline timeline(allocator, 1, QueueKind::Graphics);
    REQUIRE(timeline.StartWatcher(source, "FenceTimelineTest"));
    timeline.Submitted(1, nullptr);
    BlockedWaiter waiter;
    waiter.timeline = &timeline;
    waiter.point = timeline.Point(1);
    platform::Thread thread;
    REQUIRE(thread.Start(&BlockedWaiter::Run, &waiter) == platform::ThreadStatus::Success);
    REQUIRE(waiter.started.Wait() == platform::WaitResult::Signaled);
    source.Lose();
    JoinTestThread(thread);
    CHECK(waiter.error == ErrorCode::Destroyed);
    timeline.StopWatcher();
    CHECK(timeline.IsLost());
}
} // namespace fence_timeline_tests_file

#include <Platform/Clock.h>
#include <UiFramework/Application/UiTimer.h>
#include <UiFramework/Application/Application.h>

namespace rocks::ui {

Result<SharedPtr<UiTimer>> Application::StartTimer(const SharedPtr<Element>& owner, double interval,
                                                   Delegate<void()>&& callback) noexcept {
    if (!owner || !callback || !math::IsFinite(interval) || interval <= 0) {
        return Result<SharedPtr<UiTimer>>::Failure(ErrorCode::InvalidArgument);
    }

    auto timer =
        SharedPtr<UiTimer>::TryMake(allocator_, owner, interval, Move(callback), TimerTime());
    if (!timer) {
        return timer;
    }

    auto added = timers_.TryPushBack(WeakPtr<UiTimer>(timer.Value()));
    return added ? Move(timer) : Result<SharedPtr<UiTimer>>::Failure(added.Error());
}

Result<SharedPtr<UiTimer>> Application::StartFrameTimer(const SharedPtr<Element>& owner,
                                                        Delegate<void(double)>&& frame) noexcept {
    if (!owner || !frame) {
        return Result<SharedPtr<UiTimer>>::Failure(ErrorCode::InvalidArgument);
    }

    auto timer = SharedPtr<UiTimer>::TryMake(allocator_, owner, Move(frame), TimerTime());
    if (!timer) {
        return timer;
    }

    auto stored = timers_.TryPushBack(WeakPtr<UiTimer>(timer.Value()));
    return stored ? Move(timer) : Result<SharedPtr<UiTimer>>::Failure(stored.Error());
}

void Application::AdvanceTimers() noexcept {
    if (runningTimers_ || timers_.IsEmpty() || !root_) {
        return;
    }

    runningTimers_ = true;
    const double now = TimerTime();
    usize limit = timers_.Size();
    for (usize i = 0; i < limit;) {
        auto timer = timers_[i].Lock();
        if (!timer) {
            (void)timers_.TryRemoveAt(i);
            --limit;
            continue;
        }

        if (root_) {
            timer->Advance(now, *root_);
        }

        ++i;
    }

    runningTimers_ = false;
}

UiTimer::UiTimer(const SharedPtr<Element>& owner, double interval, Delegate<void()>&& callback,
                 double start) noexcept
    : owner_(owner), interval_(interval), due_(start + interval), callback_(Move(callback)) {
}

UiTimer::UiTimer(const SharedPtr<Element>& owner, Delegate<void(double)>&& frame,
                 double start) noexcept
    : owner_(owner), interval_(0), due_(start), frame_(Move(frame)) {
}

void Application::SetTimerClock(double (*clock)() noexcept) noexcept {
    timerClock_ = clock;
}

double Application::TimerTime() const noexcept {
    return timerClock_ ? timerClock_()
                       : static_cast<double>(platform::MonotonicNanoseconds()) * 1.e-9;
}

void UiTimer::Advance(double now, const Element& root) noexcept {
    if (frame_) {
        const double elapsed = math::Max(0.0, now - due_);
        due_ = now;
        // 입력이 비활성화된 화면도 페이드아웃을 마칠 수 있다.
        auto owner = owner_.Lock();
        if (owner && owner->IsWithin(root)) {
            frame_.ExecuteIfBound(elapsed);
        }

        return;
    }

    if (now < due_) {
        return;
    }

    due_ = now + interval_;
    auto owner = owner_.Lock();
    if (owner && owner->IsInputPathActive(root, false)) {
        callback_.ExecuteIfBound();
    }
}

} // namespace rocks::ui

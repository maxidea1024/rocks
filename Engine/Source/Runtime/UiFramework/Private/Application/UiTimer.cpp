#include <UiCore/TimeUnits.h>
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

double Application::SecondsUntilNextTimer() const noexcept {
    double wait = SecondsUntilNextToolTip();
    if (timers_.IsEmpty()) {
        return wait;
    }

    // 대기 시간은 지금 시계로 잰다(20.3절).
    const double now = ReadClock();
    for (const auto& weak : timers_) {
        auto timer = weak.Lock();
        if (!timer) {
            continue;
        }

        // 프레임 타이머는 매 프레임 돈다(애니메이션).
        if (timer->frame_.IsBound()) {
            return 0;
        }

        wait = math::Min(wait, math::Max(0.0, timer->due_ - now));
    }

    return wait;
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

double Application::TimerTime() const noexcept {
    // 등록·진행 모두 프레임 시각이다(UE RegisterActiveTimer가 GetCurrentTime — SWidget.cpp 1898행).
    return frameTime_.current;
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

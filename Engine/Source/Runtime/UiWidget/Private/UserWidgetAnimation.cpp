#include "UserWidgetAnimationSet.h"
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

namespace {

float& Component(WidgetTransform& transform, float& opacity,
                 WidgetAnimationChannel channel) noexcept {
    switch (channel) {
    case WidgetAnimationChannel::TranslationX:
        return transform.translation.x;
    case WidgetAnimationChannel::TranslationY:
        return transform.translation.y;
    case WidgetAnimationChannel::ScaleX:
        return transform.scale.x;
    case WidgetAnimationChannel::ScaleY:
        return transform.scale.y;
    case WidgetAnimationChannel::ShearX:
        return transform.shear.x;
    case WidgetAnimationChannel::ShearY:
        return transform.shear.y;
    case WidgetAnimationChannel::Angle:
        return transform.angle;
    case WidgetAnimationChannel::Opacity:
        return opacity;
    }
    return opacity;
}

} // namespace

UserWidgetAnimation::UserWidgetAnimation(Allocator& allocator) noexcept
    : targets_(allocator), channels_(allocator) {
}

Result<void> UserWidgetAnimation::Bind(ObjectRegistry& objects, Application& app, UserWidget& owner,
                                       const WidgetAnimation& clip,
                                       WidgetAnimationPlayback&& options,
                                       const SharedPtr<UserWidgetAnimation>& self) noexcept {
    if (clip.Tracks().IsEmpty() || !math::IsFinite(clip.Duration()) || clip.Duration() <= 0 ||
        !math::IsFinite(options.startAt) || options.startAt < 0 ||
        options.startAt > clip.Duration() || !math::IsFinite(options.speed) || options.speed <= 0 ||
        options.mode > WidgetAnimationMode::PingPong) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const auto access = objects.OwnerAccess();
    auto tree = objects.Resolve<WidgetTree>(access, owner.Tree());
    if (!tree) {
        return Result<void>::Failure(tree.Error());
    }
    auto index = tree.Value().Get().Value()->IndexWidgets(objects);
    if (!index) {
        return Result<void>::Failure(index.Error());
    }
    for (const auto& track : clip.Tracks()) {
        if (track.channel > static_cast<uint8>(WidgetAnimationChannel::Angle) ||
            !track.curve.KeyCount()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        ObjectHandle handle = owner.Handle();
        if (track.target != Guid{}) {
            const auto* target = index.Value().Find(track.target);
            if (!target) {
                return Result<void>::Failure(ErrorCode::NotFound);
            }
            handle = *target;
        }
        usize targetIndex = 0;
        while (targetIndex < targets_.Size() && targets_[targetIndex].handle != handle) {
            ++targetIndex;
        }
        if (targetIndex == targets_.Size()) {
            auto widget = objects.Resolve<Widget>(access, handle);
            if (!widget) {
                return Result<void>::Failure(widget.Error());
            }
            Target target;
            target.handle = handle;
            auto added = targets_.TryPushBack(target);
            if (!added) {
                return added;
            }
        }
        auto& target = targets_[targetIndex];
        const uint8 flag = static_cast<uint8>(1u << track.channel);
        if ((target.mask & flag) != 0) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
        target.mask |= flag;
        auto added = channels_.TryEmplaceBack(channels_.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }
        added.Value()->target = targetIndex;
        added.Value()->property = static_cast<WidgetAnimationChannel>(track.channel);
        auto copied = added.Value()->curve.TryCopyFrom(track.curve);
        if (!copied) {
            return copied;
        }
    }
    auto context = app.WeakContext();
    if (!context) {
        return Result<void>::Failure(context.Error());
    }
    objects_ = &objects;
    app_ = Move(context.Value());
    root_ = WeakPtr<Element>(owner.CachedElement());
    self_ = WeakPtr<UserWidgetAnimation>(self);
    options_ = Move(options);
    duration_ = clip.Duration();
    reversed_ = options_.mode == WidgetAnimationMode::Reverse;
    time_ = reversed_ ? duration_ - options_.startAt : options_.startAt;
    remainingLegs_ = static_cast<uint64>(options_.loops) *
                     (options_.mode == WidgetAnimationMode::PingPong ? 2u : 1u);
    return Result<void>::Success();
}

Result<void> UserWidgetAnimation::Schedule() noexcept {
    auto root = root_.Lock();
    auto self = self_.Lock();
    auto app = app_.Lock();
    if (!root || !self || !app || !*app) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    auto timer = (*app)->StartFrameTimer(
        root, Delegate<void(double)>::CreateWeak(self, &UserWidgetAnimation::OnFrame));
    if (!timer) {
        return Result<void>::Failure(timer.Error());
    }
    timer_ = Move(timer.Value());
    return Result<void>::Success();
}

Result<void> UserWidgetAnimation::Apply(bool restore) noexcept {
    applying_ = true;
    auto result = Sample(restore);
    applying_ = false;
    if (stopRequested_ && status_ != WidgetAnimationStatus::Ended) {
        stopRequested_ = false;
        End(WidgetAnimationEnd::Stopped);
    }
    return result;
}

Result<void> UserWidgetAnimation::Sample(bool restore) noexcept {
    const auto access = objects_->OwnerAccess();
    // 한 프레임의 모든 채널을 먼저 평가·검증하고 위젯마다 한 번에 적용한다.
    for (auto& target : targets_) {
        auto widget = objects_->Resolve<Widget>(access, target.handle);
        if (!widget) {
            return Result<void>::Failure(widget.Error());
        }
        auto* value = widget.Value().Get().Value();
        target.next = value->GetRenderTransform();
        target.nextOpacity = value->GetRenderOpacity();
        if (!applied_) {
            target.original = target.next;
            target.originalOpacity = target.nextOpacity;
        }
    }
    for (const auto& channel : channels_) {
        auto& target = targets_[channel.target];
        const float value =
            restore ? Component(target.original, target.originalOpacity, channel.property)
                    : channel.curve.Evaluate(static_cast<float>(time_));
        if (!math::IsFinite(value)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        Component(target.next, target.nextOpacity, channel.property) =
            channel.property == WidgetAnimationChannel::Opacity ? math::Clamp(value, 0.0f, 1.0f)
                                                                : value;
    }
    for (const auto& target : targets_) {
        auto affine = target.next.ToAffine();
        if (!affine) {
            return Result<void>::Failure(affine.Error());
        }
    }
    applied_ = true;
    for (const auto& target : targets_) {
        auto widget = objects_->ResolveForWrite<Widget>(access, target.handle);
        if (!widget) {
            return Result<void>::Failure(widget.Error());
        }
        auto* value = widget.Value().Get().Value();
        if ((target.mask & ~1u) != 0) {
            auto applied = value->SetRenderTransform(target.next);
            if (!applied) {
                return applied;
            }
        }
        if (target.mask & 1u) {
            auto applied = value->SetRenderOpacity(target.nextOpacity);
            if (!applied) {
                return applied;
            }
        }
    }
    return Result<void>::Success();
}

void UserWidgetAnimation::Begin() noexcept {
    started_ = true;
    status_ = WidgetAnimationStatus::Playing;
    auto callback = Move(options_.onStarted);
    callback.ExecuteIfBound();
}

void UserWidgetAnimation::End(WidgetAnimationEnd reason) noexcept {
    if (status_ == WidgetAnimationStatus::Ended) {
        return;
    }
    status_ = WidgetAnimationStatus::Ended;
    timer_.Reset();
    if (applied_ && options_.restoreState && !Apply(true)) {
        reason = WidgetAnimationEnd::Failed;
    }
    auto keepAlive = self_.Lock();
    (void)keepAlive;
    if (auto set = set_.Lock()) {
        set->Remove(this);
    }
    objects_ = nullptr;
    app_.Reset();
    channels_.Reset();
    targets_.Reset();
    auto callback = Move(options_.onEnded);
    callback.ExecuteIfBound(reason);
}

void UserWidgetAnimation::Stop() noexcept {
    if (applying_) {
        stopRequested_ = true;
        return;
    }
    End(WidgetAnimationEnd::Stopped);
}
WidgetAnimationStatus UserWidgetAnimation::Status() const noexcept {
    return status_;
}
float UserWidgetAnimation::CurrentTime() const noexcept {
    return static_cast<float>(time_);
}
bool UserWidgetAnimation::IsReversed() const noexcept {
    return reversed_;
}

Result<void> UserWidgetAnimation::Pause() noexcept {
    if (applying_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (status_ == WidgetAnimationStatus::Ended) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    timer_.Reset();
    status_ = WidgetAnimationStatus::Paused;
    return Result<void>::Success();
}

Result<void> UserWidgetAnimation::Resume() noexcept {
    if (applying_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (status_ == WidgetAnimationStatus::Ended) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    if (status_ == WidgetAnimationStatus::Paused) {
        auto ready = Schedule();
        if (!ready) {
            return ready;
        }
        status_ = started_ ? WidgetAnimationStatus::Playing : WidgetAnimationStatus::Queued;
    }
    return Result<void>::Success();
}

Result<void> UserWidgetAnimation::Reverse() noexcept {
    if (applying_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (status_ == WidgetAnimationStatus::Ended) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    reversed_ = !reversed_;
    return Result<void>::Success();
}

Result<void> UserWidgetAnimation::SetSpeed(float speed) noexcept {
    if (applying_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (!math::IsFinite(speed) || speed <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (status_ == WidgetAnimationStatus::Ended) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    options_.speed = speed;
    return Result<void>::Success();
}

Result<void> UserWidgetAnimation::Seek(float seconds) noexcept {
    if (applying_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (status_ == WidgetAnimationStatus::Ended) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    if (!math::IsFinite(seconds) || seconds < 0 || seconds > duration_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const double previous = time_;
    time_ = seconds;
    if (started_) {
        auto applied = Apply(false);
        if (!applied) {
            time_ = previous;
            return applied;
        }
    }
    return Result<void>::Success();
}

void UserWidgetAnimation::OnFrame(double elapsed) noexcept {
    if (!Advance(elapsed)) {
        End(WidgetAnimationEnd::Failed);
    }
}

Result<void> UserWidgetAnimation::Advance(double delta) noexcept {
    if (applying_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (!math::IsFinite(delta) || delta < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (status_ == WidgetAnimationStatus::Paused || status_ == WidgetAnimationStatus::Ended) {
        return Result<void>::Success();
    }
    if (status_ == WidgetAnimationStatus::Queued) {
        auto ready = Apply(false);
        if (!ready) {
            return ready;
        }
        if (status_ != WidgetAnimationStatus::Ended) {
            Begin();
        }
        return Result<void>::Success();
    }
    if (delta == 0) {
        return Result<void>::Success();
    }
    const double travel = delta * options_.speed;
    if (!math::IsFinite(travel)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    const double distance = reversed_ ? time_ : duration_ - time_;
    bool finished = false;
    if (travel < distance) {
        time_ += reversed_ ? -travel : travel;
    } else {
        // 지연된 한 프레임에 여러 주기가 들어와도 반복 횟수만큼 루프를 돌지 않는다.
        const double remaining = travel - distance;
        const double legsAfter = math::Floor(remaining / duration_);
        if (options_.loops != 0 && legsAfter >= static_cast<double>(remainingLegs_ - 1)) {
            if (options_.mode == WidgetAnimationMode::PingPong && (remainingLegs_ % 2 == 0)) {
                reversed_ = !reversed_;
            }
            time_ = reversed_ ? 0 : duration_;
            finished = true;
        } else {
            if (options_.loops != 0) {
                remainingLegs_ -= static_cast<uint64>(legsAfter) + 1;
            }
            const double inPair = math::Fmod(remaining, 2 * duration_);
            if (options_.mode == WidgetAnimationMode::PingPong && inPair < duration_) {
                reversed_ = !reversed_;
            }
            const double phase = math::Fmod(remaining, duration_);
            time_ = reversed_ ? duration_ - phase : phase;
        }
    }
    auto ready = Apply(false);
    if (!ready) {
        End(WidgetAnimationEnd::Failed);
        return ready;
    }
    if (finished) {
        End(WidgetAnimationEnd::Finished);
    }
    return Result<void>::Success();
}

} // namespace rocks::ui

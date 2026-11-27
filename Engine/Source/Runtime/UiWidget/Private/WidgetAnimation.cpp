#include <UiWidget/WidgetAnimation.h>

namespace rocks::ui {

WidgetAnimation::WidgetAnimation(Allocator& allocator) noexcept : tracks_(allocator) {
}

Result<void> WidgetAnimation::SetDuration(float seconds) noexcept {
    if (!math::IsFinite(seconds) || seconds <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    duration_ = seconds;
    return Result<void>::Success();
}

float WidgetAnimation::Duration() const noexcept {
    return duration_;
}

Span<const WidgetAnimationTrack> WidgetAnimation::Tracks() const noexcept {
    return tracks_.View();
}

Result<void> WidgetAnimation::SetTrack(Guid target, WidgetAnimationChannel channel,
                                       const TangentCurve& curve) noexcept {
    if (channel > WidgetAnimationChannel::Angle || curve.KeyCount() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const auto id = static_cast<uint8>(channel);
    for (auto& track : tracks_) {
        if (track.target == target && track.channel == id) {
            return track.curve.TryCopyFrom(curve);
        }
    }

    WidgetAnimationTrack track(tracks_.GetAllocator());
    track.target = target;
    track.channel = id;
    auto copied = track.curve.TryCopyFrom(curve);
    if (!copied) {
        return copied;
    }

    return tracks_.TryPushBack(Move(track));
}

} // namespace rocks::ui

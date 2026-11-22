#include <Audio/GainRamp.h>

#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>

namespace rocks::audio {

void GainRamp::SetTarget(float target, uint32 rampFrames) noexcept {
    // 유한하지 않은 이득은 출력 전체를 오염시키므로 호출자 계약 위반이다.
    ROCKS_ASSERT(math::IsFinite(target));
    if (rampFrames == 0) {
        Reset(target);
        return;
    }

    start_ = Current();
    target_ = target;
    total_ = rampFrames;
    done_ = 0;
}

void GainRamp::Reset(float gain) noexcept {
    ROCKS_ASSERT(math::IsFinite(gain));
    start_ = gain;
    target_ = gain;
    total_ = 0;
    done_ = 0;
}

float GainRamp::GainAt(uint32 step) const noexcept {
    if (step >= total_) {
        return target_;
    }

    // step/total을 double로 계산해 긴 ramp(수십만 frame)에서도 비율이 정확하다.
    const double fraction = static_cast<double>(step) / static_cast<double>(total_);
    return static_cast<float>(static_cast<double>(start_) +
                              (static_cast<double>(target_) - static_cast<double>(start_)) *
                                  fraction);
}

float GainRamp::Current() const noexcept {
    return GainAt(done_);
}

void GainRamp::Apply(const PlanarView& view) noexcept {
    const uint32 frames = view.frameCount;
    uint32 frame = 0;
    // ramp 구간: frame마다 식으로 이득을 구한다.
    for (; frame < frames && done_ < total_; ++frame, ++done_) {
        const float gain = GainAt(done_);
        for (uint32 channel = 0; channel < view.channelCount; ++channel) {
            view.channels[channel][frame] *= gain;
        }
    }

    if (frame == frames) {
        return;
    }

    // ramp가 끝난 구간은 목표 이득 하나다. 1이면 곱하지 않는다(출력이 같다).
    if (target_ == 1.0f) {
        return;
    }

    for (uint32 channel = 0; channel < view.channelCount; ++channel) {
        float* samples = view.channels[channel];
        for (uint32 index = frame; index < frames; ++index) {
            samples[index] *= target_;
        }
    }
}

void GainRamp::MixAdd(const float* source, float* destination, uint32 frames) noexcept {
    uint32 frame = 0;
    for (; frame < frames && done_ < total_; ++frame, ++done_) {
        destination[frame] += source[frame] * GainAt(done_);
    }

    for (; frame < frames; ++frame) {
        destination[frame] += source[frame] * target_;
    }
}

void GainRamp::Advance(uint32 frames) noexcept {
    const uint32 remaining = total_ - done_;
    done_ += frames < remaining ? frames : remaining;
}

} // namespace rocks::audio

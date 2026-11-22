#include <Audio/OnePoleFilter.h>

#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

namespace rocks::audio {

using platform::PlatformMath;

bool OnePoleFilterState::IsValidSetup(uint32 sampleRate, uint32 channels) noexcept {
    return IsValidMixSampleRate(sampleRate) && channels != 0 && channels <= MaxOutputChannels;
}

void OnePoleFilterState::Initialize(uint32 sampleRate, uint32 channels) noexcept {
    sampleRate_ = sampleRate;
    channelCount_ = channels;
}

void OnePoleFilterState::ResetState() noexcept {
    for (float& state : z_) {
        state = 0.0f;
    }
}

float OnePoleFilterState::CoefficientAt(uint32 step) const noexcept {
    if (step >= total_) {
        return total_ == 0 ? start_ : target_;
    }

    const double fraction = static_cast<double>(step) / static_cast<double>(total_);
    return static_cast<float>(static_cast<double>(start_) +
                              (static_cast<double>(target_) - static_cast<double>(start_)) *
                                  fraction);
}

void OnePoleFilterState::SetCoefficient(float cutoff, float coefficient, bool bypass,
                                        uint32 interpFrames) noexcept {
    cutoff_ = cutoff;
    if (bypass) {
        bypassed_ = true;
        return;
    }

    // 첫 설정·bypass에서 다시 켜기·보간 0은 즉시 적용한다. 다시 켤 때는 상태도 0에서 시작한다.
    if (bypassed_ || interpFrames == 0) {
        if (bypassed_) {
            ResetState();
        }

        bypassed_ = false;
        start_ = coefficient;
        target_ = coefficient;
        total_ = 0;
        done_ = 0;
        return;
    }

    start_ = CoefficientAt(done_);
    target_ = coefficient;
    total_ = interpFrames;
    done_ = 0;
}

namespace {

bool IsValidCutoff(float cutoffHz) noexcept {
    return math::IsFinite(cutoffHz) && cutoffHz >= 0.0f;
}

} // namespace

Result<OnePoleLowPass> OnePoleLowPass::TryCreate(uint32 sampleRate, uint32 channels) noexcept {
    if (!IsValidSetup(sampleRate, channels)) {
        return Result<OnePoleLowPass>::Failure(ErrorCode::InvalidArgument);
    }

    OnePoleLowPass filter;
    filter.Initialize(sampleRate, channels);
    filter.cutoff_ = FilterBypassFrequency;
    return Result<OnePoleLowPass>::Success(filter);
}

float OnePoleLowPass::ComputeCoefficient(float cutoffHz, uint32 sampleRate) noexcept {
    const double normalized = math::Clamp(
        2.0 * static_cast<double>(cutoffHz) / static_cast<double>(sampleRate), 0.0, 1.0);
    return static_cast<float>(PlatformMath::Exp(-math::Pi<double> * normalized));
}

Result<void> OnePoleLowPass::SetCutoff(float cutoffHz, uint32 interpFrames) noexcept {
    if (!IsValidCutoff(cutoffHz)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool bypass = cutoffHz >= FilterBypassFrequency - FilterFrequencyTolerance;
    SetCoefficient(cutoffHz, bypass ? 0.0f : ComputeCoefficient(cutoffHz, sampleRate_), bypass,
                   interpFrames);
    return Result<void>::Success();
}

void OnePoleLowPass::Process(const PlanarView& view) noexcept {
    ROCKS_ASSERT(view.channelCount == channelCount_);
    if (bypassed_) {
        return;
    }

    uint32 frame = 0;
    // 계수 보간 구간: frame마다 한 단계 나아간 계수로 모든 채널을 처리한다.
    for (; frame < view.frameCount && done_ < total_; ++frame) {
        ++done_;
        const float b1 = CoefficientAt(done_);
        for (uint32 channel = 0; channel < channelCount_; ++channel) {
            const float input = view.channels[channel][frame];
            const float output = input + b1 * (z_[channel] - input);
            z_[channel] = output;
            view.channels[channel][frame] = output;
        }
    }

    const float b1 = CoefficientAt(done_);
    for (uint32 channel = 0; channel < channelCount_; ++channel) {
        float* samples = view.channels[channel];
        float state = z_[channel];
        for (uint32 index = frame; index < view.frameCount; ++index) {
            const float input = samples[index];
            state = input + b1 * (state - input);
            samples[index] = state;
        }

        z_[channel] = state;
    }
}

Result<OnePoleHighPass> OnePoleHighPass::TryCreate(uint32 sampleRate, uint32 channels) noexcept {
    if (!IsValidSetup(sampleRate, channels)) {
        return Result<OnePoleHighPass>::Failure(ErrorCode::InvalidArgument);
    }

    OnePoleHighPass filter;
    filter.Initialize(sampleRate, channels);
    filter.cutoff_ = FilterMinFrequency;
    return Result<OnePoleHighPass>::Success(filter);
}

float OnePoleHighPass::ComputeCoefficient(float cutoffHz, uint32 sampleRate) noexcept {
    const double rate = static_cast<double>(sampleRate);
    // UE NyquistLimit처럼 fs/2 − 1 Hz로 자른다. fs/2에서는 a0 = 1이 되어 극이 −1에 놓이고 상태
    // z가 Nyquist 성분을 따라 끝없이 커진다.
    const double frequency = math::Clamp(static_cast<double>(cutoffHz), 0.0,
                                         0.5 * rate - static_cast<double>(HighPassNyquistMargin));
    const double g = PlatformMath::Tan(math::Pi<double> * frequency / rate);
    return static_cast<float>(g / (1.0 + g));
}

Result<void> OnePoleHighPass::SetCutoff(float cutoffHz, uint32 interpFrames) noexcept {
    if (!IsValidCutoff(cutoffHz)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 보관하는 차단 주파수도 UE처럼 자른 값이다.
    const float nyquistLimit = 0.5f * static_cast<float>(sampleRate_) - HighPassNyquistMargin;
    const float cutoff = math::Min(cutoffHz, nyquistLimit);
    const bool bypass = cutoff <= FilterMinFrequency + FilterFrequencyTolerance;
    SetCoefficient(cutoff, bypass ? 0.0f : ComputeCoefficient(cutoff, sampleRate_), bypass,
                   interpFrames);
    return Result<void>::Success();
}

void OnePoleHighPass::Process(const PlanarView& view) noexcept {
    ROCKS_ASSERT(view.channelCount == channelCount_);
    if (bypassed_) {
        return;
    }

    uint32 frame = 0;
    for (; frame < view.frameCount && done_ < total_; ++frame) {
        ++done_;
        const float a0 = CoefficientAt(done_);
        for (uint32 channel = 0; channel < channelCount_; ++channel) {
            const float input = view.channels[channel][frame];
            const float v = (input - z_[channel]) * a0;
            const float lowPass = v + z_[channel];
            z_[channel] = v + lowPass;
            view.channels[channel][frame] = input - lowPass;
        }
    }

    const float a0 = CoefficientAt(done_);
    for (uint32 channel = 0; channel < channelCount_; ++channel) {
        float* samples = view.channels[channel];
        float state = z_[channel];
        for (uint32 index = frame; index < view.frameCount; ++index) {
            const float input = samples[index];
            const float v = (input - state) * a0;
            const float lowPass = v + state;
            state = v + lowPass;
            samples[index] = input - lowPass;
        }

        z_[channel] = state;
    }
}

} // namespace rocks::audio

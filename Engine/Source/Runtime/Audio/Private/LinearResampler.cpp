#include <Audio/LinearResampler.h>

#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

namespace rocks::audio {

using platform::PlatformMath;

namespace {

// 32.32 고정소수점의 소수 bit 수와 1.0이다.
constexpr uint32 FractionBits = 32;
constexpr int64 FixedOne = int64{1} << FractionBits;
constexpr uint64 FractionMask = static_cast<uint64>(FixedOne) - 1;

// 소수부를 float로 바꾸는 배율(2^-32)이다.
constexpr float FractionScale = 1.0f / static_cast<float>(FixedOne);

bool IsValidRatio(double ratio) noexcept {
    return math::IsFinite(ratio) && ratio >= MinFrameRatio && ratio <= MaxFrameRatio;
}

// 범위 안 비율만 받는다. 100 × 2^32 < 2^39라 int64에 넉넉히 들어간다.
int64 ToFixed(double ratio) noexcept {
    return PlatformMath::RoundToInt64(ratio * static_cast<double>(FixedOne));
}

// 고정소수점 위치의 floor(정수 frame)다. 음수 위치도 내림이다(C++20 산술 시프트).
int64 FloorFrames(int64 position) noexcept {
    return position >> FractionBits;
}

int64 CeilFrames(int64 position) noexcept {
    return (position + FixedOne - 1) >> FractionBits;
}

// 위치 P의 출력을 만들려면 읽어야 하는 입력 frame 수(색인 0부터)다. x[−1]만 읽으면 0이다.
int64 ReadNeed(int64 position) noexcept {
    const int64 index = FloorFrames(position);
    const bool hasFraction = (static_cast<uint64>(position) & FractionMask) != 0;
    return index + (hasFraction ? 2 : 1);
}

} // namespace

Result<LinearResampler> LinearResampler::TryCreate(uint32 channels, double frameRatio) noexcept {
    if (channels == 0 || channels > MaxOutputChannels || !IsValidRatio(frameRatio)) {
        return Result<LinearResampler>::Failure(ErrorCode::InvalidArgument);
    }

    LinearResampler resampler;
    resampler.channelCount_ = channels;
    resampler.ratioStart_ = ToFixed(frameRatio);
    resampler.ratioTarget_ = resampler.ratioStart_;
    return Result<LinearResampler>::Success(resampler);
}

int64 LinearResampler::StepAt(uint32 step) const noexcept {
    if (step >= ratioTotal_) {
        return ratioTotal_ == 0 ? ratioStart_ : ratioTarget_;
    }

    // |target − start| < 2^39, step ≤ 2^20이라 곱이 2^59 아래다. 0 쪽으로 자르는 정수 나눗셈이고
    // 결정적이다.
    return ratioStart_ + (ratioTarget_ - ratioStart_) * static_cast<int64>(step) /
                             static_cast<int64>(ratioTotal_);
}

Result<void> LinearResampler::SetFrameRatio(double frameRatio, uint32 interpFrames) noexcept {
    if (!IsValidRatio(frameRatio) || interpFrames > MaxRatioInterpFrames) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const int64 target = ToFixed(frameRatio);
    if (interpFrames == 0) {
        ratioStart_ = target;
        ratioTarget_ = target;
        ratioTotal_ = 0;
        ratioDone_ = 0;
        return Result<void>::Success();
    }

    ratioStart_ = StepAt(ratioDone_);
    ratioTarget_ = target;
    ratioTotal_ = interpFrames;
    ratioDone_ = 0;
    return Result<void>::Success();
}

Result<uint32> LinearResampler::InputFramesNeeded(uint32 outputFrames) const noexcept {
    if (outputFrames > MaxBlockFrames) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    // 위치는 단조 증가하므로 마지막 출력의 읽기 요구와 끝 위치의 소비 수 중 큰 값이다.
    // 위치 상한은 MaxBlockFrames × 100 × 2^32 < 2^51이다.
    int64 position = position_;
    int64 needed = 0;
    uint32 done = ratioDone_;
    for (uint32 frame = 0; frame < outputFrames; ++frame) {
        needed = ReadNeed(position);
        if (done < ratioTotal_) {
            ++done;
        }

        position += StepAt(done);
    }

    const int64 consumed = CeilFrames(position);
    if (consumed > needed) {
        needed = consumed;
    }

    return Result<uint32>::Success(needed > 0 ? static_cast<uint32>(needed) : 0u);
}

uint32 LinearResampler::OutputFramesBefore(int64 inputFrame) const noexcept {
    // 위치 상한(2^51)보다 먼 입력은 MaxBlockFrames 안에 닿지 않는다. 시프트가 넘치지 않게 자른다.
    constexpr int64 FarFrames = int64{1} << 30;
    const int64 clamped =
        inputFrame > FarFrames ? FarFrames : (inputFrame < -FarFrames ? -FarFrames : inputFrame);
    const int64 limit = clamped * FixedOne;
    int64 position = position_;
    uint32 done = ratioDone_;
    uint32 frames = 0;
    while (frames < MaxBlockFrames && position < limit) {
        ++frames;
        if (done < ratioTotal_) {
            ++done;
        }

        position += StepAt(done);
    }

    return frames;
}

Result<uint32> LinearResampler::Process(const PlanarConstView& input,
                                        const PlanarView& output) noexcept {
    if (input.channelCount != channelCount_ || output.channelCount != channelCount_) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    const auto needed = InputFramesNeeded(output.frameCount);
    if (!needed) {
        return Result<uint32>::Failure(needed.Error());
    }

    if (input.frameCount < needed.Value()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    int64 position = position_;
    for (uint32 frame = 0; frame < output.frameCount; ++frame) {
        const int64 index = FloorFrames(position);
        const uint64 fraction = static_cast<uint64>(position) & FractionMask;
        if (fraction == 0) {
            for (uint32 channel = 0; channel < channelCount_; ++channel) {
                output.channels[channel][frame] =
                    index < 0 ? previous_[channel] : input.channels[channel][index];
            }
        } else {
            const float weight = static_cast<float>(static_cast<uint32>(fraction)) * FractionScale;
            for (uint32 channel = 0; channel < channelCount_; ++channel) {
                const float* samples = input.channels[channel];
                const float x0 = index < 0 ? previous_[channel] : samples[index];
                const float x1 = samples[index + 1];
                output.channels[channel][frame] = x0 + (x1 - x0) * weight;
            }
        }

        if (ratioDone_ < ratioTotal_) {
            ++ratioDone_;
        }

        position += StepAt(ratioDone_);
    }

    const int64 consumedFrames = CeilFrames(position);
    const uint32 consumed = consumedFrames > 0 ? static_cast<uint32>(consumedFrames) : 0u;
    if (consumed > 0) {
        for (uint32 channel = 0; channel < channelCount_; ++channel) {
            previous_[channel] = input.channels[channel][consumed - 1];
        }
    }

    position_ = position - static_cast<int64>(consumed) * FixedOne;
    return Result<uint32>::Success(consumed);
}

void LinearResampler::Reset() noexcept {
    position_ = 0;
    ratioStart_ = ratioTarget_;
    ratioTotal_ = 0;
    ratioDone_ = 0;
    for (float& value : previous_) {
        value = 0.0f;
    }
}

double LinearResampler::FrameRatio() const noexcept {
    const uint32 next = ratioDone_ < ratioTotal_ ? ratioDone_ + 1 : ratioDone_;
    return static_cast<double>(StepAt(next)) / static_cast<double>(FixedOne);
}

double LinearResampler::TargetFrameRatio() const noexcept {
    return static_cast<double>(ratioTarget_) / static_cast<double>(FixedOne);
}

double LinearResampler::Position() const noexcept {
    return static_cast<double>(position_) / static_cast<double>(FixedOne);
}

} // namespace rocks::audio

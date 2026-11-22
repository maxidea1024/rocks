#include <Audio/DynamicsProcessor.h>

#include <Audio/Decibels.h>
#include <Core/Assert.h>
#include <Core/Move.h>
#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

namespace rocks::audio {

using platform::PlatformMath;

namespace {

// RC 시상수 비율이다(UE FAttackRelease). analog는 시정수 하나(63.2%), digital은 ln 100(99%)에 닿는
// 시간이 어택·릴리스 시간이다.
constexpr double AnalogTimeConstant = 1.00239343;
constexpr double DigitalTimeConstant = 4.60517019;

// ms → s 배율이다.
constexpr double MillisecondsPerSecond = 1000.0;

// knee 분모가 이보다 작으면 이 값으로 둔다(UE SMALL_NUMBER).
constexpr float KneeDenominatorFloor = 1.0e-8f;

// 이득 dB가 이보다 크면 선형 1로 둔다(UE −SMALL_NUMBER 판정).
constexpr float UnityGainThresholdDb = -1.0e-8f;

bool IsFiniteAtLeast(float value, float minimum) noexcept {
    return math::IsFinite(value) && value >= minimum;
}

bool IsGainInRange(float decibels) noexcept {
    return math::IsFinite(decibels) && decibels >= -MaxDynamicsGainDb &&
           decibels <= MaxDynamicsGainDb;
}

bool IsValidSettings(const DynamicsSettings& settings) noexcept {
    const bool enums =
        static_cast<uint32>(settings.mode) <=
            static_cast<uint32>(DynamicsMode::UpwardsCompressor) &&
        static_cast<uint32>(settings.peakMode) <=
            static_cast<uint32>(EnvelopePeakMode::RootMeanSquared) &&
        static_cast<uint32>(settings.linkMode) <= static_cast<uint32>(DynamicsLinkMode::Peak);
    return enums && IsFiniteAtLeast(settings.lookaheadMs, 0.0f) &&
           settings.lookaheadMs <= MaxLookaheadMs && IsFiniteAtLeast(settings.attackMs, 0.0f) &&
           IsFiniteAtLeast(settings.releaseMs, 0.0f) && math::IsFinite(settings.thresholdDb) &&
           math::IsFinite(settings.ratio) && settings.ratio >= MinDynamicsRatio &&
           IsFiniteAtLeast(settings.kneeBandwidthDb, 0.0f) && IsGainInRange(settings.inputGainDb) &&
           IsGainInRange(settings.outputGainDb) && math::IsFinite(settings.analysisWindowMs) &&
           settings.analysisWindowMs > 0.0f;
}

// 지연 frame 수 ceil(ms·fs/1000)이다(UE GetNumDelayFrames).
uint32 DelayFramesFor(float milliseconds, uint32 sampleRate) noexcept {
    const double frames =
        PlatformMath::Ceil(static_cast<double>(milliseconds) * static_cast<double>(sampleRate) /
                           MillisecondsPerSecond);
    return frames > 0.0 ? static_cast<uint32>(frames) : 0u;
}

// 어택·릴리스 계수 exp(−1000·T/(ms·fs))다. 0 ms는 0(즉시)이다.
float TimeCoefficient(float milliseconds, uint32 sampleRate, bool analog) noexcept {
    if (milliseconds <= 0.0f) {
        return 0.0f;
    }

    const double timeConstant = analog ? AnalogTimeConstant : DigitalTimeConstant;
    return static_cast<float>(
        PlatformMath::Exp(-MillisecondsPerSecond * timeConstant /
                          (static_cast<double>(milliseconds) * static_cast<double>(sampleRate))));
}

float SlopeFor(DynamicsMode mode, float ratio) noexcept {
    switch (mode) {
    case DynamicsMode::Compressor:
    case DynamicsMode::UpwardsCompressor:
        return 1.0f - 1.0f / ratio;
    case DynamicsMode::Limiter:
        return 1.0f;
    case DynamicsMode::Expander:
        return 1.0f / ratio - 1.0f;
    case DynamicsMode::Gate:
        return -1.0f;
    }

    return 0.0f;
}

} // namespace

Result<DynamicsProcessor> DynamicsProcessor::TryCreate(uint32 sampleRate, uint32 channels,
                                                       const DynamicsSettings& settings,
                                                       Allocator& allocator) noexcept {
    if (!IsValidMixSampleRate(sampleRate) || channels == 0 || channels > MaxOutputChannels ||
        !IsValidSettings(settings)) {
        return Result<DynamicsProcessor>::Failure(ErrorCode::InvalidArgument);
    }

    // 192 kHz에서도 링은 19,201칸이고 채널 8개의 링 + scratch가 20만 float 아래라 곱이 넘치지
    // 않는다.
    const uint32 ringCapacity = DelayFramesFor(MaxLookaheadMs, sampleRate) + 1;
    const usize floats = static_cast<usize>(ringCapacity + MaxBlockFrames) * channels;
    auto allocated = allocator.Allocate(floats * sizeof(float), AudioBufferAlignment);
    if (!allocated) {
        return Result<DynamicsProcessor>::Failure(allocated.Error());
    }

    DynamicsProcessor processor;
    processor.allocator_ = &allocator;
    processor.memory_ = static_cast<float*>(allocated.Value());
    processor.sampleRate_ = sampleRate;
    processor.channelCount_ = channels;
    processor.ringCapacity_ = ringCapacity;
    processor.ApplySettings(settings);
    processor.Reset();
    return Result<DynamicsProcessor>::Success(Move(processor));
}

DynamicsProcessor::DynamicsProcessor(DynamicsProcessor&& other) noexcept {
    *this = Move(other);
}

DynamicsProcessor& DynamicsProcessor::operator=(DynamicsProcessor&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    Release();
    settings_ = other.settings_;
    allocator_ = other.allocator_;
    memory_ = other.memory_;
    sampleRate_ = other.sampleRate_;
    channelCount_ = other.channelCount_;
    ringCapacity_ = other.ringCapacity_;
    writeIndex_ = other.writeIndex_;
    delayFrames_ = other.delayFrames_;
    attack_ = other.attack_;
    release_ = other.release_;
    windowAlpha_ = other.windowAlpha_;
    windowBeta_ = other.windowBeta_;
    slope_ = other.slope_;
    kneeLow_ = other.kneeLow_;
    kneeHigh_ = other.kneeHigh_;
    halfKnee_ = other.halfKnee_;
    denominatorLowMinusHigh_ = other.denominatorLowMinusHigh_;
    denominatorHighMinusLow_ = other.denominatorHighMinusLow_;
    inputGain_ = other.inputGain_;
    outputGain_ = other.outputGain_;
    for (uint32 channel = 0; channel < MaxOutputChannels; ++channel) {
        envelope_[channel] = other.envelope_[channel];
        meanSquare_[channel] = other.meanSquare_[channel];
    }

    other.allocator_ = nullptr;
    other.memory_ = nullptr;
    other.channelCount_ = 0;
    return *this;
}

DynamicsProcessor::~DynamicsProcessor() noexcept {
    Release();
}

void DynamicsProcessor::Release() noexcept {
    if (memory_ != nullptr) {
        // 이 처리기가 할당한 주소를 돌려주므로 실패는 allocator 계약 위반이다.
        const Result<void> released = allocator_->Deallocate(memory_);
        ROCKS_ASSERT(released);
        (void)released;
        memory_ = nullptr;
    }
}

float* DynamicsProcessor::RingOf(uint32 channel) const noexcept {
    return memory_ + static_cast<usize>(channel) * ringCapacity_;
}

float* DynamicsProcessor::ScratchOf(uint32 channel) const noexcept {
    return memory_ + static_cast<usize>(channelCount_) * ringCapacity_ +
           static_cast<usize>(channel) * MaxBlockFrames;
}

Result<void> DynamicsProcessor::SetSettings(const DynamicsSettings& settings) noexcept {
    if (!IsValidSettings(settings)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ApplySettings(settings);
    return Result<void>::Success();
}

void DynamicsProcessor::ApplySettings(const DynamicsSettings& settings) noexcept {
    settings_ = settings;
    delayFrames_ = DelayFramesFor(settings.lookaheadMs, sampleRate_);
    attack_ = TimeCoefficient(settings.attackMs, sampleRate_, settings.analogMode);
    release_ = TimeCoefficient(settings.releaseMs, sampleRate_, settings.analogMode);
    windowAlpha_ = static_cast<float>(PlatformMath::Exp(
        -MillisecondsPerSecond /
        (static_cast<double>(sampleRate_) * static_cast<double>(settings.analysisWindowMs))));
    windowBeta_ = 1.0f - windowAlpha_;
    inputGain_ = DecibelsToLinear(settings.inputGainDb);
    outputGain_ = DecibelsToLinear(settings.outputGainDb);

    // UE CalculateSlope·CalculateKnee: knee 위 점은 0 dB를 넘지 않는다.
    slope_ = SlopeFor(settings.mode, settings.ratio);
    halfKnee_ = 0.5f * settings.kneeBandwidthDb;
    kneeLow_ = settings.thresholdDb - halfKnee_;
    kneeHigh_ = math::Min(settings.thresholdDb + halfKnee_, 0.0f);
    denominatorLowMinusHigh_ = kneeLow_ - kneeHigh_;
    if (math::Abs(denominatorLowMinusHigh_) < KneeDenominatorFloor) {
        denominatorLowMinusHigh_ = KneeDenominatorFloor;
    }

    denominatorHighMinusLow_ = kneeHigh_ - kneeLow_;
    if (math::Abs(denominatorHighMinusLow_) < KneeDenominatorFloor) {
        denominatorHighMinusLow_ = KneeDenominatorFloor;
    }
}

void DynamicsProcessor::Reset() noexcept {
    for (uint32 channel = 0; channel < MaxOutputChannels; ++channel) {
        envelope_[channel] = 0.0f;
        meanSquare_[channel] = 0.0f;
    }

    const usize ringFloats = static_cast<usize>(channelCount_) * ringCapacity_;
    for (usize index = 0; index < ringFloats; ++index) {
        memory_[index] = 0.0f;
    }

    writeIndex_ = 0;
}

float DynamicsProcessor::GainForLevelDb(float levelDb) const noexcept {
    const float threshold = settings_.thresholdDb;
    const bool upwards = settings_.mode == DynamicsMode::UpwardsCompressor;
    // UE IsInProcessingThreshold: UpwardsCompressor 조건은 UE 식 그대로다(반 knee ≥ 0이면 늘 거짓).
    const bool inKnee =
        upwards ? halfKnee_ >= 0.0f && levelDb < kneeLow_ && levelDb > threshold + halfKnee_
                : halfKnee_ >= 0.0f && levelDb > kneeLow_ && levelDb < threshold + halfKnee_;
    float slope = slope_;
    if (inKnee) {
        // knee 두 점 (kneeLow, 0)·(kneeHigh, slope)을 지나는 라그랑주 보간이다.
        slope = (levelDb - kneeHigh_) / denominatorLowMinusHigh_ * 0.0f;
        slope += (levelDb - kneeLow_) / denominatorHighMinusLow_ * slope_;
    }

    float gainDb = slope * (threshold - levelDb);
    if (upwards) {
        gainDb = math::Clamp(gainDb, 0.0f, UpwardsCompressionMaxGainDb);
        return DecibelsToLinear(gainDb);
    }

    return gainDb > UnityGainThresholdDb ? 1.0f : DecibelsToLinear(gainDb);
}

void DynamicsProcessor::Process(const PlanarView& view) noexcept {
    ROCKS_ASSERT(view.channelCount == channelCount_);
    // 기본 생성·이동된 빈 처리기는 아무것도 하지 않는다.
    if (memory_ == nullptr) {
        return;
    }

    // scratch는 MaxBlockFrames frame이라 큰 블록은 나눠 처리한다(상태가 표본 순서로만 진행해 결과가
    // 같다).
    for (uint32 offset = 0; offset < view.frameCount; offset += MaxBlockFrames) {
        const uint32 remaining = view.frameCount - offset;
        ProcessBlock(view.Slice(offset, remaining < MaxBlockFrames ? remaining : MaxBlockFrames));
    }
}

void DynamicsProcessor::ProcessBlock(const PlanarView& view) noexcept {
    const uint32 frames = view.frameCount;
    // 1. key(입력 × input gain)의 채널별 엔벌로프를 scratch에 쓴다.
    const EnvelopePeakMode peakMode = settings_.peakMode;
    for (uint32 channel = 0; channel < channelCount_; ++channel) {
        const float* input = view.channels[channel];
        float* envelopeOut = ScratchOf(channel);
        float value = envelope_[channel];
        float meanSquare = meanSquare_[channel];
        for (uint32 frame = 0; frame < frames; ++frame) {
            // 비유한 표본 하나가 엔벌로프를 영구히 NaN으로 만들지 않게 0으로 본다(UE는 거르지
            // 않는다).
            const float sample = input[frame];
            const float key = math::IsFinite(sample) ? sample * inputGain_ : 0.0f;
            float normalized = 0.0f;
            if (peakMode == EnvelopePeakMode::Peak) {
                normalized = math::Abs(key);
            } else {
                meanSquare = windowBeta_ * (key * key) + windowAlpha_ * meanSquare;
                normalized = peakMode == EnvelopePeakMode::MeanSquared
                                 ? meanSquare
                                 : PlatformMath::Sqrt(meanSquare);
            }

            const float difference = value - normalized;
            value = (value <= normalized ? attack_ : release_) * difference + normalized;
            envelopeOut[frame] = value;
        }

        envelope_[channel] = value;
        meanSquare_[channel] = meanSquare;
    }

    // 2. link: 이득 채널 수를 정하고 묶은 엔벌로프를 scratch 0에 둔다.
    uint32 gainChannels = channelCount_;
    if (settings_.linkMode != DynamicsLinkMode::Disabled && channelCount_ > 1) {
        float* linked = ScratchOf(0);
        const bool average = settings_.linkMode == DynamicsLinkMode::Average;
        for (uint32 channel = 1; channel < channelCount_; ++channel) {
            const float* other = ScratchOf(channel);
            for (uint32 frame = 0; frame < frames; ++frame) {
                linked[frame] =
                    average ? linked[frame] + other[frame] : math::Max(linked[frame], other[frame]);
            }
        }

        if (average) {
            const float inverse = 1.0f / static_cast<float>(channelCount_);
            for (uint32 frame = 0; frame < frames; ++frame) {
                linked[frame] *= inverse;
            }
        }

        gainChannels = 1;
    }

    // 3. 엔벌로프 → dB(하한 −96 dB) → 정적 곡선 이득.
    const float levelFloor = DecibelsToLinear(DynamicsLevelFloorDb);
    for (uint32 channel = 0; channel < gainChannels; ++channel) {
        float* gains = ScratchOf(channel);
        for (uint32 frame = 0; frame < frames; ++frame) {
            const float levelDb = LinearToDecibels(gains[frame], levelFloor);
            gains[frame] = GainForLevelDb(levelDb);
        }
    }

    // 4. lookahead 지연을 거친 입력에 이득·output gain·input gain을 곱한다.
    const float outputScale = outputGain_ * inputGain_;
    const uint32 readOffset = ringCapacity_ - delayFrames_;
    for (uint32 channel = 0; channel < channelCount_; ++channel) {
        float* samples = view.channels[channel];
        float* ring = RingOf(channel);
        const float* gains = ScratchOf(channel % gainChannels);
        uint32 write = writeIndex_;
        for (uint32 frame = 0; frame < frames; ++frame) {
            ring[write] = samples[frame];
            uint32 read = write + readOffset;
            if (read >= ringCapacity_) {
                read -= ringCapacity_;
            }

            samples[frame] = ring[read] * gains[frame] * outputScale;
            write = write + 1 == ringCapacity_ ? 0 : write + 1;
        }
    }

    writeIndex_ = static_cast<uint32>((static_cast<uint64>(writeIndex_) + frames) % ringCapacity_);
}

} // namespace rocks::audio

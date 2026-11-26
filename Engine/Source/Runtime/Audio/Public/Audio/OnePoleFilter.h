#pragma once

#include <Audio/AudioBuffer.h>

// 1극 저역·고역 필터다(Docs/Design/AudioSystem 3.20.4절). source의 차폐·공기 흡수·사용자
// LPF/HPF가 쓴다. 식은 UE FInterpolatedLPF·FInterpolatedHPF(SignalProcessing/Private/
// InterpolatedOnePole.cpp)와 같다.
//
// - LPF: b1 = exp(−π·clamp(2·fc/fs, 0, 1)), y = x + b1·(z − x), z = y.
// - HPF: G = tan(π·fc/fs), a0 = G/(1 + G), v = (x − z)·a0, lp = v + z, z = v + lp, y = x − lp.
//   fc는 UE처럼 [0, fs/2 − 1 Hz]로 자른다(fs/2에서는 a0 = 1이고 극이 −1에 놓여 상태가 발산한다).
// - 계수 보간: SetCutoff(fc, interpFrames)는 계수를 interpFrames frame에 걸쳐 선형으로 옮긴다.
//   ramp의 k번째 frame(0부터)은 start + (target − start)·(k + 1)/N을 쓴다(UE처럼 frame마다 먼저
//   한 단계 나아간 뒤 처리하므로 마지막 ramp frame이 target이다). ramp 중 재설정은 마지막으로
//   쓴 계수에서 새 ramp를 시작한다. 첫 설정과 bypass에서 다시 켜는 설정은 보간 없이 바로
//   적용한다(UE bIsFirstFrequencyChange).
// - bypass: LPF는 fc ≥ FilterBypassFrequency − FilterFrequencyTolerance, HPF는 fc ≤
//   FilterMinFrequency + FilterFrequencyTolerance면 처리하지 않는다(UE AudioMixerSourceManager의
//   판정). bypass에서 다시 켤 때 상태 z는 0에서 시작한다.
//
// 처리 함수는 할당하지 않고 noexcept다. 채널 상태는 MaxOutputChannels개 고정 배열이다.
//
// 사용 예:
//   auto created = OnePoleLowPass::TryCreate(48000, 2);
//   OnePoleLowPass& occlusion = created.Value();
//   occlusion.SetCutoff(2000.0f, 256);
//   occlusion.Process(voiceBuffer);
namespace rocks::audio {

// LPF가 이 주파수(Hz) 이상이면 bypass다(UE MAX_FILTER_FREQUENCY —
// AudioMixerCore/Public/AudioDefines.h).
inline constexpr float FilterBypassFrequency = 20000.0f;

// HPF가 이 주파수(Hz) 이하면 bypass다(UE MIN_FILTER_FREQUENCY — 같은 파일, 20 Hz).
inline constexpr float FilterMinFrequency = 20.0f;

// bypass 판정의 여유(Hz)다(UE KINDA_SMALL_NUMBER).
inline constexpr float FilterFrequencyTolerance = 1.0e-4f;

// HPF 차단 주파수 상한을 Nyquist에서 뺀 폭(Hz)이다(UE NyquistLimit = fs/2 − 1).
inline constexpr float HighPassNyquistMargin = 1.0f;

// 1극 필터의 공통 상태다. 계수 하나를 보간하고 채널마다 z를 하나 둔다.
class ROCKS_AUDIO_API OnePoleFilterState {
public:
    uint32 SampleRate() const noexcept {
        return sampleRate_;
    }

    uint32 ChannelCount() const noexcept {
        return channelCount_;
    }

    bool IsBypassed() const noexcept {
        return bypassed_;
    }

    float Cutoff() const noexcept {
        return cutoff_;
    }

    // 마지막으로 처리한 frame의 계수다(새 ramp 직후에는 ramp 시작값).
    float CurrentCoefficient() const noexcept {
        return CoefficientAt(done_);
    }

    float TargetCoefficient() const noexcept {
        return target_;
    }

    // 상태 z를 0으로 지운다. 계수는 그대로다.
    void ResetState() noexcept;

protected:
    OnePoleFilterState() noexcept = default;

    static bool IsValidSetup(uint32 sampleRate, uint32 channels) noexcept;

    void Initialize(uint32 sampleRate, uint32 channels) noexcept;

    // 새 계수와 bypass 여부를 받는다. 보간 규칙은 머리 주석과 같다.
    void SetCoefficient(float cutoff, float coefficient, bool bypass, uint32 interpFrames) noexcept;

    float CoefficientAt(uint32 step) const noexcept;

    float start_ = 0.0f;
    float target_ = 0.0f;
    uint32 total_ = 0;
    uint32 done_ = 0;
    float cutoff_ = 0.0f;
    uint32 sampleRate_ = 0;
    uint32 channelCount_ = 0;
    bool bypassed_ = true;
    float z_[MaxOutputChannels] = {};
};

class ROCKS_AUDIO_API OnePoleLowPass final : public OnePoleFilterState {
public:
    // sampleRate는 IsValidMixSampleRate, channels는 1–MaxOutputChannels다. 밖이면 InvalidArgument.
    // 만든 직후는 bypass(설정 전)다.
    [[nodiscard]] static Result<OnePoleLowPass> TryCreate(uint32 sampleRate,
                                                          uint32 channels) noexcept;

    // 차단 주파수(Hz)를 바꾼다. 유한하지 않거나 음수면 InvalidArgument이고 바꾸지 않는다.
    Result<void> SetCutoff(float cutoffHz, uint32 interpFrames) noexcept;

    // 제자리 처리한다. view의 채널 수는 ChannelCount()와 같아야 한다(다르면 assert).
    void Process(const PlanarView& view) noexcept;

    // 차단 주파수의 b1 계수다(UE 식). 시험·진단용으로 공개한다.
    static float ComputeCoefficient(float cutoffHz, uint32 sampleRate) noexcept;
};

class ROCKS_AUDIO_API OnePoleHighPass final : public OnePoleFilterState {
public:
    [[nodiscard]] static Result<OnePoleHighPass> TryCreate(uint32 sampleRate,
                                                           uint32 channels) noexcept;

    Result<void> SetCutoff(float cutoffHz, uint32 interpFrames) noexcept;

    void Process(const PlanarView& view) noexcept;

    // 차단 주파수의 a0 계수다(UE 식, fc는 [0, fs/2 − HighPassNyquistMargin]로 자른다).
    static float ComputeCoefficient(float cutoffHz, uint32 sampleRate) noexcept;
};

} // namespace rocks::audio

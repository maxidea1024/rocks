#pragma once

#include <Audio/AudioBuffer.h>

// compressor·limiter·expander·gate dynamics 처리기다(Docs/Design/AudioSystem.ko.md 3.21절). UE
// Audio::FDynamicsProcessor의 블록 경로(ProcessAudioDeinterleaveInternal)와
// FInlineEnvelopeFollower에 해당한다. 마스터 limiter(AU1)와 compressor 효과(AU7)가 쓴다.
//
// 처리 순서: key(입력 × input gain) → 채널별 엔벌로프 → link(채널 평균·최대·없음) → dB(하한 −96 dB)
// → 정적 곡선(threshold·ratio·knee)의 이득 → lookahead 지연을 거친 입력 × 이득 × output gain ×
// input gain. 상태는 표본 순서로만 진행하므로 블록을 어떻게 나눠 불러도 출력이 bit 단위로 같다.
//
// 사용 예:
//   DynamicsSettings settings;
//   settings.mode = DynamicsMode::Limiter;
//   settings.thresholdDb = -1.0f;
//   auto created = DynamicsProcessor::TryCreate(48000, 2, settings, allocator);
//   if (!created) { return created.Error(); }
//   created.Value().Process(masterBuffer.View(frames));
namespace rocks::audio {

enum class DynamicsMode : uint8 {
    Compressor,
    Limiter,
    Expander,
    Gate,
    UpwardsCompressor
};

// 엔벌로프 추종기가 입력을 보는 방식이다(UE EPeakMode, TruePeak 제외).
enum class EnvelopePeakMode : uint8 {
    Peak,
    MeanSquared,
    RootMeanSquared
};

// 채널 엔벌로프를 묶는 방식이다(UE EDynamicsProcessorChannelLinkMode).
enum class DynamicsLinkMode : uint8 {
    Disabled,
    Average,
    Peak
};

// lookahead 지연의 상한(ms)이다(UE MaxLookaheadMsec). 지연 링 크기가 이 값으로 정해진다.
inline constexpr float MaxLookaheadMs = 100.0f;

// UpwardsCompressor 이득 상한(dB)이다(UE UpwardsCompressionMaxGain).
inline constexpr float UpwardsCompressionMaxGainDb = 36.0f;

// ratio의 하한이다(UE SMALL_NUMBER). 더 작으면 1/ratio가 넘친다.
inline constexpr float MinDynamicsRatio = 1.0e-8f;

// input·output gain의 절댓값 상한(dB)이다. 선형값이 float에서 넘치지 않게 둔다(UE는 제한 없음).
inline constexpr float MaxDynamicsGainDb = 96.0f;

// 엔벌로프를 dB로 바꿀 때의 하한(dB)이다(UE ArrayMagnitudeToDecibelInPlace의 −96).
inline constexpr float DynamicsLevelFloorDb = -96.0f;

// 기본값은 UE FDynamicsProcessor 생성자 값이다. 범위는 3.21.1절 표다.
struct DynamicsSettings {
    DynamicsMode mode = DynamicsMode::Compressor;
    EnvelopePeakMode peakMode = EnvelopePeakMode::Peak;
    DynamicsLinkMode linkMode = DynamicsLinkMode::Disabled;
    float lookaheadMs = 10.0f;
    float attackMs = 20.0f;
    float releaseMs = 1000.0f;
    float thresholdDb = -6.0f;
    float ratio = 1.0f;
    float kneeBandwidthDb = 10.0f;
    float inputGainDb = 0.0f;
    float outputGainDb = 0.0f;
    bool analogMode = true;
    float analysisWindowMs = 5.0f;
};

class ROCKS_AUDIO_API DynamicsProcessor {
public:
    // 처리기를 만든다.
    // - sampleRate는 IsValidMixSampleRate, channels는 1–MaxOutputChannels, settings는
    //   3.21.1절 범위다. 밖이면 InvalidArgument.
    // - 지연 링(채널마다 100 ms + 1 frame)과 엔벌로프 scratch(채널 × MaxBlockFrames)를 한 번에
    //   할당한다. 실패는 OutOfMemory. allocator는 처리기보다 오래 살아야 한다.
    [[nodiscard]] static Result<DynamicsProcessor> TryCreate(uint32 sampleRate, uint32 channels,
                                                             const DynamicsSettings& settings,
                                                             Allocator& allocator) noexcept;

    DynamicsProcessor() noexcept = default;
    DynamicsProcessor(DynamicsProcessor&& other) noexcept;
    DynamicsProcessor& operator=(DynamicsProcessor&& other) noexcept;
    DynamicsProcessor(const DynamicsProcessor&) = delete;
    DynamicsProcessor& operator=(const DynamicsProcessor&) = delete;
    ~DynamicsProcessor() noexcept;

    // 설정을 바꾼다. 범위 밖이면 InvalidArgument이고 바꾸지 않는다. 할당하지 않는다. 엔벌로프·지연
    // 이력은 유지한다(lookahead 길이를 바꾸면 읽기 위치만 옮긴다).
    Result<void> SetSettings(const DynamicsSettings& settings) noexcept;

    const DynamicsSettings& Settings() const noexcept {
        return settings_;
    }

    // 제자리 처리한다. 채널 수는 생성 때 값이어야 한다(다르면 assert). frame 수에 제한은 없다
    // (MaxBlockFrames씩 나눠 처리한다). 비유한 표본은 엔벌로프 계산에서 0으로 본다. 빈 처리기는
    // 아무것도 하지 않는다.
    void Process(const PlanarView& view) noexcept;

    // 엔벌로프·평균 제곱·지연 링을 0으로 지운다.
    void Reset() noexcept;

    // 엔벌로프 level(dB)에 대한 정적 곡선 이득(선형)이다. 시험·진단용이다.
    float GainForLevelDb(float levelDb) const noexcept;

    uint32 LookaheadFrames() const noexcept {
        return delayFrames_;
    }

    float AttackCoefficient() const noexcept {
        return attack_;
    }

    float ReleaseCoefficient() const noexcept {
        return release_;
    }

    uint32 ChannelCount() const noexcept {
        return channelCount_;
    }

private:
    void Release() noexcept;

    void ProcessBlock(const PlanarView& view) noexcept;

    void ApplySettings(const DynamicsSettings& settings) noexcept;

    float* RingOf(uint32 channel) const noexcept;

    float* ScratchOf(uint32 channel) const noexcept;

    DynamicsSettings settings_;
    Allocator* allocator_ = nullptr;
    float* memory_ = nullptr;
    uint32 sampleRate_ = 0;
    uint32 channelCount_ = 0;
    // 지연 링 칸 수(최대 지연 + 1)와 쓰기 위치, 현재 지연 frame 수다.
    uint32 ringCapacity_ = 0;
    uint32 writeIndex_ = 0;
    uint32 delayFrames_ = 0;
    // 엔벌로프 계수와 평균 제곱 창 계수다.
    float attack_ = 0.0f;
    float release_ = 0.0f;
    float windowAlpha_ = 0.0f;
    float windowBeta_ = 0.0f;
    // 정적 곡선: 기울기, knee 두 점의 x, 반 knee 폭, 라그랑주 분모 둘.
    float slope_ = 0.0f;
    float kneeLow_ = 0.0f;
    float kneeHigh_ = 0.0f;
    float halfKnee_ = 0.0f;
    float denominatorLowMinusHigh_ = 0.0f;
    float denominatorHighMinusLow_ = 0.0f;
    float inputGain_ = 1.0f;
    float outputGain_ = 1.0f;
    float envelope_[MaxOutputChannels] = {};
    float meanSquare_[MaxOutputChannels] = {};
};

} // namespace rocks::audio

#pragma once

#include <Audio/AudioBuffer.h>

// 런타임 선형 리샘플러다(Docs/Design/AudioSystem.ko.md 3.20.5절). UE FRuntimeResampler
// (SignalProcessing/Public/DSP/RuntimeResampler.h)와 같은 선형 보간이고, 고품질 리샘플은 쿠킹이
// 맡는다.
//
// - 비율 r = 출력 1 frame당 입력 frame 진행량(= pitch × srcRate / mixRate)이고 범위는
//   [MinFrameRatio, MaxFrameRatio]다(UE와 같은 [0.01, 100]).
// - 입력 위치와 비율은 32.32 고정소수점이다(UE는 16.16). 그래서 같은 입력·비율이면 출력 블록을
//   어떻게 나눠도 위치 열이 정확히 같고 출력 PCM이 bit 단위로 같다.
// - 출력 frame n은 위치 P의 i = floor(P), frac = P − i에서 x[i] + (x[i+1] − x[i])·frac이다.
//   frac = 0이면 x[i]다. x[−1]은 직전 Process가 마지막으로 소비한 입력 frame이다.
// - Process는 다음 출력 위치보다 앞선 입력 frame을 모두 소비한다(소비 수 = ceil(다음 위치),
//   0 이상). 호출자는 소비되지 않은 입력을 다음 호출의 앞에 다시 준다.
// - 비율 보간: SetFrameRatio(r, interpFrames)는 interpFrames 출력 frame에 걸쳐 비율을 선형으로
//   옮긴다. ramp의 k번째 출력 frame(0부터) 뒤의 진행량은 start + (target − start)·(k + 1)/N이다.
//
// 사용 예:
//   auto created = LinearResampler::TryCreate(2, 44100.0 / 48000.0);
//   LinearResampler& resampler = created.Value();
//   auto needed = resampler.InputFramesNeeded(output.frameCount);
//   ... needed.Value() frame 이상을 decoder에서 읽어 input에 채운다 ...
//   auto consumed = resampler.Process(input, output);  // 소비한 입력 frame 수
namespace rocks::audio {

// 비율의 하한·상한이다(UE FRuntimeResampler::MinFrameRatio·MaxFrameRatio).
inline constexpr double MinFrameRatio = 0.01;
inline constexpr double MaxFrameRatio = 100.0;

// 비율 보간 길이의 상한(출력 frame)이다. 48 kHz에서 약 21.8초다. 고정소수점 곱이 넘치지 않는
// 한계다.
inline constexpr uint32 MaxRatioInterpFrames = 1u << 20;

class ROCKS_AUDIO_API LinearResampler {
public:
    // channels는 1–MaxOutputChannels, frameRatio는 유한하고 [MinFrameRatio, MaxFrameRatio]여야
    // 한다. 밖이면 InvalidArgument. 위치는 0, 직전 frame은 0이다.
    [[nodiscard]] static Result<LinearResampler> TryCreate(uint32 channels,
                                                           double frameRatio) noexcept;

    // 비율을 interpFrames 출력 frame에 걸쳐 옮긴다(0이면 즉시). 범위 밖 비율·interpFrames >
    // MaxRatioInterpFrames는 InvalidArgument이고 바꾸지 않는다. ramp 중에 부르면 지금 진행량에서
    // 시작한다.
    Result<void> SetFrameRatio(double frameRatio, uint32 interpFrames) noexcept;

    // outputFrames frame을 내려면 필요한 입력 frame 수다(비율 보간 경로를 따라 계산한다).
    // outputFrames > MaxBlockFrames는 InvalidArgument다.
    [[nodiscard]] Result<uint32> InputFramesNeeded(uint32 outputFrames) const noexcept;

    // output.frameCount frame을 만들고 소비한 입력 frame 수를 돌려준다.
    // - input·output의 채널 수는 ChannelCount()와 같아야 하고, output.frameCount ≤ MaxBlockFrames,
    //   input.frameCount ≥ InputFramesNeeded(output.frameCount)여야 한다. 아니면
    //   InvalidArgument이고 상태를 바꾸지 않는다(output도 쓰지 않는다).
    // - input과 output은 겹치면 안 된다.
    [[nodiscard]] Result<uint32> Process(const PlanarConstView& input,
                                         const PlanarView& output) noexcept;

    // 위치·직전 frame을 0으로 되돌리고 비율 보간을 목표값으로 끝낸다.
    void Reset() noexcept;

    uint32 ChannelCount() const noexcept {
        return channelCount_;
    }

    // 다음 출력 frame 뒤에 쓸 진행량이다.
    double FrameRatio() const noexcept;

    double TargetFrameRatio() const noexcept;

    // 다음 출력 frame의 입력 위치(frame)다. 소비하지 않은 입력의 첫 frame이 0이고 (−1, 0] 또는
    // 처음 상태의 0이다. 진단·시험용이다.
    double Position() const noexcept;

private:
    int64 StepAt(uint32 step) const noexcept;

    int64 position_ = 0;
    int64 ratioStart_ = 0;
    int64 ratioTarget_ = 0;
    uint32 ratioTotal_ = 0;
    uint32 ratioDone_ = 0;
    uint32 channelCount_ = 0;
    float previous_[MaxOutputChannels] = {};
};

} // namespace rocks::audio

#pragma once

#include <Audio/AudioBuffer.h>

// 목표 이득으로 frame 단위로 옮겨 가는 gain ramp다(Docs/Design/AudioSystem.ko.md 3.20.3절).
//
// ramp 중 k번째 frame(0부터)의 이득은 start + (target − start)·k/N이고(N = ramp 길이), N frame을
// 처리한 뒤의 현재값은 target과 정확히 같다(부동소수 누적 오차가 남지 않는다). 이득을 매 frame
// 식으로 계산하므로 Apply를 어떤 블록 크기로 나눠 불러도 출력이 bit 단위로 같다.
//
// 사용 예:
//   GainRamp ramp(0.0f);
//   ramp.SetTarget(1.0f, 480);  // 48 kHz에서 10 ms 페이드 인
//   ramp.Apply(voiceBuffer);
namespace rocks::audio {

class GainRamp {
public:
    explicit GainRamp(float initialGain = 1.0f) noexcept
        : start_(initialGain), target_(initialGain) {
    }

    // 목표를 바꾼다. target은 유한해야 한다(아니면 assert). rampFrames가 0이면 즉시 target이
    // 된다. ramp 중에 부르면 지금의 현재값에서 새 ramp를 시작한다.
    void SetTarget(float target, uint32 rampFrames) noexcept;

    // 즉시 gain으로 맞추고 ramp를 끝낸다.
    void Reset(float gain) noexcept;

    // view의 모든 채널에 frame마다 현재 이득을 곱하고 그만큼 진행한다.
    void Apply(const PlanarView& view) noexcept;

    // destination[n] += source[n] × 이득(n)을 frames frame 하고 그만큼 진행한다. 채널 이득
    // 행렬(믹서 voice → 출력 채널)에 쓴다. Apply와 같은 이득 열이다.
    void MixAdd(const float* source, float* destination, uint32 frames) noexcept;

    // frames만큼 처리한 것처럼 진행만 한다(가상화된 voice 등 출력 없이 시간만 흐를 때).
    void Advance(uint32 frames) noexcept;

    // 다음 frame에 곱할 이득이다.
    float Current() const noexcept;

    float Target() const noexcept {
        return target_;
    }

    uint32 RemainingFrames() const noexcept {
        return total_ - done_;
    }

    bool IsRamping() const noexcept {
        return done_ < total_;
    }

private:
    float GainAt(uint32 step) const noexcept;

    float start_ = 1.0f;
    float target_ = 1.0f;
    uint32 total_ = 0;
    uint32 done_ = 0;
};

} // namespace rocks::audio

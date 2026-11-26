#include "SandboxSounds.h"

#include <Audio/Decibels.h>
#include <Core/Math/MathUtility.h>

namespace sandbox {

namespace {

using namespace rocks;

constexpr uint32 SoundRate = audio::DefaultMixSampleRate;
// UI 클릭: 880 Hz, 80 ms, −6 dBFS 시작, frame마다 곱하는 감쇠(80 ms 끝에서 약 −17 dB).
constexpr float ClickHz = 880.0f;
constexpr uint32 ClickFrames = SoundRate * 80 / 1000;
constexpr float ClickDecibels = -6.0f;
constexpr float ClickDecayPerFrame = 0.9995f;
// loop: 220 Hz 톱니(1초 = 220 주기), 1 kHz one-pole LPF, −12 dBFS.
constexpr float LoopHz = 220.0f;
constexpr uint32 LoopFrames = SoundRate;
constexpr float LoopCutoffHz = 1000.0f;
constexpr float LoopDecibels = -12.0f;
// loop 감쇠: 내부 반경 200 cm, 감쇠 거리 3000 cm(데모 원 600 cm에서 선형 약 0.87).
constexpr float LoopInnerRadius = 200.0f;
constexpr float LoopFalloffDistance = 3000.0f;

Result<SharedPtr<const SoundPcm>> MakeClickPcm(Allocator& allocator) noexcept {
    auto pcm = SoundPcm::TryAllocate(allocator, 1, ClickFrames, SoundRate);
    if (!pcm) {
        return Result<SharedPtr<const SoundPcm>>::Failure(pcm.Error());
    }

    Span<float> samples = pcm.Value()->MutableChannel(0);
    float envelope = audio::DecibelsToLinear(ClickDecibels);
    for (uint32 frame = 0; frame < ClickFrames; ++frame) {
        samples[frame] =
            envelope * math::Sin(math::TwoPi<float> * ClickHz * static_cast<float>(frame) /
                                 static_cast<float>(SoundRate));
        envelope *= ClickDecayPerFrame;
    }

    return Result<SharedPtr<const SoundPcm>>::Success(SharedPtr<const SoundPcm>(Move(pcm.Value())));
}

Result<SharedPtr<const SoundPcm>> MakeLoopPcm(Allocator& allocator) noexcept {
    auto pcm = SoundPcm::TryAllocate(allocator, 1, LoopFrames, SoundRate);
    if (!pcm) {
        return Result<SharedPtr<const SoundPcm>>::Failure(pcm.Error());
    }

    // 필터를 한 주기(1초) 먼저 돌려 정상 상태에서 시작한다 — 두 번째 1초를 담으므로 loop 이음매가
    // 연속이다.
    Span<float> samples = pcm.Value()->MutableChannel(0);
    const float gain = audio::DecibelsToLinear(LoopDecibels);
    // RC 필터 계수 α = dt / (RC + dt), RC = 1 / (2π·fc).
    const float alpha =
        1.0f / (1.0f + static_cast<float>(SoundRate) / (math::TwoPi<float> * LoopCutoffHz));
    float state = 0.0f;
    for (uint32 pass = 0; pass < 2; ++pass) {
        for (uint32 frame = 0; frame < LoopFrames; ++frame) {
            const float phase = static_cast<float>(frame) * LoopHz / static_cast<float>(SoundRate);
            const float saw = 2.0f * (phase - math::Floor(phase)) - 1.0f;
            state += alpha * (saw - state);
            samples[frame] = gain * state;
        }
    }

    return Result<SharedPtr<const SoundPcm>>::Success(SharedPtr<const SoundPcm>(Move(pcm.Value())));
}

} // namespace

Result<SandboxSounds> MakeSandboxSounds(Allocator& allocator) noexcept {
    auto click = MakeClickPcm(allocator);
    auto loop = MakeLoopPcm(allocator);
    if (!click || !loop) {
        return Result<SandboxSounds>::Failure(ErrorCode::OutOfMemory);
    }

    SoundPlayable clickPlayable;
    clickPlayable.pcm = click.Value();
    SoundPlayable loopPlayable;
    loopPlayable.pcm = loop.Value();
    loopPlayable.looping = true;
    SoundAttenuation attenuation;
    attenuation.innerRadius = LoopInnerRadius;
    attenuation.distance.falloffDistance = LoopFalloffDistance;
    loopPlayable.attenuation = attenuation;
    auto clickSound = SoundSourceRef::TryCreate(allocator, clickPlayable);
    auto loopSound = SoundSourceRef::TryCreate(allocator, loopPlayable);
    if (!clickSound || !loopSound) {
        return Result<SandboxSounds>::Failure(clickSound ? loopSound.Error() : clickSound.Error());
    }

    SandboxSounds sounds;
    sounds.click = clickSound.Value();
    sounds.loop = loopSound.Value();
    return Result<SandboxSounds>::Success(sounds);
}

} // namespace sandbox

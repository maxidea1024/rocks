#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Meta/Annotations.h>

// 동시 발음 그룹 설정이다(Docs/Design/AudioSystem 3.41.2절, UE FSoundConcurrencySettings 대응).
namespace rocks {

// 동시 발음 그룹이 찼을 때의 해결 규칙이다(3.41.2절, UE EMaxConcurrentResolutionRule —
// SoundConcurrency.h:31). 앞 넷은 수락 때 희생자를 고르고(eviction), 뒤 셋은 그룹이 최대 수를 넘게
// 받아 두고 묶음마다 넘친 만큼 멈춘다(cull).
ROCKS_ENUM()
enum class SoundConcurrencyRule : uint8 {
    // 새 재생을 받지 않는다.
    PreventNew = 0,
    // 재생 시간이 가장 긴 재생을 멈춘다.
    StopOldest = 1,
    // 새 재생보다 먼 재생 중 가장 먼 것을 멈추고, 없으면 새 재생을 받지 않는다.
    StopFarthestThenPreventNew = 2,
    // 위와 같고, 같은 거리면 오래된 것을 멈춘다(UE 기본).
    StopFarthestThenOldest = 3,
    // 우선순위가 가장 낮은 재생을 멈춘다(동률이면 오래된 것).
    StopLowestPriority = 4,
    // 들리는 크기가 가장 작은 재생을 멈춘다(동률이면 새 것).
    StopQuietest = 5,
    // 우선순위가 가장 낮은 재생을 멈춘다(동률이면 새 것).
    StopLowestPriorityThenPreventNew = 6
};

// 그룹 안 재생의 볼륨 감쇠(ducking) 순서다(UE EConcurrencyVolumeScaleMode — SoundConcurrency.h:60).
ROCKS_ENUM()
enum class SoundConcurrencyVolumeScaleMode : uint8 {
    // 나중에 들어온 재생일수록 크다(앞 재생이 들어온 순서대로 줄어든다).
    Default = 0,
    // 가까운 재생일수록 크다.
    Distance = 1,
    // 우선순위가 높은 재생일수록 크다.
    Priority = 2
};

// 동시 발음 그룹 설정이다(3.41.2절, UE FSoundConcurrencySettings — SoundConcurrency.h:31–170,
// 기본값도 같다). 그룹을 처음 만들 때의 사본으로 고정된다.
ROCKS_STRUCT()
struct SoundConcurrencySettings {
    ROCKS_GENERATED_BODY()

    // 그룹의 최대 재생 수다(1 이상, UE DefaultMaxCount 16).
    ROCKS_PROPERTY() uint32 maxCount = 16;
    // 켜면 소유자(PlayOptions::owner)마다 따로 센다. 소유자가 없으면 전역 그룹이다.
    ROCKS_PROPERTY() bool limitToOwner = false;
    ROCKS_PROPERTY()
    SoundConcurrencyRule resolutionRule = SoundConcurrencyRule::StopFarthestThenOldest;
    // 그룹의 마지막 재생에서 이 초가 지나기 전 새 재생은 받지 않는다(0이면 끔).
    ROCKS_PROPERTY() float retriggerTime = 0.0f;
    // 그룹 안 재생마다 곱하는 감쇠 배수([0, 1])와 순서·바뀌는 시간(초)이다.
    ROCKS_PROPERTY() float volumeScale = 1.0f;
    ROCKS_PROPERTY()
    SoundConcurrencyVolumeScaleMode volumeScaleMode = SoundConcurrencyVolumeScaleMode::Default;
    ROCKS_PROPERTY() float volumeScaleAttackTime = 0.01f;
    ROCKS_PROPERTY() float volumeScaleReleaseTime = 0.5f;
    // 켜면 재생이 빠질 때 남은 재생의 감쇠를 되돌린다(Default 순서만).
    ROCKS_PROPERTY() bool volumeScaleCanRelease = false;
    // 빼앗긴 재생이 사라지는 시간(초, [0, MaxVoiceStealReleaseSeconds])이다. 0이면 믹서 정지 fade만
    // 한다.
    ROCKS_PROPERTY() float voiceStealReleaseTime = 0.0f;
};

// voiceStealReleaseTime 상한(초)이다(긴 steal fade가 voice 여유를 오래 묶지 않게).
inline constexpr float MaxVoiceStealReleaseSeconds = 60.0f;
// 재생 하나가 속할 수 있는 동시 발음 그룹 수다.
inline constexpr uint32 MaxConcurrencyGroups = 8;

// 설정 값 범위를 본다: maxCount ≥ 1, volumeScale [0, 1], 시간 유한 ≥ 0, voiceStealReleaseTime ≤
// 상한, enum 범위. 어기면 InvalidArgument다.
ROCKS_AUDIOASSETS_API Result<void>
ValidateSoundConcurrencySettings(const SoundConcurrencySettings& settings) noexcept;

} // namespace rocks

#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 믹서 버스 에셋의 열거·상수다(Docs/Design/AudioSystem 3.45.7절, UE USoundSubmix·
// FSoundSubmixSendInfo).
namespace rocks {

// 버스 사슬 깊이 상한이다(자기부터 루트까지 — UE에는 제한이 없다, 07장).
inline constexpr uint32 MaxMixBusDepth = 16;

// 버스 레벨(dB) 범위와 기본값이다(UE OutputVolume·WetLevel·DryLevelModulation — SoundSubmix.cpp
// 35–37).
inline constexpr float MinMixBusLevelDb = -96.0f;
inline constexpr float MaxMixBusLevelDb = 0.0f;
inline constexpr float DefaultMixBusDryLevelDb = -96.0f;

// 에셋 send 배열 하나의 상한이다(재생 라우팅 상한 audio::MaxVoiceSends와 같다).
inline constexpr uint32 MaxMixBusSends = 8;

// send 거리·레벨 기본값과 필터 범위다(UE FSoundSubmixSendInfoBase — SoundSubmixSend.cpp 6–19).
inline constexpr float DefaultMixBusSendMinDistance = 100.0f;
inline constexpr float DefaultMixBusSendMaxDistance = 1000.0f;
inline constexpr float MinMixBusSendFilterHz = 20.0f;
inline constexpr float MaxMixBusSendFilterHz = 20000.0f;

// 거리 방법의 분모 하한이다(UE max(MaxSendDistance − MinSendDistance, 1)).
inline constexpr float MinMixBusSendDistanceRange = 1.0f;

// send level을 정하는 방법이다(UE ESendLevelControlMethod — Manual·Linear·CustomCurve).
ROCKS_ENUM()
enum class MixBusSendLevelMethod : uint8 {
    // 상수 level(UE Manual).
    Constant,
    // 거리 [minDistance, maxDistance]를 [minLevel, maxLevel]로 선형 사상(UE Linear).
    DistanceLinear,
    // 정규화 거리(0–1)를 곡선으로 사상(UE CustomCurve).
    DistanceCurve
};

// send를 갈라 내는 신호 지점이다(UE ESubmixSendStage).
ROCKS_ENUM()
enum class MixBusSendStage : uint8 {
    // 필터·거리 감쇠 뒤(UE PostDistanceAttenuation, 기본).
    PostAttenuation,
    // 그 앞(UE PreDistanceAttenuation).
    PreAttenuation
};

} // namespace rocks

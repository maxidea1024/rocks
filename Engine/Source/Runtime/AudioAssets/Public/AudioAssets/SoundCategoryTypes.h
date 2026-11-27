#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 분류·믹스 스냅숏의 열거형과 상한·기본값이다(Docs/Design/AudioSystem 3.43.1절).
// 에셋(SoundCategory· MixSnapshot)과 재생 스냅숏(SoundCategoryChain·MixSnapshotData)이 같이 쓴다.
namespace rocks {

// 분류가 곱할 사용자 볼륨 CVar다(Rocks — 3.43.3절, UE에는 대응 기능이 없다). None이면 곱하지
// 않는다.
ROCKS_ENUM()
enum class SoundUserVolume : uint8 {
    None = 0,
    // audio.volume.music
    Music = 1,
    // audio.volume.effects
    Effects = 2,
    // audio.volume.dialogue
    Dialogue = 3,
    // audio.volume.interface
    Interface = 4
};

inline constexpr uint32 SoundUserVolumeCount = 5;

// 분류 사슬(자기부터 루트까지)의 최대 깊이다. 넘으면 MakeChain이 CapacityExceeded다.
inline constexpr uint32 MaxSoundCategoryDepth = 16;
// 분류 하나의 passive 믹스 수와 믹스 하나의 분류 조정 수 상한이다(UE는 배열 — Rocks 고정 용량).
inline constexpr uint32 MaxCategoryPassiveMixes = 8;
inline constexpr uint32 MaxMixAdjusters = 32;
// 믹스 스냅숏 시간(지연·fade·지속)의 상한(초 — 하루)이다. mix frame 변환이 넘치지 않게 한다.
inline constexpr float MaxMixSnapshotSeconds = 86400.0f;
// 믹스 조정 pitch 상한(UE FSoundClassAdjuster PitchAdjuster ClampMax 8 — SoundMix.h:141)이다.
inline constexpr float MaxMixAdjusterPitch = 8.0f;
// 믹스 fade 기본 시간(초 — UE USoundMix FadeInTime·FadeOutTime 0.2)이다.
inline constexpr float DefaultMixFadeSeconds = 0.2f;
// passive 믹스 볼륨 임계 기본 상한(UE FPassiveSoundMixModifier MaxVolumeThreshold 10)이다.
inline constexpr float DefaultPassiveMaxVolumeThreshold = 10.0f;

} // namespace rocks

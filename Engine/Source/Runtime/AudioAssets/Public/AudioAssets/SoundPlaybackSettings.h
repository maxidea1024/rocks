#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 재생 관리(우선순위·가상화·동시 발음)에 쓰는 값이다(Docs/Design/AudioSystem 3.41절). SoundSource
// 필드와 재생 스냅숏(SoundPlayable)이 함께 쓴다.
namespace rocks {

// 가상화 모드다(3.41.3절, UE EVirtualizationMode — SoundBase.h:57). loop 재생에만 쓰인다.
ROCKS_ENUM()
enum class SoundVirtualizationMode : uint8 {
    // 가상화하지 않는다. 들리지 않아도 voice 경쟁만 한다.
    Disabled = 0,
    // 거리로는 가상화하지 않는다(동시 발음으로 가상이 되면 다음 검사에서 바로 다시 평가한다).
    PlayWhenSilent = 1,
    // 다시 들리면 요청한 시작 위치(PlayOptions::startFrame)부터 시작한다(UE 기본).
    Restart = 2,
    // 다시 들리면 논리 커서(들리지 않는 동안에도 진행한 위치)에서 짧은 fade-in으로 이어 시작한다.
    SeekRestart = 3
};

// 재생 우선순위 범위와 기본값이다(UE USoundBase::Priority — SoundBase.h:217, MAX_SOUND_PRIORITY
// 100).
inline constexpr float MinSoundPriority = 0.0f;
inline constexpr float MaxSoundPriority = 100.0f;
inline constexpr float DefaultSoundPriority = 1.0f;

} // namespace rocks

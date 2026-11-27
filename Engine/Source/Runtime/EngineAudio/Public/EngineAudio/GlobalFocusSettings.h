#pragma once

#include <Base/Types.h>

// 장치 전체 리스너 focus 배수다(Docs/Design/AudioSystem 3.42.5절). AudioEngineSubsystem이 쥐고
// 묶음마다 제어 쪽으로 넘긴다.
namespace rocks {

// 장치 전체 focus 배수다(UE FGlobalFocusSettings — AudioDevice.h:376–398). 모두 1이 기본이고 0 이상
// 유한해야 한다(AudioEngineSubsystem::SetGlobalFocusSettings).
struct GlobalFocusSettings {
    float focusAzimuthScale = 1.0f;
    float nonFocusAzimuthScale = 1.0f;
    float focusDistanceScale = 1.0f;
    float nonFocusDistanceScale = 1.0f;
    float focusVolumeScale = 1.0f;
    float nonFocusVolumeScale = 1.0f;
    float focusPriorityScale = 1.0f;
    float nonFocusPriorityScale = 1.0f;
};

// 모든 칸이 0 이상 유한이다.
ROCKS_ENGINEAUDIO_API bool IsValidGlobalFocusSettings(const GlobalFocusSettings& settings) noexcept;

} // namespace rocks

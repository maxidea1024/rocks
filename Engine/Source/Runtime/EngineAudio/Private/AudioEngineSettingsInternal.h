#pragma once

#include "ControlBatch.h"

#include <Base/Types.h>
#include <Config/CVarHandle.h>
#include <Core/Diagnostics/LogCategory.h>

// EngineAudio 안에서 같이 쓰는 CVar 핸들·상수·로그 범주다(3.35.10절).
namespace rocks {

// EngineAudio 로그 범주다. 제어 pipe에서는 쓰지 않는다(게임 스레드에서만).
inline constexpr LogCategory LogEngineAudio{"EngineAudio"};

// 실제 voice 상한의 기본값(UE MaxChannels 기본 32 — AudioSettings.h:93)과 설정 상한이다.
inline constexpr uint32 DefaultMaxActiveVoices = 32;
inline constexpr uint32 MaxActiveVoicesSetting = 512;

// 믹서 voice 표를 실제 voice 상한의 몇 배로 만들지다. 놓기 대기(ACK 전) voice가 새 재생을 막지
// 않게 하는 여유분이다(3.35.6절).
inline constexpr uint32 MixerVoiceHeadroomFactor = 2;

// audio.mixer.samplerate의 범위(0 = 장치 기본)다.
inline constexpr uint32 MinMixerSampleRateSetting = 8000;
inline constexpr uint32 MaxMixerSampleRateSetting = 192000;

namespace engine_audio_detail {

extern cvar::CVar<float> MasterVolumeVar;
extern cvar::CVar<float> UnfocusedVolumeVar;
extern cvar::BootCVar<int32> MixerBlockFramesVar;
extern cvar::BootCVar<int32> MixerBufferCountVar;
extern cvar::BootCVar<int32> MixerSampleRateVar;
extern cvar::BootCVar<int32> MaxVoicesVar;
extern cvar::BootCVar<int32> MixerMaxBusesVar;
extern cvar::BootCVar<int32> StreamRingFramesVar;
extern cvar::BootCVar<int32> StreamMaxStreamsVar;
extern cvar::CVar<int32> StreamDefaultLoadingVar;
extern cvar::CVar<float> ConcurrencyMinDistanceDeltaVar;
extern cvar::CVar<int32> FocusInitializeOnFirstUpdateVar;
extern cvar::CVar<float> MusicVolumeVar;
extern cvar::CVar<float> EffectsVolumeVar;
extern cvar::CVar<float> DialogueVolumeVar;
extern cvar::CVar<float> InterfaceVolumeVar;
extern cvar::CVar<float> OcclusionCheckIntervalVar;
extern cvar::CVar<int32> OcclusionDisableVar;
extern cvar::CVar<int32> OcclusionFilterScaleVar;
extern cvar::CVar<int32> VirtualEnabledVar;
extern cvar::CVar<int32> VirtualAllowPlayWhenSilentVar;
extern cvar::CVar<float> VirtualUpdateMinVar;
extern cvar::CVar<float> VirtualUpdateMaxVar;
extern cvar::CVar<float> VirtualPerfDistanceVar;
extern cvar::CVar<float> VirtualForceUpdateDistanceVar;
extern cvar::CVar<float> VirtualMaxWorldDistanceVar;

// audio.virtual.updatemin·updatemax의 상한(초)이다.
inline constexpr float MaxVirtualUpdateSeconds = 60.0f;

// audio.virtual.* CVar 값을 묶음 값으로 모은다(설정 문맥이 있을 때만 부른다).
VirtualizationSettings VirtualizationFromSettings() noexcept;

} // namespace engine_audio_detail

} // namespace rocks

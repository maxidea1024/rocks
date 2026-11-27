#pragma once

#include <Audio/AudioFormat.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Vector3.h>
#include <Core/Result.h>

// 방위각 equal-power 패너다(Docs/Design/AudioSystem 3.13.2절, 3.20.6절). UE 기본 패너
// FMixerDevice::Get3DChannelMap(AudioMixer/Private/AudioMixerDevice.cpp)의 OmniDirectional 모드와
// 같다.
//
// 방위각은 리스너 앞이 0°, 오른쪽이 90°인 0–360° 시계 방향이다. 방위각을 감싸는 두
// 스피커(prev·next) 사이의 각도 비율 f로 next = sin(f·π/2), prev = cos(f·π/2)를 준다. 스피커
// 방위각은 스테레오 FL 270°· FR 90°, 5.1·7.1은 FL 330°·FR 30°·SL 250°·SR 110°·BL 210°·BR 150°이고
// FC·LFE는 방위각 계산에서 뺀다.
//
// 사용 예:
//   const float azimuth = ComputeAzimuthDegrees(listenerForward, listenerRight, toSoundDirection);
//   float gains[MaxOutputChannels];
//   (void)ComputeChannelGains(ChannelLayout::Stereo, azimuth, 0.0f, 0.0f, Span<float>(gains, 2));
namespace rocks::audio {

// mono 원본의 업믹스 이득이다(UE MonoUpmixLinear — 3.13.2절, mono 2D 0.5).
inline constexpr float MonoUpmixLinearGain = 0.5f;

// 비공간화 비율만큼 섞는 이득(omni)의 출력 채널 배치다(UE ENonSpatializedRadiusSpeakerMapMode —
// AudioMixerSource.cpp:2058–2160·2254–2310, 3.42.6절).
enum class AudioOmniMode : uint8 {
    // 모든 출력 채널에 1 ÷ 출력 채널 수.
    OmniDirectional,
    // mono는 FL·FR에 MonoUpmixLinearGain, 스테레오 원본은 L → FL 1·R → FR 1.
    Direct2D,
    // Direct2D에 5.1은 SL·SR, 7.1은 BL·BR을 더한다(스테레오 출력에서는 Direct2D와 같다).
    Surround2D
};

inline constexpr uint32 AudioOmniModeCount = 3;

// 방위각 패닝(과 omni 행)을 받는 원본 채널 수의 상한(mono·stereo)이다.
inline constexpr uint32 MaxAzimuthSourceChannels = 2;

// 리스너 기준 방위각(도, [0, 360))이다. UE FAudioDevice::GetAzimuth(AudioDevice.cpp)와 같은 식이다.
// - listenerForward·listenerRight: 리스너의 앞(+X)·오른쪽(+Y) 단위 축(UE 좌표계, 3.13.1절).
// - listenerToSound: 리스너에서 소리로 가는 단위 방향.
// 수평 성분이 없으면(정확히 위·아래) 0°다.
ROCKS_AUDIO_API float ComputeAzimuthDegrees(const Vec3& listenerForward, const Vec3& listenerRight,
                                            const Vec3& listenerToSound) noexcept;

// 레이아웃의 채널마다 패닝 이득을 out에 쓴다(out[i] = 레이아웃의 i번째 채널, AudioFormat.h 순서).
// - azimuthDegrees: [0, 360). 밖이면 360으로 감싼다. 유한하지 않으면 InvalidArgument.
// - nonSpatializedAmount: [0, 1]. 채널마다 Lerp(pan, 1/채널 수, 비율)이다(UE OmniDirectional).
// - lfeBleed: [0, 1]. LFE 채널 이득이다.
// - Mono 레이아웃은 1이다. out 크기가 ChannelCount(layout)보다 작으면 InvalidArgument.
// - 범위 밖 비율·bleed는 InvalidArgument이고 out을 쓰지 않는다.
ROCKS_AUDIO_API Result<void> ComputeChannelGains(ChannelLayout layout, float azimuthDegrees,
                                                 float nonSpatializedAmount, float lfeBleed,
                                                 Span<float> out) noexcept;

// 원본 채널 하나의 omni 이득 행을 out에 쓴다(out[i] = 레이아웃의 i번째 채널). sourceChannel은 원본
// 채널(0 = mono 또는 L, 1 = R), sourceChannels는 원본 채널 수(1·2)다. 범위 밖이거나 out이 모자라면
// InvalidArgument. Mono 레이아웃은 1이다.
ROCKS_AUDIO_API Result<void> ComputeOmniGains(ChannelLayout layout, AudioOmniMode mode,
                                              uint32 sourceChannel, uint32 sourceChannels,
                                              Span<float> out) noexcept;

// omni 이득 행(ComputeOmniGains)으로 섞는 판이다: 채널마다 Lerp(pan, omni[i], 비율)(UE
// Get3DChannelMap의 InOmniMap — AudioMixerDevice.cpp:2983–). omniGains 크기가
// ChannelCount(layout)보다 작으면 InvalidArgument. 그 밖의 규칙은 위와 같다.
ROCKS_AUDIO_API Result<void> ComputeChannelGains(ChannelLayout layout, float azimuthDegrees,
                                                 float nonSpatializedAmount, float lfeBleed,
                                                 Span<const float> omniGains,
                                                 Span<float> out) noexcept;

// 센터 전용 재생의 이득이다(UE bCenterChannelOnly). 스테레오는 양쪽 1/√2, Mono는 1, 그 밖은 FC =
// 1이고 나머지는 0이다. out 크기가 모자라면 InvalidArgument.
ROCKS_AUDIO_API Result<void> ComputeCenterChannelOnlyGains(ChannelLayout layout,
                                                           Span<float> out) noexcept;

} // namespace rocks::audio

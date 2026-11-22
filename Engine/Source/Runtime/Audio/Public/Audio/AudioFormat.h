#pragma once

#include <Base/Types.h>

// 믹서가 다루는 채널 레이아웃·채널 순서·샘플률 한계다(Docs/Design/AudioSystem.ko.md 3.3절,
// 3.20.1절).
//
// 내부 채널 순서는 FL, FR, FC, LFE, SL, SR, BL, BR이고 레이아웃의 i번째 채널은 이 순서의 앞
// ChannelCount개다(Mono만 예외로 FrontCenter 하나). 장치 순서(7.1의 BL·BR·SL·SR 등)로 바꾸는 일은
// 장치 backend(AU2)가 출력 직전에 한다.
//
// 사용 예:
//   for (uint32 index = 0; index < ChannelCount(layout); ++index) {
//       if (ChannelAt(layout, index) == AudioChannel::LowFrequency) { ... }
//   }
namespace rocks::audio {

enum class ChannelLayout : uint8 {
    Mono,
    Stereo,
    Surround51,
    Surround71
};

enum class AudioChannel : uint8 {
    FrontLeft,
    FrontRight,
    FrontCenter,
    LowFrequency,
    SideLeft,
    SideRight,
    BackLeft,
    BackRight
};

// 믹서 출력 채널 수의 상한(7.1)이다. planar 버퍼·필터 상태의 고정 배열 크기다.
inline constexpr uint32 MaxOutputChannels = 8;

// 기본 믹서 샘플률과 허용 범위(Hz)다. 계획 §18의 기본 48 kHz다.
inline constexpr uint32 DefaultMixSampleRate = 48000;
inline constexpr uint32 MinMixSampleRate = 8000;
inline constexpr uint32 MaxMixSampleRate = 192000;

// 한 번에 처리하는 frame 수의 상한이다. 장치 콜백 크기 상한이자 DSP 처리 단위 상한이다.
inline constexpr uint32 MaxBlockFrames = 4096;

// 레이아웃의 채널 수다(1·2·6·8).
constexpr uint32 ChannelCount(ChannelLayout layout) noexcept {
    switch (layout) {
    case ChannelLayout::Mono:
        return 1;
    case ChannelLayout::Stereo:
        return 2;
    case ChannelLayout::Surround51:
        return 6;
    case ChannelLayout::Surround71:
        return 8;
    }

    return 0;
}

// 레이아웃의 index번째 채널이다. index는 ChannelCount(layout)보다 작아야 한다(넘으면 FrontCenter).
constexpr AudioChannel ChannelAt(ChannelLayout layout, uint32 index) noexcept {
    if (layout == ChannelLayout::Mono || index >= ChannelCount(layout)) {
        return AudioChannel::FrontCenter;
    }

    return static_cast<AudioChannel>(index);
}

// 샘플률이 믹서 허용 범위 안인지 본다.
constexpr bool IsValidMixSampleRate(uint32 sampleRate) noexcept {
    return sampleRate >= MinMixSampleRate && sampleRate <= MaxMixSampleRate;
}

} // namespace rocks::audio

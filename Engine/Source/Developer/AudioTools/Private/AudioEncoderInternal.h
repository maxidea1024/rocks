#pragma once

#include <AudioTools/AudioEncoder.h>

// 인코더들이 함께 쓰는 입력 검사와 결과 머리 채우기다(Docs/Design/AudioSystem.ko.md 3.30.3절).
namespace rocks::audio::encoderdetail {

// 채널 수(1–info.maxChannels), 채널 마스크(9비트 안, bit 수 = 채널 수), rate 범위, frame > 0,
// 유한 표본을 본다. 어긋나면 InvalidArgument다.
Result<void> CheckEncodeInput(const PlanarConstView& pcm, uint32 sampleRate, uint32 channelMask,
                              const AudioCodecInfo& info) noexcept;

// codec·판·채널·마스크·rate·frame 수를 out에 적는다.
void FillCommon(const AudioCodecInfo& info, const PlanarConstView& pcm, uint32 sampleRate,
                uint32 channelMask, EncodedAudio& out) noexcept;

} // namespace rocks::audio::encoderdetail

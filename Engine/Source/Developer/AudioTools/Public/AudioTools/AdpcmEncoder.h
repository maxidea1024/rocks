#pragma once

#include <Audio/Adpcm.h>
#include <Audio/AudioBuffer.h>
#include <Core/Containers/Array.h>

// MS ADPCM 블록 인코더다(Docs/Design/AudioSystem 3.24.2절). UE AudioFormatADPCM의
// EncodeNibble· EncodeBlock과 같은 식이고, 블록마다 계수 세트 7개를 모두 시험해 제곱 오차가 가장
// 작은 것을 고른다(UE는 색인 0 고정). 출력 배치는 블록 묶음마다 채널을 교차한다(Audio/Adpcm.h).
// 디코드는 Audio의 DecodeAdpcmFrames다.
//
// 사용 예:
//   Array<uint8> payload(allocator);
//   if (auto encoded = EncodeAdpcm(clip.View(), payload); !encoded) { return encoded.Error(); }
namespace rocks::audio {

// float 표본 하나를 int16로 바꾼다: ×32768, round-half-even, [−32768, 32767]로 자른다. 비유한 값은
// 0이다.
ROCKS_AUDIOTOOLS_API int16 QuantizePcm16(float sample) noexcept;

// planar float 입력을 ADPCM payload로 인코드해 out에 쓴다(out은 비우고 다시 채운다).
// - 채널 1–MaxOutputChannels, frame 1 이상. 비유한 표본이 있으면 InvalidArgument.
// - out을 늘리지 못하면 OutOfMemory(out은 비어 있다).
// 같은 입력이면 같은 byte다.
ROCKS_AUDIOTOOLS_API Result<void> EncodeAdpcm(const PlanarConstView& input,
                                              Array<uint8>& out) noexcept;

} // namespace rocks::audio

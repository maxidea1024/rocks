#pragma once

#include <Audio/AudioBuffer.h>
#include <Audio/AudioDecoder.h>
#include <Core/Result.h>

// 쿠킹 리샘플러다(Docs/Design/AudioSystem 3.31절). UE ResampleWaveData와 같이 libsamplerate
// SRC_SINC_BEST_QUALITY(band-limited)를 쓴다. Opus 플랫폼 데이터(48 kHz)로 바꿀 때 쓰고, 런타임
// 리샘플러(선형)와는 다르다.
//
// 사용 예:
//   DecodedAudioPcm resampled(allocator);
//   auto result = ResampleForCooking(source.View(), 44100, 48000, allocator, resampled);
//   if (!result) { return result; }
//   auto loopStart = CookResampledPosition(sourceLoopStart, 44100, 48000);
namespace rocks::audio {

// 쿠킹 리샘플러 판이다(libsamplerate 0.2.2 SRC_SINC_BEST_QUALITY). 출력이 바뀌면 올리고, 너겟 키에
// 넣는다.
inline constexpr uint32 CookResamplerVersion = 1;

// 리샘플 뒤 frame 수 = round-half-even(frames × target / source)이다. 0이 되면 1이다.
// rate가 [8000, 192000] 밖이거나 frames가 2^32를 넘으면 InvalidArgument다.
ROCKS_AUDIOTOOLS_API Result<uint64> CookResampledFrames(uint64 frames, uint32 sourceRate,
                                                        uint32 targetRate) noexcept;

// loop·cue 위치 변환 = round-half-even(position × target / source)이다(3.11.2절). 0은 0이다.
// rate가 [8000, 192000] 밖이거나 position이 2^32를 넘으면 InvalidArgument다.
ROCKS_AUDIOTOOLS_API Result<uint64> CookResampledPosition(uint64 position, uint32 sourceRate,
                                                          uint32 targetRate) noexcept;

// in(planar, sourceRate)을 targetRate로 바꿔 out을 채운다(out은 먼저 비운다).
// - frame 수는 CookResampledFrames다. 입력 뒤에 ceil(source / target) + 1 frame의 0을 붙여
//   libsamplerate가 끝 frame까지 내게 하고, 넘치는 출력은 자른다. 필터 지연은 libsamplerate가
//   맞춘다(출력 frame 0 = 원본 시각 0). 원본 끝 뒤는 무음으로 본다(loop 경계도 같다).
// - 같은 rate면 필터 없이 그대로 복사한다.
// - 입력은 4096 frame 블록으로 나눠 넣는다(채널 수와 길이에 상관없이 int·long을 넘지 않는다).
// - rate [8000, 192000] 밖, 채널 0·9 이상, frame 0, 비유한 표본, in과 out의 기존 버퍼가 겹치면
//   InvalidArgument, 결과 PCM이 MaxSoundClipPcmBytes를 넘으면 CapacityExceeded, 할당 실패는
//   OutOfMemory, libsamplerate가 예상과 다르게 동작하면 Unsupported다. 실패하면 out을 비운다
//   (겹침 거부만 예외: out을 그대로 둔다).
// 같은 입력이면 같은 결과다(같은 아키텍처·OS, 기본 반올림 모드).
ROCKS_AUDIOTOOLS_API Result<void> ResampleForCooking(const PlanarConstView& in, uint32 sourceRate,
                                                     uint32 targetRate, Allocator& allocator,
                                                     DecodedAudioPcm& out) noexcept;

} // namespace rocks::audio

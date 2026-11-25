#pragma once

#include <Audio/AudioPlatformData.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>

// Opus 플랫폼 데이터의 bitstream 상수와 codec 머리다(Docs/Design/AudioSystem.ko.md 3.32.2절). 쿠킹
// 인코더(AudioTools)와 런타임 디코더(AudioOpus)가 같이 쓴다. libopus에 기대지 않는다.
//
// 사용 예:
//   auto parsed = ParseOpusCodecHeader(view.CodecHeader(), view.Header());
//   if (!parsed) { return parsed.Error(); }
//   const OpusCodecHeader& opus = parsed.Value();
namespace rocks::audio {

// bitstream 1: 모든 패킷이 20 ms(960 codec frame)다. 단위 = [u16 LE 길이][multistream 패킷].
inline constexpr uint32 OpusBitstreamVersion = 1;
inline constexpr uint32 OpusPacketFrames = 960;

// 단위 길이 접두 byte 수다.
inline constexpr uint32 OpusLengthPrefixBytes = 2;

// 청크 앞 복제 pre-roll이자 시작·seek 때 앞에서 디코드해 버리는 양이다(320 ms = 16 패킷,
// 3.32.4절). RFC 7845의 80 ms는 최소값이고, 20 ms CELT의 frame 사이 에너지 예측은 80 ms 뒤에도 순음
// 기준 최대 18% 어긋나 320 ms(16비트 1 LSB 미만)로 정했다.
inline constexpr uint32 OpusPreRollFrames = 15360;

// multistream 패킷 하나의 최대 byte 수다. stream은 최대 8개이고 stream마다 최대 1275 byte와 자기
// 구분 길이 2 byte다.
inline constexpr uint32 OpusMaxStreamPacketBytes = 1277;
inline constexpr uint32 OpusMaxPacketBytes = OpusMaxStreamPacketBytes * MaxOutputChannels;

// codec 머리 byte 수와 mapping family(1 = Vorbis 채널 배치)다.
inline constexpr usize OpusCodecHeaderBytes = 16;
inline constexpr uint8 OpusMappingFamilyVorbis = 1;

// codec 머리 값이다. mapping[c]는 출력 채널 c(Vorbis 순서)의 coded 채널이다.
struct OpusCodecHeader {
    uint8 streams = 0;
    uint8 coupledStreams = 0;
    uint16 preSkip = 0;
    uint8 mapping[MaxOutputChannels] = {};
};

// codec 머리 16 byte를 쓴다(family 1, 예약 0, gain 0, 채널 수 뒤 mapping 칸 0).
ROCKS_AUDIO_API void StoreOpusCodecHeader(const OpusCodecHeader& header, uint32 channels,
                                          uint8* out) noexcept;

// codec 머리를 머리(채널 수·encoder delay)와 맞춰 읽는다. 크기 ≠ 16, family ≠ 1, stream 0,
// coupled > stream, stream + coupled ≠ 채널 수, mapping 값 ≥ 채널 수, 채널 수 뒤 mapping 칸·예약 칸
// ≠ 0, gain ≠ 0, pre-skip ≠ encoder delay, 채널 수 1–8 밖이면 InvalidArgument다.
ROCKS_AUDIO_API Result<OpusCodecHeader>
ParseOpusCodecHeader(Span<const uint8> bytes, const AudioPlatformDataHeader& header) noexcept;

} // namespace rocks::audio

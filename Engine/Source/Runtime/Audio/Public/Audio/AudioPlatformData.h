#pragma once

#include <Audio/AudioFormat.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>

// 쿠킹된 오디오 플랫폼 데이터 형식이다(Docs/Design/AudioSystem 3.30.1절). byte 배치는
// [메타 = 머리 80 + codec 머리 + 청크 표 64 × 청크 수 + 16 정렬 0][payload 영역]이고 little
// endian이다. 메타와 payload 영역을 나눠 둘 수 있다(패키지에는 메타와 payload 앞부분, sidecar에는
// payload 영역).
//
// 사용 예:
//   auto view = AudioPlatformDataView::Parse(bytes);
//   if (!view) { return view.Error(); }
//   const AudioChunkEntry chunk = view.Value().Chunk(0);
//   Span<const uint8> payload = view.Value().ChunkPayload(0);   // 없으면 빈 Span
namespace rocks::audio {

// 저장되는 codec id다. 번호를 바꾸지 않는다.
enum class AudioCodecId : uint8 {
    None = 0,
    Pcm16 = 1,
    Adpcm = 2,
    Opus = 3
};

// codec id 상한(표 크기)이다. 유효한 id는 1–MaxAudioCodecs − 1이다.
inline constexpr uint32 MaxAudioCodecs = 8;

// 형식 상수다.
inline constexpr uint16 AudioPlatformDataVersion = 1;
inline constexpr usize AudioPlatformDataHeaderBytes = 80;
inline constexpr usize AudioChunkEntryBytes = 64;
inline constexpr usize AudioChunkAlignment = 16;
inline constexpr usize AudioCodecHeaderAlignment = 8;
inline constexpr usize AudioChunkHashBytes = 32;
inline constexpr uint32 AudioChunkFlagIndependent = 1;

// PCM16 단위(frame 묶음)의 frame 수와 PCM16·ADPCM의 bitstream 판이다.
inline constexpr uint32 Pcm16UnitFrames = 1024;

// PCM16 표본 byte 수와 int16 ↔ float 배율(÷ 32768)이다.
inline constexpr uint32 Pcm16SampleBytes = 2;
inline constexpr float Pcm16SampleScale = 1.0f / 32768.0f;
inline constexpr uint32 Pcm16BitstreamVersion = 1;
inline constexpr uint32 AdpcmBitstreamVersion = 1;

// Opus 플랫폼 데이터는 언제나 이 rate다.
inline constexpr uint32 OpusPlatformSampleRate = 48000;

// 압축 품질 범위와 기본값이다(3.32.1절, UE USoundWave::CompressionQuality와 같다). 1이 가장 많이
// 압축하고 100이 가장 좋은 품질이다. 쿠킹 인코더와 SoundClip 설정이 함께 쓴다.
inline constexpr uint32 MinAudioCompressionQuality = 1;
inline constexpr uint32 MaxAudioCompressionQuality = 100;
inline constexpr uint32 DefaultAudioCompressionQuality = 80;

// PCM16·ADPCM 인코딩 rate 범위(Hz)다.
inline constexpr uint32 MinPlatformSampleRate = 8000;
inline constexpr uint32 MaxPlatformSampleRate = 192000;

// 채널 마스크 bit다. data 채널 i는 i번째로 켜진 bit의 자리다(bit 순서 = 3.3절 내부 순서, BC는 끝).
inline constexpr uint32 AudioChannelFrontLeft = 1u << 0;
inline constexpr uint32 AudioChannelFrontRight = 1u << 1;
inline constexpr uint32 AudioChannelFrontCenter = 1u << 2;
inline constexpr uint32 AudioChannelLowFrequency = 1u << 3;
inline constexpr uint32 AudioChannelSideLeft = 1u << 4;
inline constexpr uint32 AudioChannelSideRight = 1u << 5;
inline constexpr uint32 AudioChannelBackLeft = 1u << 6;
inline constexpr uint32 AudioChannelBackRight = 1u << 7;
inline constexpr uint32 AudioChannelBackCenter = 1u << 8;
inline constexpr uint32 AudioChannelMaskAll = (1u << 9) - 1;

// 채널 수별 기본 마스크다(3.29.2절 표: 1 FC, 2 FL FR, 3 +FC, 4 FL FR SL SR, 5 +FC, 6 +LFE, 7 +BC,
// 8 FL FR FC LFE SL SR BL BR). 채널 수가 1–8 밖이면 0이다.
ROCKS_AUDIO_API uint32 DefaultAudioChannelMask(uint32 channels) noexcept;

// 켜진 bit 수다.
ROCKS_AUDIO_API uint32 AudioChannelMaskCount(uint32 mask) noexcept;

// 머리(메타 앞 80 byte)의 값이다.
struct AudioPlatformDataHeader {
    AudioCodecId codec = AudioCodecId::None;
    uint32 channels = 0;
    uint32 bitstreamVersion = 0;
    uint32 sampleRate = 0;
    // 유효 frame 수와 loop 구간(유효 frame 기준, loopEnd 0이면 구간 없음)이다.
    uint64 frames = 0;
    uint64 loopStart = 0;
    uint64 loopEnd = 0;
    // codec frame 시간축: 유효 frame f = codec frame (f + encoderDelay)다.
    uint32 encoderDelay = 0;
    uint32 endPadding = 0;
    uint32 chunkCount = 0;
    uint32 codecHeaderBytes = 0;
    uint32 channelMask = 0;
    uint64 payloadBytes = 0;

    // codec frame 합(delay + 유효 + padding)이다. 넘침은 검증이 막는다.
    uint64 CodecFrames() const noexcept {
        return encoderDelay + frames + endPadding;
    }
};

// 청크 표 한 줄이다.
struct AudioChunkEntry {
    uint64 payloadOffset = 0;
    uint32 payloadBytes = 0;
    uint32 flags = 0;
    // 첫 codec frame과 codec frame 수(pre-roll 제외), payload 앞에서 디코드해 버릴 pre-roll codec
    // frame 수다.
    uint64 firstFrame = 0;
    uint32 frameCount = 0;
    uint32 preRollFrames = 0;
    uint8 sha256[AudioChunkHashBytes] = {};
};

// 머리·청크 줄을 byte로 쓴다(쓰기 도구용 — 형식의 byte 배치는 이 두 함수와 Parse가 정한다).
// out은 AudioPlatformDataHeaderBytes·AudioChunkEntryBytes 이상이어야 한다.
ROCKS_AUDIO_API void StoreAudioPlatformDataHeader(const AudioPlatformDataHeader& header,
                                                  uint8* out) noexcept;
ROCKS_AUDIO_API void StoreAudioChunkEntry(const AudioChunkEntry& chunk, uint8* out) noexcept;

// 머리·codec 머리·청크 표를 합친 메타 byte 수(16 정렬)다. 넘치면 0이다.
ROCKS_AUDIO_API usize AudioPlatformDataMetaBytes(uint32 codecHeaderBytes,
                                                 uint32 chunkCount) noexcept;

// 검증한 플랫폼 데이터의 읽기 전용 보기다. bytes를 소유하지 않는다(bytes가 보기보다 오래 살아야
// 한다).
class ROCKS_AUDIO_API AudioPlatformDataView {
public:
    // bytes = 메타 + payload 영역 앞부분(없어도 된다)을 검증한다(3.30.1절 검증 목록).
    // - 형식·값 위반: InvalidArgument. codec id가 표 범위(MaxAudioCodecs) 밖이면
    //   Unsupported(범위 안이면 형식만 보고, 디코더 유무는 AudioDecoderTable이 정한다 — NotFound).
    // - codec별 검사(codec 머리, 청크 byte와 frame 관계)는 디코더가 한다.
    static Result<AudioPlatformDataView> Parse(Span<const uint8> bytes) noexcept;

    AudioPlatformDataView() noexcept = default;

    const AudioPlatformDataHeader& Header() const noexcept {
        return header_;
    }

    Span<const uint8> CodecHeader() const noexcept;

    // 청크 줄이다. index는 청크 수보다 작아야 한다(assert).
    AudioChunkEntry Chunk(uint32 index) const noexcept;

    // 청크 payload다. 주어진 bytes에 없으면 빈 Span이다.
    Span<const uint8> ChunkPayload(uint32 index) const noexcept;

    // payload 영역이 모두 있는지다.
    bool HasAllPayload() const noexcept {
        return presentPayloadBytes_ == header_.payloadBytes;
    }

    usize MetaBytes() const noexcept {
        return metaBytes_;
    }

private:
    Span<const uint8> bytes_;
    AudioPlatformDataHeader header_;
    usize metaBytes_ = 0;
    uint64 presentPayloadBytes_ = 0;
};

// 청크 payload의 SHA-256이 표와 같은지 본다. 다르거나 크기가 다르면 InvalidArgument다.
ROCKS_AUDIO_API Result<void> VerifyAudioChunk(const AudioChunkEntry& chunk,
                                              Span<const uint8> payload) noexcept;

} // namespace rocks::audio

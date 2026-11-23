#pragma once

#include <Audio/AudioBuffer.h>
#include <Audio/AudioPlatformData.h>
#include <Core/Containers/Array.h>
#include <Core/Result.h>

// 쿠킹 쪽 인코더 인터페이스와 PCM16·ADPCM 인코더다(Docs/Design/AudioSystem.ko.md 3.30.3절).
// 인코더는 codec 단위(패킷·블록 묶음) 목록을 내고,
// WriteAudioPlatformData(AudioPlatformDataWriter.h)가 청크로 나눠 플랫폼 데이터 byte를 만든다.
//
// 사용 예:
//   AdpcmAudioEncoder encoder;
//   EncodedAudio encoded(allocator);
//   const uint32 mask = DefaultAudioChannelMask(2);
//   if (auto result = encoder.Encode(clip.View(), 48000, mask, {}, encoded); !result) { ... }
namespace rocks::audio {

// codec 단위 하나(payload 안 byte 구간과 codec frame 수)다.
struct EncodedAudioUnit {
    uint64 payloadOffset = 0;
    uint32 payloadBytes = 0;
    uint32 frames = 0;
};

// 인코더 출력이다. 단위 byte는 3.30.1절 단위 framing을 따른다.
struct ROCKS_AUDIOTOOLS_API EncodedAudio {
    explicit EncodedAudio(Allocator& allocator) noexcept
        : codecHeader(allocator), payload(allocator), units(allocator) {
    }

    AudioCodecId codec = AudioCodecId::None;
    uint32 bitstreamVersion = 0;
    // 너겟 키용 인코더 판이다(머리에 넣지 않는다).
    uint32 encoderVersion = 0;
    uint32 channels = 0;
    uint32 channelMask = 0;
    uint32 sampleRate = 0;
    uint64 frames = 0;
    uint32 encoderDelay = 0;
    uint32 endPadding = 0;
    // 청크 앞에 둘 최소 pre-roll codec frame이다(Opus 80 ms, 그 밖 0).
    uint32 preRollFrames = 0;
    Array<uint8> codecHeader;
    Array<uint8> payload;
    Array<EncodedAudioUnit> units;

    // 값을 처음 상태로 둔다(배열은 비운다).
    void Reset() noexcept;
};

// 인코더 정보다.
struct AudioCodecInfo {
    AudioCodecId codec = AudioCodecId::None;
    const char* name = "";
    uint32 encoderVersion = 0;
    uint32 bitstreamVersion = 0;
    uint32 minSampleRate = 0;
    uint32 maxSampleRate = 0;
    uint32 maxChannels = 0;
};

// 인코드 설정이다. PCM16·ADPCM은 쓰지 않는다(Opus 품질은 C3).
struct AudioEncodeSettings {
    float quality = 1.0f;
};

class ROCKS_AUDIOTOOLS_API AudioEncoder {
public:
    virtual ~AudioEncoder() = default;

    virtual const AudioCodecInfo& Info() const noexcept = 0;

    // pcm(채널 수 = pcm.channelCount)을 인코드해 out을 채운다(out은 먼저 비운다). 채널 1–8 밖, 채널
    // 마스크 bit 수 불일치, rate 범위 밖, frame 0, 비유한 표본은 InvalidArgument, 할당 실패는
    // OutOfMemory다. 실패하면 out을 비운다. 같은 입력이면 같은 결과다.
    virtual Result<void> Encode(const PlanarConstView& pcm, uint32 sampleRate, uint32 channelMask,
                                const AudioEncodeSettings& settings,
                                EncodedAudio& out) noexcept = 0;
};

// PCM16 인코더: 단위 = 1024 frame 교차 int16 LE(QuantizePcm16).
class ROCKS_AUDIOTOOLS_API Pcm16AudioEncoder final : public AudioEncoder {
public:
    const AudioCodecInfo& Info() const noexcept override;
    Result<void> Encode(const PlanarConstView& pcm, uint32 sampleRate, uint32 channelMask,
                        const AudioEncodeSettings& settings, EncodedAudio& out) noexcept override;
};

// ADPCM 인코더: EncodeAdpcm 결과를 블록 묶음 단위로 나눈다.
class ROCKS_AUDIOTOOLS_API AdpcmAudioEncoder final : public AudioEncoder {
public:
    const AudioCodecInfo& Info() const noexcept override;
    Result<void> Encode(const PlanarConstView& pcm, uint32 sampleRate, uint32 channelMask,
                        const AudioEncodeSettings& settings, EncodedAudio& out) noexcept override;
};

} // namespace rocks::audio

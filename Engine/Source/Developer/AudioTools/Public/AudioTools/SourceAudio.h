#pragma once

#include <Core/Assert.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/String.h>

// 오디오 원본 파일 디코드 결과와 디코더다(Docs/Design/AudioSystem.ko.md 3.10절, 3.25–3.28절). 파일
// byte를 받아 planar float32 PCM과 loop·cue 메타데이터를 낸다. SoundClipImporter가 이 결과를
// SoundClip bulk로 옮긴다. 디코더: WAV·AIFF(자체), MP3(minimp3), FLAC(libFLAC),
// Vorbis(libvorbis).
//
// 오류 코드(임포터가 진단으로 옮긴다):
// - InvalidArgument: 필수 chunk·MPEG frame·STREAMINFO 없음, 크기·길이 불일치,
//   float 표본 NaN·무한대, FLAC CRC·MD5 불일치(InvalidSource)
// - Unsupported: 모르는 부호화, Layer I·II, 9채널 이상, rate 범위 밖, stream 중간 형식 변경
//   (UnsupportedSourceFormat)
// - CapacityExceeded: 디코드한 PCM이 MaxSoundClipPcmBytes를 넘음(할당 전에 판정)
// - OutOfMemory: 할당 실패
//
// 사용 예:
//   DecodedSourceAudio audio(allocator);
//   if (auto decoded = DecodeWav(fileBytes, audio); !decoded) { return decoded.Error(); }
//   const float* left = audio.Channel(0);
namespace rocks::audio {

// 원본 rate 범위(Hz)와 채널 상한, 디코드한 PCM 상한(byte, float32 기준)이다(3.10.1절).
inline constexpr uint32 MinSourceSampleRate = 8000;
inline constexpr uint32 MaxSourceSampleRate = 192000;
inline constexpr uint32 MaxSourceChannels = 8;
inline constexpr uint64 MaxSoundClipPcmBytes = uint64{2} * 1024 * 1024 * 1024;

// 원본 표본 형식이다. SoundClip bulk 저장 형식(int16·int24·float32)을 정하는 데 쓴다.
enum class SourceSampleFormat : uint8 {
    UInt8,
    Int16,
    Int24,
    Int32,
    Float32,
    Float64,
    // 손실 코덱 디코드 결과(float32로 저장).
    Lossy
};

// 원본 코덱이다(SoundClip sourceCodec — 세대 손실 진단).
enum class SourceCodec : uint8 {
    Pcm,
    Flac,
    Vorbis,
    Opus,
    Mp3
};

// 파일 loop다. end는 포함하지 않는다.
struct SourceLoop {
    uint64 start = 0;
    uint64 end = 0;
};

// 파일 cue다. name은 UTF-8(유효하지 않으면 빈 문자열)이다.
struct SourceCue {
    explicit SourceCue(Allocator& allocator) noexcept : name(allocator) {
    }

    uint32 id = 0;
    uint64 frame = 0;
    String name;
};

class ROCKS_AUDIOTOOLS_API DecodedSourceAudio {
public:
    explicit DecodedSourceAudio(Allocator& allocator) noexcept
        : allocator_(&allocator), pcm_(allocator), loops_(allocator), cues_(allocator) {
    }

    uint32 ChannelCount() const noexcept {
        return channels_;
    }

    uint32 SampleRate() const noexcept {
        return sampleRate_;
    }

    uint64 FrameCount() const noexcept {
        return frames_;
    }

    SourceSampleFormat SampleFormat() const noexcept {
        return sampleFormat_;
    }

    SourceCodec Codec() const noexcept {
        return codec_;
    }

    // 채널 index의 표본(frame 수만큼)이다. index는 ChannelCount()보다 작아야 한다.
    const float* Channel(uint32 index) const noexcept {
        ROCKS_ASSERT(index < channels_);
        return pcm_.Data() + static_cast<usize>(index) * frames_;
    }

    Span<const SourceLoop> Loops() const noexcept {
        return Span<const SourceLoop>(loops_.Data(), loops_.Size());
    }

    Span<const SourceCue> Cues() const noexcept {
        return Span<const SourceCue>(cues_.Data(), cues_.Size());
    }

    // 건너뛴 모르는 chunk 수다(임포터의 IgnoredSourceChunks 경고).
    uint32 IgnoredChunks() const noexcept {
        return ignoredChunks_;
    }

    // 결과 할당에 쓰는 allocator다(디코더가 임시 버퍼에도 쓴다).
    Allocator& GetAllocator() noexcept {
        return *allocator_;
    }

    // 디코더 구현이 채운다.
    void Reset() noexcept;
    Result<float*> TryAllocatePcm(uint32 channels, uint32 sampleRate, uint64 frames,
                                  SourceSampleFormat format, SourceCodec codec) noexcept;
    Result<void> TryAddLoop(SourceLoop loop) noexcept;
    Result<void> TryAddCue(uint32 id, uint64 frame) noexcept;
    // id가 같은 cue에 이름을 붙인다(없으면 아무 일도 하지 않는다). 유효하지 않은 UTF-8은 빈
    // 이름으로 둔다.
    Result<void> TryNameCue(uint32 id, StringView name) noexcept;
    void CountIgnoredChunk() noexcept {
        ++ignoredChunks_;
    }

private:
    Allocator* allocator_;
    Array<float> pcm_;
    Array<SourceLoop> loops_;
    Array<SourceCue> cues_;
    uint32 channels_ = 0;
    uint32 sampleRate_ = 0;
    uint64 frames_ = 0;
    SourceSampleFormat sampleFormat_ = SourceSampleFormat::Int16;
    SourceCodec codec_ = SourceCodec::Pcm;
    uint32 ignoredChunks_ = 0;
};

// RIFF/RIFX WAVE를 디코드한다(3.25.2절). out은 먼저 비우고, 실패하면 다시 비운다.
ROCKS_AUDIOTOOLS_API Result<void> DecodeWav(Span<const uint8> file,
                                            DecodedSourceAudio& out) noexcept;

// AIFF·AIFF-C를 디코드한다(3.25.3절). out은 먼저 비우고, 실패하면 다시 비운다.
ROCKS_AUDIOTOOLS_API Result<void> DecodeAiff(Span<const uint8> file,
                                             DecodedSourceAudio& out) noexcept;

// FLAC(native, Ogg FLAC 제외)을 디코드한다(3.27절). STREAMINFO에 길이·MD5가 있으면 맞춰 보고, frame
// CRC 불일치·머리 오류·frame 사이 손상(sync를 잃은 뒤 이어지는 audio)·frame 번호 끊김은
// InvalidArgument다 (마지막 frame 뒤 쓰레기는 받는다). 형식이 frame마다 바뀌면 Unsupported다. 표본
// 형식은 bit 수(≤16 Int16, ≤24 Int24, 그 위 Int32), 코덱은 Flac이다. out은 먼저 비우고, 실패하면
// 다시 비운다.
ROCKS_AUDIOTOOLS_API Result<void> DecodeFlac(Span<const uint8> file,
                                             DecodedSourceAudio& out) noexcept;

// Ogg Vorbis를 디코드한다(3.28절). 디코드 전에 page 구조(CRC, BOS·EOS, page 번호 연속)를 확인하므로
// page 손상·누락·잘림은 InvalidArgument다. 길이는 granule 기준(encoder 앞뒤 자르기 반영)이다.
// 연결된 stream의 link마다 채널·rate가 같아야 하고(다르면 Unsupported), 다중화된 stream도
// Unsupported다. 표본 형식은 Lossy, 코덱은 Vorbis다. out은 먼저 비우고, 실패하면 다시 비운다.
ROCKS_AUDIOTOOLS_API Result<void> DecodeVorbis(Span<const uint8> file,
                                               DecodedSourceAudio& out) noexcept;

// MPEG-1·2·2.5 Layer III(MP3)를 디코드한다(3.26절). ID3v2·ID3v1·APEv2 tag는 건너뛰고,
// Xing/Info(LAME) tag가 있으면 encoder delay·padding을 잘라 원본 길이로 맞춘다. 채널·rate가 중간에
// 바뀌거나 Layer I·II면 Unsupported, MPEG frame이 없으면 InvalidArgument다. 표본 형식은 Lossy,
// 코덱은 Mp3다. out은 먼저 비우고, 실패하면 다시 비운다.
ROCKS_AUDIOTOOLS_API Result<void> DecodeMp3(Span<const uint8> file,
                                            DecodedSourceAudio& out) noexcept;

} // namespace rocks::audio

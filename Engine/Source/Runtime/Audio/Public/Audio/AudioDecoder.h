#pragma once

#include <Audio/AudioBuffer.h>
#include <Audio/AudioPlatformData.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>

// 오디오 디코더 인터페이스와 codec별 디코더 표다(Docs/Design/AudioSystem.ko.md 3.30.2절).
// Audio(Runtime)는 codec 구현 모듈(AudioOpus 등)을 링크하지 않고 이 표로만 찾는다. 내장
// PCM16·ADPCM은 RegisterBuiltInAudioDecoders가 넣고, codec 모듈은 시작 때 HostServices에 factory를
// 등록해 표를 만드는 쪽이 넣는다.
//
// 사용 예:
//   AudioDecoderTable table;
//   RegisterBuiltInAudioDecoders(table);
//   auto decoder = table.Create(view.Header().codec, allocator);
//   decoder.Value()->Open(view.Header(), view.CodecHeader());
//   decoder.Value()->BeginChunk(chunk, payload, chunk.firstFrame);
//   while (true) {
//       auto written = decoder.Value()->DecodeNext(out);
//       if (!written || written.Value() == 0) { break; }
//   }
namespace rocks::audio {

// 청크 단위로 디코드하는 디코더다. 한 인스턴스는 한 번에 한 작업만 한다(디코드 worker 하나가 쓴다).
// Open이 상태·scratch를 모두 잡고, BeginChunk·DecodeNext는 할당하지 않는다.
class ROCKS_AUDIO_API AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    // 머리와 codec 머리를 확인한다(codec·채널 1–8·rate·bitstream 판). 다르면 InvalidArgument(판은
    // Unsupported)다. 실패하면 앞 상태(열린 청크)를 남기지 않는다.
    virtual Result<void> Open(const AudioPlatformDataHeader& header,
                              Span<const uint8> codecHeader) noexcept = 0;

    // 청크를 startCodecFrame(∈ [chunk.firstFrame, 끝))부터 디코드할 준비를 한다. 상태를 비우고
    // payload 단위 경계·크기를 먼저 검사하며(손상은 InvalidArgument), pre-roll과 startCodecFrame 앞
    // frame은 디코드해 버린다. payload는 청크를 다 낼 때까지 살아 있어야 한다.
    virtual Result<void> BeginChunk(const AudioChunkEntry& chunk, Span<const uint8> payload,
                                    uint64 startCodecFrame) noexcept = 0;

    // 다음 단위를 out에 planar로 쓴다(시작 단위는 startCodecFrame부터의 나머지). 쓴 codec frame 수,
    // 청크 끝이면 0이다. out 채널 수가 다르거나 frame 용량이 MaxFramesPerCall보다 작거나
    // Open·BeginChunk 전이면 InvalidArgument다.
    virtual Result<uint32> DecodeNext(const PlanarView& out) noexcept = 0;

    // DecodeNext 한 번이 쓸 수 있는 최대 frame 수다(PCM16 1024, ADPCM 1012, Opus 5760).
    virtual uint32 MaxFramesPerCall() const noexcept = 0;
};

// 디코더를 만드는 함수다. 할당 실패는 OutOfMemory다.
using AudioDecoderFactory = Result<UniquePtr<AudioDecoder>> (*)(Allocator& allocator) noexcept;

// codec id별 디코더 factory 표다. 전역 표는 없다 — 소유자(AudioEngineSubsystem, 도구, 시험)가
// 만든다. 제어 쪽에서만 쓴다(스레드 안전하지 않다).
class ROCKS_AUDIO_API AudioDecoderTable {
public:
    // codec id에 factory를 넣는다. id가 None이거나 범위 밖이거나 factory가 null이면
    // InvalidArgument, 이미 있으면 AlreadyExists다.
    Result<void> Register(AudioCodecId codec, AudioDecoderFactory factory) noexcept;

    // codec id의 디코더를 만든다. 없으면 NotFound, factory 실패는 그 오류다.
    Result<UniquePtr<AudioDecoder>> Create(AudioCodecId codec, Allocator& allocator) const noexcept;

    bool Contains(AudioCodecId codec) const noexcept;

private:
    AudioDecoderFactory factories_[MaxAudioCodecs] = {};
};

// 내장 디코더(PCM16·ADPCM)를 넣는다. 이미 있으면 AlreadyExists다.
ROCKS_AUDIO_API Result<void> RegisterBuiltInAudioDecoders(AudioDecoderTable& table) noexcept;

// 전체 디코드 결과(유효 frame만, planar float)다.
class ROCKS_AUDIO_API DecodedAudioPcm {
public:
    explicit DecodedAudioPcm(Allocator& allocator) noexcept : samples_(allocator) {
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

    // 채널 index의 표본이다. index는 채널 수보다 작아야 한다.
    const float* Channel(uint32 index) const noexcept;

    // DecodeAudioPlatformData가 채운다.
    Result<float*> TryAllocate(uint32 channels, uint32 sampleRate, uint64 frames) noexcept;
    void Reset() noexcept;

private:
    Array<float> samples_;
    uint32 channels_ = 0;
    uint32 sampleRate_ = 0;
    uint64 frames_ = 0;
};

// 플랫폼 데이터 전체를 디코드한다(에디터 미리듣기·도구·시험용, AudioRT는 쓰지 않는다). 할당 전에
// 모든 청크의 SHA와 단위 경계를 먼저 확인하고, delay·padding을 잘라 유효 frame만 남긴다. 실패하면
// out을 비운다. 오류: payload가 다 없거나 청크 손상 InvalidArgument, 디코더가 표에 없음 NotFound,
// bitstream 판을 모름 Unsupported, 결과가 주소 공간을 넘음 CapacityExceeded, 할당 실패 OutOfMemory.
ROCKS_AUDIO_API Result<void> DecodeAudioPlatformData(const AudioPlatformDataView& view,
                                                     const AudioDecoderTable& table,
                                                     Allocator& allocator,
                                                     DecodedAudioPcm& out) noexcept;

} // namespace rocks::audio

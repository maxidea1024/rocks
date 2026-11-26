#pragma once

#include <Audio/AudioBuffer.h>
#include <Audio/AudioPlatformData.h>
#include <Core/Module/HostServiceKey.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>

// 오디오 디코더 인터페이스와 codec별 디코더 표다(Docs/Design/AudioSystem 3.30.2절).
// Audio(Runtime)는 codec 구현 모듈(AudioOpus 등)을 링크하지 않고 이 표로만 찾는다. 내장
// PCM16·ADPCM은 RegisterBuiltInAudioDecoders가 넣고, codec 모듈은 시작 때 HostServices에 factory를
// 등록해 표를 만드는 쪽이 넣는다.
//
// 사용 예:
//   AudioDecoderTable table;
//   RegisterBuiltInAudioDecoders(table);
//   auto decoder = table.Create(view.Header().codec, allocator);
//   decoder.Value()->Open(view.Header(), view.CodecHeader());
//   decoder.Value()->BeginChunk(chunk, payload, chunk.firstFrame);   // 시작·seek: 상태를 비운다
//   // ... 청크를 끝까지 낸 뒤 다음 청크는 ContinueChunk(next, nextPayload)로 상태를 잇는다.
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

    // 머리와 codec 머리를 확인한다. 머리 codec이 이 디코더가 아니면 InvalidArgument(호출자 오용),
    // bitstream 판을 모르면 Unsupported(codec 다음, 다른 규칙보다 먼저 본다), 채널 1–8·rate·codec
    // 머리 규칙 위반은 CorruptData다(3.38절). 실패하면 앞 상태(열린 청크)를 남기지 않는다.
    virtual Result<void> Open(const AudioPlatformDataHeader& header,
                              Span<const uint8> codecHeader) noexcept = 0;

    // 청크를 startCodecFrame(∈ [chunk.firstFrame, 끝))부터 디코드할 준비를 한다. 상태를 비우고
    // payload 단위 경계·크기를 먼저 검사하며(손상은 CorruptData), pre-roll과 startCodecFrame 앞
    // frame은 디코드해 버린다. Open 전, payload 길이 ≠ chunk.payloadBytes, startCodecFrame이 청크
    // 밖이면 InvalidArgument다(호출자 오용). payload는 청크를 다 낼 때까지 살아 있어야 한다.
    virtual Result<void> BeginChunk(const AudioChunkEntry& chunk, Span<const uint8> payload,
                                    uint64 startCodecFrame) noexcept = 0;

    // 앞 청크를 끝까지 낸 뒤(DecodeNext가 0) 바로 다음 청크(첫 frame = 앞 청크 끝)를 상태를 이어
    // 연다. 복제 pre-roll은 디코드하지 않고 건너뛴다. 그래서 처음부터 순차로 낸 결과는 연속
    // 디코드와 같다(3.30.2절 청크 경계 규칙). payload 검사는 BeginChunk와 같다. 열린 청크가 없거나,
    // 앞 청크를 다 내지 않았거나, 이어지지 않으면 InvalidArgument이고 열린 청크도 닫는다. 모든
    // codec이 이 순서 규칙을 지킨다(청크 사이 상태가 없는 PCM16·ADPCM도 같다). 그래서 호출 순서
    // 오류가 codec에 따라 숨지 않는다.
    virtual Result<void> ContinueChunk(const AudioChunkEntry& chunk,
                                       Span<const uint8> payload) noexcept = 0;

    // 다음 단위를 out에 planar로 쓴다(시작 단위는 startCodecFrame부터의 나머지). 쓴 codec frame 수,
    // 청크 끝이면 0이다. out 채널 수가 다르거나 frame 용량이 MaxFramesPerCall보다 작거나
    // Open·BeginChunk 전이면 InvalidArgument, 단위 디코드가 실패하면(손상) CorruptData다.
    virtual Result<uint32> DecodeNext(const PlanarView& out) noexcept = 0;

    // DecodeNext 한 번이 쓸 수 있는 최대 frame 수다(PCM16 1024, ADPCM 1012,
    // Opus 960(bitstream 1)).
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

// 호스트가 HostServices에 올리는 codec 모듈 디코더 등록부다(Docs/Design/AudioSystem 3.37.1절).
// Audio(Runtime)는 ClientOnly codec 모듈(AudioOpus)을 링크하지 않으므로 그 모듈의 Startup이 여기에
// factory를 넣고, 표를 만드는 쪽(AudioEngineSubsystem·도구)이 Fill로 옮긴다. 출력 backend
// 등록부(AudioOutputBackendRegistry)와 같은 방식이다. 등록·조회는 로더 스레드에서 한다.
//
// 사용 예(codec 모듈 Startup):
//   if (auto* registry = context.Services().Find<AudioDecoderRegistry>()) {
//       registry->SetFactory(AudioCodecId::Opus, &CreateOpusAudioDecoder);
//   }
class AudioDecoderRegistry final {
public:
    // codec의 factory를 바꾼다(nullptr이면 지운다). None·범위 밖 codec은 무시한다. 모듈 Shutdown은
    // 자기 factory를 지운다.
    void SetFactory(AudioCodecId codec, AudioDecoderFactory factory) noexcept {
        const uint32 index = static_cast<uint32>(codec);
        if (codec != AudioCodecId::None && index < MaxAudioCodecs) {
            factories_[index] = factory;
        }
    }

    AudioDecoderFactory Factory(AudioCodecId codec) const noexcept {
        const uint32 index = static_cast<uint32>(codec);
        return index < MaxAudioCodecs ? factories_[index] : nullptr;
    }

    // 등록된 factory를 table에 넣는다. table에 이미 있는 codec은 그대로 둔다(내장 디코더가
    // 앞선다). 오류는 없다(넣을 수 있는 것만 넣는다).
    void Fill(AudioDecoderTable& table) const noexcept {
        for (uint32 index = 1; index < MaxAudioCodecs; ++index) {
            const auto codec = static_cast<AudioCodecId>(index);
            if (factories_[index] != nullptr && !table.Contains(codec)) {
                (void)table.Register(codec, factories_[index]);
            }
        }
    }

private:
    AudioDecoderFactory factories_[MaxAudioCodecs] = {};
};

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
// out을 비운다. 오류: payload가 다 없는 보기 InvalidArgument(호출자 오용), 청크 SHA·크기·단위
// 손상과 디코더가 청크 frame 수와 다르게 냄 CorruptData, 디코더가 표에 없음 NotFound,
// bitstream 판을 모름 Unsupported, 결과가 주소 공간을 넘음 CapacityExceeded, 할당 실패 OutOfMemory.
ROCKS_AUDIO_API Result<void> DecodeAudioPlatformData(const AudioPlatformDataView& view,
                                                     const AudioDecoderTable& table,
                                                     Allocator& allocator,
                                                     DecodedAudioPcm& out) noexcept;

} // namespace rocks::audio

ROCKS_DECLARE_HOST_SERVICE(rocks::audio::AudioDecoderRegistry, ROCKS_AUDIO_API)

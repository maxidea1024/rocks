#pragma once

#include <Audio/AudioDecoder.h>
#include <AudioAssets/SoundClipData.h>
#include <Base/Compiler.h>
#include <AudioAssets/SoundPcm.h>
#include <AudioAssets/SoundSource.h>
#include <Core/Containers/Array.h>

// PCM 사운드 에셋이다(Docs/Design/AudioSystem 3.9·3.36.2·3.37.2절, UE USoundWave 대응). 필드는
// 넷이다.
// - 임포트 결과(channelCount·sampleRate·frameCount·sourceCodec·파일 loop·cues): 임포터가 매번 전부
//   쓴다.
// - 사용자 설정(looping·loopOverride·compression·compressionQuality, SoundSource의 volume·pitch·
//   attenuation): 임포터가 건드리지 않는다.
// - 에디터 원본(source — EditorOnly): 디코드한 PCM bulk다. 쿠킹 패키지에 넣지 않는다.
// - 쿠킹 결과(platformData): 쿠킹 처리기가 쓰는 플랫폼 데이터 bulk다. 에디터 패키지에서는 비어
//   있다. 쿠킹 패키지의 sampleRate·frameCount·loop·cue는 인코딩 rate 기준이다(3.37.3절).
// 재생 PCM(pcm_)은 실행 데이터다. 시험·Sandbox는 SetPcm으로 메모리에서 만들고, 에디터·Tryout은
// LoadEditorPcm으로 원본 bulk에서, 쿠킹된 실행은 LoadCookedPcm으로 플랫폼 데이터에서 만든다(AD25 —
// 3.36.4·3.37.4절). 청크 스트리밍은 AU6이다.
//
// 사용 예(에디터 재생):
//   (void)clip.LoadEditorPcm(allocator, bulkSource);   // 같은 원본이면 다시 올리지 않는다
//   auto sound = clip.MakePlayable(allocator, registry, registry.OwnerAccess());
//
// 사용 예(쿠킹된 실행 — reader는 Package::BulkBytes를 부른다):
//   (void)clip.LoadCookedPcm(allocator, &ReadPackageBulk, &package, audio->DecoderTable());
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다.
namespace rocks {

class PackageBulkSource;

// 쿠킹 플랫폼 데이터 bulk를 읽는 함수다(WorldIntegration PlatformBulkReader와 같은 모양 — 호출자가
// 그 패키지의 Package::BulkBytes를 넘긴다). 돌려준 byte는 LoadCookedPcm이 끝날 때까지 살아야 한다.
using SoundBulkReader = Result<Span<const uint8>> (*)(void* context,
                                                      const BulkDataRef& ref) noexcept;

// 재생 PCM의 원본 종류다(SoundPcmKey).
enum class SoundPcmOrigin : uint8 {
    // 올린 PCM이 없거나 SetPcm으로 메모리에서 만들었다.
    None = 0,
    // 에디터 원본 bulk(LoadEditorPcm).
    Editor = 1,
    // 쿠킹 플랫폼 데이터(LoadCookedPcm).
    Cooked = 2
};

// 마지막으로 올린 재생 PCM의 키다(재사용 판정 — 3.36.4·3.37.4절). 리플렉션 밖 실행 데이터다. 에디터
// 원본과 쿠킹 데이터가 함께 쓴다(쿠킹은 format이 None이다).
struct SoundPcmKey {
    SoundPcmOrigin origin = SoundPcmOrigin::None;
    BulkDataRef bulk;
    SoundSourceStorage format = SoundSourceStorage::None;
    uint32 channels = 0;
    uint32 sampleRate = 0;
    uint32 frames = 0;
};

// 원본 bulk byte(SoundClipSource 저장 규칙 — planar LE, 채널마다 이어 담음)를 재생 PCM으로 푼다.
// 에디터 재생(LoadEditorPcm)과 쿠킹 처리기가 같은 함수를 쓴다(3.37.3절 2번). 오류: 채널이
// MaxClipChannels보다 많음 Unsupported, 형식 None·채널 0·frame 0·byte 수가 형식·채널·frame과 다름·
// rate가 SoundPcm 범위 밖·비유한 float(손상) CorruptData, 할당 실패 OutOfMemory.
ROCKS_AUDIOASSETS_API Result<SharedPtr<SoundPcm>>
DecodeSoundClipSource(Allocator& allocator, SoundSourceStorage format, uint32 channels,
                      uint32 sampleRate, uint32 frames, Span<const uint8> bytes) noexcept;

ROCKS_CLASS()
class ROCKS_AUDIOASSETS_API SoundClip final : public SoundSource {
    ROCKS_GENERATED_BODY()

public:
    explicit SoundClip(Allocator& allocator) noexcept : SoundSource(allocator), cues(allocator) {
    }

    // --- 사용자 설정 ---

    // loop 재생이다(임포터는 켜지 않는다 — UE bLooping과 같다).
    ROCKS_PROPERTY() bool looping = false;
    // 사용자 loop 구간이다(enabled면 파일 loop보다 앞선다).
    ROCKS_PROPERTY() SoundLoopOverride loopOverride;
    // 쿠킹 압축 형식이다(PlatformDefault면 길이로 ADPCM·Opus를 고른다 — 3.37.2절).
    ROCKS_PROPERTY() SoundCompression compression = SoundCompression::PlatformDefault;
    // 압축 품질 [MinAudioCompressionQuality, MaxAudioCompressionQuality]다(UE CompressionQuality —
    // Opus만 쓴다, 범위 밖이면 쿠킹이 InvalidSetting으로 실패한다).
    ROCKS_PROPERTY() uint32 compressionQuality = audio::DefaultAudioCompressionQuality;

    // --- 임포트 결과 ---

    ROCKS_PROPERTY() uint32 channelCount = 0;
    ROCKS_PROPERTY() uint32 sampleRate = 0;
    ROCKS_PROPERTY() uint32 frameCount = 0;
    ROCKS_PROPERTY() SoundSourceCodec sourceCodec = SoundSourceCodec::Pcm;
    // 파일 loop(첫 `smpl` loop)다. 구간은 [begin, end) frame이다.
    ROCKS_PROPERTY() bool hasFileLoop = false;
    ROCKS_PROPERTY() uint32 fileLoopBegin = 0;
    ROCKS_PROPERTY() uint32 fileLoopEnd = 0;
    ROCKS_PROPERTY() Array<SoundCuePoint> cues;

    // --- 쿠킹 결과 ---

    // 쿠킹 플랫폼 데이터(3.30.1절 형식)다. 에디터 패키지에서는 비어 있다.
    ROCKS_PROPERTY() BulkDataRef platformData;

#if ROCKS_WITH_EDITOR_DATA
    // --- 에디터 원본 ---
    ROCKS_PROPERTY(EditorOnly) SoundClipSource source;
#endif

    // 재생 loop 구간이다: loopOverride.enabled면 그 구간, 아니면 hasFileLoop면 파일 구간, 둘 다
    // 아니면 (0, 0) = clip 전체.
    void EffectiveLoop(uint32& begin, uint32& end) const noexcept;

    // 재생 PCM을 바꾸고 channelCount·sampleRate·frameCount를 그 PCM으로 채운다(메모리 생성 —
    // 시험·Sandbox). 이미 시작한 재생은 이전 PCM을 끝까지 쓴다(스냅숏). 비유한 표본이 있으면
    // InvalidArgument다. nullptr은 지움이다(형식 필드는 0이 된다).
    Result<void> SetPcm(SharedPtr<const SoundPcm> pcm) noexcept;

#if ROCKS_WITH_EDITOR_DATA
    // 에디터 원본 bulk로 재생 PCM을 만든다(AD25 — 3.36.4절). 키(원본 내용·형식·채널·rate·frame)가
    // 마지막으로 올린 것과 같으면 아무것도 하지 않는다. 다르면 새 PCM으로 바꾸고 PcmRevision을
    // 올린다. 오류: 원본 없음 NotReady, DecodeSoundClipSource의 오류(손상 CorruptData, 채널이 2보다
    // 많음 Unsupported — 다채널은 AU9), bulk 읽기 오류, 할당 실패. 게임 스레드에서 동기로 읽는다.
    Result<void> LoadEditorPcm(Allocator& allocator, PackageBulkSource& bulk) noexcept;
#endif

    // 쿠킹 플랫폼 데이터 전체를 디코드해 재생 PCM을 만든다(3.37.4절). 키(platformData 내용·채널·
    // rate·frame)가 마지막으로 올린 것과 같으면 아무것도 하지 않는다. 다르면 새 PCM으로 바꾸고
    // PcmRevision을 올린다. 오류:
    // - platformData 없음·reader null: NotReady.
    // - 채널이 MaxClipChannels보다 많음: Unsupported(AU9).
    // - 형식·청크 손상, payload 모자람(잘림), 머리(채널·rate·frame)와 필드 불일치, 디코드 결과
    //   차원이 머리와 다름: CorruptData.
    // - decoders에 그 codec 디코더가 없음: NotFound(AudioOpus를 링크하지 않은 실행).
    // - 형식 판·codec id·bitstream 판을 모름: Unsupported(새 쿠커의 데이터 — 손상이 아니다).
    // - reader 오류, 할당 실패.
    // 하부(Parse·디코더)의 오류 값은 그대로 넘긴다(3.38절).
    // 실패하면 앞 PCM을 그대로 둔다. 게임 스레드에서 동기로 디코드한다(긴 clip은 로드 때 디코드
    // 비용이 든다 — 청크 스트리밍은 AU6).
    Result<void> LoadCookedPcm(Allocator& allocator, SoundBulkReader reader, void* context,
                               const audio::AudioDecoderTable& decoders) noexcept;

    const SharedPtr<const SoundPcm>& Pcm() const noexcept {
        return pcm_;
    }

    // 재생 PCM을 바꿀 때마다 올라간다(진단 — 스냅숏의 revision).
    uint64 PcmRevision() const noexcept {
        return pcmRevision_;
    }

protected:
    // PCM이 없거나 PCM 차원이 형식 필드와 다르면(재임포트 직후 LoadEditorPcm 전) NotReady다.
    Result<void> FillPlayable(SoundPlayable& playable) const noexcept override;

private:
    ROCKS_NATIVE() SharedPtr<const SoundPcm> pcm_;
    ROCKS_NATIVE() uint64 pcmRevision_ = 0;
    // 마지막으로 올린 PCM의 키다(SetPcm이 지운다). 쿠킹 데이터도 쓰므로 에디터 데이터 없는
    // 빌드에서도 읽힌다.
    ROCKS_NATIVE() SoundPcmKey pcmKey_;
};

} // namespace rocks

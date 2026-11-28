#pragma once

#include <Audio/AudioDecoder.h>
#include <AudioAssets/SoundClipData.h>
#include <Base/Compiler.h>
#include <AudioAssets/SoundPcm.h>
#include <AudioAssets/SoundSource.h>
#include <AudioAssets/SoundStreamData.h>
#include <Core/Containers/Array.h>

// PCM 사운드 에셋이다(Docs/Design/AudioSystem 3.9·3.36.2·3.37.2절, UE USoundWave 대응). 필드는
// 넷이다.
// - 임포트 결과(channelCount·sampleRate·frameCount·sourceCodec·파일 loop·cues): 임포터가 매번 전부
//   쓴다.
// - 사용자 설정(looping·loopOverride·compression·compressionQuality·sampleRateQuality,
// SoundSource의
//   volume·pitch·attenuation): 임포터가 건드리지 않는다.
// - 에디터 원본(source — EditorOnly): 디코드한 PCM bulk다. 쿠킹 패키지에 넣지 않는다.
// - 쿠킹 결과(platformData): 쿠킹 처리기가 쓰는 플랫폼 데이터 bulk다. 에디터 패키지에서는 비어
//   있다. 쿠킹 패키지의 sampleRate·frameCount·loop·cue는 인코딩 rate 기준이다(3.37.3절).
// 재생 원본(pcm_ 또는 stream_)은 실행 데이터다. 시험·Sandbox는 SetPcm으로 메모리에서 만들고,
// 에디터·Tryout은 LoadEditorPcm으로 원본 bulk에서, 쿠킹된 실행은 LoadCooked로 플랫폼 데이터에서
// 만든다 (AD25 — 3.36.4·3.37.4·3.40.3절). LoadCooked는 상주 압축 실시간 디코드(LoadCookedStream)를
// 쓰고, 1 s 미만 loop만 통째 디코드(LoadCookedPcm)한다. 스트리밍 clip의 payload는 sidecar
// (streamedPayload)에 있고 엔진 로드 도우미(AudioEngineSubsystem::LoadCookedClip)가 연다(AU6b —
// 3.46절).
//
// 사용 예(에디터 재생):
//   (void)clip.LoadEditorPcm(allocator, bulkSource);   // 같은 원본이면 다시 올리지 않는다
//   auto sound = clip.MakePlayable(allocator, registry, registry.OwnerAccess());
//
// 사용 예(쿠킹된 실행 — 엔진 로드 도우미가 sidecar를 열고 LoadCooked를 부른다):
//   (void)audio->LoadCookedClip(clip, package, assets, lease);
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
    // 쿠킹 플랫폼 데이터를 통째로 푼 PCM(LoadCookedPcm).
    Cooked = 2,
    // 쿠킹 플랫폼 데이터의 상주 압축 실시간 디코드(LoadCookedStream).
    CookedStream = 3
};

// loop 길이가 이 초보다 짧으면 로드 때 통째로 푼다(3.40.2절 — 바퀴마다 디코더를 다시 여는 비용
// 상한).
inline constexpr uint32 StreamMinLoopSeconds = 1;

// LoadCooked의 엔진 설정 값이다(AudioEngineSubsystem::CookedLoadOptions가 CVar에서 채운다).
struct SoundCookedLoadOptions {
    // loadingBehavior가 Inherited일 때 쓰는 값이다. UE처럼 RetainOnLoad–LoadOnDemand로 clamp한다
    // (ForceInline은 clip에만 둘 수 있다 — UE GetDefaultLoadingBehaviorCVar).
    SoundLoadingBehavior defaultLoading = SoundLoadingBehavior::LoadOnDemand;
    // clip이 Inherited일 때 분류 사슬이 정한 값이다(ChainLoadingBehavior — 3.12절, Inherited면
    // defaultLoading으로 간다). 호출자가 사운드의 분류 사슬에서 채운다.
    SoundLoadingBehavior categoryLoading = SoundLoadingBehavior::Inherited;
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
    // 플랫폼 rate 등급이다(UE SampleRateQuality — 대상 프로파일이 resampleForDevice를 켤 때만
    // PCM16· ADPCM 인코딩 rate를 낮춘다, 3.39절).
    ROCKS_PROPERTY() SoundSampleRateQuality sampleRateQuality = SoundSampleRateQuality::Max;
    // 로딩 동작이다(UE LoadingBehavior — 3.40.3절). 쿠킹된 실행의 LoadCooked가 디코드 방식을 고를
    // 때 쓴다.
    ROCKS_PROPERTY() SoundLoadingBehavior loadingBehavior = SoundLoadingBehavior::Inherited;

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

    // 쿠킹 플랫폼 데이터(3.30.1절 형식)다. 에디터 패키지에서는 비어 있다. 스트리밍 clip이면 메타 +
    // 청크 0(플랫폼 데이터 앞부분)이다(3.46.2절).
    ROCKS_PROPERTY() BulkDataRef platformData;
    // 스트리밍 clip의 payload 영역 전체(sidecar bulk — 3.46.2절)다. 패키지만인 clip·에디터
    // 패키지에서는 비어 있다. sidecar 출력이 없는 쿠킹(서버 프로필)에서는 패키지 안 bulk다.
    ROCKS_PROPERTY() BulkDataRef streamedPayload;

#if ROCKS_WITH_EDITOR_DATA
    // --- 에디터 원본 ---
    ROCKS_PROPERTY(EditorOnly) SoundClipSource source;
#endif

    // 재생 loop 구간이다: loopOverride.enabled면 그 구간, 아니면 hasFileLoop면 파일 구간, 둘 다
    // 아니면 (0, 0) = clip 전체.
    void EffectiveLoop(uint32& begin, uint32& end) const noexcept;

    // looping이고 재생 loop 길이가 sampleRate × StreamMinLoopSeconds보다 짧으면 true다(로드 때 통째
    // 디코드 — 바퀴마다 디코더를 다시 여는 비용 상한, 3.40.2절).
    bool UsesWholeDecode() const noexcept;

    // 통째 디코드 판정 식이다: looping이고 loop 길이(begin·end가 0·0이면 frames)가 sampleRate ×
    // StreamMinLoopSeconds보다 짧다. 쿠킹은 쿠킹 값(인코딩 rate·옮긴 loop)으로 같은 식을 불러
    // sidecar 여부를 정한다(3.46.2절 — 쿠킹과 런타임 판정이 같은 입력을 본다).
    static bool IsWholeDecodeLoop(bool looping, uint32 loopBegin, uint32 loopEnd, uint32 frames,
                                  uint32 sampleRate) noexcept;

    // 쿠킹이 payload를 sidecar에 둘 수 있는 clip이다(ForceInline·통째 디코드가 아님 — 3.46.2절).
    // 청크가 하나뿐이면 쿠킹이 패키지에만 둔다.
    bool StreamsFromSidecar() const noexcept {
        return loadingBehavior != SoundLoadingBehavior::ForceInline && !UsesWholeDecode();
    }

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
    // 비용이 든다 — 청크 스트리밍은 AU6). streamedPayload가 있으면 그 bulk가 패키지 안이어야 하고
    // (sidecar 출력이 없는 쿠킹) platformData의 메타 뒤에 이어 읽는다. sidecar면 reader 오류다
    // (Package::BulkBytes는 Unsupported).
    Result<void> LoadCookedPcm(Allocator& allocator, SoundBulkReader reader, void* context,
                               const audio::AudioDecoderTable& decoders) noexcept;

    // 쿠킹 플랫폼 데이터 byte를 공유 불변 SoundStreamData로 복사해 실시간 디코드 원본으로 둔다
    // (3.40.3절 — 재생 PCM은 만들지 않는다). 키(platformData 내용·채널·rate·frame)가 같으면
    // 아무것도 하지 않는다. 오류: platformData 없음·reader null NotReady, 채널이
    // MaxClipChannels보다 많음 Unsupported, 형식·청크 손상(모든 청크 SHA를 여기서 본다)·잘림·머리와
    // 필드 불일치 CorruptData, 모르는 형식 판·codec Unsupported, decoders에 그 codec 디코더가 없음
    // NotFound(LoadCookedPcm과 같다 — 재생 때마다 실패하지 않게 로드 때 거부한다), reader 오류,
    // 할당 실패. 실패하면 앞 원본을 그대로 둔다. streamedPayload는 LoadCookedPcm과 같이 읽는다.
    Result<void> LoadCookedStream(Allocator& allocator, SoundBulkReader reader, void* context,
                                  const audio::AudioDecoderTable& decoders) noexcept;

    // 실제 로딩 동작이다: loadingBehavior, Inherited면 options.defaultLoading을 RetainOnLoad–
    // LoadOnDemand로 clamp한 값(UE 우선순위에서 SoundClass 단계는 AU7에서 사이에 들어간다).
    SoundLoadingBehavior
    EffectiveLoadingBehavior(const SoundCookedLoadOptions& options) const noexcept;

    // 쿠킹 데이터를 올린다(3.40.3·3.46.3절). 경로:
    // - streamed가 null이면 상주 경로다: 통째 디코드 대상(UsesWholeDecode)이면 LoadCookedPcm,
    // 아니면
    //   LoadCookedStream이다. streamedPayload가 있으면 그 bulk가 패키지 안이어야 한다(sidecar
    //   출력이 없는 쿠킹 — 서버 프로필). reader가 payload bulk를 주지 못하면(sidecar라 메모리에
    //   없음) 그 오류다.
    // - streamed가 있는데 streamedPayload가 비었거나 통째 디코드 대상이면 InvalidArgument다.
    // - 그 밖은 sidecar 경로다: platformData(메타 + 청크 0)로
    //   sidecar 형태 SoundStreamData를 만든다(SoundStreamData::TryCreateStreamed — 청크 0 검증,
    //   Bytes() 불일치 InvalidArgument). 재생은 청크 캐시가 나머지를 읽는다(AU6b2).
    // options(로딩 동작)는 엔진 로드 도우미가 청크 1 후크에 쓴다(3.46.5절). 그 밖 오류는
    // LoadCookedPcm·LoadCookedStream과 같다. 실패하면 앞 원본을 둔다.
    Result<void> LoadCooked(Allocator& allocator, SoundBulkReader reader, void* context,
                            const audio::AudioDecoderTable& decoders,
                            const SoundCookedLoadOptions& options,
                            SharedPtr<SoundStreamedPayload> streamed = {}) noexcept;

    // 키(platformData 내용·채널·rate·frame)가 마지막으로 올린 쿠킹 데이터와 같은지다(엔진 로드
    // 도우미가 sidecar를 열기 전에 본다 — 3.46.3절).
    bool HasCookedData() const noexcept;

    const SharedPtr<const SoundPcm>& Pcm() const noexcept {
        return pcm_;
    }

    // 실시간 디코드 원본이다(LoadCookedStream이 둔다 — 없으면 null).
    const SharedPtr<const SoundStreamData>& Stream() const noexcept {
        return stream_;
    }

    // 재생 PCM을 바꿀 때마다 올라간다(진단 — 스냅숏의 revision).
    uint64 PcmRevision() const noexcept {
        return pcmRevision_;
    }

protected:
    // PCM이 없거나 PCM 차원이 형식 필드와 다르면(재임포트 직후 LoadEditorPcm 전) NotReady다.
    Result<void> FillPlayable(SoundPlayable& playable) const noexcept override;

private:
    // 플랫폼 데이터 전체 byte로 PCM·상주 스트림을 올린다(LoadCookedPcm·LoadCookedStream·패키지 안
    // payload 경로가 같이 쓴다).
    Result<void> LoadPcmBytes(Allocator& allocator, Span<const uint8> bytes,
                              const audio::AudioDecoderTable& decoders) noexcept;
    Result<void> LoadStreamBytes(Allocator& allocator, Span<const uint8> bytes,
                                 const audio::AudioDecoderTable& decoders) noexcept;
    Result<void> AdoptStream(SharedPtr<SoundStreamData> made,
                             const audio::AudioDecoderTable& decoders) noexcept;
    // 플랫폼 데이터 전체 byte를 읽는다. streamedPayload가 있으면(패키지 안 payload) 메타와
    // payload를 joined에 이어 붙이고 그 보기를 준다.
    Result<Span<const uint8>> ReadResidentBytes(SoundBulkReader reader, void* context,
                                                Array<uint8>& joined) const noexcept;

    ROCKS_NATIVE() SharedPtr<const SoundPcm> pcm_;
    ROCKS_NATIVE() SharedPtr<const SoundStreamData> stream_;
    ROCKS_NATIVE() uint64 pcmRevision_ = 0;
    // 마지막으로 올린 PCM의 키다(SetPcm이 지운다). 쿠킹 데이터도 쓰므로 에디터 데이터 없는
    // 빌드에서도 읽힌다.
    ROCKS_NATIVE() SoundPcmKey pcmKey_;
};

} // namespace rocks

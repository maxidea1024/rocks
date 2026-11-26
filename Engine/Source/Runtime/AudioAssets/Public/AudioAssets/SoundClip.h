#pragma once

#include <AudioAssets/SoundClipData.h>
#include <Base/Compiler.h>
#include <AudioAssets/SoundPcm.h>
#include <AudioAssets/SoundSource.h>
#include <Core/Containers/Array.h>

// PCM 사운드 에셋이다(Docs/Design/AudioSystem 3.9·3.36.2절, UE USoundWave 대응). 필드는 셋이다.
// - 임포트 결과(channelCount·sampleRate·frameCount·sourceCodec·파일 loop·cues): 임포터가 매번 전부
//   쓴다.
// - 사용자 설정(looping·loopOverride, SoundSource의 volume·pitch·attenuation): 임포터가 건드리지
//   않는다.
// - 에디터 원본(source — EditorOnly): 디코드한 PCM bulk다. 쿠킹 패키지에 넣지 않는다.
// 재생 PCM(pcm_)은 실행 데이터다. 시험·Sandbox는 SetPcm으로 메모리에서 만들고, 에디터·Tryout은
// LoadEditorPcm으로 원본 bulk에서 만든다(AD25 — 3.36.4절). 쿠킹 플랫폼 데이터는 AU5·AU6이다.
//
// 사용 예(에디터 재생):
//   (void)clip.LoadEditorPcm(allocator, bulkSource);   // 같은 원본이면 다시 올리지 않는다
//   auto sound = clip.MakePlayable(allocator, registry, registry.OwnerAccess());
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다.
namespace rocks {

class PackageBulkSource;

// LoadEditorPcm이 마지막으로 올린 원본의 키다(재사용 판정 — 3.36.4절). 리플렉션 밖 실행 데이터다.
struct SoundEditorPcmKey {
    bool loaded = false;
    SoundClipSource source;
    uint32 channels = 0;
    uint32 sampleRate = 0;
    uint32 frames = 0;
};

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
    // 올린다. 오류: 원본 없음 NotReady, bulk 크기가 형식·채널·frame과 다르거나 비유한 float·형식
    // 필드가 범위 밖(손상) CorruptData, 채널이 2보다 많음 Unsupported(다채널은 AU9), bulk 읽기
    // 오류, 할당 실패. 게임 스레드에서 동기로 읽는다.
    Result<void> LoadEditorPcm(Allocator& allocator, PackageBulkSource& bulk) noexcept;
#endif

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
    // 마지막으로 올린 에디터 원본의 키다(에디터 빌드만 읽는다 — ROCKS_NATIVE는 전처리 조건 안에 둘
    // 수 없어 늘 둔다. SetPcm이 지운다).
    ROCKS_NATIVE() SoundEditorPcmKey editorPcmKey_;
};

} // namespace rocks

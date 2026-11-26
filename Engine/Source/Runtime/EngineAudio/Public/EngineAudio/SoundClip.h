#pragma once

#include <EngineAudio/SoundPcm.h>
#include <EngineAudio/SoundSource.h>

// PCM 사운드 에셋이다(Docs/Design/AudioSystem 3.9·3.35.3절, UE USoundWave 대응). AU3의 PCM은 실행
// 중 메모리 생성뿐이다(SetPcm — 시험·Sandbox). 에디터 원본 bulk·플랫폼 데이터는 AU4·AU5가 더하고
// 그때 PCM은 플랫폼 데이터에서 만든다. 채널 수·rate·frame 수는 PCM에서 읽는다.
//
// 사용 예:
//   auto pcm = SoundPcm::TryAllocate(allocator, 1, frames, 48000);   // 채운 뒤
//   (void)clip.SetPcm(Move(pcm.Value()));
//   clip.looping = true;
//   auto sound = clip.MakePlayable(allocator, registry, registry.OwnerAccess());
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다.
namespace rocks {

ROCKS_CLASS()
class ROCKS_ENGINEAUDIO_API SoundClip final : public SoundSource {
    ROCKS_GENERATED_BODY()

public:
    explicit SoundClip(Allocator& allocator) noexcept : SoundSource(allocator) {
    }

    ROCKS_PROPERTY() bool looping = false;
    // loop 구간(frame)이다. 둘 다 0이면 clip 전체다.
    ROCKS_PROPERTY() uint32 loopBegin = 0;
    ROCKS_PROPERTY() uint32 loopEnd = 0;

    // PCM을 바꾼다. 이미 시작한 재생은 이전 PCM을 끝까지 쓴다(스냅숏). 비유한 표본이 있으면
    // InvalidArgument다. nullptr은 지움이다.
    Result<void> SetPcm(SharedPtr<const SoundPcm> pcm) noexcept;

    const SharedPtr<const SoundPcm>& Pcm() const noexcept {
        return pcm_;
    }

    // PCM을 바꿀 때마다 올라간다(진단 — 스냅숏의 revision).
    uint64 PcmRevision() const noexcept {
        return pcmRevision_;
    }

protected:
    Result<void> FillPlayable(SoundPlayable& playable) const noexcept override;

private:
    ROCKS_NATIVE() SharedPtr<const SoundPcm> pcm_;
    ROCKS_NATIVE() uint64 pcmRevision_ = 0;
};

} // namespace rocks

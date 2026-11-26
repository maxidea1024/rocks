#pragma once

#include <Audio/DistanceAttenuation.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Optional.h>
#include <EngineAudio/SoundPcm.h>
#include <Object/AssetId.h>

// 재생 요청이 넘기는 사운드 스냅숏이다(Docs/Design/AudioSystem 3.35.3절). 재생은 시작 때의
// 스냅숏을 끝까지 쓰므로, 원본 객체가 GC되거나 다시 임포트되어도 이미 시작한 재생에 영향이 없다.
// 게임 스레드에서 만들고, 제어 pipe가 공유 참조로 받는다.
//
// 사용 예:
//   SoundPlayable playable;
//   playable.pcm = pcm;              // SharedPtr<const SoundPcm>
//   playable.looping = true;
//   auto sound = SoundSourceRef::TryCreate(allocator, playable);
//   (void)audio->Play2D(sound.Value());
namespace rocks {

// 감쇠 스냅숏이다(AttenuationSettings 값 — 3.13.1절). 거리는 cm다.
struct SoundAttenuation {
    // 거리 감쇠를 한다(UE bAttenuate).
    bool attenuate = true;
    // 리스너 방향으로 패닝한다(UE bSpatialize). false면 Direct(2D) 출력이다.
    bool spatialize = true;
    audio::DistanceAttenuationSettings distance;
    // 이 반경 안은 감쇠 1이다(UE AttenuationShapeExtents.X — 구 모양).
    float innerRadius = 400.0f;
    // UE NonSpatializedRadiusStart·End다(3.35.7절 식).
    float nonSpatializedRadiusStart = 0.0f;
    float nonSpatializedRadiusEnd = 0.0f;
};

struct SoundPlayable {
    SharedPtr<const SoundPcm> pcm;
    bool looping = false;
    // loop 구간이다(frame). 둘 다 0이면 clip 전체다.
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
    // 사운드 기본 volume([0, MaxVoiceGain])·pitch([MinPitch, MaxPitch])다.
    float volume = 1.0f;
    float pitch = 1.0f;
    // 없으면 비공간화(2D)다.
    Optional<SoundAttenuation> attenuation;
    // 진단용 원본 에셋과 revision이다(메모리 생성이면 빈 값).
    AssetId asset;
    uint64 revision = 0;
};

class ROCKS_ENGINEAUDIO_API SoundSourceRef {
public:
    SoundSourceRef() noexcept = default;

    // 스냅숏을 검사해 공유 참조로 만든다. 제어 pipe가 마지막 참조를 놓을 수 있으므로 allocator는
    // 스레드 안전해야 한다(엔진 기본 할당자·SystemAllocator — SoundPcm과 같은 조건). pcm이 없거나
    // 유한하지 않은 표본이 있거나, loop 구간이 잘못됐거나(끝 ≤ 시작, 끝 > frame 수),
    // volume·pitch·감쇠 값이 범위 밖·비유한이면 InvalidArgument다. 할당 실패는 OutOfMemory다.
    [[nodiscard]] static Result<SoundSourceRef> TryCreate(Allocator& allocator,
                                                          const SoundPlayable& playable) noexcept;

    bool IsValid() const noexcept {
        return static_cast<bool>(playable_);
    }

    // 빈 참조면 nullptr이다.
    const SoundPlayable* Get() const noexcept {
        return playable_.Get();
    }

    // 공유 참조 수다(진단·시험).
    uint64 UseCount() const noexcept;

private:
    explicit SoundSourceRef(SharedPtr<const SoundPlayable> playable) noexcept
        : playable_(Move(playable)) {
    }

    SharedPtr<const SoundPlayable> playable_;
};

// 스냅숏 검사만 한다(TryCreate와 같은 규칙). AttenuationSettings 등이 값 확인에 쓴다.
ROCKS_ENGINEAUDIO_API Result<void> ValidateSoundAttenuation(const SoundAttenuation& value) noexcept;

} // namespace rocks

#pragma once

#include <AssetRuntime/Package.h>
#include <EngineAudio/AttenuationSettings.h>
#include <EngineAudio/SoundSourceRef.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefs.h>

// 재생할 수 있는 사운드 에셋의 기반이다(Docs/Design/AudioSystem 3.9·3.35.3절, UE USoundBase 대응).
// AU3은 volume·pitch·감쇠 참조만 가진다(분류·동시 발음·가상화·버스는 AU7).
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다.
namespace rocks {

class ObjectRegistry;

ROCKS_CLASS(Abstract)
class ROCKS_ENGINEAUDIO_API SoundSource : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit SoundSource(Allocator& allocator) noexcept {
        (void)allocator;
    }

    // [0, MaxVoiceGain]다.
    ROCKS_PROPERTY() float volume = 1.0f;
    // [MinPitch, MaxPitch]다.
    ROCKS_PROPERTY() float pitch = 1.0f;
    // 비었으면 2D다. 같은 에셋 Registry 안의 간선이다(값은 ObjectRegistry::SetReference로 바꾼다).
    ROCKS_PROPERTY() ObjectRef<AttenuationSettings> attenuation;

    // 재생 스냅숏을 만든다(3.35.3절): 파생 클래스의 PCM·loop와 이 클래스의 volume·pitch, 감쇠
    // 참조가 가리키는 값을 복사한다. registry·access는 이 객체와 감쇠 객체를 가진 Registry와 그
    // 소유 문맥이다. 오류: PCM 없음(NotReady), 범위 밖 값·감쇠 객체를 찾지 못함(InvalidArgument),
    // 할당 실패.
    Result<SoundSourceRef> MakePlayable(Allocator& allocator, ObjectRegistry& registry,
                                        const ObjectAccessContext& access) const noexcept;

protected:
    // 파생 클래스가 PCM·loop를 채운다. 없으면 NotReady다.
    virtual Result<void> FillPlayable(SoundPlayable& playable) const noexcept = 0;
};

} // namespace rocks

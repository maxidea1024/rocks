#pragma once

#include <AssetRuntime/Package.h>
#include <AudioAssets/AttenuationSettings.h>
#include <AudioAssets/ConcurrencyGroup.h>
#include <AudioAssets/MixBus.h>
#include <AudioAssets/MixBusSend.h>
#include <AudioAssets/SoundCategory.h>
#include <AudioAssets/SoundSourceRef.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

// 재생할 수 있는 사운드 에셋의 기반이다(Docs/Design/AudioSystem 3.9·3.35.3절, UE USoundBase 대응).
// volume·pitch·감쇠·분류 참조, 재생 관리(우선순위·가상화·동시 발음 — 3.41절), 버스
// 라우팅(3.45.7절)을 가진다.
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다.
namespace rocks {

class ObjectRegistry;

ROCKS_CLASS(Abstract)
class ROCKS_AUDIOASSETS_API SoundSource : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit SoundSource(Allocator& allocator) noexcept
        : concurrency(allocator), busSends(allocator) {
    }

    // [0, MaxVoiceGain]다.
    ROCKS_PROPERTY() float volume = 1.0f;
    // [MinPitch, MaxPitch]다.
    ROCKS_PROPERTY() float pitch = 1.0f;
    // 비었으면 2D다. 같은 에셋 Registry 안의 간선이다(값은 ObjectRegistry::SetReference로 바꾼다).
    ROCKS_PROPERTY() ObjectRef<AttenuationSettings> attenuation;
    // 분류다(UE SoundClassObject — 3.43.1절). 비었으면 엔진 기본 분류나 분류 없음이다.
    ROCKS_PROPERTY() ObjectRef<SoundCategory> category;
    // 켜면 2채널 원본의 볼륨이 ×0.5다(UE bApplyNormalizationToStereoSounds — 3.43.1절).
    ROCKS_PROPERTY() bool normalizeStereo = false;
    // 재생 우선순위다([MinSoundPriority, MaxSoundPriority], UE Priority 기본 1). voice 상한을
    // 넘으면 들리는 크기 × 우선순위가 낮은 재생부터 voice를 잃는다(3.41.1절).
    ROCKS_PROPERTY() float priority = DefaultSoundPriority;
    // 켜면 voice 배정에서 들리는 크기를 곱하지 않고 우선순위만 본다(UE
    // bBypassVolumeScaleForPriority).
    ROCKS_PROPERTY() bool bypassVolumeScaleForPriority = false;
    // loop 재생의 가상화 모드다(UE VirtualizationMode 기본 Restart — 3.41.3절).
    ROCKS_PROPERTY() SoundVirtualizationMode virtualizationMode = SoundVirtualizationMode::Restart;
    // 동시 발음 그룹이다(UE ConcurrencySet — 최대 MaxConcurrencyGroups, 같은 에셋 Registry 안
    // 간선). overrideConcurrency면 이것 대신 concurrencyOverrides 하나를 이 사운드만의 그룹으로
    // 쓴다(UE bOverrideConcurrency·ConcurrencyOverrides). 둘 다 없으면 동시 발음 제한이
    // 없다(3.41.2절).
    ROCKS_PROPERTY() ObjectRefArray<ConcurrencyGroup> concurrency;
    ROCKS_PROPERTY() bool overrideConcurrency = false;
    ROCKS_PROPERTY() SoundConcurrencySettings concurrencyOverrides;
    // 기본 출력 버스다(UE SoundSubmixObject — 3.45.7절). 비었으면 분류의 defaultBus, 엔진
    // BaseDefaultBus, Master 순이다. enableBaseBus를 끄면 기본 버스 send level이 0이다(UE
    // bEnableBaseSubmix).
    ROCKS_PROPERTY() ObjectRef<MixBus> bus;
    ROCKS_PROPERTY() bool enableBaseBus = true;
    // 버스 send다(UE SoundSubmixSends·bEnableSubmixSends — 최대 MaxMixBusSends).
    ROCKS_PROPERTY() StructRefArray<MixBusSend> busSends;
    ROCKS_PROPERTY() bool enableBusSends = true;

    // 재생 스냅숏을 만든다(3.35.3절): 파생 클래스의 PCM·loop와 이 클래스의 volume·pitch, 감쇠
    // 참조·동시 발음 그룹이 가리키는 값을 복사한다. registry·access는 이 객체와 감쇠·그룹 객체를
    // 가진 Registry와 그 소유 문맥이다. 오류: PCM 없음(NotReady), 범위 밖 값·감쇠·그룹 객체를 찾지
    // 못함 (InvalidArgument), 그룹이 MaxConcurrencyGroups보다 많음(CapacityExceeded), 할당 실패.
    Result<SoundSourceRef> MakePlayable(Allocator& allocator, ObjectRegistry& registry,
                                        const ObjectAccessContext& access) const noexcept;

    // 분류 계층이 정하는 clip 로딩 동작이다(3.12절 Inherited — 분류부터 부모 쪽으로 처음 만난
    // Inherited 아닌 값, 없으면 Inherited). SoundCookedLoadOptions::categoryLoading에 넣는다. 오류:
    // 분류 참조를 찾지 못함 InvalidArgument, 깊이 초과(순환 포함) CapacityExceeded.
    Result<SoundLoadingBehavior>
    CategoryLoadingBehavior(ObjectRegistry& registry,
                            const ObjectAccessContext& access) const noexcept;

protected:
    // 파생 클래스가 PCM·loop를 채운다. 없으면 NotReady다.
    virtual Result<void> FillPlayable(SoundPlayable& playable) const noexcept = 0;
};

} // namespace rocks

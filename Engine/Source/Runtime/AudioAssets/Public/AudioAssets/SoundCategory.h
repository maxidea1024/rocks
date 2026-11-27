#pragma once

#include <AssetRuntime/Package.h>
#include <AudioAssets/SoundCategoryChain.h>
#include <AudioAssets/SoundCategoryPassiveMix.h>
#include <AudioAssets/AttenuationTypes.h>
#include <AudioAssets/SoundCategoryTypes.h>
#include <AudioAssets/SoundClipData.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

// 사운드 분류 에셋이다(Docs/Design/AudioSystem 3.43.1절, UE USoundClass 대응). 부모를 가리키는 한
// 방향 계층이고, 계층은 volume·pitch를 곱하고 isUiSound·isMusic을 OR한다(나머지는 재생 분류 값만).
// 재생은 MakeChain이 만든 사슬 스냅숏을 쓴다(에셋을 고치면 새 재생부터 바뀐다).
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다.
//
// 사용 예:
//   auto chain = category.MakeChain(allocator, registry, registry.OwnerAccess());
//   PlayOptions options;
//   options.category = chain.Value();
namespace rocks {

class ObjectRegistry;

ROCKS_CLASS()
class ROCKS_AUDIOASSETS_API SoundCategory final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit SoundCategory(Allocator& allocator) noexcept : passiveMixes(allocator) {
    }

    // 부모 분류다(UE ParentClass). null이면 루트다. 순환은 MakeChain이 거부한다.
    ROCKS_PROPERTY() ObjectRef<SoundCategory> parent;
    // ≥ 0이다.
    ROCKS_PROPERTY() float volume = 1.0f;
    // (0, MaxPitch]다(UE는 범위가 없다 — 최종 pitch는 재생에서 자른다).
    ROCKS_PROPERTY() float pitch = 1.0f;
    // [0, 20000] Hz다.
    ROCKS_PROPERTY() float lowPassFrequency = MaxAttenuationFilterFrequency;
    // 감쇠 거리 배수다(≥ 0 — 계층·믹스와 상관없다, 3.43.6절).
    ROCKS_PROPERTY() float attenuationDistanceScale = 1.0f;
    // voice 배정에서 늘 맨 앞이다(UE bAlwaysPlay — 재생 분류 값만, 3.43.4절).
    ROCKS_PROPERTY() bool alwaysPlay = false;
    // UI 소리다(UE bIsUISound — 계층 OR, 장면 일시정지 면제는 AU7e2).
    ROCKS_PROPERTY() bool isUiSound = false;
    // 음악이다(UE bIsMusic — 계층 OR, 리버브 끔은 AU7h).
    ROCKS_PROPERTY() bool isMusic = false;
    // 곱할 사용자 볼륨 CVar다(Rocks — 3.43.3절).
    ROCKS_PROPERTY() SoundUserVolume userVolume = SoundUserVolume::None;
    // 이 분류의 clip 로딩 동작이다(UE LoadingBehavior — Inherited면 부모 쪽으로 찾는다, 3.12절).
    ROCKS_PROPERTY() SoundLoadingBehavior loadingBehavior = SoundLoadingBehavior::Inherited;
    // passive 믹스다(UE PassiveSoundMixModifiers — 최대 MaxCategoryPassiveMixes).
    ROCKS_PROPERTY() StructRefArray<SoundCategoryPassiveMix> passiveMixes;

    // 이 분류부터 루트까지의 사슬 스냅숏이다(passive 믹스는 MixSnapshot::MakeRef로 평탄화).
    // allocator는 스레드 안전해야 한다(제어 pipe가 마지막 참조를 놓는다). 오류: 깊이·passive 수
    // 초과 CapacityExceeded, 순환·범위 밖 값·참조를 찾지 못함 InvalidArgument, 할당 실패.
    Result<SoundCategoryChainRef> MakeChain(Allocator& allocator, ObjectRegistry& registry,
                                            const ObjectAccessContext& access) const noexcept;
};

} // namespace rocks

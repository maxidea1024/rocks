#pragma once

#include <AudioAssets/AttenuationTypes.h>
#include <AudioAssets/SoundCategoryTypes.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>

// 믹스 스냅숏의 분류 조정 하나다(Docs/Design/AudioSystem 3.43.1·3.43.2절, UE FSoundClassAdjuster —
// SoundMix.h:129–167). 믹스가 활성인 동안 보간 계수 t로 category(applyToChildren이면 그 자손도)의
// volume·pitch에 a·t + 1 − t를 곱하고 LPF를 로그 보간 값과 min한다.
//
// 참조 struct다(ObjectRef — Hard): MixSnapshot::adjusters(StructRefArray)의 원소로만 둔다.
namespace rocks {

class SoundCategory;

ROCKS_STRUCT()
struct MixCategoryAdjuster {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ObjectRef<SoundCategory> category;
    // ≥ 0이다.
    ROCKS_PROPERTY() float volume = 1.0f;
    // [0, MaxMixAdjusterPitch]다.
    ROCKS_PROPERTY() float pitch = 1.0f;
    // [0, 20000] Hz다(보간 때 [20, 20000]으로 자른다).
    ROCKS_PROPERTY() float lowPassFrequency = MaxAttenuationFilterFrequency;
    ROCKS_PROPERTY() bool applyToChildren = false;
};

} // namespace rocks

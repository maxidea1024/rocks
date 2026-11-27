#pragma once

#include <AudioAssets/SoundCategoryTypes.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>

// 분류의 passive 믹스 항목이다(Docs/Design/AudioSystem 3.43.7절, UE FPassiveSoundMixModifier —
// SoundClass.h:167–190). 이 분류(잎)의 재생이 voice 배정 자리 안에 있고 들리는 크기 v가
// [minVolumeThreshold, maxVolumeThreshold](경계 포함)면 mix가 passive로 활성이 된다.
//
// 참조 struct다(ObjectRef — Hard): SoundCategory::passiveMixes(StructRefArray)의 원소로만 둔다.
namespace rocks {

class MixSnapshot;

ROCKS_STRUCT()
struct SoundCategoryPassiveMix {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ObjectRef<MixSnapshot> mix;
    // ≥ 0이고 min ≤ max다.
    ROCKS_PROPERTY() float minVolumeThreshold = 0.0f;
    ROCKS_PROPERTY() float maxVolumeThreshold = DefaultPassiveMaxVolumeThreshold;
};

} // namespace rocks

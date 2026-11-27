#pragma once

#include <AudioAssets/MixSnapshotData.h>
#include <AudioAssets/AttenuationTypes.h>
#include <AudioAssets/SoundCategoryTypes.h>
#include <AudioAssets/SoundClipData.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Result.h>

// 분류 사슬 스냅숏이다(Docs/Design/AudioSystem 3.43.1·3.43.2절). 재생 분류(잎)부터 루트까지 각
// 분류의 키와 속성 사본을 담는다. 재생은 수락 때의 사슬로 계층·믹스를 계산하므로, 에셋을 고치면 새
// 재생부터 바뀐다. 게임 스레드의 SoundCategory::MakeChain이 만들고 제어 쪽이 Registry 없이 읽는다.
namespace rocks {

// 분류 하나의 속성 값이다(UE FSoundClassProperties 중 AU7e1이 쓰는 칸 — 3.43.1절).
struct SoundCategoryProperties {
    float volume = 1.0f;
    float pitch = 1.0f;
    float lowPassFrequency = MaxAttenuationFilterFrequency;
    float attenuationDistanceScale = 1.0f;
    bool alwaysPlay = false;
    bool isUiSound = false;
    bool isMusic = false;
    SoundUserVolume userVolume = SoundUserVolume::None;
    SoundLoadingBehavior loadingBehavior = SoundLoadingBehavior::Inherited;
};

// passive 믹스 하나다(UE FPassiveSoundMixModifier). 재생의 v가 [min, max](경계 포함)면 활성이다.
struct SoundCategoryPassiveMixValue {
    MixSnapshotRef mix;
    float minVolumeThreshold = 0.0f;
    float maxVolumeThreshold = DefaultPassiveMaxVolumeThreshold;
};

struct SoundCategoryLink {
    // 분류 객체의 AudioObjectKey다.
    uint64 key = 0;
    SoundCategoryProperties properties;
    uint32 passiveCount = 0;
    SoundCategoryPassiveMixValue passive[MaxCategoryPassiveMixes];
};

// links[0]이 재생 분류(잎), links[depth − 1]이 루트다.
struct SoundCategoryChain {
    uint32 depth = 0;
    SoundCategoryLink links[MaxSoundCategoryDepth];

    // 키가 사슬 안에 있는지다(applyToChildren 조정의 대상 판정).
    bool Contains(uint64 key) const noexcept {
        for (uint32 index = 0; index < depth; ++index) {
            if (links[index].key == key) {
                return true;
            }
        }

        return false;
    }
};

using SoundCategoryChainRef = SharedPtr<const SoundCategoryChain>;

// 값 검사다: 깊이 0·초과, 키 0·중복(순환), 범위 밖·비유한 값(InvalidArgument — 깊이 초과는
// CapacityExceeded), passive 믹스 null.
ROCKS_AUDIOASSETS_API Result<void>
ValidateSoundCategoryChain(const SoundCategoryChain& chain) noexcept;

// 사슬이 정하는 로딩 동작이다(3.12절 Inherited — 잎부터 처음 만난 Inherited 아닌 값, 없으면
// Inherited).
ROCKS_AUDIOASSETS_API SoundLoadingBehavior
ChainLoadingBehavior(const SoundCategoryChain& chain) noexcept;

} // namespace rocks

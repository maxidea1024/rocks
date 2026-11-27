#pragma once

#include <AudioAssets/AttenuationTypes.h>
#include <AudioAssets/SoundCategoryTypes.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Result.h>

// 믹스 스냅숏의 불변 재생 값이다(Docs/Design/AudioSystem 3.43.1·3.43.7절, UE USoundMix 값). 게임
// 스레드의 MixSnapshot::MakeRef가 만들고 push·base·override 명령과 분류 사슬의 passive 목록이 공유
// 참조로 싣는다. 제어 쪽이 Registry 없이 읽는다.
namespace rocks {

// 분류 하나의 조정이다(UE FSoundClassAdjuster). 키는 분류 객체의 AudioObjectKey다.
struct MixAdjusterValue {
    uint64 categoryKey = 0;
    // ≥ 0이다.
    float volume = 1.0f;
    // [0, MaxMixAdjusterPitch]다.
    float pitch = 1.0f;
    // [0, 20000] Hz다.
    float lowPassFrequency = MaxAttenuationFilterFrequency;
    bool applyToChildren = false;
};

struct MixSnapshotData {
    // 믹스 객체의 AudioObjectKey다(활성 표의 키 — 0이 아니다).
    uint64 key = 0;
    // 초다. duration이 음수면 무한이다.
    float initialDelay = 0.0f;
    float fadeInTime = DefaultMixFadeSeconds;
    float duration = -1.0f;
    float fadeOutTime = DefaultMixFadeSeconds;
    uint32 adjusterCount = 0;
    MixAdjusterValue adjusters[MaxMixAdjusters];
};

using MixSnapshotRef = SharedPtr<const MixSnapshotData>;

// 값 검사다: 키 0, 조정 수 초과(CapacityExceeded), 범위 밖·비유한 값(InvalidArgument).
ROCKS_AUDIOASSETS_API Result<void> ValidateMixSnapshotData(const MixSnapshotData& data) noexcept;

} // namespace rocks

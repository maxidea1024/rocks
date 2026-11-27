#pragma once

#include <AssetRuntime/Package.h>
#include <AudioAssets/SoundConcurrencyEntry.h>

// 동시 발음 그룹 에셋이다(Docs/Design/AudioSystem 3.41.2절, UE USoundConcurrency 대응). 같은 그룹을
// 가리키는 사운드 재생들이 한 그룹(소유자 제한이면 소유자마다 하나)으로 세어진다.
//
// 사용 예:
//   ConcurrencyGroup& footsteps = ...;          // 에셋 Registry의 객체
//   footsteps.settings.maxCount = 4;
//   footsteps.settings.resolutionRule = SoundConcurrencyRule::StopOldest;
//   clip.concurrency에 이 객체를 넣으면 MakePlayable이 스냅숏에 항목을 담는다.
//   코드 재생은 PlayOptions::concurrency에 footsteps.MakeEntry()를 넣는다.
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다.
namespace rocks {

ROCKS_CLASS()
class ROCKS_AUDIOASSETS_API ConcurrencyGroup final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit ConcurrencyGroup(Allocator& allocator) noexcept {
        (void)allocator;
    }

    ROCKS_PROPERTY() SoundConcurrencySettings settings;

    // 재생 스냅숏 항목이다(키는 이 객체의 실행 중 유일 id). 설정이 범위 밖이면 InvalidArgument다.
    Result<SoundConcurrencyEntry> MakeEntry() const noexcept;
};

} // namespace rocks

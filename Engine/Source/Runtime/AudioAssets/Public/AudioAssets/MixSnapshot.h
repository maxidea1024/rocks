#pragma once

#include <AssetRuntime/Package.h>
#include <AudioAssets/MixCategoryAdjuster.h>
#include <AudioAssets/MixSnapshotData.h>
#include <AudioAssets/SoundCategoryTypes.h>
#include <Object/ObjectAccessContext.h>
#include <Object/StructRefArray.h>

// 믹스 스냅숏 에셋이다(Docs/Design/AudioSystem 3.43.1·3.43.7절, UE USoundMix 대응). push·pop·base로
// 걸면 fade 상태 기계를 따라 분류 조정을 보간해 적용한다. 여러 믹스는 곱과 LPF min으로 합친다.
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다.
//
// 사용 예:
//   auto mix = duck.MakeRef(allocator, registry, registry.OwnerAccess());
//   (void)audio->PushMix(mix.Value());
namespace rocks {

class ObjectRegistry;

ROCKS_CLASS()
class ROCKS_AUDIOASSETS_API MixSnapshot final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit MixSnapshot(Allocator& allocator) noexcept : adjusters(allocator) {
    }

    // 분류 조정이다(최대 MaxMixAdjusters).
    ROCKS_PROPERTY() StructRefArray<MixCategoryAdjuster> adjusters;
    // 시작 지연·fade-in·지속·fade-out(초)이다. duration이 음수면 pop할 때까지 무한이다.
    ROCKS_PROPERTY() float initialDelay = 0.0f;
    ROCKS_PROPERTY() float fadeInTime = DefaultMixFadeSeconds;
    ROCKS_PROPERTY() float duration = -1.0f;
    ROCKS_PROPERTY() float fadeOutTime = DefaultMixFadeSeconds;

    // 불변 재생 값이다(키는 이 객체의 AudioObjectKey, 조정 분류는 그 객체의 키). allocator는 스레드
    // 안전해야 한다. 오류: 조정 수 초과 CapacityExceeded, 범위 밖 값·분류 참조 없음
    // InvalidArgument, 할당 실패.
    Result<MixSnapshotRef> MakeRef(Allocator& allocator, ObjectRegistry& registry,
                                   const ObjectAccessContext& access) const noexcept;
};

} // namespace rocks

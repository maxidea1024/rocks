#pragma once

#include <AssetRuntime/Package.h>
#include <Core/Memory/Allocator.h>
#include <InputActions/Haptics/HapticEffect.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 게임패드 진동 효과 에셋이다(UE UForceFeedbackEffect — Docs/Design/InputActions 2.8절). 값 struct
// HapticEffect를 그대로 감싼다. 재생은 효과의 깊은 사본을 쓰므로(6.2절) lease로 읽어 사본을 만든다:
// CopyHapticEffect(InputAssetAccess.h) 또는 HapticPlayer::PlayHapticEffect의 lease 판.
//
// 사용 예(로드한 lease로 재생):
//   (void)controller.Haptics().PlayHapticEffect(assets, hitLease, params);
//
// 스레드: 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_INPUTACTIONS_API HapticEffectAsset final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit HapticEffectAsset(Allocator& allocator) noexcept : effect(allocator) {
    }

    // 실효 효과 id다(2.8절): effect.id가 0이 아니면 그것, 0이면 대표 에셋의 AssetId 값이다.
    // CopyHapticEffect가 사본의 id로 쓴다.
    Guid EffectiveId() const noexcept {
        return effect.id != Guid{} ? effect.id : Id().value;
    }

    ROCKS_PROPERTY() HapticEffect effect;
};

} // namespace rocks::input

#pragma once

#include <Core/Memory/Allocator.h>
#include <CurveAssets/CurveAssetConstants.h>
#include <Curves/TangentCurve.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 진동 효과의 채널 하나다(UE FForceFeedbackChannelDetails — Docs/Design/InputActions 6.1절).
// curve는 재생 시각(초) → 세기(0–1)이고, affects* 모터에 그 세기를 최댓값으로 합친다.
ROCKS_STRUCT()
struct HapticChannel {
    ROCKS_GENERATED_BODY()

    explicit HapticChannel(Allocator& allocator) noexcept : curve(allocator) {
    }

    ROCKS_PROPERTY() bool affectsLeftLarge = true;
    ROCKS_PROPERTY() bool affectsLeftSmall = true;
    ROCKS_PROPERTY() bool affectsRightLarge = true;
    ROCKS_PROPERTY() bool affectsRightSmall = true;
    ROCKS_PROPERTY() TangentCurve curve;
    // 커브 에셋 slot이다(HapticEffectAsset::curveAssets 번호, 없으면 NoCurveAssetSlot — UE
    // FRuntimeFloatCurve::ExternalCurve, 2.9절 IA9). CopyHapticEffect가 에셋 커브를 curve로
    // 복사하고 비운다(재생은 비운 값만 받는다).
    ROCKS_PROPERTY() uint32 curveAsset = NoCurveAssetSlot;
};

} // namespace rocks::input

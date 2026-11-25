#pragma once

#include <Core/Optional.h>
#include <Core/Memory/Allocator.h>
#include <MaterialSchema/MaterialTraitEnums.h>
#include <MaterialSchema/VertexInputUsage.h>
#include <Meta/Annotations.h>
#include <RenderData/MaterialEnums.h>

// 머티리얼 인스턴스의 기본 속성 override다(Docs/Design/MaterialSystem 19.11절, UE
// FMaterialInstanceBasePropertyOverrides —
// RE/Public/Materials/MaterialInstanceBasePropertyOverrides.h:12–141, UMaterialInstance
// BlendableLocationOverride·BlendablePriorityOverride). UE의 bOverride_* bool과 값 짝을 Optional
// 하나로 둔다. 값이 있으면 덮는다. 덮으면 static 변형이 생기고 실행 중 인스턴스는 덮을 수
// 없다(4.2절).
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialTraitOverrides {
    ROCKS_GENERATED_BODY()

    explicit MaterialTraitOverrides(Allocator& allocator) noexcept : usage(allocator) {
    }

    ROCKS_PROPERTY() Optional<float> opacityMaskClipValue;
    ROCKS_PROPERTY() Optional<BlendMode> blendMode;
    // 고전 shading model 값이다. Rocks에서는 SurfaceFromShadingModel의 preset 값
    // override다(10.3절).
    ROCKS_PROPERTY() Optional<ShadingModel> shadingModel;
    ROCKS_PROPERTY() Optional<bool> ditheredLodTransition;
    ROCKS_PROPERTY() Optional<bool> castDynamicShadowAsMasked;
    ROCKS_PROPERTY() Optional<bool> twoSided;
    ROCKS_PROPERTY() Optional<bool> thinSurface;
    ROCKS_PROPERTY() Optional<bool> outputTranslucentVelocity;
    ROCKS_PROPERTY() Optional<bool> hasPixelAnimation;
    // TODO(MPQ-VirtualizedGeometry): 아래 여섯은 가상화 geometry·테셀레이션 pass가 읽는다.
    ROCKS_PROPERTY() Optional<bool> enableTessellation;
    ROCKS_PROPERTY() Optional<float> displacementMagnitude;
    ROCKS_PROPERTY() Optional<float> displacementCenter;
    ROCKS_PROPERTY() Optional<bool> enableDisplacementFade;
    ROCKS_PROPERTY() Optional<float> displacementFadeStartSizePixels;
    ROCKS_PROPERTY() Optional<float> displacementFadeEndSizePixels;
    ROCKS_PROPERTY() Optional<float> maxWorldPositionOffset;
    ROCKS_PROPERTY() Optional<bool> surfaceCacheSharing;
    ROCKS_PROPERTY() Optional<BlendableLocation> blendableLocation;
    ROCKS_PROPERTY() Optional<int32> blendablePriority;
    // 사용 대상을 덮는가와 그 값이다(UE bOverride_UsageFlags·UsageFlags).
    ROCKS_PROPERTY() bool overridesUsage = false;
    ROCKS_PROPERTY() VertexInputUsage usage;
};

} // namespace rocks::material

#pragma once

#include <MaterialSchema/MaterialAssetReference.h>
#include <Meta/Annotations.h>

// Color Ramp 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionColorRamp — RE/Public/Materials/MaterialExpressionColorRamp.h). 이름·기본값은
// UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ColorRampProperties {
    ROCKS_GENERATED_BODY()

    // 기대 클래스는 ColorCurveAsset(UE CurveLinearColor)이다(19.4.3절 — 번역은 원본 공급
    // FindColorCurve로 찾는다, 6.13절 MS2f4).
    ROCKS_PROPERTY() MaterialAssetReference colorCurve;
};

} // namespace rocks::material

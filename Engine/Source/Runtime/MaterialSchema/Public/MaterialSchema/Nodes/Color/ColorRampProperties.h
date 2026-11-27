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

    // TODO(MPQ-CurveAsset): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 CurveLinearColor다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference colorCurve;
};

} // namespace rocks::material

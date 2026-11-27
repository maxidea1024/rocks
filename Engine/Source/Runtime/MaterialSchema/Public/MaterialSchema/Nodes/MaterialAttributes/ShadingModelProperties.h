#pragma once

#include <MaterialSchema/MaterialTraitEnums.h>
#include <Meta/Annotations.h>

// Shading Model 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionShadingModel — RE/Public/Materials/MaterialExpressionShadingModel.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ShadingModelProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ShadingModelPreset shadingModel = ShadingModelPreset::DefaultLit;
};

} // namespace rocks::material

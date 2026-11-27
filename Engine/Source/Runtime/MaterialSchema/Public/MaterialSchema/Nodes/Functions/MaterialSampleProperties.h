#pragma once

#include <MaterialSchema/MaterialAssetReference.h>
#include <Meta/Annotations.h>

// Material Sample 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionMaterialSample — RE/Internal/Materials/MaterialExpressionMaterialSample.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialSampleProperties {
    ROCKS_GENERATED_BODY()

    // 기대 클래스는 MaterialBase다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference materialReference;
};

} // namespace rocks::material

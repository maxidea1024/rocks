#pragma once

#include <Meta/Annotations.h>

// Volumetric Advanced Output 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionVolumetricAdvancedMaterialOutput —
// RE/Public/Materials/MaterialExpressionVolumetricAdvancedMaterialOutput.h). 이름·기본값은 UE와
// 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct VolumetricAdvancedOutputProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool perSamplePhaseEvaluation = false;
    ROCKS_PROPERTY() uint32 multiScatteringApproximationOctaveCount = 0;
    ROCKS_PROPERTY() bool groundContribution = false;
    ROCKS_PROPERTY() bool grayScaleMaterial = false;
    ROCKS_PROPERTY() bool rayMarchVolumeShadow = true;
    ROCKS_PROPERTY() bool clampMultiScatteringContribution = true;
};

} // namespace rocks::material

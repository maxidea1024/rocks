#pragma once

#include <Meta/Annotations.h>

// Sampler Budget Switch 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionRequiredSamplersSwitch —
// RE/Public/Materials/MaterialExpressionRequiredSamplersSwitch.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SamplerBudgetSwitchProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 requiredSamplers = 16;
};

} // namespace rocks::material

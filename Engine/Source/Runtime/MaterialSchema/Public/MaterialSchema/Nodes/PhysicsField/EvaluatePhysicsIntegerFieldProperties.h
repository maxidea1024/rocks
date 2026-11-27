#pragma once

#include <MaterialSchema/Nodes/PhysicsField/PhysicsFieldNodeEnums.h>
#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Evaluate Physics Integer Field 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionEvalPhysicsIntegerField —
// RE/Public/Materials/MaterialExpressionSamplePhysicsField.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct EvaluatePhysicsIntegerFieldProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() PositionOrigin worldPositionOriginType = PositionOrigin::Absolute;
    ROCKS_PROPERTY() PhysicsFieldIntegerType fieldTarget = PhysicsFieldIntegerType::DynamicState;
};

} // namespace rocks::material

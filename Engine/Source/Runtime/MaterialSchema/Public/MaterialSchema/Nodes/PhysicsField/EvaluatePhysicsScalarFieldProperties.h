#pragma once

#include <MaterialSchema/Nodes/PhysicsField/PhysicsFieldNodeEnums.h>
#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Evaluate Physics Scalar Field 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionEvalPhysicsScalarField —
// RE/Public/Materials/MaterialExpressionSamplePhysicsField.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct EvaluatePhysicsScalarFieldProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() PositionOrigin worldPositionOriginType = PositionOrigin::Absolute;
    ROCKS_PROPERTY() PhysicsFieldScalarType fieldTarget = PhysicsFieldScalarType::DynamicConstraint;
};

} // namespace rocks::material

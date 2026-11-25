#pragma once

#include <MaterialSchema/Nodes/PhysicsField/PhysicsFieldNodeEnums.h>
#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// SamplePhysicsScalarField 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionSamplePhysicsScalarField —
// RE/Public/Materials/MaterialExpressionSamplePhysicsField.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SamplePhysicsScalarFieldProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() PositionOrigin worldPositionOriginType = PositionOrigin::Absolute;
    ROCKS_PROPERTY() PhysicsFieldScalarType fieldTarget = PhysicsFieldScalarType::DynamicConstraint;
};

} // namespace rocks::material

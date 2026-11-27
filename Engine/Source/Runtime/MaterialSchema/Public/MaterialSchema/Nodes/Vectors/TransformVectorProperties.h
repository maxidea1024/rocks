#pragma once

#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Transform Vector 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionTransform — RE/Public/Materials/MaterialExpressionTransform.h). 이름·기본값은
// UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct TransformVectorProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() VectorTransformSource transformSourceType = VectorTransformSource::Tangent;
    ROCKS_PROPERTY() VectorTransformDestination transformType = VectorTransformDestination::World;
};

} // namespace rocks::material

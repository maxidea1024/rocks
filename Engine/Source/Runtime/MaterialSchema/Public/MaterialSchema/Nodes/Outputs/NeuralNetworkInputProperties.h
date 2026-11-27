#pragma once

#include <MaterialSchema/Nodes/Outputs/OutputsNodeEnums.h>
#include <Meta/Annotations.h>

// Neural Input (Texture) 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionNeuralNetworkInput —
// RE/Public/Materials/MaterialExpressionNeuralPostProcessNode.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct NeuralNetworkInputProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() NeuralIndexType neuralIndexType = NeuralIndexType::TextureIndex;
};

} // namespace rocks::material

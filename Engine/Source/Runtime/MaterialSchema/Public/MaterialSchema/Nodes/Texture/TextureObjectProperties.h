#pragma once

#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>
#include <Meta/Annotations.h>

// Texture Object 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionTextureObject — RE/Public/Materials/MaterialExpressionTextureObject.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct TextureObjectProperties {
    ROCKS_GENERATED_BODY()

    // 기대 클래스는 Texture다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference texture;
    ROCKS_PROPERTY() SamplerType samplerType = SamplerType::Color;
    ROCKS_PROPERTY() bool isDefaultMeshpaintTexture = false;
};

} // namespace rocks::material

#pragma once

#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>
#include <Meta/Annotations.h>

// Texture Object From Collection 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionTextureObjectFromCollection —
// RE/Public/Materials/MaterialExpressionTextureObjectFromCollection.h). 이름·기본값은 UE와 같다(b
// 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct TextureObjectFromCollectionProperties {
    ROCKS_GENERATED_BODY()

    // TODO(MPQ-TextureCollection): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 TextureCollection다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference textureCollectionObject;
    ROCKS_PROPERTY()
    TextureCollectionMemberType textureType = TextureCollectionMemberType::Texture2D;
};

} // namespace rocks::material

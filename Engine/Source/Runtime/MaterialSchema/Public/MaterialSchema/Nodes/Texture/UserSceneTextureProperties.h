#pragma once

#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// User Scene Texture 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionUserSceneTexture — RE/Public/Materials/MaterialExpressionUserSceneTexture.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct UserSceneTextureProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name userSceneTexture;
    ROCKS_PROPERTY() bool filtered = false;
    ROCKS_PROPERTY() bool clamped {};
};

} // namespace rocks::material

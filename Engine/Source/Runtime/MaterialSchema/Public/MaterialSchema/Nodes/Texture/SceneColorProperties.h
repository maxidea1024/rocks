#pragma once

#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>
#include <Meta/Annotations.h>

// Scene Color 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionSceneColor — RE/Public/Materials/MaterialExpressionSceneColor.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SceneColorProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() SceneAttributeInputMode inputMode = SceneAttributeInputMode::Coordinates;
};

} // namespace rocks::material

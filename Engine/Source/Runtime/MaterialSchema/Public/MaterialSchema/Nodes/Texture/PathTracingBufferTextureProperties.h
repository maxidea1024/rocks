#pragma once

#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>
#include <Meta/Annotations.h>

// Path Tracing Buffer Texture 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionPathTracingBufferTexture —
// RE/Public/Materials/MaterialExpressionPathTracingBufferTexture.h). 이름·기본값은 UE와 같다(b
// 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct PathTracingBufferTextureProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY()
    PathTracingBufferTextureId pathTracingBufferTextureId = PathTracingBufferTextureId::Radiance;
};

} // namespace rocks::material

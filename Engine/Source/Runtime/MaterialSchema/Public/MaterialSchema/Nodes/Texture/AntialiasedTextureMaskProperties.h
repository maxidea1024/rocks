#pragma once

#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/Parameters/ParameterChannelNames.h>
#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>
#include <Meta/Annotations.h>

// AAMasked Param2D 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionAntialiasedTextureMask —
// RE/Public/Materials/MaterialExpressionAntialiasedTextureMask.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct AntialiasedTextureMaskProperties {
    ROCKS_GENERATED_BODY()

    explicit AntialiasedTextureMaskProperties(Allocator& allocator) noexcept
        : channelNames(allocator) {
    }

    // 기대 클래스는 Texture다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference texture;
    ROCKS_PROPERTY() SamplerType samplerType = SamplerType::Color;
    ROCKS_PROPERTY() bool isDefaultMeshpaintTexture = false;
    ROCKS_PROPERTY() TextureMipValueMode mipValueMode = TextureMipValueMode::None;
    ROCKS_PROPERTY() SamplerSourceMode samplerSource = SamplerSourceMode::FromTextureAsset;
    ROCKS_PROPERTY() TextureGatherMode gatherMode = TextureGatherMode::None;
    ROCKS_PROPERTY() bool automaticViewMipBias = true;
    ROCKS_PROPERTY() ParameterChannelNames channelNames;
    ROCKS_PROPERTY() uint8 constCoordinate = 0;
    ROCKS_PROPERTY() int32 constMipValue = -1;
    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
    ROCKS_PROPERTY() float threshold = 0.5f;
    ROCKS_PROPERTY() TextureColorChannel channel = TextureColorChannel::Alpha;
};

} // namespace rocks::material

#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 머티리얼 텍스처 sampling scale이다(Docs/Design/MaterialSystem 12.5절, TextureStreaming T4, UE
// FMaterialTextureInfo). 번역이 텍스처 slot마다 UV 채널과 scale을 계산해 넣는다.
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialTextureSamplingScale {
    ROCKS_GENERATED_BODY()

    // 번역 결과의 텍스처 slot 번호(MaterialTexture<k>)다.
    ROCKS_PROPERTY() uint32 textureSlot = 0;
    ROCKS_PROPERTY() uint32 uvChannel = 0;
    ROCKS_PROPERTY() float samplingScale = 1.0f;
};

} // namespace rocks::material
